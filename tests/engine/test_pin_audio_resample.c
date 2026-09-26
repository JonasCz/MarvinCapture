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
 * ctest: pin_audio_resample.c's linear-interpolation resampler, hardware-
 * free. Uses a perfectly linear ramp as input: linear interpolation of a
 * linear function is exact, so with a ratio/step chosen so ratio*step is a
 * whole number, every output sample has an exactly predictable value with
 * no floating-point tolerance needed.
 */

#include "pin_audio_resample.h"
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static void test_identity_same_rate(void)
{
    pin_resampler_t r;
    pin_resampler_reset(&r);
    int16_t in[6] = { 1, 2, 3, 4, 5, 6 }; /* 3 stereo frames */
    int16_t out[3 * 2] = { 0 };
    size_t n = pin_resample_s16_stereo(&r, in, 3, 48000, out, 3, 48000);
    CHECK(n == 3, "same-rate resample copies every frame");
    CHECK(memcmp(in, out, sizeof(in)) == 0, "same-rate resample is a plain copy");
}

static void test_downsample_exact_ramp(void)
{
    /* in_rate=32000 -> out_rate=48000 is an *upsample* (ratio in/out = 2/3);
     * step chosen so ratio*step is a whole number (2), making every output
     * sample exactly predictable: value(k) = 2*k. */
    pin_resampler_t r;
    pin_resampler_reset(&r);
    enum { N = 100 };
    int16_t in[N * 2];
    for (int n = 0; n < N; n++) {
        in[n * 2 + 0] = (int16_t)(n * 3);  /* L step = 3 */
        in[n * 2 + 1] = (int16_t)(n * 6);  /* R step = 6 */
    }
    int16_t out[512];
    size_t out_n = pin_resample_s16_stereo(&r, in, N, 32000, out, 256, 48000);
    CHECK(out_n > 50 && out_n < 200, "a plausible number of output frames came back");
    /* value(k) = round(k * ratio * step); ratio*step = (32000/48000)*3 = 2
     * for L, 4 for R -- exact integers, no rounding tolerance needed. */
    for (size_t k = 0; k < out_n && k < 10; k++) {
        CHECK(out[k * 2 + 0] == (int16_t)(2 * k), "L sample k matches the exact linear ramp");
        CHECK(out[k * 2 + 1] == (int16_t)(4 * k), "R sample k matches the exact linear ramp");
    }
    /* also check a sample well past the start */
    if (out_n > 50) {
        size_t k = 50;
        CHECK(out[k * 2 + 0] == (int16_t)(2 * k), "L sample 50 matches");
        CHECK(out[k * 2 + 1] == (int16_t)(4 * k), "R sample 50 matches");
    }
}

static void test_continuity_across_chunks(void)
{
    enum { N = 100 };
    int16_t in[N * 2];
    for (int n = 0; n < N; n++) {
        in[n * 2 + 0] = (int16_t)(n * 3);
        in[n * 2 + 1] = (int16_t)(n * 6);
    }

    pin_resampler_t r_whole;
    pin_resampler_reset(&r_whole);
    int16_t out_whole[512];
    size_t n_whole = pin_resample_s16_stereo(&r_whole, in, N, 32000, out_whole, 256, 48000);

    pin_resampler_t r_split;
    pin_resampler_reset(&r_split);
    int16_t out_split[512];
    size_t n1 = pin_resample_s16_stereo(&r_split, in, 60, 32000, out_split, 256, 48000);
    size_t n2 = pin_resample_s16_stereo(&r_split, in + 60 * 2, N - 60, 32000, out_split + n1 * 2,
                                        256 - n1, 48000);
    size_t n_split = n1 + n2;

    CHECK(n_whole > 0 && n_split > 0, "both paths produced output");
    size_t common = n_whole < n_split ? n_whole : n_split;
    /* Splitting the same continuous ramp into two chunks must resample
     * identically to feeding it whole -- no click/drift at the boundary. */
    int mismatch = 0;
    for (size_t k = 0; k < common; k++) {
        if (out_whole[k * 2 + 0] != out_split[k * 2 + 0] ||
            out_whole[k * 2 + 1] != out_split[k * 2 + 1]) {
            mismatch = 1;
            break;
        }
    }
    CHECK(!mismatch, "chunked resampling matches whole-buffer resampling sample-for-sample");
}

static void test_empty_input_is_safe(void)
{
    pin_resampler_t r;
    pin_resampler_reset(&r);
    int16_t out[4];
    size_t n = pin_resample_s16_stereo(&r, NULL, 0, 44100, out, 4, 48000);
    CHECK(n == 0, "zero input frames yields zero output, no crash");
}

int main(void)
{
    test_identity_same_rate();
    test_downsample_exact_ramp();
    test_continuity_across_chunks();
    test_empty_input_is_safe();

    if (g_failures == 0) {
        printf("test_pin_audio_resample: all tests passed\n");
        return 0;
    }
    printf("test_pin_audio_resample: %d failure(s)\n", g_failures);
    return g_failures;
}
