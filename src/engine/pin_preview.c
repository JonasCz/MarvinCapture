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

#include "pin_preview.h"

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum { PV_IN_NONE = 0, PV_IN_ANALOG, PV_IN_DV, PV_IN_HDV } pv_in_kind_t;

typedef struct {
    uint8_t *y, *u, *v;
    size_t y_cap, u_cap, v_cap;
    int w, h;
    int stride[3];
    int shift_x, shift_y;
    pin_matrix_t matrix;
    int full_range;
    int dar_num, dar_den;
    int interlaced, tff;
    int valid;
} pv_buf_t;

struct pin_preview {
    pthread_t thread;
    int started;
    volatile int stop;
    int enabled;

    pthread_mutex_t in_mtx;
    pthread_cond_t in_cond;
    pv_in_kind_t pending_kind;
    uint8_t *pending_buf;
    size_t pending_len, pending_cap;
    unsigned pending_w, pending_h;
    int pending_is_pal;
    int pending_video_pid;

    pthread_mutex_t out_mtx;
    pthread_cond_t out_cond;
    pv_buf_t bufs[3];
    int ready_idx;   /* -1 or the newest fully-decoded buffer */
    int locked_idx;  /* -1 or the buffer currently on loan via pin_previewer_lock */
    uint64_t seq;

    pin_aspect_t aspect_override;

    const AVCodec *dv_codec, *hdv_codec;
    AVCodecContext *dv_ctx, *hdv_ctx;
    uint8_t *hdv_es;
    size_t hdv_es_cap;
};

pin_preview_t *pin_previewer_create(void)
{
    pin_preview_t *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    pthread_mutex_init(&p->in_mtx, NULL);
    pthread_cond_init(&p->in_cond, NULL);
    pthread_mutex_init(&p->out_mtx, NULL);
    pthread_cond_init(&p->out_cond, NULL);
    p->ready_idx = -1;
    p->locked_idx = -1;
    p->enabled = 1;
    p->aspect_override = PIN_ASPECT_AUTO;
    return p;
}

static void free_buf(pv_buf_t *b)
{
    free(b->y); free(b->u); free(b->v);
    memset(b, 0, sizeof(*b));
}

static void *pv_thread(void *arg);

static pin_status_t ensure_started(pin_preview_t *p)
{
    if (p->started)
        return PIN_OK;
    p->stop = 0;
    if (pthread_create(&p->thread, NULL, pv_thread, p) != 0)
        return PIN_ERR_INTERNAL;
    p->started = 1;
    return PIN_OK;
}

void pin_previewer_destroy(pin_preview_t *p)
{
    if (!p)
        return;
    if (p->started) {
        pthread_mutex_lock(&p->in_mtx);
        p->stop = 1;
        pthread_cond_signal(&p->in_cond);
        pthread_mutex_unlock(&p->in_mtx);
        pthread_join(p->thread, NULL);
    }
    if (p->dv_ctx) avcodec_free_context(&p->dv_ctx);
    if (p->hdv_ctx) avcodec_free_context(&p->hdv_ctx);
    free(p->pending_buf);
    free(p->hdv_es);
    for (int i = 0; i < 3; i++)
        free_buf(&p->bufs[i]);
    pthread_mutex_destroy(&p->in_mtx);
    pthread_cond_destroy(&p->in_cond);
    pthread_mutex_destroy(&p->out_mtx);
    pthread_cond_destroy(&p->out_cond);
    free(p);
}

void pin_previewer_enable(pin_preview_t *p, int enabled)
{
    if (!p)
        return;
    pthread_mutex_lock(&p->in_mtx);
    p->enabled = enabled != 0;
    pthread_mutex_unlock(&p->in_mtx);
}

void pin_previewer_set_aspect_override(pin_preview_t *p, pin_aspect_t aspect)
{
    if (!p)
        return;
    pthread_mutex_lock(&p->out_mtx);
    p->aspect_override = aspect;
    pthread_mutex_unlock(&p->out_mtx);
}

static void push_pending(pin_preview_t *p, pv_in_kind_t kind, const uint8_t *data, size_t len,
                          unsigned w, unsigned h, int is_pal, int video_pid)
{
    if (ensure_started(p) != PIN_OK)
        return;
    pthread_mutex_lock(&p->in_mtx);
    if (!p->enabled) {
        pthread_mutex_unlock(&p->in_mtx);
        return;
    }
    if (len > p->pending_cap) {
        uint8_t *nb = realloc(p->pending_buf, len);
        if (!nb) {
            pthread_mutex_unlock(&p->in_mtx);
            return;
        }
        p->pending_buf = nb;
        p->pending_cap = len;
    }
    memcpy(p->pending_buf, data, len);
    p->pending_len = len;
    p->pending_kind = kind;
    p->pending_w = w;
    p->pending_h = h;
    p->pending_is_pal = is_pal;
    p->pending_video_pid = video_pid;
    pthread_cond_signal(&p->in_cond);
    pthread_mutex_unlock(&p->in_mtx);
}

void pin_previewer_push_analog(pin_preview_t *p, const uint8_t *yuyv, unsigned width,
                              unsigned height, int is_pal)
{
    if (!p) return;
    push_pending(p, PV_IN_ANALOG, yuyv, (size_t)width * height * 2, width, height, is_pal, 0);
}

void pin_previewer_push_dv(pin_preview_t *p, const uint8_t *frame, size_t len, int is_pal)
{
    if (!p) return;
    push_pending(p, PV_IN_DV, frame, len, 0, 0, is_pal, 0);
}

void pin_previewer_push_hdv(pin_preview_t *p, const uint8_t *ts_packets, size_t n_packets,
                           int video_pid)
{
    if (!p) return;
    push_pending(p, PV_IN_HDV, ts_packets, n_packets * 188, 0, 0, 0, video_pid);
}

/* --- output side ----------------------------------------------------------
 * Triple buffer: the decode thread always writes into a slot that is
 * neither `ready_idx` (the one a caller might lock next) nor `locked_idx`
 * (the one a caller currently holds), so pin_previewer_lock() never blocks
 * the decoder and the decoder never overwrites what a caller is reading. */

static int pick_write_slot(pin_preview_t *p)
{
    for (int i = 0; i < 3; i++)
        if (i != p->ready_idx && i != p->locked_idx)
            return i;
    return 0; /* unreachable with 3 slots and at most 2 reserved */
}

static int ensure_cap(uint8_t **buf, size_t *cap, size_t need)
{
    if (*cap >= need)
        return 1;
    uint8_t *nb = realloc(*buf, need);
    if (!nb)
        return 0;
    *buf = nb;
    *cap = need;
    return 1;
}

static void publish_from_avframe(pin_preview_t *p, const AVFrame *f, pin_matrix_t matrix,
                                  int dar_num, int dar_den, int interlaced, int tff)
{
    int shift_x = 1, shift_y = 1;
    if (f->format == AV_PIX_FMT_YUV411P) { shift_x = 2; shift_y = 0; }
    else if (f->format == AV_PIX_FMT_YUV422P) { shift_x = 1; shift_y = 0; }
    else if (f->format == AV_PIX_FMT_YUV420P) { shift_x = 1; shift_y = 1; }
    else return; /* unsupported output format from the decoder: drop this frame */

    pthread_mutex_lock(&p->out_mtx);
    int idx = pick_write_slot(p);
    pv_buf_t *b = &p->bufs[idx];
    int cw = f->width >> shift_x, ch = f->height >> shift_y;
    if (!ensure_cap(&b->y, &b->y_cap, (size_t)f->width * f->height) ||
        !ensure_cap(&b->u, &b->u_cap, (size_t)cw * ch) ||
        !ensure_cap(&b->v, &b->v_cap, (size_t)cw * ch)) {
        pthread_mutex_unlock(&p->out_mtx);
        return;
    }
    for (int row = 0; row < f->height; row++)
        memcpy(b->y + (size_t)row * f->width, f->data[0] + (size_t)row * f->linesize[0], (size_t)f->width);
    for (int row = 0; row < ch; row++) {
        memcpy(b->u + (size_t)row * cw, f->data[1] + (size_t)row * f->linesize[1], (size_t)cw);
        memcpy(b->v + (size_t)row * cw, f->data[2] + (size_t)row * f->linesize[2], (size_t)cw);
    }
    b->w = f->width; b->h = f->height;
    b->stride[0] = f->width; b->stride[1] = cw; b->stride[2] = cw;
    b->shift_x = shift_x; b->shift_y = shift_y;
    b->matrix = matrix;
    b->full_range = 0;
    b->dar_num = dar_num; b->dar_den = dar_den;
    b->interlaced = interlaced; b->tff = tff;
    b->valid = 1;

    p->ready_idx = idx;
    p->seq++;
    pthread_cond_broadcast(&p->out_cond);
    pthread_mutex_unlock(&p->out_mtx);
}

static void decode_analog(pin_preview_t *p, const uint8_t *yuyv, unsigned w, unsigned h)
{
    pthread_mutex_lock(&p->out_mtx);
    int idx = pick_write_slot(p);
    pv_buf_t *b = &p->bufs[idx];
    size_t cw = w / 2, ch = h;
    if (!ensure_cap(&b->y, &b->y_cap, (size_t)w * h) ||
        !ensure_cap(&b->u, &b->u_cap, cw * ch) ||
        !ensure_cap(&b->v, &b->v_cap, cw * ch)) {
        pthread_mutex_unlock(&p->out_mtx);
        return;
    }
    for (unsigned row = 0; row < h; row++) {
        const uint8_t *src = yuyv + (size_t)row * w * 2;
        uint8_t *yd = b->y + (size_t)row * w;
        uint8_t *ud = b->u + (size_t)row * cw;
        uint8_t *vd = b->v + (size_t)row * cw;
        for (unsigned x = 0; x < w / 2; x++) {
            yd[2 * x]     = src[4 * x + 0];
            ud[x]         = src[4 * x + 1];
            yd[2 * x + 1] = src[4 * x + 2];
            vd[x]         = src[4 * x + 3];
        }
    }
    b->w = (int)w; b->h = (int)h;
    b->stride[0] = (int)w; b->stride[1] = (int)cw; b->stride[2] = (int)cw;
    b->shift_x = 1; b->shift_y = 0;
    b->matrix = PIN_MATRIX_BT601;
    b->full_range = 0;
    pin_aspect_t asp = p->aspect_override;
    if (asp == PIN_ASPECT_16_9) { b->dar_num = 16; b->dar_den = 9; }
    else { b->dar_num = 4; b->dar_den = 3; }
    b->interlaced = 1; b->tff = 1;
    b->valid = 1;
    p->ready_idx = idx;
    p->seq++;
    pthread_cond_broadcast(&p->out_cond);
    pthread_mutex_unlock(&p->out_mtx);
}

static void decode_dv(pin_preview_t *p, const uint8_t *data, size_t len, int is_pal)
{
    if (!p->dv_ctx) {
        p->dv_codec = avcodec_find_decoder(AV_CODEC_ID_DVVIDEO);
        if (!p->dv_codec)
            return;
        p->dv_ctx = avcodec_alloc_context3(p->dv_codec);
        if (!p->dv_ctx || avcodec_open2(p->dv_ctx, p->dv_codec, NULL) < 0)
            return;
    }
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!pkt || !frame)
        goto out;
    pkt->data = (uint8_t *)data;
    pkt->size = (int)len;
    if (avcodec_send_packet(p->dv_ctx, pkt) == 0) {
        if (avcodec_receive_frame(p->dv_ctx, frame) == 0) {
            pin_aspect_t asp = p->aspect_override;
            int dn = 4, dd = 3;
            if (asp == PIN_ASPECT_16_9) { dn = 16; dd = 9; }
            publish_from_avframe(p, frame, PIN_MATRIX_BT601, dn, dd, 0, 0);
        }
    }
    (void)is_pal;
out:
    av_frame_free(&frame);
    av_packet_free(&pkt);
}

/* Minimal TS -> PID elementary-stream extraction, preview-only (see
 * pin_preview.h header comment for why this doesn't share hdv_aux.c's
 * private, metadata-focused helpers). Strips the 4-byte TS header and any
 * adaptation field; on the packet carrying payload_unit_start_indicator it
 * also strips the PES header (9 fixed bytes + PES_header_data_length more).
 * Best-effort: garbage in yields no decoded frame out, never a crash. */
static size_t extract_es(pin_preview_t *p, const uint8_t *ts, size_t n_packets, int pid)
{
    size_t out_len = 0;
    int seen_start = 0;
    for (size_t i = 0; i < n_packets; i++) {
        const uint8_t *pkt = ts + i * 188;
        if (pkt[0] != 0x47)
            continue;
        int this_pid = ((pkt[1] & 0x1f) << 8) | pkt[2];
        if (this_pid != pid)
            continue;
        int pusi = (pkt[1] & 0x40) != 0;
        int afc = (pkt[3] >> 4) & 0x3;
        const uint8_t *payload = pkt + 4;
        int avail = 184;
        if (afc == 2) { continue; }               /* adaptation field only, no payload */
        if (afc == 3) {
            int af_len = payload[0];
            payload += 1 + af_len;
            avail -= 1 + af_len;
        }
        if (avail <= 0)
            continue;
        if (pusi) {
            seen_start = 1;
            if (avail >= 9 && payload[0] == 0 && payload[1] == 0 && payload[2] == 1) {
                int hdr_data_len = payload[8];
                int skip = 9 + hdr_data_len;
                if (skip < avail) {
                    payload += skip;
                    avail -= skip;
                } else {
                    avail = 0;
                }
            }
        }
        if (!seen_start || avail <= 0)
            continue;
        if (!ensure_cap(&p->hdv_es, &p->hdv_es_cap, out_len + (size_t)avail + 4096))
            break;
        memcpy(p->hdv_es + out_len, payload, (size_t)avail);
        out_len += (size_t)avail;
    }
    return out_len;
}

static void decode_hdv(pin_preview_t *p, const uint8_t *ts_packets, size_t len, int video_pid)
{
    if (video_pid <= 0)
        return;
    if (!p->hdv_ctx) {
        p->hdv_codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
        if (!p->hdv_codec)
            return;
        p->hdv_ctx = avcodec_alloc_context3(p->hdv_codec);
        if (!p->hdv_ctx || avcodec_open2(p->hdv_ctx, p->hdv_codec, NULL) < 0)
            return;
    }
    size_t n_packets = len / 188;
    size_t es_len = extract_es(p, ts_packets, n_packets, video_pid);
    if (es_len == 0)
        return;

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!pkt || !frame)
        goto out;
    pkt->data = p->hdv_es;
    pkt->size = (int)es_len;
    if (avcodec_send_packet(p->hdv_ctx, pkt) == 0) {
        while (avcodec_receive_frame(p->hdv_ctx, frame) == 0) {
            pin_aspect_t asp = p->aspect_override;
            int dn = 16, dd = 9;
            if (asp == PIN_ASPECT_4_3) { dn = 4; dd = 3; }
            /* BT.709, 1080i top-field-first, per the plan. */
            publish_from_avframe(p, frame, PIN_MATRIX_BT709, dn, dd, 1, 1);
        }
    }
out:
    av_frame_free(&frame);
    av_packet_free(&pkt);
}

static void *pv_thread(void *arg)
{
    pin_preview_t *p = arg;
    uint8_t *work = NULL;
    size_t work_cap = 0;

    for (;;) {
        pthread_mutex_lock(&p->in_mtx);
        while (!p->stop && (p->pending_kind == PV_IN_NONE || !p->enabled)) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100000000L; /* 100 ms: also re-checks `enabled` promptly */
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&p->in_cond, &p->in_mtx, &ts);
        }
        if (p->stop) {
            pthread_mutex_unlock(&p->in_mtx);
            break;
        }
        pv_in_kind_t kind = p->pending_kind;
        size_t len = p->pending_len;
        unsigned w = p->pending_w, h = p->pending_h;
        int is_pal = p->pending_is_pal, pid = p->pending_video_pid;
        if (len > work_cap) {
            uint8_t *nb = realloc(work, len);
            if (nb) { work = nb; work_cap = len; }
        }
        if (work && len <= work_cap)
            memcpy(work, p->pending_buf, len);
        p->pending_kind = PV_IN_NONE; /* consumed: drop-if-busy applies to the NEXT push */
        pthread_mutex_unlock(&p->in_mtx);

        if (!work)
            continue;
        switch (kind) {
        case PV_IN_ANALOG: decode_analog(p, work, w, h); break;
        case PV_IN_DV:      decode_dv(p, work, len, is_pal); break;
        case PV_IN_HDV:      decode_hdv(p, work, len, pid); break;
        default: break;
        }
    }
    free(work);
    return NULL;
}

int pin_previewer_wait(pin_preview_t *p, uint64_t after_seq, int timeout_ms)
{
    if (!p)
        return -1;
    pthread_mutex_lock(&p->out_mtx);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    int rc = 0;
    while (p->seq <= after_seq) {
        int w = pthread_cond_timedwait(&p->out_cond, &p->out_mtx, &ts);
        if (w != 0) { rc = 0; goto done; }
    }
    rc = 1;
done:
    pthread_mutex_unlock(&p->out_mtx);
    return rc;
}

pin_status_t pin_previewer_lock(pin_preview_t *p, pin_frame_t *out)
{
    if (!p)
        return PIN_ERR_STATE;
    pthread_mutex_lock(&p->out_mtx);
    if (p->ready_idx < 0 || !p->bufs[p->ready_idx].valid) {
        pthread_mutex_unlock(&p->out_mtx);
        return PIN_ERR_STATE;
    }
    p->locked_idx = p->ready_idx;
    pv_buf_t *b = &p->bufs[p->locked_idx];
    out->size = sizeof(*out);
    out->seq = p->seq;
    out->width = b->w; out->height = b->h;
    out->chroma_shift_x = b->shift_x; out->chroma_shift_y = b->shift_y;
    out->plane[0] = b->y; out->plane[1] = b->u; out->plane[2] = b->v;
    out->stride[0] = b->stride[0]; out->stride[1] = b->stride[1]; out->stride[2] = b->stride[2];
    out->matrix = b->matrix;
    out->full_range = b->full_range;
    out->dar_num = b->dar_num; out->dar_den = b->dar_den;
    out->interlaced = b->interlaced; out->top_field_first = b->tff;
    pthread_mutex_unlock(&p->out_mtx);
    return PIN_OK;
}

void pin_previewer_unlock(pin_preview_t *p)
{
    if (!p)
        return;
    pthread_mutex_lock(&p->out_mtx);
    p->locked_idx = -1;
    pthread_mutex_unlock(&p->out_mtx);
}
