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
 * pin_yuv_to_rgb_matrix() and pin_fit_rect(): the preview shaders of every
 * GUI apply the matrix to raw 0..1 samples exactly as pin_api.h documents,
 * so pin down the known points -- limited-range black, white and neutral
 * grey, and 100% red -- for both matrices.
 */

#include "pin_api.h"

#include <math.h>
#include <stdio.h>

static int g_failures;

static void apply(const float m[12], int y, int cb, int cr, double rgb[3])
{
    double in[4] = { y / 255.0, cb / 255.0, cr / 255.0, 1.0 };
    for (int r = 0; r < 3; r++) {
        rgb[r] = 0;
        for (int c = 0; c < 4; c++)
            rgb[r] += m[r * 4 + c] * in[c];
    }
}

static void expect(const char *what, pin_matrix_t mx, int y, int cb, int cr,
                   double r, double g, double b)
{
    float m[12];
    double rgb[3];
    pin_yuv_to_rgb_matrix(mx, 0, m);
    apply(m, y, cb, cr, rgb);
    if (fabs(rgb[0] - r) > 0.01 || fabs(rgb[1] - g) > 0.01 || fabs(rgb[2] - b) > 0.01) {
        printf("FAIL %s: got %.3f %.3f %.3f, want %.3f %.3f %.3f\n", what,
               rgb[0], rgb[1], rgb[2], r, g, b);
        g_failures++;
    }
}

int main(void)
{
    for (int mx = 0; mx < 2; mx++) {
        expect("black", (pin_matrix_t)mx, 16, 128, 128, 0, 0, 0);
        expect("white", (pin_matrix_t)mx, 235, 128, 128, 1, 1, 1);
        expect("grey", (pin_matrix_t)mx, 126, 128, 128, 110.0 / 219, 110.0 / 219, 110.0 / 219);
    }
    /* BT.601 100% red: Y 81, Cb 90, Cr 240 */
    expect("601 red", PIN_MATRIX_BT601, 81, 90, 240, 1.0, 0.0, 0.0);
    /* BT.709 100% red: Y 63, Cb 102, Cr 240 */
    expect("709 red", PIN_MATRIX_BT709, 63, 102, 240, 1.0, 0.0, 0.0);

    int x, y, w, h;
    pin_fit_rect(16, 9, 800, 800, &x, &y, &w, &h);
    if (x != 0 || y != 175 || w != 800 || h != 450) {
        printf("FAIL fit 16:9 in 800x800: %d %d %d %d\n", x, y, w, h);
        g_failures++;
    }
    pin_fit_rect(4, 3, 1600, 900, &x, &y, &w, &h);
    if (x != 200 || y != 0 || w != 1200 || h != 900) {
        printf("FAIL fit 4:3 in 1600x900: %d %d %d %d\n", x, y, w, h);
        g_failures++;
    }

    if (!g_failures)
        printf("colour matrix and fit rect OK\n");
    return g_failures ? 1 : 0;
}
