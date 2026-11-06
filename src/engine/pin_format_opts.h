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
 * The format settings (pin_capture_opts_t's deinterlace, video_quality,
 * ffv1_compression): which apply to which format, their ranges and defaults,
 * and the ready-made texts the GUIs show for them (pin_format_options(),
 * pin_format_quality_hint(), the H.264 / HEVC part of pin_format_warning()).
 * Pure; tested by tests/engine/test_pin_format_opts.c.
 */

#ifndef PIN_FORMAT_OPTS_H
#define PIN_FORMAT_OPTS_H

#include "../api/pin_api.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PIN_QUALITY_MIN 0
#define PIN_QUALITY_MAX 51
#define PIN_QUALITY_DEFAULT 18
#define PIN_DEINTERLACE_DEFAULT PIN_DEINTERLACE_DOUBLE_RATE
#define PIN_FFV1_DEFAULT PIN_FFV1_FAST

/* 1 for the four H.264 / HEVC formats. */
int pin_fmt_is_hwenc(pin_format_t f);
/* 1 for the two HEVC ones. */
int pin_fmt_is_hevc(pin_format_t f);
/* 1 for the formats that always split into scenes (pin_format_info_t.scene_split_forced): HDV MOV
 * and MKV, written with timing of their own that must not run across another recording. */
int pin_fmt_always_splits(pin_format_t f);

/* pin_format_options() without the size check: fills every field but size. PIN_ERR_ARG for
 * an unknown format. */
pin_status_t pin_fmt_options(pin_format_t f, pin_format_options_t *out);

/* pin_format_quality_hint(). */
void pin_fmt_quality_hint(pin_format_t f, int video_quality, int deinterlace, int is_60hz,
                          char *out, size_t cap);

/* The H.264 / HEVC warning of pin_format_warning() (lossy; DV re-encoded), "" for any other
 * format. */
void pin_fmt_hwenc_warning(pin_format_t f, char *out, size_t cap);

/* The sentence for a capture refused because format f needs an encoder this machine lacks. */
void pin_fmt_unavailable_text(pin_format_t f, char *out, size_t cap);

/* Values out of range back into it: quality to 0..51, deinterlace and ffv1_compression to
 * their enums (anything else becomes the default). */
int pin_fmt_clamp_quality(int q);
int pin_fmt_clamp_deinterlace(int d);
int pin_fmt_clamp_ffv1(int c);

#ifdef __cplusplus
}
#endif

#endif /* PIN_FORMAT_OPTS_H */
