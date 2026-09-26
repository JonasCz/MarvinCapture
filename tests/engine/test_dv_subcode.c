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

/* Plain C test executable: no framework, prints failures, returns the
 * failure count (0 = success) so ctest sees a nonzero exit on failure. */

#include "dv_subcode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static int bcd_byte(int tens, int units)
{
    return ((tens & 0xF) << 4) | (units & 0xF);
}

static void write_pack(uint8_t *frame, unsigned seq_count, unsigned seq,
                        unsigned block, unsigned slot, uint8_t pack_id,
                        const uint8_t data[4])
{
    (void)seq_count;
    size_t off = seq * DV_SEQ_SIZE + dv_subcode_pack_offset(block, slot);
    frame[off] = pack_id;
    memcpy(frame + off + 1, data, 4);
}

static void set_block_section(uint8_t *frame, unsigned seq, unsigned block, int section)
{
    size_t off = seq * DV_SEQ_SIZE + (size_t)block * DV_BLOCK_SIZE;
    frame[off + 0] = (uint8_t)(section << 5);
    frame[off + 1] = 0xFF;
    frame[off + 2] = 0xFF;
}

/* Lays out a synthetic frame: block 0 = header, 1-2 = subcode, 3-5 = VAUX,
 * then an audio block before every 15 video blocks (6, 22, ...). Every pack slot starts
 * invalid (PC0 = 0xFF); callers fill in the packs they want present. */
static void build_frame_skeleton(uint8_t *frame, unsigned seq_count, int is_pal)
{
    memset(frame, 0xFF, (size_t)seq_count * DV_SEQ_SIZE);
    for (unsigned s = 0; s < seq_count; s++) {
        set_block_section(frame, s, 0, 0);
        size_t hdr_off = s * DV_SEQ_SIZE + 3;
        frame[hdr_off] = is_pal ? 0x80 : 0x00;

        for (unsigned b = 1; b <= 2; b++)
            set_block_section(frame, s, b, 1);
        for (unsigned b = 3; b <= 5; b++)
            set_block_section(frame, s, b, 2);
        for (unsigned b = 6; b < DV_SEQ_BLOCKS; b++)
            set_block_section(frame, s, b, ((b - 6) % 16 == 0) ? 3 : 4);
    }
}

/* Writes the timecode pack into both subcode blocks of every sequence
 * (SSYB 3 and 9 in the real spec, slot 3 of each of our two blocks here). */
static void fill_timecode(uint8_t *frame, unsigned seq_count, int h, int m, int s, int f, int drop)
{
    uint8_t d[4];
    d[0] = (uint8_t)((drop ? 0x40 : 0) | bcd_byte(f / 10, f % 10));
    d[1] = (uint8_t)bcd_byte(s / 10, s % 10);
    d[2] = (uint8_t)bcd_byte(m / 10, m % 10);
    d[3] = (uint8_t)bcd_byte(h / 10, h % 10);
    for (unsigned sq = 0; sq < seq_count; sq++) {
        write_pack(frame, seq_count, sq, 1, 3, DV_PACK_TIMECODE, d);
        write_pack(frame, seq_count, sq, 2, 3, DV_PACK_TIMECODE, d);
    }
}

static void fill_rec_date(uint8_t *frame, unsigned seq_count, int day, int month, int year)
{
    uint8_t d[4];
    int y2 = year % 100;
    d[0] = 0xFF; /* time zone */
    d[1] = (uint8_t)bcd_byte(day / 10, day % 10);
    d[2] = (uint8_t)bcd_byte(month / 10, month % 10);
    d[3] = (uint8_t)bcd_byte(y2 / 10, y2 % 10);
    for (unsigned sq = 0; sq < seq_count; sq++)
        for (unsigned b = 3; b <= 5; b++)
            write_pack(frame, seq_count, sq, b, 0, DV_PACK_VAUX_REC_DATE, d);
}

static void fill_rec_time(uint8_t *frame, unsigned seq_count, int h, int m, int s)
{
    uint8_t d[4];
    d[0] = 0xFF; /* frames: unset on most cameras */
    d[1] = (uint8_t)bcd_byte(s / 10, s % 10);
    d[2] = (uint8_t)bcd_byte(m / 10, m % 10);
    d[3] = (uint8_t)bcd_byte(h / 10, h % 10);
    for (unsigned sq = 0; sq < seq_count; sq++)
        for (unsigned b = 3; b <= 5; b++)
            write_pack(frame, seq_count, sq, b, 1, DV_PACK_VAUX_REC_TIME, d);
}

static void fill_aspect(uint8_t *frame, unsigned seq_count, int is_16_9)
{
    uint8_t d[4] = { 0xFF, (uint8_t)(0xF8 | (is_16_9 ? 0x02 : 0x00)), 0xFF, 0xFF };
    for (unsigned sq = 0; sq < seq_count; sq++)
        for (unsigned b = 3; b <= 5; b++)
            write_pack(frame, seq_count, sq, b, 2, DV_PACK_VAUX_SOURCE_CTRL, d);
}

static void fill_audio(uint8_t *frame, unsigned seq_count, int sample_rate, int channels)
{
    int freq_code = sample_rate == 48000 ? 0 : sample_rate == 44100 ? 1 : sample_rate == 32000 ? 2 : 7;
    uint8_t d[4] = { 0xD0, 0x30, (uint8_t)(channels == 4 ? 0x02 : 0x00),
                     (uint8_t)(0xC0 | (freq_code << 3)) };
    /* One pack per audio block; 0x50 sits in audio blocks 0 and 3 of the
     * nine (FFmpeg: 80*6 + 80*16*{0,3}). */
    for (unsigned sq = 0; sq < seq_count; sq++)
        for (unsigned k = 0; k <= 3; k += 3)
            write_pack(frame, seq_count, sq, 6 + 16 * k, 0, DV_PACK_AAUX_SOURCE, d);
}

static void fill_rec_start(uint8_t *frame, unsigned seq_count, int flag)
{
    /* REC ST is PC2 bit 7, active-low. */
    uint8_t d[4] = { 0xFF, (uint8_t)(flag ? 0x7F : 0xFF), 0xFF, 0xFF };
    for (unsigned sq = 0; sq < seq_count; sq++)
        for (unsigned k = 1; k <= 4; k += 3) /* audio blocks 1 and 4 */
            write_pack(frame, seq_count, sq, 6 + 16 * k, 0, DV_PACK_AAUX_SOURCE_CTRL, d);
}

static void test_clean_ntsc_frame(void)
{
    unsigned seq_count = DV_SEQ_COUNT_NTSC;
    uint8_t *frame = calloc(seq_count, DV_SEQ_SIZE);
    build_frame_skeleton(frame, seq_count, 0);
    fill_timecode(frame, seq_count, 1, 2, 3, 4, 1);
    fill_rec_date(frame, seq_count, 15, 8, 2003);
    fill_rec_time(frame, seq_count, 9, 30, 0);
    fill_aspect(frame, seq_count, 1);
    fill_audio(frame, seq_count, 48000, 2);
    fill_rec_start(frame, seq_count, 0);

    dv_frame_info_t info;
    int rc = dv_subcode_parse_frame(frame, (size_t)seq_count * DV_SEQ_SIZE, &info);
    CHECK(rc == 0, "parse should succeed");

    CHECK(info.timecode.valid, "timecode valid");
    CHECK(info.timecode.hour == 1 && info.timecode.minute == 2 &&
          info.timecode.second == 3 && info.timecode.frame == 4, "timecode fields");
    CHECK(info.timecode.drop_frame == 1, "drop frame flag");

    CHECK(info.rec_date.valid, "rec date valid");
    CHECK(info.rec_date.day == 15 && info.rec_date.month == 8 && info.rec_date.year == 2003,
          "rec date fields");

    CHECK(info.rec_time.valid, "rec time valid");
    CHECK(info.rec_time.hour == 9 && info.rec_time.minute == 30 && info.rec_time.second == 0,
          "rec time fields");

    CHECK(info.aspect.valid && info.aspect.is_16_9, "aspect 16:9");

    CHECK(info.audio.valid, "audio valid");
    CHECK(info.audio.sample_rate == 48000 && info.audio.channels == 2, "audio fields");

    CHECK(info.rec_start_valid && info.rec_start == 0, "rec start clear");

    CHECK(info.dsf_valid && info.is_pal == 0, "DSF says NTSC");
    CHECK(info.sequence_count == (int)DV_SEQ_COUNT_NTSC, "sequence count");

    free(frame);
}

static void test_pal_frame_century_heuristic(void)
{
    unsigned seq_count = DV_SEQ_COUNT_PAL;
    uint8_t *frame = calloc(seq_count, DV_SEQ_SIZE);
    build_frame_skeleton(frame, seq_count, 1);
    fill_rec_date(frame, seq_count, 1, 1, 1998); /* -> 98, heuristic must give 1998 */

    dv_frame_info_t info;
    int rc = dv_subcode_parse_frame(frame, (size_t)seq_count * DV_SEQ_SIZE, &info);
    CHECK(rc == 0, "parse should succeed");
    CHECK(info.dsf_valid && info.is_pal == 1, "DSF says PAL");
    CHECK(info.rec_date.valid && info.rec_date.year == 1998, "century heuristic (pre-2000)");

    /* Now a low two-digit year should land in the 2000s. */
    fill_rec_date(frame, seq_count, 1, 1, 2021);
    rc = dv_subcode_parse_frame(frame, (size_t)seq_count * DV_SEQ_SIZE, &info);
    CHECK(rc == 0, "parse should succeed");
    CHECK(info.rec_date.valid && info.rec_date.year == 2021, "century heuristic (post-2000)");

    free(frame);
}

static void test_missing_pack_is_invalid(void)
{
    unsigned seq_count = DV_SEQ_COUNT_NTSC;
    uint8_t *frame = calloc(seq_count, DV_SEQ_SIZE);
    build_frame_skeleton(frame, seq_count, 0);
    /* No packs filled in at all: everything should come back invalid, not
     * garbage, and the call must still succeed. */
    dv_frame_info_t info;
    int rc = dv_subcode_parse_frame(frame, (size_t)seq_count * DV_SEQ_SIZE, &info);
    CHECK(rc == 0, "parse should succeed even with nothing present");
    CHECK(!info.timecode.valid, "timecode absent -> invalid");
    CHECK(!info.rec_date.valid, "rec date absent -> invalid");
    CHECK(!info.rec_time.valid, "rec time absent -> invalid");
    CHECK(!info.aspect.valid, "aspect absent -> invalid");
    CHECK(!info.audio.valid, "audio absent -> invalid");
    CHECK(!info.rec_start_valid, "rec start absent -> invalid");
    free(frame);
}

static void test_corrupted_copy_outvoted(void)
{
    unsigned seq_count = DV_SEQ_COUNT_NTSC;
    uint8_t *frame = calloc(seq_count, DV_SEQ_SIZE);
    build_frame_skeleton(frame, seq_count, 0);
    fill_timecode(frame, seq_count, 10, 20, 30, 5, 0);

    /* Corrupt exactly one of the 2*10 = 20 timecode copies with garbage
     * data (PC0 stays 0x13, so it still looks like a timecode pack, just a
     * wrong one). Majority voting must still recover the true value: 19
     * copies agree, 1 doesn't. */
    uint8_t garbage[4] = { 0x7F, 0x7F, 0x7F, 0x7F };
    write_pack(frame, seq_count, 4, 2, 3, DV_PACK_TIMECODE, garbage);

    dv_frame_info_t info;
    int rc = dv_subcode_parse_frame(frame, (size_t)seq_count * DV_SEQ_SIZE, &info);
    CHECK(rc == 0, "parse should succeed");
    CHECK(info.timecode.valid, "timecode still valid despite one bad copy");
    CHECK(info.timecode.hour == 10 && info.timecode.minute == 20 &&
          info.timecode.second == 30 && info.timecode.frame == 5,
          "majority vote recovers the true timecode");

    free(frame);
}

static void test_all_copies_invalid_marker(void)
{
    /* Every subcode slot holding the timecode pack is explicitly marked
     * 0xFF (no data): parser must not invent a value. */
    unsigned seq_count = DV_SEQ_COUNT_NTSC;
    uint8_t *frame = calloc(seq_count, DV_SEQ_SIZE);
    build_frame_skeleton(frame, seq_count, 0);
    /* build_frame_skeleton already leaves every slot at 0xFF; nothing more
     * to do. */
    dv_frame_info_t info;
    int rc = dv_subcode_parse_frame(frame, (size_t)seq_count * DV_SEQ_SIZE, &info);
    CHECK(rc == 0, "parse should succeed");
    CHECK(!info.timecode.valid, "all-0xFF copies -> invalid, not garbage");
    free(frame);
}

static void test_bad_length_rejected(void)
{
    uint8_t small[DV_SEQ_SIZE * 3]; /* neither 10 nor 12 sequences */
    memset(small, 0xFF, sizeof(small));
    dv_frame_info_t info;
    int rc = dv_subcode_parse_frame(small, sizeof(small), &info);
    CHECK(rc != 0, "wrong sequence count must be rejected");

    rc = dv_subcode_parse_frame(small, DV_SEQ_SIZE * DV_SEQ_COUNT_NTSC + 1, &info);
    CHECK(rc != 0, "non-multiple-of-sequence-size length must be rejected");
}

int main(void)
{
    test_clean_ntsc_frame();
    test_pal_frame_century_heuristic();
    test_missing_pack_is_invalid();
    test_corrupted_copy_outvoted();
    test_all_copies_invalid_marker();
    test_bad_length_rejected();

    if (g_failures == 0) {
        printf("test_dv_subcode: all tests passed\n");
        return 0;
    }
    printf("test_dv_subcode: %d failure(s)\n", g_failures);
    return g_failures;
}
