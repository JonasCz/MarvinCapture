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

int dv_error_analyze(const uint8_t *frame, size_t len, dv_frame_errors_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!frame || len % DV_SEQ_SIZE != 0)
        return -1;
    unsigned nseq = (unsigned)(len / DV_SEQ_SIZE);
    if (nseq != DV_SEQ_COUNT_NTSC && nseq != DV_SEQ_COUNT_PAL)
        return -1;

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
                unsigned w0 = ((unsigned)blk[8] << 8) | blk[9];
                int uniform = 1;
                for (int i = 1; i < 36 && uniform; i++)
                    if ((((unsigned)blk[8 + 2 * i] << 8) | blk[9 + 2 * i]) != w0)
                        uniform = 0;
                if (uniform && w0 != 0) out->audio_err++;
                if (si < nseq / 2) {
                    if (uniform && w0 == 0) zero_first++;
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
