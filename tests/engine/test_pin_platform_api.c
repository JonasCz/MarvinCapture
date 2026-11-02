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
 * list and warnings, and the keep-busy mode. Hardware-free; links src/api/pin_api.c. */

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

static void sleep_ms(int ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

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
    snprintf(ini, sizeof(ini), "%s/settings.ini", cfg);

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
#if defined(__ANDROID__)
        if (i == PIN_FMT_ANALOG_AVI)
            CHECK(strstr(w, "75 GB") != NULL, "Android: uncompressed AVI warning");
        else
            CHECK(w[0] == 0, "Android: no warning");
#else
        CHECK(w[0] == 0, "desktop: no format warnings");
#endif
    }
    pin_format_warning(PIN_FMT_ANALOG_AVI, NULL, 0);    /* no crash */
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

int main(void)
{
    test_usb_model();
    test_open_fd();
    test_app_dirs();
    test_formats();
    test_keep_busy();
    if (g_failures) {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
