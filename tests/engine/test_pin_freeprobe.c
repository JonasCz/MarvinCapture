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

/* The free-space probe: a lookup that hangs (a network share that has gone away) must not block
 * the caller, runs once at a time, and must not keep the probe from being released. */

#include "pin_freeprobe.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int g_failures = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_hang, g_calls;
static uint64_t g_value = 1000;

static int fake_lookup(const char *dir, uint64_t *free_bytes, int *fat32)
{
    (void)dir;
    pthread_mutex_lock(&g_mu);
    g_calls++;
    while (g_hang)
        pthread_cond_wait(&g_cv, &g_mu);
    *free_bytes = g_value;
    *fat32 = 1;
    pthread_mutex_unlock(&g_mu);
    return 0;
}

static void set_hang(int on)
{
    pthread_mutex_lock(&g_mu);
    g_hang = on;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void)
{
    uint64_t b = 0;
    int known = 0, fat = 0;

    /* a first lookup: no answer yet, then one */
    pin_freeprobe_t *p = pin_freeprobe_create(fake_lookup);
    CHECK(p != NULL, "create");
    CHECK(pin_freeprobe_wait_idle(p, 10), "idle before any lookup");
    pin_freeprobe_get(p, "/a", 1.0, &b, &known, &fat);
    CHECK(pin_freeprobe_wait_idle(p, 2000), "the lookup finishes");
    CHECK(pin_freeprobe_get(p, "/a", 1.0, &b, &known, &fat) == 1 && known && b == 1000 && fat == 1,
          "result for the folder");
    CHECK(g_calls == 1, "fresh result is reused");

    /* a result for another folder is not reused */
    CHECK(pin_freeprobe_get(p, "/b", 1.0, &b, &known, &fat) == 0, "no result for another folder yet");
    CHECK(pin_freeprobe_wait_idle(p, 2000), "second folder looked up");
    CHECK(pin_freeprobe_get(p, "/b", 1.0, &b, &known, &fat) == 1 && g_calls == 2, "second folder result");

    /* a hanging lookup: the caller is not held, only one runs, the old value stays readable */
    set_hang(1);
    g_value = 2000;
    double t0 = now_s();
    CHECK(pin_freeprobe_get(p, "/b", 0.0, &b, &known, &fat) == 1 && b == 1000, "stale value while it hangs");
    for (int i = 0; i < 5; i++)
        pin_freeprobe_get(p, "/b", 0.0, &b, &known, &fat);
    CHECK(now_s() - t0 < 0.5, "callers are not blocked");
    CHECK(!pin_freeprobe_wait_idle(p, 50), "still busy");
    pthread_mutex_lock(&g_mu);
    int calls = g_calls;
    pthread_mutex_unlock(&g_mu);
    CHECK(calls == 3, "one lookup in flight");

    /* releasing the probe while the lookup hangs does not wait for it */
    t0 = now_s();
    pin_freeprobe_release(p);
    CHECK(now_s() - t0 < 0.5, "release does not wait");
    set_hang(0);
    struct timespec nap = { 0, 100 * 1000 * 1000 };
    nanosleep(&nap, NULL);   /* the worker finishes and frees the probe (ASan / valgrind see a leak otherwise) */

    if (g_failures == 0) printf("test_pin_freeprobe: ok\n");
    return g_failures;
}
