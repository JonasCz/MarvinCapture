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

#include "pin_closer.h"
#include "../core/pin_log.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct job {
    struct job *next;
    pin_writer_t *writer;
    pin_sink_t *sink;
    char *path;
    int announced;
    uint64_t bytes, units, reserve;
} job_t;

struct pin_closer {
    pthread_mutex_t lock;
    pthread_cond_t wake, idle;
    pthread_t thread;
    job_t *head, *tail;
    int busy;                 /* a job is being closed (it is off the list) */
    int stop;
    uint64_t pending;         /* reserve of the queued jobs and the one being closed */
    int64_t more_bytes, more_units;
    pin_closer_done_fn done;
    void *user;
};

/* Stops the writer, counts, closes. Returns the close status. */
static pin_status_t close_job(const job_t *j, int64_t *bytes, int64_t *units, uint64_t *final_units)
{
    int wfail = j->writer ? pin_writer_stop(j->writer) != 0 : 0;
    pin_sink_status_t sst;
    memset(&sst, 0, sizeof(sst));
    if (j->sink->get_status) {
        j->sink->get_status(j->sink, &sst);
        *bytes = (int64_t)sst.bytes_written - (int64_t)j->bytes;
        *units = (int64_t)sst.units_written - (int64_t)j->units;
        *final_units = sst.units_written;
    } else {
        *bytes = *units = 0;
        *final_units = j->units;
    }
    pin_status_t st = j->sink->close(j->sink);
    return st == PIN_OK && wfail ? PIN_ERR_IO : st;
}

static void report(pin_closer_t *c, const job_t *j, pin_status_t st, uint64_t units)
{
    if (st != PIN_OK)
        pin_logf(PIN_LOG_WARN, "closer: %s: %s\n", j->path, pin_strerror(st));
    pin_closer_done_t d = { .status = st, .units = units, .announced = j->announced, .path = j->path };
    if (c->done)
        c->done(c->user, &d);
}

static void *closer_thread(void *arg)
{
    pin_closer_t *c = arg;
    pthread_mutex_lock(&c->lock);
    for (;;) {
        while (!c->head && !c->stop)
            pthread_cond_wait(&c->wake, &c->lock);
        job_t *j = c->head;
        if (!j)
            break;                      /* stopped and nothing left */
        c->head = j->next;
        if (!c->head)
            c->tail = NULL;
        c->busy = 1;
        pthread_mutex_unlock(&c->lock);

        int64_t bytes, units;
        uint64_t final_units;
        pin_status_t st = close_job(j, &bytes, &units, &final_units);
        report(c, j, st, final_units);

        pthread_mutex_lock(&c->lock);
        c->more_bytes += bytes;
        c->more_units += units;
        c->pending -= j->reserve;
        c->busy = 0;
        free(j->path);
        free(j);
        if (!c->head)
            pthread_cond_broadcast(&c->idle);
    }
    pthread_mutex_unlock(&c->lock);
    return NULL;
}

pin_closer_t *pin_closer_create(pin_closer_done_fn done, void *user)
{
    pin_closer_t *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->done = done;
    c->user = user;
    pthread_mutex_init(&c->lock, NULL);
    pthread_cond_init(&c->wake, NULL);
    pthread_cond_init(&c->idle, NULL);
    if (pthread_create(&c->thread, NULL, closer_thread, c) != 0) {
        pthread_mutex_destroy(&c->lock);
        pthread_cond_destroy(&c->wake);
        pthread_cond_destroy(&c->idle);
        free(c);
        return NULL;
    }
    return c;
}

void pin_closer_submit(pin_closer_t *c, pin_writer_t *writer, pin_sink_t *sink, const char *path,
                       int announced, uint64_t bytes, uint64_t units, uint64_t reserve)
{
    job_t *j = calloc(1, sizeof(*j));
    char *p = strdup(path ? path : "");
    if (!c || !j || !p) {
        free(j);
        free(p);
        job_t now = { .writer = writer, .sink = sink, .path = (char *)(path ? path : ""),
                      .announced = announced, .bytes = bytes, .units = units };
        int64_t b, u;
        uint64_t final_units;
        pin_status_t st = close_job(&now, &b, &u, &final_units);
        if (c) {
            report(c, &now, st, final_units);
            pthread_mutex_lock(&c->lock);
            c->more_bytes += b;
            c->more_units += u;
            pthread_mutex_unlock(&c->lock);
        }
        return;
    }
    *j = (job_t){ .writer = writer, .sink = sink, .path = p, .announced = announced,
                  .bytes = bytes, .units = units, .reserve = reserve };
    pthread_mutex_lock(&c->lock);
    if (c->tail)
        c->tail->next = j;
    else
        c->head = j;
    c->tail = j;
    c->pending += reserve;
    pthread_cond_signal(&c->wake);
    pthread_mutex_unlock(&c->lock);
}

void pin_closer_wait(pin_closer_t *c)
{
    if (!c)
        return;
    pthread_mutex_lock(&c->lock);
    while (c->head || c->busy)
        pthread_cond_wait(&c->idle, &c->lock);
    pthread_mutex_unlock(&c->lock);
}

int pin_closer_busy(pin_closer_t *c)
{
    if (!c)
        return 0;
    pthread_mutex_lock(&c->lock);
    int busy = c->head || c->busy;
    pthread_mutex_unlock(&c->lock);
    return busy;
}

void pin_closer_take(pin_closer_t *c, int64_t *bytes, int64_t *units)
{
    *bytes = *units = 0;
    if (!c)
        return;
    pthread_mutex_lock(&c->lock);
    *bytes = c->more_bytes;
    *units = c->more_units;
    c->more_bytes = c->more_units = 0;
    pthread_mutex_unlock(&c->lock);
}

uint64_t pin_closer_pending_bytes(pin_closer_t *c)
{
    if (!c)
        return 0;
    pthread_mutex_lock(&c->lock);
    uint64_t n = c->pending;
    pthread_mutex_unlock(&c->lock);
    return n;
}

void pin_closer_destroy(pin_closer_t *c)
{
    if (!c)
        return;
    pthread_mutex_lock(&c->lock);
    c->stop = 1;
    pthread_cond_signal(&c->wake);
    pthread_mutex_unlock(&c->lock);
    pthread_join(c->thread, NULL);
    pthread_mutex_destroy(&c->lock);
    pthread_cond_destroy(&c->wake);
    pthread_cond_destroy(&c->idle);
    free(c);
}
