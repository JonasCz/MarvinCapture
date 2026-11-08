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

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

/* DV frames a sink holds back while looking for one with audio (the streams are fixed by the header). */
#define DV_HOLD_MAX 8

#ifdef __cplusplus
extern "C" {
#endif

/* A condition variable for pin_cond_timedwait(): the wait runs on the monotonic clock, so a
 * wall-clock step (NTP, the user setting the time) cannot end it early or late. macOS has no
 * pthread_condattr_setclock(); its relative wait is monotonic already. Where the clock cannot
 * be set the plain variable is used and the wait falls back to the wall clock. */
#if !defined(__APPLE__)
static inline int *pin_cond_mono_flag(void)
{
    static int ok = 1;      /* per translation unit; init and wait are in the same one */
    return &ok;
}
#endif
static inline void pin_cond_init_timed(pthread_cond_t *c)
{
#if !defined(__APPLE__)
    pthread_condattr_t a;
    if (pthread_condattr_init(&a) == 0) {
        if (pthread_condattr_setclock(&a, CLOCK_MONOTONIC) == 0 && pthread_cond_init(c, &a) == 0) {
            pthread_condattr_destroy(&a);
            return;
        }
        pthread_condattr_destroy(&a);
    }
    *pin_cond_mono_flag() = 0;
#endif
    pthread_cond_init(c, NULL);
}

/* Waits up to `seconds` for c (mutex m held). Returns ETIMEDOUT or 0. */
static inline int pin_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, int seconds)
{
#if defined(__APPLE__)
    struct timespec rel = { .tv_sec = seconds, .tv_nsec = 0 };
    return pthread_cond_timedwait_relative_np(c, m, &rel);
#else
    struct timespec ts;
    clock_gettime(*pin_cond_mono_flag() ? CLOCK_MONOTONIC : CLOCK_REALTIME, &ts);
    ts.tv_sec += seconds;
    return pthread_cond_timedwait(c, m, &ts);
#endif
}

pin_sink_t *sink_raw_create(void);
pin_sink_t *sink_nut_create(void);        /* analog to stdout: NUT, YUY2 + PCM */
pin_sink_t *sink_avi_create(void);        /* PIN_FMT_ANALOG_AVI */
pin_sink_t *sink_ffv1_create(void);       /* PIN_FMT_ANALOG_FFV1_MKV */

/* Tests only: start sink_ffv1's timelines at the given video frame and
 * audio sample instead of 0, to check timestamp handling as if the capture
 * had already run for hours. Call between create and open. */
void sink_ffv1_test_set_start(pin_sink_t *s, int64_t video_frame, int64_t audio_sample);
pin_sink_t *sink_rewrap_create(pin_format_t format); /* DV_AVI, DV_MOV */
pin_sink_t *sink_hdvmux_create(pin_format_t format); /* HDV_MOV, HDV_MKV: live, repairs damaged GOPs */
/* Tests only: sink_hdvmux's limit of bytes waiting for a repair before it gives up (behind), and
 * a delay each repair takes on top. Call between create and open. */
void sink_hdvmux_test_set(pin_sink_t *s, uint64_t pend_limit, int worker_delay_ms);

/* Tests only (pin_sink_fault.c): the seam PIN_TEST_WRITE_FAIL_AFTER=<bytes>, a file that takes
 * that many bytes and refuses the rest, as a full drive would. pin_sink_fault_budget() is a new
 * file's budget (UINT64_MAX: the seam is off); pin_sink_fault_fwrite() is fwrite() within
 * *left (short, errno ENOSPC, where it runs out); pin_sink_fault_avio_open() opens path for a
 * muxer with such a budget, pin_sink_fault_avio_close() closes what it opened (negative: the final
 * flush or the close failed). */
struct AVIOContext;
uint64_t pin_sink_fault_budget(void);
size_t pin_sink_fault_fwrite(const void *data, size_t len, FILE *f, uint64_t *left);
int pin_sink_fault_avio_open(struct AVIOContext **pb, const char *path, uint64_t budget);
int pin_sink_fault_avio_close(struct AVIOContext **pb);
pin_sink_t *sink_hwenc_create(pin_format_t format);  /* ANALOG_ / DV_ H264_MKV, HEVC_MKV */

/* Shared by the sinks and the engine: PAL/NTSC DV sample aspect ratios per the
 * FFmpeg dv profile tables (documented at their point of use), and HDV's from its
 * raster. height is used for HDV (SD takes 576 / 480 lines from is_pal). */
void pin_sink_sar_for(pin_kind_t kind, int is_pal, pin_aspect_t aspect, int width, int height,
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
