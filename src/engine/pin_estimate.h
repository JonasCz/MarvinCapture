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
 * Capture size / time estimates (pure, unit-tested in tests/engine/test_pin_estimate.c).
 *
 *  - default data rates: DV and HDV are ~25 Mbit/s, 13 GB/h; analog AVI is
 *    computed from what the core writes (YUY2 720 x 576|480 at 25|29.97 fps
 *    plus 48 kHz 16-bit stereo PCM); analog FFV1 defaults to 30 GB/h until a
 *    capture has taught us the user's own rate.
 *  - the learned FFV1 rate is the average over the last 10 minutes of a
 *    capture (pin_rate_window_t), kept in the core settings as
 *    core.ffv1_bytes_per_hour (section "core", key "ffv1_bytes_per_hour").
 *  - while capturing the measured rate wins over both.
 */

#ifndef PIN_ESTIMATE_H
#define PIN_ESTIMATE_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PIN_EST_DV_BYTES_PER_HOUR 13.0e9
#define PIN_EST_HDV_BYTES_PER_HOUR 13.0e9
#define PIN_EST_FFV1_BYTES_PER_HOUR 30.0e9

typedef enum {
    PIN_EST_DV = 0,
    PIN_EST_HDV,
    PIN_EST_ANALOG_AVI,
    PIN_EST_ANALOG_FFV1
} pin_est_kind_t;

/* ---- H.264 / HEVC: quality -> bit rate --------------------------------------------
 *
 * pin_capture_opts_t.video_quality is on x264's CRF scale: 0..51, lower is better, 18 by
 * default, 6 steps double the size. Each encoder gets the nearest thing it has
 * (sink_hwenc.c / pin_hwenc.c): Media Foundation a constant QP of the same number, Apple
 * Silicon's VideoToolbox constant quality (pin_hwenc_vt_quality()), and everything else
 * (MediaCodec, whose constant-quality mode few phone encoders have and the NDK cannot
 * query; VideoToolbox on Intel) a variable bit rate from this formula:
 *
 *     bits per pixel = 0.30 * 2^((23 - quality) / 6)      (H.264)
 *                      x 0.65                             (HEVC: about a third smaller)
 *     bit rate       = bits per pixel * width * height * output frames per second
 *                      x 0.75 when deinterlacing doubled the frame rate (two pictures
 *                      from one frame have much in common, so 50p costs 1.5x 25i, not 2x)
 *
 * clamped to PIN_EST_VIDEO_MIN_BPS .. PIN_EST_VIDEO_MAX_BPS. For PAL at 25 fps that is
 * 5.5 Mbit/s at 18 and 3.1 Mbit/s at 23 for H.264, 3.6 / 2.0 Mbit/s for HEVC; a hardware
 * encoder needs about this much more than x264 for the same picture. The ceiling is the
 * 12 Mbit/s Android's software AVC encoder (the fallback on phones without a hardware one)
 * accepts; 20 Mbit/s elsewhere, beyond which SD has nothing left to gain. The same formula
 * is the size estimate for every encoder (pin_est_hwenc_rate()). */
#define PIN_EST_VIDEO_BPP_AT_23 0.30
#define PIN_EST_VIDEO_HEVC_FACTOR 0.65
#define PIN_EST_VIDEO_DOUBLED_FACTOR 0.75
#define PIN_EST_VIDEO_MIN_BPS 250e3
#if defined(__ANDROID__)
#define PIN_EST_VIDEO_MAX_BPS 12e6
#else
#define PIN_EST_VIDEO_MAX_BPS 20e6
#endif
/* FLAC of 48 kHz 16-bit stereo comes to about 60 % of the 1.536 Mbit/s of PCM */
#define PIN_EST_FLAC_BPS 0.9e6

/* Video bits per second for quality q (clamped to 0..51) at width x height and fps_out
 * output frames per second; hevc 0/1; doubled = deinterlaced to twice the frame rate. */
static inline double pin_est_video_bitrate(int hevc, int width, int height, double fps_out,
                                           int doubled, int q)
{
    if (q < 0) q = 0;
    if (q > 51) q = 51;
    double bpp = PIN_EST_VIDEO_BPP_AT_23 * pow(2.0, (23.0 - q) / 6.0);
    if (hevc)
        bpp *= PIN_EST_VIDEO_HEVC_FACTOR;
    double bps = bpp * (double)width * (double)height * fps_out;
    if (doubled)
        bps *= PIN_EST_VIDEO_DOUBLED_FACTOR;
    if (bps < PIN_EST_VIDEO_MIN_BPS) bps = PIN_EST_VIDEO_MIN_BPS;
    if (bps > PIN_EST_VIDEO_MAX_BPS) bps = PIN_EST_VIDEO_MAX_BPS;
    return bps;
}

/* Bytes per second of an H.264 / HEVC + FLAC file: SD of the standard (720 x 576 at 25 fps,
 * or 720 x 480 at 29.97 when is_60hz; width / height 0 = that size), deinterlace doubling the
 * frame rate or not, quality q. Container overhead ~1 %. */
double pin_est_hwenc_rate(int hevc, int width, int height, int is_60hz, int deinterlace, int q);

/* FFV1 compression levels (pin_ffv1_compression_t) against "fast": the default 30 GB/h and
 * the learned rate are for "fast" (the learned one is stored divided by the factor of the
 * level it was measured at). A rough 0.95 for both slower levels (see sink_ffv1.c); the
 * rate measured during the capture takes over after 10 s anyway. */
double pin_est_ffv1_factor(int ffv1_compression);

typedef enum {
    PIN_EST_SRC_DEFAULT = 0,    /* built-in nominal rate */
    PIN_EST_SRC_LEARNED = 1,    /* FFV1 rate learned from an earlier capture */
    PIN_EST_SRC_MEASURED = 2    /* measured on the running capture */
} pin_est_source_t;

/* Nominal bytes per second. width/height/is_60hz describe the analog picture
 * (0 = unknown: the standard size for is_60hz is assumed); ignored for
 * DV/HDV. learned_bph > 0 replaces the FFV1 default; *source (optional) says
 * which one was used. */
double pin_est_default_rate(pin_est_kind_t kind, int width, int height, int is_60hz,
                            double learned_bph, int *source);

/* Rate to use: measured_bps > 0 wins (source MEASURED), else the default. */
double pin_est_rate(pin_est_kind_t kind, int width, int height, int is_60hz, double learned_bph,
                    double measured_bps, int *source);

/* free_bytes / rate, or -1 when the rate is unknown. */
double pin_est_seconds_left(uint64_t free_bytes, double bytes_per_s);

/* Settings persistence of the learned FFV1 rate. load returns bytes/hour, or 0
 * when unset or implausible (< 1 GB/h, > 300 GB/h); store ignores implausible
 * values and returns 0 on success. path is the settings file. */
double pin_est_load_ffv1(const char *settings_path);
int pin_est_store_ffv1(const char *settings_path, double bytes_per_hour);

/* Rolling window of (time, total bytes) samples for the measured rate. */
#define PIN_RATE_SAMPLES 140
#define PIN_RATE_SPACING_S 5.0
#define PIN_RATE_WINDOW_S 600.0

typedef struct {
    double t[PIN_RATE_SAMPLES];
    uint64_t b[PIN_RATE_SAMPLES];
    int head;       /* next slot */
    int count;
    double latest_t;
    uint64_t latest_b;
    int have_latest;
} pin_rate_window_t;

void pin_rate_reset(pin_rate_window_t *w);
/* total_bytes must not decrease; t in seconds. Cheap, call as often as wanted. */
void pin_rate_add(pin_rate_window_t *w, double t, uint64_t total_bytes);
/* Average bytes/s over the last PIN_RATE_WINDOW_S, or -1 if the samples span
 * less than min_span_s. */
double pin_rate_bytes_per_s(const pin_rate_window_t *w, double min_span_s);

#ifdef __cplusplus
}
#endif

#endif /* PIN_ESTIMATE_H */
