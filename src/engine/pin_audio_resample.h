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
 * A small, dependency-free linear-interpolation resampler for interleaved
 * stereo s16 PCM, used to bring DV audio (48/44.1/32 kHz) and HDV's decoded
 * mp2 audio to the fixed 48 kHz the monitor ring / meters expect (pin_api.h's
 * pin_monitor_read()). The project's static FFmpeg build has swresample
 * disabled (see scripts/build-ffmpeg.sh), so this exists instead of swr_convert.
 *
 * Not a high-quality resampler (no anti-aliasing filter -- linear
 * interpolation between the two nearest input samples), which is an
 * acceptable tradeoff for a live monitor/meter feed, not archival audio
 * (the captured file always gets the original, unresampled bytes).
 *
 * One pin_resampler_t instance is meant to live for the duration of a single
 * continuous stream (e.g. one session's DV audio, or its HDV audio): it
 * carries the fractional read position and the last sample of the previous
 * call across calls so back-to-back chunks resample as if they were one
 * continuous buffer, with no audible click or drift at chunk boundaries.
 */

#ifndef PIN_AUDIO_RESAMPLE_H
#define PIN_AUDIO_RESAMPLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double pos;       /* next input-sample-index (fractional) to emit, in a
                          virtual timeline where index 0 is `last[]` (the
                          previous call's final sample) and index 1.. are
                          this call's in[] samples */
    int16_t last[2];  /* last emitted stereo sample of the previous call */
    int primed;
} pin_resampler_t;

/* Zeroes the state; call once before the first pin_resample_s16_stereo()
 * for a new stream (or when a stream's source rate changes discontinuously
 * and continuity across the gap isn't wanted). */
void pin_resampler_reset(pin_resampler_t *r);

/* Resamples in[0..in_frames) (interleaved stereo s16, in_rate Hz) to
 * out_rate Hz, writing up to out_cap frames to out[] and returning how many
 * were written. If in_rate == out_rate this is a plain bounded copy (state
 * untouched). Safe with in_frames == 0 (returns 0). */
size_t pin_resample_s16_stereo(pin_resampler_t *r, const int16_t *in, size_t in_frames,
                                int in_rate, int16_t *out, size_t out_cap, int out_rate);

#ifdef __cplusplus
}
#endif

#endif /* PIN_AUDIO_RESAMPLE_H */
