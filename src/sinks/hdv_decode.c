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
 * The second stage of the HDV pipeline: every picture decoded, the decoder's verdict per
 * picture (hdv_decode.h; hdv_demux.h has the whole picture).
 */

#include "hdv_decode.h"

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>

#include <stdlib.h>
#include <string.h>

struct hdv_decoder {
    AVCodecContext *ctx;
    hdv_gop_t *inflight;       /* fed, not all frames back yet (in order) */
    int64_t max_out_id;        /* the latest picture the decoder gave back */
    int flushed;
};

hdv_decoder_t *hdv_decoder_create(int threads)
{
    hdv_decoder_t *d = calloc(1, sizeof(*d));
    const AVCodec *dc = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    if (!d || !dc || !(d->ctx = avcodec_alloc_context3(dc))) {
        free(d);
        return NULL;
    }
    d->ctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;   /* a concealed picture comes back, flagged */
    d->ctx->thread_count = threads > 0 ? threads : 1;
    d->ctx->thread_type = FF_THREAD_SLICE;          /* frames come back in order, without delay */
    if (avcodec_open2(d->ctx, dc, NULL) < 0) {
        avcodec_free_context(&d->ctx);
        free(d);
        return NULL;
    }
    d->max_out_id = -1;
    return d;
}

/* A frame back: to its GOP's picture (its pts is the picture's id). */
static void route_frame(hdv_decoder_t *d, AVFrame *f)
{
    int64_t id = f->pts;
    for (hdv_gop_t *g = d->inflight; g; g = g->next) {
        if (id >= g->first_id && id < g->first_id + g->pics.n) {
            int i = (int)(id - g->first_id);
            if (!g->frames[i]) {
                g->frames[i] = f;
                f = NULL;
            }
            break;
        }
    }
    if (id > d->max_out_id)
        d->max_out_id = id;
    av_frame_free(&f);
}

static void receive_frames(hdv_decoder_t *d)
{
    for (;;) {
        AVFrame *f = av_frame_alloc();
        if (!f || avcodec_receive_frame(d->ctx, f) < 0) {
            av_frame_free(&f);
            return;
        }
        route_frame(d, f);
    }
}

void hdv_decoder_feed(hdv_decoder_t *d, hdv_gop_t *g)
{
    g->next = NULL;
    hdv_gop_t **tail = &d->inflight;
    while (*tail)
        tail = &(*tail)->next;
    *tail = g;
    AVPacket *pkt = av_packet_alloc();
    for (int i = 0; i < g->pics.n && pkt; i++) {
        const hdv_pic_t *p = &g->pics.p[i];
        if (av_new_packet(pkt, (int)p->len) < 0)
            break;
        memcpy(pkt->data, p->data, p->len);
        pkt->pts = g->first_id + i;
        int rc = avcodec_send_packet(d->ctx, pkt);
        av_packet_unref(pkt);
        if (rc < 0 && rc != AVERROR(EAGAIN))
            g->dec_error = 1;      /* refused: it will not come back either */
        receive_frames(d);
    }
    av_packet_free(&pkt);
}

hdv_gop_t *hdv_decoder_ready(hdv_decoder_t *d)
{
    hdv_gop_t *g = d->inflight;
    if (!g || (!d->flushed && d->max_out_id < g->first_id + g->pics.n))
        return NULL;
    d->inflight = g->next;
    g->next = NULL;
    return g;
}

void hdv_decoder_flush(hdv_decoder_t *d)
{
    avcodec_send_packet(d->ctx, NULL);
    receive_frames(d);
    d->flushed = 1;
}

void hdv_decoder_free(hdv_decoder_t *d)
{
    if (!d)
        return;
    while (d->inflight) {
        hdv_gop_t *g = d->inflight;
        d->inflight = g->next;
        hdv_gop_free(g);
    }
    avcodec_free_context(&d->ctx);
    free(d);
}

int hdv_pic_ok(const hdv_gop_t *g, int i)
{
    const AVFrame *f = g->frames[i];
    return f && !f->decode_error_flags && !(f->flags & AV_FRAME_FLAG_CORRUPT);
}
