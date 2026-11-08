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
 * HDV (MPEG-2 transport stream) to MOV or MKV, written live, every GOP checked by decoding it,
 * damaged GOPs repaired (PIN_FMT_HDV_MOV, PIN_FMT_HDV_MKV). docs/hdv.md ("MOV and MKV") has the
 * whole story; hdv_demux.h the three stages of the HDV pipeline, of which this file is the
 * third, the MPEG-2 output:
 *
 * Writer thread (write_unit): stage 1, hdv_demux_t: the transport stream cut into GOPs of
 * pictures (with the damage the packets show and the camera clock) and MP2 frames. GOPs go to
 * the decode thread's queue (on_gop()), audio frames to the muxer's queue (on_audio()).
 *
 * Decode thread (dec_main()): stage 2, hdv_decoder_t: every picture decoded; a GOP comes back
 * once all its frames are back. Then this file's part: gop_decide() passes a GOP through bit for
 * bit when it begins with an I picture, its temporal_references are 0..n-1, no packet of it
 * was lost or flagged, the camera clock shows no pictures missing in front of it, and the
 * decoder gave every picture back without concealing anything (hdv_pic_ok()). Otherwise its
 * decoded pictures (concealment and all) go into a repair (span_add(), span_close()), which an
 * encode worker (run_job()) encodes again as closed MPEG-2 GOPs of the source's size, rate,
 * aspect, field order and bit rate; each picture lands on the frame its PES PTS says, a frame
 * no picture landed on repeats a neighbour. The open GOP after a repair gives its leading B
 * frames to the repair and is made closed; the first GOP of a file (and one after a clock jump)
 * loses its leading B pictures, so a file always starts on an I frame. The decode thread also
 * writes the file (drain()), in order, waiting for a repair's result before anything after it.
 *
 * Timestamps are the file's own (frames and samples from 0); the camera clock only places
 * things: audio by its PTS through the GOP it belongs to (drain_audio()), silence (made once
 * with the MP2 encoder) where audio is missing or damaged, overlapping audio dropped.
 *
 * Counts (pin_sink_status_t): frames_out, frames_error (re-encoded, or played with silenced
 * audio), frames_repeated; final_status gets them at close. Too much waiting in front of the
 * decoder, the encoders or the muxer: the sink fails (PIN_ERR_CODEC, reencode_behind) and the
 * capture ends.
 *
 * Shared with a future HDV to H.264 / HEVC sink, and to move out of here then (hdv_demux.h):
 * the timeline (span_close()'s placement by PTS and repeats, the leading-B and clock-jump rules
 * of gop_decide()), the audio placement (drain_audio()) and the error counts (mark_error()).
 * What stays here: the passthrough / repair decision, the MPEG-2 encoder and the muxing.
 */

#include "pin_sink.h"
#include "sinks_internal.h"
#include "hdv_demux.h"
#include "hdv_decode.h"
#include "../core/pin_log.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/cpu.h>
#include <libavutil/channel_layout.h>
#include <libavutil/time.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define NOPTS AV_NOPTS_VALUE
#define SPAN_MAX_SECONDS 2.0         /* a longer span is handed to a worker in parts */
#define PEND_LIMIT (160ull << 20)    /* bytes waiting in front of the muxer: the repair is behind */
#define GQ_LIMIT (64ull << 20)       /* bytes waiting for the decode thread: the check is behind */
#define MAX_HELD_SECONDS 6.0         /* decoded frames waiting for an encoder: the repair is behind */
#define MAX_WORKERS 3

/* --------------------------------------------------------- the output -- */

typedef struct {
    uint8_t *data;
    int len;
    int64_t cam;          /* camera time of the frame, 90 kHz */
} aframe_t;


/* A decoded frame for a repair, and the camera time of its picture. */
typedef struct {
    AVFrame *f;
    int64_t pts;          /* NOPTS: none */
    int64_t slot;         /* the frame it must be in the span (NOPTS: by pts, else next) */
} sframe_t;

enum { ITEM_PASS, ITEM_SPAN };

typedef struct {
    sframe_t *in;         /* while the span is collected */
    int nin, capin;
    AVFrame **slots;      /* the frames to encode, one per output frame */
    int target;           /* frames to produce */
    int repeated;         /* of them, repeats of a neighbour (pictures lost) */
    hdv_seq_t seq;        /* the stream's values (the workers' own) */
    int gop_len;
    AVPacket **out;       /* the result, in decode order */
    int nout;
    int failed;
    int done;
} job_t;

typedef struct item {
    int kind;
    int64_t out_start;    /* first frame */
    int frames;
    int64_t cam0;         /* camera time of out_start, NOPTS unknown */
    hdv_pics_t pics;          /* ITEM_PASS: the GOP */
    job_t job;            /* ITEM_SPAN */
    size_t bytes;
    struct item *next;
} item_t;

typedef struct {
    pin_format_t format;
    pin_sink_params_t params;
    char path[1200];

    /* ---- the writer thread (write_unit) */
    hdv_demux_t *dmx;          /* stage 1: transport stream -> GOPs, audio frames (hdv_demux.h) */

    /* ---- shared, under lock */
    pthread_mutex_t lock;
    pthread_cond_t dec_wake;   /* a GOP for the decode thread, a repair done, or the end */
    pthread_cond_t enc_wake;   /* a repair for the workers, or the end */
    hdv_gop_t *gq_head, *gq_tail;  /* GOPs waiting for the decode thread */
    size_t gq_bytes;
    item_t *jq[64];            /* repairs waiting for a worker */
    int jq_n;
    int frames_held;           /* decoded frames in repairs not yet encoded */
    int abort_;                /* stop now, write nothing more */
    int dec_done;              /* the decode thread hands out no more repairs */
    aframe_t *aq;              /* audio frames waiting for the muxer */
    int aq_n, aq_cap;
    int a_sr, a_ch, a_kbps;    /* the first audio frame's */
    pin_sink_status_t st;

    /* ---- the decode thread: decode, decide, mux */
    hdv_decoder_t *dec;        /* stage 2: every picture decoded (hdv_decode.h) */
    int gops_seen;
    int64_t last_cam_end;      /* camera time after the last frame given out */
    int prev_pass_ok;          /* the last item is a whole GOP on the same clock */
    item_t *span;              /* the repair being collected (not yet submitted) */
    int span_took_lead;
    int gop_len;               /* pictures in the first whole GOP: the re-encode's GOP length */
    int64_t frame_pos;         /* next output frame */
    double dur;                /* one frame in 90 kHz ticks */
    item_t *head, *tail;       /* the output queue */
    size_t pend_bytes;
    int64_t a_pos;             /* audio samples written */
    int audio_started;         /* a real audio frame is written */
    uint8_t *silence;
    int silence_len;
    int64_t anchor_cam, anchor_out;  /* camera time <-> output frame */
    int anchor_valid;
    int64_t video_end;         /* output frames written */
    uint8_t *eflag;            /* per output frame: 1 with error (re-encoded, or audio silenced) */
    size_t eflag_cap;
    AVFormatContext *fmt;
    AVStream *vst, *ast;
    int header_written, header_failed;
    int file_created;         /* avio_open() made (or truncated) the file: it is ours to remove */
    int fault_io;
    int any_written;

    /* threads */
    pthread_t dec_thr;
    int dec_started;
    pthread_t thr[MAX_WORKERS];
    int nthr;
    int enc_threads;

    uint64_t spans, pics_reencoded, pics_dropped, frames_silenced;
    uint64_t pend_limit;  /* PEND_LIMIT, or a test's */
    int test_delay_ms;    /* tests: each repair takes this much longer */
} hdvmux_t;

static void set_error(hdvmux_t *m, pin_status_t err)
{
    pthread_mutex_lock(&m->lock);
    if (m->st.last_error == PIN_OK)
        m->st.last_error = err;
    pthread_cond_broadcast(&m->dec_wake);
    pthread_mutex_unlock(&m->lock);
}

static pin_status_t get_error(hdvmux_t *m)
{
    pthread_mutex_lock(&m->lock);
    pin_status_t e = m->st.last_error;
    pthread_mutex_unlock(&m->lock);
    return e;
}

/* The repair cannot keep up with the capture: the sink fails, the capture ends. */
static void behind(hdvmux_t *m, const char *what)
{
    pthread_mutex_lock(&m->lock);
    int first = m->st.last_error == PIN_OK;
    if (first) {
        m->st.reencode_behind = 1;
        m->st.last_error = PIN_ERR_CODEC;
    }
    pthread_cond_broadcast(&m->dec_wake);
    pthread_mutex_unlock(&m->lock);
    if (first)
        pin_logf(PIN_LOG_ERROR, "sink_hdvmux: %s cannot keep up with the capture\n", what);
}

/* ------------------------------------------------------------- workers -- */

static AVFrame *black_frame(const hdv_seq_t *si)
{
    AVFrame *f = av_frame_alloc();
    if (!f)
        return NULL;
    f->format = AV_PIX_FMT_YUV420P;
    f->width = si->width;
    f->height = si->height;
    if (av_frame_get_buffer(f, 0) < 0) {
        av_frame_free(&f);
        return NULL;
    }
    for (int y = 0; y < f->height; y++)
        memset(f->data[0] + (size_t)y * f->linesize[0], 16, f->width);
    for (int y = 0; y < f->height / 2; y++) {
        memset(f->data[1] + (size_t)y * f->linesize[1], 128, f->width / 2);
        memset(f->data[2] + (size_t)y * f->linesize[2], 128, f->width / 2);
    }
    return f;
}

static AVCodecContext *open_encoder(const hdvmux_t *m, const job_t *j)
{
    const hdv_seq_t *si = &j->seq;
    const AVCodec *c = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    if (!c)
        return NULL;
    AVCodecContext *e = avcodec_alloc_context3(c);
    if (!e)
        return NULL;
    e->width = si->width;
    e->height = si->height;
    e->pix_fmt = AV_PIX_FMT_YUV420P;
    e->time_base = (AVRational){ si->fps_den, si->fps_num };
    e->framerate = (AVRational){ si->fps_num, si->fps_den };
    /* the display aspect of the sequence header: 16:9 (code 3), 4:3 (2) */
    AVRational dar = si->aspect_code == 3 ? (AVRational){ 16, 9 }
                     : si->aspect_code == 2 ? (AVRational){ 4, 3 }
                     : si->aspect_code == 4 ? (AVRational){ 221, 100 } : (AVRational){ 0, 1 };
    if (dar.num)
        av_reduce(&e->sample_aspect_ratio.num, &e->sample_aspect_ratio.den,
                  (int64_t)dar.num * si->height, (int64_t)dar.den * si->width, 1 << 16);
    else
        e->sample_aspect_ratio = (AVRational){ 1, 1 };
    int64_t br = si->bit_rate > 0 ? si->bit_rate : 25000000;
    e->bit_rate = br;
    e->rc_max_rate = br;
    e->rc_buffer_size = si->vbv_bits > 0 ? si->vbv_bits : 7340032;
    e->rc_initial_buffer_occupancy = e->rc_buffer_size * 3 / 4;
    e->gop_size = j->gop_len >= 6 && j->gop_len <= 18 ? j->gop_len : 15;
    e->max_b_frames = 2;
    e->flags |= AV_CODEC_FLAG_CLOSED_GOP;
    if (!si->progressive_seq)
        e->flags |= AV_CODEC_FLAG_INTERLACED_DCT | AV_CODEC_FLAG_INTERLACED_ME;
    if (si->profile_level) {
        e->profile = (si->profile_level >> 4) & 7;
        e->level = si->profile_level & 0x0F;
    }
    e->color_primaries = AVCOL_PRI_BT709;
    e->color_trc = AVCOL_TRC_BT709;
    e->colorspace = AVCOL_SPC_BT709;
    e->color_range = AVCOL_RANGE_MPEG;
    e->field_order = si->progressive_seq ? AV_FIELD_PROGRESSIVE : si->tff ? AV_FIELD_TT : AV_FIELD_BB;
    e->thread_count = m->enc_threads;
    e->thread_type = FF_THREAD_SLICE;
    AVDictionary *o = NULL;
    av_dict_set(&o, "sc_threshold", "1000000000", 0);   /* closed GOPs need it */
    av_dict_set_int(&o, "intra_dc_precision", si->intra_dc, 0);
    av_dict_set_int(&o, "non_linear_quant", si->q_scale_type, 0);
    av_dict_set_int(&o, "intra_vlc", si->intra_vlc, 0);
    av_dict_set_int(&o, "alternate_scan", si->alt_scan, 0);
    int rc = avcodec_open2(e, c, &o);
    av_dict_free(&o);
    if (rc < 0) {
        pin_logf(PIN_LOG_ERROR, "sink_hdvmux: opening the MPEG-2 encoder failed (%d)\n", rc);
        avcodec_free_context(&e);
    }
    return e;
}

/* Encodes one repair's frames (a worker thread); the decode thread decoded them. */
static void run_job(hdvmux_t *m, job_t *j)
{
    AVPacket *pkt = av_packet_alloc();
    AVCodecContext *e = open_encoder(m, j);
    j->out = calloc((size_t)j->target + 8, sizeof(*j->out));
    if (!pkt || !e || !j->out)
        goto fail;
    for (int k = 0; k <= j->target; k++) {
        if (k < j->target) {
            AVFrame *f = j->slots[k];
            if (f->format != AV_PIX_FMT_YUV420P || f->width != j->seq.width ||
                f->height != j->seq.height) {
                AVFrame *b = black_frame(&j->seq);   /* a different raster: never fed to it */
                if (!b)
                    goto fail;
                av_frame_free(&j->slots[k]);
                j->slots[k] = f = b;
            }
            f->pts = k;
            f->pict_type = AV_PICTURE_TYPE_NONE;
            f->flags &= ~(AV_FRAME_FLAG_KEY | AV_FRAME_FLAG_CORRUPT);
            f->decode_error_flags = 0;
            if (!j->seq.progressive_seq) {
                f->flags |= AV_FRAME_FLAG_INTERLACED;
                if (j->seq.tff)
                    f->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
                else
                    f->flags &= ~AV_FRAME_FLAG_TOP_FIELD_FIRST;
            }
            if (avcodec_send_frame(e, f) < 0)
                goto fail;
        } else if (avcodec_send_frame(e, NULL) < 0) {
            goto fail;
        }
        for (;;) {
            int rc = avcodec_receive_packet(e, pkt);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
                break;
            if (rc < 0 || j->nout >= j->target + 8)
                goto fail;
            j->out[j->nout++] = av_packet_clone(pkt);
            av_packet_unref(pkt);
        }
    }
    if (j->nout == j->target)
        goto out;
fail:
    j->failed = 1;
out:
    avcodec_free_context(&e);
    av_packet_free(&pkt);
}

static void *worker_main(void *arg)
{
    hdvmux_t *m = arg;
    pthread_mutex_lock(&m->lock);
    for (;;) {
        while (!m->abort_ && !m->dec_done && m->jq_n == 0)
            pthread_cond_wait(&m->enc_wake, &m->lock);
        if (m->abort_ || m->jq_n == 0)
            break;            /* stopped, or the decode thread is done and nothing is left */
        item_t *it = m->jq[0];
        memmove(m->jq, m->jq + 1, (size_t)(--m->jq_n) * sizeof(m->jq[0]));
        pthread_mutex_unlock(&m->lock);
        if (m->test_delay_ms > 0)
            av_usleep((unsigned)m->test_delay_ms * 1000);
        run_job(m, &it->job);
        int held = it->job.target;
        for (int k = 0; k < it->job.target; k++)
            av_frame_free(&it->job.slots[k]);
        pthread_mutex_lock(&m->lock);
        m->frames_held -= held;
        it->job.done = 1;
        pthread_cond_broadcast(&m->dec_wake);
    }
    pthread_mutex_unlock(&m->lock);
    return NULL;
}

/* -------------------------------------------------------------- muxing -- */

static uint32_t hdv_tag(const hdv_seq_t *si)
{
    int rate = (int)lrint((double)si->fps_num / si->fps_den);
    if (si->width == 1440 && si->height == 1080) {
        if (!si->progressive_seq)
            return rate == 25 ? MKTAG('h', 'd', 'v', '3') : rate == 30 ? MKTAG('h', 'd', 'v', '2') : 0;
        return rate == 24 ? MKTAG('h', 'd', 'v', '6') : rate == 25 ? MKTAG('h', 'd', 'v', '7')
               : rate == 30 ? MKTAG('h', 'd', 'v', '8') : 0;
    }
    if (si->width == 1280 && si->height == 720 && si->progressive_seq) {
        switch (rate) {
        case 24: return MKTAG('h', 'd', 'v', '4');
        case 25: return MKTAG('h', 'd', 'v', '5');
        case 30: return MKTAG('h', 'd', 'v', '1');
        case 50: return MKTAG('h', 'd', 'v', 'a');
        case 60: return MKTAG('h', 'd', 'v', '9');
        }
    }
    return 0;
}

static int make_silence(hdvmux_t *m, int sr, int ch, int kbps)
{
    const AVCodec *c = avcodec_find_encoder(AV_CODEC_ID_MP2);
    AVCodecContext *e = c ? avcodec_alloc_context3(c) : NULL;
    AVFrame *f = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    int ok = 0;
    if (!e || !f || !pkt)
        goto out;
    e->sample_rate = sr;
    e->sample_fmt = AV_SAMPLE_FMT_S16;
    e->bit_rate = (int64_t)kbps * 1000;
    av_channel_layout_default(&e->ch_layout, ch);
    if (avcodec_open2(e, c, NULL) < 0)
        goto out;
    f->nb_samples = e->frame_size;
    f->format = AV_SAMPLE_FMT_S16;
    f->sample_rate = sr;
    av_channel_layout_copy(&f->ch_layout, &e->ch_layout);
    if (av_frame_get_buffer(f, 0) < 0)
        goto out;
    av_samples_set_silence(f->data, 0, f->nb_samples, ch, AV_SAMPLE_FMT_S16);
    f->pts = 0;
    if (avcodec_send_frame(e, f) < 0)
        goto out;
    avcodec_send_frame(e, NULL);
    if (avcodec_receive_packet(e, pkt) == 0 && pkt->size > 0) {
        m->silence = malloc((size_t)pkt->size);
        if (m->silence) {
            memcpy(m->silence, pkt->data, (size_t)pkt->size);
            m->silence_len = pkt->size;
            ok = 1;
        }
    }
out:
    if (!ok)
        pin_logf(PIN_LOG_WARN, "sink_hdvmux: no MP2 silence (encoder), gaps stay gaps\n");
    avcodec_free_context(&e);
    av_frame_free(&f);
    av_packet_free(&pkt);
    return ok;
}

static int write_header(hdvmux_t *m, const hdv_seq_t *si)
{
    AVStream *v = avformat_new_stream(m->fmt, NULL);
    if (!v)
        return -1;
    m->vst = v;
    AVCodecParameters *cp = v->codecpar;
    cp->codec_type = AVMEDIA_TYPE_VIDEO;
    cp->codec_id = AV_CODEC_ID_MPEG2VIDEO;
    cp->width = si->width;
    cp->height = si->height;
    cp->format = AV_PIX_FMT_YUV420P;
    cp->bit_rate = si->bit_rate;
    if (si->profile_level) {
        cp->profile = (si->profile_level >> 4) & 7;
        cp->level = si->profile_level & 0x0F;
    }
    /* what the engine read from the stream (sequence and picture coding extensions) */
    cp->field_order = !m->params.interlaced ? AV_FIELD_PROGRESSIVE
                      : m->params.top_field_first ? AV_FIELD_TT : AV_FIELD_BB;
    cp->color_primaries = AVCOL_PRI_BT709;
    cp->color_trc = AVCOL_TRC_BT709;
    cp->color_space = AVCOL_SPC_BT709;
    cp->color_range = AVCOL_RANGE_MPEG;
    int sn, sd;
    pin_sink_sar_for(PIN_KIND_HDV, 1, m->params.aspect, si->width, si->height, &sn, &sd);
    cp->sample_aspect_ratio = (AVRational){ sn, sd };
    v->sample_aspect_ratio = (AVRational){ sn, sd };
    v->time_base = (AVRational){ si->fps_den, si->fps_num };
    v->avg_frame_rate = (AVRational){ si->fps_num, si->fps_den };
    v->r_frame_rate = v->avg_frame_rate;
    if (m->format == PIN_FMT_HDV_MOV) {
        cp->codec_tag = hdv_tag(si);
        /* else the muxer takes the XDCAM tag of the raster (xdv3 for 1440x1080i50) */
        m->fmt->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
    } else {
        cp->extradata = av_mallocz((size_t)si->hdr_len + AV_INPUT_BUFFER_PADDING_SIZE);
        if (cp->extradata) {
            memcpy(cp->extradata, si->hdr, (size_t)si->hdr_len);
            cp->extradata_size = si->hdr_len;
        }
    }
    pthread_mutex_lock(&m->lock);
    int sr = m->a_sr, ch = m->a_ch, kbps = m->a_kbps;
    pthread_mutex_unlock(&m->lock);
    if (sr > 0) {
        AVStream *a = avformat_new_stream(m->fmt, NULL);
        if (!a)
            return -1;
        m->ast = a;
        a->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        a->codecpar->codec_id = AV_CODEC_ID_MP2;
        a->codecpar->sample_rate = sr;
        a->codecpar->bit_rate = (int64_t)kbps * 1000;
        a->codecpar->frame_size = 1152;
        av_channel_layout_default(&a->codecpar->ch_layout, ch);
        a->time_base = (AVRational){ 1, sr };
        make_silence(m, sr, ch, kbps);
    }
    if (m->params.title[0])
        av_dict_set(&m->fmt->metadata, "title", m->params.title, 0);
    if (avformat_write_header(m->fmt, NULL) < 0)
        return -1;
    m->header_written = 1;
    return 0;
}

static void mux(hdvmux_t *m, AVPacket *pkt, AVStream *s, AVRational from)
{
    pkt->stream_index = s->index;
    av_packet_rescale_ts(pkt, from, s->time_base);
    if (av_interleaved_write_frame(m->fmt, pkt) < 0)
        set_error(m, PIN_ERR_IO);
    if (m->fmt->pb) {
        uint64_t b = (uint64_t)avio_tell(m->fmt->pb);
        pthread_mutex_lock(&m->lock);
        m->st.bytes_written = b;
        pthread_mutex_unlock(&m->lock);
    }
}

/* Output frames [a, b) are "with error" (re-encoded, or their audio silenced). */
static void mark_error(hdvmux_t *m, int64_t a, int64_t b)
{
    if (a < 0)
        a = 0;
    if (b <= a)
        return;
    if ((size_t)b > m->eflag_cap) {
        size_t cap = m->eflag_cap ? m->eflag_cap : 4096;
        while (cap < (size_t)b)
            cap *= 2;
        uint8_t *e = realloc(m->eflag, cap);
        if (!e)
            return;
        memset(e + m->eflag_cap, 0, cap - m->eflag_cap);
        m->eflag = e;
        m->eflag_cap = cap;
    }
    uint64_t n = 0;
    for (int64_t i = a; i < b; i++)
        if (!m->eflag[i]) {
            m->eflag[i] = 1;
            n++;
        }
    pthread_mutex_lock(&m->lock);
    m->st.frames_error += n;
    pthread_mutex_unlock(&m->lock);
}

static void put_audio_frame(hdvmux_t *m, const uint8_t *d, int len, int sr)
{
    AVPacket *pkt = av_packet_alloc();
    if (!pkt || av_new_packet(pkt, len) < 0) {
        av_packet_free(&pkt);
        return;
    }
    memcpy(pkt->data, d, (size_t)len);
    pkt->pts = pkt->dts = m->a_pos;
    pkt->duration = 1152;
    pkt->flags = AV_PKT_FLAG_KEY;
    mux(m, pkt, m->ast, (AVRational){ 1, sr });
    av_packet_free(&pkt);
    m->a_pos += 1152;
}

/* n frames of silence where audio is missing. The video frames they play with are "with error",
 * but at the very start and end of a file only beyond 2 audio frames (where the camera's audio
 * just begins or ends a little after or before its video). */
static void put_silence(hdvmux_t *m, int n, int sr, int edge, const hdv_seq_t *si)
{
    if (n <= 0 || !m->silence)
        return;
    int64_t a0 = m->a_pos;
    for (int i = 0; i < n; i++)
        put_audio_frame(m, m->silence, m->silence_len, sr);
    pthread_mutex_lock(&m->lock);
    m->st.audio_padded_samples += (uint64_t)n * 1152;
    pthread_mutex_unlock(&m->lock);
    if (edge && n <= 2)
        return;
    double spf = (double)sr * si->fps_den / si->fps_num;    /* samples per video frame */
    int64_t f0 = (int64_t)floor(a0 / spf), f1 = (int64_t)ceil(m->a_pos / spf);
    if (f1 > m->video_end)
        f1 = m->video_end;
    int64_t before = 0;
    pthread_mutex_lock(&m->lock);
    before = (int64_t)m->st.frames_error;
    pthread_mutex_unlock(&m->lock);
    mark_error(m, f0, f1);
    pthread_mutex_lock(&m->lock);
    m->frames_silenced += m->st.frames_error - (uint64_t)before;
    pthread_mutex_unlock(&m->lock);
}

/* Writes the queued audio frames up to camera time cam_end (all of them if final), each where it
 * falls on the video, silence where audio is missing. */
static void drain_audio(hdvmux_t *m, int64_t cam_end, int final, const hdv_seq_t *si)
{
    pthread_mutex_lock(&m->lock);
    int sr = m->a_sr;
    pthread_mutex_unlock(&m->lock);
    double fr_s = (double)si->fps_den / si->fps_num;
    int64_t video_end_samples = (int64_t)llround(m->video_end * fr_s * sr);
    for (;;) {
        pthread_mutex_lock(&m->lock);
        if (m->aq_n == 0 || (!final && cam_end != NOPTS && m->aq[0].cam >= cam_end)) {
            pthread_mutex_unlock(&m->lock);
            break;
        }
        aframe_t a = m->aq[0];
        memmove(m->aq, m->aq + 1, (size_t)(--m->aq_n) * sizeof(*m->aq));
        pthread_mutex_unlock(&m->lock);
        if (!m->ast || !sr) {
            free(a.data);
            continue;
        }
        int64_t t;
        if (m->anchor_valid)
            t = (int64_t)llround((m->anchor_out * fr_s + (a.cam - m->anchor_cam) / 90000.0) * sr);
        else
            t = m->a_pos;
        if (final && t >= video_end_samples) {
            free(a.data);
            continue;                /* past the end of the video */
        }
        int64_t d = t - m->a_pos;
        if (m->audio_started && llabs(d) > 2LL * sr)
            d = 0;                   /* a clock that makes no sense here: just go on */
        if (d < -576) {
            free(a.data);            /* before the video's start, or overlaps what is written */
            continue;
        }
        if (d >= 576)
            put_silence(m, (int)((d + 576) / 1152), sr, !m->audio_started, si);
        put_audio_frame(m, a.data, a.len, sr);
        m->audio_started = 1;
        free(a.data);
    }
    /* the end: missing audio up to the end of the video is silence too (2 s at most) */
    if (final && m->ast && m->silence && m->audio_started && video_end_samples > m->a_pos &&
        video_end_samples - m->a_pos < 2LL * sr) {
        int n = (int)((video_end_samples - m->a_pos + 575) / 1152);
        if (m->a_pos + (int64_t)(n - 1) * 1152 + 576 >= video_end_samples)
            n--;                     /* not past it by more than half a frame */
        put_silence(m, n, sr, 1, si);
    }
}

static void free_item(item_t *it)
{
    hdv_pics_free(&it->pics);
    for (int i = 0; i < it->job.nin; i++)
        av_frame_free(&it->job.in[i].f);
    free(it->job.in);
    if (it->job.slots)
        for (int k = 0; k < it->job.target; k++)
            av_frame_free(&it->job.slots[k]);
    free(it->job.slots);
    for (int i = 0; i < it->job.nout; i++)
        av_packet_free(&it->job.out[i]);
    free(it->job.out);
    free(it);
}

/* Writes the items at the head of the queue that are ready (final: waits for the repairs). */
static void drain(hdvmux_t *m, int final, const hdv_seq_t *si)
{
    while (m->head && get_error(m) == PIN_OK) {
        item_t *it = m->head;
        if (it->kind == ITEM_SPAN) {
            pthread_mutex_lock(&m->lock);
            if (final)
                while (!it->job.done && !m->abort_)
                    pthread_cond_wait(&m->dec_wake, &m->lock);
            int done = it->job.done;
            pthread_mutex_unlock(&m->lock);
            if (!done)
                return;
        }
        if (!m->header_written) {
            if (m->header_failed)
                return;
            /* wait for the audio's parameters a little, as a demuxer probes */
            double fr = (double)si->fps_num / si->fps_den;
            int64_t queued = m->frame_pos - it->out_start;
            pthread_mutex_lock(&m->lock);
            int have_audio = m->a_sr > 0;
            pthread_mutex_unlock(&m->lock);
            if (!have_audio && !final && queued < 2 * fr)
                return;
            if (write_header(m, si) < 0) {
                m->header_failed = 1;
                set_error(m, PIN_ERR_IO);
                pin_logf(PIN_LOG_ERROR, "sink_hdvmux: writing the file header failed\n");
                return;
            }
        }
        AVRational vtb = { si->fps_den, si->fps_num };
        if (it->kind == ITEM_PASS) {
            AVPacket *pkt = av_packet_alloc();
            for (int i = 0; i < it->pics.n && pkt; i++) {
                hdv_pic_t *p = &it->pics.p[i];
                if (av_new_packet(pkt, (int)p->len) < 0)
                    break;
                memcpy(pkt->data, p->data, p->len);
                pkt->pts = it->out_start + p->tr;
                pkt->dts = it->out_start + i - 1;
                pkt->duration = 1;
                pkt->flags = i == 0 ? AV_PKT_FLAG_KEY : 0;
                mux(m, pkt, m->vst, vtb);
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
        } else if (!it->job.failed) {
            for (int i = 0; i < it->job.nout; i++) {
                AVPacket *q = it->job.out[i];
                q->pts += it->out_start;
                q->dts = it->out_start + i - 1;
                q->duration = 1;
                mux(m, q, m->vst, vtb);
            }
        } else {
            set_error(m, PIN_ERR_CODEC);
            pin_logf(PIN_LOG_ERROR, "sink_hdvmux: re-encoding damaged HDV pictures failed\n");
        }
        m->any_written = 1;
        m->video_end = it->out_start + it->frames;
        pthread_mutex_lock(&m->lock);
        m->st.frames_out += (uint64_t)it->frames;
        if (it->kind == ITEM_SPAN)
            m->st.frames_repeated += (uint64_t)it->job.repeated;
        pthread_mutex_unlock(&m->lock);
        if (it->kind == ITEM_SPAN)
            mark_error(m, it->out_start, it->out_start + it->frames);

        /* audio up to the end of this item, through its clock */
        if (it->cam0 != NOPTS) {
            m->anchor_cam = it->cam0;
            m->anchor_out = it->out_start;
            m->anchor_valid = 1;
        }
        int64_t cam_end = NOPTS;
        if (m->anchor_valid)
            cam_end = m->anchor_cam + (int64_t)llround((m->video_end - m->anchor_out) * m->dur);
        drain_audio(m, cam_end, 0, si);

        m->head = it->next;
        if (!m->head)
            m->tail = NULL;
        m->pend_bytes -= it->bytes;
        free_item(it);
    }
}

static void enqueue(hdvmux_t *m, item_t *it)
{
    it->next = NULL;
    if (m->tail)
        m->tail->next = it;
    else
        m->head = it;
    m->tail = it;
    m->pend_bytes += it->bytes;
    if (m->pend_bytes > m->pend_limit)
        behind(m, "writing the file behind a repair");
}

/* --------------------------------------------------------------- spans -- */

static int sframe_push(job_t *j, AVFrame *f, int64_t pts, int64_t slot)
{
    if (j->nin == j->capin) {
        int cap = j->capin ? j->capin * 2 : 64;
        sframe_t *n = realloc(j->in, (size_t)cap * sizeof(*n));
        if (!n)
            return -1;
        j->in = n;
        j->capin = cap;
    }
    j->in[j->nin++] = (sframe_t){ f, pts, slot };
    return 0;
}

/* Ends the repair being collected and hands it to a worker. next_cam: the camera time at which the
 * next whole GOP's first frame is shown (NOPTS: none, or unknown). The leading B frames of that GOP,
 * if the span takes them, are in the span already with their slot relative to `target`. */
static void span_close(hdvmux_t *m, int64_t next_cam, const hdv_seq_t *si)
{
    item_t *it = m->span;
    if (!it)
        return;
    m->span = NULL;
    job_t *j = &it->job;
    double fps = (double)si->fps_num / si->fps_den;
    int npics = (int)it->bytes;   /* (the pictures, counted in bytes while collecting) */
    it->bytes = 0;
    int collected = j->nin;       /* frames counted in frames_held */

    /* where the span starts on the camera clock: where the file has got to, or (at the start
     * of a file) its earliest picture */
    int64_t origin = it->cam0;
    if (origin == NOPTS)
        for (int i = 0; i < j->nin; i++)
            if (j->in[i].slot == NOPTS && j->in[i].pts != NOPTS && (origin == NOPTS || j->in[i].pts < origin))
                origin = j->in[i].pts;
    /* its length: up to the next good GOP if the clock says so plausibly, else its pictures,
     * or up to its latest picture */
    int target = npics;
    if (origin != NOPTS && next_cam != NOPTS) {
        int64_t t = llround((next_cam - origin) / m->dur);
        if (t >= npics / 2 && t <= npics + (int64_t)ceil(fps))
            target = (int)t;
    } else if (origin != NOPTS) {
        int64_t last = NOPTS;
        for (int i = 0; i < j->nin; i++)
            if (j->in[i].slot == NOPTS && j->in[i].pts != NOPTS && (last == NOPTS || j->in[i].pts > last))
                last = j->in[i].pts;
        if (last != NOPTS) {
            int64_t t = llround((last - origin) / m->dur) + 1;
            if (t > target && t <= npics + (int64_t)ceil(fps))
                target = (int)t;
        }
    }
    int lead = 0;
    for (int i = 0; i < j->nin; i++)
        if (j->in[i].slot != NOPTS)
            lead++;
    int total = target + lead;

    /* each frame where its picture's time puts it; a frame without one after the one before */
    j->slots = calloc((size_t)total + 1, sizeof(*j->slots));
    if (!j->slots) {
        pthread_mutex_lock(&m->lock);
        j->failed = j->done = 1;
        pthread_mutex_unlock(&m->lock);
        total = 0;
    }
    int cursor = 0;
    for (int i = 0; i < j->nin && j->slots; i++) {
        int64_t s = j->in[i].slot;
        if (s != NOPTS)
            s += target;                      /* a leading B of the next GOP */
        else if (origin != NOPTS && j->in[i].pts != NOPTS) {
            s = llround((j->in[i].pts - origin) / m->dur);
            if (s < 0 || s >= target)
                s = NOPTS;
        }
        if (s == NOPTS)
            s = cursor;
        if (s >= 0 && s < total && !j->slots[s]) {
            j->slots[s] = j->in[i].f;
            j->in[i].f = NULL;
            cursor = (int)s + 1;
        }
        av_frame_free(&j->in[i].f);
    }
    free(j->in);
    j->in = NULL;
    j->nin = j->capin = 0;
    /* frames nothing landed on (pictures lost): the one before, or at the start the first */
    AVFrame *last = NULL;
    int repeated = 0, first = -1;
    for (int k = 0; k < total; k++) {
        if (j->slots[k]) {
            last = j->slots[k];
            if (first < 0)
                first = k;
        } else if (last) {
            j->slots[k] = av_frame_clone(last);
            repeated++;
        }
    }
    for (int k = 0; k < first; k++) {
        j->slots[k] = av_frame_clone(j->slots[first]);
        repeated++;
    }
    for (int k = 0; k < total; k++) {
        if (!j->slots[k]) {
            j->slots[k] = black_frame(si);    /* nothing decoded at all */
            repeated++;
            if (!j->slots[k]) {
                pthread_mutex_lock(&m->lock);
                j->failed = 1;
                pthread_mutex_unlock(&m->lock);
                for (int q = 0; q < total; q++)
                    av_frame_free(&j->slots[q]);
                total = 0;
                break;
            }
        }
    }
    j->target = total;
    j->repeated = repeated;
    j->seq = *si;
    j->gop_len = m->gop_len;
    it->out_start = m->frame_pos;
    it->frames = total;
    it->cam0 = origin;
    m->frame_pos += total;
    m->last_cam_end = origin != NOPTS ? origin + (int64_t)llround(total * m->dur) : NOPTS;
    m->spans++;
    m->pics_reencoded += (uint64_t)npics + (uint64_t)lead;
    pin_logf(PIN_LOG_INFO, "sink_hdvmux: re-encoding %d picture(s) as %d frame(s) at frame %lld "
             "(%d repeated for lost pictures)\n", npics + lead, total, (long long)it->out_start,
             repeated);
    enqueue(m, it);
    pthread_mutex_lock(&m->lock);
    m->frames_held -= collected;  /* they are the job's slots now (or gone) */
    if (j->done) {
        /* nothing to encode */
    } else if (m->jq_n < (int)(sizeof(m->jq) / sizeof(m->jq[0]))) {
        m->jq[m->jq_n++] = it;
        m->frames_held += total;
        pthread_cond_signal(&m->enc_wake);
    } else {
        j->failed = j->done = 1;
    }
    int held = m->frames_held;
    pthread_mutex_unlock(&m->lock);
    if (held > MAX_HELD_SECONDS * (double)si->fps_num / si->fps_den)
        behind(m, "repairing damaged pictures");
}

static item_t *span_new(hdvmux_t *m)
{
    item_t *it = calloc(1, sizeof(*it));
    if (!it)
        return NULL;
    it->kind = ITEM_SPAN;
    it->cam0 = m->last_cam_end;   /* where it starts on the camera clock (NOPTS: unknown yet) */
    return it;
}

/* Moves the decoded frames of g into the open span. drop_lead: its leading B pictures have no
 * reference here (the first GOP of a file): left out. */
static void span_add(hdvmux_t *m, hdv_gop_t *g, int drop_lead)
{
    job_t *j = &m->span->job;
    hdv_pics_t *p = &g->pics;
    int lead = drop_lead && p->n && p->p[0].type == 1 ? p->p[0].tr : 0;
    int64_t end = NOPTS;
    int added = 0;
    for (int i = 0; i < p->n; i++) {
        if (i > 0 && p->p[i].type == 3 && p->p[i].tr < lead) {
            av_frame_free(&g->frames[i]);
            continue;
        }
        if (g->frames[i]) {
            if (sframe_push(j, g->frames[i], p->p[i].pts, NOPTS) == 0) {
                g->frames[i] = NULL;
                added++;
            } else
                av_frame_free(&g->frames[i]);
        }
        m->span->bytes++;             /* pictures, until span_close() */
        if (p->p[i].pts != NOPTS && (end == NOPTS || p->p[i].pts + (int64_t)llround(m->dur) > end))
            end = p->p[i].pts + (int64_t)llround(m->dur);
    }
    /* where the camera clock has got to: the latest picture's time, else the pictures counted */
    if (end != NOPTS && (m->last_cam_end == NOPTS || end > m->last_cam_end))
        m->last_cam_end = end;
    else if (m->last_cam_end != NOPTS)
        m->last_cam_end += (int64_t)llround(p->n * m->dur);
    pthread_mutex_lock(&m->lock);
    m->frames_held += added;
    pthread_mutex_unlock(&m->lock);
}

/* --------------------------------------------------------------- GOPs --- */

/* Drops the leading B pictures of a GOP (first `lead` in display order) and makes it closed. */
static void gop_drop_lead(hdv_pics_t *g, int lead)
{
    int w = 0;
    for (int i = 0; i < g->n; i++) {
        hdv_pic_t *p = &g->p[i];
        if (p->type == 3 && p->tr < lead && i > 0) {
            free(p->data);
            continue;
        }
        p->tr -= lead;
        uint8_t *h = p->data + p->pic_off;
        h[4] = (uint8_t)(p->tr >> 2);
        h[5] = (uint8_t)((h[5] & 0x3F) | ((p->tr & 3) << 6));
        if (p->gop_off >= 0)
            p->data[p->gop_off + 7] = (uint8_t)((p->data[p->gop_off + 7] | 0x40) & ~0x20);
        g->p[w++] = *p;
    }
    g->n = w;
}

/* A GOP whose frames are all back from the decoder: passed through, or repaired. */
static void gop_decide(hdvmux_t *m, hdv_gop_t *g)
{
    hdv_pics_t *gp = &g->pics;
    const hdv_seq_t *si = &g->seq;
    m->dur = 90000.0 * si->fps_den / si->fps_num;
    double fps = (double)si->fps_num / si->fps_den;
    int first_of_file = m->gops_seen++ == 0;

    /* whole? an I picture first, temporal_references 0..n-1, decodable in this order, no
     * packet lost */
    int valid = si->valid && gp->p[0].type == 1;
    unsigned char seen[HDV_MAX_GOP_PICS + 1];
    memset(seen, 0, sizeof(seen));
    for (int i = 0; i < gp->n && valid; i++) {
        const hdv_pic_t *p = &gp->p[i];
        if (p->damaged || p->type < 1 || p->type > 3 || p->tr >= gp->n || seen[p->tr] ||
            p->tr < i - 1 || (i > 0 && p->type == 1))
            valid = 0;
        else
            seen[p->tr] = 1;
    }
    int lead = valid ? gp->p[0].tr : 0;
    if (valid && !m->gop_len)
        m->gop_len = gp->n;
    int closed = gp->p[0].gop_off >= 0 && gp->p[0].closed_gop;
    int64_t cam = valid && gp->p[0].pts != NOPTS ? gp->p[0].pts - (int64_t)llround(lead * m->dur) : NOPTS;

    /* the clock against where the file has got to: pictures missing in front (a dropout that
     * took whole GOPs), or a jump (another recording; normally a new file anyway) */
    int gap = 0, jump = 0;
    if (valid && cam != NOPTS && m->last_cam_end != NOPTS) {
        double d = (cam - m->last_cam_end) / m->dur;
        if (d > 0.5 && d <= fps + 0.5)
            gap = !m->span;   /* an open span takes up the gap (frames repeated) anyway */
        else if (d > fps + 0.5 || d < -0.5)
            jump = 1;
    }
    /* the leading B pictures refer to a picture this file does not have: they will be left out,
     * and the decoder (which had nothing before them, or another recording) is not asked about them */
    int no_ref = first_of_file || jump || (!m->prev_pass_ok && !m->span);

    /* what the decoder says: every picture back, none concealed */
    int dec_bad = g->dec_error;
    for (int i = 0; i < gp->n && !dec_bad; i++) {
        int skip = no_ref && i > 0 && gp->p[i].type == 3 && gp->p[0].type == 1 && gp->p[i].tr < gp->p[0].tr;
        if (!skip && !hdv_pic_ok(g, i))
            dec_bad = 1;
    }

    {
        /* the --debug log: each GOP, its pictures in decode order (type, temporal_reference,
         * ! damaged, ? no PTS, - not decoded) */
        char t[HDV_MAX_GOP_PICS * 7 + 1];
        size_t tl = 0;
        for (int i = 0; i < gp->n && tl + 8 < sizeof(t); i++)
            tl += (size_t)snprintf(t + tl, sizeof(t) - tl, " %c%d%s%s%s", " IPB????"[gp->p[i].type & 7],
                                   gp->p[i].tr, gp->p[i].damaged ? "!" : "",
                                   gp->p[i].pts == NOPTS ? "?" : "", g->frames[i] ? "" : "-");
        t[tl] = 0;
        pin_logf(PIN_LOG_DEBUG, "sink_hdvmux: GOP %d at frame %lld:%s%s%s%s%s%s\n", m->gops_seen,
                 (long long)m->frame_pos, t, valid ? "" : " (damaged)", dec_bad ? " (decoder)" : "",
                 gap ? " (pictures missing before)" : "", jump ? " (clock jump)" : "",
                 closed ? " (closed)" : "");
    }

    if (!valid || gap || dec_bad) {
        if (!m->span) {
            m->span = span_new(m);
            if (!m->span) {
                set_error(m, PIN_ERR_NOMEM);
                return;
            }
        }
        span_add(m, g, no_ref);
        m->prev_pass_ok = 0;
        /* a long run of damage goes to the workers in parts */
        if ((int64_t)m->span->bytes >= (int64_t)(SPAN_MAX_SECONDS * fps))
            span_close(m, NOPTS, si);
        return;
    }

    int drop = 0;
    if (m->span) {
        if (!jump && lead > 0 && !closed) {
            /* the leading B frames go into the repair: they refer to its last picture */
            for (int i = 1; i < gp->n; i++)
                if (gp->p[i].type == 3 && gp->p[i].tr < lead && g->frames[i]) {
                    if (sframe_push(&m->span->job, g->frames[i], gp->p[i].pts, gp->p[i].tr) == 0) {
                        g->frames[i] = NULL;
                        pthread_mutex_lock(&m->lock);
                        m->frames_held++;
                        pthread_mutex_unlock(&m->lock);
                    }
                }
            span_close(m, cam, si);
            drop = lead;
            m->span_took_lead = 1;
        } else {
            span_close(m, jump ? NOPTS : cam, si);
            if (lead > 0 && !closed)
                drop = lead;
        }
    } else if (no_ref && lead > 0 && !closed) {
        drop = lead;
    }
    if (drop) {
        gop_drop_lead(gp, drop);
        if (!m->span_took_lead)
            m->pics_dropped += (uint64_t)drop;
    }
    m->span_took_lead = 0;

    item_t *it = calloc(1, sizeof(*it));
    if (!it) {
        set_error(m, PIN_ERR_NOMEM);
        return;
    }
    it->kind = ITEM_PASS;
    it->pics = *gp;
    memset(gp, 0, sizeof(*gp));
    it->frames = it->pics.n;
    it->out_start = m->frame_pos;
    it->cam0 = cam != NOPTS ? cam + (int64_t)llround(drop * m->dur) : NOPTS;
    it->bytes = hdv_pics_bytes(&it->pics);
    m->frame_pos += it->frames;
    m->last_cam_end = it->cam0 != NOPTS ? it->cam0 + (int64_t)llround(it->frames * m->dur) : NOPTS;
    m->prev_pass_ok = 1;
    enqueue(m, it);
}

/* ------------------------------------------------------- decode thread -- */

/* Decides the GOPs the decoder has given back all frames of, and writes what is ready. */
static void decide_ready(hdvmux_t *m)
{
    hdv_gop_t *g;
    while ((g = hdv_decoder_ready(m->dec)) != NULL) {
        gop_decide(m, g);
        drain(m, 0, &g->seq);
        hdv_gop_free(g);
    }
}

/* The decode thread: decodes every GOP (the check), decides, hands repairs to the workers and
 * writes the file in order. */
static void *dec_main(void *arg)
{
    hdvmux_t *m = arg;
    hdv_seq_t last_seq;
    memset(&last_seq, 0, sizeof(last_seq));
    int ended = 0;
    item_t *blocked = NULL;
    while (!ended) {
        pthread_mutex_lock(&m->lock);
        while (!m->gq_head && !m->abort_ && m->st.last_error == PIN_OK) {
            /* a repair done (the item at the head may be ready now) also wakes it */
            int ready = m->head && m->head != blocked && m->head->kind == ITEM_SPAN && m->head->job.done;
            if (ready)
                break;
            pthread_cond_wait(&m->dec_wake, &m->lock);
        }
        hdv_gop_t *g = m->gq_head;
        if (g) {
            m->gq_head = g->next;
            if (!m->gq_head)
                m->gq_tail = NULL;
            m->gq_bytes -= g->bytes;
        }
        int stop = m->abort_ || m->st.last_error != PIN_OK;
        pthread_mutex_unlock(&m->lock);
        if (stop) {
            hdv_gop_free(g);
            break;
        }
        if (!g) {
            if (last_seq.valid)
                drain(m, 0, &last_seq);
            blocked = m->head;    /* still there: it waits for something else (the header) */
            continue;
        }
        blocked = NULL;
        if (g->eof) {
            /* the end: the decoder's last frames, the last GOP, the last repair, everything out */
            if (m->dec) {
                hdv_decoder_flush(m->dec);
                decide_ready(m);
            }
            if (last_seq.valid) {
                span_close(m, NOPTS, &last_seq);
                pthread_mutex_lock(&m->lock);
                m->dec_done = 1;
                pthread_cond_broadcast(&m->enc_wake);
                pthread_mutex_unlock(&m->lock);
                drain(m, 1, &last_seq);
                if (get_error(m) == PIN_OK)
                    drain_audio(m, NOPTS, 1, &last_seq);
            }
            hdv_gop_free(g);
            ended = 1;
            break;
        }
        last_seq = g->seq;
        if (!m->dec && !(m->dec = hdv_decoder_create(2))) {
            pin_logf(PIN_LOG_ERROR, "sink_hdvmux: the MPEG-2 decoder does not open\n");
            set_error(m, PIN_ERR_CODEC);
            hdv_gop_free(g);
            break;
        }
        hdv_decoder_feed(m->dec, g);
        decide_ready(m);
        drain(m, 0, &last_seq);
    }
    pthread_mutex_lock(&m->lock);
    m->dec_done = 1;
    pthread_cond_broadcast(&m->enc_wake);
    pthread_mutex_unlock(&m->lock);
    return NULL;
}

/* ------------------------------------- writer thread: what stage 1 gives -- */

/* A GOP (or the end marker) for the decode thread. */
static void queue_gop(hdvmux_t *m, hdv_gop_t *g)
{
    pthread_mutex_lock(&m->lock);
    if (m->gq_tail)
        m->gq_tail->next = g;
    else
        m->gq_head = g;
    m->gq_tail = g;
    m->gq_bytes += g->bytes;
    size_t waiting = m->gq_bytes;
    pthread_cond_signal(&m->dec_wake);
    pthread_mutex_unlock(&m->lock);
    if (waiting > GQ_LIMIT)
        behind(m, "checking the HDV pictures (decoding)");
}

static void on_gop(void *user, hdv_gop_t *g)
{
    g->next = NULL;
    queue_gop(user, g);
}

/* An audio frame for the muxer (the decode thread). The first one sets the stream's parameters;
 * a frame of other parameters is left out. */
static void on_audio(void *user, const uint8_t *d, int len, int64_t cam, int sr, int ch, int kbps)
{
    hdvmux_t *m = user;
    uint8_t *c = malloc((size_t)len);
    if (!c)
        return;
    memcpy(c, d, (size_t)len);
    pthread_mutex_lock(&m->lock);
    if (!m->a_sr) {
        m->a_sr = sr;
        m->a_ch = ch;
        m->a_kbps = kbps;
    }
    if (sr == m->a_sr && ch == m->a_ch && m->aq_n == m->aq_cap) {
        int cap = m->aq_cap ? m->aq_cap * 2 : 64;
        aframe_t *q = realloc(m->aq, (size_t)cap * sizeof(*q));
        if (q) {
            m->aq = q;
            m->aq_cap = cap;
        }
    }
    if (sr == m->a_sr && ch == m->a_ch && m->aq_n < m->aq_cap) {
        m->aq[m->aq_n++] = (aframe_t){ c, len, cam };
        c = NULL;
    }
    pthread_mutex_unlock(&m->lock);
    free(c);
}

static void on_error(void *user, pin_status_t err)
{
    set_error(user, err);
}

/* --------------------------------------------------------------- sink --- */

static pin_status_t hdvmux_open(pin_sink_t *s, const char *path, const pin_sink_params_t *params)
{
    hdvmux_t *m = s->priv;
    m->params = *params;
    if (strlen(path) >= sizeof(m->path))
        return PIN_ERR_ARG;
    snprintf(m->path, sizeof(m->path), "%s", path);
    /* MOV: no movflags (fragmenting), so the moov index is written only at close (about 11 GB
     * an hour of HDV). A capture that ends uncleanly (kill, power loss, pulled drive, ENOSPC at
     * the trailer) leaves an mdat that no player can open; MKV stays readable. */
    const char *muxer = m->format == PIN_FMT_HDV_MOV ? "mov" : "matroska";
    avformat_alloc_output_context2(&m->fmt, NULL, muxer, path);
    if (!m->fmt)
        return PIN_ERR_CODEC;
    int rc;
    uint64_t budget = pin_sink_fault_budget();
    if (budget != UINT64_MAX) {
        rc = pin_sink_fault_avio_open(&m->fmt->pb, path, budget);
        m->fault_io = rc >= 0;
    } else {
        rc = avio_open(&m->fmt->pb, path, AVIO_FLAG_WRITE);
    }
    if (rc < 0)
        return PIN_ERR_IO;
    m->file_created = 1;
    hdv_demux_cb_t cb = { on_gop, on_audio, on_error, m };
    m->dmx = hdv_demux_create(&cb, params->interlaced ? params->top_field_first != 0 : -1);
    if (!m->dmx)
        return PIN_ERR_NOMEM;

    /* the decode thread (it decodes every GOP with 2 slice threads), and the encoders: about
     * one per two further cores, each with slice threads of its own */
    int cpus = av_cpu_count();
    m->nthr = cpus >= 8 ? 3 : cpus >= 4 ? 2 : 1;
    m->enc_threads = cpus / m->nthr > 1 ? cpus / m->nthr : 1;
    if (pthread_create(&m->dec_thr, NULL, dec_main, m) != 0)
        return PIN_ERR_NOMEM;
    m->dec_started = 1;
    for (int i = 0; i < m->nthr; i++) {
        if (pthread_create(&m->thr[i], NULL, worker_main, m) != 0) {
            m->nthr = i;
            break;
        }
    }
    if (m->nthr == 0)
        return PIN_ERR_NOMEM;
    return PIN_OK;
}

static pin_status_t hdvmux_write_unit(pin_sink_t *s, const uint8_t *data, size_t len)
{
    hdvmux_t *m = s->priv;
    pin_status_t e = get_error(m);
    if (e != PIN_OK)
        return e;
    hdv_demux_feed(m->dmx, data, len);
    pthread_mutex_lock(&m->lock);
    m->st.units_written++;
    e = m->st.last_error;
    pthread_mutex_unlock(&m->lock);
    return e;
}

static void stop_threads(hdvmux_t *m, int abort_now)
{
    pthread_mutex_lock(&m->lock);
    if (abort_now)
        m->abort_ = 1;
    pthread_cond_broadcast(&m->dec_wake);
    pthread_cond_broadcast(&m->enc_wake);
    pthread_mutex_unlock(&m->lock);
    if (m->dec_started)
        pthread_join(m->dec_thr, NULL);
    m->dec_started = 0;
    pthread_mutex_lock(&m->lock);
    m->dec_done = 1;
    if (abort_now || m->st.last_error != PIN_OK)
        m->abort_ = 1;
    pthread_cond_broadcast(&m->enc_wake);
    pthread_mutex_unlock(&m->lock);
    for (int i = 0; i < m->nthr; i++)
        pthread_join(m->thr[i], NULL);
    m->nthr = 0;
}

static pin_status_t hdvmux_close(pin_sink_t *s)
{
    hdvmux_t *m = s->priv;
    if (m->fmt && get_error(m) == PIN_OK && m->dec_started) {
        /* the end of the stream: the last PES, picture and GOP, then the decode thread writes
         * everything out and ends */
        hdv_demux_flush(m->dmx);
        hdv_gop_t *eof = calloc(1, sizeof(*eof));
        if (eof) {
            eof->eof = 1;
            queue_gop(m, eof);
            stop_threads(m, 0);
        } else {
            set_error(m, PIN_ERR_NOMEM);
            stop_threads(m, 1);
        }
    } else {
        stop_threads(m, 1);
    }

    if (m->fmt) {
        if (m->header_written && av_write_trailer(m->fmt) < 0)
            set_error(m, PIN_ERR_IO);
        if (m->fault_io) {
            if (pin_sink_fault_avio_close(&m->fmt->pb) < 0)
                set_error(m, PIN_ERR_IO);
        } else if (m->fmt->pb && avio_closep(&m->fmt->pb) < 0) {
            set_error(m, PIN_ERR_IO);
        }
        avformat_free_context(m->fmt);
        if (!m->any_written && m->file_created) {
            pin_logf(PIN_LOG_WARN, "sink_hdvmux: no HDV pictures captured, leaving no output file\n");
            remove(m->path);
        }
    }
    if (m->spans || m->frames_silenced)
        pin_logf(PIN_LOG_INFO, "sink_hdvmux: %s: %llu repair(s), %llu picture(s) re-encoded, "
                               "%llu frame(s) repeated for lost pictures, %llu frame(s) with audio "
                               "silenced, %llu leading B picture(s) without their reference dropped\n",
                 m->path, (unsigned long long)m->spans, (unsigned long long)m->pics_reencoded,
                 (unsigned long long)m->st.frames_repeated, (unsigned long long)m->frames_silenced,
                 (unsigned long long)m->pics_dropped);
    pin_status_t rc = m->st.last_error;
    if (s->final_status)
        *s->final_status = m->st;
    while (m->head) {
        item_t *it = m->head;
        m->head = it->next;
        free_item(it);
    }
    if (m->span)
        free_item(m->span);
    while (m->gq_head) {
        hdv_gop_t *g = m->gq_head;
        m->gq_head = g->next;
        hdv_gop_free(g);
    }
    hdv_decoder_free(m->dec);
    hdv_demux_free(m->dmx);
    for (int i = 0; i < m->aq_n; i++)
        free(m->aq[i].data);
    free(m->aq);
    free(m->eflag);
    free(m->silence);
    pthread_mutex_destroy(&m->lock);
    pthread_cond_destroy(&m->dec_wake);
    pthread_cond_destroy(&m->enc_wake);
    free(m);
    free(s);
    return rc;
}

static void hdvmux_get_status(pin_sink_t *s, pin_sink_status_t *out)
{
    hdvmux_t *m = s->priv;
    pthread_mutex_lock(&m->lock);
    *out = m->st;
    pthread_mutex_unlock(&m->lock);
}

pin_sink_t *sink_hdvmux_create(pin_format_t format)
{
    if (format != PIN_FMT_HDV_MOV && format != PIN_FMT_HDV_MKV)
        return NULL;
    pin_sink_t *s = calloc(1, sizeof(*s));
    hdvmux_t *m = calloc(1, sizeof(*m));
    if (!s || !m) {
        free(s);
        free(m);
        return NULL;
    }
    m->format = format;
    m->last_cam_end = NOPTS;
    m->dur = 3600;
    m->pend_limit = PEND_LIMIT;
    pthread_mutex_init(&m->lock, NULL);
    pthread_cond_init(&m->dec_wake, NULL);
    pthread_cond_init(&m->enc_wake, NULL);
    s->priv = m;
    s->open = hdvmux_open;
    s->write_unit = hdvmux_write_unit;
    s->close = hdvmux_close;
    s->get_status = hdvmux_get_status;
    s->supports_title = 1;
    return s;
}

void sink_hdvmux_test_set(pin_sink_t *s, uint64_t pend_limit, int worker_delay_ms)
{
    hdvmux_t *m = s->priv;
    m->pend_limit = pend_limit;
    m->test_delay_ms = worker_delay_ms;
}
