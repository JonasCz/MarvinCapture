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

#include "dv_error.h"
#include "dv_subcode.h"
#include <string.h>

/* Section and DBN a block at index b (0..149) of a sequence must carry. */
static void expected_id(unsigned b, unsigned *sct, unsigned *dbn)
{
    if (b == 0) { *sct = 0; *dbn = 0; }
    else if (b < 3) { *sct = 1; *dbn = b - 1; }
    else if (b < 6) { *sct = 2; *dbn = b - 3; }
    else {
        unsigned r = b - 6, g = r / 16, k = r % 16;
        if (k == 0) { *sct = 3; *dbn = g; }
        else { *sct = 4; *dbn = g * 15 + (k - 1); }
    }
}

/* IEC 61834 / SMPTE 314M audio error code: 0x8000 per 16-bit sample, 0x800 per 12-bit sample.
 * 12-bit samples are packed two per 3 bytes (MSBs of both, then the two low nibbles), so an
 * error-coded pair reads 80 80 00. A block counts only when every sample carries the code:
 * a block of one constant non-zero value (DC offset, a clipped passage) is real audio. */
static int audio_block_is_error(const uint8_t *blk, int bits)
{
    const uint8_t *d = blk + 8;
    int e16 = bits != 12, e12 = bits != 16;
    for (int i = 0; i < 72 && (e16 || e12); i++) {
        if (e16 && d[i] != ((i & 1) ? 0x00 : 0x80)) e16 = 0;
        if (e12 && d[i] != ((i % 3 == 2) ? 0x00 : 0x80)) e12 = 0;
    }
    return e16 || e12;
}

int dv_error_analyze(const uint8_t *frame, size_t len, dv_frame_errors_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!frame || len % DV_SEQ_SIZE != 0)
        return -1;
    unsigned nseq = (unsigned)(len / DV_SEQ_SIZE);
    if (nseq != DV_SEQ_COUNT_NTSC && nseq != DV_SEQ_COUNT_PAL)
        return -1;

    /* the quantisation from the AAUX source pack; without one either error code counts */
    int abits = 0;
    dv_frame_info_t info;
    if (dv_subcode_parse_frame(frame, len, &info) == 0 && info.audio.valid)
        abits = info.audio.bits;

    unsigned sta14 = 0, zero_first = 0, nonzero_first = 0;
    for (unsigned si = 0; si < nseq; si++) {
        for (unsigned b = 0; b < DV_SEQ_BLOCKS; b++) {
            const uint8_t *blk = frame + (size_t)si * DV_SEQ_SIZE + (size_t)b * DV_BLOCK_SIZE;
            unsigned esct, edbn;
            expected_id(b, &esct, &edbn);
            out->blocks++;
            if ((unsigned)(blk[0] >> 5) != esct || (unsigned)(blk[1] >> 4) != si ||
                (blk[1] & 7) != 7 || blk[2] != edbn) {
                out->missing_blocks++;
                continue;
            }
            if (esct == 4) {
                out->video_blocks++;
                unsigned sta = blk[3] >> 4;
                if (sta == 7 || sta == 15) out->video_err++;
                else if (sta) out->video_concealed++;
                if (sta == 14) sta14++;
            } else if (esct == 3) {
                out->audio_blocks++;
                if (audio_block_is_error(blk, abits)) out->audio_err++;
                if (si < nseq / 2) {
                    int zero = 1;
                    for (int i = 8; i < 80 && zero; i++)
                        if (blk[i]) zero = 0;
                    if (zero) zero_first++;
                    else nonzero_first++;
                }
            }
        }
    }
    if (zero_first && nonzero_first)
        out->audio_mute = zero_first;
    if (out->video_blocks && sta14 * 100 >= out->video_blocks * 99u)
        out->reencoded = 1;
    out->frame_dropped = out->missing_blocks * 10u >= out->blocks * 9u;
    out->frame_error = out->missing_blocks || out->video_err || out->video_concealed ||
                       out->audio_err || out->audio_mute || out->reencoded;
    return 0;
}

/* A 72-byte audio payload that is one byte value repeated (silence, the error code's bytes):
 * such payloads repeat legitimately. */
static int payload_constant(const uint8_t *d)
{
    for (int i = 1; i < 72; i++)
        if (d[i] != d[0]) return 0;
    return 1;
}

int dv_error_block_map(const uint8_t *frame, size_t len, const uint8_t *prev, uint8_t *map)
{
    if (!frame || !map || len % DV_SEQ_SIZE != 0)
        return -1;
    unsigned nseq = (unsigned)(len / DV_SEQ_SIZE);
    if (nseq != DV_SEQ_COUNT_NTSC && nseq != DV_SEQ_COUNT_PAL)
        return -1;
    dv_frame_errors_t fe;
    dv_error_analyze(frame, len, &fe);
    int abits = 0;
    dv_frame_info_t info;
    if (dv_subcode_parse_frame(frame, len, &info) == 0 && info.audio.valid)
        abits = info.audio.bits;

    unsigned nblk = nseq * DV_SEQ_BLOCKS;
    int worst = DV_BLOCK_GOOD;
    for (unsigned i = 0; i < nblk; i++) {
        unsigned si = i / DV_SEQ_BLOCKS, b = i % DV_SEQ_BLOCKS;
        const uint8_t *blk = frame + (size_t)i * DV_BLOCK_SIZE;
        unsigned esct, edbn;
        expected_id(b, &esct, &edbn);
        uint8_t g = DV_BLOCK_GOOD;
        if ((unsigned)(blk[0] >> 5) != esct || (unsigned)(blk[1] >> 4) != si ||
            (blk[1] & 7) != 7 || blk[2] != edbn) {
            g = DV_BLOCK_ERROR;
        } else if (esct == 4) {
            unsigned sta = blk[3] >> 4;
            if (sta == 7 || sta == 15) {
                g = DV_BLOCK_ERROR;
            } else if (sta == 14 && fe.reencoded) {
                /* re-encoding camera: only a payload stuck from the previous frame is concealed */
                const uint8_t *pb = prev ? prev + (size_t)i * DV_BLOCK_SIZE : NULL;
                if (pb && memcmp(blk + 4, pb + 4, DV_BLOCK_SIZE - 4) == 0)
                    g = DV_BLOCK_CONCEALED;
            } else if (sta) {
                g = DV_BLOCK_CONCEALED;
            }
        } else if (esct == 3) {
            const uint8_t *d = blk + 8;
            if (audio_block_is_error(blk, abits)) {
                g = DV_BLOCK_ERROR;
            } else if (fe.audio_mute && si < nseq / 2) {
                int zero = 1;
                for (int k = 0; k < 72 && zero; k++)
                    if (d[k]) zero = 0;
                if (zero) g = DV_BLOCK_CONCEALED;
            }
            if (g == DV_BLOCK_GOOD && !payload_constant(d)) {
                /* copied audio: the same payload elsewhere in this frame, or here last frame */
                const uint8_t *pb = prev ? prev + (size_t)i * DV_BLOCK_SIZE : NULL;
                if (pb && memcmp(d, pb + 8, 72) == 0)
                    g = DV_BLOCK_SUSPECT;
                for (unsigned j = 6; j < nblk && g == DV_BLOCK_GOOD; j++) {
                    if (j == i || (j % DV_SEQ_BLOCKS) < 6 || ((j % DV_SEQ_BLOCKS) - 6) % 16 != 0)
                        continue;
                    if (memcmp(d, frame + (size_t)j * DV_BLOCK_SIZE + 8, 72) == 0)
                        g = DV_BLOCK_SUSPECT;
                }
            }
        }
        map[i] = g;
        if (g > worst) worst = g;
    }
    return worst;
}
