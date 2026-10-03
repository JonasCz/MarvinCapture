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
 * Analog capture to stdout (`--capture -`): the NUT container with the frames
 * as they come, rawvideo YUY2 (yuyv422) and pcm_s16le, no encoder. NUT needs no
 * seeking, so it streams into a pipe (`... | ffmpeg -i - ...`). Timestamps are
 * counted from the frame and sample numbers (frame rate 25/1 or 30000/1001,
 * 1/48000 for audio); the sample aspect ratio follows the capture's aspect.
 *
 * Both write_video() and write_audio() run on the one pin_writer thread, so no
 * lock is needed. Every packet is flushed at once (AVFMT_FLAG_FLUSH_PACKETS):
 * a live pipe wants its data now, not when the muxer's buffer fills.
 */

#include "pin_sink.h"
#include "sinks_internal.h"
#include "pin_stdout.h"
#include "../core/pin_log.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>

#include <stdlib.h>
#include <string.h>

#define AVIO_BUF 65536

typedef struct {
    AVFormatContext *fmt;
    AVIOContext *io;
    AVStream *vst, *ast;
    int width, height, audio_channels;
    int64_t vpts, apts;
    int header_written;
    pin_sink_status_t st;
} nut_priv_t;

static int nut_io_write(void *opaque, const uint8_t *buf, int size)
{
    nut_priv_t *p = opaque;
    int rc = pin_stdout_write(buf, (size_t)size);
    if (rc == PIN_STDOUT_OK)
        return size;
    p->st.pipe_closed = rc == PIN_STDOUT_CLOSED;
    return AVERROR(EIO);
}

static pin_status_t nut_open(pin_sink_t *s, const char *path, const pin_sink_params_t *params)
{
    nut_priv_t *p = s->priv;
    if (params->kind != PIN_KIND_ANALOG || !pin_path_is_stdout(path))
        return PIN_ERR_ARG;
    pin_stdout_prepare();
    p->width = params->width;
    p->height = params->height;
    p->audio_channels = params->audio_channels;

    int rc = avformat_alloc_output_context2(&p->fmt, NULL, "nut", NULL);
    if (rc < 0 || !p->fmt) {
        pin_logf(PIN_LOG_ERROR, "sink_nut: no NUT muxer (%d)\n", rc);
        return PIN_ERR_CODEC;
    }

    int is_pal = params->fps_num * 1 == params->fps_den * 25;
    int sn, sd;
    pin_sink_sar_for(PIN_KIND_ANALOG, is_pal, params->aspect, params->width, &sn, &sd);

    p->vst = avformat_new_stream(p->fmt, NULL);
    p->ast = avformat_new_stream(p->fmt, NULL);
    if (!p->vst || !p->ast)
        return PIN_ERR_NOMEM;

    AVCodecParameters *vp = p->vst->codecpar;
    vp->codec_type = AVMEDIA_TYPE_VIDEO;
    vp->codec_id = AV_CODEC_ID_RAWVIDEO;
    vp->codec_tag = MKTAG('Y', 'U', 'Y', '2');
    vp->format = AV_PIX_FMT_YUYV422;
    vp->width = params->width;
    vp->height = params->height;
    vp->sample_aspect_ratio = (AVRational){ sn, sd };
    vp->field_order = AV_FIELD_TT;
    vp->color_range = AVCOL_RANGE_MPEG;
    vp->color_primaries = params->colour_matrix == PIN_MATRIX_BT709 ? AVCOL_PRI_BT709 : AVCOL_PRI_BT470BG;
    vp->color_trc = params->colour_matrix == PIN_MATRIX_BT709 ? AVCOL_TRC_BT709 : AVCOL_TRC_GAMMA28;
    vp->color_space = params->colour_matrix == PIN_MATRIX_BT709 ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
    p->vst->time_base = (AVRational){ params->fps_den, params->fps_num };
    p->vst->avg_frame_rate = (AVRational){ params->fps_num, params->fps_den };
    p->vst->r_frame_rate = p->vst->avg_frame_rate;
    p->vst->sample_aspect_ratio = vp->sample_aspect_ratio;

    AVCodecParameters *ap = p->ast->codecpar;
    ap->codec_type = AVMEDIA_TYPE_AUDIO;
    ap->codec_id = AV_CODEC_ID_PCM_S16LE;
    ap->sample_rate = params->audio_rate;
    ap->format = AV_SAMPLE_FMT_S16;
    av_channel_layout_default(&ap->ch_layout, params->audio_channels);
    ap->bits_per_coded_sample = 16;
    ap->block_align = params->audio_channels * 2;
    p->ast->time_base = (AVRational){ 1, params->audio_rate };

    if (params->title[0])
        av_dict_set(&p->fmt->metadata, "title", params->title, 0);

    uint8_t *buf = av_malloc(AVIO_BUF);
    if (!buf)
        return PIN_ERR_NOMEM;
    p->io = avio_alloc_context(buf, AVIO_BUF, 1, p, NULL, nut_io_write, NULL);
    if (!p->io) {
        av_free(buf);
        return PIN_ERR_NOMEM;
    }
    p->fmt->pb = p->io;
    p->fmt->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_FLUSH_PACKETS;

    rc = avformat_write_header(p->fmt, NULL);
    if (rc < 0) {
        pin_logf(PIN_LOG_ERROR, "sink_nut: avformat_write_header failed (%d)\n", rc);
        return p->st.pipe_closed ? PIN_ERR_IO : PIN_ERR_CODEC;
    }
    avio_flush(p->io);
    p->header_written = 1;
    return PIN_OK;
}

static pin_status_t nut_write_packet(nut_priv_t *p, AVStream *st, int64_t pts, int64_t dur,
                                     AVRational nominal_tb, const void *data, size_t len)
{
    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
        return PIN_ERR_NOMEM;
    if (av_new_packet(pkt, (int)len) < 0) {
        av_packet_free(&pkt);
        return PIN_ERR_NOMEM;
    }
    memcpy(pkt->data, data, len);
    pkt->stream_index = st->index;
    pkt->pts = pkt->dts = pts;
    pkt->duration = dur;
    pkt->flags |= AV_PKT_FLAG_KEY;
    av_packet_rescale_ts(pkt, nominal_tb, st->time_base);
    int rc = av_interleaved_write_frame(p->fmt, pkt);
    av_packet_free(&pkt);
    if (rc < 0) {
        if (!p->st.pipe_closed)
            pin_logf(PIN_LOG_ERROR, "sink_nut: write failed (%d)\n", rc);
        p->st.last_error = PIN_ERR_IO;
        return PIN_ERR_IO;
    }
    return PIN_OK;
}

static pin_status_t nut_write_video(pin_sink_t *s, const uint8_t *yuyv, size_t len)
{
    nut_priv_t *p = s->priv;
    if (!p->header_written)
        return PIN_ERR_STATE;
    size_t expect = (size_t)p->width * p->height * 2;
    if (len < expect) {
        pin_logf(PIN_LOG_WARN, "sink_nut: short video frame (%zu of %zu bytes), skipping\n", len, expect);
        p->st.units_damaged++;
        return PIN_OK;
    }
    pin_status_t rc = nut_write_packet(p, p->vst, p->vpts++, 1,
                                       (AVRational){ p->vst->avg_frame_rate.den, p->vst->avg_frame_rate.num },
                                       yuyv, expect);
    if (rc == PIN_OK) {
        p->st.units_written++;
        p->st.bytes_written += expect;
    }
    return rc;
}

static pin_status_t nut_write_audio(pin_sink_t *s, const int16_t *pcm, size_t frames)
{
    nut_priv_t *p = s->priv;
    if (!p->header_written)
        return PIN_ERR_STATE;
    size_t len = frames * (size_t)p->audio_channels * sizeof(int16_t);
    int64_t start = p->apts;
    p->apts += (int64_t)frames;
    pin_status_t rc = nut_write_packet(p, p->ast, start, (int64_t)frames,
                                       (AVRational){ 1, p->ast->codecpar->sample_rate }, pcm, len);
    if (rc == PIN_OK)
        p->st.bytes_written += len;
    return rc;
}

static pin_status_t nut_close(pin_sink_t *s)
{
    nut_priv_t *p = s->priv;
    pin_status_t rc = PIN_OK;
    if (p->fmt) {
        if (p->header_written && av_write_trailer(p->fmt) < 0 && p->st.last_error == PIN_OK)
            p->st.last_error = PIN_ERR_IO;
        if (p->io) {
            avio_flush(p->io);
            av_freep(&p->io->buffer);
            avio_context_free(&p->io);
            p->fmt->pb = NULL;
        }
        avformat_free_context(p->fmt);
    }
    if (p->st.last_error != PIN_OK)
        rc = p->st.last_error;
    free(p);
    free(s);
    return rc;
}

static void nut_get_status(pin_sink_t *s, pin_sink_status_t *out)
{
    nut_priv_t *p = s->priv;
    *out = p->st;
}

pin_sink_t *sink_nut_create(void)
{
    pin_sink_t *s = calloc(1, sizeof(*s));
    nut_priv_t *p = calloc(1, sizeof(*p));
    if (!s || !p) {
        free(s);
        free(p);
        return NULL;
    }
    s->priv = p;
    s->open = nut_open;
    s->write_video = nut_write_video;
    s->write_audio = nut_write_audio;
    s->close = nut_close;
    s->get_status = nut_get_status;
    s->supports_title = 1;
    return s;
}
