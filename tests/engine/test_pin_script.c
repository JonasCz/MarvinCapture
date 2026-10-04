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

/* The command-line parser (docs/cli.md): valid scripts, every validation
 * error, extension -> format mapping, timecodes and durations, help. */

#include "pin_script.h"
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

#define ARGS(...) ((const char *const[]){ __VA_ARGS__ })
#define NARGS(...) ((int)(sizeof((const char *const[]){ __VA_ARGS__ }) / sizeof(char *)))
#define PARSE(sc, err, ...) pin_script_parse_args(NARGS(__VA_ARGS__), ARGS(__VA_ARGS__), &(sc), err, sizeof(err))

static const pin_script_item_t *find_step(const pin_script_t *sc, int step)
{
    for (int i = 0; i < sc->nitems; i++)
        if (sc->items[i].kind == PIN_SITEM_STEP && sc->items[i].step == step)
            return &sc->items[i];
    return NULL;
}

static void expect_text(const pin_script_t *sc, int step, const char *want)
{
    char buf[256];
    CHECK(pin_script_step_description(sc, step, buf, sizeof(buf)) == PIN_OK, "step text");
    if (strcmp(buf, want) != 0) {
        printf("FAIL step %d: \"%s\" != \"%s\"\n", step, buf, want);
        g_failures++;
    }
}

/* argv must fail with an error message containing `needle` */
static void expect_error_n(int argc, const char *const *argv, const char *needle)
{
    pin_script_t *sc = NULL;
    char err[256] = "";
    pin_status_t st = pin_script_parse_args(argc, argv, &sc, err, sizeof(err));
    if (st != PIN_ERR_ARG || sc != NULL || !strstr(err, needle) || strchr(err, '\n')) {
        printf("FAIL expected usage error containing \"%s\" for:", needle);
        for (int i = 0; i < argc; i++)
            printf(" %s", argv[i]);
        printf("\n  got status %d, message \"%s\"\n", (int)st, err);
        g_failures++;
        pin_script_destroy(sc);
    }
}
#define EXPECT_ERROR(needle, ...) expect_error_n(NARGS(__VA_ARGS__), ARGS(__VA_ARGS__), needle)

static void test_timecodes(void)
{
    pin_tc_t t;
    char why[64];
    CHECK(pin_tc_parse("00:14:30:00", &t, why, sizeof(why)), "tc ok");
    CHECK(t.h == 0 && t.m == 14 && t.s == 30 && t.f == 0 && t.sep == ':', "tc fields");
    CHECK(pin_tc_parse("01:02:03;29", &t, why, sizeof(why)) && t.sep == ';' && t.f == 29, "drop frame");
    char out[16];
    pin_tc_format(&t, out, sizeof(out));
    CHECK(strcmp(out, "01:02:03;29") == 0, "format keeps ';'");
    CHECK(!pin_tc_parse("00:60:00:00", &t, why, sizeof(why)) && strstr(why, "minutes"), "minutes 60");
    CHECK(!pin_tc_parse("00:00:60:00", &t, why, sizeof(why)) && strstr(why, "seconds"), "seconds 60");
    CHECK(!pin_tc_parse("00:00:00:30", &t, why, sizeof(why)) && strstr(why, "frames"), "frames 30");
    CHECK(!pin_tc_parse("0:00:00:00", &t, why, sizeof(why)), "too short");
    CHECK(!pin_tc_parse("00:00:00", &t, why, sizeof(why)), "three fields");
    CHECK(!pin_tc_parse("00-00-00-00", &t, why, sizeof(why)), "wrong separators");
    CHECK(!pin_tc_parse("0a:00:00:00", &t, why, sizeof(why)), "non-digit");
    CHECK(!pin_tc_parse("", &t, why, sizeof(why)), "empty");

    pin_tc_t a, b;
    pin_tc_parse("00:59:59:29", &a, NULL, 0);
    pin_tc_parse("01:00:00:00", &b, NULL, 0);
    CHECK(pin_tc_compare(&a, &b) < 0 && pin_tc_compare(&b, &a) > 0 && pin_tc_compare(&a, &a) == 0, "compare");
    pin_tc_parse("00:00:01:15", &a, NULL, 0);
    CHECK(fabs(pin_tc_seconds(&a, 30.0) - 1.5) < 1e-9, "seconds at 30 fps");
    CHECK(fabs(pin_tc_seconds(&a, 0) - 1.6) < 1e-9, "seconds default 25 fps");
}

static int cond_is(const pin_script_cond_t *c, pin_cond_kind_t kind, int h, int m, int sec, int f)
{
    return c->kind == kind && c->tc.h == h && c->tc.m == m && c->tc.s == sec && c->tc.f == f;
}

static void expect_cond_text(const char *tok, const char *want)
{
    pin_script_cond_t c;
    char buf[64];
    if (!pin_script_cond_parse(tok, &c, NULL, 0)) {
        printf("FAIL condition \"%s\" does not parse\n", tok);
        g_failures++;
        return;
    }
    pin_script_cond_text(&c, buf, sizeof(buf));
    if (strcmp(buf, want) != 0) {
        printf("FAIL condition text \"%s\" != \"%s\"\n", buf, want);
        g_failures++;
    }
}

static void test_conditions(void)
{
    pin_script_cond_t c;
    char why[160];
    pin_tc_t d;
    CHECK(pin_dur_parse("01:02:03", &d, why, sizeof(why)) && d.h == 1 && d.m == 2 && d.s == 3 && d.f == 0, "dur HH:MM:SS");
    CHECK(pin_dur_parse("01:02:03:04", &d, why, sizeof(why)) && d.f == 4, "dur HH:MM:SS:FF");
    CHECK(!pin_dur_parse("01:02", &d, why, sizeof(why)), "dur too short");
    CHECK(!pin_dur_parse("+01:02:03", &d, why, sizeof(why)), "dur with +");
    CHECK(!pin_dur_parse("00:00:61", &d, why, sizeof(why)) && strstr(why, "seconds"), "dur seconds 61");

    /* without a value: 5 s for idle and signal, one minute for nosignal */
    CHECK(pin_script_cond_parse("idle", &c, why, sizeof(why)) && cond_is(&c, PIN_COND_IDLE, 0, 0, 5, 0), "idle default");
    CHECK(pin_script_cond_parse("signal", &c, why, sizeof(why)) && cond_is(&c, PIN_COND_SIGNAL, 0, 0, 5, 0), "signal default");
    CHECK(pin_script_cond_parse("nosignal", &c, why, sizeof(why)) && cond_is(&c, PIN_COND_NOSIGNAL, 0, 1, 0, 0),
          "nosignal default");
    /* with a value, both DUR forms */
    CHECK(pin_script_cond_parse("idle=00:00:10", &c, why, sizeof(why)) && cond_is(&c, PIN_COND_IDLE, 0, 0, 10, 0), "idle=DUR");
    CHECK(pin_script_cond_parse("signal=00:00:05:12", &c, why, sizeof(why)) && cond_is(&c, PIN_COND_SIGNAL, 0, 0, 5, 12),
          "signal=DUR:FF");
    CHECK(pin_script_cond_parse("nosignal=00:00:30", &c, why, sizeof(why)) && cond_is(&c, PIN_COND_NOSIGNAL, 0, 0, 30, 0),
          "nosignal=30s");
    CHECK(pin_script_cond_parse("nosignal=00:00:30:10", &c, why, sizeof(why)) && cond_is(&c, PIN_COND_NOSIGNAL, 0, 0, 30, 10),
          "nosignal=30s+10f");
    CHECK(pin_script_cond_parse("timecode=00:14:30:00", &c, why, sizeof(why)) && cond_is(&c, PIN_COND_TIMECODE, 0, 14, 30, 0),
          "timecode");
    CHECK(pin_script_cond_parse("timecode=01:02:03;04", &c, why, sizeof(why)) && c.tc.sep == ';', "timecode drop-frame");
    CHECK(pin_script_cond_parse("wallclock=00:00:15", &c, why, sizeof(why)) && cond_is(&c, PIN_COND_WALLCLOCK, 0, 0, 15, 0),
          "wallclock=DUR");
    CHECK(pin_script_cond_parse("wallclock=04:00:00:00", &c, why, sizeof(why)) && cond_is(&c, PIN_COND_WALLCLOCK, 4, 0, 0, 0),
          "wallclock=DUR:FF");
    CHECK(pin_script_cond_parse("captured=04:00:00", &c, why, sizeof(why)) && cond_is(&c, PIN_COND_CAPTURED, 4, 0, 0, 0),
          "captured=DUR");
    CHECK(pin_script_cond_parse("captured=00:00:01:05", &c, why, sizeof(why)) && cond_is(&c, PIN_COND_CAPTURED, 0, 0, 1, 5),
          "captured=DUR:FF");

    /* values that are required, malformed, or the old spellings */
    CHECK(!pin_script_cond_parse("timecode", &c, why, sizeof(why)) && strstr(why, "needs a value"), "timecode needs a value");
    CHECK(!pin_script_cond_parse("wallclock", &c, why, sizeof(why)) && strstr(why, "needs a value"), "wallclock needs a value");
    CHECK(!pin_script_cond_parse("captured", &c, why, sizeof(why)) && strstr(why, "needs a value"), "captured needs a value");
    CHECK(!pin_script_cond_parse("timecode=00:14:30", &c, why, sizeof(why)), "timecode needs frames");
    CHECK(!pin_script_cond_parse("timecode=00:60:00:00", &c, why, sizeof(why)), "timecode minutes 60");
    CHECK(!pin_script_cond_parse("idle=", &c, why, sizeof(why)), "idle= empty");
    CHECK(!pin_script_cond_parse("idle=+00:00:10", &c, why, sizeof(why)), "no leading +");
    CHECK(!pin_script_cond_parse("nosignal=+00:00:30:00", &c, why, sizeof(why)), "old nosignal=+DUR");
    CHECK(!pin_script_cond_parse("nosignal=00:99:00", &c, why, sizeof(why)), "nosignal bad value");
    CHECK(!pin_script_cond_parse("wallclock=00:00:00:30", &c, why, sizeof(why)), "duration frames 30");
    CHECK(!pin_script_cond_parse("+00:00:10:00", &c, why, sizeof(why)), "old +HH:MM:SS:FF");
    CHECK(!pin_script_cond_parse("00:21:00:00", &c, why, sizeof(why)), "old bare timecode");
    CHECK(!pin_script_cond_parse("nosignals", &c, why, sizeof(why)), "nosignals unknown");
    CHECK(!pin_script_cond_parse("idles=00:00:10", &c, why, sizeof(why)), "idles unknown");
    CHECK(!pin_script_cond_parse("bogus", &c, why, sizeof(why)), "bogus");
    CHECK(!pin_script_cond_parse("", &c, why, sizeof(why)), "empty");

    expect_cond_text("idle", "idle=00:00:05");
    expect_cond_text("signal=00:00:05:12", "signal=00:00:05:12");
    expect_cond_text("nosignal=00:00:30:00", "nosignal=00:00:30");
    expect_cond_text("timecode=00:14:30:00", "timecode=00:14:30:00");
    expect_cond_text("wallclock=00:00:15", "wallclock=00:00:15");
    expect_cond_text("captured=04:00:00", "captured=04:00:00");
}

static void test_extensions(void)
{
    pin_format_t f[3];
    unsigned mask;
    /* index: analog, DV, HDV */
    CHECK(pin_script_ext_formats("dv", f, &mask) && f[1] == PIN_FMT_DV_RAW && mask == (1u | 4u), ".dv: DV only");
    CHECK(pin_script_ext_formats("avi", f, &mask) && f[0] == PIN_FMT_ANALOG_AVI && f[1] == PIN_FMT_DV_AVI &&
              f[2] == PIN_FMT_HDV_TS && mask == 4u, ".avi: analog and DV");
    CHECK(pin_script_ext_formats("MOV", f, &mask) && f[1] == PIN_FMT_DV_MOV && f[2] == PIN_FMT_HDV_MOV &&
              f[0] == PIN_FMT_ANALOG_AVI && mask == 1u, ".mov (case-insensitive): DV and HDV");
    CHECK(pin_script_ext_formats("ts", f, &mask) && f[2] == PIN_FMT_HDV_TS && mask == (1u | 2u), ".ts");
    CHECK(pin_script_ext_formats("m2t", f, &mask) && f[2] == PIN_FMT_HDV_TS, ".m2t");
    CHECK(pin_script_ext_formats("mkv", f, &mask) && f[0] == PIN_FMT_ANALOG_FFV1_MKV && f[2] == PIN_FMT_HDV_MKV &&
              f[1] == PIN_FMT_DV_RAW && mask == 2u, ".mkv: analog and HDV");
    CHECK(!pin_script_ext_formats("mp4", f, &mask), "unknown extension");

    char base[128], ext[16];
    pin_script_split_ext("C:\\caps\\tape.01.AVI", base, sizeof(base), ext, sizeof(ext));
    CHECK(strcmp(base, "C:\\caps\\tape.01") == 0 && strcmp(ext, "avi") == 0, "split keeps dots in the name");
    pin_script_split_ext("dir.d/clip", base, sizeof(base), ext, sizeof(ext));
    CHECK(strcmp(base, "dir.d/clip") == 0 && ext[0] == 0, "dot in a directory is no extension");
    pin_script_split_ext("clip.xyz", base, sizeof(base), ext, sizeof(ext));
    CHECK(strcmp(base, "clip.xyz") == 0 && ext[0] == 0, "unknown extension stays in the base");
}

static void test_cli_examples(void)
{
    pin_script_t *sc;
    char err[256];

    /* the first example of docs/cli.md */
    CHECK(PARSE(sc, err, "--rew", "--wait", "--play", "--capture", "tape01.avi", "--wait") == PIN_OK, "example 1");
    expect_text(sc, 0, "rew");
    expect_text(sc, 1, "wait-any idle=00:00:05");
    expect_text(sc, 2, "play");
    expect_text(sc, 3, "capture tape01.avi");
    expect_text(sc, 4, "wait-any idle=00:00:05");
    CHECK(sc->nsteps == 5, "example 1 has 5 steps");
    CHECK(!sc->help, "no help");
    pin_script_destroy(sc);

    /* example 2 */
    CHECK(PARSE(sc, err, "--rew", "--wait", "--play", "--wait-any", "timecode=00:14:30:00", "--capture", "clip.dv",
                "--wait-any", "timecode=00:21:00:00", "--stop") == PIN_OK, "example 2");
    CHECK(sc->nsteps == 7, "example 2 steps");
    expect_text(sc, 3, "wait-any timecode=00:14:30:00");
    expect_text(sc, 5, "wait-any timecode=00:21:00:00");
    expect_text(sc, 6, "stop");
    const pin_script_item_t *cap = find_step(sc, 4);
    CHECK(cap && cap->step_kind == PIN_SSTEP_CAPTURE && strcmp(cap->cap.base, "clip") == 0 &&
              cap->cap.format[1] == PIN_FMT_DV_RAW && cap->cap.warn_mask == (1u | 4u), "clip.dv capture");
    const pin_script_item_t *w = find_step(sc, 3);
    CHECK(w && !w->wait_all && w->nconds == 1 && w->conds[0].kind == PIN_COND_TIMECODE, "wait-any item");
    pin_script_destroy(sc);

    /* example 4: analog */
    CHECK(PARSE(sc, err, "-i", "svideo", "--std", "pal", "--capture", "vhs.mkv", "--wait-any", "signal=00:00:05",
                "--wait-any", "nosignal=00:00:30,captured=04:00:00") == PIN_OK, "example 4");
    CHECK(sc->nsteps == 3, "example 4 steps");
    expect_text(sc, 1, "wait-any signal=00:00:05");
    expect_text(sc, 2, "wait-any nosignal=00:00:30,captured=04:00:00");
    CHECK(sc->initial.has_input && sc->initial.input == PIN_INPUT_SVIDEO && sc->initial.has_std &&
              sc->initial.std == PIN_STD_PAL, "initial settings");
    cap = find_step(sc, 0);
    CHECK(cap && cap->cap.format[0] == PIN_FMT_ANALOG_FFV1_MKV, "vhs.mkv is FFV1");
    pin_script_destroy(sc);

    /* wait-all */
    CHECK(PARSE(sc, err, "--rew", "--wait", "--play", "--capture", "t.avi", "--wait-all", "idle,nosignal=00:00:10")
              == PIN_OK, "wait-all example");
    w = find_step(sc, 4);
    CHECK(w && w->wait_all && w->nconds == 2, "wait-all item");
    expect_text(sc, 4, "wait-all idle=00:00:05,nosignal=00:00:10");
    pin_script_destroy(sc);

    /* streaming to stdout parses and carries the flag */
    CHECK(PARSE(sc, err, "--play", "--capture", "-", "--wait") == PIN_OK, "example 3");
    cap = find_step(sc, 1);
    CHECK(cap && cap->cap.to_stdout, "stdout flag");
    expect_text(sc, 1, "capture -");
    pin_script_destroy(sc);

    /* one stream, no scene split */
    EXPECT_ERROR("only once", "--capture", "-", "--wait", "wallclock=00:00:01", "--capture", "-");
    EXPECT_ERROR("--split", "--split", "--capture", "-");
    CHECK(PARSE(sc, err, "--capture", "-", "--wait", "wallclock=00:00:01", "--capture", "a.dv") == PIN_OK,
          "a file after the stream is fine");
    pin_script_destroy(sc);
}

static void test_wait_forms(void)
{
    pin_script_t *sc;
    char err[256];
    const pin_script_item_t *w;

    /* --wait is --wait-any; with a list too; bare is idle */
    CHECK(PARSE(sc, err, "--wait", "signal,wallclock=00:00:10", "--wait") == PIN_OK, "--wait with a list");
    w = find_step(sc, 0);
    CHECK(w && !w->wait_all && w->nconds == 2, "--wait is wait-any");
    expect_text(sc, 0, "wait-any signal=00:00:05,wallclock=00:00:10");
    expect_text(sc, 1, "wait-any idle=00:00:05");
    pin_script_destroy(sc);

    /* inline forms */
    CHECK(PARSE(sc, err, "--wait-any=idle,signal", "--wait-all=signal,nosignal=00:00:02:10", "--wait=idle") == PIN_OK,
          "inline = forms");
    expect_text(sc, 0, "wait-any idle=00:00:05,signal=00:00:05");
    expect_text(sc, 1, "wait-all signal=00:00:05,nosignal=00:00:02:10");
    expect_text(sc, 2, "wait-any idle=00:00:05");
    pin_script_destroy(sc);

    /* chaining, any number */
    CHECK(PARSE(sc, err, "--play", "--wait-any", "wallclock=00:00:01", "--wait-all", "signal=00:00:01",
                "--wait", "wallclock=00:00:02", "--wait-any", "idle") == PIN_OK && sc->nsteps == 5, "chained waits");
    pin_script_destroy(sc);

    /* the list is mandatory for --wait-any / --wait-all, and a following option is not a list */
    EXPECT_ERROR("--wait-any needs a condition list", "--wait-any");
    EXPECT_ERROR("--wait-all needs a condition list", "--wait-all", "--stop");
    EXPECT_ERROR("--wait-any idle,: empty condition", "--wait-any", "idle,");

    /* the removed forms */
    EXPECT_ERROR("--wait +00:00:10:00", "--wait", "+00:00:10:00");
    EXPECT_ERROR("--wait-any 00:21:00:00", "--wait-any", "00:21:00:00");
    EXPECT_ERROR("--wait-all nosignal=+00:00:30:00", "--wait-all", "nosignal=+00:00:30:00");
    EXPECT_ERROR("timecode needs a value", "--wait", "timecode");

    /* captured needs an open capture */
    EXPECT_ERROR("captured needs an open capture", "--wait-any", "captured=00:00:10");
    EXPECT_ERROR("captured needs an open capture", "--play", "--wait", "signal,captured=00:00:10");
    EXPECT_ERROR("captured needs an open capture", "--capture", "a.dv", "--stop", "--wait", "captured=00:00:10");
    CHECK(PARSE(sc, err, "--capture", "a.dv", "--wait-all", "signal=00:00:01,captured=00:00:10", "--wait-any",
                "captured=00:00:20") == PIN_OK, "captured after a capture, over several waits");
    pin_script_destroy(sc);
    CHECK(PARSE(sc, err, "-i", "svideo", "--capture", "a.avi", "--wait", "captured=04:00:00") == PIN_OK,
          "captured on an analog input");
    pin_script_destroy(sc);
    /* a new --capture opens a new one */
    CHECK(PARSE(sc, err, "--capture", "a.dv", "--wait", "wallclock=00:00:01", "--capture", "b.dv", "--wait",
                "captured=00:00:05") == PIN_OK, "captured after the second capture");
    pin_script_destroy(sc);

    /* too many conditions */
    EXPECT_ERROR("too many conditions", "--wait", "signal,signal,signal,signal,signal,signal,signal,signal,signal");
}

static void test_settings_state(void)
{
    pin_script_t *sc;
    char err[256];
    CHECK(PARSE(sc, err, "-d", "usb:2-1", "--aspect", "16:9", "--split", "--title", "My tape", "--keep-raw",
                "--overwrite", "--format", "dv-avi", "--format=hdv-mov", "--brightness", "-10", "--hue=5",
                "--audio-gain", "30", "--debug", "--capture", "a.mov", "--wait", "wallclock=00:00:05",
                "--capture", "b.avi") == PIN_OK, "settings");
    CHECK(sc->has_device && strcmp(sc->device, "usb:2-1") == 0, "device");
    CHECK(sc->debug, "debug");
    pin_script_settings_t st = { .size = sizeof(st) };
    st = sc->initial;
    CHECK(st.has_aspect && st.aspect == PIN_ASPECT_16_9 && st.has_split && st.split && st.has_title &&
              strcmp(st.title, "My tape") == 0 && st.has_keep_raw, "initial capture settings");
    CHECK(st.has_format_dv && st.format_dv == PIN_FMT_DV_AVI && st.has_format_hdv && st.format_hdv == PIN_FMT_HDV_MOV &&
              !st.has_format_analog, "initial formats per kind");
    CHECK((st.controls_set & (1u << PIN_CTL_BRIGHTNESS)) && st.control_brightness == -10 && st.control_hue == 5 &&
              st.control_audio_gain == 30 && !(st.controls_set & (1u << PIN_CTL_CONTRAST)), "controls");
    const pin_script_item_t *a = find_step(sc, 0), *b = find_step(sc, 2);
    CHECK(a && a->cap.aspect == PIN_ASPECT_16_9 && a->cap.split && a->cap.keep_raw && a->cap.overwrite &&
              strcmp(a->cap.title, "My tape") == 0, "capture a carries the settings");
    CHECK(a->cap.format[1] == PIN_FMT_DV_AVI && a->cap.format[2] == PIN_FMT_HDV_MOV, "explicit --format wins");
    CHECK(b && b->cap.format[1] == PIN_FMT_DV_AVI, "format persists to the next capture");
    pin_script_destroy(sc);

    /* settings between captures apply to the steps after them only */
    CHECK(PARSE(sc, err, "--capture", "a.dv", "--stop", "--aspect", "4:3", "--capture", "b.dv") == PIN_OK,
          "settings after a transport action");
    a = find_step(sc, 0);
    b = find_step(sc, 2);
    CHECK(a->cap.aspect == PIN_ASPECT_AUTO && b->cap.aspect == PIN_ASPECT_4_3, "aspect state per step");
    CHECK(!sc->initial.has_aspect, "later setting is not an initial one");
    pin_script_destroy(sc);

    /* a standard name with and without the dash */
    CHECK(PARSE(sc, err, "--std", "ntsc443") == PIN_OK && sc->initial.std == PIN_STD_NTSC_443, "ntsc443");
    pin_script_destroy(sc);
    CHECK(PARSE(sc, err, "--std", "NTSC-443") == PIN_OK && sc->initial.std == PIN_STD_NTSC_443, "ntsc-443");
    CHECK(sc->nsteps == 0, "settings only: no steps");
    pin_script_destroy(sc);

    /* .m2t is kept as the extension of a raw HDV file, .ts leaves the default */
    CHECK(PARSE(sc, err, "--capture", "a.m2t", "--stop", "--capture", "b.ts") == PIN_OK, "m2t parse");
    a = find_step(sc, 0);
    b = find_step(sc, 2);
    CHECK(a && strcmp(a->cap.ts_ext, "m2t") == 0 && strcmp(a->cap.base, "a") == 0, ".m2t kept");
    CHECK(b && b->cap.ts_ext[0] == 0, ".ts is the default extension");
    pin_script_destroy(sc);

    /* a bare "--capture x" after "--wait" with the equals form */
    CHECK(PARSE(sc, err, "--play", "--wait=idle", "--capture=z.ts") == PIN_OK && sc->nsteps == 3, "= forms");
    expect_text(sc, 1, "wait-any idle=00:00:05");
    expect_text(sc, 2, "capture z.ts");
    pin_script_destroy(sc);
}

static void test_help_and_empty(void)
{
    pin_script_t *sc;
    char err[64];
    CHECK(pin_script_parse_args(0, NULL, &sc, err, sizeof(err)) == PIN_OK && sc->help, "no arguments: help");
    pin_script_destroy(sc);
    CHECK(PARSE(sc, err, "-h") == PIN_OK && sc->help, "-h");
    pin_script_destroy(sc);
    CHECK(PARSE(sc, err, "--rew", "--help", "--bogus") == PIN_OK && sc->help, "--help anywhere wins");
    pin_script_destroy(sc);
    const char *h = pin_script_help_text();
    CHECK(strstr(h, "--capture") && strstr(h, "--wait-any") && strstr(h, "--wait-all") && strstr(h, "captured=") &&
              strstr(h, "nosignal") && strstr(h, "--debug") &&
              strstr(h, "--overwrite") && strstr(h, "not precise"), "help covers the language and the winding note");
    CHECK(strstr(h, "Exit codes") && strstr(h, "5 a capture received no video"), "help lists exit codes incl. 5");
}

static void test_errors(void)
{
    EXPECT_ERROR("--bogus", "--bogus");
    EXPECT_ERROR("-x", "-x");
    EXPECT_ERROR("stray", "stray");
    EXPECT_ERROR("--input", "--input");                         /* missing value */
    EXPECT_ERROR("--input: expected dv, svideo or composite", "--input", "s-video");
    EXPECT_ERROR("--std: unknown standard", "--std", "pal-x");
    EXPECT_ERROR("--format: unknown format \"mp4\"", "--format", "mp4");
    EXPECT_ERROR("--aspect: expected", "--aspect", "21:9");
    EXPECT_ERROR("--brightness: expected a whole number", "--brightness", "bright");
    EXPECT_ERROR("--split takes no value", "--split=1");
    EXPECT_ERROR("--rew takes no value", "--rew=now");
    EXPECT_ERROR("--capture needs a file name", "--capture");
    EXPECT_ERROR("--capture needs a file name", "--capture", "--wait");
    EXPECT_ERROR("cannot tell the format", "--capture", "clip");
    EXPECT_ERROR("cannot tell the format", "--capture", "clip.mp4");
    EXPECT_ERROR("--wait bogus", "--wait", "bogus");
    EXPECT_ERROR("--wait timecode=00:00:60:00", "--wait", "timecode=00:00:60:00");
    EXPECT_ERROR("--wait wallclock=00:00:00:30", "--wait", "wallclock=00:00:00:30");
    EXPECT_ERROR("--wait nosignal=00:01", "--wait", "nosignal=00:01");
    EXPECT_ERROR("--wait idle,", "--wait", "idle,");
    EXPECT_ERROR("must come before the first action", "--play", "-d", "usb:1-1");
    EXPECT_ERROR("must come before the first action", "--wait", "--device", "x");
    /* settings while a capture is open */
    EXPECT_ERROR("--format cannot change while a capture is open", "--capture", "a.dv", "--format", "dv-avi");
    EXPECT_ERROR("--split cannot change while a capture is open", "--capture", "a.dv", "--wait", "--split");
    EXPECT_ERROR("--brightness cannot change", "--capture", "a.dv", "--brightness", "1");
    EXPECT_ERROR("--input cannot change", "--capture", "a.dv", "-i", "svideo");
    /* analog input */
    EXPECT_ERROR("--rew needs the DV input", "-i", "svideo", "--rew");
    EXPECT_ERROR("--play needs the DV input", "-i", "composite", "--play");
    EXPECT_ERROR("--wait idle: idle needs the DV input", "-i", "svideo", "--wait", "idle");
    EXPECT_ERROR("idle needs the DV input", "-i", "svideo", "--wait-all", "signal,idle=00:00:10");
    EXPECT_ERROR("timecode needs the DV input", "-i", "svideo", "--wait", "signal,timecode=00:00:10:00");
    EXPECT_ERROR("--stop needs the DV input", "--input=composite", "--stop");

    /* but the analog-only conditions are fine, and so is dv again after svideo */
    pin_script_t *sc;
    char err[128];
    CHECK(PARSE(sc, err, "-i", "svideo", "--wait", "signal,nosignal,wallclock=00:00:10") == PIN_OK, "analog waits");
    pin_script_destroy(sc);
    CHECK(PARSE(sc, err, "-i", "svideo", "--capture", "a.avi", "--wait", "wallclock=00:00:10", "-i", "dv" ) == PIN_ERR_ARG,
          "input cannot change while capturing");
    CHECK(PARSE(sc, err, "-i", "svideo", "--wait", "signal", "-i", "dv", "--rew") == PIN_OK, "dv again");
    pin_script_destroy(sc);
}

static void test_clone_and_text_range(void)
{
    pin_script_t *sc, *c;
    char err[64], buf[64];
    CHECK(PARSE(sc, err, "--play", "--wait") == PIN_OK, "parse");
    c = pin_script_clone(sc);
    pin_script_destroy(sc);
    CHECK(c && c->nsteps == 2, "clone survives the original");
    expect_text(c, 1, "wait-any idle=00:00:05");
    CHECK(pin_script_step_description(c, 2, buf, sizeof(buf)) == PIN_ERR_ARG, "index out of range");
    CHECK(pin_script_step_description(c, -1, buf, sizeof(buf)) == PIN_ERR_ARG, "negative index");
    pin_script_destroy(c);
}

int main(void)
{
    test_timecodes();
    test_conditions();
    test_extensions();
    test_cli_examples();
    test_wait_forms();
    test_settings_state();
    test_help_and_empty();
    test_errors();
    test_clone_and_text_range();
    if (g_failures) {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("OK: pin_script parser\n");
    return 0;
}
