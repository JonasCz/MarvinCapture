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
 * PIN_FMT_ANALOG_FFV1_MKV: analog capture losslessly to FFV1 level 3 (slice
 * threaded, slice CRC) + pcm_s16le, muxed as Matroska, via libavformat /
 * libavcodec.
 *
 * The pin_writer consumer thread (see pin_writer.h) calls write_video()
 * once per captured YUYV frame; that thread must not stall waiting for a
 * slow FFV1 encode (falling behind here must not make pin_writer start
 * dropping *capture* data upstream of the sink). So write_video() only
 * converts YUYV -> planar YUV422P (a cheap deinterleave, no scaling maths)
 * and hands the plane off to a small bounded queue; a dedicated encoder
 * thread does the actual avcodec_send_frame/receive_packet + mux write.
 * If that queue is full the frame is dropped and counted as
 * st.encoder_behind, exactly like pin_writer's own overflow handling one
 * layer up.
 *
 * Audio needs no such encode step (pcm_s16le is a repack, not a codec), so
 * write_audio() muxes directly from the caller's thread; av_mux_lock
 * serialises it against the encoder thread's writes into the same
 * AVFormatContext (libavformat's own interleaving queue is not thread-safe
 * across independent writers).
 */

#include "pin_sink.h"
#include "sinks_internal.h"
#include "../core/pin_log.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#define QUEUE_DEPTH 8

typedef struct {
    AVFormatContext *fmt;
    AVStream *vst, *ast;
    AVCodecContext *venc;
    pthread_mutex_t mux_lock;

    int width, height;
    int audio_channels;
    int64_t vpts, apts;   /* per-instance timelines -- see write_video/write_audio */

    /* bounded frame queue -> encoder thread */
    AVFrame *queue[QUEUE_DEPTH];
    int qhead, qtail, qcount;
    int closing;
    pthread_mutex_t qlock;
    pthread_cond_t qwake, qspace;
    pthread_t enc_thread;
    int enc_started;

    pin_sink_status_t st;
    pthread_mutex_t st_lock;
} ffv1_priv_t;

static int cpu_count(void)
{
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 4;
#endif
}

static void set_error(ffv1_priv_t *p, pin_status_t e)
{
    pthread_mutex_lock(&p->st_lock);
    p->st.last_error = e;
    pthread_mutex_unlock(&p->st_lock);
}

/* Encodes and muxes one already-converted frame. Called only on the encoder
 * thread. */
static void encode_one(ffv1_priv_t *p, AVFrame *frame)
{
    int rc = avcodec_send_frame(p->venc, frame);
    if (rc < 0) {
        pin_logf(PIN_LOG_ERROR, "sink_ffv1: avcodec_send_frame failed (%d)\n", rc);
        set_error(p, PIN_ERR_CODEC);
        return;
    }
    for (;;) {
        AVPacket *pkt = av_packet_alloc();
        rc = avcodec_receive_packet(p->venc, pkt);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
            av_packet_free(&pkt);
            break;
        }
        if (rc < 0) {
            av_packet_free(&pkt);
            pin_logf(PIN_LOG_ERROR, "sink_ffv1: avcodec_receive_packet failed (%d)\n", rc);
            set_error(p, PIN_ERR_CODEC);
            break;
        }
        av_packet_rescale_ts(pkt, p->venc->time_base, p->vst->time_base);
        pkt->stream_index = p->vst->index;
        size_t pkt_size = (size_t)pkt->size;
        pthread_mutex_lock(&p->mux_lock);
        rc = av_interleaved_write_frame(p->fmt, pkt);
        pthread_mutex_unlock(&p->mux_lock);
        av_packet_free(&pkt);
        if (rc < 0) {
            pin_logf(PIN_LOG_ERROR, "sink_ffv1: mux write failed (%d)\n", rc);
            set_error(p, PIN_ERR_IO);
            break;
        }
        pthread_mutex_lock(&p->st_lock);
        p->st.units_written++;
        p->st.bytes_written += pkt_size;
        pthread_mutex_unlock(&p->st_lock);
    }
}

static void *encoder_thread_fn(void *arg)
{
    ffv1_priv_t *p = arg;
    pthread_mutex_lock(&p->qlock);
    for (;;) {
        while (p->qcount == 0 && !p->closing)
            pthread_cond_wait(&p->qwake, &p->qlock);
        if (p->qcount == 0 && p->closing)
            break;
        AVFrame *frame = p->queue[p->qhead];
        p->qhead = (p->qhead + 1) % QUEUE_DEPTH;
        p->qcount--;
        pthread_cond_signal(&p->qspace);
        pthread_mutex_unlock(&p->qlock);

        encode_one(p, frame);
        av_frame_free(&frame);

        pthread_mutex_lock(&p->qlock);
    }
    pthread_mutex_unlock(&p->qlock);
    /* Flush: send a NULL frame to drain any B/lookahead-free but still
     * buffered packets (FFV1 with g=1 has none, but this stays correct if
     * that ever changes). */
    encode_one(p, NULL);
    return NULL;
}

static pin_status_t ffv1_open(pin_sink_t *s, const char *path, const pin_sink_params_t *params)
{
    ffv1_priv_t *p = s->priv;
    if (params->kind != PIN_KIND_ANALOG)
        return PIN_ERR_ARG;

    p->width = params->width;
    p->height = params->height;
    p->audio_channels = params->audio_channels;

    int rc = avformat_alloc_output_context2(&p->fmt, NULL, "matroska", path);
    if (rc < 0 || !p->fmt) {
        pin_logf(PIN_LOG_ERROR, "sink_ffv1: avformat_alloc_output_context2 failed (%d)\n", rc);
        return PIN_ERR_CODEC;
    }

    const AVCodec *venc_codec = avcodec_find_encoder(AV_CODEC_ID_FFV1);
    if (!venc_codec)
        return PIN_ERR_CODEC;
    p->vst = avformat_new_stream(p->fmt, NULL);
    p->venc = avcodec_alloc_context3(venc_codec);
    if (!p->vst || !p->venc)
        return PIN_ERR_NOMEM;

    p->venc->width = params->width;
    p->venc->height = params->height;
    p->venc->pix_fmt = AV_PIX_FMT_YUV422P;
    p->venc->time_base = (AVRational){ params->fps_den, params->fps_num };
    p->venc->framerate = (AVRational){ params->fps_num, params->fps_den };
    p->venc->gop_size = 1;                          /* every frame a keyframe */
    /* Derived from params->aspect + geometry here, rather than trusting
     * params->sample_aspect_num/den as pre-computed by the caller: a
     * non-AUTO aspect (4:3 or 16:9 override, e.g. from pin_set_aspect())
     * must reach the file's SAR/DAR regardless of whether every caller
     * remembered to call pin_sink_sar_for() itself first. is_pal comes
     * from the frame rate, matching how every other analog geometry
     * decision in this codebase (avi_writer.c's own PAL/NTSC branch, for
     * one) already infers it. */
    {
        int is_pal = params->fps_num * 1 == params->fps_den * 25; /* 25/1 vs 30000/1001 */
        int sn, sd;
        pin_sink_sar_for(PIN_KIND_ANALOG, is_pal, params->aspect, params->width, &sn, &sd);
        p->venc->sample_aspect_ratio = (AVRational){ sn, sd };
    }
    p->venc->color_range = AVCOL_RANGE_MPEG;
    p->venc->color_primaries =
        params->colour_matrix == PIN_MATRIX_BT709 ? AVCOL_PRI_BT709 : AVCOL_PRI_BT470BG;
    p->venc->color_trc =
        params->colour_matrix == PIN_MATRIX_BT709 ? AVCOL_TRC_BT709 : AVCOL_TRC_GAMMA28;
    p->venc->colorspace =
        params->colour_matrix == PIN_MATRIX_BT709 ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
    p->venc->field_order = AV_FIELD_TT;              /* analog capture: top field first */

    av_opt_set_int(p->venc->priv_data, "level", 3, 0);
    av_opt_set_int(p->venc->priv_data, "slices", 16, 0);
    av_opt_set_int(p->venc->priv_data, "slicecrc", 1, 0);
    int threads = cpu_count() - 1;
    if (threads < 2)
        threads = 2;
    p->venc->thread_count = threads;
    p->venc->thread_type = FF_THREAD_SLICE;
    if (p->fmt->oformat->flags & AVFMT_GLOBALHEADER)
        p->venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    rc = avcodec_open2(p->venc, venc_codec, NULL);
    if (rc < 0) {
        pin_logf(PIN_LOG_ERROR, "sink_ffv1: avcodec_open2(ffv1) failed (%d)\n", rc);
        return PIN_ERR_CODEC;
    }
    avcodec_parameters_from_context(p->vst->codecpar, p->venc);
    p->vst->time_base = p->venc->time_base;
    p->vst->avg_frame_rate = p->venc->framerate;
    p->vst->sample_aspect_ratio = p->venc->sample_aspect_ratio;

    const AVCodec *aenc_codec = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);
    if (!aenc_codec)
        return PIN_ERR_CODEC;
    p->ast = avformat_new_stream(p->fmt, NULL);
    if (!p->ast)
        return PIN_ERR_NOMEM;
    p->ast->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    p->ast->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
    p->ast->codecpar->sample_rate = params->audio_rate;
    p->ast->codecpar->format = AV_SAMPLE_FMT_S16;
    av_channel_layout_default(&p->ast->codecpar->ch_layout, params->audio_channels);
    p->ast->codecpar->bits_per_coded_sample = 16;
    p->ast->codecpar->block_align = (int)(params->audio_channels * 2);
    p->ast->time_base = (AVRational){ 1, params->audio_rate };

    if (params->title[0])
        av_dict_set(&p->fmt->metadata, "title", params->title, 0);

    rc = avio_open(&p->fmt->pb, path, AVIO_FLAG_WRITE);
    if (rc < 0) {
        pin_logf(PIN_LOG_ERROR, "sink_ffv1: avio_open(%s) failed (%d)\n", path, rc);
        return PIN_ERR_IO;
    }
    rc = avformat_write_header(p->fmt, NULL);
    if (rc < 0) {
        pin_logf(PIN_LOG_ERROR, "sink_ffv1: avformat_write_header failed (%d)\n", rc);
        return PIN_ERR_CODEC;
    }

    pthread_mutex_init(&p->mux_lock, NULL);
    pthread_mutex_init(&p->qlock, NULL);
    pthread_mutex_init(&p->st_lock, NULL);
    pthread_cond_init(&p->qwake, NULL);
    pthread_cond_init(&p->qspace, NULL);
    if (pthread_create(&p->enc_thread, NULL, encoder_thread_fn, p) != 0)
        return PIN_ERR_INTERNAL;
    p->enc_started = 1;
    return PIN_OK;
}

static pin_status_t ffv1_write_video(pin_sink_t *s, const uint8_t *yuyv, size_t len)
{
    ffv1_priv_t *p = s->priv;
    if (!p->enc_started)
        return PIN_ERR_STATE;
    size_t expect = (size_t)p->width * p->height * 2;
    if (len < expect) {
        pin_logf(PIN_LOG_WARN, "sink_ffv1: short video frame (%zu of %zu bytes), skipping\n",
                len, expect);
        pthread_mutex_lock(&p->st_lock);
        p->st.units_damaged++;
        pthread_mutex_unlock(&p->st_lock);
        return PIN_OK;
    }

    AVFrame *f = av_frame_alloc();
    if (!f)
        return PIN_ERR_NOMEM;
    f->format = AV_PIX_FMT_YUV422P;
    f->width = p->width;
    f->height = p->height;
    if (av_frame_get_buffer(f, 0) < 0) {
        av_frame_free(&f);
        return PIN_ERR_NOMEM;
    }

    /* YUYV (Y0 U0 Y1 V0 per 2 luma samples) -> planar YUV422P. A cheap
     * deinterleave, no resampling/scaling -- both are 4:2:2 already. */
    for (int y = 0; y < p->height; y++) {
        const uint8_t *src = yuyv + (size_t)y * p->width * 2;
        uint8_t *py = f->data[0] + (size_t)y * f->linesize[0];
        uint8_t *pu = f->data[1] + (size_t)y * f->linesize[1];
        uint8_t *pv = f->data[2] + (size_t)y * f->linesize[2];
        for (int x = 0; x < p->width / 2; x++) {
            py[2 * x] = src[4 * x + 0];
            pu[x] = src[4 * x + 1];
            py[2 * x + 1] = src[4 * x + 2];
            pv[x] = src[4 * x + 3];
        }
    }

    /* p->vpts (not a function-local static): write_video is only called
     * from the single pin_writer consumer thread, but a `static` counter
     * here would persist for the process's whole life and leak into the
     * next capture's sink instance, offsetting its timestamps by whatever
     * the previous capture (in this same running GUI process) left behind. */
    f->pts = p->vpts++;

    pthread_mutex_lock(&p->qlock);
    if (p->qcount == QUEUE_DEPTH) {
        pthread_mutex_unlock(&p->qlock);
        av_frame_free(&f);
        pin_logf(PIN_LOG_WARN, "sink_ffv1: encoder falling behind, dropped a frame\n");
        pthread_mutex_lock(&p->st_lock);
        p->st.encoder_behind = 1;
        p->st.units_damaged++;
        pthread_mutex_unlock(&p->st_lock);
        return PIN_OK;
    }
    p->queue[p->qtail] = f;
    p->qtail = (p->qtail + 1) % QUEUE_DEPTH;
    p->qcount++;
    pthread_cond_signal(&p->qwake);
    pthread_mutex_unlock(&p->qlock);
    return PIN_OK;
}

static pin_status_t ffv1_write_audio(pin_sink_t *s, const int16_t *pcm, size_t frames)
{
    ffv1_priv_t *p = s->priv;
    if (!p->ast)
        return PIN_ERR_STATE;

    size_t len = frames * (size_t)p->audio_channels * sizeof(int16_t);
    AVPacket *pkt = av_packet_alloc();
    if (av_new_packet(pkt, (int)len) < 0) {
        av_packet_free(&pkt);
        return PIN_ERR_NOMEM;
    }
    memcpy(pkt->data, pcm, len);
    pkt->stream_index = p->ast->index;

    /* Per-instance, see f->pts's comment in ffv1_write_video above -- a
     * `static` counter here is exactly what produced absurd Matroska
     * Duration values (audio track's timestamps carrying on from a
     * previous, unrelated capture in the same process instead of starting
     * at 0), even though every actual sample written was correct. */
    int64_t start = p->apts;
    p->apts += (int64_t)frames;

    /* pts/duration are computed above in plain sample counts (the stream's
     * *nominal* time_base, 1/audio_rate, set in ffv1_open()) and must be
     * rescaled into p->ast->time_base as it stands *now* -- avformat_write_
     * header() is free to renormalise a stream's declared time_base once
     * the header is written (matroska does this for PCM audio), and this
     * mirrors encode_one()'s identical av_packet_rescale_ts() call for
     * video just below. Skipping this for audio is exactly what silently
     * turned every packet's sample-count pts into a millisecond count,
     * inflating the muxed Duration by a factor of audio_rate/1000 (48x at
     * 48 kHz) while every actual sample on disk stayed correct. */
    pkt->pts = start;
    pkt->dts = start;
    pkt->duration = (int64_t)frames;
    av_packet_rescale_ts(pkt, (AVRational){ 1, p->ast->codecpar->sample_rate }, p->ast->time_base);

    pthread_mutex_lock(&p->mux_lock);
    int rc = av_interleaved_write_frame(p->fmt, pkt);
    pthread_mutex_unlock(&p->mux_lock);
    av_packet_free(&pkt);
    if (rc < 0) {
        set_error(p, PIN_ERR_IO);
        return PIN_ERR_IO;
    }
    pthread_mutex_lock(&p->st_lock);
    p->st.bytes_written += len;
    pthread_mutex_unlock(&p->st_lock);
    return PIN_OK;
}

static pin_status_t ffv1_close(pin_sink_t *s)
{
    ffv1_priv_t *p = s->priv;
    pin_status_t rc = PIN_OK;

    if (p->enc_started) {
        pthread_mutex_lock(&p->qlock);
        p->closing = 1;
        pthread_cond_signal(&p->qwake);
        pthread_mutex_unlock(&p->qlock);
        pthread_join(p->enc_thread, NULL);
    }
    if (p->fmt) {
        if (p->fmt->pb) {
            if (av_write_trailer(p->fmt) < 0)
                rc = PIN_ERR_IO;
            avio_closep(&p->fmt->pb);
        }
        avcodec_free_context(&p->venc);
        avformat_free_context(p->fmt);
    }
    if (p->st.last_error != PIN_OK)
        rc = p->st.last_error;

    pthread_mutex_destroy(&p->mux_lock);
    pthread_mutex_destroy(&p->qlock);
    pthread_mutex_destroy(&p->st_lock);
    pthread_cond_destroy(&p->qwake);
    pthread_cond_destroy(&p->qspace);
    free(p);
    free(s);
    return rc;
}

static void ffv1_get_status(pin_sink_t *s, pin_sink_status_t *out)
{
    ffv1_priv_t *p = s->priv;
    pthread_mutex_lock(&p->st_lock);
    *out = p->st;
    pthread_mutex_unlock(&p->st_lock);
}

pin_sink_t *sink_ffv1_create(void)
{
    pin_sink_t *s = calloc(1, sizeof(*s));
    ffv1_priv_t *p = calloc(1, sizeof(*p));
    if (!s || !p) {
        free(s);
        free(p);
        return NULL;
    }
    s->priv = p;
    s->open = ffv1_open;
    s->write_video = ffv1_write_video;
    s->write_audio = ffv1_write_audio;
    s->close = ffv1_close;
    s->get_status = ffv1_get_status;
    s->supports_title = 1;
    return s;
}
