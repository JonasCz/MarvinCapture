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

/* MarvinPlayout's command line, and the sequencer run against the real
 * virtual deck on a fake clock (the session's loop, minus the bus). */

#include "pout_script.h"
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

/* ---- parser ---- */

#define ARGS(...) ((const char *const[]){ __VA_ARGS__ })
#define NARGS(...) ((int)(sizeof((const char *const[]){ __VA_ARGS__ }) / sizeof(char *)))
#define PARSE(sc, err, ...) pout_script_parse(NARGS(__VA_ARGS__), ARGS(__VA_ARGS__), &(sc), err, sizeof(err))

static void expect_bad(const char *what, const char *fragment, pin_status_t rc, const char *err)
{
    if (rc != PIN_ERR_ARG || !strstr(err, fragment)) {
        printf("FAIL %s: expected a usage error mentioning \"%s\", got rc=%d \"%s\"\n", what, fragment, (int)rc, err);
        g_failures++;
    }
}

static void test_parse_good(void)
{
    pout_script_t *sc = NULL;
    char err[200] = "";
    CHECK(PARSE(sc, err, "a.dv") == PIN_OK && sc, "just a file");
    CHECK(sc->nfiles == 1 && strcmp(sc->files[0], "a.dv") == 0 && sc->nactions == 0, "one file, no actions");
    CHECK(sc->channel == 63 && sc->speed == 100 && sc->virtual_deck == 1 && !sc->stay && !sc->has_dry_run &&
          !sc->has_device && !sc->debug, "defaults");
    CHECK(sc->timing.wind_speed == 30.0 && sc->timing.play_delay == 1.0 && sc->timing.search_settle == 1.5, "default timing");
    pout_script_destroy(sc);

    CHECK(PARSE(sc, err, "a.dv", "-f", "b.m2t", "--file=c.ts", "-d", "usb:2-1", "--debug", "--channel", "5", "--speed", "s400",
                "--no-virtual-deck", "--wind-speed", "60", "--start-delay=0.25", "--search-settle", "2", "--stay",
                "--rew", "--wait", "--play", "--wait", "timecode=00:00:10:00", "--stop", "--goto", "00:01:00:00",
                "--wait-all", "idle,wallclock=00:00:10", "--pause", "--ff", "--wait-any=wallclock=00:00:01") == PIN_OK,
          "everything");
    if (sc) {
        CHECK(sc->nfiles == 3 && !strcmp(sc->files[1], "b.m2t") && !strcmp(sc->files[2], "c.ts"), "files in order");
        CHECK(sc->has_device && !strcmp(sc->device, "usb:2-1") && sc->debug && sc->channel == 5 && sc->speed == 400 &&
              !sc->virtual_deck && sc->stay, "settings");
        CHECK(sc->timing.wind_speed == 60.0 && sc->timing.play_delay == 0.25 && sc->timing.search_settle == 2.0, "timing settings");
        CHECK(sc->nactions == 10, "ten actions");
        if (sc->nactions == 10) {
            pout_action_t *a = sc->actions;
            CHECK(a[0].kind == POUT_ACT_REW && a[1].kind == POUT_ACT_WAIT && a[2].kind == POUT_ACT_PLAY &&
                  a[3].kind == POUT_ACT_WAIT && a[4].kind == POUT_ACT_STOP && a[5].kind == POUT_ACT_GOTO &&
                  a[6].kind == POUT_ACT_WAIT && a[7].kind == POUT_ACT_PAUSE && a[8].kind == POUT_ACT_FF &&
                  a[9].kind == POUT_ACT_WAIT, "action kinds");
            CHECK(!a[1].wait_all && a[1].nconds == 1 && a[1].conds[0].kind == PIN_COND_IDLE, "bare --wait is idle");
            CHECK(a[3].nconds == 1 && a[3].conds[0].kind == PIN_COND_TIMECODE && a[3].conds[0].tc.s == 10, "timecode wait");
            CHECK(a[5].tc.m == 1 && a[5].tc.s == 0, "goto 00:01:00:00");
            CHECK(a[6].wait_all && a[6].nconds == 2 && a[6].conds[1].kind == PIN_COND_WALLCLOCK, "wait-all list");
            CHECK(!a[9].wait_all && a[9].nconds == 1, "--wait-any=COND");
            char t[100];
            pout_script_action_text(sc, 6, t, sizeof(t));
            CHECK(!strcmp(t, "wait-all idle=00:00:05,wallclock=00:00:10"), "action text");
            pout_script_action_text(sc, 5, t, sizeof(t));
            CHECK(!strcmp(t, "goto 00:01:00:00"), "goto text");
        }
        pout_script_destroy(sc);
    }

    CHECK(PARSE(sc, err, "a.dv", "--dry-run", "out.dv", "--realtime") == PIN_OK && sc && sc->has_dry_run &&
          !strcmp(sc->dry_run, "out.dv") && sc->realtime, "dry run, realtime");
    pout_script_destroy(sc);

    CHECK(PARSE(sc, err, "--help") == PIN_OK && sc && sc->help, "--help");
    pout_script_destroy(sc);
    CHECK(PARSE(sc, err, "a.dv", "--play", "-h") == PIN_OK && sc && sc->help, "-h anywhere");
    pout_script_destroy(sc);
    CHECK(pout_script_parse(0, NULL, &sc, err, sizeof(err)) == PIN_OK && sc && sc->help, "no arguments is help");
    pout_script_destroy(sc);
    CHECK(strstr(pout_script_help_text(), "--goto") && strstr(pout_script_help_text(), "--dry-run"), "help text");
}

static void test_parse_bad(void)
{
    pout_script_t *sc = NULL;
    char err[200];
#define BAD(frag, ...) do { err[0] = 0; pin_status_t rc_ = PARSE(sc, err, __VA_ARGS__); \
        expect_bad(#__VA_ARGS__, frag, rc_, err); if (rc_ == PIN_OK) pout_script_destroy(sc); } while (0)
    BAD("no tape file", "--play");
    BAD("--bogus", "a.dv", "--bogus");
    BAD("-x", "a.dv", "-x");
    BAD("--channel", "a.dv", "--channel", "64");
    BAD("--channel", "a.dv", "--channel", "-1");
    BAD("--channel", "a.dv", "--channel", "x");
    BAD("--channel needs", "a.dv", "--channel");
    BAD("--speed", "a.dv", "--speed", "s800");
    BAD("--wind-speed", "a.dv", "--wind-speed", "0");
    BAD("--wind-speed", "a.dv", "--wind-speed", "fast");
    BAD("--start-delay", "a.dv", "--start-delay", "-1");
    BAD("--search-settle", "a.dv", "--search-settle", "99");
    BAD("--stay", "a.dv", "--play", "--stay");
    BAD("--channel", "a.dv", "--play", "--channel", "3");
    BAD("before the first action", "a.dv", "--play", "b.dv");
    BAD("--play takes no value", "a.dv", "--play=1");
    BAD("--goto", "a.dv", "--goto");
    BAD("--goto 1:00", "a.dv", "--goto", "1:00");
    BAD("frames must be 0..29", "a.dv", "--goto", "00:00:00:30");
    BAD("--wait-any needs", "a.dv", "--wait-any");
    BAD("--wait-all needs", "a.dv", "--wait-all", "--play");
    BAD("signal", "a.dv", "--wait", "signal");
    BAD("nosignal", "a.dv", "--wait-any", "nosignal=00:00:10");
    BAD("captured", "a.dv", "--wait", "captured=00:00:10");
    BAD("bad timecode", "a.dv", "--wait", "timecode=00:99:00:00");
    BAD("timecode needs a value", "a.dv", "--wait", "timecode");
    BAD("--realtime", "a.dv", "--realtime");
    BAD("--dry-run", "a.dv", "-d", "usb:1", "--dry-run", "o.dv");
    BAD("--dry-run needs", "a.dv", "--dry-run");
    BAD("--debug takes no value", "a.dv", "--debug=1");
    BAD("--capture", "a.dv", "--capture", "x.avi");
#undef BAD
}

/* ---- the sequencer against the real deck ---- */

typedef struct {
    pout_deck_t deck;
    double now;
    int ncmd;
    struct { pout_act_kind_t kind; double t; int64_t pos; pout_deck_mode_t state; } cmd[16];
    pout_seq_status_t end;
    pout_seq_out_t last;
} run_t;

static void observe(run_t *r, pout_obs_t *o)
{
    o->now = r->now;
    o->state = pout_deck_state(&r->deck, r->now);
    o->picture = pout_deck_picture(&r->deck, r->now);
    o->tc_valid = pout_deck_timecode(&r->deck, r->now, &o->tc) == 0;
    o->fps = 25.0;
}

/* The session's loop: the output loop every 40 ms, the sequencer every 120 ms. */
static void run(run_t *r, const pout_script_t *sc, double limit_s)
{
    pout_tape_ops_t ops;
    fake_ops(&ops);
    memset(r, 0, sizeof(*r));
    pout_deck_init(&r->deck, &ops, NULL, 0);
    pout_seq_t q;
    pout_seq_init(&q, sc);
    r->end = POUT_SEQ_WAITING;
    for (int tick = 0; r->now < limit_s; tick++) {
        r->now = tick * 0.04;
        int64_t f;
        pout_deck_next_output(&r->deck, r->now, &f);
        if (tick % 3)
            continue;
        for (;;) {
            pout_obs_t o;
            observe(r, &o);
            pout_seq_status_t st = pout_seq_step(&q, &o, &r->last);
            if (st == POUT_SEQ_COMMAND) {
                pout_act_kind_t k = r->last.cmd;
                int rc = 0;
                if (k == POUT_ACT_PLAY) pout_deck_play(&r->deck, r->now);
                else if (k == POUT_ACT_PAUSE) pout_deck_pause(&r->deck, r->now);
                else if (k == POUT_ACT_STOP) pout_deck_stop(&r->deck, r->now);
                else if (k == POUT_ACT_FF) pout_deck_ff(&r->deck, r->now);
                else if (k == POUT_ACT_REW) pout_deck_rew(&r->deck, r->now);
                else rc = pout_deck_search_tc(&r->deck, r->now, &r->last.tc);
                if (r->ncmd < 16) {
                    r->cmd[r->ncmd].kind = k;
                    r->cmd[r->ncmd].t = r->now;
                    r->cmd[r->ncmd].pos = pout_deck_position(&r->deck, r->now);
                    r->cmd[r->ncmd++].state = pout_deck_state(&r->deck, r->now);
                }
                if (rc != 0) {
                    pout_seq_command_failed(&q, "timecode not on the tape", &r->last);
                    r->end = POUT_SEQ_FAILED;
                    return;
                }
                continue;
            }
            r->end = st;
            if (st == POUT_SEQ_DONE || st == POUT_SEQ_FAILED)
                return;
            break;
        }
    }
}

static pout_script_t *must_parse(int n, const char *const *argv)
{
    pout_script_t *sc = NULL;
    char err[200] = "";
    if (pout_script_parse(n, argv, &sc, err, sizeof(err)) != PIN_OK) {
        printf("FAIL parse: %s\n", err);
        g_failures++;
        return NULL;
    }
    return sc;
}

static void test_sequence_rew_play_stop(void)
{
    /* the example of the plan: park in the middle first, then rew, play to 10 s, stop */
    const char *argv[] = { "t.dv", "--goto", "00:00:20:00", "--wait", "--rew", "--wait", "--play", "--wait",
                           "timecode=00:00:10:00", "--stop" };
    pout_script_t *sc = must_parse(10, argv);
    if (!sc)
        return;
    run_t r;
    run(&r, sc, 300);
    CHECK(r.end == POUT_SEQ_DONE && r.last.exit_code == POUT_EXIT_OK, "script finished");
    CHECK(r.ncmd == 4, "four commands");
    if (r.ncmd == 4) {
        CHECK(r.cmd[0].kind == POUT_ACT_GOTO && r.cmd[1].kind == POUT_ACT_REW && r.cmd[2].kind == POUT_ACT_PLAY &&
              r.cmd[3].kind == POUT_ACT_STOP, "order: goto rew play stop");
        /* goto: the wait ends when the search is over and the deck has been paused 3 s */
        CHECK(r.cmd[1].t - r.cmd[0].t >= 0.5 + 500.0 / 750.0 + 1.5 + 3.0, "the wait after goto waited for the search and 3 s");
        CHECK(r.cmd[1].state == POUT_DECK_REW, "rew reported at once");
        CHECK(r.cmd[2].t - r.cmd[1].t >= 0.5 + 500.0 / 750.0 + 3.0, "the wait after rew waited for the wind and 3 s");
        CHECK(r.cmd[2].pos == 0, "play from the start of the tape");
        /* play: 1 s before the picture, then 10 s of frames; polling is every 120 ms */
        double played = r.cmd[3].t - r.cmd[2].t;
        CHECK(played >= 11.0 && played <= 11.3, "stop came when the deck timecode reached 00:00:10:00");
        CHECK(r.cmd[3].pos >= 250 && r.cmd[3].pos <= 252, "at frame 250");
        CHECK(pout_deck_state(&r.deck, r.now) == POUT_DECK_STOP, "deck stopped");
    }
    pout_script_destroy(sc);
}

static void test_sequence_plan_example(void)
{
    const char *argv[] = { "t.dv", "--rew", "--wait", "--play", "--wait", "timecode=00:00:10:00", "--stop" };
    pout_script_t *sc = must_parse(7, argv);
    if (!sc)
        return;
    run_t r;
    run(&r, sc, 300);
    CHECK(r.end == POUT_SEQ_DONE && r.ncmd == 3, "rew at the start of the tape: still finishes");
    if (r.ncmd == 3) {
        CHECK(r.cmd[0].kind == POUT_ACT_REW && r.cmd[1].kind == POUT_ACT_PLAY && r.cmd[2].kind == POUT_ACT_STOP, "order");
        CHECK(r.cmd[2].pos >= 250 && r.cmd[2].pos <= 252, "stopped at 00:00:10:00");
    }
    pout_script_destroy(sc);
}

static void test_sequence_gap(void)
{
    const char *argv[] = { "t.dv", "--goto", "01:00:05:00", "--wait", "--play", "--wait", "timecode=01:00:08:00", "--stop" };
    pout_script_t *sc = must_parse(8, argv);
    if (!sc)
        return;
    run_t r;
    run(&r, sc, 300);
    CHECK(r.end == POUT_SEQ_DONE && r.ncmd == 3, "goto into the second recording, play, stop");
    if (r.ncmd == 3) {
        CHECK(r.cmd[1].pos == 750 + 125, "play starts at 01:00:05:00");
        CHECK(r.cmd[2].pos >= 750 + 200 && r.cmd[2].pos <= 750 + 202, "stop at 01:00:08:00");
    }
    pout_script_destroy(sc);
}

static void test_sequence_failures(void)
{
    run_t r;
    pout_script_t *sc;

    /* a timecode that is not on the tape: the deck refuses, the script fails with exit 3 */
    const char *a1[] = { "t.dv", "--goto", "00:30:00:00", "--wait" };
    sc = must_parse(4, a1);
    if (sc) {
        run(&r, sc, 100);
        CHECK(r.end == POUT_SEQ_FAILED && r.last.exit_code == POUT_EXIT_DECK && strstr(r.last.why, "not on the tape"),
              "goto into the gap fails");
        pout_script_destroy(sc);
    }

    /* FF to the end, then wait for a timecode that never comes: fails when the deck goes idle */
    const char *a2[] = { "t.dv", "--ff", "--wait", "timecode=02:00:00:00" };
    sc = must_parse(4, a2);
    if (sc) {
        run(&r, sc, 100);
        CHECK(r.end == POUT_SEQ_FAILED && r.last.exit_code == POUT_EXIT_DECK, "timecode wait fails when the deck stops");
        CHECK(strstr(r.last.why, "stopped") != NULL, "and says why");
        pout_script_destroy(sc);
    }

    /* a wallclock wait runs for its time; pause follows */
    const char *a3[] = { "t.dv", "--play", "--wait", "wallclock=00:00:05", "--pause" };
    sc = must_parse(5, a3);
    if (sc) {
        run(&r, sc, 100);
        CHECK(r.end == POUT_SEQ_DONE && r.ncmd == 2, "play, wait, pause");
        if (r.ncmd == 2) {
            double w = r.cmd[1].t - r.cmd[0].t;
            CHECK(w >= 5.0 && w < 5.4, "five seconds of real time");
            CHECK(r.cmd[1].state == POUT_DECK_PAUSE, "paused");
        }
        pout_script_destroy(sc);
    }

    /* no actions: done at once (the session decides what to serve) */
    const char *a4[] = { "t.dv" };
    sc = must_parse(1, a4);
    if (sc) {
        run(&r, sc, 10);
        CHECK(r.end == POUT_SEQ_DONE && r.ncmd == 0, "no actions: DONE");
        pout_script_destroy(sc);
    }

    /* wait-any: a met wallclock beats the timecode that would fail; calls after the end repeat the end */
    const char *a5[] = { "t.dv", "--play", "--wait-any", "timecode=02:00:00:00,wallclock=00:00:02", "--stop" };
    sc = must_parse(5, a5);
    if (sc) {
        run(&r, sc, 100);
        CHECK(r.end == POUT_SEQ_DONE && r.ncmd == 2, "wait-any met by wallclock");
        pout_script_destroy(sc);
    }
}

int main(void)
{
    test_parse_good();
    test_parse_bad();
    test_sequence_rew_play_stop();
    test_sequence_plan_example();
    test_sequence_gap();
    test_sequence_failures();
    if (g_failures) {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
