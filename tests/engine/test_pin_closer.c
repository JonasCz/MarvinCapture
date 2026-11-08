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
    pin_status_t close_st;   /* what close() returns */
    int counts_frames;       /* as sink_hdvmux: frame counts, more of them written while closing */
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
    sleep_ms(f->close_ms);   /* a slow close */
    if (f->counts_frames && s->final_status) {
        fake_status(s, s->final_status);
        s->final_status->bytes_written += 1000;   /* what closing wrote */
        s->final_status->frames_out = 100;
        s->final_status->frames_error = 12;
        s->final_status->frames_repeated = 3;
    }
    pin_status_t st = f->close_st;
    free(f);
    free(s);
    return st;
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

static int consume_slow(pin_unit_kind_t kind, uint64_t index, const uint8_t *data, size_t len, void *user)
{
    sleep_ms(50);   /* the disk is behind: the queue in front of it fills */
    return consume(kind, index, data, len, user);
}

typedef struct {
    pthread_mutex_t lock;
    char order[32][16];
    uint64_t units[32];
    int n;
} log_t;

static void on_done(void *user, const pin_closer_done_t *d)
{
    log_t *l = user;
    pthread_mutex_lock(&l->lock);
    if (l->n < 32) {
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
    pin_closer_submit(c, wa, a, "scene1", 1, 200, 2, 5000, NULL);
    pin_sink_t *b = fake_sink(0);
    pin_closer_submit(c, NULL, b, "scene2", 0, 0, 0, 0, NULL);
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
    pin_closer_frames_t fr;
    pin_closer_take(c, &bytes, &units, &fr);
    CHECK(bytes == 800 && units == 8, "only what the caller had not counted");
    CHECK(fr.frames == 0 && fr.error == 0 && fr.repeated == 0, "a sink that counts no frames");
    pin_closer_take(c, &bytes, &units, NULL);
    CHECK(bytes == 0 && units == 0, "taken once");

    /* a sink that counts its frames (HDV MOV / MKV): what it still counted and wrote while
     * closing, beyond what was counted at hand-over */
    pin_sink_t *h = fake_sink(0);
    ((fake_t *)h->priv)->counts_frames = 1;
    pin_closer_frames_t counted = { 90, 10, 3 };
    pin_closer_submit(c, NULL, h, "hdv", 1, 0, 0, 0, &counted);
    pin_closer_wait(c);
    pin_closer_take(c, &bytes, &units, &fr);
    CHECK(fr.frames == 10 && fr.error == 2 && fr.repeated == 0, "frames counted while closing");
    CHECK(bytes == 1000, "bytes written while closing");

    /* a file whose close fails is counted and remembered (the first one), once */
    CHECK(pin_closer_failures(c) == 0, "no failure so far");
    pin_sink_t *bad1 = fake_sink(0);
    ((fake_t *)bad1->priv)->close_st = PIN_ERR_IO;
    pin_sink_t *bad2 = fake_sink(0);
    ((fake_t *)bad2->priv)->close_st = PIN_ERR_ARG;
    pin_closer_submit(c, NULL, bad1, "bad1", 1, 0, 0, 0, NULL);
    pin_closer_submit(c, NULL, fake_sink(0), "good", 1, 0, 0, 0, NULL);
    pin_closer_submit(c, NULL, bad2, "bad2", 1, 0, 0, 0, NULL);
    pin_closer_wait(c);
    CHECK(pin_closer_failures(c) == 2, "two failed closes");
    pin_status_t fst = PIN_OK;
    char fpath[64] = "";
    CHECK(pin_closer_take_failure(c, &fst, fpath, sizeof(fpath)) == 2, "taken: two");
    CHECK(fst == PIN_ERR_IO && strcmp(fpath, "bad1") == 0, "the first failure is kept");
    CHECK(pin_closer_failures(c) == 0 && pin_closer_take_failure(c, NULL, NULL, 0) == 0, "cleared");

    /* a writer that overflowed (fatal mode) before the hand-over: the file lacks the refused
     * units, so its close fails although the sink closed fine */
    pin_sink_t *ov = fake_sink(0);
    pin_writer_t *wov = pin_writer_start_ex(250, consume_slow, ov, PIN_WRITER_OVERFLOW_FATAL);
    int accepted = 0;
    for (int i = 0; i < 6; i++)
        accepted += pin_writer_push(wov, PIN_UNIT_RAW, (uint64_t)i, buf, sizeof(buf));
    CHECK(accepted < 6, "the queue overflowed");
    pin_closer_submit(c, wov, ov, "overflowed", 1, 0, 0, 0, NULL);
    pin_sink_t *okw = fake_sink(0);
    pin_writer_t *wok = pin_writer_start_ex(250, consume, okw, PIN_WRITER_OVERFLOW_FATAL);
    CHECK(pin_writer_push(wok, PIN_UNIT_RAW, 0, buf, sizeof(buf)) == 1, "a unit that fits");
    pin_closer_submit(c, wok, okw, "no_overflow", 1, 0, 0, 0, NULL);
    pin_closer_wait(c);
    CHECK(pin_closer_take_failure(c, &fst, fpath, sizeof(fpath)) == 1 && fst == PIN_ERR_IO &&
              strcmp(fpath, "overflowed") == 0, "an overflowed writer is a failed close, and only that one");

    /* the test seam: the n-th file closed fails */
    pin_closer_t *c2 = pin_closer_create(on_done, &log);
    setenv("PIN_TEST_CLOSE_FAIL", "2", 1);
    pin_closer_submit(c2, NULL, fake_sink(0), "seam1", 1, 0, 0, 0, NULL);
    pin_closer_submit(c2, NULL, fake_sink(0), "seam2", 1, 0, 0, 0, NULL);
    pin_closer_wait(c2);
    unsetenv("PIN_TEST_CLOSE_FAIL");
    CHECK(pin_closer_take_failure(c2, &fst, fpath, sizeof(fpath)) == 1 && fst == PIN_ERR_IO &&
              strcmp(fpath, "seam2") == 0, "seam: the 2nd close fails");
    pin_closer_destroy(c2);
    int n_before = log.n;

    /* destroy closes what is still queued */
    pin_closer_submit(c, NULL, fake_sink(50), "scene3", 1, 0, 0, 0, NULL);
    pin_closer_destroy(c);
    CHECK(log.n == n_before + 1 && strcmp(log.order[log.n - 1], "scene3") == 0, "closed on destroy");

    if (g_failures) {
        printf("test_pin_closer: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_pin_closer: OK\n");
    return 0;
}
