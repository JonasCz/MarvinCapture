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

/* Condition variables whose timed wait runs on the monotonic clock. */

#ifndef PIN_COND_H
#define PIN_COND_H

#include <errno.h>
#include <pthread.h>
#include <time.h>

/* A condition variable for pin_cond_timedwait(): the wait runs on the monotonic clock, so a
 * wall-clock step (NTP, the user setting the time) cannot end it early or late. macOS has no
 * pthread_condattr_setclock(); its relative wait is monotonic already. Where the clock cannot
 * be set the plain variable is used and the wait falls back to the wall clock. */
#if !defined(__APPLE__)
static inline int *pin_cond_mono_flag(void)
{
    static int ok = 1;      /* per translation unit; init and wait are in the same one */
    return &ok;
}
#endif
static inline void pin_cond_init_timed(pthread_cond_t *c)
{
#if !defined(__APPLE__)
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

/* Waits up to `ms` milliseconds for c (mutex m held). Returns ETIMEDOUT or 0. */
static inline int pin_cond_timedwait_ms(pthread_cond_t *c, pthread_mutex_t *m, long ms)
{
#if defined(__APPLE__)
    struct timespec rel = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    return pthread_cond_timedwait_relative_np(c, m, &rel);
#else
    struct timespec ts;
    clock_gettime(*pin_cond_mono_flag() ? CLOCK_MONOTONIC : CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(c, m, &ts);
#endif
}

/* The same in whole seconds. */
static inline int pin_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, int seconds)
{
    return pin_cond_timedwait_ms(c, m, (long)seconds * 1000);
}


#endif
