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
 * ctest: the command-line sequencer (pin_script_parse / pin_script_run /
 * pin_script_cancel, docs/cli.md) against the replay device: a capture closed
 * by a duration wait, STEP and DONE events, an existing target without
 * --overwrite, "--capture -" not being supported yet, an analog script on the
 * replay device failing with exit code 2, and Ctrl-C (cancel) finalising an
 * open capture with exit code 130.
 *
 * Usage: test_pin_script_run <trace_file>
 */

#include "replay_test_util.h"

typedef struct {
    int done;
    int code;
    char text[PIN_PATH_MAX];
    int steps[16];
    int nsteps;
    char step_text[16][PIN_PATH_MAX];
} outcome_t;

static pin_script_t *parse(const char *const *argv, int argc)
{
    pin_script_t *sc = NULL;
    char err[256] = "";
    pin_status_t st = pin_script_parse(argc, argv, &sc, err, sizeof(err));
    if (st != PIN_OK) {
        fprintf(stderr, "parse failed: %s\n", err);
        exit(1);
    }
    return sc;
}
#define PARSE(...) parse((const char *const[]){ __VA_ARGS__ }, (int)(sizeof((const char *const[]){ __VA_ARGS__ }) / sizeof(char *)))

/* Drains events until DONE (or timeout_ms). */
static void wait_done(pin_session_t *s, outcome_t *o, int timeout_ms)
{
    memset(o, 0, sizeof(*o));
    for (int waited = 0; waited < timeout_ms && !o->done; waited += 20) {
        pin_event_t ev;
        ev.size = sizeof(ev);
        while (pin_poll_event(s, &ev)) {
            if (ev.kind == PIN_EVT_STEP && o->nsteps < 16) {
                o->steps[o->nsteps] = ev.a;
                snprintf(o->step_text[o->nsteps], sizeof(o->step_text[0]), "%s", ev.text);
                o->nsteps++;
            } else if (ev.kind == PIN_EVT_DONE) {
                o->done = 1;
                o->code = ev.a;
                snprintf(o->text, sizeof(o->text), "%s", ev.text);
            }
            ev.size = sizeof(ev);
        }
        if (!o->done)
            pin_test_sleep_ms(20);
    }
}

static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const char *trace = argv[1];
    if (!pin_test_file_exists(trace)) {
        printf("SKIP: %s not present\n", trace);
        return 0;
    }
    remove("script_a.dv");
    remove("script_b.dv");
    remove("script_c.dv");

    /* a fresh session, never brought up: the runner brings up the DV input itself */
    pin_session_t *s = pin_test_open_replay(trace);
    outcome_t o;

    /* 1. capture, closed by the duration wait and by the end of the script */
    pin_script_t *sc = PARSE("--capture", "script_a.dv", "--wait", "+00:00:02:00");
    CHECK(pin_script_needs_session(sc));
    CHECK_EQ_I(pin_script_step_count(sc), 2);
    CHECK(pin_script_run(s, sc) == PIN_OK);
    CHECK(pin_script_run(s, sc) == PIN_ERR_STATE); /* one at a time */
    pin_script_free(sc);                            /* the running script has its own copy */
    wait_done(s, &o, 30000);
    CHECK(o.done);
    if (o.code != 0)
        fprintf(stderr, "DONE %d: %s\n", o.code, o.text);
    CHECK_EQ_I(o.code, 0);
    CHECK_EQ_I(o.nsteps, 2);
    CHECK_EQ_I(o.steps[0], 0);
    CHECK_EQ_I(o.steps[1], 1);
    CHECK(strcmp(o.step_text[0], "capture script_a.dv") == 0);
    CHECK(strcmp(o.step_text[1], "wait +00:00:02:00") == 0);
    CHECK(file_size("script_a.dv") >= 0);
    pin_status_snapshot_t snap = { .size = sizeof(snap) };
    pin_get_status(s, &snap);
    CHECK_EQ_I(snap.state, PIN_STATE_READY); /* the capture was finalised */
    CHECK(snap.stop_reason == PIN_STOP_USER || snap.stop_reason == PIN_STOP_END_OF_TAPE);
    printf("OK: --capture script_a.dv --wait +00:00:02:00 (%ld bytes)\n", file_size("script_a.dv"));

    /* 2. the same name again: an error without --overwrite, fine with it */
    sc = PARSE("--capture", "script_a.dv", "--wait", "+00:00:01:00");
    CHECK(pin_script_run(s, sc) == PIN_OK);
    pin_script_free(sc);
    wait_done(s, &o, 30000);
    CHECK(o.done);
    CHECK_EQ_I(o.code, 1);
    CHECK(strstr(o.text, "script_a.dv") && strstr(o.text, "--overwrite"));
    printf("OK: existing file without --overwrite: exit 1 (%s)\n", o.text);

    /* ... and found before the first step runs, so an earlier step (a tape command on
     * a real deck) is not started for nothing */
    remove("script_pre.dv");
    sc = PARSE("--capture", "script_pre.dv", "--wait", "+00:00:01:00", "--capture", "script_a.dv");
    CHECK(pin_script_run(s, sc) == PIN_OK);
    pin_script_free(sc);
    wait_done(s, &o, 30000);
    CHECK(o.done);
    CHECK_EQ_I(o.code, 1);
    CHECK_EQ_I(o.nsteps, 0);
    CHECK(file_size("script_pre.dv") < 0);
    printf("OK: existing file is found before any step runs\n");

    sc = PARSE("--overwrite", "--capture", "script_a.dv", "--wait", "+00:00:01:00");
    CHECK(pin_script_run(s, sc) == PIN_OK);
    pin_script_free(sc);
    wait_done(s, &o, 30000);
    CHECK(o.done && o.code == 0);
    printf("OK: --overwrite\n");

    /* 3. two captures in a row: the next --capture closes the previous one */
    sc = PARSE("--capture", "script_b.dv", "--wait", "+00:00:01:00", "--capture", "script_c.dv", "--wait",
               "+00:00:01:00");
    CHECK(pin_script_run(s, sc) == PIN_OK);
    pin_script_free(sc);
    wait_done(s, &o, 30000);
    CHECK(o.done);
    CHECK_EQ_I(o.code, 0);
    CHECK(file_size("script_b.dv") >= 0 && file_size("script_c.dv") >= 0);
    printf("OK: two captures in one script\n");

    /* 4. no-op wait (no deck movement asked for) and signal: the replay has a signal */
    sc = PARSE("--wait", "signal");
    CHECK(pin_script_run(s, sc) == PIN_OK);
    pin_script_free(sc);
    wait_done(s, &o, 30000);
    CHECK(o.done && o.code == 0);
    printf("OK: --wait signal\n");

    /* 5. stdout streaming is not supported yet: fails before doing anything */
    sc = PARSE("--capture", "-", "--wait", "+00:00:01:00");
    CHECK(pin_script_run(s, sc) == PIN_OK);
    pin_script_free(sc);
    wait_done(s, &o, 10000);
    CHECK(o.done);
    CHECK_EQ_I(o.code, 1);
    CHECK(strstr(o.text, "not supported yet"));
    CHECK_EQ_I(o.nsteps, 0);
    printf("OK: --capture - : %s\n", o.text);

    /* 6. Ctrl-C while a script waits (the short fixture ends the replay capture by itself,
     * so what is checked is: cancel is honoured, DONE 130, nothing left open) */
    remove("script_a.dv");
    sc = PARSE("--capture", "script_a.dv", "--wait", "+01:00:00:00");
    CHECK(pin_script_run(s, sc) == PIN_OK);
    pin_script_free(sc);
    pin_test_sleep_ms(500);
    CHECK(pin_script_cancel(s) == PIN_OK);
    wait_done(s, &o, 30000);
    CHECK(o.done);
    CHECK_EQ_I(o.code, 130);
    pin_get_status(s, &snap);
    CHECK_EQ_I(snap.state, PIN_STATE_READY);
    CHECK(file_size("script_a.dv") >= 0);
    CHECK(pin_script_cancel(s) == PIN_ERR_STATE); /* nothing running any more */
    printf("OK: cancel: exit 130\n");

    /* 7. an analog script on the replay device (DV only) fails with a device error */
    sc = PARSE("-i", "svideo", "--wait", "+00:00:01:00");
    CHECK(pin_script_run(s, sc) == PIN_OK);
    pin_script_free(sc);
    wait_done(s, &o, 30000);
    CHECK(o.done);
    CHECK_EQ_I(o.code, 2);
    CHECK(o.text[0]);
    printf("OK: analog input on the replay device: exit 2 (%s)\n", o.text);

    pin_close(s);
    remove("script_a.dv");
    remove("script_b.dv");
    remove("script_c.dv");
    return 0;
}
