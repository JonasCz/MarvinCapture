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

#include "pin_writer.h"
#include "../core/pin_log.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    pin_unit_kind_t kind;
    uint64_t index;
    size_t off;      /* into byte ring, mod capacity */
    size_t len;
} unit_hdr_t;

struct pin_writer {
    uint8_t *buf;
    size_t capacity;
    size_t byte_head, byte_tail;      /* monotonic counters */

    unit_hdr_t units[PIN_WRITER_MAX_QUEUED_UNITS];
    size_t unit_head, unit_tail;      /* monotonic counters, mod array size */

    pin_writer_stats_t stats;
    int closed, failed, overflowed, aborted;
    unsigned flags;

    pthread_mutex_t lock;
    pthread_cond_t wake;
    pthread_t thread;

    pin_writer_consume_fn consume;
    void *user;
};

static void *writer_thread(void *arg)
{
    pin_writer_t *w = arg;
    /* Scratch buffer for units that wrap the ring; sized once we see the
     * largest unit pushed (grown lazily, freed at thread exit). Most units
     * (a single DV frame, one video/audio block) don't wrap, so the common
     * path below copies nothing extra. */
    uint8_t *scratch = NULL;
    size_t scratch_cap = 0;

    pthread_mutex_lock(&w->lock);
    for (;;) {
        while (w->unit_head == w->unit_tail && !w->closed && !w->aborted)
            pthread_cond_wait(&w->wake, &w->lock);
        if (w->aborted || w->unit_head == w->unit_tail)
            break;                              /* closed and fully drained */

        unit_hdr_t u = w->units[w->unit_tail % PIN_WRITER_MAX_QUEUED_UNITS];
        pthread_mutex_unlock(&w->lock);

        const uint8_t *ptr;
        if (u.off + u.len <= w->capacity) {
            ptr = w->buf + u.off;
        } else {
            if (u.len > scratch_cap) {
                free(scratch);
                scratch = malloc(u.len);
                scratch_cap = scratch ? u.len : 0;
            }
            if (!scratch) {
                pin_logf(PIN_LOG_ERROR, "pin_writer: out of memory reassembling a wrapped unit\n");
                pthread_mutex_lock(&w->lock);
                w->failed = 1;
                break;
            }
            size_t first = w->capacity - u.off;
            memcpy(scratch, w->buf + u.off, first);
            memcpy(scratch + first, w->buf, u.len - first);
            ptr = scratch;
        }

        int rc = w->consume(u.kind, u.index, ptr, u.len, w->user);

        pthread_mutex_lock(&w->lock);
        w->byte_tail += u.len;
        w->unit_tail++;
        w->stats.backlog_bytes = w->byte_head - w->byte_tail;
        w->stats.units_consumed++;
        if (rc != 0) {
            w->failed = 1;
            break;
        }
    }
    pthread_mutex_unlock(&w->lock);
    free(scratch);
    return NULL;
}

pin_writer_t *pin_writer_start(size_t capacity_bytes, pin_writer_consume_fn consume, void *user)
{
    return pin_writer_start_ex(capacity_bytes, consume, user, 0);
}

pin_writer_t *pin_writer_start_ex(size_t capacity_bytes, pin_writer_consume_fn consume, void *user,
                                  unsigned flags)
{
    if (!capacity_bytes)
        capacity_bytes = PIN_WRITER_DEFAULT_CAPACITY;
    pin_writer_t *w = calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    w->buf = malloc(capacity_bytes);
    if (!w->buf) {
        free(w);
        return NULL;
    }
    w->capacity = capacity_bytes;
    w->consume = consume;
    w->flags = flags;
    w->user = user;
    pthread_mutex_init(&w->lock, NULL);
    pthread_cond_init(&w->wake, NULL);
    if (pthread_create(&w->thread, NULL, writer_thread, w) != 0) {
        free(w->buf);
        free(w);
        return NULL;
    }
    return w;
}

int pin_writer_push(pin_writer_t *w, pin_unit_kind_t kind, uint64_t index,
                    const uint8_t *data, size_t len)
{
    if (!w)
        return 0;
    pthread_mutex_lock(&w->lock);
    if (w->closed || w->failed || w->overflowed || w->aborted) {
        pthread_mutex_unlock(&w->lock);
        return 0;
    }
    size_t fill = w->byte_head - w->byte_tail;
    size_t units_fill = w->unit_head - w->unit_tail;
    if (len > w->capacity - fill || units_fill >= PIN_WRITER_MAX_QUEUED_UNITS) {
        w->stats.overflow_count++;
        if (w->flags & PIN_WRITER_OVERFLOW_FATAL)
            w->overflowed = 1;
        pthread_mutex_unlock(&w->lock);
        return 0;
    }

    size_t off = w->byte_head % w->capacity;
    size_t first = w->capacity - off;
    if (first > len)
        first = len;
    memcpy(w->buf + off, data, first);
    if (len > first)
        memcpy(w->buf, data + first, len - first);

    w->units[w->unit_head % PIN_WRITER_MAX_QUEUED_UNITS] =
        (unit_hdr_t){ .kind = kind, .index = index, .off = off, .len = len };
    w->unit_head++;
    w->byte_head += len;

    w->stats.units_pushed++;
    w->stats.bytes_pushed += len;
    w->stats.backlog_bytes = w->byte_head - w->byte_tail;
    if (w->stats.backlog_bytes > w->stats.backlog_high_water)
        w->stats.backlog_high_water = w->stats.backlog_bytes;

    pthread_cond_signal(&w->wake);
    pthread_mutex_unlock(&w->lock);
    return 1;
}

void pin_writer_get_stats(pin_writer_t *w, pin_writer_stats_t *out)
{
    if (!w || !out)
        return;
    pthread_mutex_lock(&w->lock);
    *out = w->stats;
    out->failed = w->failed;
    out->overflowed = w->overflowed;
    pthread_mutex_unlock(&w->lock);
}

void pin_writer_abort(pin_writer_t *w)
{
    if (!w)
        return;
    pthread_mutex_lock(&w->lock);
    w->aborted = 1;
    pthread_cond_signal(&w->wake);
    pthread_mutex_unlock(&w->lock);
}

int pin_writer_stop(pin_writer_t *w)
{
    if (!w)
        return 0;
    pthread_mutex_lock(&w->lock);
    w->closed = 1;
    pthread_cond_signal(&w->wake);
    pthread_mutex_unlock(&w->lock);
    pthread_join(w->thread, NULL);

    int failed = w->failed;
    if (w->stats.overflow_count)
        pin_logf(PIN_LOG_WARN, "pin_writer: dropped %llu unit(s), disk fell behind "
                                "(high-water %.1f MB of %.1f MB)\n",
                (unsigned long long)w->stats.overflow_count,
                w->stats.backlog_high_water / (1024.0 * 1024.0),
                w->capacity / (1024.0 * 1024.0));

    pthread_mutex_destroy(&w->lock);
    pthread_cond_destroy(&w->wake);
    free(w->buf);
    free(w);
    return failed ? -1 : 0;
}
