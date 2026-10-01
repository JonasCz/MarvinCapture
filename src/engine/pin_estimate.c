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
#include "pin_settings.h"
#include <string.h>

double pin_est_default_rate(pin_est_kind_t kind, int width, int height, int is_60hz,
                            double learned_bph, int *source)
{
    if (source) *source = PIN_EST_SRC_DEFAULT;
    switch (kind) {
    case PIN_EST_DV:  return PIN_EST_DV_BYTES_PER_HOUR / 3600.0;
    case PIN_EST_HDV: return PIN_EST_HDV_BYTES_PER_HOUR / 3600.0;
    case PIN_EST_ANALOG_FFV1:
        if (learned_bph > 0) {
            if (source) *source = PIN_EST_SRC_LEARNED;
            return learned_bph / 3600.0;
        }
        return PIN_EST_FFV1_BYTES_PER_HOUR / 3600.0;
    case PIN_EST_ANALOG_AVI:
    default: {
        if (width <= 0 || height <= 0) { width = 720; height = is_60hz ? 480 : 576; }
        double fps = is_60hz ? 30000.0 / 1001.0 : 25.0;
        /* YUY2 is 2 bytes per pixel; audio is 48 kHz, 16-bit, stereo */
        return (double)width * height * 2.0 * fps + 48000.0 * 2 * 2;
    }
    }
}

double pin_est_rate(pin_est_kind_t kind, int width, int height, int is_60hz, double learned_bph,
                    double measured_bps, int *source)
{
    if (measured_bps > 0) {
        if (source) *source = PIN_EST_SRC_MEASURED;
        return measured_bps;
    }
    return pin_est_default_rate(kind, width, height, is_60hz, learned_bph, source);
}

double pin_est_seconds_left(uint64_t free_bytes, double bytes_per_s)
{
    return bytes_per_s > 0 ? (double)free_bytes / bytes_per_s : 0.0;
}

#define FFV1_SECTION "core"
#define FFV1_KEY "ffv1_bytes_per_hour"
#define FFV1_MIN_BPH 1.0e9
#define FFV1_MAX_BPH 300.0e9

double pin_est_load_ffv1(const char *settings_path)
{
    if (!settings_path) return 0;
    pin_settings_t st;
    pin_settings_init(&st);
    pin_settings_load(&st, settings_path);
    double v = pin_settings_get_double(&st, FFV1_SECTION, FFV1_KEY, 0.0);
    pin_settings_free(&st);
    return (v >= FFV1_MIN_BPH && v <= FFV1_MAX_BPH) ? v : 0.0;
}

int pin_est_store_ffv1(const char *settings_path, double bytes_per_hour)
{
    if (!settings_path || bytes_per_hour < FFV1_MIN_BPH || bytes_per_hour > FFV1_MAX_BPH)
        return -1;
    pin_settings_t st;
    pin_settings_init(&st);
    pin_settings_load(&st, settings_path);
    pin_settings_set_double(&st, FFV1_SECTION, FFV1_KEY, bytes_per_hour);
    int rc = pin_settings_save(&st, settings_path);
    pin_settings_free(&st);
    return rc == 0 ? 0 : -1;
}

void pin_rate_reset(pin_rate_window_t *w)
{
    memset(w, 0, sizeof(*w));
}

void pin_rate_add(pin_rate_window_t *w, double t, uint64_t total_bytes)
{
    w->latest_t = t;
    w->latest_b = total_bytes;
    w->have_latest = 1;
    if (w->count > 0) {
        int last = (w->head + PIN_RATE_SAMPLES - 1) % PIN_RATE_SAMPLES;
        if (t - w->t[last] < PIN_RATE_SPACING_S)
            return;
    }
    w->t[w->head] = t;
    w->b[w->head] = total_bytes;
    w->head = (w->head + 1) % PIN_RATE_SAMPLES;
    if (w->count < PIN_RATE_SAMPLES) w->count++;
}

double pin_rate_bytes_per_s(const pin_rate_window_t *w, double min_span_s)
{
    if (!w->have_latest || w->count == 0)
        return -1;
    /* the oldest stored sample still inside the window */
    int oldest = -1;
    for (int i = 0; i < w->count; i++) {
        int idx = (w->head + PIN_RATE_SAMPLES - w->count + i) % PIN_RATE_SAMPLES;
        if (w->latest_t - w->t[idx] <= PIN_RATE_WINDOW_S) { oldest = idx; break; }
    }
    if (oldest < 0)
        return -1;
    double span = w->latest_t - w->t[oldest];
    if (span < min_span_s || span <= 0 || w->latest_b < w->b[oldest])
        return -1;
    return (double)(w->latest_b - w->b[oldest]) / span;
}
