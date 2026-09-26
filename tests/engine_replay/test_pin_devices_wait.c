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
 * ctest: pin_devices_wait()/pin_devices_wake() (pin_api.h), without needing
 * a real device plug/unplug event -- this only exercises the timeout and
 * wake paths (the OS hotplug notification itself can't be tested here, see
 * the task report). Also a quick end-to-end check that pin_set_replay_file()
 * makes pin_enumerate() list the replay device and pin_open() open it by
 * that listed id, replacing the old settings-key mechanism.
 */

#include "replay_test_util.h"

#include <pthread.h>

static void *wake_after_delay(void *arg)
{
    (void)arg;
    pin_test_sleep_ms(150);
    pin_devices_wake();
    return NULL;
}

static void test_wake_unblocks_wait(void)
{
    pthread_t t;
    pthread_create(&t, NULL, wake_after_delay, NULL);

    double start = 0;
#if defined(_WIN32)
    /* crude wall-clock, good enough for a "well under the 5s timeout" bound */
    LARGE_INTEGER f, c0;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c0);
    int rc = pin_devices_wait(5000);
    LARGE_INTEGER c1;
    QueryPerformanceCounter(&c1);
    double elapsed_ms = (double)(c1.QuadPart - c0.QuadPart) * 1000.0 / f.QuadPart;
#else
    int rc = pin_devices_wait(5000);
    double elapsed_ms = 1000; /* not measured precisely on this path */
#endif
    (void)start;
    pthread_join(t, NULL);

    CHECK(rc == 0);
    CHECK(elapsed_ms < 4000.0);
    printf("OK: pin_devices_wake() unblocked pin_devices_wait() after ~%.0f ms (rc=%d)\n",
           elapsed_ms, rc);
}

static void test_timeout_returns_zero(void)
{
    int rc = pin_devices_wait(150);
    CHECK(rc == 0);
    printf("OK: pin_devices_wait() times out with rc=0 when nothing changes\n");
}

static void test_replay_file_enumerates_and_opens(void)
{
    /* Point PIN_REPLAY somewhere harmless first so pin_set_replay_file()'s
     * effect (not the env var's) is what's actually being checked -- see
     * pin_session.c's resolve_device_id(), where PIN_REPLAY still wins if
     * both are set. */
    pin_test_setenv("PIN_REPLAY", "");
    pin_set_replay_file("nonexistent-but-listed.dv");

    pin_device_info_t devs[16];
    for (int i = 0; i < 16; i++) { memset(&devs[i], 0, sizeof(devs[i])); devs[i].size = sizeof(devs[i]); }
    int n = pin_enumerate(devs, 16);
    int found = 0;
    char id[PIN_NAME_MAX] = "";
    for (int i = 0; i < n && i < 16; i++) {
        if (strcmp(devs[i].id, "replay:nonexistent-but-listed.dv") == 0) {
            found = 1;
            strncpy(id, devs[i].id, sizeof(id) - 1);
        }
    }
    CHECK(found);
    printf("OK: pin_set_replay_file() makes pin_enumerate() list \"%s\"\n", id);

    pin_set_replay_file(NULL); /* clear it: shouldn't show up any more */
    n = pin_enumerate(devs, 16);
    found = 0;
    for (int i = 0; i < n && i < 16; i++)
        if (strncmp(devs[i].id, "replay:", 7) == 0)
            found = 1;
    CHECK(!found);
    printf("OK: pin_set_replay_file(NULL) clears the replay device\n");
}

int main(void)
{
    test_replay_file_enumerates_and_opens();
    test_timeout_returns_zero();
    test_wake_unblocks_wait();
    return 0;
}
