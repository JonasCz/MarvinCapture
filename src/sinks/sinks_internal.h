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
#include "../core/pin_cond.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* DV frames a sink holds back while looking for one with audio (the streams are fixed by the header). */
#define DV_HOLD_MAX 8

#ifdef __cplusplus
extern "C" {
#endif

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

/* Tests only (pin_sink_fault.c): PIN_TEST_WRITE_STALL=<bytes>:<ms> makes a file opened through
 * pin_sink_fault_avio_open() (the seam turns that on by itself) stop for <ms> once, at the first
 * write past <bytes>, as a drive that spins up or a share that stalls would.
 * PIN_TEST_STALL_TIMEOUT_MS=<ms> replaces the encoder sinks' watchdog (STALL_TIMEOUT_S) and
 * close()'s patience with a thread stuck in a write (PIN_SINK_CLOSE_STALL_S) by <ms>. */
long pin_sink_fault_stall_timeout_ms(long dflt_ms);

/* A sink's status as its writer thread last left it, for readers on other threads (the
 * session's status, the size and disk checks): the writer thread updates its own copy freely and
 * publishes it after each call, so a reader never sees a half-updated struct. */
typedef struct {
    pthread_mutex_t m;
    pin_sink_status_t st;
} pin_sink_status_pub_t;

static inline void pin_sink_status_pub_init(pin_sink_status_pub_t *p)
{
    pthread_mutex_init(&p->m, NULL);
    memset(&p->st, 0, sizeof(p->st));
}

static inline void pin_sink_status_pub_destroy(pin_sink_status_pub_t *p)
{
    pthread_mutex_destroy(&p->m);
}

static inline void pin_sink_status_publish(pin_sink_status_pub_t *p, const pin_sink_status_t *st)
{
    pthread_mutex_lock(&p->m);
    p->st = *st;
    pthread_mutex_unlock(&p->m);
}

static inline void pin_sink_status_read(pin_sink_status_pub_t *p, pin_sink_status_t *out)
{
    pthread_mutex_lock(&p->m);
    *out = p->st;
    pthread_mutex_unlock(&p->m);
}

/* The encoder sinks (FFV1, H.264 / HEVC) write the file from their own threads. This tells a
 * drive that stalls (a thread is inside a file write) from an encoder that hangs (none is), and
 * lets close() give up on threads stuck in a write to a share that is gone instead of waiting for
 * the OS forever. */
#define PIN_SINK_CLOSE_STALL_S 60
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    int in_write;        /* threads inside a file write */
    uint64_t writes;     /* file writes finished */
    int done;            /* threads that ended */
} pin_sink_watch_t;

static inline void pin_sink_watch_init(pin_sink_watch_t *w)
{
    pthread_mutex_init(&w->m, NULL);
    pin_cond_init_timed(&w->c);
    w->in_write = 0;
    w->writes = 0;
    w->done = 0;
}

static inline void pin_sink_watch_destroy(pin_sink_watch_t *w)
{
    pthread_mutex_destroy(&w->m);
    pthread_cond_destroy(&w->c);
}

static inline void pin_sink_watch_write(pin_sink_watch_t *w, int begin)
{
    pthread_mutex_lock(&w->m);
    if (begin) {
        w->in_write++;
    } else {
        w->in_write--;
        w->writes++;
    }
    pthread_cond_broadcast(&w->c);
    pthread_mutex_unlock(&w->m);
}

/* 1 while a thread is inside a file write: a full queue is the drive's doing, not the encoder's */
static inline int pin_sink_watch_writing(pin_sink_watch_t *w)
{
    pthread_mutex_lock(&w->m);
    int n = w->in_write;
    pthread_mutex_unlock(&w->m);
    return n > 0;
}

/* A thread's last act. */
static inline void pin_sink_watch_thread_done(pin_sink_watch_t *w)
{
    pthread_mutex_lock(&w->m);
    w->done++;
    pthread_cond_broadcast(&w->c);
    pthread_mutex_unlock(&w->m);
}

/* Waits until `threads` threads have called pin_sink_watch_thread_done(), as long as something
 * moves (a thread ends or a write finishes) at least every stall_ms. 0: they ended (join them);
 * -1: stuck that long, most likely in a write that never returns (a share that is gone). */
static inline int pin_sink_watch_wait_done(pin_sink_watch_t *w, int threads, long stall_ms)
{
    pthread_mutex_lock(&w->m);
    for (;;) {
        if (w->done >= threads) {
            pthread_mutex_unlock(&w->m);
            return 0;
        }
        uint64_t writes = w->writes;
        int done = w->done;
        if (pin_cond_timedwait_ms(&w->c, &w->m, stall_ms) == ETIMEDOUT && w->writes == writes &&
            w->done == done && w->done < threads) {
            pthread_mutex_unlock(&w->m);
            return -1;
        }
    }
}

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
