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

/* The hardware-free helpers behind pin_format_bytes(), pin_deck_cmd_allowed(),
 * pin_capture_action_allowed() and pin_next_file_number(). */

#include "pin_ui_text.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

static int g_failures = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)
#define CHECK_STR(a, b, msg)                                                 \
    do {                                                                     \
        if (strcmp((a), (b)) != 0) {                                         \
            printf("FAIL %s:%d: %s (got \"%s\", want \"%s\")\n", __FILE__,   \
                   __LINE__, msg, (a), (b));                                 \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static void test_formats(void)
{
    char b[64];
    pin_ui_format_bytes(0, b, sizeof(b));
    CHECK_STR(b, "0 B", "zero");
    pin_ui_format_bytes(1023, b, sizeof(b));
    CHECK_STR(b, "1023 B", "bytes");
    pin_ui_format_bytes(1024, b, sizeof(b));
    CHECK_STR(b, "1.0 KB", "KB");
    pin_ui_format_bytes(1610612736ull, b, sizeof(b));
    CHECK_STR(b, "1.5 GB", "GB");
    pin_ui_format_bytes(5ull << 40, b, sizeof(b));
    CHECK_STR(b, "5.0 TB", "TB");
    pin_ui_format_bytes(5000ull << 40, b, sizeof(b));
    CHECK_STR(b, "5000.0 TB", "stays in TB");

    pin_ui_format_time_left(0, b, sizeof(b));
    CHECK_STR(b, "0 min left", "zero");
    pin_ui_format_time_left(59.9, b, sizeof(b));
    CHECK_STR(b, "0 min left", "truncated, not rounded");
    pin_ui_format_time_left(-5, b, sizeof(b));
    CHECK_STR(b, "0 min left", "negative");
    pin_ui_format_time_left(45 * 60 + 59, b, sizeof(b));
    CHECK_STR(b, "45 min left", "minutes");
    pin_ui_format_time_left(3600, b, sizeof(b));
    CHECK_STR(b, "1 h 00 min left", "one hour");
    pin_ui_format_time_left(2 * 3600 + 5 * 60 + 59, b, sizeof(b));
    CHECK_STR(b, "2 h 05 min left", "hours and minutes");
    pin_ui_format_time_left(100 * 3600, b, sizeof(b));
    CHECK_STR(b, "100 h 00 min left", "many hours");

    pin_ui_format_count(0, b, sizeof(b));
    CHECK_STR(b, "0", "count 0");
    pin_ui_format_count(999, b, sizeof(b));
    CHECK_STR(b, "999", "count 999");
    pin_ui_format_count(1000, b, sizeof(b));
    CHECK_STR(b, "1,000", "count 1000");
    pin_ui_format_count(1234567, b, sizeof(b));
    CHECK_STR(b, "1,234,567", "count 1234567");
    pin_ui_format_count(18446744073709551615ull, b, sizeof(b));
    CHECK_STR(b, "18,446,744,073,709,551,615", "count max");

    char tiny[5];
    pin_ui_format_bytes(1610612736ull, tiny, sizeof(tiny));
    CHECK_STR(tiny, "1.5 ", "truncated to the buffer");
}

static void touch(const char *dir, const char *name)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (f)
        fclose(f);
}

static void test_next_file_number(void)
{
    const char *dir = "pin_ui_text_test_dir";
    const char *exts[] = { "avi", "dv", "ts" };
    MKDIR(dir);
    const char *files[] = { "tape-0001.dv", "tape-0002.avi", "TAPE-0007.TS", "tape-3.mkv",
                            "tape-0004", "tape-0009.tar.gz", "tape-x.dv", "tape2-0050.dv",
                            "other-0100.dv", "tape-0001-pass-2.dv", "tape-99999999999.dv" };
    char path[300];

    snprintf(path, sizeof(path), "%s/tape", dir);
    CHECK(pin_ui_next_file_number(path, exts, 3) == 1, "empty directory");

    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        touch(dir, files[i]);

    CHECK(pin_ui_next_file_number(path, exts, 3) == 8, "highest is TAPE-0007.TS (case-insensitive, any ext)");
    snprintf(path, sizeof(path), "%s/tape.dv", dir);
    CHECK(pin_ui_next_file_number(path, exts, 3) == 8, "known extension stripped");
    snprintf(path, sizeof(path), "%s/Tape.AVI", dir);
    CHECK(pin_ui_next_file_number(path, exts, 3) == 8, "extension match is case-insensitive");
    snprintf(path, sizeof(path), "%s/tape.mkv", dir);
    CHECK(pin_ui_next_file_number(path, exts, 3) == 1, "unknown extension is part of the name");
    snprintf(path, sizeof(path), "%s/tape.mkv", dir);
    {
        const char *with_mkv[] = { "mkv" };
        CHECK(pin_ui_next_file_number(path, with_mkv, 1) == 8, "mkv known: name is tape");
    }
    snprintf(path, sizeof(path), "%s/other", dir);
    CHECK(pin_ui_next_file_number(path, exts, 3) == 101, "other name");
    snprintf(path, sizeof(path), "%s/nothing", dir);
    CHECK(pin_ui_next_file_number(path, exts, 3) == 1, "no file of that name");
    snprintf(path, sizeof(path), "%s/", dir);
    CHECK(pin_ui_next_file_number(path, exts, 3) == 1, "empty name");
    CHECK(pin_ui_next_file_number("pin_ui_text_test_missing_dir/tape", exts, 3) == 1, "missing directory");
    CHECK(pin_ui_next_file_number("", exts, 3) == 1, "empty path");
    CHECK(pin_ui_next_file_number(NULL, exts, 3) == 1, "NULL path");
    snprintf(path, sizeof(path), "%s\\tape", dir);
    CHECK(pin_ui_next_file_number(path, exts, 3) == 8, "backslash separator");

    /* no directory part: the current directory */
    touch(".", "pin_ui_text_cwd-0003.dv");
    CHECK(pin_ui_next_file_number("pin_ui_text_cwd", exts, 3) == 4, "current directory");
    remove("pin_ui_text_cwd-0003.dv");

    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, files[i]);
        remove(path);
    }
}

static void test_rules(void)
{
    const pin_deck_cmd_t cmds[] = { PIN_DECK_CMD_PLAY, PIN_DECK_CMD_PAUSE, PIN_DECK_CMD_STOP,
                                    PIN_DECK_CMD_FF, PIN_DECK_CMD_REW };

    /* idle, camera there, tape in: each command except the one the deck is already doing */
    CHECK(pin_ui_deck_cmd_allowed(PIN_STATE_READY, 1, PIN_DECK_UNKNOWN, PIN_DECK_CMD_PLAY), "unknown: play");
    CHECK(pin_ui_deck_cmd_allowed(PIN_STATE_READY, 1, PIN_DECK_STOPPED, PIN_DECK_CMD_PLAY), "stopped: play");
    CHECK(!pin_ui_deck_cmd_allowed(PIN_STATE_READY, 1, PIN_DECK_STOPPED, PIN_DECK_CMD_STOP), "stopped: no stop");
    CHECK(pin_ui_deck_cmd_allowed(PIN_STATE_READY, 1, PIN_DECK_STOPPED, PIN_DECK_CMD_FF), "stopped: ff");
    CHECK(pin_ui_deck_cmd_allowed(PIN_STATE_READY, 1, PIN_DECK_STOPPED, PIN_DECK_CMD_REW), "stopped: rew");
    CHECK(!pin_ui_deck_cmd_allowed(PIN_STATE_READY, 1, PIN_DECK_PLAYING, PIN_DECK_CMD_PLAY), "playing: no play");
    CHECK(!pin_ui_deck_cmd_allowed(PIN_STATE_READY, 1, PIN_DECK_RECORDING, PIN_DECK_CMD_PLAY), "recording: no play");
    CHECK(pin_ui_deck_cmd_allowed(PIN_STATE_READY, 1, PIN_DECK_PLAYING, PIN_DECK_CMD_PAUSE), "playing: pause");
    CHECK(!pin_ui_deck_cmd_allowed(PIN_STATE_READY, 1, PIN_DECK_PAUSED, PIN_DECK_CMD_PAUSE), "paused: no pause");
    CHECK(pin_ui_deck_cmd_allowed(PIN_STATE_READY, 1, PIN_DECK_PAUSED, PIN_DECK_CMD_PLAY), "paused: play");
    CHECK(!pin_ui_deck_cmd_allowed(PIN_STATE_READY, 1, PIN_DECK_FAST_FORWARD, PIN_DECK_CMD_FF), "ff: no ff");
    CHECK(pin_ui_deck_cmd_allowed(PIN_STATE_READY, 1, PIN_DECK_FAST_FORWARD, PIN_DECK_CMD_REW), "ff: rew");
    CHECK(!pin_ui_deck_cmd_allowed(PIN_STATE_READY, 1, PIN_DECK_REWINDING, PIN_DECK_CMD_REW), "rew: no rew");
    CHECK(pin_ui_deck_cmd_allowed(PIN_STATE_READY, 1, PIN_DECK_REWINDING, PIN_DECK_CMD_STOP), "rew: stop");

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        CHECK(!pin_ui_deck_cmd_allowed(PIN_STATE_READY, 1, PIN_DECK_NO_TAPE, cmds[i]), "no tape: nothing");
        CHECK(!pin_ui_deck_cmd_allowed(PIN_STATE_READY, 0, PIN_DECK_STOPPED, cmds[i]), "no camera: nothing");
        for (int st = 0; st <= PIN_STATE_ERROR; st++)
            if (st != PIN_STATE_READY)
                CHECK(!pin_ui_deck_cmd_allowed((pin_state_t)st, 1, PIN_DECK_STOPPED, cmds[i]), "only while READY");
    }

    /* capture buttons */
    CHECK(pin_ui_capture_action_allowed(PIN_STATE_READY, 0, PIN_CAPTURE_START_MANUAL), "manual needs no camera");
    CHECK(!pin_ui_capture_action_allowed(PIN_STATE_READY, 0, PIN_CAPTURE_START_AUTO), "auto needs a camera");
    CHECK(pin_ui_capture_action_allowed(PIN_STATE_READY, 1, PIN_CAPTURE_START_AUTO), "auto with a camera");
    CHECK(!pin_ui_capture_action_allowed(PIN_STATE_READY, 1, PIN_CAPTURE_STOP), "nothing to stop");
    CHECK(pin_ui_manual_capture_allowed(PIN_STATE_READY, 1, PIN_DECK_STOPPED, 1), "manual dv with signal, deck stopped");
    CHECK(pin_ui_manual_capture_allowed(PIN_STATE_READY, 1, PIN_DECK_PLAYING, 1), "manual dv with signal, deck playing");
    CHECK(pin_ui_manual_capture_allowed(PIN_STATE_READY, 0, PIN_DECK_UNKNOWN, 1), "manual dv signal, no camera");
    CHECK(!pin_ui_manual_capture_allowed(PIN_STATE_READY, 1, PIN_DECK_PLAYING, 0), "manual dv needs signal");
    CHECK(!pin_ui_manual_capture_allowed(PIN_STATE_PREPARING, 1, PIN_DECK_STOPPED, 1), "manual dv needs READY");
    CHECK(strcmp(pin_ui_manual_capture_block(1, PIN_DECK_PLAYING, 0), "No incoming video signal") == 0, "no signal text");
    CHECK(pin_ui_manual_capture_block(1, PIN_DECK_STOPPED, 1)[0] == ' ', "no block text with signal");
    CHECK(!pin_ui_capture_action_allowed(PIN_STATE_PREPARING, 1, PIN_CAPTURE_START_MANUAL), "not while preparing");
    CHECK(!pin_ui_capture_action_allowed(PIN_STATE_ERROR, 1, PIN_CAPTURE_START_MANUAL), "not in error");
    CHECK(!pin_ui_capture_action_allowed(PIN_STATE_CAPTURING, 1, PIN_CAPTURE_START_MANUAL), "not while capturing");
    CHECK(pin_ui_capture_action_allowed(PIN_STATE_CAPTURING, 0, PIN_CAPTURE_STOP), "stop while capturing");
    CHECK(!pin_ui_capture_action_allowed(PIN_STATE_CAPTURING, 0, PIN_CAPTURE_STOP_TAPE), "stop tape needs a camera");
    CHECK(pin_ui_capture_action_allowed(PIN_STATE_CAPTURING, 1, PIN_CAPTURE_STOP_TAPE), "stop tape with a camera");
    CHECK(pin_ui_capture_action_allowed(PIN_STATE_REWINDING, 1, PIN_CAPTURE_STOP), "stop while rewinding");
    CHECK(!pin_ui_capture_action_allowed(PIN_STATE_STOPPING, 1, PIN_CAPTURE_STOP), "not while finalising");
    CHECK(!pin_ui_capture_action_allowed(PIN_STATE_STOPPING, 1, PIN_CAPTURE_STOP_TAPE), "not while finalising (tape)");
}

int main(void)
{
    test_formats();
    test_next_file_number();
    test_rules();
    if (g_failures == 0)
        printf("test_pin_ui_text: all passed\n");
    return g_failures ? 1 : 0;
}
