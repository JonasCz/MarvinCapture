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

/* Condition variables whose timed wait is measured on the monotonic clock. */

#ifndef PIN_COND_H
#define PIN_COND_H

#include <errno.h>
#include <pthread.h>
#include <time.h>

/* pin_cond_timedwait_ms() returns ETIMEDOUT only once the requested time has really passed on
 * the monotonic clock: every single platform wait is followed by a check of that clock, and a
 * wait that ended early (a wall-clock step, a platform that ignores the clock attribute) is
 * resumed for the rest. So a caller's watchdog can never fire at once on a timeout that did not
 * happen, and a wall-clock step cannot end a wait early.
 *
 * How each platform waits for one slice:
 *   macOS    pthread_cond_timedwait_relative_np(): relative, so no clock is involved.
 *   Windows  a wall-clock deadline on a plain condition variable. winpthreads turns the
 *            deadline into a relative wait when it is called, so a later step of the wall
 *            clock does not stretch the wait either. Its support for
 *            pthread_condattr_setclock() differs between versions, so it is not relied on.
 *   others   a monotonic deadline on a variable set to CLOCK_MONOTONIC (glibc, bionic). If
 *            setting the clock ever fails, the variable keeps the wall clock and the deadline
 *            is computed on that clock instead. */
#if !defined(__APPLE__) && !defined(_WIN32)
static inline int *pin_cond_mono_flag(void)
{
    static int ok = 1;      /* per translation unit; init and wait are in the same one */
    return &ok;
}
#endif

static inline void pin_cond_init_timed(pthread_cond_t *c)
{
#if !defined(__APPLE__) && !defined(_WIN32)
    pthread_condattr_t a;
    if (pthread_condattr_init(&a) == 0) {
        if (pthread_condattr_setclock(&a, CLOCK_MONOTONIC) == 0 && pthread_cond_init(c, &a) == 0) {
            pthread_condattr_destroy(&a);
            return;
        }
        pthread_condattr_destroy(&a);
    }
    *pin_cond_mono_flag() = 0;
#endif
    pthread_cond_init(c, NULL);
}

static inline long long pin_cond_mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* One platform wait of at most ns nanoseconds (see above). May end early. */
static inline int pin_cond_wait_slice(pthread_cond_t *c, pthread_mutex_t *m, long long ns)
{
#if defined(__APPLE__)
    struct timespec rel = { .tv_sec = (time_t)(ns / 1000000000LL), .tv_nsec = (long)(ns % 1000000000LL) };
    return pthread_cond_timedwait_relative_np(c, m, &rel);
#else
    struct timespec ts;
#if defined(_WIN32)
    clock_gettime(CLOCK_REALTIME, &ts);
#else
    clock_gettime(*pin_cond_mono_flag() ? CLOCK_MONOTONIC : CLOCK_REALTIME, &ts);
#endif
    ts.tv_sec += (time_t)(ns / 1000000000LL);
    ts.tv_nsec += (long)(ns % 1000000000LL);
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(c, m, &ts);
#endif
}

/* Waits up to `ms` milliseconds for c (mutex m held). Returns 0 when woken (a signal, or a
 * spurious wake-up: the caller re-checks its condition as with any condition variable), or
 * ETIMEDOUT once `ms` milliseconds have passed on the monotonic clock. */
static inline int pin_cond_timedwait_ms(pthread_cond_t *c, pthread_mutex_t *m, long ms)
{
    long long end = pin_cond_mono_ns() + (ms > 0 ? (long long)ms * 1000000LL : 0);
    for (;;) {
        long long left = end - pin_cond_mono_ns();
        if (left <= 0)
            return ETIMEDOUT;
        int rc = pin_cond_wait_slice(c, m, left);
        if (rc != ETIMEDOUT)
            return rc;
    }
}

/* The same in whole seconds. */
static inline int pin_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, int seconds)
{
    return pin_cond_timedwait_ms(c, m, (long)seconds * 1000);
}

#endif
