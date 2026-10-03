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
 */

#include "pin_sink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    pin_sink_t *inner;
    char path[PIN_PATH_MAX];
    pin_sink_params_t params;
    int have_open;              /* open() was called */
    int opened;                 /* the real sink was opened (the file exists) */
    pin_status_t open_error;
    uint64_t units;             /* video units the real sink took */
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

static pin_status_t lazy_ensure_open(lazy_priv_t *p)
{
    if (p->opened)
        return PIN_OK;
    if (p->open_error != PIN_OK)
        return p->open_error;
    if (!p->have_open)
        return PIN_ERR_STATE;
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
    if (!p->opened)
        return PIN_OK;      /* before the first video: dropped */
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
    pin_status_t rc = p->inner->close(p->inner);
    if (!p->opened)
        rc = PIN_OK;        /* nothing on disk, the inner sink was only freed */
    if (p->opened && p->units == 0)
        remove(p->path);    /* only damaged units reached it: the empty container we just created */
    if (rc == PIN_OK && p->open_error != PIN_OK)
        rc = p->open_error;
    free(p);
    free(s);
    return rc;
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
