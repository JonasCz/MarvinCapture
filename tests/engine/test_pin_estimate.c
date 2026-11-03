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

#include "pin_estimate.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int g_failures = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static int near(double a, double b, double tol) { return fabs(a - b) <= tol; }

int main(void)
{
    int src;
    CHECK(near(pin_est_default_rate(PIN_EST_DV, 0, 0, 0, 0, &src) * 3600, 13e9, 1) &&
              src == PIN_EST_SRC_DEFAULT, "DV 13 GB/h");
    CHECK(near(pin_est_default_rate(PIN_EST_HDV, 0, 0, 0, 0, NULL) * 3600, 13e9, 1), "HDV 13 GB/h");
    CHECK(near(pin_est_default_rate(PIN_EST_ANALOG_FFV1, 0, 0, 0, 0, &src) * 3600, 30e9, 1) &&
              src == PIN_EST_SRC_DEFAULT, "FFV1 default 30 GB/h");
    CHECK(near(pin_est_default_rate(PIN_EST_ANALOG_FFV1, 0, 0, 0, 20e9, &src) * 3600, 20e9, 1) &&
              src == PIN_EST_SRC_LEARNED, "FFV1 learned");
    /* AVI: PAL 720x576x2x25 + 192000 */
    CHECK(near(pin_est_default_rate(PIN_EST_ANALOG_AVI, 720, 576, 0, 0, NULL), 20736000 + 192000, 1), "AVI PAL");
    CHECK(near(pin_est_default_rate(PIN_EST_ANALOG_AVI, 720, 480, 1, 0, NULL),
               720.0 * 480 * 2 * 30000.0 / 1001 + 192000, 1), "AVI NTSC");
    CHECK(near(pin_est_default_rate(PIN_EST_ANALOG_AVI, 0, 0, 1, 0, NULL),
               720.0 * 480 * 2 * 30000.0 / 1001 + 192000, 1), "AVI unknown geometry uses the standard");
    CHECK(near(pin_est_rate(PIN_EST_DV, 0, 0, 0, 0, 5e6, &src), 5e6, 0) && src == PIN_EST_SRC_MEASURED,
          "measured wins");
    CHECK(near(pin_est_seconds_left(36000000, 3600000), 10, 1e-9), "seconds left");
    CHECK(pin_est_seconds_left(100, 0) == 0, "unknown rate");
    /* remux: (free - 2 * written) / (3 * rate) */
    CHECK(near(pin_est_seconds_left_remux(1000, 100, 10), 800.0 / 30.0, 1e-9), "remux seconds left");
    CHECK(pin_est_seconds_left_remux(100, 100, 10) == 0, "remux: no room left");
    CHECK(near(pin_est_seconds_left_remux(900, 0, 10), 30, 1e-9), "remux before a capture");

    /* persistence */
    const char *path = "test_pin_estimate.ini";
    remove(path);
    CHECK(pin_est_load_ffv1(path) == 0, "nothing stored");
    CHECK(pin_est_store_ffv1(path, 22.5e9) == 0, "store");
    CHECK(near(pin_est_load_ffv1(path), 22.5e9, 1e3), "load back");
    CHECK(pin_est_store_ffv1(path, 5.0) == -1 && pin_est_store_ffv1(path, 1e12) == -1, "implausible rejected");
    CHECK(near(pin_est_load_ffv1(path), 22.5e9, 1e3), "kept after rejection");
    remove(path);

    /* rate window: 1 MB/s for 15 minutes, then 3 MB/s for 5 minutes: the last
     * 10 minutes average (5 min @1 + 5 min @3) / 10 min = 2 MB/s */
    pin_rate_window_t w;
    pin_rate_reset(&w);
    CHECK(pin_rate_bytes_per_s(&w, 1) < 0, "empty window");
    uint64_t total = 0;
    for (int t = 0; t <= 1200; t++) {
        total += t < 900 ? 1000000u : 3000000u;
        pin_rate_add(&w, t, total);
        if (t == 3) CHECK(pin_rate_bytes_per_s(&w, 10) < 0, "span too short");
        if (t == 60) CHECK(near(pin_rate_bytes_per_s(&w, 10), 1e6, 2e5), "1 MB/s early");
    }
    double r = pin_rate_bytes_per_s(&w, 10);
    CHECK(near(r, 2e6, 6e4), "10-minute average");

    /* H.264 / HEVC: the quality -> bit rate mapping (pin_estimate.h) */
    double pal = 720.0 * 576 * 25;
    CHECK(near(pin_est_video_bitrate(0, 720, 576, 25, 0, 23), 0.30 * pal, 1), "H.264 at 23: 0.30 bit/pixel");
    CHECK(near(pin_est_video_bitrate(0, 720, 576, 25, 0, 17), 0.60 * pal, 1), "6 steps down double it");
    CHECK(near(pin_est_video_bitrate(1, 720, 576, 25, 0, 23), 0.30 * 0.65 * pal, 1), "HEVC 0.65 of it");
    CHECK(near(pin_est_video_bitrate(0, 720, 576, 50, 1, 23), 0.30 * pal * 2 * 0.75, 1),
          "double rate: 1.5 times, not 2");
    double q18 = pin_est_video_bitrate(0, 720, 576, 25, 0, 18);
    CHECK(q18 > 5.4e6 && q18 < 5.6e6, "default 18: about 5.5 Mbit/s");
    CHECK(pin_est_video_bitrate(0, 720, 576, 25, 0, 0) == PIN_EST_VIDEO_MAX_BPS, "clamped at the top");
    CHECK(pin_est_video_bitrate(0, 720, 576, 25, 0, 51) == PIN_EST_VIDEO_MIN_BPS, "clamped at the bottom");
    CHECK(pin_est_video_bitrate(0, 720, 576, 25, 0, -5) == pin_est_video_bitrate(0, 720, 576, 25, 0, 0) &&
          pin_est_video_bitrate(0, 720, 576, 25, 0, 99) == pin_est_video_bitrate(0, 720, 576, 25, 0, 51),
          "quality clamped to 0..51");
    double fr = pin_est_hwenc_rate(0, 0, 0, 0, 0, 18);
    CHECK(near(fr, (q18 + PIN_EST_FLAC_BPS) / 8 * 1.01, 1), "file rate: video + FLAC + 1 %");
    CHECK(pin_est_hwenc_rate(0, 0, 0, 0, 1, 18) > fr * 1.3, "deinterlaced is bigger");
    CHECK(pin_est_hwenc_rate(1, 0, 0, 0, 0, 18) < fr, "HEVC is smaller");
    CHECK(near(pin_est_hwenc_rate(0, 0, 0, 1, 0, 18), pin_est_hwenc_rate(0, 720, 480, 1, 0, 18), 1e-6),
          "NTSC geometry by default");
    CHECK(pin_est_ffv1_factor(0) == 1.0 && pin_est_ffv1_factor(1) < 1.0 &&
          pin_est_ffv1_factor(2) <= pin_est_ffv1_factor(1) && pin_est_ffv1_factor(7) == 1.0, "FFV1 levels");
    if (g_failures == 0) printf("test_pin_estimate: ok\n");
    return g_failures;
}
