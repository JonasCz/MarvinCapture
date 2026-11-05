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

/* The calls an app on Android needs and the desktop GUIs do not: naming a USB device found
 * elsewhere (pin_usb_model, pin_device_fd_problem), opening one by file descriptor
 * (pin_open("fd:N")), the app's own directories (pin_set_app_dirs), the per-platform format
 * list and warnings (with the H.264 / HEVC formats that depend on the encoder probe), the
 * format settings (pin_format_options, the size-versioned pin_capture_opts_t), and the
 * keep-busy mode. Hardware-free; links src/api/pin_api.c. */

#include "../../src/api/pin_api.h"
#include "../../src/core/pin_thread_boost.h"
#include "../../src/core/pinnacle_lock.h"
#include "../../src/engine/pin_settings.h"
#include "../../src/sinks/pin_sink.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

static int g_failures = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static int exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

#if defined(__linux__)   /* only the Linux / Android fd test waits */
static void sleep_ms(int ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

static pin_device_info_t info(void)
{
    pin_device_info_t d;
    memset(&d, 0, sizeof(d));
    d.size = sizeof(d);
    return d;
}

static void test_usb_model(void)
{
    pin_device_info_t d = info();
    char b[512];
    CHECK(pin_usb_model(0x2304, 0x0213, &d) == PIN_OK, "500-USB known");
    CHECK(strcmp(d.name, "Pinnacle Studio 500-USB") == 0, "500-USB name");
    CHECK(d.vid == 0x2304 && d.pid == 0x0213, "vid/pid");
    CHECK(d.tested == 1 && d.state == PIN_DEV_READY, "500-USB tested, ready");
    CHECK(d.hub_depth == -1, "hub depth unknown");
    CHECK(d.id[0] == 0 && d.serial[0] == 0 && d.owner_pid == 0, "no id, serial, owner");
    CHECK(d.size == sizeof(d), "size kept");
    CHECK(pin_device_fd_problem(&d, b, sizeof(b)) == 0 && b[0] == 0, "500-USB works from an fd");

    d = info();
    CHECK(pin_usb_model(0x2304, 0x0223, &d) == PIN_OK && d.tested == 1, "510-USB");
    d = info();
    CHECK(pin_usb_model(0x2304, 0x0212, &d) == PIN_OK && d.tested == 0 && d.state == PIN_DEV_READY,
          "700-USB: supported, untested");
    pin_device_display_name(&d, b, sizeof(b));
    CHECK(strcmp(b, "Pinnacle Studio 700-USB (untested)") == 0, "display name");

    /* the MovieBox Deluxe: described as the table has it, but not usable from an fd yet */
    d = info();
    CHECK(pin_usb_model(0x2304, 0x0206, &d) == PIN_OK && d.state == PIN_DEV_READY && d.tested == 0,
          "Deluxe described truthfully");
    CHECK(pin_device_fd_problem(&d, b, sizeof(b)) == 1 && strstr(b, "MovieBox Deluxe"), "Deluxe fd problem");

    d = info();
    CHECK(pin_usb_model(0x2304, 0x9999, &d) == PIN_ERR_NOT_FOUND, "unknown pid");
    CHECK(pin_usb_model(0x1234, 0x0213, &d) == PIN_ERR_NOT_FOUND, "other vendor");
    CHECK(pin_usb_model(0x2304, 0x0213, NULL) == PIN_ERR_ARG, "NULL out");
    CHECK(pin_device_fd_problem(NULL, b, sizeof(b)) == 0 && b[0] == 0, "fd problem NULL");

    /* a caller compiled before hub_depth: nothing written past its size */
    unsigned char buf[sizeof(pin_device_info_t) + 16];
    memset(buf, 0xAA, sizeof(buf));
    pin_device_info_t *old = (pin_device_info_t *)(void *)buf;
    old->size = (uint32_t)offsetof(pin_device_info_t, hub_depth);
    CHECK(pin_usb_model(0x2304, 0x0213, old) == PIN_OK, "old size");
    int untouched = 1;
    for (size_t i = offsetof(pin_device_info_t, hub_depth); i < sizeof(buf); i++)
        if (buf[i] != 0xAA)
            untouched = 0;
    CHECK(untouched && strcmp(old->name, "Pinnacle Studio 500-USB") == 0, "old layout respected");
}

static void test_open_fd(void)
{
    static const char *const bad[] = { "fd:", "fd:abc", "fd:-1", "fd: 3", "fd:3x", "fd:99999999999" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        pin_session_t *s = NULL;
        CHECK(pin_open(bad[i], &s) == PIN_ERR_ARG && s == NULL, bad[i]);
    }
#if !defined(_WIN32)
    /* a descriptor that was open once and is closed now */
    int fd = open("/dev/null", O_RDONLY);
    CHECK(fd >= 0, "open /dev/null");
    close(fd);
    char id[32];
    snprintf(id, sizeof(id), "fd:%d", fd);
    pin_session_t *s = NULL;
    CHECK(pin_open(id, &s) == PIN_ERR_ARG && s == NULL, "closed fd");

#if defined(__linux__)
    /* an open descriptor that is no USB device: pin_open() takes it (the device is opened
     * by the bring-up), the bring-up then fails cleanly */
    fd = open("/dev/null", O_RDWR);
    CHECK(fd >= 0, "open /dev/null rw");
    snprintf(id, sizeof(id), "fd:%d", fd);
    s = NULL;
    CHECK(pin_open(id, &s) == PIN_OK && s, "open fd session");
    if (s) {
        CHECK(pin_set_input(s, PIN_INPUT_DV) == PIN_OK, "set input");
        pin_status_snapshot_t st;
        int state = PIN_STATE_CLOSED;
        for (int i = 0; i < 200 && state != PIN_STATE_ERROR; i++) {
            memset(&st, 0, sizeof(st));
            st.size = sizeof(st);
            if (pin_get_status(s, &st) == PIN_OK)
                state = st.state;
            if (state != PIN_STATE_ERROR)
                sleep_ms(25);
        }
        CHECK(state == PIN_STATE_ERROR && st.last_error != PIN_OK && st.error_text[0],
              "not a USB device: error state");
        pin_close(s);
    }
    CHECK(fcntl(fd, F_GETFD) != -1, "the core does not close the caller's descriptor");
    close(fd);
#else
    /* macOS: libusb cannot wrap a descriptor there */
    fd = open("/dev/null", O_RDWR);
    snprintf(id, sizeof(id), "fd:%d", fd);
    s = NULL;
    CHECK(pin_open(id, &s) == PIN_ERR_ARG && s == NULL, "fd unsupported here");
    close(fd);
#endif
#else
    pin_session_t *s = NULL;
    CHECK(pin_open("fd:3", &s) == PIN_ERR_ARG && s == NULL, "fd unsupported on Windows");
#endif
}

static void test_app_dirs(void)
{
    char base[512];
    const char *tmp = getenv("TMPDIR");
#if defined(_WIN32)
    if (!tmp || !tmp[0])
        tmp = getenv("TEMP");
#endif
    snprintf(base, sizeof(base), "%s/marvin-appdirs-%ld", tmp && tmp[0] ? tmp : "/tmp", (long)time(NULL));
    char cfg[600], run[600], tdir[600], ini[700];
    snprintf(cfg, sizeof(cfg), "%s/config/sub", base);   /* created by the core */
    snprintf(run, sizeof(run), "%s/run", base);
    snprintf(tdir, sizeof(tdir), "%s/tmp", base);
#if defined(_WIN32)
    snprintf(ini, sizeof(ini), "%s\\settings.ini", cfg);  /* joined with the native separator */
#else
    snprintf(ini, sizeof(ini), "%s/settings.ini", cfg);
#endif

    char before[1024] = "";
    pin_settings_default_path(before, sizeof(before));

    char longp[600];
    memset(longp, 'x', sizeof(longp) - 1);
    longp[sizeof(longp) - 1] = 0;
    CHECK(pin_set_app_dirs(longp, NULL, NULL) == PIN_ERR_ARG, "too long");
    char now[1024] = "";
    pin_settings_default_path(now, sizeof(now));
    CHECK(strcmp(now, before) == 0, "nothing changed by a refused call");

    CHECK(pin_set_app_dirs(cfg, run, tdir) == PIN_OK, "set app dirs");
    pin_settings_default_path(now, sizeof(now));
    CHECK(strcmp(now, ini) == 0, "settings path in config_dir");
    CHECK(pin_settings_set("test.app_dirs", "yes") == PIN_OK, "settings write");
    CHECK(exists(ini), "settings.ini in config_dir");
    char v[64] = "";
    CHECK(pin_settings_get("test.app_dirs", v, sizeof(v)) == PIN_OK && strcmp(v, "yes") == 0, "read back");

#if !defined(_WIN32)
    /* lock files in runtime_dir */
    pinnacle_lock_t *lk = NULL;
    CHECK(pinnacle_lock_acquire("usb:99-9", &lk) == PINNACLE_OK && lk, "lock in runtime_dir");
    char lockf[700];
    snprintf(lockf, sizeof(lockf), "%s/usb_99-9.lock", run);
    CHECK(exists(lockf), "lock file in runtime_dir");
    pinnacle_lock_release(lk);
#endif

    /* HDV MOV's remux temp file in temp_dir, not next to the output */
#if defined(_WIN32)
    mkdir(tdir);
#else
    mkdir(tdir, 0700);
#endif
    char out[700], raw[800], beside[800];
    snprintf(out, sizeof(out), "%s/clip-0001.mov", base);
    snprintf(raw, sizeof(raw), "%s/clip-0001.mov.rawts.tmp", tdir);
    snprintf(beside, sizeof(beside), "%s.rawts.tmp", out);
    pin_sink_t *sk = pin_sink_create(PIN_FMT_HDV_MOV);
    CHECK(sk != NULL, "HDV MOV sink");
    if (sk) {
        pin_sink_params_t p;
        memset(&p, 0, sizeof(p));
        p.kind = PIN_KIND_HDV;
        p.width = 1440;
        p.height = 1080;
        p.fps_num = 25;
        p.fps_den = 1;
        p.aspect = PIN_ASPECT_16_9;
        p.audio_rate = 48000;
        p.audio_channels = 2;
        CHECK(sk->open(sk, out, &p) == PIN_OK, "HDV MOV open");
        CHECK(exists(raw) && !exists(beside), "temp file in temp_dir");
        sk->close(sk);       /* nothing written: no output, temp file removed */
        CHECK(!exists(raw) && !exists(out), "temp file removed");
    }

    /* back to the defaults */
    CHECK(pin_set_app_dirs(NULL, "", NULL) == PIN_OK, "restore");
    pin_settings_default_path(now, sizeof(now));
    CHECK(strcmp(now, before) == 0, "settings path restored");

    /* clean up (the lock file may stay behind after release) */
    remove(ini);
    char d[700];
    snprintf(d, sizeof(d), "%s/config", base);
    rmdir(cfg);
    rmdir(d);
#if !defined(_WIN32)
    remove(lockf);
#endif
    rmdir(run);
    rmdir(tdir);
    rmdir(base);
}

static void test_formats(void)
{
    pin_format_info_t f[PIN_FMT_COUNT];
    for (int i = 0; i < PIN_FMT_COUNT; i++) {
        memset(&f[i], 0, sizeof(f[i]));
        f[i].size = sizeof(f[i]);
    }
    int n = pin_formats(PIN_KIND_HDV, f, PIN_FMT_COUNT);
#if defined(__ANDROID__)
    CHECK(n == 1 && f[0].format == PIN_FMT_HDV_TS && f[0].is_default, "Android: HDV raw TS only");
#else
    CHECK(n == 3 && f[0].format == PIN_FMT_HDV_TS && f[0].is_default, "desktop: all HDV formats");
#endif
    pin_format_info_t fi;
    memset(&fi, 0, sizeof(fi));
    fi.size = sizeof(fi);
    CHECK(pin_format_info(PIN_FMT_HDV_MOV, &fi) == PIN_OK && strcmp(fi.extension, "mov") == 0,
          "format info answers for every format");

    char w[512];
    for (int i = 0; i < PIN_FMT_COUNT; i++) {
        memset(w, 'x', sizeof(w));
        pin_format_warning((pin_format_t)i, w, sizeof(w));
        int lossy = i == PIN_FMT_ANALOG_H264_MKV || i == PIN_FMT_ANALOG_HEVC_MKV ||
                    i == PIN_FMT_DV_H264_MKV || i == PIN_FMT_DV_HEVC_MKV;
        if (lossy)
            CHECK(strstr(w, "lossy") != NULL, "every platform: H.264 / HEVC are lossy");
#if defined(__ANDROID__)
        else if (i == PIN_FMT_ANALOG_AVI)
            CHECK(strstr(w, "75 GB") != NULL, "Android: uncompressed AVI warning");
        else
            CHECK(w[0] == 0, "Android: no warning");
#else
        else
            CHECK(w[0] == 0, "desktop: no other format warnings");
#endif
    }
    pin_format_warning(PIN_FMT_ANALOG_AVI, NULL, 0);    /* no crash */
}

static int listed(pin_kind_t kind, pin_format_t f)
{
    pin_format_info_t l[PIN_FMT_COUNT];
    for (int i = 0; i < PIN_FMT_COUNT; i++) {
        memset(&l[i], 0, sizeof(l[i]));
        l[i].size = sizeof(l[i]);
    }
    int n = pin_formats(kind, l, PIN_FMT_COUNT);
    for (int i = 0; i < n; i++)
        if (l[i].format == f) {
            CHECK(!l[i].is_default, "an H.264 / HEVC format is never the default");
            return 1;
        }
    return 0;
}

/* The H.264 / HEVC formats: listed only where the encoder probe found them. Linux has no
 * encoder; PIN_HWENC_ENCODER (tests only) makes FFV1 stand in for it. */
static void test_hwenc_formats(void)
{
    pin_format_info_t fi;
    memset(&fi, 0, sizeof(fi));
    fi.size = sizeof(fi);
    CHECK(pin_format_info(PIN_FMT_DV_HEVC_MKV, &fi) == PIN_OK && fi.kind == PIN_KIND_DV &&
          strcmp(fi.extension, "mkv") == 0 && fi.supports_title && fi.supports_scene_split &&
          fi.supports_multi_pass && !fi.is_default, "DV HEVC: like the other DV formats");
    CHECK(strstr(fi.label, "HEVC") != NULL && strstr(fi.label, "hardware encoder") != NULL, "label");
    CHECK(pin_format_info(PIN_FMT_ANALOG_H264_MKV, &fi) == PIN_OK && fi.kind == PIN_KIND_ANALOG &&
          strcmp(fi.extension, "mkv") == 0 && fi.supports_title && !fi.supports_scene_split &&
          !fi.supports_multi_pass && !fi.is_default, "analog H.264");

#if defined(__linux__) && !defined(__ANDROID__)
    CHECK(pin_encoders_probe() == 0, "Linux: no H.264 / HEVC encoder");
    CHECK(!listed(PIN_KIND_ANALOG, PIN_FMT_ANALOG_H264_MKV) && !listed(PIN_KIND_DV, PIN_FMT_DV_HEVC_MKV),
          "Linux: not listed");
    CHECK(listed(PIN_KIND_ANALOG, PIN_FMT_ANALOG_FFV1_MKV), "the others still are");
    setenv("PIN_HWENC_ENCODER", "ffv1", 1);
    CHECK(pin_encoders_probe() == (PIN_ENC_H264 | PIN_ENC_HEVC), "the stand-in opens for both");
    CHECK(listed(PIN_KIND_ANALOG, PIN_FMT_ANALOG_H264_MKV) && listed(PIN_KIND_ANALOG, PIN_FMT_ANALOG_HEVC_MKV) &&
          listed(PIN_KIND_DV, PIN_FMT_DV_H264_MKV) && listed(PIN_KIND_DV, PIN_FMT_DV_HEVC_MKV),
          "listed once the probe finds an encoder");
    CHECK(!listed(PIN_KIND_HDV, PIN_FMT_DV_H264_MKV), "not for HDV");
    setenv("PIN_HWENC_ENCODER", "no-such-encoder", 1);
    CHECK(pin_encoders_probe() == 0 && !listed(PIN_KIND_ANALOG, PIN_FMT_ANALOG_H264_MKV),
          "an encoder that is not there is not listed");
    unsetenv("PIN_HWENC_ENCODER");
    CHECK(pin_encoders_probe() == 0, "back to none");
#else
    int m = pin_encoders_probe();
    CHECK(listed(PIN_KIND_ANALOG, PIN_FMT_ANALOG_H264_MKV) == ((m & PIN_ENC_H264) != 0) &&
          listed(PIN_KIND_DV, PIN_FMT_DV_HEVC_MKV) == ((m & PIN_ENC_HEVC) != 0), "listed as probed");
    printf("encoders here: H.264 %s, HEVC %s\n", m & PIN_ENC_H264 ? "yes" : "no", m & PIN_ENC_HEVC ? "yes" : "no");
#endif
    CHECK(pin_encoders_probe() == pin_encoders_probe(), "cached");
}

/* pin_format_options() and the appended pin_capture_opts_t / pin_script_settings_t fields. */
static void test_format_settings(void)
{
    pin_format_options_t o;
    memset(&o, 0, sizeof(o));
    o.size = sizeof(o) - 4;
    CHECK(pin_format_options(PIN_FMT_ANALOG_H264_MKV, &o) == PIN_ERR_ABI, "a size it does not know");
    o.size = sizeof(o);
    CHECK(pin_format_options(PIN_FMT_ANALOG_H264_MKV, &o) == PIN_OK && o.has_quality && o.has_deinterlace &&
          !o.has_ffv1_compression && o.quality_default == 18, "H.264 options");
    CHECK(pin_format_options(PIN_FMT_ANALOG_FFV1_MKV, &o) == PIN_OK && !o.has_quality &&
          o.has_ffv1_compression, "FFV1 options");
    CHECK(pin_format_options(PIN_FMT_COUNT, &o) == PIN_ERR_ARG, "unknown format");
    char h[128];
    pin_format_quality_hint(PIN_FMT_DV_H264_MKV, 18, PIN_DEINTERLACE_DOUBLE_RATE, 0, h, sizeof(h));
    CHECK(strncmp(h, "About ", 6) == 0, "quality hint");

    pin_capture_opts_t d;
    memset(&d, 0, sizeof(d));
    pin_capture_opts_defaults(&d);
    CHECK(d.size == sizeof(d) && d.deinterlace == PIN_DEINTERLACE_DOUBLE_RATE && d.video_quality == 18 &&
          d.ffv1_compression == PIN_FFV1_FAST && d.format_analog == PIN_FMT_ANALOG_AVI, "defaults");

    /* a caller compiled before the format settings: only its own bytes are written */
    const size_t old = offsetof(pin_capture_opts_t, deinterlace);
    unsigned char buf[sizeof(pin_capture_opts_t) + 16];
    memset(buf, 0xAA, sizeof(buf));
    pin_capture_opts_t *oldo = (pin_capture_opts_t *)buf;
    oldo->size = (uint32_t)old;
    pin_capture_opts_defaults(oldo);
    CHECK(oldo->size == old && oldo->passes == 1 && oldo->format_dv == PIN_FMT_DV_RAW, "old size: filled");
    int untouched = 1;
    for (size_t i = old; i < sizeof(buf); i++)
        if (buf[i] != 0xAA)
            untouched = 0;
    CHECK(untouched, "old size: nothing written past it");

    pin_script_t *sc = NULL;
    char err[256];
    const char *argv[] = { "-i", "svideo", "--quality", "23", "--deinterlace", "no",
                           "--ffv1-compression", "smallest", "--format", "hevc-mkv" };
    CHECK(pin_script_parse(10, argv, &sc, err, sizeof(err)) == PIN_OK, "parse");
    pin_script_settings_t ss;
    memset(&ss, 0, sizeof(ss));
    ss.size = sizeof(ss);
    CHECK(pin_script_settings(sc, &ss) == PIN_OK && ss.has_video_quality && ss.video_quality == 23 &&
          ss.has_deinterlace && ss.deinterlace == PIN_DEINTERLACE_KEEP_FIELDS && ss.has_ffv1_compression &&
          ss.ffv1_compression == PIN_FFV1_SMALLEST && ss.has_format_analog &&
          ss.format_analog == PIN_FMT_ANALOG_HEVC_MKV, "script settings");
    const size_t old_ss = offsetof(pin_script_settings_t, has_deinterlace);
    unsigned char sbuf[sizeof(pin_script_settings_t) + 16];
    memset(sbuf, 0xAA, sizeof(sbuf));
    pin_script_settings_t *olds = (pin_script_settings_t *)sbuf;
    olds->size = (uint32_t)old_ss;
    CHECK(pin_script_settings(sc, olds) == PIN_OK && olds->size == old_ss && olds->has_input, "old size accepted");
    untouched = 1;
    for (size_t i = old_ss; i < sizeof(sbuf); i++)
        if (sbuf[i] != 0xAA)
            untouched = 0;
    CHECK(untouched, "script settings, old size: nothing written past it");
    olds->size = 7;
    CHECK(pin_script_settings(sc, olds) == PIN_ERR_ABI, "a size it does not know");
    pin_script_free(sc);
}

static void test_keep_busy(void)
{
    pin_set_cpu_keep_busy(PIN_KEEP_BUSY_ON);
    CHECK(pin_thread_get_keep_busy() == PIN_KEEP_BUSY_ON, "on");
    pin_set_cpu_keep_busy(PIN_KEEP_BUSY_OFF);
    CHECK(pin_thread_get_keep_busy() == PIN_KEEP_BUSY_OFF, "off");
    pin_set_cpu_keep_busy(7);
    pin_set_cpu_keep_busy(-1);
    CHECK(pin_thread_get_keep_busy() == PIN_KEEP_BUSY_OFF, "other values ignored");
    pin_set_cpu_keep_busy(PIN_KEEP_BUSY_AUTO);
    CHECK(pin_thread_get_keep_busy() == PIN_KEEP_BUSY_AUTO, "auto");

#if defined(__linux__)
    /* what the read loops do with it */
    pin_thread_boost_t b;
    pin_set_cpu_keep_busy(PIN_KEEP_BUSY_ON);
    pin_thread_boost(&b);
    CHECK(b.busy == 1, "on: a core is kept busy");
    pin_thread_unboost(&b);
    CHECK(b.busy == 0, "on: stopped again");
    pin_set_cpu_keep_busy(PIN_KEEP_BUSY_OFF);
    pin_thread_boost(&b);
    CHECK(b.busy == 0, "off: never");
    pin_thread_unboost(&b);
    pin_set_cpu_keep_busy(PIN_KEEP_BUSY_AUTO);
    pin_thread_boost(&b);
    CHECK(b.busy == (b.pmqos_fd < 0), "auto: only without the latency request");
    pin_thread_unboost(&b);
#endif
}

/* The front-end driven update check: weekly rule, what is remembered (needs its own config dir). */
static void test_update_state(void)
{
    char base[512], cfg[600];
    const char *tmp = getenv("TMPDIR");
#if defined(_WIN32)
    if (!tmp || !tmp[0])
        tmp = getenv("TEMP");
#endif
    snprintf(base, sizeof(base), "%s/marvin-update-%ld", tmp && tmp[0] ? tmp : "/tmp", (long)time(NULL));
    snprintf(cfg, sizeof(cfg), "%s/config", base);
    CHECK(pin_set_app_dirs(cfg, NULL, NULL) == PIN_OK, "update: app dirs");

    const char *ver = pin_app_version();
    char newer[64], same[64], file[256];
    snprintf(newer, sizeof(newer), "%s.1", ver);   /* "1.0" -> "1.0.1": newer */
    snprintf(same, sizeof(same), "%s", ver);
    const int64_t t0 = 1800000000, week = 7 * 24 * 60 * 60;
    pin_update_info_t u;
    memset(&u, 0, sizeof(u));
    u.size = sizeof(u);

    CHECK(strncmp(pin_update_url(), "https://", 8) == 0 || getenv("MARVIN_UPDATE_URL"), "default address");
    CHECK(pin_update_due(t0) == 1, "never checked: due");
    CHECK(pin_update_known(&u) == 0 && u.available == 0, "nothing known yet");

    snprintf(file, sizeof(file), "{\"version\": \"%s\", \"download_url\": \"https://example.com/get\"}", newer);
    CHECK(pin_update_check_json("junk", 4, t0, &u) == PIN_ERR_IO, "invalid file rejected");
    CHECK(pin_update_due(t0) == 1 && pin_update_known(&u) == 0, "invalid file remembers nothing");
    CHECK(pin_update_check_json(file, strlen(file), t0, &u) == PIN_OK && u.available == 1, "newer found");
    CHECK(pin_update_due(t0 + 60) == 0, "not due right after");
    CHECK(pin_update_due(t0 + week - 1) == 0, "not due within the week");
    CHECK(pin_update_due(t0 + week) == 1, "due after a week");
    CHECK(pin_update_due(t0 - 3600) == 1, "due when the clock went back");

    memset(&u, 0, sizeof(u));
    u.size = sizeof(u);
    CHECK(pin_update_known(&u) == 1 && u.available == 1, "remembered");
    CHECK(strcmp(u.latest, newer) == 0 && strcmp(u.current, ver) == 0, "remembered versions");
    CHECK(strcmp(u.download_url, "https://example.com/get") == 0, "remembered address");
    char text[256];
    pin_format_update(&u, text, sizeof(text));
    CHECK(strstr(text, newer) != NULL && strstr(text, "is available") != NULL, "notice text");

    /* a later check finds nothing newer (the app was updated, or the release was withdrawn) */
    snprintf(file, sizeof(file), "{\"version\": \"%s\"}", same);
    CHECK(pin_update_check_json(file, strlen(file), t0 + week, &u) == PIN_OK && u.available == 0, "same: not available");
    CHECK(pin_update_known(&u) == 0, "notice gone");
    CHECK(pin_update_due(t0 + week + 60) == 0, "the second check counted too");

    CHECK(pin_update_check_json(NULL, 0, t0, &u) == PIN_ERR_ARG, "no json");
    u.size = 4;
    CHECK(pin_update_check_json(file, strlen(file), t0, &u) == PIN_ERR_ABI, "small struct");
    CHECK(pin_set_app_dirs(NULL, "", NULL) == PIN_OK, "update: restore app dirs");
}

int main(void)
{
    test_update_state();
    test_usb_model();
    test_open_fd();
    test_app_dirs();
    test_formats();
    test_hwenc_formats();
    test_format_settings();
    test_keep_busy();
    if (g_failures) {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
