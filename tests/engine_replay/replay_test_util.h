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
 * Shared helpers for tests/engine_replay: hardware-free ctest coverage of
 * the session engine (pin_session/pin_deck/pin_preview) driven entirely
 * through pin_api.h against the replay (virtual device) source, exactly as
 * a real GUI or MarvinCaptureCLI would use it -- no libusb, no device, no rig.
 *
 * Every test in this directory links only marvin-core (the shared
 * pin_api.h library) plus whatever else it specifically needs (libavformat
 * to probe a muxed output, pinnacle_engine_pure to build a synthetic DV
 * frame for the scene-split test). None of them talk to src/engine's
 * internals directly.
 */

#ifndef PIN_REPLAY_TEST_UTIL_H
#define PIN_REPLAY_TEST_UTIL_H

#include "pin_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1); \
        } \
    } while (0)

#define CHECK_EQ_I(a, b) \
    do { \
        long long _a = (long long)(a), _b = (long long)(b); \
        if (_a != _b) { \
            fprintf(stderr, "%s:%d: CHECK_EQ failed: %s (%lld) != %s (%lld)\n", \
                    __FILE__, __LINE__, #a, _a, #b, _b); \
            exit(1); \
        } \
    } while (0)

#if defined(_WIN32)
#include <windows.h>
static inline void pin_test_sleep_ms(int ms) { Sleep((DWORD)ms); }
static inline void pin_test_setenv(const char *name, const char *value)
{
    _putenv_s(name, value);
}
#else
#include <time.h>
#include <stdlib.h>
static inline void pin_test_sleep_ms(int ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
static inline void pin_test_setenv(const char *name, const char *value)
{
    setenv(name, value, 1);
}
#endif

static inline int pin_test_file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

/* Points PIN_REPLAY at `trace_path` and opens a session on it (pin_open with
 * device_id NULL falls back to the env var, see pin_session_open's
 * resolve_device_id()). Aborts the test on failure. */
static inline pin_session_t *pin_test_open_replay(const char *trace_path)
{
    pin_test_setenv("PIN_REPLAY", trace_path);
    pin_session_t *s = NULL;
    pin_status_t st = pin_open(NULL, &s);
    if (st != PIN_OK) {
        fprintf(stderr, "pin_open(replay:%s) failed: %s\n", trace_path, pin_strerror(st));
        exit(1);
    }
    return s;
}

/* Polls pin_get_status() until it matches one of the two target states (a
 * second target of -1 to only look for one) or timeout_ms elapses; fills
 * *out with the last snapshot read. Returns the state reached. */
static inline pin_state_t pin_test_wait_state(pin_session_t *s, pin_state_t target_a, int target_b,
                                        int timeout_ms, pin_status_snapshot_t *out)
{
    pin_status_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.size = sizeof(snap);
    int waited = 0;
    for (;;) {
        pin_get_status(s, &snap);
        if (snap.state == target_a || (target_b >= 0 && (int)snap.state == target_b))
            break;
        if (waited >= timeout_ms)
            break;
        pin_test_sleep_ms(20);
        waited += 20;
    }
    if (out)
        *out = snap;
    return snap.state;
}

/* Brings a freshly opened replay session up on the DV input and waits for
 * READY (or ERROR, which the caller should check for). */
static inline void pin_test_prepare_dv(pin_session_t *s)
{
    pin_set_input(s, PIN_INPUT_DV);
    pin_status_snapshot_t snap;
    pin_test_wait_state(s, PIN_STATE_READY, PIN_STATE_ERROR, 5000, &snap);
    CHECK(snap.state == PIN_STATE_READY);
}

#endif /* PIN_REPLAY_TEST_UTIL_H */
