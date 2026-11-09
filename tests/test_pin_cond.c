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

/* pin_cond.h: the timed wait lasts about the requested time on the monotonic clock (never
 * returns ETIMEDOUT early, which the encoder sinks' 10 s watchdogs rely on) and wakes on a
 * signal. */

#include "pin_cond.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond, msg)                                                          \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);         \
            exit(1);                                                              \
        }                                                                         \
    } while (0)

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv;
static int flag;

static double elapsed_ms(long long t0)
{
    return (double)(pin_cond_mono_ns() - t0) / 1e6;
}

static void *signaller(void *arg)
{
    (void)arg;
    struct timespec ts = { 0, 80 * 1000000L };
    nanosleep(&ts, NULL);
    pthread_mutex_lock(&mu);
    flag = 1;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
    return NULL;
}

int main(void)
{
    pin_cond_init_timed(&cv);

    /* No signal: ETIMEDOUT, and not before the time is up. */
    pthread_mutex_lock(&mu);
    long long t0 = pin_cond_mono_ns();
    int rc;
    do
        rc = pin_cond_timedwait_ms(&cv, &mu, 250);
    while (rc == 0);  /* a spurious wake-up is allowed; the caller loops */
    double ms = elapsed_ms(t0);
    pthread_mutex_unlock(&mu);
    CHECK(rc == ETIMEDOUT, "an unsignalled wait ends with ETIMEDOUT");
    CHECK(ms >= 249.0, "ETIMEDOUT never comes before the requested time");
    CHECK(ms < 2000.0, "the wait does not run far past the requested time");
    printf("ok: 250 ms wait took %.1f ms\n", ms);

    /* Zero and negative times: no wait at all. */
    pthread_mutex_lock(&mu);
    t0 = pin_cond_mono_ns();
    CHECK(pin_cond_timedwait_ms(&cv, &mu, 0) == ETIMEDOUT, "0 ms is an immediate timeout");
    CHECK(pin_cond_timedwait_ms(&cv, &mu, -5) == ETIMEDOUT, "a negative time is an immediate timeout");
    pthread_mutex_unlock(&mu);
    CHECK(elapsed_ms(t0) < 100.0, "0 ms does not block");

    /* Seconds variant. */
    pthread_mutex_lock(&mu);
    t0 = pin_cond_mono_ns();
    do
        rc = pin_cond_timedwait(&cv, &mu, 1);
    while (rc == 0);
    ms = elapsed_ms(t0);
    pthread_mutex_unlock(&mu);
    CHECK(rc == ETIMEDOUT && ms >= 999.0 && ms < 3000.0, "1 s wait lasts about a second");
    printf("ok: 1 s wait took %.1f ms\n", ms);

    /* A signal ends the wait well before the timeout. */
    pthread_t th;
    CHECK(pthread_create(&th, NULL, signaller, NULL) == 0, "start the signalling thread");
    pthread_mutex_lock(&mu);
    t0 = pin_cond_mono_ns();
    rc = 0;
    while (!flag && rc == 0)
        rc = pin_cond_timedwait_ms(&cv, &mu, 5000);
    ms = elapsed_ms(t0);
    pthread_mutex_unlock(&mu);
    pthread_join(th, NULL);
    CHECK(flag && rc == 0, "the signal woke the waiter");
    CHECK(ms < 2500.0, "woken long before the 5 s timeout");
    printf("ok: woken by a signal after %.1f ms\n", ms);

    pthread_cond_destroy(&cv);
    printf("all pin_cond tests passed\n");
    return 0;
}
