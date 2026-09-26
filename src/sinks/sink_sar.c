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

#include "sinks_internal.h"

/*
 * Sample (pixel) aspect ratio for a 720-pixel-wide SD frame (DV and analog
 * capture at the SAA7113's standard digitising rate both use 720 luma
 * samples/line). These four fractions are FFmpeg's own DV profile constants
 * (libavformat/dvprofile.c ff_dv_frame_profile*, sar field): they encode
 * ITU-R BT.601's convention that a 4:3 picture sampled at 13.5 MHz over a
 * 720-sample active line is not quite square-pixel, and a 16:9 recording on
 * the same raster stretches the same samples further.
 *
 *   PAL  (720x576) 4:3  -> 16:15   16:9  -> 64:45
 *   NTSC (720x480) 4:3  ->  8:9    16:9  -> 32:27
 *
 * For any other capture width (a future analog geometry) this falls back to
 * dar/width:height reduced by gcd, which is the general definition SAR is
 * derived from (SAR = DAR * height / width) and matches the 720-wide cases
 * above exactly when plugged in with 4:3/16:9 and 576/480 lines.
 */

static int gcd_i(int a, int b)
{
    while (b) {
        int t = a % b;
        a = b;
        b = t;
    }
    return a < 0 ? -a : a;
}

void pin_sink_sar_for(pin_kind_t kind, int is_pal, pin_aspect_t aspect, int width,
                      int *sar_num, int *sar_den)
{
    int dar_num = (aspect == PIN_ASPECT_16_9) ? 16 : 4;
    int dar_den = (aspect == PIN_ASPECT_16_9) ? 9 : 3;

    if (kind != PIN_KIND_HDV && width == 720) {
        if (is_pal) {
            *sar_num = (aspect == PIN_ASPECT_16_9) ? 64 : 16;
            *sar_den = (aspect == PIN_ASPECT_16_9) ? 45 : 15;
        } else {
            *sar_num = (aspect == PIN_ASPECT_16_9) ? 32 : 8;
            *sar_den = (aspect == PIN_ASPECT_16_9) ? 27 : 9;
        }
        return;
    }

    if (kind == PIN_KIND_HDV) {
        /* 1440x1080 HDV: the well-known 4:3 SAR that makes 1440 samples
         * display as 1920 (i.e. 1440 * 4/3), same idea for 16:9 anamorphic
         * full-raster HDV -- but our capture is always the half-horizontal
         * 1440-wide variant, so this is effectively constant in practice. */
        int height = 1080;
        *sar_num = dar_num * height;
        *sar_den = dar_den * width;
        int g = gcd_i(*sar_num, *sar_den);
        if (g) {
            *sar_num /= g;
            *sar_den /= g;
        }
        return;
    }

    int height = is_pal ? 576 : 480;
    *sar_num = dar_num * height;
    *sar_den = dar_den * width;
    int g = gcd_i(*sar_num, *sar_den);
    if (g) {
        *sar_num /= g;
        *sar_den /= g;
    }
}
