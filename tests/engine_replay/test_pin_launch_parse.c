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
 * ctest: pin_launch_parse() (pin_api.h), the GUI/pinctl command line parser,
 * exercised as a plain library call -- no session, no device, no replay.
 * Covers a few representative command lines, including the plan's own
 * example ("--actions rewind,capture --exit-when-done").
 *
 * pin_launch_parse()'s argv is documented as "argv[0] skipped" (it expects
 * the same argv a real main() gets, program name included), so every call
 * here goes through pin_parse() below, which prepends a placeholder argv[0]
 * -- exactly what pinctl.c's cmd_actions() does with its own argv.
 */

#include "replay_test_util.h"

static pin_status_t pin_parse(const char *const *flags, int nflags, pin_launch_t *out, char *err,
                               size_t err_cap)
{
    const char **argv = malloc(sizeof(char *) * (size_t)(nflags + 1));
    CHECK(argv);
    argv[0] = "pinctl";
    for (int i = 0; i < nflags; i++)
        argv[i + 1] = flags[i];
    pin_status_t st = pin_launch_parse(nflags + 1, argv, out, err, err_cap);
    free(argv);
    return st;
}

static void test_actions_rewind_capture(void)
{
    const char *argv[] = { "--actions", "rewind,capture", "--exit-when-done" };
    pin_launch_t launch;
    char err[256] = "";
    CHECK(pin_parse(argv, 3, &launch, err, sizeof(err)) == PIN_OK);
    CHECK_EQ_I(launch.action_count, 2);
    CHECK_EQ_I(launch.actions[0], PIN_ACT_REWIND);
    CHECK_EQ_I(launch.actions[1], PIN_ACT_CAPTURE);
    CHECK_EQ_I(launch.exit_when_done, 1);
    printf("OK: --actions rewind,capture --exit-when-done\n");
}

static void test_full_capture_opts(void)
{
    const char *argv[] = {
        "--device", "usb-1-4", "--input", "dv", "--output", "clip",
        "--format", "dv-avi", "--split", "--passes", "3", "--idle-min", "10",
        "--aspect", "16:9", "--title", "My Tape",
    };
    pin_launch_t launch;
    char err[256] = "";
    CHECK(pin_parse(argv, (int)(sizeof(argv) / sizeof(argv[0])), &launch, err, sizeof(err)) ==
          PIN_OK);
    CHECK(strcmp(launch.device, "usb-1-4") == 0);
    CHECK(launch.has_input && launch.input == PIN_INPUT_DV);
    CHECK(launch.has_capture_opts);
    CHECK((launch.capture_fields & PIN_OPT_PATH) != 0);
    CHECK(strcmp(launch.capture.path, "clip") == 0);
    CHECK((launch.capture_fields & PIN_OPT_FORMAT) != 0);
    CHECK_EQ_I(launch.capture.format_dv, PIN_FMT_DV_AVI);
    CHECK((launch.capture_fields & PIN_OPT_SPLIT) != 0);
    CHECK(launch.capture.scene_split);
    CHECK((launch.capture_fields & PIN_OPT_PASSES) != 0);
    CHECK_EQ_I(launch.capture.passes, 3);
    CHECK((launch.capture_fields & PIN_OPT_IDLE) != 0);
    CHECK_EQ_I(launch.capture.idle_stop_minutes, 10);
    CHECK((launch.capture_fields & PIN_OPT_ASPECT) != 0);
    CHECK_EQ_I(launch.capture.aspect, PIN_ASPECT_16_9);
    CHECK((launch.capture_fields & PIN_OPT_TITLE) != 0);
    CHECK(strcmp(launch.capture.title, "My Tape") == 0);
    printf("OK: full capture option set parsed\n");
}

static void test_bad_format(void)
{
    const char *argv[] = { "--format", "betamax" };
    pin_launch_t launch;
    char err[256] = "";
    CHECK(pin_parse(argv, 2, &launch, err, sizeof(err)) == PIN_ERR_ARG);
    CHECK(err[0] != '\0');
    printf("OK: unknown --format rejected (%s)\n", err);
}

static void test_missing_value(void)
{
    const char *argv[] = { "--passes" };
    pin_launch_t launch;
    char err[256] = "";
    CHECK(pin_parse(argv, 1, &launch, err, sizeof(err)) == PIN_ERR_ARG);
    CHECK(err[0] != '\0');
    printf("OK: --passes with no value rejected (%s)\n", err);
}

static void test_help_does_not_crash(void)
{
    const char *argv[] = { "--help" };
    pin_launch_t launch;
    char err[256] = "";
    pin_status_t st = pin_parse(argv, 1, &launch, err, sizeof(err));
    CHECK(st == PIN_OK);
    CHECK_EQ_I(launch.action_count, 0);
    CHECK(pin_launch_help() != NULL && pin_launch_help()[0] != '\0');
    printf("OK: --help handled\n");
}

/* --device <existing file>: pin_cmdline.c can't itself call the replay
 * setter (it's hardware-free, no session/replay state -- see
 * pin_cmdline_opts_t.device_replay_path's comment), so pin_launch_parse()
 * is what actually calls pin_set_replay_file(); this checks the whole
 * chain end to end, including that pin_open() can then open the device by
 * the "replay:<basename>" id pin_launch_parse() put in launch.device. */
static void test_device_existing_file_becomes_replay(void)
{
    const char *path = "pin_launch_parse_test_replay_source.bin";
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) { fputc(0, f); fclose(f); }

    const char *argv[] = { "--device", path };
    pin_launch_t launch;
    char err[256] = "";
    CHECK(pin_parse(argv, 2, &launch, err, sizeof(err)) == PIN_OK);
    char expected[300];
    snprintf(expected, sizeof(expected), "replay:%s", path);
    CHECK(strcmp(launch.device, expected) == 0);

    pin_session_t *s = NULL;
    pin_status_t st = pin_open(launch.device, &s);
    CHECK(st == PIN_OK);
    if (s)
        pin_close(s);

    remove(path);
    printf("OK: --device <existing file> becomes \"%s\" and opens\n", launch.device);
}

int main(void)
{
    test_actions_rewind_capture();
    test_full_capture_opts();
    test_bad_format();
    test_missing_value();
    test_help_does_not_crash();
    test_device_existing_file_becomes_replay();
    return 0;
}
