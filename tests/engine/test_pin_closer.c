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

/* pin_closer: a file handed over closes on the closer's thread (the hand-over
 * itself does not wait for a slow close), files close in order, the writer's
 * queue is drained into the file before it is closed, and the counts and
 * disk reserve add up. */

#include "pin_closer.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
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

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void sleep_ms(int ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

typedef struct {
    uint64_t units, bytes;
    int close_ms;
    int *order, *norder;
} fake_t;

static pin_status_t fake_unit(pin_sink_t *s, const uint8_t *d, size_t len)
{
    (void)d;
    fake_t *f = s->priv;
    f->units++;
    f->bytes += len;
    return PIN_OK;
}

static void fake_status(pin_sink_t *s, pin_sink_status_t *out)
{
    fake_t *f = s->priv;
    memset(out, 0, sizeof(*out));
    out->units_written = f->units;
    out->bytes_written = f->bytes;
}

static pin_status_t fake_close(pin_sink_t *s)
{
    fake_t *f = s->priv;
    sleep_ms(f->close_ms);   /* a remux */
    free(f);
    free(s);
    return PIN_OK;
}

static pin_sink_t *fake_sink(int close_ms)
{
    pin_sink_t *s = calloc(1, sizeof(*s));
    fake_t *f = calloc(1, sizeof(*f));
    f->close_ms = close_ms;
    s->write_unit = fake_unit;
    s->get_status = fake_status;
    s->close = fake_close;
    s->priv = f;
    return s;
}

static int consume(pin_unit_kind_t kind, uint64_t index, const uint8_t *data, size_t len, void *user)
{
    (void)kind;
    (void)index;
    pin_sink_t *s = user;
    return s->write_unit(s, data, len) == PIN_OK ? 0 : -1;
}

typedef struct {
    pthread_mutex_t lock;
    char order[8][16];
    uint64_t units[8];
    int n;
} log_t;

static void on_done(void *user, const pin_closer_done_t *d)
{
    log_t *l = user;
    pthread_mutex_lock(&l->lock);
    if (l->n < 8) {
        snprintf(l->order[l->n], sizeof(l->order[0]), "%s", d->path);
        l->units[l->n] = d->units;
        l->n++;
    }
    pthread_mutex_unlock(&l->lock);
}

int main(void)
{
    log_t log;
    memset(&log, 0, sizeof(log));
    pthread_mutex_init(&log.lock, NULL);
    pin_closer_t *c = pin_closer_create(on_done, &log);
    CHECK(c != NULL, "created");

    /* scene 1: 10 units of 100 bytes queued, 2 of them already written */
    pin_sink_t *a = fake_sink(300);
    pin_writer_t *wa = pin_writer_start(1 << 20, consume, a);
    uint8_t buf[100] = { 0 };
    for (int i = 0; i < 10; i++)
        pin_writer_push(wa, PIN_UNIT_RAW, (uint64_t)i, buf, sizeof(buf));
    double t0 = now_s();
    pin_closer_submit(c, wa, a, "scene1", 1, 200, 2, 5000);
    pin_sink_t *b = fake_sink(0);
    pin_closer_submit(c, NULL, b, "scene2", 0, 0, 0, 0);
    CHECK(now_s() - t0 < 0.1, "handing over does not wait for the close");
    CHECK(pin_closer_pending_bytes(c) == 5000, "reserve counted while closing");

    pin_closer_wait(c);
    CHECK(now_s() - t0 >= 0.29, "wait returns after the slow close");
    CHECK(pin_closer_pending_bytes(c) == 0, "reserve released");
    CHECK(log.n == 2, "both files reported");
    CHECK(strcmp(log.order[0], "scene1") == 0 && strcmp(log.order[1], "scene2") == 0, "in order");
    CHECK(log.units[0] == 10, "the queue was drained into the file before it closed");
    CHECK(log.units[1] == 0, "an empty file reports no units");
    int64_t bytes, units;
    pin_closer_take(c, &bytes, &units);
    CHECK(bytes == 800 && units == 8, "only what the caller had not counted");
    pin_closer_take(c, &bytes, &units);
    CHECK(bytes == 0 && units == 0, "taken once");

    /* destroy closes what is still queued */
    pin_closer_submit(c, NULL, fake_sink(50), "scene3", 1, 0, 0, 0);
    pin_closer_destroy(c);
    CHECK(log.n == 3 && strcmp(log.order[2], "scene3") == 0, "closed on destroy");

    if (g_failures) {
        printf("test_pin_closer: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_pin_closer: OK\n");
    return 0;
}
