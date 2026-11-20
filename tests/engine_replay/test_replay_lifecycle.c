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
 * ctest: the session's command handling and how a capture ends, on the replay device:
 *   - commands in a row are not lost: a stop right after a start ends that capture (one
 *     CAPTURE_ENDED), a second start while one runs is refused, a deck command while a capture
 *     runs or before an input is up is refused;
 *   - a file a cut cannot open (PIN_TEST_OPEN_FAIL inside a FAT32 size cut) ends the capture
 *     with WRITE_ERROR and the session in ERROR, CAPTURE_ENDED before ERROR, no hang;
 *   - a failed close with a long non-ASCII file name: the status' stop_text keeps the whole
 *     "could not be written completely" sentence as valid UTF-8, the event names the whole file;
 *   - a raw EP 0x88 dump that ends by itself: END_OF_TAPE, the file has the dump's frames;
 *   - a script cancelled (Ctrl-C) whose capture then fails to close exits 4, not 130;
 *   - the queue overflows as the last file is finished (the units held for a scene cut go out
 *     together, onto a slow drive): the capture ends with DISK_SLOW.
 *
 * Usage: test_replay_lifecycle <dv-pal.dv> <ep88-pal.bin>
 */

#include "replay_test_util.h"

typedef struct {
    int ended, errors, scenes;
    int ended_before_error;
    int ended_reason;
    char ended_text[PIN_PATH_MAX];
} events_t;

static void drain(pin_session_t *s, events_t *e)
{
    pin_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.size = sizeof(ev);
    while (pin_poll_event(s, &ev)) {
        if (ev.kind == PIN_EVT_CAPTURE_ENDED) {
            e->ended++;
            e->ended_reason = ev.a;
            snprintf(e->ended_text, sizeof(e->ended_text), "%s", ev.text);
        } else if (ev.kind == PIN_EVT_ERROR) {
            if (e->ended && !e->errors)
                e->ended_before_error = 1;
            e->errors++;
        } else if (ev.kind == PIN_EVT_SCENE) {
            e->scenes++;
        }
        ev.size = sizeof(ev);
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

static int valid_utf8(const char *t)
{
    for (const unsigned char *p = (const unsigned char *)t; *p;) {
        int len = *p < 0x80 ? 1 : (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : (*p & 0xF8) == 0xF0 ? 4 : 0;
        if (!len)
            return 0;
        for (int k = 1; k < len; k++)
            if ((p[k] & 0xC0) != 0x80)
                return 0;
        p += len;
    }
    return 1;
}

/* Waits until the capture has ended on its own (stop_reason set) and the session left it. */
static void wait_end(pin_session_t *s, pin_status_snapshot_t *snap, events_t *e)
{
    for (int waited = 0; waited < 15000; waited += 20) {
        drain(s, e);
        pin_get_status(s, snap);
        if (snap->stop_reason != PIN_STOP_NONE && snap->state != PIN_STATE_CAPTURING &&
            snap->state != PIN_STATE_STOPPING)
            break;
        pin_test_sleep_ms(20);
    }
    drain(s, e);
}

static void test_commands(const char *dv)
{
    pin_session_t *s = pin_test_open_replay(dv);
    /* no input up yet: nothing to send a deck command with */
    CHECK_EQ_I(pin_deck(s, PIN_DECK_CMD_PLAY), PIN_ERR_STATE);
    pin_test_prepare_dv(s);

    pin_capture_opts_t o;
    pin_capture_opts_defaults(&o);
    snprintf(o.path, sizeof(o.path), "lc_quick");
    o.format_dv = PIN_FMT_DV_RAW;
    events_t e = { 0 };
    pin_status_snapshot_t snap = { .size = sizeof(snap) };

    /* start, then at once a second start and a stop: the second start is refused, the stop
     * ends the first capture (not lost behind it, not dropping it) */
    CHECK_EQ_I(pin_capture_start(s, &o, 1), PIN_OK);
    CHECK_EQ_I(pin_capture_start(s, &o, 1), PIN_ERR_STATE);
    CHECK_EQ_I(pin_capture_stop(s), PIN_OK);
    pin_test_wait_state(s, PIN_STATE_READY, -1, 5000, &snap);
    for (int i = 0; i < 50 && !e.ended; i++) {
        drain(s, &e);
        pin_test_sleep_ms(20);
    }
    CHECK_EQ_I(e.ended, 1);
    CHECK_EQ_I(snap.state, PIN_STATE_READY);

    /* while a capture runs, a deck command other than Stop is refused */
    CHECK_EQ_I(pin_capture_start(s, &o, 1), PIN_OK);
    pin_test_wait_state(s, PIN_STATE_CAPTURING, PIN_STATE_ERROR, 3000, &snap);
    CHECK_EQ_I(snap.state, PIN_STATE_CAPTURING);
    CHECK_EQ_I(pin_deck(s, PIN_DECK_CMD_FF), PIN_ERR_STATE);
    CHECK_EQ_I(pin_capture_start(s, &o, 1), PIN_ERR_STATE);
    CHECK_EQ_I(pin_deck(s, PIN_DECK_CMD_STOP), PIN_OK);   /* ends the capture */
    pin_test_wait_state(s, PIN_STATE_READY, -1, 5000, &snap);
    CHECK_EQ_I(snap.state, PIN_STATE_READY);
    pin_close(s);
    remove("lc_quick.dv");
    printf("OK: commands in a row: none lost, a second start and a deck command refused\n");
}

/* A size cut's next file cannot be opened: the capture ends there, the session in ERROR. */
static void test_open_fail(const char *dv)
{
    remove("lc_openfail.dv");
    remove("lc_openfail-0002.dv");
    pin_test_setenv("PIN_TEST_FAT32_LIMIT", "2000000");
    pin_test_setenv("PIN_TEST_OPEN_FAIL", "2");
    pin_session_t *s = pin_test_open_replay(dv);
    pin_test_prepare_dv(s);
    pin_capture_opts_t o;
    pin_capture_opts_defaults(&o);
    snprintf(o.path, sizeof(o.path), "lc_openfail");
    o.format_dv = PIN_FMT_DV_RAW;
    CHECK_EQ_I(pin_capture_start(s, &o, 1), PIN_OK);
    events_t e = { 0 };
    pin_status_snapshot_t snap = { .size = sizeof(snap) };
    wait_end(s, &snap, &e);
    printf("open fail: state %d, stop %d \"%s\"\n", (int)snap.state, (int)snap.stop_reason, snap.stop_text);
    CHECK_EQ_I(snap.stop_reason, PIN_STOP_WRITE_ERROR);
    CHECK_EQ_I(snap.state, PIN_STATE_ERROR);
    CHECK_EQ_I(e.ended, 1);
    CHECK(e.ended_before_error);
    CHECK_EQ_I(e.scenes, 0);
    CHECK(file_size("lc_openfail.dv") > 0);
    CHECK(!pin_test_file_exists("lc_openfail-0002.dv"));
    pin_close(s);
    pin_test_setenv("PIN_TEST_FAT32_LIMIT", "");
    pin_test_setenv("PIN_TEST_OPEN_FAIL", "");
    remove("lc_openfail.dv");
    printf("OK: a cut that cannot open its file ends the capture (WRITE_ERROR, ERROR)\n");
}

/* A failed close of a file with a long non-ASCII name: the reason stays whole. */
static void test_long_name(const char *dv)
{
    char base[400] = "lc_";
    for (int i = 0; i < 90; i++)
        strcat(base, "\xc3\xa9");          /* 180 bytes of "é" (names stop at 200) */
    char file[420];
    snprintf(file, sizeof(file), "%s.dv", base);
    remove(file);
    pin_test_setenv("PIN_TEST_CLOSE_FAIL", "1");
    pin_session_t *s = pin_test_open_replay(dv);
    pin_test_prepare_dv(s);
    pin_capture_opts_t o;
    pin_capture_opts_defaults(&o);
    snprintf(o.path, sizeof(o.path), "%s", base);
    o.format_dv = PIN_FMT_DV_RAW;
    CHECK_EQ_I(pin_capture_start(s, &o, 1), PIN_OK);
    events_t e = { 0 };
    pin_status_snapshot_t snap = { .size = sizeof(snap) };
    wait_end(s, &snap, &e);
    printf("long name: %s\n", snap.stop_text);
    CHECK_EQ_I(snap.stop_reason, PIN_STOP_WRITE_ERROR);
    CHECK(valid_utf8(snap.stop_text));
    CHECK(strstr(snap.stop_text, " could not be written completely: ") != NULL);
    CHECK(snap.stop_text[strlen(snap.stop_text) - 1] == '.');
    CHECK(strstr(e.ended_text, file) != NULL);      /* the event has the whole name */
    pin_close(s);
    pin_test_setenv("PIN_TEST_CLOSE_FAIL", "");
    remove(file);
    printf("OK: a long file name keeps the failure sentence whole\n");
}

/* A raw dump, ending by itself: END_OF_TAPE, and a file. */
static void test_end_of_tape(const char *dump)
{
    remove("lc_pass.dv");
    pin_session_t *s = pin_test_open_replay(dump);
    pin_test_prepare_dv(s);
    pin_status_snapshot_t snap = { .size = sizeof(snap) };
    CHECK_EQ_I(pin_deck(s, PIN_DECK_CMD_REW), PIN_OK);
    int stopped = 0;
    for (int i = 0; i < 300 && !stopped; i++) {
        pin_test_sleep_ms(20);
        pin_get_status(s, &snap);
        stopped = snap.deck == PIN_DECK_STOPPED;
    }
    CHECK(stopped);
    CHECK_EQ_I(pin_deck(s, PIN_DECK_CMD_PLAY), PIN_OK);
    pin_capture_opts_t o;
    pin_capture_opts_defaults(&o);
    snprintf(o.path, sizeof(o.path), "lc_pass");
    o.format_dv = PIN_FMT_DV_RAW;
    CHECK_EQ_I(pin_capture_start(s, &o, 1), PIN_OK);
    events_t e = { 0 };
    wait_end(s, &snap, &e);
    CHECK_EQ_I(snap.stop_reason, PIN_STOP_END_OF_TAPE);
    long a = file_size("lc_pass.dv");
    printf("end of tape: %ld bytes\n", a);
    CHECK(a > 0 && a % 144000 == 0);
    pin_close(s);
    remove("lc_pass.dv");
    printf("OK: a raw dump ends by itself\n");
}

/* The last units overflow the queue as the file is finished (the units held back for a scene
 * cut go out together, onto a slow drive): the capture ends with DISK_SLOW. */
static void test_disk_slow_at_end(const char *dv)
{
    remove("lc_slow.dv");
    /* a drive that keeps up with one frame at a time, behind a queue of two frames */
    pin_test_setenv("PIN_TEST_SINK_DELAY_MS", "25");
    pin_test_setenv("PIN_TEST_WRITER_QUEUE", "300000");
    pin_session_t *s = pin_test_open_replay(dv);
    pin_test_prepare_dv(s);
    pin_capture_opts_t o;
    pin_capture_opts_defaults(&o);
    snprintf(o.path, sizeof(o.path), "lc_slow");
    o.format_dv = PIN_FMT_DV_RAW;
    o.scene_split = 1;
    CHECK_EQ_I(pin_capture_start(s, &o, 1), PIN_OK);
    events_t e = { 0 };
    pin_status_snapshot_t snap = { .size = sizeof(snap) };
    wait_end(s, &snap, &e);
    printf("disk slow: stop %d, \"%s\", %s\n", (int)snap.stop_reason, snap.stop_text,
           snap.current_file);
    CHECK_EQ_I(snap.stop_reason, PIN_STOP_DISK_SLOW);
    pin_close(s);
    pin_test_setenv("PIN_TEST_SINK_DELAY_MS", "");
    pin_test_setenv("PIN_TEST_WRITER_QUEUE", "");
    remove("lc_slow-0001.dv");
    printf("OK: an overflow as the last file is finished ends the capture with DISK_SLOW\n");
}

/* Ctrl-C, then the capture's file cannot be closed completely: exit 4, not 130. */
static void test_cancel_then_close_fail(const char *dv)
{
    remove("lc_script.dv");
    pin_test_setenv("PIN_TEST_CLOSE_FAIL", "1");
    pin_session_t *s = pin_test_open_replay(dv);
    pin_test_prepare_dv(s);
    const char *argv[] = { "--capture", "lc_script.dv", "--wait", "wallclock=01:00:00" };
    pin_script_t *sc = NULL;
    char err[256] = "";
    CHECK_EQ_I(pin_script_parse(4, argv, &sc, err, sizeof(err)), PIN_OK);
    CHECK_EQ_I(pin_script_run(s, sc), PIN_OK);
    pin_script_free(sc);
    pin_status_snapshot_t snap = { .size = sizeof(snap) };
    pin_test_wait_state(s, PIN_STATE_CAPTURING, PIN_STATE_ERROR, 5000, &snap);
    CHECK_EQ_I(snap.state, PIN_STATE_CAPTURING);   /* the cancel comes while it runs */
    CHECK_EQ_I(pin_script_cancel(s), PIN_OK);
    int done = 0, code = -1;
    for (int waited = 0; waited < 30000 && !done; waited += 20) {
        pin_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.size = sizeof(ev);
        while (pin_poll_event(s, &ev)) {
            if (ev.kind == PIN_EVT_DONE) {
                done = 1;
                code = ev.a;
                printf("script: exit %d (%s)\n", code, ev.text);
            }
            ev.size = sizeof(ev);
        }
        if (!done)
            pin_test_sleep_ms(20);
    }
    CHECK(done);
    CHECK_EQ_I(code, 4);
    pin_close(s);
    pin_test_setenv("PIN_TEST_CLOSE_FAIL", "");
    remove("lc_script.dv");
    printf("OK: Ctrl-C and a failed close: exit 4\n");
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    pin_test_require_fixture(argv[1]);
    pin_test_require_fixture(argv[2]);
    test_commands(argv[1]);
    test_open_fail(argv[1]);
    test_long_name(argv[1]);
    test_end_of_tape(argv[2]);
    test_cancel_then_close_fail(argv[1]);
    test_disk_slow_at_end(argv[1]);
    return 0;
}
