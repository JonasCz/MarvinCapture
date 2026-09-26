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

#include "pin_audio_resample.h"

#include <math.h>
#include <string.h>

void pin_resampler_reset(pin_resampler_t *r)
{
    memset(r, 0, sizeof(*r));
}

/* Virtual timeline: index 0 = r->last[] (previous call's final sample, or
 * this call's own in[0] the very first time, so the first output sample is
 * exact rather than interpolated from silence); index k (1..in_frames) =
 * in[k-1]. */
static void sample_at(const int16_t *in, size_t in_frames, const int16_t last[2], long idx,
                       int16_t out2[2])
{
    if (idx <= 0) {
        out2[0] = last[0];
        out2[1] = last[1];
    } else if ((size_t)idx <= in_frames) {
        out2[0] = in[(idx - 1) * 2 + 0];
        out2[1] = in[(idx - 1) * 2 + 1];
    } else {
        /* Never requested (the loop below stops before it needs this), kept
         * only so an off-by-one can't read out of bounds. */
        out2[0] = in[(in_frames - 1) * 2 + 0];
        out2[1] = in[(in_frames - 1) * 2 + 1];
    }
}

size_t pin_resample_s16_stereo(pin_resampler_t *r, const int16_t *in, size_t in_frames,
                                int in_rate, int16_t *out, size_t out_cap, int out_rate)
{
    if (!r || !in || !out || in_frames == 0 || out_cap == 0 || in_rate <= 0 || out_rate <= 0)
        return 0;

    if (in_rate == out_rate) {
        size_t n = in_frames < out_cap ? in_frames : out_cap;
        memcpy(out, in, n * 2 * sizeof(int16_t));
        r->last[0] = in[(n - 1) * 2 + 0];
        r->last[1] = in[(n - 1) * 2 + 1];
        r->primed = 1;
        r->pos = 0.0; /* unused when rates match; keep it sane regardless */
        return n;
    }

    if (!r->primed) {
        r->last[0] = in[0];
        r->last[1] = in[1];
        r->pos = 1.0; /* start exactly on in[0] (virtual index 1), no lead-in guess */
        r->primed = 1;
    }

    double ratio = (double)in_rate / (double)out_rate; /* input samples per output sample */
    size_t out_n = 0;
    double pos = r->pos;

    while (out_n < out_cap) {
        long i0 = (long)floor(pos);
        double t = pos - (double)i0;
        long i1 = i0 + 1;
        if ((size_t)i1 > in_frames)
            break; /* not enough input yet for the next output sample */

        int16_t s0[2], s1[2];
        sample_at(in, in_frames, r->last, i0, s0);
        sample_at(in, in_frames, r->last, i1, s1);
        for (int ch = 0; ch < 2; ch++) {
            double v = s0[ch] + t * (double)(s1[ch] - s0[ch]);
            if (v > 32767.0) v = 32767.0;
            if (v < -32768.0) v = -32768.0;
            out[out_n * 2 + ch] = (int16_t)lrint(v);
        }
        out_n++;
        pos += ratio;
    }

    /* Carry state forward: the next call's virtual index 0 is this call's
     * last real input sample, so translate `pos` (and any not-yet-consumed
     * fractional position) into that new frame of reference. */
    r->last[0] = in[(in_frames - 1) * 2 + 0];
    r->last[1] = in[(in_frames - 1) * 2 + 1];
    r->pos = pos - (double)in_frames;
    return out_n;
}
