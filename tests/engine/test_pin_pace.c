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
 * pin_pace (the preview's jitter buffer) against synthetic arrivals: the
 * present times it hands out must step by the source frame period however
 * jittery or bursty the arrivals are, and come after the arrivals.
 */

#include "pin_pace.h"

#include <math.h>
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

static unsigned g_rng = 12345;
static double rnd(void) /* 0..1, deterministic */
{
    g_rng = g_rng * 1103515245u + 12345u;
    return (double)((g_rng >> 8) & 0xFFFF) / 65535.0;
}

typedef struct {
    double worst_step;  /* largest |step - period| after settling */
    double max_delay;
    int late;           /* frames whose present time is before their arrival */
} result_t;

/* n frames of a source running at `period` (the nominal one, times
 * 1 + `ppm`/1e6) starting at t0; jitter(i) gives each arrival's extra
 * lateness. Steps are judged from frame `settle` on. */
static result_t run(double period, double ppm, int n, int settle, double (*jitter)(int))
{
    pin_pace_t pc;
    memset(&pc, 0, sizeof(pc));
    result_t r = { 0, 0, 0 };
    double prev = 0;
    for (int i = 0; i < n; i++) {
        double arrive = 100.0 + i * period * (1 + ppm / 1e6) + jitter(i);
        double t = pin_pace_frame(&pc, arrive, period, 1);
        if (i >= settle) {
            double err = fabs(t - prev - period);
            if (err > r.worst_step) r.worst_step = err;
            if (t < arrive) r.late++;
            if (pc.delay > r.max_delay) r.max_delay = pc.delay;
        }
        prev = t;
    }
    return r;
}

static double steady(int i) { (void)i; return 0.0002 * rnd(); }           /* analog over USB */
static double coarse_sleep(int i) { (void)i; return 0.016 * rnd(); }      /* a ~16 ms timer */
static double pairs(int i) { return (i & 1) ? 0 : 0.040; }                /* two frames at once */

int main(void)
{
    /* analog: arrivals already even, so the delay stays near its minimum */
    result_t r = run(0.040, 0, 500, 50, steady);
    CHECK(r.worst_step < 0.0001, "steady 25 fps: even steps");
    CHECK(r.max_delay < 0.006, "steady 25 fps: small delay");
    CHECK(r.late == 0, "steady 25 fps: never before arrival");

    /* a source 0.1 % off its nominal rate (an analog VCR): the grid follows,
     * the delay stays small */
    r = run(0.040, 1000, 3000, 100, steady);
    printf("0.1%% off: worst step %.3f ms, delay %.1f ms, %d late\n", r.worst_step * 1000, r.max_delay * 1000, r.late);
    CHECK(r.worst_step < 0.0002, "0.1% off: even steps");
    CHECK(r.max_delay < 0.010, "0.1% off: small delay");
    CHECK(r.late == 0, "0.1% off: never before arrival");

    /* NTSC rate with 16 ms of jitter: the grid smooths it to well under 1 ms */
    r = run(1001.0 / 30000, 0, 1000, 200, coarse_sleep);
    printf("jittery: worst step %.3f ms, delay %.1f ms, %d late\n", r.worst_step * 1000, r.max_delay * 1000, r.late);
    CHECK(r.worst_step < 0.0003, "jittery 29.97 fps: steps within 0.3 ms");
    CHECK(r.max_delay < 0.030, "jittery 29.97 fps: delay bounded");
    CHECK(r.late < 10, "jittery 29.97 fps: (almost) never before arrival");

    /* bursts of two pictures every two periods */
    r = run(0.040, 0, 1000, 200, pairs);
    printf("bursts: worst step %.3f ms, delay %.1f ms, %d late\n", r.worst_step * 1000, r.max_delay * 1000, r.late);
    CHECK(r.worst_step < 0.0005, "bursts: even steps");
    CHECK(r.late == 0, "bursts: never before arrival");

    /* a dropped frame keeps its slot: the next present time is two periods on */
    pin_pace_t pc;
    memset(&pc, 0, sizeof(pc));
    double t = 0;
    for (int i = 0; i < 100; i++)
        t = pin_pace_frame(&pc, 10.0 + i * 0.040, 0.040, 1);
    double t2 = pin_pace_frame(&pc, 10.0 + 101 * 0.040, 0.040, 2);
    CHECK(fabs(t2 - t - 0.080) < 0.0005, "dropped frame: grid skips its slot");

    /* a long gap (paused, no signal) starts the grid over at the next arrival */
    double t3 = pin_pace_frame(&pc, 20.0, 0.040, 1);
    CHECK(t3 > 20.0 && t3 < 20.0 + 0.010, "after a gap: shortly after arrival");
    double t4 = pin_pace_frame(&pc, 20.040, 0.040, 1);
    CHECK(fabs(t4 - t3 - 0.040) < 0.0002, "after a gap: paced from the second frame on");

    /* rate not known: shown as it comes */
    CHECK(pin_pace_frame(&pc, 30.0, 0, 1) == 30.0, "no rate: shown on arrival");

    if (g_failures) {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
