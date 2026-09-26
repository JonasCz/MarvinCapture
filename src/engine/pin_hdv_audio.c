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

#include "pin_hdv_audio.h"
#include "pin_audio_resample.h"

#include <libavcodec/avcodec.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define PIN_HDV_AUDIO_QUEUE_CAP 6
#define HDV_TS_PKT 188

typedef struct {
    uint8_t *data;
    size_t n_packets;
    int audio_pid;
} hdv_audio_job_t;

struct pin_hdv_audio {
    pthread_t thread;
    int started;
    volatile int stop;

    pthread_mutex_t mtx;
    pthread_cond_t cond;
    hdv_audio_job_t queue[PIN_HDV_AUDIO_QUEUE_CAP];
    unsigned head, count;

    pin_hdv_audio_feed_cb cb;
    void *user;

    const AVCodec *codec;
    AVCodecContext *ctx;
    int last_pid; /* audio_pid the ctx was opened for; -1 if not open yet */
    pin_resampler_t resampler;

    uint8_t *es_buf;
    size_t es_cap;
};

/* Same TS/PES stripping pin_preview.c's extract_es() does for video, kept
 * as its own small copy here rather than shared (see pin_preview.c's own
 * comment on why it doesn't share hdv_aux.c's metadata-only helpers --
 * same reasoning applies to sharing between preview and this module: two
 * ~20-line, single-purpose static functions are cheaper to keep separate
 * than to design a shared API around). */
static size_t extract_es(pin_hdv_audio_t *a, const uint8_t *ts, size_t n_packets, int pid)
{
    size_t out_len = 0;
    int seen_start = 0;
    for (size_t i = 0; i < n_packets; i++) {
        const uint8_t *pkt = ts + i * HDV_TS_PKT;
        if (pkt[0] != 0x47)
            continue;
        int this_pid = ((pkt[1] & 0x1f) << 8) | pkt[2];
        if (this_pid != pid)
            continue;
        int pusi = (pkt[1] & 0x40) != 0;
        int afc = (pkt[3] >> 4) & 0x3;
        const uint8_t *payload = pkt + 4;
        int avail = 184;
        if (afc == 2)
            continue;
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
        if (out_len + (size_t)avail + 4096 > a->es_cap) {
            size_t want = out_len + (size_t)avail + 4096;
            uint8_t *nb = realloc(a->es_buf, want);
            if (!nb)
                break;
            a->es_buf = nb;
            a->es_cap = want;
        }
        memcpy(a->es_buf + out_len, payload, (size_t)avail);
        out_len += (size_t)avail;
    }
    return out_len;
}

/* Converts one decoded AVFrame (whatever sample format the mp2 decoder
 * produced -- no swresample available, see pin_audio_resample.h) to
 * interleaved stereo s16, resamples to 48 kHz and feeds the callback.
 * Mono is duplicated to both channels; more than 2 channels only uses the
 * first 2 -- HDV audio is always mono or stereo in practice. */
static void handle_frame(pin_hdv_audio_t *a, const AVFrame *f)
{
    int ch = f->ch_layout.nb_channels;
    if (ch <= 0)
        return;
    int nb = f->nb_samples;
    if (nb <= 0)
        return;

    int16_t *tmp = malloc((size_t)nb * 2 * sizeof(int16_t));
    if (!tmp)
        return;

    int planar = av_sample_fmt_is_planar(f->format);
    for (int i = 0; i < nb; i++) {
        for (int outc = 0; outc < 2; outc++) {
            int srcc = ch == 1 ? 0 : (outc < ch ? outc : ch - 1);
            const uint8_t *plane = planar ? f->extended_data[srcc] : f->extended_data[0];
            int stride = planar ? 1 : ch;
            int idx = planar ? i : i * ch + srcc;
            double v = 0;
            switch (f->format) {
            case AV_SAMPLE_FMT_S16:
            case AV_SAMPLE_FMT_S16P:
                v = ((const int16_t *)plane)[planar ? idx : idx];
                break;
            case AV_SAMPLE_FMT_S32:
            case AV_SAMPLE_FMT_S32P:
                v = ((const int32_t *)plane)[idx] / 65536.0;
                break;
            case AV_SAMPLE_FMT_FLT:
            case AV_SAMPLE_FMT_FLTP:
                v = ((const float *)plane)[idx] * 32768.0;
                break;
            case AV_SAMPLE_FMT_DBL:
            case AV_SAMPLE_FMT_DBLP:
                v = ((const double *)plane)[idx] * 32768.0;
                break;
            default:
                v = 0;
                break;
            }
            (void)stride;
            if (v > 32767.0) v = 32767.0;
            if (v < -32768.0) v = -32768.0;
            tmp[i * 2 + outc] = (int16_t)v;
        }
    }

    if (f->sample_rate == 48000) {
        a->cb(a->user, tmp, (unsigned)nb);
    } else {
        int16_t out[4096];
        size_t written = pin_resample_s16_stereo(&a->resampler, tmp, (size_t)nb, f->sample_rate,
                                                  out, sizeof(out) / sizeof(out[0]) / 2, 48000);
        if (written)
            a->cb(a->user, out, (unsigned)written);
    }
    free(tmp);
}

static void process_job(pin_hdv_audio_t *a, hdv_audio_job_t *job)
{
    if (job->audio_pid <= 0)
        return;
    if (!a->ctx || a->last_pid != job->audio_pid) {
        if (a->ctx)
            avcodec_free_context(&a->ctx);
        if (!a->codec)
            a->codec = avcodec_find_decoder(AV_CODEC_ID_MP2);
        if (!a->codec)
            return;
        a->ctx = avcodec_alloc_context3(a->codec);
        if (!a->ctx || avcodec_open2(a->ctx, a->codec, NULL) < 0)
            return;
        a->last_pid = job->audio_pid;
        pin_resampler_reset(&a->resampler);
    }

    size_t es_len = extract_es(a, job->data, job->n_packets, job->audio_pid);
    if (es_len == 0)
        return;

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!pkt || !frame)
        goto out;
    pkt->data = a->es_buf;
    pkt->size = (int)es_len;
    if (avcodec_send_packet(a->ctx, pkt) == 0) {
        while (avcodec_receive_frame(a->ctx, frame) == 0)
            handle_frame(a, frame);
    }
out:
    av_frame_free(&frame);
    av_packet_free(&pkt);
}

static void *hdv_audio_thread(void *arg)
{
    pin_hdv_audio_t *a = arg;
    for (;;) {
        pthread_mutex_lock(&a->mtx);
        while (!a->stop && a->count == 0)
            pthread_cond_wait(&a->cond, &a->mtx);
        if (a->stop && a->count == 0) {
            pthread_mutex_unlock(&a->mtx);
            break;
        }
        hdv_audio_job_t job = a->queue[a->head];
        a->head = (a->head + 1) % PIN_HDV_AUDIO_QUEUE_CAP;
        a->count--;
        pthread_mutex_unlock(&a->mtx);

        process_job(a, &job);
        free(job.data);
    }
    return NULL;
}

pin_hdv_audio_t *pin_hdv_audio_create(pin_hdv_audio_feed_cb cb, void *user)
{
    pin_hdv_audio_t *a = calloc(1, sizeof(*a));
    if (!a)
        return NULL;
    a->cb = cb;
    a->user = user;
    a->last_pid = -1;
    pthread_mutex_init(&a->mtx, NULL);
    pthread_cond_init(&a->cond, NULL);
    if (pthread_create(&a->thread, NULL, hdv_audio_thread, a) != 0) {
        pthread_mutex_destroy(&a->mtx);
        pthread_cond_destroy(&a->cond);
        free(a);
        return NULL;
    }
    a->started = 1;
    return a;
}

void pin_hdv_audio_destroy(pin_hdv_audio_t *a)
{
    if (!a)
        return;
    if (a->started) {
        pthread_mutex_lock(&a->mtx);
        a->stop = 1;
        pthread_cond_signal(&a->cond);
        pthread_mutex_unlock(&a->mtx);
        pthread_join(a->thread, NULL);
    }
    for (unsigned i = 0; i < a->count; i++)
        free(a->queue[(a->head + i) % PIN_HDV_AUDIO_QUEUE_CAP].data);
    if (a->ctx)
        avcodec_free_context(&a->ctx);
    free(a->es_buf);
    pthread_mutex_destroy(&a->mtx);
    pthread_cond_destroy(&a->cond);
    free(a);
}

void pin_hdv_audio_push(pin_hdv_audio_t *a, const uint8_t *ts_packets, size_t n_packets,
                         int audio_pid)
{
    if (!a || !ts_packets || n_packets == 0 || audio_pid <= 0)
        return;
    size_t len = n_packets * HDV_TS_PKT;
    uint8_t *copy = malloc(len);
    if (!copy)
        return;
    memcpy(copy, ts_packets, len);

    pthread_mutex_lock(&a->mtx);
    if (a->count == PIN_HDV_AUDIO_QUEUE_CAP) {
        /* Drop the oldest queued job -- monitor audio only, never blocks
         * the pusher (dv_on_unit(), on the USB read loop / replay thread). */
        hdv_audio_job_t *oldest = &a->queue[a->head];
        free(oldest->data);
        a->head = (a->head + 1) % PIN_HDV_AUDIO_QUEUE_CAP;
        a->count--;
    }
    unsigned tail = (a->head + a->count) % PIN_HDV_AUDIO_QUEUE_CAP;
    a->queue[tail].data = copy;
    a->queue[tail].n_packets = n_packets;
    a->queue[tail].audio_pid = audio_pid;
    a->count++;
    pthread_cond_signal(&a->cond);
    pthread_mutex_unlock(&a->mtx);
}
