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
pin_sink_t *sink_nut_create(void);        /* analog to stdout: NUT, YUY2 + PCM */
pin_sink_t *sink_avi_create(void);        /* PIN_FMT_ANALOG_AVI */
pin_sink_t *sink_ffv1_create(void);       /* PIN_FMT_ANALOG_FFV1_MKV */
pin_sink_t *sink_rewrap_create(pin_format_t format); /* DV_AVI, DV_MOV, HDV_MOV, HDV_MKV */

/* Shared by sink_ffv1.c and sink_rewrap.c: PAL/NTSC DV sample aspect ratios
 * per the FFmpeg dv profile tables (documented at their point of use). */
void pin_sink_sar_for(pin_kind_t kind, int is_pal, pin_aspect_t aspect, int width,
                      int *sar_num, int *sar_den);

/* sink_rewrap.c's per-frame DV audio rule, shared with its test. A track
 * that has written apts samples is handed avail more for the frame whose
 * end is target_after samples into the file. DV frames carry a varying
 * number of samples (1067 or 1068 at 32 kHz / 29.97 fps, 1600 or 1602 at
 * 48 kHz), so every sample is kept while the track stays within half a
 * frame of the video. Only beyond that (a frame whose audio did not demux,
 * or the demuxer returning two frames' worth) is it trimmed or padded back
 * onto the video timeline. frame is the samples in one frame. */
static inline void pin_dv_audio_fit(int64_t apts, int64_t avail, int64_t target_after,
                                    int64_t frame, int64_t *take, int64_t *pad)
{
    int64_t tol = frame / 2;
    *take = avail;
    *pad = 0;
    if (apts + avail > target_after + tol)
        *take = target_after > apts ? target_after - apts : 0;
    else if (apts + avail < target_after - tol)
        *pad = target_after - (apts + avail);
}

#ifdef __cplusplus
}
#endif

#endif /* SINKS_INTERNAL_H */
