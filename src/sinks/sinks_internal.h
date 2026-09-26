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

/* Per-implementation constructors, wired up by the format switch in
 * pin_sink_factory.c. Not part of the public sinks API (pin_sink.h). */

#ifndef SINKS_INTERNAL_H
#define SINKS_INTERNAL_H

#include "pin_sink.h"

#ifdef __cplusplus
extern "C" {
#endif

pin_sink_t *sink_raw_create(void);
pin_sink_t *sink_avi_create(void);        /* PIN_FMT_ANALOG_AVI */
pin_sink_t *sink_ffv1_create(void);       /* PIN_FMT_ANALOG_FFV1_MKV */
pin_sink_t *sink_rewrap_create(pin_format_t format); /* DV_AVI, DV_MOV, HDV_MOV, HDV_MKV */

/* Shared by sink_ffv1.c and sink_rewrap.c: PAL/NTSC DV sample aspect ratios
 * per the FFmpeg dv profile tables (documented at their point of use). */
void pin_sink_sar_for(pin_kind_t kind, int is_pal, pin_aspect_t aspect, int width,
                      int *sar_num, int *sar_den);

#ifdef __cplusplus
}
#endif

#endif /* SINKS_INTERNAL_H */
