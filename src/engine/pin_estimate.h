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

/* free_bytes / rate, or 0 when the rate is unknown. */
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
