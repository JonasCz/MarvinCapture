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

/* Wait-condition evaluation with a made-up clock and made-up observations. */

#include "pin_script_eval.h"
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

static pin_script_cond_t cond(const char *text)
{
    pin_script_cond_t c;
    char why[64];
    if (!pin_script_cond_parse(text, &c, why, sizeof(why))) {
        printf("FAIL bad test condition %s: %s\n", text, why);
        g_failures++;
        memset(&c, 0, sizeof(c));
    }
    return c;
}

static pin_script_obs_t obs(double now, pin_deck_state_t deck, int signal, const char *tc)
{
    pin_script_obs_t o;
    memset(&o, 0, sizeof(o));
    o.now = now;
    o.deck = deck;
    o.camera_present = 1;
    o.signal = signal;
    o.fps = 25;
    if (tc)
        snprintf(o.timecode, sizeof(o.timecode), "%s", tc);
    return o;
}

static pin_eval_result_t ev1(const char *c, const pin_script_obs_t *o, pin_script_track_t *t, char *why, size_t cap)
{
    pin_script_cond_t cc = cond(c);
    int idx;
    return pin_script_eval(&cc, 1, o, t, &idx, why, cap);
}

static void test_idle_with_motion(void)
{
    pin_script_track_t t;
    pin_script_track_init(&t);
    char why[128];
    pin_script_track_transport(&t, PIN_DECK_CMD_REW, 100.0);
    pin_script_obs_t o = obs(100.5, PIN_DECK_UNKNOWN, 0, "");
    pin_script_track_wait_begin(&t, &o);
    CHECK(ev1("idle", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "unknown state: pending");
    o = obs(102, PIN_DECK_REWINDING, 0, "00:10:00:00");
    CHECK(ev1("idle", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "moving: pending");
    o = obs(160, PIN_DECK_STOPPED, 0, "00:00:00:00");
    CHECK(ev1("idle", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "just stopped: pending (not stable)");
    o = obs(162.9, PIN_DECK_STOPPED, 0, "00:00:00:00");
    CHECK(ev1("idle", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "stopped 2.9 s: pending");
    o = obs(163.1, PIN_DECK_STOPPED, 0, "00:00:00:00");
    CHECK(ev1("idle", &o, &t, why, sizeof(why)) == PIN_EVAL_MET, "stopped 3.1 s after motion: met");
    CHECK(!t.transport_valid, "an idle wait consumes the transport command");
    /* nothing started since: met at once */
    o = obs(164, PIN_DECK_STOPPED, 0, "00:00:00:00");
    CHECK(ev1("idle", &o, &t, why, sizeof(why)) == PIN_EVAL_MET, "idle again: met at once");
}

static void test_idle_motion_restarts_the_stable_clock(void)
{
    pin_script_track_t t;
    pin_script_track_init(&t);
    char why[128];
    pin_script_track_transport(&t, PIN_DECK_CMD_PLAY, 10.0);
    pin_script_obs_t o = obs(11, PIN_DECK_PLAYING, 1, "00:00:01:00");
    pin_script_track_wait_begin(&t, &o);
    CHECK(ev1("idle", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "playing");
    o = obs(20, PIN_DECK_PAUSED, 1, "00:00:10:00");
    CHECK(ev1("idle", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "paused: not yet stable");
    o = obs(22, PIN_DECK_PLAYING, 1, "00:00:10:00");
    CHECK(ev1("idle", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "moves again");
    o = obs(24, PIN_DECK_STOPPED, 0, "");
    CHECK(ev1("idle", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "stopped, the clock restarted at 24");
    o = obs(26.5, PIN_DECK_STOPPED, 0, "");
    CHECK(ev1("idle", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "2.5 s");
    o = obs(27.1, PIN_DECK_STOPPED, 0, "");
    CHECK(ev1("idle", &o, &t, why, sizeof(why)) == PIN_EVAL_MET, "3.1 s: met");
}

static void test_idle_early_stopped(void)
{
    /* REW at the start of the tape: the deck never moves. Early status can still say
     * stopped, so not before 5 s have passed since the command. */
    pin_script_track_t t;
    pin_script_track_init(&t);
    char why[128];
    pin_script_track_transport(&t, PIN_DECK_CMD_REW, 50.0);
    pin_script_obs_t o = obs(50.2, PIN_DECK_STOPPED, 0, "");
    pin_script_track_wait_begin(&t, &o);
    CHECK(ev1("idle", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "stale stopped right after the command");
    o = obs(53.5, PIN_DECK_STOPPED, 0, "");
    CHECK(ev1("idle", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "stable 3 s but only 3.5 s after the command");
    o = obs(55.1, PIN_DECK_STOPPED, 0, "");
    CHECK(ev1("idle", &o, &t, why, sizeof(why)) == PIN_EVAL_MET, "5 s and stable: met without motion");
}

static void test_idle_stopped_before_command_does_not_count(void)
{
    pin_script_track_t t;
    pin_script_track_init(&t);
    char why[128];
    pin_script_obs_t o = obs(0, PIN_DECK_STOPPED, 0, "");
    pin_script_track_observe(&t, &o);
    o = obs(20, PIN_DECK_STOPPED, 0, "");
    pin_script_track_observe(&t, &o); /* has stood still for 20 s */
    pin_script_track_transport(&t, PIN_DECK_CMD_PLAY, 20.0);
    o = obs(20.1, PIN_DECK_STOPPED, 0, "");
    pin_script_track_wait_begin(&t, &o);
    CHECK(ev1("idle", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "the stillness before the command is not counted");
}

static void test_idle_deck_errors(void)
{
    pin_script_track_t t;
    pin_script_track_init(&t);
    char why[128] = "";
    pin_script_obs_t o = obs(1, PIN_DECK_NO_TAPE, 0, "");
    pin_script_track_wait_begin(&t, &o);
    CHECK(ev1("idle", &o, &t, why, sizeof(why)) == PIN_EVAL_ERROR && strstr(why, "no tape"), "no tape: error");
    o = obs(2, PIN_DECK_UNKNOWN, 0, "");
    o.camera_present = 0;
    CHECK(ev1("idle", &o, &t, why, sizeof(why)) == PIN_EVAL_ERROR && strstr(why, "camera"), "no camera: error");
    /* signal/duration conditions do not need a deck */
    o = obs(3, PIN_DECK_NO_TAPE, 1, "");
    CHECK(ev1("signal", &o, &t, why, sizeof(why)) == PIN_EVAL_MET, "signal needs no deck");
    /* idle with no transport and the deck unknown keeps waiting */
    pin_script_track_init(&t);
    o = obs(4, PIN_DECK_UNKNOWN, 0, "");
    pin_script_track_wait_begin(&t, &o);
    CHECK(ev1("idle", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "unknown deck, no transport: pending");
}

static void test_timecode_up_and_down(void)
{
    pin_script_track_t t;
    pin_script_track_init(&t);
    char why[128];
    /* playing forward: reaches or passes */
    pin_script_track_transport(&t, PIN_DECK_CMD_PLAY, 0);
    pin_script_obs_t o = obs(5, PIN_DECK_PLAYING, 1, "00:14:29:24");
    pin_script_track_wait_begin(&t, &o);
    CHECK(ev1("00:14:30:00", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "just before");
    o = obs(6, PIN_DECK_PLAYING, 1, "00:14:30:00");
    CHECK(ev1("00:14:30:00", &o, &t, why, sizeof(why)) == PIN_EVAL_MET, "reached");
    o = obs(7, PIN_DECK_PLAYING, 1, "00:14:31;05");
    CHECK(ev1("00:14:30:00", &o, &t, why, sizeof(why)) == PIN_EVAL_MET, "passed (drop-frame timecode)");
    o = obs(8, PIN_DECK_PLAYING, 1, "");
    CHECK(ev1("00:14:30:00", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "unreadable timecode: pending");

    /* rewinding: going down */
    pin_script_track_transport(&t, PIN_DECK_CMD_REW, 10);
    o = obs(11, PIN_DECK_REWINDING, 0, "00:30:00:00");
    pin_script_track_wait_begin(&t, &o);
    CHECK(ev1("00:20:00:00", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "rewinding, still above");
    o = obs(20, PIN_DECK_REWINDING, 0, "00:19:59:10");
    CHECK(ev1("00:20:00:00", &o, &t, why, sizeof(why)) == PIN_EVAL_MET, "rewinding, passed downwards");
    /* and a time ahead of a rewinding deck is already "passed" */
    o = obs(21, PIN_DECK_REWINDING, 0, "00:19:00:00");
    CHECK(ev1("00:25:00:00", &o, &t, why, sizeof(why)) == PIN_EVAL_MET, "target above while going down: reached");
}

static void test_idle_before_timecode(void)
{
    pin_script_track_t t;
    pin_script_track_init(&t);
    char why[128] = "";
    pin_script_track_transport(&t, PIN_DECK_CMD_PLAY, 0);
    pin_script_obs_t o = obs(2, PIN_DECK_PLAYING, 1, "00:01:00:00");
    pin_script_track_wait_begin(&t, &o);
    CHECK(ev1("00:14:30:00", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "playing");
    o = obs(30, PIN_DECK_STOPPED, 0, "00:02:00:00");
    CHECK(ev1("00:14:30:00", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "stopped, not yet stable");
    o = obs(33.5, PIN_DECK_STOPPED, 0, "00:02:00:00");
    pin_eval_result_t r = ev1("00:14:30:00", &o, &t, why, sizeof(why));
    CHECK(r == PIN_EVAL_ERROR && strstr(why, "stopped at 00:02:00:00") && strstr(why, "00:14:30:00"),
          "the deck went idle before the timecode: error");
    /* a timecode that is reached wins over the idle check */
    o = obs(34, PIN_DECK_STOPPED, 0, "00:15:00:00");
    CHECK(ev1("00:14:30:00", &o, &t, why, sizeof(why)) == PIN_EVAL_MET, "reached while stopped: met");
}

static void test_nosignal(void)
{
    pin_script_track_t t;
    pin_script_track_init(&t);
    char why[64];
    /* signal present at the start of the wait */
    pin_script_obs_t o = obs(100, PIN_DECK_PLAYING, 1, "");
    pin_script_track_wait_begin(&t, &o);
    CHECK(ev1("nosignal=+00:00:30:00", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "signal: pending");
    o = obs(110, PIN_DECK_PLAYING, 0, "");
    CHECK(ev1("nosignal=+00:00:30:00", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "signal lost at 110");
    o = obs(139, PIN_DECK_PLAYING, 0, "");
    CHECK(ev1("nosignal=+00:00:30:00", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "29 s of no signal");
    o = obs(141, PIN_DECK_PLAYING, 0, "");
    CHECK(ev1("nosignal=+00:00:30:00", &o, &t, why, sizeof(why)) == PIN_EVAL_MET, "31 s of no signal");

    /* a blip of signal restarts the count */
    pin_script_track_init(&t);
    o = obs(0, PIN_DECK_PLAYING, 0, "");
    pin_script_track_wait_begin(&t, &o);
    o = obs(20, PIN_DECK_PLAYING, 0, "");
    CHECK(ev1("nosignal=+00:00:30:00", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "20 s");
    o = obs(25, PIN_DECK_PLAYING, 1, "");
    CHECK(ev1("nosignal=+00:00:30:00", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "signal back");
    o = obs(50, PIN_DECK_PLAYING, 0, "");
    CHECK(ev1("nosignal=+00:00:30:00", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "count restarted at 50");
    o = obs(81, PIN_DECK_PLAYING, 0, "");
    CHECK(ev1("nosignal=+00:00:30:00", &o, &t, why, sizeof(why)) == PIN_EVAL_MET, "31 s later");

    /* no signal long before the wait: counted from the start of the wait (a capture
     * in READY with no first frame yet counts as no signal too) */
    pin_script_track_init(&t);
    o = obs(0, PIN_DECK_STOPPED, 0, "");
    pin_script_track_observe(&t, &o);
    o = obs(600, PIN_DECK_STOPPED, 0, "");
    pin_script_track_wait_begin(&t, &o);
    CHECK(ev1("nosignal", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "wait just started: pending");
    o = obs(659, PIN_DECK_STOPPED, 0, "");
    CHECK(ev1("nosignal", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "default 1 min: 59 s");
    o = obs(661, PIN_DECK_STOPPED, 0, "");
    CHECK(ev1("nosignal", &o, &t, why, sizeof(why)) == PIN_EVAL_MET, "default 1 min: 61 s");
}

static void test_signal_and_duration(void)
{
    pin_script_track_t t;
    pin_script_track_init(&t);
    char why[64];
    pin_script_obs_t o = obs(10, PIN_DECK_STOPPED, 0, "");
    pin_script_track_wait_begin(&t, &o);
    CHECK(ev1("signal", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "no signal yet");
    o = obs(11, PIN_DECK_STOPPED, 1, "");
    CHECK(ev1("signal", &o, &t, why, sizeof(why)) == PIN_EVAL_MET, "signal");

    pin_script_track_init(&t);
    o = obs(100, PIN_DECK_STOPPED, 1, "");
    pin_script_track_wait_begin(&t, &o);
    CHECK(ev1("+00:00:02:00", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "0 s of 2 s");
    o = obs(101.9, PIN_DECK_STOPPED, 1, "");
    CHECK(ev1("+00:00:02:00", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "1.9 s");
    o = obs(102.0, PIN_DECK_STOPPED, 1, "");
    CHECK(ev1("+00:00:02:00", &o, &t, why, sizeof(why)) == PIN_EVAL_MET, "2.0 s");
    /* frames use the current frame rate */
    pin_script_track_init(&t);
    o = obs(0, PIN_DECK_STOPPED, 1, "");
    o.fps = 30000.0 / 1001.0;
    pin_script_track_wait_begin(&t, &o);
    o.now = 0.49;
    CHECK(ev1("+00:00:00:15", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "15 frames of NTSC = 0.5 s: 0.49");
    o.now = 0.51;
    CHECK(ev1("+00:00:00:15", &o, &t, why, sizeof(why)) == PIN_EVAL_MET, "0.51");
    pin_script_track_init(&t);
    o = obs(0, PIN_DECK_STOPPED, 1, "");
    o.fps = 0; /* unknown: 25 fps */
    pin_script_track_wait_begin(&t, &o);
    o.now = 0.59;
    CHECK(ev1("+00:00:00:15", &o, &t, why, sizeof(why)) == PIN_EVAL_PENDING, "15 frames at 25 fps = 0.6 s: 0.59");
    o.now = 0.61;
    CHECK(ev1("+00:00:00:15", &o, &t, why, sizeof(why)) == PIN_EVAL_MET, "0.61");
}

static void test_combined_first_wins(void)
{
    pin_script_track_t t;
    pin_script_track_init(&t);
    pin_script_cond_t cs[3] = { cond("idle"), cond("nosignal=+00:00:10:00"), cond("+00:00:20:00") };
    char why[64];
    int idx = -1;
    pin_script_track_transport(&t, PIN_DECK_CMD_PLAY, 0);
    pin_script_obs_t o = obs(1, PIN_DECK_PLAYING, 1, "");
    pin_script_track_wait_begin(&t, &o);
    CHECK(pin_script_eval(cs, 3, &o, &t, &idx, why, sizeof(why)) == PIN_EVAL_PENDING, "playing with signal");
    /* signal lost at 5; at 16 the nosignal (10 s) is met, the duration (20 s from 1) is not */
    o = obs(5, PIN_DECK_PLAYING, 0, "");
    pin_script_eval(cs, 3, &o, &t, &idx, why, sizeof(why));
    o = obs(16, PIN_DECK_PLAYING, 0, "");
    CHECK(pin_script_eval(cs, 3, &o, &t, &idx, why, sizeof(why)) == PIN_EVAL_MET && idx == 1, "nosignal first");
    /* both nosignal and duration met in the same poll: the first listed wins */
    o = obs(22, PIN_DECK_PLAYING, 0, "");
    CHECK(pin_script_eval(cs, 3, &o, &t, &idx, why, sizeof(why)) == PIN_EVAL_MET && idx == 1, "list order decides");
    pin_script_cond_t cs2[2] = { cond("+00:00:20:00"), cond("nosignal=+00:00:10:00") };
    CHECK(pin_script_eval(cs2, 2, &o, &t, &idx, why, sizeof(why)) == PIN_EVAL_MET && idx == 0, "reversed order");
    /* idle met together with a later duration */
    pin_script_track_init(&t);
    pin_script_track_transport(&t, PIN_DECK_CMD_PLAY, 0);
    o = obs(1, PIN_DECK_PLAYING, 1, "");
    pin_script_track_wait_begin(&t, &o);
    pin_script_eval(cs, 3, &o, &t, &idx, why, sizeof(why));
    o = obs(8, PIN_DECK_STOPPED, 1, "");
    pin_script_eval(cs, 3, &o, &t, &idx, why, sizeof(why));
    o = obs(12, PIN_DECK_STOPPED, 1, "");
    CHECK(pin_script_eval(cs, 3, &o, &t, &idx, why, sizeof(why)) == PIN_EVAL_MET && idx == 0, "idle wins");
}

int main(void)
{
    test_idle_with_motion();
    test_idle_motion_restarts_the_stable_clock();
    test_idle_early_stopped();
    test_idle_stopped_before_command_does_not_count();
    test_idle_deck_errors();
    test_timecode_up_and_down();
    test_idle_before_timecode();
    test_nosignal();
    test_signal_and_duration();
    test_combined_first_wins();
    if (g_failures) {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("OK: pin_script eval\n");
    return 0;
}
