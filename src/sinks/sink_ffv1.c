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
 * threaded, slice CRC) + FLAC (compression level 8), muxed as Matroska, via
 * libavformat / libavcodec.
 *
 * The pin_writer consumer thread (see pin_writer.h) calls write_video()
 * once per captured YUYV frame. write_video() only converts YUYV -> planar
 * YUV422P (a cheap deinterleave, no scaling maths) and hands the plane off
 * to a small bounded queue; a dedicated encoder thread does the actual
 * avcodec_send_frame/receive_packet + mux write. If that queue is full,
 * write_video() waits for room (and sets st.encoder_behind): the writer
 * thread may block, that is what pin_writer's much larger ring in front of
 * it is for, and only if that fills too is anything dropped (and counted
 * there). Dropping here instead would lose a frame while that ring sat
 * nearly empty.
 *
 * Audio gets the same treatment on its own thread: write_audio() only copies
 * the interleaved s16 samples into a bounded ring (AUDIO_RING_SECONDS long)
 * and a dedicated audio thread cuts it into FLAC blocks, encodes and muxes
 * them, so FLAC never runs on the writer thread or competes with the FFV1
 * slice threads' scheduling. A full ring makes write_audio() wait, as for
 * video. mux_lock serialises the two encoder threads' writes into the same
 * AVFormatContext (libavformat's interleaving queue is not thread-safe
 * across independent writers).
 *
 * Long captures: every timestamp is an int64 count (video frames, audio
 * samples) rescaled by libavformat, and the ring indices are size_t modulo
 * the ring size, so nothing wraps in any realistic capture length. 48 kHz
 * passes 2^31 samples after 12.4 h and 2^32 after 24.9 h, which is why no
 * sample count may ever go through an int. FLAC's own limit is STREAMINFO's
 * 36-bit total-samples field, about 397 h at 48 kHz.
 *
 * At close both threads drain and flush before the trailer: the FLAC
 * encoder's last packet carries the final STREAMINFO (total samples, MD5)
 * as new extradata, which matroskaenc writes back into CodecPrivate.
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
#define FFV1_SLICES 16
#define FLAC_LEVEL 8             /* highest within the FLAC subset (LPC order <= 12) */
#define AUDIO_RING_SECONDS 4

typedef struct {
    AVFormatContext *fmt;
    AVStream *vst, *ast;
    AVCodecContext *venc, *aenc;
    pthread_mutex_t mux_lock;

    int width, height;
    int audio_channels, audio_rate;
    int64_t vpts, apts;   /* per-instance timelines -- see write_video/audio_thread_fn */

    /* bounded frame queue -> encoder thread */
    AVFrame *queue[QUEUE_DEPTH];
    int qhead, qtail, qcount;
    int closing;
    pthread_mutex_t qlock;
    pthread_cond_t qwake, qspace;
    pthread_t enc_thread;
    int enc_started;

    /* bounded interleaved-s16 ring -> audio thread. Positions and counts
     * are in sample frames (one sample per channel). */
    int16_t *aring;
    size_t aring_frames, ahead, acount;
    int aclosing;
    pthread_mutex_t alock;
    pthread_cond_t awake, aspace;
    pthread_t aud_thread;
    int aud_started;

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

/* Encodes one block of audio (NULL flushes) and muxes what comes out.
 * Called only on the audio thread. */
static void encode_audio(ffv1_priv_t *p, AVFrame *frame)
{
    int rc = avcodec_send_frame(p->aenc, frame);
    if (rc < 0) {
        pin_logf(PIN_LOG_ERROR, "sink_ffv1: avcodec_send_frame(flac) failed (%d)\n", rc);
        set_error(p, PIN_ERR_CODEC);
        return;
    }
    for (;;) {
        AVPacket *pkt = av_packet_alloc();
        rc = avcodec_receive_packet(p->aenc, pkt);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
            av_packet_free(&pkt);
            break;
        }
        if (rc < 0) {
            av_packet_free(&pkt);
            pin_logf(PIN_LOG_ERROR, "sink_ffv1: avcodec_receive_packet(flac) failed (%d)\n", rc);
            set_error(p, PIN_ERR_CODEC);
            break;
        }
        /* p->ast->time_base as it stands after avformat_write_header()
         * (matroska renormalises it to 1/1000), not the nominal 1/rate. */
        av_packet_rescale_ts(pkt, p->aenc->time_base, p->ast->time_base);
        pkt->stream_index = p->ast->index;
        size_t pkt_size = (size_t)pkt->size;
        pthread_mutex_lock(&p->mux_lock);
        rc = av_interleaved_write_frame(p->fmt, pkt);
        pthread_mutex_unlock(&p->mux_lock);
        av_packet_free(&pkt);
        if (rc < 0) {
            pin_logf(PIN_LOG_ERROR, "sink_ffv1: mux write (audio) failed (%d)\n", rc);
            set_error(p, PIN_ERR_IO);
            break;
        }
        pthread_mutex_lock(&p->st_lock);
        p->st.bytes_written += pkt_size;
        pthread_mutex_unlock(&p->st_lock);
    }
}

static void *audio_thread_fn(void *arg)
{
    ffv1_priv_t *p = arg;
    const size_t block = (size_t)p->aenc->frame_size;
    const size_t ch = (size_t)p->audio_channels;

    pthread_mutex_lock(&p->alock);
    for (;;) {
        /* Whole FLAC blocks while capturing; at close, the short remainder
         * too (the encoder takes a small last frame). */
        while (p->acount < block && !p->aclosing)
            pthread_cond_wait(&p->awake, &p->alock);
        if (p->acount == 0)
            break;   /* closing and drained */
        size_t n = p->acount < block ? p->acount : block;
        pthread_mutex_unlock(&p->alock);

        AVFrame *f = av_frame_alloc();
        int ok = f != NULL;
        if (ok) {
            f->format = AV_SAMPLE_FMT_S16;
            f->sample_rate = p->audio_rate;
            f->nb_samples = (int)n;
            ok = av_channel_layout_copy(&f->ch_layout, &p->aenc->ch_layout) >= 0 &&
                 av_frame_get_buffer(f, 0) >= 0;
        }

        /* Copy out of the ring (at most two runs: up to the end, then from
         * the start). Only this thread moves ahead, and write_audio() only
         * appends past ahead+acount, so the n frames are stable unlocked. */
        if (ok) {
            size_t first = p->aring_frames - p->ahead;
            if (first > n)
                first = n;
            memcpy(f->data[0], p->aring + p->ahead * ch, first * ch * sizeof(int16_t));
            memcpy(f->data[0] + first * ch * sizeof(int16_t), p->aring,
                   (n - first) * ch * sizeof(int16_t));
            f->pts = p->apts;
        } else {
            pin_logf(PIN_LOG_ERROR, "sink_ffv1: audio frame alloc failed, %zu samples lost\n", n);
            set_error(p, PIN_ERR_NOMEM);
        }
        p->apts += (int64_t)n;   /* keep the timeline even for a lost block */

        pthread_mutex_lock(&p->alock);
        p->ahead = (p->ahead + n) % p->aring_frames;
        p->acount -= n;
        pthread_cond_signal(&p->aspace);
        pthread_mutex_unlock(&p->alock);

        if (ok)
            encode_audio(p, f);
        av_frame_free(&f);

        pthread_mutex_lock(&p->alock);
    }
    pthread_mutex_unlock(&p->alock);
    encode_audio(p, NULL);   /* drain; the last packet carries the final STREAMINFO */
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
    p->audio_rate = params->audio_rate;
    if (p->audio_channels <= 0 || p->audio_rate <= 0)
        return PIN_ERR_ARG;

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
        pin_sink_sar_for(PIN_KIND_ANALOG, is_pal, params->aspect, params->width, params->height, &sn, &sd);
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
    av_opt_set_int(p->venc->priv_data, "slices", FFV1_SLICES, 0);
    av_opt_set_int(p->venc->priv_data, "slicecrc", 1, 0);
    /* Compression (pin_capture_opts_t.ffv1_compression). FFmpeg's defaults, Golomb-Rice
     * coding with the small context model, are the fastest mode: that is "fast", what this
     * sink always wrote before the setting existed. "smaller" is the range coder with a
     * custom state table (written into the header; it beat the default table in every test);
     * "smallest" adds the large context model. Both cost CPU time; every level is lossless and
     * any FFV1 decoder reads all of them. What they save depends on the material: on synthetic
     * SD test pictures the range coder saved 2-11 %, but the large context model, whose many
     * contexts restart in each of the 16 slices of every frame, came out 2-8 % larger than the
     * range coder alone (it only helped with 4 slices). */
    if (params->ffv1_compression == PIN_FFV1_SMALLER || params->ffv1_compression == PIN_FFV1_SMALLEST)
        av_opt_set(p->venc->priv_data, "coder", "range_tab", 0);
    if (params->ffv1_compression == PIN_FFV1_SMALLEST)
        av_opt_set_int(p->venc->priv_data, "context", 1, 0);
    /* Slice threading runs one slice per thread, so more threads than
     * slices would only sit idle. */
    int threads = cpu_count() - 1;
    if (threads > FFV1_SLICES)
        threads = FFV1_SLICES;
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

    const AVCodec *aenc_codec = avcodec_find_encoder(AV_CODEC_ID_FLAC);
    if (!aenc_codec)
        return PIN_ERR_CODEC;
    p->ast = avformat_new_stream(p->fmt, NULL);
    p->aenc = avcodec_alloc_context3(aenc_codec);
    if (!p->ast || !p->aenc)
        return PIN_ERR_NOMEM;
    p->aenc->sample_fmt = AV_SAMPLE_FMT_S16;   /* interleaved, as captured */
    p->aenc->sample_rate = params->audio_rate;
    av_channel_layout_default(&p->aenc->ch_layout, params->audio_channels);
    p->aenc->time_base = (AVRational){ 1, params->audio_rate };
    p->aenc->compression_level = FLAC_LEVEL;
    if (p->fmt->oformat->flags & AVFMT_GLOBALHEADER)
        p->aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    rc = avcodec_open2(p->aenc, aenc_codec, NULL);
    if (rc < 0) {
        pin_logf(PIN_LOG_ERROR, "sink_ffv1: avcodec_open2(flac) failed (%d)\n", rc);
        return PIN_ERR_CODEC;
    }
    avcodec_parameters_from_context(p->ast->codecpar, p->aenc);
    p->ast->time_base = p->aenc->time_base;

    p->aring_frames = (size_t)params->audio_rate * AUDIO_RING_SECONDS;
    if (p->aring_frames < (size_t)p->aenc->frame_size)
        p->aring_frames = (size_t)p->aenc->frame_size;
    p->aring = malloc(p->aring_frames * (size_t)p->audio_channels * sizeof(int16_t));
    if (!p->aring)
        return PIN_ERR_NOMEM;

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

    if (pthread_create(&p->enc_thread, NULL, encoder_thread_fn, p) != 0)
        return PIN_ERR_INTERNAL;
    p->enc_started = 1;
    if (pthread_create(&p->aud_thread, NULL, audio_thread_fn, p) != 0)
        return PIN_ERR_INTERNAL;
    p->aud_started = 1;
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
        pthread_mutex_lock(&p->st_lock);
        p->st.encoder_behind = 1;
        pthread_mutex_unlock(&p->st_lock);
        while (p->qcount == QUEUE_DEPTH)
            pthread_cond_wait(&p->qspace, &p->qlock);
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
    if (!p->aud_started)
        return PIN_ERR_STATE;
    const size_t ch = (size_t)p->audio_channels;

    /* Appends in pieces of at most the free space, so a call larger than
     * the whole ring still goes through (waiting for the audio thread). */
    pthread_mutex_lock(&p->alock);
    while (frames > 0) {
        if (p->acount == p->aring_frames) {
            pthread_mutex_lock(&p->st_lock);
            p->st.encoder_behind = 1;
            pthread_mutex_unlock(&p->st_lock);
            while (p->acount == p->aring_frames)
                pthread_cond_wait(&p->aspace, &p->alock);
        }
        size_t tail = (p->ahead + p->acount) % p->aring_frames;
        size_t n = p->aring_frames - p->acount;   /* free space */
        if (n > p->aring_frames - tail)
            n = p->aring_frames - tail;           /* up to the ring's end */
        if (n > frames)
            n = frames;
        memcpy(p->aring + tail * ch, pcm, n * ch * sizeof(int16_t));
        p->acount += n;
        pcm += n * ch;
        frames -= n;
        pthread_cond_signal(&p->awake);
    }
    pthread_mutex_unlock(&p->alock);
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
    if (p->aud_started) {
        pthread_mutex_lock(&p->alock);
        p->aclosing = 1;
        pthread_cond_signal(&p->awake);
        pthread_mutex_unlock(&p->alock);
        pthread_join(p->aud_thread, NULL);
    }
    if (p->fmt) {
        if (p->fmt->pb) {
            if (av_write_trailer(p->fmt) < 0)
                rc = PIN_ERR_IO;
            avio_closep(&p->fmt->pb);
        }
        avformat_free_context(p->fmt);
    }
    avcodec_free_context(&p->venc);
    avcodec_free_context(&p->aenc);
    free(p->aring);
    if (p->st.last_error != PIN_OK)
        rc = p->st.last_error;

    pthread_mutex_destroy(&p->mux_lock);
    pthread_mutex_destroy(&p->qlock);
    pthread_mutex_destroy(&p->st_lock);
    pthread_cond_destroy(&p->qwake);
    pthread_cond_destroy(&p->qspace);
    pthread_mutex_destroy(&p->alock);
    pthread_cond_destroy(&p->awake);
    pthread_cond_destroy(&p->aspace);
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

void sink_ffv1_test_set_start(pin_sink_t *s, int64_t video_frame, int64_t audio_sample)
{
    ffv1_priv_t *p = s->priv;
    p->vpts = video_frame;
    p->apts = audio_sample;
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
    /* Initialised here rather than in open(), so close() after a failed
     * open() never destroys an uninitialised mutex. */
    pthread_mutex_init(&p->mux_lock, NULL);
    pthread_mutex_init(&p->qlock, NULL);
    pthread_mutex_init(&p->st_lock, NULL);
    pthread_cond_init(&p->qwake, NULL);
    pthread_cond_init(&p->qspace, NULL);
    pthread_mutex_init(&p->alock, NULL);
    pthread_cond_init(&p->awake, NULL);
    pthread_cond_init(&p->aspace, NULL);
    s->priv = p;
    s->open = ffv1_open;
    s->write_video = ffv1_write_video;
    s->write_audio = ffv1_write_audio;
    s->close = ffv1_close;
    s->get_status = ffv1_get_status;
    s->supports_title = 1;
    return s;
}
