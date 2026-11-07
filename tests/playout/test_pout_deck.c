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

/* pout_deck: the virtual transport on a fake clock, over a fake tape of two
 * recordings with a timecode gap. Default timing: play delay 1.0 s, pause/stop
 * 0.3 s, wind start 0.5 s, wind 30x (750 frames/s at 25 fps, so the 1500-frame
 * tape winds end to end in 2 s), search settle 1.5 s. */

#include "pout_deck.h"
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

/* Fake tape: two recordings, 25 fps. Frames 0..749 carry timecode 00:00:00:00.., frames 750..1499
 * carry 01:00:00:00.. (so the timecode jumps by an hour between them, and 00:00:30:00 to
 * 00:59:59:24 is not on the tape). */
#define REC_LEN 750
#define REC_B_START_FRAMES (3600 * 25)
static int fake_tc_of_frame(void *ctx, int64_t f, pin_tc_t *tc)
{
    (void)ctx;
    if (f < 0 || f >= 2 * REC_LEN)
        return -1;
    int64_t n = f < REC_LEN ? f : REC_B_START_FRAMES + (f - REC_LEN);
    tc->f = (int)(n % 25);
    tc->s = (int)(n / 25 % 60);
    tc->m = (int)(n / 25 / 60 % 60);
    tc->h = (int)(n / 25 / 3600);
    tc->sep = ':';
    return 0;
}
static int64_t fake_frame_of_tc(void *ctx, const pin_tc_t *tc)
{
    (void)ctx;
    int64_t n = (((int64_t)tc->h * 60 + tc->m) * 60 + tc->s) * 25 + tc->f;
    if (n >= 0 && n < REC_LEN)
        return n;
    if (n >= REC_B_START_FRAMES && n < REC_B_START_FRAMES + REC_LEN)
        return REC_LEN + (n - REC_B_START_FRAMES);
    return -1;
}
static void fake_ops(pout_tape_ops_t *ops)
{
    ops->ctx = NULL;
    ops->frames = 2 * REC_LEN;
    ops->fps = 25.0;
    ops->tc_of_frame = fake_tc_of_frame;
    ops->frame_of_tc = fake_frame_of_tc;
}

#define FP 0.04     /* frame period at 25 fps */

static void fresh(pout_deck_t *d, double t0)
{
    pout_tape_ops_t ops;
    fake_ops(&ops);
    pout_deck_init(d, &ops, NULL, t0);
}

/* Runs the output loop from t0 for n periods, returns the kinds counted. */
static void run_out(pout_deck_t *d, double t0, int n, int *frames, int *repeats, int *blanks, int64_t *last)
{
    *frames = *repeats = *blanks = 0;
    for (int i = 0; i < n; i++) {
        int64_t f = -1;
        pout_out_kind_t k = pout_deck_next_output(d, t0 + i * FP, &f);
        if (k == POUT_OUT_FRAME) (*frames)++;
        else if (k == POUT_OUT_REPEAT) (*repeats)++;
        else (*blanks)++;
        if (k != POUT_OUT_BLANK && last)
            *last = f;
    }
}

static void test_play_pause(void)
{
    pout_deck_t d;
    fresh(&d, 0);
    int64_t f = -1;
    CHECK(pout_deck_state(&d, 0) == POUT_DECK_STOP, "starts stopped");
    CHECK(pout_deck_next_output(&d, 0, &f) == POUT_OUT_BLANK, "stopped: blank");
    CHECK(!pout_deck_moving(&d, 0), "stopped is not moving");

    pout_deck_play(&d, 10.0);
    CHECK(pout_deck_state(&d, 10.0) == POUT_DECK_PLAY, "reports PLAY at once");
    CHECK(pout_deck_in_transition(&d, 10.0), "in transition during the delay");
    CHECK(pout_deck_moving(&d, 10.0), "moving");
    CHECK(pout_deck_next_output(&d, 10.5, &f) == POUT_OUT_BLANK, "no picture before the delay is over");
    CHECK(!pout_deck_picture(&d, 10.5), "picture off");
    CHECK(pout_deck_next_output(&d, 10.99, &f) == POUT_OUT_BLANK, "still blank just before");
    CHECK(pout_deck_next_output(&d, 11.0, &f) == POUT_OUT_FRAME && f == 0, "frame 0 right after the delay");
    CHECK(!pout_deck_in_transition(&d, 11.0), "transition over");

    /* a jittery clock: consecutive frames, no skip, no repeat */
    double t = 11.0;
    int64_t expect = 1;
    int ok = 1;
    for (int i = 0; i < 300; i++) {
        t += FP + ((i * 7) % 5 - 2) * 0.01;           /* +-20 ms of jitter */
        if (i % 50 == 49)
            t += 0.5;                                 /* and a stall */
        if (pout_deck_next_output(&d, t, &f) != POUT_OUT_FRAME || f != expect++)
            ok = 0;
    }
    CHECK(ok, "play gives consecutive frames whatever the clock does");
    CHECK(pout_deck_position(&d, t) == 301, "position follows the output calls");
    pin_tc_t tc;
    CHECK(pout_deck_timecode(&d, t, &tc) == 0 && tc.s == 12 && tc.f == 1 && tc.m == 0 && tc.h == 0,
          "timecode of frame 301 is 00:00:12:01");
    /* a clock that steps back changes nothing */
    CHECK(pout_deck_next_output(&d, t - 5.0, &f) == POUT_OUT_FRAME && f == 301, "backwards clock still gives the next frame");

    /* pause: reported at once, the picture goes on for the pause delay, then repeats the last frame */
    double tp = t + 1.0;
    pout_deck_pause(&d, tp);
    CHECK(pout_deck_state(&d, tp) == POUT_DECK_PAUSE, "PAUSE reported at once");
    CHECK(!pout_deck_moving(&d, tp), "paused is not moving");
    int64_t last_frame = -1;
    int fr = 0, rp = 0, bl = 0;
    run_out(&d, tp, 7, &fr, &rp, &bl, &last_frame);          /* 0 .. 0.24 s */
    CHECK(fr == 7 && rp == 0, "frames keep coming during the pause delay");
    run_out(&d, tp + 0.4, 10, &fr, &rp, &bl, &last_frame);
    CHECK(fr == 0 && rp == 10 && bl == 0, "then the frame repeats");
    CHECK(last_frame == 308, "the repeated frame is the last one sent");
    pout_deck_next_output(&d, tp + 1.0, &f);
    CHECK(f == last_frame, "repeat stays on one frame");

    /* play again: delay, then it continues with that frame */
    pout_deck_play(&d, tp + 2.0);
    CHECK(pout_deck_next_output(&d, tp + 2.5, &f) == POUT_OUT_REPEAT, "pause shows its frame during the start delay");
    CHECK(pout_deck_next_output(&d, tp + 3.0, &f) == POUT_OUT_FRAME && f == last_frame, "resume starts at the paused frame");
    CHECK(pout_deck_next_output(&d, tp + 3.04, &f) == POUT_OUT_FRAME && f == last_frame + 1, "and goes on");

    /* stop: state at once, picture for 0.3 s more, then nothing */
    double ts = tp + 4.0;
    pout_deck_stop(&d, ts);
    CHECK(pout_deck_state(&d, ts) == POUT_DECK_STOP, "STOP reported at once");
    CHECK(pout_deck_next_output(&d, ts + 0.1, &f) == POUT_OUT_FRAME, "picture during the stop delay");
    CHECK(pout_deck_next_output(&d, ts + 0.4, &f) == POUT_OUT_BLANK, "blank after it");
    CHECK(pout_deck_position(&d, ts + 0.5) > 300, "stopped in place");
}

static void test_commands_cancel(void)
{
    pout_deck_t d;
    fresh(&d, 0);
    int64_t f;
    pout_deck_play(&d, 0.0);
    pout_deck_stop(&d, 0.1);        /* before the picture started */
    CHECK(pout_deck_state(&d, 0.1) == POUT_DECK_STOP && !pout_deck_in_transition(&d, 0.1), "stop cancels a pending play");
    CHECK(pout_deck_next_output(&d, 2.0, &f) == POUT_OUT_BLANK, "no picture came");
    /* play pause play quickly: ends playing */
    pout_deck_play(&d, 3.0);
    pout_deck_pause(&d, 3.1);
    pout_deck_play(&d, 3.2);
    CHECK(pout_deck_state(&d, 3.2) == POUT_DECK_PLAY, "last command wins");
    CHECK(pout_deck_next_output(&d, 4.3, &f) == POUT_OUT_FRAME && f == 0, "plays after the delay of the last play");
    /* zero start delay */
    pout_deck_timing_t tm;
    pout_deck_timing_defaults(&tm);
    tm.play_delay = 0;
    pout_tape_ops_t ops;
    fake_ops(&ops);
    pout_deck_init(&d, &ops, &tm, 0);
    pout_deck_play(&d, 1.0);
    CHECK(pout_deck_next_output(&d, 1.0, &f) == POUT_OUT_FRAME && f == 0, "start delay 0: first frame at once");
}

static void test_wind(void)
{
    pout_deck_t d;
    fresh(&d, 0);
    int64_t f;
    /* REW at the start of the tape: reported, then stops by itself */
    pout_deck_rew(&d, 1.0);
    CHECK(pout_deck_state(&d, 1.0) == POUT_DECK_REW, "REW reported");
    CHECK(pout_deck_state(&d, 1.2) == POUT_DECK_REW, "still REW during the wind delay");
    CHECK(pout_deck_state(&d, 1.6) == POUT_DECK_STOP, "REW at frame 0 stops (wind stop)");
    CHECK(pout_deck_position(&d, 1.6) == 0, "position 0");

    pout_deck_ff(&d, 10.0);
    CHECK(pout_deck_state(&d, 10.0) == POUT_DECK_FF && pout_deck_moving(&d, 10.0), "FF reported");
    CHECK(pout_deck_position(&d, 10.4) == 0, "does not move during the wind delay");
    int64_t p = pout_deck_position(&d, 11.0);       /* 0.5 s at 750 frames/s */
    CHECK(p >= 374 && p <= 375, "30x: 375 frames in 0.5 s of winding");
    CHECK(pout_deck_next_output(&d, 11.0, &f) == POUT_OUT_BLANK, "nothing goes out while winding");
    CHECK(pout_deck_state(&d, 12.4) == POUT_DECK_FF, "still winding before the end");
    CHECK(pout_deck_state(&d, 12.6) == POUT_DECK_STOP, "FF at the end stops");
    CHECK(pout_deck_position(&d, 12.6) == 1499, "clamped at the last frame");
    pin_tc_t tc;
    CHECK(pout_deck_timecode(&d, 12.6, &tc) == 0 && tc.h == 1 && tc.m == 0 && tc.s == 29 && tc.f == 24, "timecode at the end");

    pout_deck_rew(&d, 20.0);
    CHECK(pout_deck_state(&d, 21.0) == POUT_DECK_REW, "REW from the end");
    CHECK(pout_deck_state(&d, 22.4) == POUT_DECK_REW, "winding back");
    CHECK(pout_deck_state(&d, 22.6) == POUT_DECK_STOP && pout_deck_position(&d, 22.6) == 0, "REW stops at frame 0");

    /* a wind interrupted by STOP stays where it was; PLAY continues there */
    pout_deck_ff(&d, 30.0);
    pout_deck_stop(&d, 31.0);       /* 0.5 s of winding: frame 375 */
    CHECK(pout_deck_state(&d, 31.0) == POUT_DECK_STOP, "wind stop is immediate");
    int64_t at = pout_deck_position(&d, 35.0);
    CHECK(at >= 374 && at <= 375, "stopped where the wind was");
    pout_deck_play(&d, 35.0);
    CHECK(pout_deck_next_output(&d, 36.0, &f) == POUT_OUT_FRAME && f == at, "play continues from there");

    /* a long gap between two calls gives the same result as short ones */
    fresh(&d, 0);
    pout_deck_ff(&d, 0);
    CHECK(pout_deck_position(&d, 100.0) == 1499 && pout_deck_state(&d, 100.0) == POUT_DECK_STOP, "one big step");

    /* wind speed from the timing struct */
    pout_deck_timing_t tm;
    pout_deck_timing_defaults(&tm);
    tm.wind_speed = 5.0;            /* 125 frames/s */
    pout_tape_ops_t ops;
    fake_ops(&ops);
    pout_deck_init(&d, &ops, &tm, 0);
    pout_deck_ff(&d, 0);
    p = pout_deck_position(&d, 1.5);
    CHECK(p >= 124 && p <= 125, "5x: 125 frames in 1 s of winding");
}

static void test_end_of_tape_in_play(void)
{
    pout_deck_t d;
    fresh(&d, 0);
    int64_t f = -1;
    pout_deck_search_frame(&d, 0, 1497);
    CHECK(pout_deck_state(&d, 20.0) == POUT_DECK_PAUSE, "search finished");
    pout_deck_play(&d, 20.0);
    double t = 21.0;
    CHECK(pout_deck_next_output(&d, t, &f) == POUT_OUT_FRAME && f == 1497, "1497");
    CHECK(pout_deck_next_output(&d, t + FP, &f) == POUT_OUT_FRAME && f == 1498, "1498");
    CHECK(pout_deck_next_output(&d, t + 2 * FP, &f) == POUT_OUT_FRAME && f == 1499, "1499, the last frame");
    CHECK(pout_deck_state(&d, t + 2 * FP) == POUT_DECK_PLAY, "still playing until the end is reached");
    CHECK(pout_deck_next_output(&d, t + 3 * FP, &f) == POUT_OUT_BLANK, "then nothing");
    CHECK(pout_deck_state(&d, t + 3 * FP) == POUT_DECK_STOP, "end of tape in play: STOP");
    CHECK(pout_deck_position(&d, t + 3 * FP) == 1499, "position stays on the last frame");
}

static void test_search(void)
{
    pout_deck_t d;
    fresh(&d, 0);
    int64_t f = -1;
    pin_tc_t tc;

    /* frame search forward: 0 -> 1000: starts at 0.5 s, 1000/750 s of winding, 1.5 s settle */
    pout_deck_search_frame(&d, 20.0, 1000);
    CHECK(pout_deck_state(&d, 20.0) == POUT_DECK_FF, "forward search reports FF");
    CHECK(pout_deck_searching(&d, 20.0) && pout_deck_in_transition(&d, 20.0), "searching, in transition");
    CHECK(pout_deck_state(&d, 21.0) == POUT_DECK_FF, "winding");
    CHECK(pout_deck_position(&d, 21.0) > 300 && pout_deck_position(&d, 21.0) < 400, "position moves at wind speed");
    CHECK(pout_deck_state(&d, 22.5) == POUT_DECK_FF && pout_deck_in_transition(&d, 22.5), "settling still reports FF");
    CHECK(pout_deck_position(&d, 22.5) == 1000, "arrived at the target");
    CHECK(pout_deck_next_output(&d, 22.5, &f) == POUT_OUT_BLANK, "no picture while settling");
    CHECK(pout_deck_state(&d, 23.4) == POUT_DECK_PAUSE, "ends paused");
    CHECK(!pout_deck_searching(&d, 23.4) && !pout_deck_in_transition(&d, 23.4), "search over");
    CHECK(pout_deck_next_output(&d, 23.4, &f) == POUT_OUT_REPEAT && f == 1000, "paused picture is the target");

    /* backwards: REW while searching back */
    pout_deck_search_frame(&d, 30.0, 100);
    CHECK(pout_deck_state(&d, 30.0) == POUT_DECK_REW, "backward search reports REW");
    CHECK(pout_deck_state(&d, 34.0) == POUT_DECK_PAUSE && pout_deck_position(&d, 34.0) == 100, "arrived paused at 100");

    /* by timecode, across the gap (frame 100 in recording A, the target in recording B) */
    pin_tc_t want = { 1, 0, 5, 0, ':' };
    CHECK(pout_deck_search_tc(&d, 40.0, &want) == 0, "search 01:00:05:00");
    CHECK(pout_deck_state(&d, 40.5) == POUT_DECK_FF, "forward");
    CHECK(pout_deck_state(&d, 44.0) == POUT_DECK_PAUSE, "done");
    CHECK(pout_deck_position(&d, 44.0) == 750 + 125, "frame of 01:00:05:00");
    CHECK(pout_deck_timecode(&d, 44.0, &tc) == 0 && pin_tc_compare(&tc, &want) == 0, "timecode matches");

    /* a timecode in the gap or beyond the tape is refused and nothing moves */
    pin_tc_t gap = { 0, 30, 0, 0, ':' };
    CHECK(pout_deck_search_tc(&d, 50.0, &gap) == -1, "gap timecode refused");
    pin_tc_t beyond = { 2, 0, 0, 0, ':' };
    CHECK(pout_deck_search_tc(&d, 50.0, &beyond) == -1, "timecode past the tape refused");
    CHECK(pout_deck_state(&d, 50.1) == POUT_DECK_PAUSE && pout_deck_position(&d, 50.1) == 875, "deck unchanged");

    /* frames outside the tape are clamped */
    pout_deck_search_frame(&d, 60.0, 99999);
    CHECK(pout_deck_state(&d, 70.0) == POUT_DECK_PAUSE && pout_deck_position(&d, 70.0) == 1499, "clamped to the last frame");
    pout_deck_search_frame(&d, 80.0, -5);
    CHECK(pout_deck_state(&d, 90.0) == POUT_DECK_PAUSE && pout_deck_position(&d, 90.0) == 0, "clamped to 0");

    /* a new command during a search wins: STOP leaves the deck stopped where it was */
    pout_deck_search_frame(&d, 100.0, 1400);
    pout_deck_stop(&d, 101.0);
    CHECK(pout_deck_state(&d, 101.0) == POUT_DECK_STOP, "stop during a search");
    CHECK(pout_deck_state(&d, 110.0) == POUT_DECK_STOP, "and it stays stopped");
    CHECK(pout_deck_position(&d, 110.0) < 1400, "never arrived");

    /* settle time from the timing struct: 0 = paused at once on arrival */
    pout_deck_timing_t tm;
    pout_deck_timing_defaults(&tm);
    tm.search_settle = 0;
    pout_tape_ops_t ops;
    fake_ops(&ops);
    pout_deck_init(&d, &ops, &tm, 0);
    pout_deck_search_frame(&d, 0, 750);
    CHECK(pout_deck_state(&d, 1.4) == POUT_DECK_FF && pout_deck_state(&d, 1.6) == POUT_DECK_PAUSE, "no settle");
}

int main(void)
{
    test_play_pause();
    test_commands_cancel();
    test_wind();
    test_end_of_tape_in_play();
    test_search();
    if (g_failures) {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
