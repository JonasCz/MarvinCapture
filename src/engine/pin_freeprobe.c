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

#include "pin_freeprobe.h"
#include "../core/pin_cond.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct pin_freeprobe {
    pthread_mutex_t mu;
    pthread_cond_t idle;
    pin_freeprobe_fn fn;
    int refs;
    int busy;                         /* a lookup runs */
    char query_dir[PIN_FREEPROBE_DIR_MAX];   /* what the running lookup asks about */
    int have;
    char dir[PIN_FREEPROBE_DIR_MAX];  /* what the stored result is about */
    uint64_t bytes;
    int known, fat32;
    double t;
};

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

pin_freeprobe_t *pin_freeprobe_create(pin_freeprobe_fn fn)
{
    pin_freeprobe_t *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    pthread_mutex_init(&p->mu, NULL);
    pin_cond_init_timed(&p->idle);
    p->fn = fn;
    p->refs = 1;
    return p;
}

static void unref(pin_freeprobe_t *p)
{
    pthread_mutex_lock(&p->mu);
    int last = --p->refs == 0;
    pthread_mutex_unlock(&p->mu);
    if (last) {
        pthread_cond_destroy(&p->idle);
        pthread_mutex_destroy(&p->mu);
        free(p);
    }
}

void pin_freeprobe_release(pin_freeprobe_t *p)
{
    if (p)
        unref(p);
}

static void lookup(pin_freeprobe_t *p, const char *dir)
{
    uint64_t bytes = 0;
    int fat32 = 0;
    int ok = p->fn(dir, &bytes, &fat32) == 0;
    pthread_mutex_lock(&p->mu);
    snprintf(p->dir, sizeof(p->dir), "%s", dir);
    p->bytes = ok ? bytes : 0;
    p->fat32 = ok ? fat32 : 0;
    p->known = ok;
    p->have = 1;
    p->t = now_s();
    p->busy = 0;
    pthread_cond_broadcast(&p->idle);
    pthread_mutex_unlock(&p->mu);
}

static void *probe_thread(void *arg)
{
    pin_freeprobe_t *p = arg;
    char dir[PIN_FREEPROBE_DIR_MAX];
    pthread_mutex_lock(&p->mu);
    memcpy(dir, p->query_dir, sizeof(dir));
    pthread_mutex_unlock(&p->mu);
    lookup(p, dir);
    unref(p);
    return NULL;
}

int pin_freeprobe_get(pin_freeprobe_t *p, const char *dir, double max_age_s,
                      uint64_t *free_bytes, int *known, int *fat32)
{
    int start = 0;
    int got = 0;
    pthread_mutex_lock(&p->mu);
    int same = p->have && strcmp(p->dir, dir) == 0;
    if (same) {
        *free_bytes = p->bytes;
        *known = p->known;
        if (fat32)
            *fat32 = p->fat32;
        got = 1;
    }
    if (!p->busy && (!same || now_s() - p->t >= max_age_s)) {
        p->busy = 1;
        snprintf(p->query_dir, sizeof(p->query_dir), "%s", dir);
        p->refs++;
        start = 1;
    }
    pthread_mutex_unlock(&p->mu);
    if (start) {
        pthread_t th;
        if (pthread_create(&th, NULL, probe_thread, p) == 0) {
            pthread_detach(th);
        } else {
            /* no thread to be had: ask here rather than never */
            lookup(p, dir);
            unref(p);
            pthread_mutex_lock(&p->mu);
            *free_bytes = p->bytes;
            *known = p->known;
            if (fat32)
                *fat32 = p->fat32;
            pthread_mutex_unlock(&p->mu);
            got = 1;
        }
    }
    return got;
}

int pin_freeprobe_wait_idle(pin_freeprobe_t *p, int ms)
{
    double end = now_s() + ms / 1000.0;
    int idle;
    pthread_mutex_lock(&p->mu);
    while (p->busy) {
        double left = end - now_s();
        if (left <= 0)
            break;
        pin_cond_timedwait_ms(&p->idle, &p->mu, (long)(left * 1000.0) + 1);
    }
    idle = !p->busy;
    pthread_mutex_unlock(&p->mu);
    return idle;
}
