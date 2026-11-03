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
 * The format settings (pin_format_opts.c): which apply to which format, their ranges and
 * defaults, the ready-made texts, the size hint and the clamping.
 */

#include "pin_format_opts.h"
#include "pin_estimate.h"

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

static void get(pin_format_t f, pin_format_options_t *o)
{
    memset(o, 0x55, sizeof(*o));
    o->size = sizeof(*o);
    CHECK(pin_fmt_options(f, o) == PIN_OK, "options");
    CHECK(o->size == sizeof(*o), "size kept");
}

int main(void)
{
    pin_format_options_t o;

    /* which settings apply where */
    for (int f = 0; f < PIN_FMT_COUNT; f++) {
        get((pin_format_t)f, &o);
        int hw = f == PIN_FMT_ANALOG_H264_MKV || f == PIN_FMT_ANALOG_HEVC_MKV ||
                 f == PIN_FMT_DV_H264_MKV || f == PIN_FMT_DV_HEVC_MKV;
        CHECK(o.has_quality == hw && o.has_deinterlace == hw, "quality / deinterlace: H.264 and HEVC only");
        CHECK(o.has_ffv1_compression == (f == PIN_FMT_ANALOG_FFV1_MKV), "FFV1 compression: FFV1 only");
        CHECK((o.note[0] != 0) == hw, "note: the lossy formats");
        CHECK(pin_fmt_is_hwenc((pin_format_t)f) == hw, "is_hwenc");
        /* texts are filled for every format, so a GUI can lay out the panel once */
        CHECK(o.quality_label[0] && o.deinterlace_keep_label[0] && o.ffv1_smallest_label[0], "labels");
    }
    pin_format_options_t bad = { .size = sizeof(bad) };
    CHECK(pin_fmt_options(PIN_FMT_COUNT, &bad) == PIN_ERR_ARG, "unknown format");
    CHECK(pin_fmt_options(PIN_FMT_DV_RAW, NULL) == PIN_ERR_ARG, "NULL");

    get(PIN_FMT_ANALOG_H264_MKV, &o);
    CHECK(o.quality_min == 0 && o.quality_max == 51 && o.quality_default == 18 &&
          o.quality_lower_is_better == 1, "quality range");
    CHECK(strcmp(o.quality_label, "Quality") == 0 && strcmp(o.quality_low_label, "Best") == 0 &&
          strcmp(o.quality_high_label, "Smallest") == 0, "quality labels");
    CHECK(strstr(o.quality_help, "18") != NULL && strstr(o.quality_help, "CRF") != NULL, "quality help");
    CHECK(o.deinterlace_default == PIN_DEINTERLACE_DOUBLE_RATE, "deinterlaced by default");
    CHECK(strstr(o.deinterlace_keep_help, "loses quality") != NULL, "keep fields: the quality warning");
    CHECK(strstr(o.deinterlace_double_help, "50") != NULL && strstr(o.deinterlace_double_help, "59.94") != NULL,
          "double rate: 50 / 59.94");
    CHECK(strstr(o.note, "Lossy") != NULL && strstr(o.note, "FFV1") != NULL, "analog note");
    get(PIN_FMT_DV_HEVC_MKV, &o);
    CHECK(strstr(o.note, "DV") != NULL, "DV note");
    get(PIN_FMT_ANALOG_FFV1_MKV, &o);
    CHECK(o.ffv1_default == PIN_FFV1_FAST && strcmp(o.ffv1_fast_label, "Fast") == 0 &&
          strcmp(o.ffv1_smaller_label, "Smaller") == 0 && strcmp(o.ffv1_smallest_label, "Smallest") == 0,
          "FFV1 levels");
    CHECK(strstr(o.ffv1_help, "lossless") != NULL && strstr(o.ffv1_help, "CPU") != NULL, "FFV1 help");
    CHECK(strlen(o.quality_help) < PIN_OPT_HELP_MAX - 1 && strlen(o.deinterlace_keep_help) < PIN_OPT_HELP_MAX - 1 &&
          strlen(o.deinterlace_double_help) < PIN_OPT_HELP_MAX - 1 && strlen(o.ffv1_help) < PIN_OPT_HELP_MAX - 1,
          "help texts fit");

    /* the size hint */
    char h[128];
    pin_fmt_quality_hint(PIN_FMT_ANALOG_H264_MKV, 18, PIN_DEINTERLACE_KEEP_FIELDS, 0, h, sizeof(h));
    char want[64];
    double bps = pin_est_hwenc_rate(0, 0, 0, 0, 0, 18);
    snprintf(want, sizeof(want), "About %.1f GB per hour", bps * 3600 / 1e9);
    CHECK(strncmp(h, want, strlen(want)) == 0 && strstr(h, "Mbit/s") != NULL, "hint for H.264 18");
    printf("  hint: %s\n", h);
    pin_fmt_quality_hint(PIN_FMT_ANALOG_FFV1_MKV, 18, 0, 0, h, sizeof(h));
    CHECK(h[0] == 0, "no hint for FFV1");
    char h2[128];
    pin_fmt_quality_hint(PIN_FMT_DV_H264_MKV, 99, 7, 0, h, sizeof(h));
    pin_fmt_quality_hint(PIN_FMT_DV_H264_MKV, 51, PIN_DEINTERLACE_DOUBLE_RATE, 0, h2, sizeof(h2));
    CHECK(strcmp(h, h2) == 0, "out-of-range values clamped / defaulted");
    pin_fmt_quality_hint(PIN_FMT_DV_H264_MKV, 18, 0, 0, NULL, 0);    /* no crash */

    /* warnings */
    char w[512];
    pin_fmt_hwenc_warning(PIN_FMT_ANALOG_HEVC_MKV, w, sizeof(w));
    CHECK(strstr(w, "HEVC is lossy") != NULL, "analog warning");
    pin_fmt_hwenc_warning(PIN_FMT_DV_H264_MKV, w, sizeof(w));
    CHECK(strstr(w, "DV video to H.264") != NULL, "DV warning");
    pin_fmt_hwenc_warning(PIN_FMT_DV_RAW, w, sizeof(w));
    CHECK(w[0] == 0, "no warning for DV");
    pin_fmt_unavailable_text(PIN_FMT_ANALOG_HEVC_MKV, w, sizeof(w));
    CHECK(strstr(w, "HEVC encoder") != NULL, "unavailable text");

    /* clamping */
    CHECK(pin_fmt_clamp_quality(-1) == 0 && pin_fmt_clamp_quality(52) == 51 && pin_fmt_clamp_quality(30) == 30,
          "quality");
    CHECK(pin_fmt_clamp_deinterlace(0) == 0 && pin_fmt_clamp_deinterlace(1) == 1 &&
          pin_fmt_clamp_deinterlace(2) == PIN_DEINTERLACE_DEFAULT, "deinterlace");
    CHECK(pin_fmt_clamp_ffv1(2) == 2 && pin_fmt_clamp_ffv1(3) == PIN_FFV1_FAST && pin_fmt_clamp_ffv1(-1) == 0,
          "ffv1");

    if (g_failures == 0)
        printf("test_pin_format_opts: ok\n");
    return g_failures;
}
