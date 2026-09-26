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
 * PIN_FMT_ANALOG_AVI: thin adapter from the pin_sink_t vtable onto the
 * existing src/core/avi_writer.[ch] (uncompressed YUY2 + PCM OpenDML AVI).
 * All the format decisions (frame size math, OpenDML vprp aspect, index
 * chunks) stay in avi_writer.c, unchanged; this file only wires up
 * open/write/close and the new title support (avi_open_titled).
 */

#include "pin_sink.h"
#include "sinks_internal.h"
#include "../core/avi_writer.h"

#include <stdlib.h>

typedef struct {
    avi_writer_t *w;
    pin_sink_status_t st;
} avi_priv_t;

static pin_status_t avi_sink_open(pin_sink_t *s, const char *path, const pin_sink_params_t *params)
{
    avi_priv_t *p = s->priv;
    if (params->kind != PIN_KIND_ANALOG)
        return PIN_ERR_ARG;

    /* avi_writer's aspect_num/den is the whole-frame display aspect (its
     * OpenDML vprp box), not a pixel SAR -- so it's derived from
     * params->aspect directly rather than via pin_sink_sar_for(), which is
     * the SAR-computing counterpart used by sink_ffv1.c/sink_rewrap.c.
     * PIN_ASPECT_AUTO defaults to 4:3 here, matching every other sink in
     * this directory: by pin_sink.h's contract AUTO should already have
     * been resolved by the caller before it reaches a sink, but a 4:3
     * default is the safe, non-crashing fallback if that contract is ever
     * violated. */
    int dar_num = (params->aspect == PIN_ASPECT_16_9) ? 16 : 4;
    int dar_den = (params->aspect == PIN_ASPECT_16_9) ? 9 : 3;

    p->w = avi_open_titled(path, (unsigned)params->width, (unsigned)params->height,
                           (unsigned)params->fps_num, (unsigned)params->fps_den,
                           (unsigned)dar_num, (unsigned)dar_den,
                           (unsigned)params->audio_rate, (unsigned)params->audio_channels,
                           params->title);
    if (!p->w) {
        p->st.last_error = PIN_ERR_IO;
        return PIN_ERR_IO;
    }
    return PIN_OK;
}

static pin_status_t avi_sink_write_video(pin_sink_t *s, const uint8_t *yuyv, size_t len)
{
    avi_priv_t *p = s->priv;
    if (!p->w)
        return PIN_ERR_STATE;
    if (avi_write_video(p->w, yuyv, len) != 0) {
        p->st.last_error = PIN_ERR_IO;
        return PIN_ERR_IO;
    }
    p->st.bytes_written += len;
    p->st.units_written++;
    return PIN_OK;
}

static pin_status_t avi_sink_write_audio(pin_sink_t *s, const int16_t *pcm, size_t frames)
{
    avi_priv_t *p = s->priv;
    if (!p->w)
        return PIN_ERR_STATE;
    size_t len = frames * 2 * sizeof(int16_t); /* stereo s16 */
    if (avi_write_audio(p->w, (const uint8_t *)pcm, len) != 0) {
        p->st.last_error = PIN_ERR_IO;
        return PIN_ERR_IO;
    }
    p->st.bytes_written += len;
    return PIN_OK;
}

static pin_status_t avi_sink_close(pin_sink_t *s)
{
    avi_priv_t *p = s->priv;
    pin_status_t rc = PIN_OK;
    if (p->w) {
        if (avi_close(p->w) != 0)
            rc = PIN_ERR_IO;
        p->w = NULL;
    }
    free(p);
    free(s);
    return rc;
}

static void avi_sink_get_status(pin_sink_t *s, pin_sink_status_t *out)
{
    avi_priv_t *p = s->priv;
    *out = p->st;
}

pin_sink_t *sink_avi_create(void)
{
    pin_sink_t *s = calloc(1, sizeof(*s));
    avi_priv_t *p = calloc(1, sizeof(*p));
    if (!s || !p) {
        free(s);
        free(p);
        return NULL;
    }
    s->priv = p;
    s->open = avi_sink_open;
    s->write_video = avi_sink_write_video;
    s->write_audio = avi_sink_write_audio;
    s->close = avi_sink_close;
    s->get_status = avi_sink_get_status;
    s->supports_title = 1;
    return s;
}
