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

#include "pin_vidfmt.h"
#include <stdio.h>
#include <string.h>

int pin_vidfmt_rate_from_code(int code, int *num, int *den)
{
    static const int tab[9][2] = {
        {0, 0}, {24000, 1001}, {24, 1}, {25, 1}, {30000, 1001},
        {30, 1}, {50, 1}, {60000, 1001}, {60, 1}
    };
    if (code < 1 || code > 8) { *num = *den = 0; return 0; }
    *num = tab[code][0];
    *den = tab[code][1];
    return 1;
}

int pin_vidfmt_hdv_interlaced(int height)
{
    return height != 720;
}

void pin_vidfmt_label(int height, int fps_num, int fps_den, int interlaced,
                      char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = 0;
    if (height <= 0 || fps_num <= 0 || fps_den <= 0) return;
    if (interlaced && height == 576 && fps_num == 25 * fps_den) {
        snprintf(out, cap, "PAL");
        return;
    }
    if (interlaced && height == 480 && fps_num * 1001 == 30000 * fps_den) {
        snprintf(out, cap, "NTSC");
        return;
    }
    char r[16];
    if (fps_num % fps_den == 0) {
        snprintf(r, sizeof(r), "%d", fps_num / fps_den);
    } else {
        snprintf(r, sizeof(r), "%.3f", (double)fps_num / fps_den);
        size_t n = strlen(r);
        while (n > 0 && r[n - 1] == '0') r[--n] = 0;
        if (n > 0 && r[n - 1] == '.') r[n - 1] = 0;
    }
    snprintf(out, cap, "%d%c%s", height, interlaced ? 'i' : 'p', r);
}
