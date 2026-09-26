/*
 * Pinnacle Studio 500-USB open driver
 * Copyright (C) 2026 Jonas Cz.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "dv_subcode.h"
#include <string.h>

/* Section type from a DIF block's first ID byte, per this file's header
 * comment. */
static int dv_block_section(uint8_t id0)
{
    return (id0 >> 5) & 0x7;
}

/* Where the packs sit inside a block of each section (SMPTE 314M; the same
 * offsets as FFmpeg's dv_extract_pack):
 *   subcode: 3-byte ID, then 6 SSYBs of 8 bytes = 2-byte SSYB ID, one
 *            reserved 0xFF byte, a 5-byte pack -> pack i at 6 + 8*i
 *   VAUX:    3-byte ID, then 15 packs back to back -> pack i at 3 + 5*i
 *   audio:   3-byte ID, then one AAUX pack at 3, then the samples
 * Returns the number of pack slots and sets *first / *pitch. */
static unsigned section_slots(int section, size_t *first, size_t *pitch)
{
    switch (section) {
    case 1: *first = 6; *pitch = 8; return DV_SUBCODE_PACKS_PER_BLOCK;
    case 2: *first = 3; *pitch = 5; return DV_VAUX_PACKS_PER_BLOCK;
    case 3: *first = 3; *pitch = 0; return DV_AAUX_PACKS_PER_BLOCK;
    default: *first = 0; *pitch = 0; return 0;
    }
}

/* Section a block index within a sequence has in a standard 25 Mbit/s
 * sequence: 0 header, 1-2 subcode, 3-5 VAUX, then an audio block before
 * every 15 video blocks (6, 22, 38, ...). Only used to place packs. */
static int layout_section(unsigned block)
{
    if (block == 0)
        return 0;
    if (block <= 2)
        return 1;
    if (block <= 5)
        return 2;
    return ((block - 6) % 16 == 0) ? 3 : 4;
}

size_t dv_subcode_pack_offset(unsigned block_index_in_seq, unsigned slot)
{
    size_t first, pitch;
    section_slots(layout_section(block_index_in_seq), &first, &pitch);
    return (size_t)block_index_in_seq * DV_BLOCK_SIZE + first + (size_t)slot * pitch;
}

/* Up to this many redundant copies of one pack are collected per frame
 * (12 sequences * 3 VAUX blocks * up to a few matching slots is well under
 * this in practice; it's a generous ceiling, not a hard spec limit). */
#define MAX_PACK_COPIES 64

typedef struct {
    uint8_t data[4];
    unsigned count;
} pack_vote_slot_t;

/* Scans every block of the given section in every sequence for a pack
 * whose PC0 == pack_id, collects each distinct 4-byte payload with its
 * occurrence count, and returns the most frequent one (first-seen wins
 * ties). Returns 1 if at least one valid copy was found, 0 otherwise. */
static int collect_and_vote(const uint8_t *frame, unsigned seq_count, int section,
                             unsigned packs_per_block, uint8_t pack_id, uint8_t out_data[4])
{
    pack_vote_slot_t votes[MAX_PACK_COPIES];
    unsigned num_votes = 0;

    for (unsigned s = 0; s < seq_count; s++) {
        const uint8_t *seq = frame + (size_t)s * DV_SEQ_SIZE;
        for (unsigned b = 0; b < DV_SEQ_BLOCKS; b++) {
            const uint8_t *block = seq + (size_t)b * DV_BLOCK_SIZE;
            if (dv_block_section(block[0]) != section)
                continue;
            size_t first, pitch;
            unsigned slots = section_slots(section, &first, &pitch);
            if (slots > packs_per_block)
                slots = packs_per_block;
            for (unsigned slot = 0; slot < slots; slot++) {
                size_t off = first + (size_t)slot * pitch;
                if (off + DV_PACK_SIZE > DV_BLOCK_SIZE)
                    break;
                const uint8_t *pack = block + off;
                if (pack[0] != pack_id)
                    continue; /* wrong or absent (0xFF) pack in this slot */

                unsigned v;
                for (v = 0; v < num_votes; v++) {
                    if (memcmp(votes[v].data, pack + 1, 4) == 0) {
                        votes[v].count++;
                        break;
                    }
                }
                if (v == num_votes && num_votes < MAX_PACK_COPIES) {
                    memcpy(votes[num_votes].data, pack + 1, 4);
                    votes[num_votes].count = 1;
                    num_votes++;
                }
            }
        }
    }

    if (num_votes == 0)
        return 0;

    unsigned best = 0;
    for (unsigned v = 1; v < num_votes; v++) {
        if (votes[v].count > votes[best].count)
            best = v;
    }
    memcpy(out_data, votes[best].data, 4);
    return 1;
}

/* Two-digit BCD with a masked tens field. Returns -1 when either digit is
 * not a decimal digit: an unset pack is all 0xFF, and tape dropouts produce
 * random bytes, neither of which may decode to a plausible-looking value. */
static int bcd2(uint8_t b, int tens_mask, int tens_shift)
{
    int units = b & 0x0F;
    int tens = (b >> tens_shift) & tens_mask;
    if (units > 9 || tens > 9)
        return -1;
    return tens * 10 + units;
}

static int in_range(int v, int lo, int hi)
{
    return v >= lo && v <= hi;
}

static void parse_timecode(const uint8_t d[4], dv_timecode_t *tc)
{
    /* PC1: CF (bit 7), DF (bit 6), frame tens (5-4), units. */
    tc->frame = bcd2(d[0], 0x3, 4);
    tc->drop_frame = (d[0] >> 6) & 1;
    tc->second = bcd2(d[1], 0x7, 4);
    tc->minute = bcd2(d[2], 0x7, 4);
    tc->hour = bcd2(d[3], 0x3, 4);
    tc->valid = in_range(tc->frame, 0, 29) && in_range(tc->second, 0, 59) &&
                in_range(tc->minute, 0, 59) && in_range(tc->hour, 0, 23);
}

static void parse_rec_date(const uint8_t d[4], dv_rec_date_t *date)
{
    /* PC1 is the time zone; PC2 day, PC3 month, PC4 two-digit year. */
    date->day = bcd2(d[1], 0x3, 4);
    date->month = bcd2(d[2], 0x1, 4);
    int year2 = bcd2(d[3], 0xF, 4);
    /* Century heuristic (DV only stores a 2-digit year): DV recording
     * started in the mid-1990s, so a low two-digit year is read as 20xx
     * and a high one as 19xx. ffmpeg's libavformat/dv.c uses the same kind
     * of cutoff for this reason. */
    date->year = year2 < 0 ? -1 : (year2 < 75) ? (2000 + year2) : (1900 + year2);
    date->valid = in_range(date->day, 1, 31) && in_range(date->month, 1, 12) && year2 >= 0;
}

static void parse_rec_time(const uint8_t d[4], dv_rec_time_t *t)
{
    /* PC1 frames (usually unset: reads as 0), PC2 s, PC3 min, PC4 h. */
    int f = bcd2(d[0], 0x3, 4);
    t->frame = f < 0 ? 0 : f;
    t->second = bcd2(d[1], 0x7, 4);
    t->minute = bcd2(d[2], 0x7, 4);
    t->hour = bcd2(d[3], 0x3, 4);
    t->valid = in_range(t->second, 0, 59) && in_range(t->minute, 0, 59) &&
               in_range(t->hour, 0, 23);
}

static void parse_aspect(const uint8_t d[4], dv_aspect_t *a)
{
    /* PC2 bits 2-0: DISP (FFmpeg: vsc_pack[2] & 7, vsc_pack[0] being the ID). */
    int code = d[1] & 0x07;
    a->is_16_9 = (code == 0x02 || code == 0x07);
    a->valid = 1;
}

static void parse_audio(const uint8_t d[4], dv_audio_info_t *a)
{
    /* PC1 bits 5-0: smpls; PC3 bits 4-0: STYPE (0 = one stereo pair; 2 =
     * two pairs, the 4-channel 32 kHz mode); PC4 bits 5-3: SMP, bits 2-0:
     * QU (0 = 16-bit, 1 = 12-bit nonlinear). The fields FFmpeg's
     * dv_extract_audio_info()/dv_extract_audio() use (d[0]=PC1, d[2]=PC3,
     * d[3]=PC4 here, since d[] is pack+1..+4). */
    static const int freq_table[8] = { 48000, 44100, 32000, 0, 0, 0, 0, 0 };
    int freq_code = (d[3] >> 3) & 0x7;
    int stype = d[2] & 0x1F;
    int quant = d[3] & 0x7;
    a->sample_rate = freq_table[freq_code];
    a->stype = stype;
    a->smpls = d[0] & 0x3F;
    /* Real-world quirk FFmpeg's dv_extract_audio_info() also matches: some
     * cameras signal the 4-channel 32 kHz 12-bit mode with STYPE=0 (nominal
     * 2ch) rather than STYPE=2, distinguishable only because that
     * combination (12-bit quantisation at 32 kHz) is otherwise meaningless
     * for a 2ch stream. */
    a->channels = (stype == 2 || (stype == 0 && quant == 1 && freq_code == 2)) ? 4 : 2;
    a->bits = (quant == 1) ? 12 : 16;
    a->valid = (a->sample_rate != 0);
}

static void parse_rec_start(const uint8_t d[4], int *rec_start, int *valid)
{
    /* PC2 bit 7 is REC ST and active-low: 0 marks the first frame of a
     * recording (libdv dv_aaux_asc_pc2_t, dvgrab Frame::IsNewRecording). */
    *rec_start = (d[1] & 0x80) ? 0 : 1;
    *valid = 1;
}

int dv_subcode_parse_frame(const uint8_t *frame, size_t frame_len, dv_frame_info_t *out)
{
    if (!frame || !out || frame_len == 0 || frame_len % DV_SEQ_SIZE != 0)
        return -1;

    unsigned seq_count = (unsigned)(frame_len / DV_SEQ_SIZE);
    if (seq_count != DV_SEQ_COUNT_NTSC && seq_count != DV_SEQ_COUNT_PAL)
        return -1;

    memset(out, 0, sizeof(*out));
    out->sequence_count = (int)seq_count;

    uint8_t d[4];

    if (collect_and_vote(frame, seq_count, 1, DV_SUBCODE_PACKS_PER_BLOCK, DV_PACK_TIMECODE, d))
        parse_timecode(d, &out->timecode);

    if (collect_and_vote(frame, seq_count, 2, DV_VAUX_PACKS_PER_BLOCK, DV_PACK_VAUX_REC_DATE, d))
        parse_rec_date(d, &out->rec_date);

    if (collect_and_vote(frame, seq_count, 2, DV_VAUX_PACKS_PER_BLOCK, DV_PACK_VAUX_REC_TIME, d))
        parse_rec_time(d, &out->rec_time);

    if (collect_and_vote(frame, seq_count, 2, DV_VAUX_PACKS_PER_BLOCK, DV_PACK_VAUX_SOURCE_CTRL, d))
        parse_aspect(d, &out->aspect);

    if (collect_and_vote(frame, seq_count, 3, DV_AAUX_PACKS_PER_BLOCK, DV_PACK_AAUX_SOURCE, d))
        parse_audio(d, &out->audio);

    if (collect_and_vote(frame, seq_count, 3, DV_AAUX_PACKS_PER_BLOCK, DV_PACK_AAUX_SOURCE_CTRL, d))
        parse_rec_start(d, &out->rec_start, &out->rec_start_valid);

    /* DSF (525/60 vs 625/50): header block, data byte right after the
     * 3-byte ID, bit 7. The header block is always block 0 of a sequence
     * in this project's layout, but we scan for section 0 rather than
     * assume that, for the same robustness reason as the pack scan above. */
    for (unsigned s = 0; s < seq_count && !out->dsf_valid; s++) {
        const uint8_t *seq = frame + (size_t)s * DV_SEQ_SIZE;
        for (unsigned b = 0; b < DV_SEQ_BLOCKS; b++) {
            const uint8_t *block = seq + (size_t)b * DV_BLOCK_SIZE;
            if (dv_block_section(block[0]) != 0)
                continue;
            out->is_pal = (block[3] & 0x80) ? 1 : 0;
            out->dsf_valid = 1;
            break;
        }
    }

    return 0;
}
