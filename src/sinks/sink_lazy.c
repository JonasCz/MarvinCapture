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
 * pin_sink_lazy(): wraps a file sink so that the file is created when the
 * first video unit arrives, not when the sink is opened. A capture that
 * receives no video at all (a blank tape, a deck that never plays) then leaves
 * nothing behind, and an existing file that --overwrite would replace is not
 * touched until there is something to replace it with.
 *
 * open() only remembers the path and parameters (and checks that the
 * directory exists, so the commonest failure still shows when the capture
 * starts). The first write_video()/write_unit() opens the real sink; an open
 * error is returned from that write, so the writer fails and the capture ends
 * with a write error. Audio that arrives before the first video is dropped (an
 * audio-only file is not worth keeping, and the video starts at its own
 * beginning). A unit the real sink rejects at its first write (an
 * open error) is not counted.
 *
 * get_status() reports units_written as the number of video units the real
 * sink accepted, which unlike its own counter is exact at any time (sink_ffv1
 * counts in an encoder thread).
 *
 * Taken names (numbered output, pin_sink_lazy_set_renumber()): in Android's
 * shared storage (Movies/) an app lists only its own files, and after a
 * reinstall it no longer owns the ones it wrote before. pin_next_file_number()
 * cannot see those, so it may pick a number whose file exists; creating it then
 * fails (EEXIST from the media provider, or EACCES / EPERM) and the capture
 * would end with a write error at its first frame. So before the real sink
 * creates a numbered file, the name is claimed with an exclusive create (and
 * the empty file removed again at once, so the real sink creates it as
 * before); a name that is taken moves the file to the next number, up to
 * PIN_SINK_LAZY_MAX_SKIP times. The engine picks the new name and number up
 * with pin_sink_lazy_renumbered(). When the name is free nothing changes.
 * Numbered output never overwrites, so an existing file of the same name
 * (which pin_next_file_number() avoids anyway) counts as taken too. POSIX
 * only: Windows has no such storage.
 */

#include "pin_sink.h"
#include "../core/pin_log.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

typedef struct {
    pin_sink_t *inner;
    char path[PIN_PATH_MAX];    /* written by the writer thread when renumbered: under mtx */
    pin_sink_params_t params;
    int have_open;              /* open() was called */
    int opened;                 /* the real sink was opened (the file exists) */
    pin_status_t open_error;
    uint64_t units;             /* video units the real sink took */
    /* numbered output: candidates for a taken name (pin_sink_lazy_set_renumber) */
    pin_sink_name_fn next;
    void *next_ctx;
    void (*next_free)(void *ctx);
    unsigned skipped;           /* names skipped (under mtx) */
    pthread_mutex_t mtx;
} lazy_priv_t;

static pin_status_t lazy_open(pin_sink_t *s, const char *path, const pin_sink_params_t *params)
{
    lazy_priv_t *p = s->priv;
    if (strlen(path) >= sizeof(p->path))
        return PIN_ERR_ARG;
    /* the directory has to be there: report that now, not at the first frame */
    char dir[PIN_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *sep = NULL;
    for (char *c = dir; *c; c++)
        if (*c == '/' || *c == '\\')
            sep = c;
    if (sep && sep > dir && sep[-1] != ':') {
        *sep = 0;
        struct stat st;
        if (stat(dir, &st) != 0 || !(st.st_mode & S_IFDIR))
            return PIN_ERR_IO;
    }
    memcpy(p->path, path, strlen(path) + 1);
    p->params = *params;
    p->have_open = 1;
    return PIN_OK;
}

#if !defined(_WIN32)
/* 1 if path cannot become this capture's new file (see "Taken names" above); *err = why. A
 * free name is created and removed again, so the real sink creates it exactly as before. */
static int name_taken(const char *path, int *err)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd >= 0) {
        close(fd);
        unlink(path);
        return 0;
    }
    *err = errno;
    return errno == EEXIST || errno == EACCES || errno == EPERM;
}
#endif

/* Moves a numbered file whose name is taken to the next free number. Any other failure
 * (no directory, read-only volume) is left to the real sink's open, as before. */
static void lazy_renumber(lazy_priv_t *p)
{
#if !defined(_WIN32)
    char cand[PIN_PATH_MAX];
    snprintf(cand, sizeof(cand), "%s", p->path);
    int first_err = 0, err = 0;
    unsigned k = 0;
    while (name_taken(cand, &err)) {
        if (k == 0)
            first_err = err;
        if (++k > PIN_SINK_LAZY_MAX_SKIP || p->next(p->next_ctx, k, cand, sizeof(cand)) != 0) {
            pin_logf(PIN_LOG_WARN, "sink_lazy: %s cannot be created (%s) and no free number "
                     "follows it\n", p->path, strerror(first_err));
            return;
        }
    }
    if (k == 0)
        return;
    pin_logf(PIN_LOG_INFO, "sink_lazy: %s cannot be created (%s; in Android's shared storage "
             "usually a file left by an earlier installation, which this app cannot see): "
             "writing %s instead\n", p->path, strerror(first_err), cand);
    pthread_mutex_lock(&p->mtx);
    snprintf(p->path, sizeof(p->path), "%s", cand);
    p->skipped = k;
    pthread_mutex_unlock(&p->mtx);
#else
    (void)p;
#endif
}

static pin_status_t lazy_ensure_open(lazy_priv_t *p)
{
    if (p->opened)
        return PIN_OK;
    if (p->open_error != PIN_OK)
        return p->open_error;
    if (!p->have_open)
        return PIN_ERR_STATE;
    if (p->next)
        lazy_renumber(p);
    pin_status_t st = p->inner->open(p->inner, p->path, &p->params);
    if (st != PIN_OK) {
        p->open_error = st;
        return st;
    }
    p->opened = 1;
    return PIN_OK;
}

static pin_status_t lazy_write_unit(pin_sink_t *s, const uint8_t *data, size_t len)
{
    lazy_priv_t *p = s->priv;
    pin_status_t st = lazy_ensure_open(p);
    if (st != PIN_OK)
        return st;
    st = p->inner->write_unit(p->inner, data, len);
    if (st == PIN_OK)
        p->units++;
    return st;
}

static pin_status_t lazy_write_video(pin_sink_t *s, const uint8_t *yuyv, size_t len)
{
    lazy_priv_t *p = s->priv;
    pin_status_t st = lazy_ensure_open(p);
    if (st != PIN_OK)
        return st;
    st = p->inner->write_video(p->inner, yuyv, len);
    if (st == PIN_OK)
        p->units++;
    return st;
}

static pin_status_t lazy_write_audio(pin_sink_t *s, const int16_t *pcm, size_t frames)
{
    lazy_priv_t *p = s->priv;
    /* Before the first video: dropped. The file is created by its first video unit, so when
     * the audio packet of frame F is delivered ahead of video frame F, the file starts with the
     * audio of F+1: about one frame (40 ms) of audio is missing at the start. That is accepted;
     * holding audio back until the video arrives would need a queue for a loss this small. */
    if (!p->opened)
        return PIN_OK;
    return p->inner->write_audio(p->inner, pcm, frames);
}

static void lazy_get_status(pin_sink_t *s, pin_sink_status_t *out)
{
    lazy_priv_t *p = s->priv;
    memset(out, 0, sizeof(*out));
    if (p->opened && p->inner->get_status)
        p->inner->get_status(p->inner, out);
    out->units_written = p->units;
    if (p->open_error != PIN_OK)
        out->last_error = p->open_error;
}

static pin_status_t lazy_close(pin_sink_t *s)
{
    lazy_priv_t *p = s->priv;
    p->inner->final_status = p->opened ? s->final_status : NULL;
    pin_status_t rc = p->inner->close(p->inner);
    if (s->final_status) {
        if (!p->opened)
            memset(s->final_status, 0, sizeof(*s->final_status));
        s->final_status->units_written = p->units;
    }
    if (!p->opened)
        rc = PIN_OK;        /* nothing on disk, the inner sink was only freed */
    if (p->opened && p->units == 0)
        remove(p->path);    /* only damaged units reached it: the empty container we just created */
    if (rc == PIN_OK && p->open_error != PIN_OK)
        rc = p->open_error;
    if (p->next_free)
        p->next_free(p->next_ctx);
    pthread_mutex_destroy(&p->mtx);
    free(p);
    free(s);
    return rc;
}

void pin_sink_lazy_set_renumber(pin_sink_t *s, pin_sink_name_fn next, void *ctx,
                                void (*ctx_free)(void *ctx))
{
    if (!s || s->open != lazy_open || !next) {
        if (ctx_free)
            ctx_free(ctx);
        return;
    }
    lazy_priv_t *p = s->priv;
    if (p->next_free)
        p->next_free(p->next_ctx);
    p->next = next;
    p->next_ctx = ctx;
    p->next_free = ctx_free;
}

unsigned pin_sink_lazy_renumbered(pin_sink_t *s, char *out, size_t cap)
{
    if (!s || s->open != lazy_open)
        return 0;
    lazy_priv_t *p = s->priv;
    pthread_mutex_lock(&p->mtx);
    unsigned k = p->skipped;
    if (k && out && cap)
        snprintf(out, cap, "%s", p->path);
    pthread_mutex_unlock(&p->mtx);
    return k;
}

pin_sink_t *pin_sink_lazy(pin_sink_t *inner)
{
    if (!inner)
        return NULL;
    pin_sink_t *s = calloc(1, sizeof(*s));
    lazy_priv_t *p = calloc(1, sizeof(*p));
    if (!s || !p) {
        free(s);
        free(p);
        inner->close(inner);
        return NULL;
    }
    p->inner = inner;
    pthread_mutex_init(&p->mtx, NULL);
    s->priv = p;
    s->open = lazy_open;
    s->write_unit = lazy_write_unit;
    s->write_video = lazy_write_video;
    s->write_audio = lazy_write_audio;
    s->close = lazy_close;
    s->get_status = lazy_get_status;
    s->supports_title = inner->supports_title;
    return s;
}
