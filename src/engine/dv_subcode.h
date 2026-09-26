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

/*
 * Parses one complete, already-reassembled DV frame (as dv_reassembler.c
 * produces: DIF sequences of 150 * 80 bytes, 10 sequences for NTSC / 12 for
 * PAL) into timecode, recording date/time, aspect and audio-format metadata.
 *
 * Hardware-free: this module only reads bytes the caller hands it. It is
 * unit-tested with synthetic frames (see tests/engine/test_dv_subcode.c),
 * not real captures. The block layout below follows the well documented
 * parts of IEC 61834 / SMPTE 306M (also used by libdv and ffmpeg's
 * libavcodec/dvdata.c, libavformat/dv.c):
 *
 *   - A DIF block's first byte's top 3 bits give its section: 0 = header,
 *     1 = subcode, 2 = VAUX, 3 = audio, 4..7 = video.
 *   - A sequence (150 blocks) holds 1 header, 2 subcode, 3 VAUX, 9 audio and
 *     135 video blocks, in that section order but this parser does not rely
 *     on fixed positions: it classifies each block by its ID byte, so it
 *     tolerates any ordering a real capture (or a future camera model)
 *     turns out to use.
 *   - Every "pack" (subcode SSYB, VAUX or AAUX) is 5 bytes: a header byte
 *     (PC0, the pack ID, e.g. 0x13/0x62/0x63/0x61/0x50/0x51) followed by 4
 *     data bytes. PC0 == 0xFF means "no data in this slot".
 *   - Packs of the same kind are repeated several times per sequence and
 *     again across every sequence in the frame (redundancy against bit
 *     errors). This parser gathers every copy found and returns the value
 *     with the most matching copies (first-seen wins on a tie), ignoring
 *     0xFF slots.
 *
 * Exact intra-block pack *positions* (subcode SSYB numbering 0..11, which
 * VAUX pack occupies which of the 15 slots, etc.) are this project's own
 * layout for the synthetic test frames; they are not claimed to be
 * bit-exact with any particular camera's bitstream. The values used to
 * *interpret* a pack's 4 data bytes (timecode BCD fields, VAUX 0x62/0x63
 * date/time BCD fields, the 16:9 aspect codes 0x02/0x07) are the commonly
 * documented IEC 61834 / SMPTE 314M encodings and match ffmpeg's
 * libavformat/dv.c (dv_extract_timecode) and libavcodec/dvdata.c aspect
 * handling.
 *
 * REC ST ("recording start point"): bit 7 of the AAUX Source Control
 * pack's (PC0 = 0x51) PC2 byte (data[1]), active-low -- 0 marks the first
 * frame of a recording. This is libdv's dv_aaux_asc_pc2_t layout and what
 * dvgrab's Frame::IsNewRecording() tests. Not every camera sets it, so the
 * scene detector treats it as one signal among several. Still to be checked
 * against a real tape with a known recording break on the rig.
 */

#ifndef DV_SUBCODE_H
#define DV_SUBCODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DV_BLOCK_SIZE 80
#define DV_SEQ_BLOCKS 150
#define DV_SEQ_SIZE (DV_SEQ_BLOCKS * DV_BLOCK_SIZE) /* 12000 */
#define DV_SEQ_COUNT_NTSC 10
#define DV_SEQ_COUNT_PAL 12

#define DV_PACK_SIZE 5

/* Packs per block for each section, this project's own synthetic layout
 * (see file header comment). Real captures are scanned the same way; if a
 * real block carries fewer/more slots the scan simply finds fewer copies,
 * it never misreads adjacent bytes as a different pack because every slot
 * is checked against its expected PC0 first. */
#define DV_SUBCODE_PACKS_PER_BLOCK 6
#define DV_VAUX_PACKS_PER_BLOCK 15
#define DV_AAUX_PACKS_PER_BLOCK 3

#define DV_PACK_TIMECODE 0x13
#define DV_PACK_VAUX_REC_DATE 0x62
#define DV_PACK_VAUX_REC_TIME 0x63
#define DV_PACK_VAUX_SOURCE_CTRL 0x61
#define DV_PACK_AAUX_SOURCE 0x50
#define DV_PACK_AAUX_SOURCE_CTRL 0x51
#define DV_PACK_NONE 0xFF

typedef struct {
    int hour, minute, second, frame;
    int drop_frame;   /* NTSC drop-frame flag; meaningless (and not set) on PAL */
    int valid;
} dv_timecode_t;

typedef struct {
    int day, month, year; /* year is 4-digit, century from the BCD-decade heuristic below */
    int valid;
} dv_rec_date_t;

typedef struct {
    int hour, minute, second, frame;
    int valid;
} dv_rec_time_t;

typedef struct {
    int is_16_9;      /* aspect code 0x02 or 0x07 => 16:9, else 4:3 */
    int valid;
} dv_aspect_t;

typedef struct {
    int sample_rate;  /* 48000, 44100 or 32000; 0 if unknown */
    int channels;     /* 2, or 4 (two stereo pairs, the 32 kHz 12-bit mode) */
    int bits;         /* 16, or 12 (nonlinear) */
    int valid;
} dv_audio_info_t;

typedef struct {
    dv_timecode_t timecode;       /* from SSYB pack 0x13 */
    dv_rec_date_t rec_date;       /* from VAUX pack 0x62 */
    dv_rec_time_t rec_time;       /* from VAUX pack 0x63 */
    dv_aspect_t aspect;           /* from VAUX pack 0x61 */
    dv_audio_info_t audio;        /* from AAUX pack 0x50 */
    int rec_start;                /* best-effort, see file header comment */
    int rec_start_valid;
    int is_pal;                   /* DSF bit from the header block */
    int dsf_valid;
    int sequence_count;           /* sequences actually present in the buffer fed in */
} dv_frame_info_t;

/* frame_len must be an exact multiple of DV_SEQ_SIZE, either
 * DV_SEQ_COUNT_NTSC or DV_SEQ_COUNT_PAL sequences (any other count is
 * rejected: a real reassembled frame is always one of the two). Returns 0
 * on success, -1 on a malformed buffer (wrong length). On success every
 * field of *out is populated, with .valid/.rec_start_valid/.dsf_valid
 * false where no usable copy of that pack was found anywhere in the
 * frame. */
int dv_subcode_parse_frame(const uint8_t *frame, size_t frame_len, dv_frame_info_t *out);

/* Byte offset of a DIF sequence's Nth pack slot for a given block index
 * within the sequence and a given block-local slot index, i.e.
 * following the SMPTE 314M layout described in dv_subcode.c. Exposed so
 * tests (and anything building synthetic frames) can place packs without
 * duplicating the layout. block_index_in_seq is 0..149. */
size_t dv_subcode_pack_offset(unsigned block_index_in_seq, unsigned slot);

#ifdef __cplusplus
}
#endif

#endif /* DV_SUBCODE_H */
