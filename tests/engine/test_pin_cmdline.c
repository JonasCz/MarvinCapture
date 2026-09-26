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

#include "pin_cmdline.h"
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

static void test_defaults(void)
{
    pin_cmdline_opts_t o;
    pin_cmdline_defaults(&o);
    CHECK(strcmp(o.device, "first") == 0, "default device is \"first\"");
    CHECK(o.input == PIN_CMDLINE_INPUT_UNSET, "default input unset");
    CHECK(o.passes == 1, "default passes is 1");
    CHECK(o.aspect == PIN_CMDLINE_ASPECT_AUTO, "default aspect is auto");
}

static void test_basic_flags(void)
{
    pin_cmdline_opts_t o;
    pin_cmdline_defaults(&o);
    char err[256];
    char *argv[] = { (char *)"pinctl", (char *)"--device", (char *)"usb-1-4",
                      (char *)"--input", (char *)"dv", (char *)"--output",
                      (char *)"C:\\out\\clip", (char *)"--format", (char *)"dv-avi",
                      (char *)"--split", (char *)"--passes", (char *)"3",
                      (char *)"--idle-min", (char *)"10", (char *)"--aspect",
                      (char *)"16:9", (char *)"--title", (char *)"My Tape" };
    int argc = sizeof(argv) / sizeof(argv[0]);
    int rc = pin_cmdline_parse(argc, argv, &o, err, sizeof(err));
    CHECK(rc == 0, "parse should succeed");
    CHECK(strcmp(o.device, "usb-1-4") == 0, "device parsed");
    CHECK(o.input == PIN_CMDLINE_INPUT_DV, "input parsed");
    CHECK(strcmp(o.output, "C:\\out\\clip") == 0, "output parsed");
    CHECK(strcmp(o.format, "dv-avi") == 0, "format parsed");
    CHECK(o.split == 1 && o.split_set, "bare --split means on");
    CHECK(o.passes == 3, "passes parsed");
    CHECK(o.idle_min == 10, "idle-min parsed");
    CHECK(o.aspect == PIN_CMDLINE_ASPECT_16_9, "aspect parsed");
    CHECK(strcmp(o.title, "My Tape") == 0, "title parsed");
}

static void test_bad_input_value(void)
{
    pin_cmdline_opts_t o;
    pin_cmdline_defaults(&o);
    char err[256] = "";
    char *argv[] = { (char *)"pinctl", (char *)"--input", (char *)"betamax" };
    int rc = pin_cmdline_parse(3, argv, &o, err, sizeof(err));
    CHECK(rc == -1, "bad --input value must fail");
    CHECK(err[0] != '\0', "an error message is produced");
}

static void test_missing_value(void)
{
    pin_cmdline_opts_t o;
    pin_cmdline_defaults(&o);
    char err[256] = "";
    char *argv[] = { (char *)"pinctl", (char *)"--output" };
    int rc = pin_cmdline_parse(2, argv, &o, err, sizeof(err));
    CHECK(rc == -1, "a flag with no value must fail");
    CHECK(err[0] != '\0', "an error message is produced");
}

static void test_unrecognised_flag(void)
{
    pin_cmdline_opts_t o;
    pin_cmdline_defaults(&o);
    char err[256] = "";
    char *argv[] = { (char *)"pinctl", (char *)"--frobnicate" };
    int rc = pin_cmdline_parse(2, argv, &o, err, sizeof(err));
    CHECK(rc == -1, "an unknown flag must fail");
    CHECK(err[0] != '\0', "an error message is produced");
}

static void test_actions(void)
{
    pin_cmdline_opts_t o;
    pin_cmdline_defaults(&o);
    char err[256];
    char *argv[] = { (char *)"pinctl", (char *)"--actions",
                      (char *)"rewind,play,capture,stop,wait-eot", (char *)"--exit-when-done" };
    int rc = pin_cmdline_parse(4, argv, &o, err, sizeof(err));
    CHECK(rc == 0, "parse should succeed");
    CHECK(o.num_actions == 5, "five actions parsed");
    CHECK(o.actions[0] == PIN_ACTION_REWIND && o.actions[1] == PIN_ACTION_PLAY &&
          o.actions[2] == PIN_ACTION_CAPTURE && o.actions[3] == PIN_ACTION_STOP &&
          o.actions[4] == PIN_ACTION_WAIT_EOT,
          "action order preserved");
    CHECK(o.exit_when_done, "--exit-when-done set");

    pin_cmdline_action_t acts[4];
    unsigned n;
    char aerr[128];
    int arc = pin_cmdline_parse_actions("rewind,nonsense", acts, 4, &n, aerr, sizeof(aerr));
    CHECK(arc == -1, "an unknown action name must fail");
    CHECK(aerr[0] != '\0', "action parse error message produced");
}

static void test_preset_file_with_override(void)
{
    const char *path = "pin_cmdline_test_preset.ini";
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL, "can write preset file");
    if (f) {
        fputs("[Preset]\n", f);
        fputs("device=usb-preset\n", f);
        fputs("input=svideo\n", f);
        fputs("passes=5\n", f);
        fputs("split=true\n", f);
        fclose(f);
    }

    pin_cmdline_opts_t o;
    pin_cmdline_defaults(&o);
    char err[256];
    /* --input on the command line must win over the preset's svideo. */
    char *argv[] = { (char *)"pinctl", (char *)"--preset", (char *)path, (char *)"--input",
                      (char *)"dv" };
    int rc = pin_cmdline_parse(5, argv, &o, err, sizeof(err));
    CHECK(rc == 0, "parse with preset should succeed");
    CHECK(strcmp(o.device, "usb-preset") == 0, "device comes from the preset file");
    CHECK(o.input == PIN_CMDLINE_INPUT_DV, "explicit --input overrides the preset file");
    CHECK(o.passes == 5, "passes comes from the preset file");
    CHECK(o.split == 1, "split comes from the preset file");

    remove(path);
}

static void test_help(void)
{
    char buf[4096];
    CHECK(pin_cmdline_help(buf, sizeof(buf)) == 0, "help text should fit");
    CHECK(strstr(buf, "--device") != NULL, "help mentions --device");
    CHECK(strstr(buf, "--actions") != NULL, "help mentions --actions");

    char tiny[4];
    CHECK(pin_cmdline_help(tiny, sizeof(tiny)) == -1, "help into a too-small buffer must fail");
}

int main(void)
{
    test_defaults();
    test_basic_flags();
    test_bad_input_value();
    test_missing_value();
    test_unrecognised_flag();
    test_actions();
    test_preset_file_with_override();
    test_help();

    if (g_failures == 0) {
        printf("test_pin_cmdline: all tests passed\n");
        return 0;
    }
    printf("test_pin_cmdline: %d failure(s)\n", g_failures);
    return g_failures;
}
