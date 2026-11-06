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
 * HDV (MPEG-2 transport stream) to MOV or MKV, written live, with repair of
 * damaged GOPs (PIN_FMT_HDV_MOV, PIN_FMT_HDV_MKV).
 *
 * The sink demuxes the transport stream itself and hands the packets to
 * libavformat's muxer, as `ffmpeg -c copy` would, but it does not trust the
 * input's timing or its damaged parts:
 *
 * Demux. The PAT and PMT give the video PID (MPEG-2) and the audio PID
 * (MPEG-1 Layer II). PES packets are put back together per PID. A
 * continuity-counter gap, a transport error bit or a lost sync byte marks
 * the PES being assembled as damaged. The video elementary stream is cut
 * into pictures at its start codes; a sequence or GOP header goes with the
 * picture after it, and two field pictures make one frame. Audio PES
 * payloads are cut into MP2 frames.
 *
 * GOPs. A GOP starts at a picture with a GOP or sequence header, or at an I
 * picture. It is passed through untouched when it is whole: it begins with
 * an I picture, its temporal_references are 0..n-1, no picture in it is
 * damaged, and the camera clock shows no pictures missing in front of it.
 * Anything else is "bad".
 *
 * Repair. Consecutive bad GOPs form a span. A worker thread decodes the span
 * with FFmpeg's MPEG-2 decoder (its error concealment fills what is lost),
 * after the reference pictures of the GOP before it, and encodes the result
 * again with FFmpeg's MPEG-2 encoder as closed GOPs with the source's size,
 * frame rate, aspect, field order and bit rate. Each decoded picture lands
 * on the frame its camera time (PES PTS) says; a frame no picture landed on
 * (lost in a dropout) repeats the one before, so the file keeps the tape's
 * length and the audio stays in sync. When the GOP after a span is open
 * (its leading B pictures refer to the span's last picture), those leading
 * B pictures are decoded and encoded into the span too, and the GOP itself
 * is made closed (the B pictures dropped, the temporal_references moved
 * down, closed_gop set), so nothing in the file refers to a re-encoded
 * picture it was not encoded against, and the repair ends at the next good
 * GOP's first anchor. The first GOP of a file, or one after a jump of the
 * camera clock, loses its leading B pictures the same way (they refer to a
 * picture that is not in the file).
 *
 * Spans go to a pool of worker threads; the muxer writes in order, waiting
 * for a span's result before anything after it. If the work in front of the
 * muxer grows past PEND_LIMIT bytes, the repair cannot keep up with the
 * capture: write_unit() fails (PIN_ERR_CODEC, reencode_behind) and the
 * capture ends with that error.
 *
 * Timestamps. The output's clocks are its own: the video counts frames from
 * 0 (a GOP's pictures at its first frame + temporal_reference, the decode
 * order one before), the audio counts samples from 0. The camera clock only
 * decides where things go: an audio frame goes where its PTS falls on the
 * video, mapped through the GOP it belongs to. Where audio is missing
 * (lost, damaged, or not there yet at the start) MP2 silence fills in, made
 * once with FFmpeg's MP2 encoder; audio that would overlap is dropped.
 * Damaged audio is never written: a PES with a gap keeps only the frames in
 * front of the gap, and the next intact PES brings the audio back. A jump of
 * the camera clock (the engine normally splits the file there,
 * pin_scene.c) is just taken up: the output goes on without a gap.
 *
 * MOV gets the HDV sample description of the stream's raster and rate
 * (hdv3 1080i50, hdv2 1080i60, hdv5 720p25, ...), as Final Cut writes it;
 * Matroska gets the sequence header as CodecPrivate.
 */

#include "pin_sink.h"
#include "sinks_internal.h"
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

#define TSP 188
#define NOPTS AV_NOPTS_VALUE
#define TS_REF_ONLY (-(1LL << 40))   /* decoder packet pts: a reference, its picture not output */
#define MAX_GOP_PICS 120             /* a "GOP" this long lost its headers: cut it anyway */
#define SPAN_MAX_SECONDS 4.0         /* a longer span is handed to a worker in parts */
#define PEND_LIMIT (160ull << 20)    /* bytes waiting in front of the muxer: the repair is behind */
#define MAX_WORKERS 4

/* ------------------------------------------------------------- buffers -- */

typedef struct {
    uint8_t *d;
    size_t n, cap;
} buf_t;

static int buf_add(buf_t *b, const uint8_t *src, size_t len)
{
    if (b->n + len > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + len)
            cap *= 2;
        uint8_t *d = realloc(b->d, cap);
        if (!d)
            return -1;
        b->d = d;
        b->cap = cap;
    }
    if (len)
        memcpy(b->d + b->n, src, len);
    b->n += len;
    return 0;
}

static void buf_free(buf_t *b)
{
    free(b->d);
    memset(b, 0, sizeof(*b));
}

/* ------------------------------------------------------------ pictures -- */

typedef struct {
    uint8_t *data;
    size_t len;
    int type;          /* 1 I, 2 P, 3 B, 0 unknown */
    int tr;            /* temporal_reference */
    int damaged;
    int pic_off;       /* offset of the (first) picture start code in data */
    int gop_off;       /* offset of a GOP header in data, -1 none */
    int seq;           /* a sequence header is in front of it */
    int closed_gop, broken_link;
    int64_t pts;       /* camera clock, 90 kHz, unwrapped; NOPTS */
} pic_t;

typedef struct {
    pic_t *p;
    int n, cap;
} pics_t;

static int pics_push(pics_t *l, const pic_t *pic)
{
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 32;
        pic_t *p = realloc(l->p, (size_t)cap * sizeof(*p));
        if (!p)
            return -1;
        l->p = p;
        l->cap = cap;
    }
    l->p[l->n++] = *pic;
    return 0;
}

static int pic_copy(pic_t *dst, const pic_t *src)
{
    *dst = *src;
    dst->data = malloc(src->len ? src->len : 1);
    if (!dst->data)
        return -1;
    memcpy(dst->data, src->data, src->len);
    return 0;
}

static void pics_free(pics_t *l)
{
    for (int i = 0; i < l->n; i++)
        free(l->p[i].data);
    free(l->p);
    memset(l, 0, sizeof(*l));
}

static size_t pics_bytes(const pics_t *l)
{
    size_t b = 0;
    for (int i = 0; i < l->n; i++)
        b += l->p[i].len;
    return b;
}

/* ------------------------------------------------------- stream values -- */

/* The sequence header (and its extension) the file is made of. */
typedef struct {
    int valid;
    int width, height;
    int aspect_code, frame_rate_code;
    int fps_num, fps_den;
    int64_t bit_rate;          /* bit/s, 0 = variable */
    int vbv_bits;
    int profile_level;         /* profile_and_level_indication, 0 unknown */
    int progressive_seq;
    int chroma;                /* 1 = 4:2:0 */
    /* from the first picture coding extension */
    int intra_dc, q_scale_type, intra_vlc, alt_scan, tff;
    uint8_t hdr[256];          /* sequence header + extensions, for Matroska */
    int hdr_len;
} seqinfo_t;

static const int k_fr[9][2] = { { 0, 1 }, { 24000, 1001 }, { 24, 1 }, { 25, 1 }, { 30000, 1001 },
                                { 30, 1 }, { 50, 1 }, { 60000, 1001 }, { 60, 1 } };

/* Parses the sequence header at d (its start code) and the extensions after it, up to end. */
static void parse_seq(const uint8_t *d, size_t end, seqinfo_t *si)
{
    if (end < 12)
        return;
    seqinfo_t s = *si;
    s.width = (d[4] << 4) | (d[5] >> 4);
    s.height = ((d[5] & 0x0F) << 8) | d[6];
    s.aspect_code = d[7] >> 4;
    s.frame_rate_code = d[7] & 0x0F;
    int64_t br = ((int64_t)d[8] << 10) | (d[9] << 2) | (d[10] >> 6);
    int vbv = ((d[10] & 0x1F) << 5) | (d[11] >> 3);
    s.progressive_seq = 0;
    s.chroma = 1;
    int fr_n = 0, fr_d = 0;
    size_t stop = end;
    for (size_t i = 12; i + 4 <= end; i++) {
        if (d[i] || d[i + 1] || d[i + 2] != 1)
            continue;
        if (d[i + 3] != 0xB5) {
            stop = i;         /* the sequence header and its extensions only */
            break;
        }
        const uint8_t *e = d + i;
        if (i + 10 <= end && (e[4] >> 4) == 1) {
            s.profile_level = ((e[4] & 0x0F) << 4) | (e[5] >> 4);
            s.progressive_seq = (e[5] >> 3) & 1;
            s.chroma = (e[5] >> 1) & 3;
            s.width |= (((e[5] & 1) << 1) | (e[6] >> 7)) << 12;
            s.height |= ((e[6] >> 5) & 3) << 12;
            br |= (int64_t)(((e[6] & 0x1F) << 7) | (e[7] >> 1)) << 18;
            vbv |= e[8] << 10;
            fr_n = (e[9] >> 5) & 3;
            fr_d = e[9] & 0x1F;
        }
    }
    if (s.frame_rate_code < 1 || s.frame_rate_code > 8 || s.width <= 0 || s.height <= 0)
        return;
    s.fps_num = k_fr[s.frame_rate_code][0] * (fr_n + 1);
    s.fps_den = k_fr[s.frame_rate_code][1] * (fr_d + 1);
    s.bit_rate = br == 0x3FFFF ? 0 : br * 400;
    s.vbv_bits = vbv * 16 * 1024;
    size_t hl = stop < sizeof(s.hdr) ? stop : sizeof(s.hdr);
    memcpy(s.hdr, d, hl);
    s.hdr_len = (int)hl;
    s.valid = 1;
    *si = s;
}

/* The picture coding extension after the picture header at d (len bytes from it). Returns
 * picture_structure (3 frame), or 3 if there is none; the other fields only if found. */
static int parse_picext(const uint8_t *d, size_t len, int *dc, int *qst, int *ivlc, int *alt,
                        int *tff)
{
    for (size_t i = 4; i + 9 <= len; i++) {
        if (d[i] || d[i + 1] || d[i + 2] != 1)
            continue;
        if (d[i + 3] != 0xB5)
            return 3;
        const uint8_t *e = d + i;
        if ((e[4] >> 4) != 8)
            continue;
        if (dc) {
            *dc = (e[6] >> 2) & 3;
            *tff = e[7] >> 7;
            *qst = (e[7] >> 4) & 1;
            *ivlc = (e[7] >> 3) & 1;
            *alt = (e[7] >> 2) & 1;
        }
        return e[6] & 3 ? e[6] & 3 : 3;
    }
    return 3;
}

/* ------------------------------------------------------------- audio --- */

typedef struct {
    uint8_t *data;
    int len;
    int64_t cam;          /* camera time of the frame, 90 kHz */
} aframe_t;

static const int k_mp2_br[15] = { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 };
static const int k_mp2_sr[3] = { 44100, 48000, 32000 };

/* An MPEG-1 Layer II frame header at d: its size, sample rate, channels, bit rate. 0 if not one. */
static int mp2_header(const uint8_t *d, int *sr, int *ch, int *kbps)
{
    if (d[0] != 0xFF || (d[1] & 0xFE) != 0xFC)    /* sync, MPEG-1, Layer II */
        return 0;
    int bri = d[2] >> 4, sri = (d[2] >> 2) & 3, pad = (d[2] >> 1) & 1;
    if (bri == 0 || bri == 15 || sri == 3)
        return 0;
    *sr = k_mp2_sr[sri];
    *kbps = k_mp2_br[bri];
    *ch = (d[3] >> 6) == 3 ? 1 : 2;
    return 144000 * *kbps / *sr + pad;
}

/* --------------------------------------------------------- the output -- */

enum { ITEM_PASS, ITEM_SPAN };

typedef struct {
    pics_t pics;          /* feed order: warm-up references, the span, the next GOP's I + leading B */
    int64_t *slot;        /* per picture: its frame in the span, TS_REF_ONLY, or NOPTS */
    int target;           /* frames to produce */
    seqinfo_t seq;        /* the stream's values, copied when submitted (the workers' own) */
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
    pics_t pics;          /* ITEM_PASS: the GOP */
    job_t job;            /* ITEM_SPAN */
    size_t bytes;
    struct item *next;
} item_t;

typedef struct {
    pin_format_t format;
    pin_sink_params_t params;
    char path[1200];

    /* demux */
    int pmt_pid, vpid, apid;
    int cc[8192 / 1];     /* last continuity counter per PID (-1 none); only vpid/apid used */
    buf_t vpes, apes;
    int vpes_on, apes_on;
    int vpes_loss;
    long apes_loss_at;    /* payload bytes before the first loss, -1 none */
    int64_t last_ts;      /* the clock unwrap */

    /* video elementary stream -> pictures */
    buf_t es;
    size_t es_scan;       /* scanned up to here */
    int au_dmg, au_has_pic, au_fields, au_pic_off;
    int64_t au_pts;
    seqinfo_t seq;        /* the first good sequence header */
    int picext_seen;

    /* pictures -> GOPs */
    pics_t gop;
    int gops_seen;
    int64_t last_cam_end; /* camera time after the last frame given out */
    int prev_pass_ok;     /* the last item is a good GOP on the same clock (span warm-up) */
    pics_t warm;          /* its reference pictures */
    item_t *span;         /* the span being collected (not yet submitted) */
    int span_gops;
    int span_took_lead;
    int gop_len;          /* pictures in the first whole GOP: the re-encode's GOP length */
    int64_t frame_pos;    /* next output frame */
    double dur;           /* one frame in 90 kHz ticks */

    /* the output queue (writer thread) */
    item_t *head, *tail;
    size_t pend_bytes;

    /* audio */
    buf_t acarry;         /* a frame cut by the end of a PES */
    int64_t a_next_cam;   /* camera time of the next frame, NOPTS */
    aframe_t *aq;
    int aq_n, aq_cap;
    int a_sr, a_ch, a_kbps;
    int64_t a_pos;        /* samples written */
    uint8_t *silence;
    int silence_len;
    int64_t anchor_cam, anchor_out;  /* camera time <-> output frame */
    int anchor_valid;
    int64_t video_end;    /* output frames written */

    /* muxer */
    AVFormatContext *fmt;
    AVStream *vst, *ast;
    int header_written, header_failed;
    int fault_io;
    int any_written;
    int64_t last_dts;

    /* workers */
    pthread_mutex_t lock;
    pthread_cond_t wake, done_cv;
    pthread_t thr[MAX_WORKERS];
    int nthr;
    item_t *jq[64];       /* submitted spans not yet taken */
    int jq_n;
    int quit;
    int enc_threads;

    uint64_t spans, pics_reencoded, pics_dropped;
    uint64_t pend_limit;  /* PEND_LIMIT, or a test's */
    int test_delay_ms;    /* tests: each repair takes this much longer */
    pin_sink_status_t st;
} hdvmux_t;

/* ------------------------------------------------------------- workers -- */

static AVFrame *black_frame(const seqinfo_t *si)
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
    if (!si->progressive_seq) {
        f->flags |= AV_FRAME_FLAG_INTERLACED;
        if (si->tff)
            f->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
    }
    return f;
}

static AVCodecContext *open_encoder(const hdvmux_t *m, const job_t *j)
{
    const seqinfo_t *si = &j->seq;
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

static void place_frame(AVFrame **slots, int target, AVFrame *f, int *cursor)
{
    int64_t s = f->pts;
    if (s == TS_REF_ONLY || (s != NOPTS && s < 0)) {
        av_frame_free(&f);
        return;
    }
    if (s == NOPTS)
        s = *cursor;
    if (s >= target || slots[s]) {
        av_frame_free(&f);
        return;
    }
    slots[s] = f;
    *cursor = (int)s + 1;
}

/* Decodes and re-encodes one span (a worker thread). */
static void run_job(hdvmux_t *m, job_t *j)
{
    AVFrame **slots = calloc((size_t)j->target + 1, sizeof(*slots));
    const AVCodec *dc = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    AVCodecContext *d = dc ? avcodec_alloc_context3(dc) : NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    AVCodecContext *e = NULL;
    int cursor = 0;
    if (!slots || !d || !pkt || !fr)
        goto fail;
    d->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    d->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
    d->thread_count = 1;
    if (avcodec_open2(d, dc, NULL) < 0)
        goto fail;
    for (int i = 0; i <= j->pics.n; i++) {
        if (i < j->pics.n) {
            const pic_t *p = &j->pics.p[i];
            if (av_new_packet(pkt, (int)p->len) < 0)
                goto fail;
            memcpy(pkt->data, p->data, p->len);
            pkt->pts = j->slot[i];
            pkt->dts = NOPTS;
            avcodec_send_packet(d, pkt);   /* a broken picture is the decoder's to conceal */
            av_packet_unref(pkt);
        } else {
            avcodec_send_packet(d, NULL);
        }
        while (avcodec_receive_frame(d, fr) == 0) {
            AVFrame *keep = av_frame_clone(fr);
            av_frame_unref(fr);
            if (keep)
                place_frame(slots, j->target, keep, &cursor);
        }
    }
    /* frames nothing landed on: the frame before (or, at the start, the first one there) */
    AVFrame *last = NULL;
    for (int k = 0; k < j->target; k++) {
        if (slots[k])
            last = slots[k];
        else if (last)
            slots[k] = av_frame_clone(last);
    }
    for (int k = j->target - 1; k >= 0; k--) {
        if (slots[k])
            last = slots[k];
        else if (last)
            slots[k] = av_frame_clone(last);
    }
    if (j->target > 0 && !slots[0]) {
        for (int k = 0; k < j->target; k++)
            if (!(slots[k] = black_frame(&j->seq)))
                goto fail;
    }

    e = open_encoder(m, j);
    if (!e)
        goto fail;
    j->out = calloc((size_t)j->target + 8, sizeof(*j->out));
    if (!j->out)
        goto fail;
    for (int k = 0; k <= j->target; k++) {
        if (k < j->target) {
            AVFrame *f = slots[k];
            f->pts = k;
            f->pict_type = AV_PICTURE_TYPE_NONE;
            f->flags &= ~AV_FRAME_FLAG_KEY;
            if (!j->seq.progressive_seq) {
                f->flags |= AV_FRAME_FLAG_INTERLACED;
                if (j->seq.tff)
                    f->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
                else
                    f->flags &= ~AV_FRAME_FLAG_TOP_FIELD_FIRST;
            }
            if (f->format != AV_PIX_FMT_YUV420P || f->width != j->seq.width ||
                f->height != j->seq.height) {
                AVFrame *b = black_frame(&j->seq);   /* a different raster: never fed to it */
                if (!b)
                    goto fail;
                b->pts = k;
                av_frame_free(&slots[k]);
                slots[k] = f = b;
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
    if (j->nout != j->target)
        goto fail;
    goto out;
fail:
    j->failed = 1;
out:
    if (slots) {
        for (int k = 0; k < j->target; k++)
            av_frame_free(&slots[k]);
        free(slots);
    }
    avcodec_free_context(&d);
    avcodec_free_context(&e);
    av_packet_free(&pkt);
    av_frame_free(&fr);
}

static void *worker_main(void *arg)
{
    hdvmux_t *m = arg;
    pthread_mutex_lock(&m->lock);
    for (;;) {
        while (!m->quit && m->jq_n == 0)
            pthread_cond_wait(&m->wake, &m->lock);
        if (m->jq_n == 0)
            break;
        item_t *it = m->jq[0];
        memmove(m->jq, m->jq + 1, (size_t)(--m->jq_n) * sizeof(m->jq[0]));
        pthread_mutex_unlock(&m->lock);
        if (m->test_delay_ms > 0)
            av_usleep((unsigned)m->test_delay_ms * 1000);
        run_job(m, &it->job);
        pthread_mutex_lock(&m->lock);
        it->job.done = 1;
        pthread_cond_broadcast(&m->done_cv);
    }
    pthread_mutex_unlock(&m->lock);
    return NULL;
}

/* -------------------------------------------------------------- muxing -- */

static uint32_t hdv_tag(const seqinfo_t *si)
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

static int make_silence(hdvmux_t *m)
{
    const AVCodec *c = avcodec_find_encoder(AV_CODEC_ID_MP2);
    AVCodecContext *e = c ? avcodec_alloc_context3(c) : NULL;
    AVFrame *f = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    int ok = 0;
    if (!e || !f || !pkt)
        goto out;
    e->sample_rate = m->a_sr;
    e->sample_fmt = AV_SAMPLE_FMT_S16;
    e->bit_rate = (int64_t)m->a_kbps * 1000;
    av_channel_layout_default(&e->ch_layout, m->a_ch);
    if (avcodec_open2(e, c, NULL) < 0)
        goto out;
    f->nb_samples = e->frame_size;
    f->format = AV_SAMPLE_FMT_S16;
    f->sample_rate = m->a_sr;
    av_channel_layout_copy(&f->ch_layout, &e->ch_layout);
    if (av_frame_get_buffer(f, 0) < 0)
        goto out;
    av_samples_set_silence(f->data, 0, f->nb_samples, m->a_ch, AV_SAMPLE_FMT_S16);
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

static void set_error(hdvmux_t *m, pin_status_t err)
{
    if (m->st.last_error == PIN_OK)
        m->st.last_error = err;
}

static int write_header(hdvmux_t *m)
{
    const seqinfo_t *si = &m->seq;
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
    if (m->a_sr > 0) {
        AVStream *a = avformat_new_stream(m->fmt, NULL);
        if (!a)
            return -1;
        m->ast = a;
        a->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        a->codecpar->codec_id = AV_CODEC_ID_MP2;
        a->codecpar->sample_rate = m->a_sr;
        a->codecpar->bit_rate = (int64_t)m->a_kbps * 1000;
        a->codecpar->frame_size = 1152;
        av_channel_layout_default(&a->codecpar->ch_layout, m->a_ch);
        a->time_base = (AVRational){ 1, m->a_sr };
        make_silence(m);
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
    if (m->fmt->pb)
        m->st.bytes_written = (uint64_t)avio_tell(m->fmt->pb);
}

static void put_audio_frame(hdvmux_t *m, const uint8_t *d, int len)
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
    mux(m, pkt, m->ast, (AVRational){ 1, m->a_sr });
    av_packet_free(&pkt);
    m->a_pos += 1152;
}

/* Writes the queued audio frames up to camera time cam_end (all of them if cam_end is NOPTS
 * and final), each where it falls on the video, silence where audio is missing. */
static void drain_audio(hdvmux_t *m, int64_t cam_end, int final)
{
    if (!m->ast) {
        for (int i = 0; i < m->aq_n; i++)
            free(m->aq[i].data);
        m->aq_n = 0;
        return;
    }
    int i = 0;
    double fr_s = (double)m->seq.fps_den / m->seq.fps_num;
    int64_t video_end_samples = (int64_t)llround(m->video_end * fr_s * m->a_sr);
    for (; i < m->aq_n; i++) {
        aframe_t *a = &m->aq[i];
        if (!final && cam_end != NOPTS && a->cam >= cam_end)
            break;
        int64_t t;
        if (m->anchor_valid)
            t = (int64_t)llround((m->anchor_out * fr_s + (a->cam - m->anchor_cam) / 90000.0) * m->a_sr);
        else
            t = m->a_pos;
        if (final && t >= video_end_samples) {
            free(a->data);
            continue;                /* past the end of the video */
        }
        int64_t d = t - m->a_pos;
        if (m->a_pos > 0 && llabs(d) > 2LL * m->a_sr)
            d = 0;                   /* a clock that makes no sense here: just go on */
        if (d < -576) {
            free(a->data);           /* before the video's start, or overlaps what is written */
            continue;
        }
        while (d >= 576 && m->silence) {
            put_audio_frame(m, m->silence, m->silence_len);
            m->st.audio_padded_samples += 1152;
            d -= 1152;
        }
        put_audio_frame(m, a->data, a->len);
        free(a->data);
    }
    memmove(m->aq, m->aq + i, (size_t)(m->aq_n - i) * sizeof(*m->aq));
    m->aq_n -= i;
    /* the end: missing audio up to the end of the video is silence too (2 s at most) */
    if (final && m->silence && m->a_pos > 0) {
        while (m->a_pos + 576 < video_end_samples && video_end_samples - m->a_pos < 2LL * m->a_sr) {
            put_audio_frame(m, m->silence, m->silence_len);
            m->st.audio_padded_samples += 1152;
        }
    }
}

/* Writes the items at the head of the queue that are ready. */
static void drain(hdvmux_t *m, int final)
{
    while (m->head) {
        item_t *it = m->head;
        if (it->kind == ITEM_SPAN) {
            pthread_mutex_lock(&m->lock);
            if (final)
                while (!it->job.done)
                    pthread_cond_wait(&m->done_cv, &m->lock);
            int done = it->job.done;
            pthread_mutex_unlock(&m->lock);
            if (!done)
                return;
        }
        if (!m->header_written) {
            if (m->header_failed)
                return;
            /* wait for the audio's parameters a little, as a demuxer probes */
            double fr = (double)m->seq.fps_num / m->seq.fps_den;
            int64_t queued = m->frame_pos - it->out_start;
            if (!m->a_sr && !final && queued < 2 * fr)
                return;
            if (write_header(m) < 0) {
                m->header_failed = 1;
                set_error(m, PIN_ERR_IO);
                pin_logf(PIN_LOG_ERROR, "sink_hdvmux: writing the file header failed\n");
                return;
            }
        }
        AVRational vtb = { m->seq.fps_den, m->seq.fps_num };
        AVPacket *pkt = av_packet_alloc();
        if (it->kind == ITEM_PASS) {
            for (int i = 0; i < it->pics.n && pkt; i++) {
                pic_t *p = &it->pics.p[i];
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
        } else if (!it->job.failed) {
            for (int i = 0; i < it->job.nout && pkt; i++) {
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
        av_packet_free(&pkt);
        m->any_written = 1;
        m->video_end = it->out_start + it->frames;

        /* audio up to the end of this item, through its clock */
        if (it->cam0 != NOPTS) {
            m->anchor_cam = it->cam0;
            m->anchor_out = it->out_start;
            m->anchor_valid = 1;
        }
        int64_t cam_end = NOPTS;
        if (m->anchor_valid)
            cam_end = m->anchor_cam + (int64_t)llround((m->video_end - m->anchor_out) * m->dur);
        drain_audio(m, cam_end, 0);

        m->head = it->next;
        if (!m->head)
            m->tail = NULL;
        m->pend_bytes -= it->bytes;
        pics_free(&it->pics);
        pics_free(&it->job.pics);
        free(it->job.slot);
        for (int i = 0; i < it->job.nout; i++)
            av_packet_free(&it->job.out[i]);
        free(it->job.out);
        free(it);
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
}

/* --------------------------------------------------------------- spans -- */

/* The I and P pictures of a good GOP: what a span after it needs to decode its first B pictures. */
static void keep_warm(hdvmux_t *m, const pics_t *g)
{
    pics_free(&m->warm);
    for (int i = 0; i < g->n; i++) {
        if (g->p[i].type == 3)
            continue;
        pic_t c;
        if (pic_copy(&c, &g->p[i]) == 0 && pics_push(&m->warm, &c) < 0)
            free(c.data);
    }
}

static int submit(hdvmux_t *m, item_t *it)
{
    pthread_mutex_lock(&m->lock);
    int ok = m->jq_n < (int)(sizeof(m->jq) / sizeof(m->jq[0]));
    if (ok) {
        m->jq[m->jq_n++] = it;
        pthread_cond_signal(&m->wake);
    }
    pthread_mutex_unlock(&m->lock);
    return ok;
}

/* Ends the span being collected. next_cam: the camera time at which the next good GOP's
 * first frame is shown (NOPTS: none, or unknown). tail: that GOP's I picture and its
 * leading B pictures, which the span takes over (NULL none). */
static void span_close(hdvmux_t *m, int64_t next_cam, const pic_t *tail, int ntail)
{
    item_t *it = m->span;
    if (!it)
        return;
    m->span = NULL;
    job_t *j = &it->job;
    int nwarm = 0;
    while (nwarm < j->pics.n && j->slot[nwarm] == TS_REF_ONLY)
        nwarm++;
    int nspan = j->pics.n - nwarm;
    double fps = (double)m->seq.fps_num / m->seq.fps_den;

    /* where the span starts on the camera clock: where the file has got to, or (at the start
     * of a file) its earliest picture */
    int64_t origin = it->cam0;
    if (origin == NOPTS) {
        for (int i = nwarm; i < j->pics.n; i++)
            if (j->pics.p[i].pts != NOPTS && (origin == NOPTS || j->pics.p[i].pts < origin))
                origin = j->pics.p[i].pts;
    }
    /* its length: up to the next good GOP, if the clock says so plausibly */
    int target = nspan;
    if (origin != NOPTS && next_cam != NOPTS) {
        int64_t t = llround((next_cam - origin) / m->dur);
        if (t >= nspan / 2 && t <= nspan + (int64_t)ceil(fps))
            target = (int)t;
    }
    else if (origin != NOPTS && next_cam == NOPTS) {
        /* no next GOP (the end, or a part of a long span): up to its latest picture */
        int64_t last = NOPTS;
        for (int i = nwarm; i < j->pics.n; i++)
            if (j->pics.p[i].pts != NOPTS && (last == NOPTS || j->pics.p[i].pts > last))
                last = j->pics.p[i].pts;
        if (last != NOPTS) {
            int64_t t = llround((last - origin) / m->dur) + 1;
            if (t > target && t <= nspan + (int64_t)ceil(fps))
                target = (int)t;
        }
    }
    int lead = ntail > 1 ? ntail - 1 : 0;
    /* each picture at its own time, if it has one that fits */
    for (int i = nwarm; i < j->pics.n; i++) {
        int64_t s = NOPTS;
        if (origin != NOPTS && j->pics.p[i].pts != NOPTS) {
            s = llround((j->pics.p[i].pts - origin) / m->dur);
            if (s < 0 || s >= target)
                s = NOPTS;
        }
        j->slot[i] = s;
    }
    /* the next GOP's leading B pictures, after its I picture (a reference only) */
    for (int k = 0; k < ntail; k++) {
        pic_t c;
        if (pic_copy(&c, &tail[k]) < 0 || pics_push(&j->pics, &c) < 0)
            continue;
        int64_t *ns = realloc(j->slot, (size_t)j->pics.n * sizeof(*ns));
        if (!ns)
            continue;
        j->slot = ns;
        j->slot[j->pics.n - 1] = k == 0 ? TS_REF_ONLY : target + tail[k].tr;
        it->bytes += c.len;
    }
    j->target = target + lead;
    j->seq = m->seq;
    j->gop_len = m->gop_len;
    it->out_start = m->frame_pos;
    it->frames = j->target;
    it->cam0 = origin;
    m->frame_pos += it->frames;
    m->last_cam_end = origin != NOPTS ? origin + (int64_t)llround(it->frames * m->dur) : NOPTS;
    m->spans++;
    m->pics_reencoded += (uint64_t)nspan + (uint64_t)lead;
    pin_logf(PIN_LOG_INFO, "sink_hdvmux: re-encoding %d damaged picture(s) as %d frame(s) at frame %lld\n",
             nspan + lead, it->frames, (long long)it->out_start);
    enqueue(m, it);
    if (!submit(m, it)) {
        pthread_mutex_lock(&m->lock);
        it->job.failed = 1;
        it->job.done = 1;
        pthread_mutex_unlock(&m->lock);
    }
}

static item_t *span_new(hdvmux_t *m)
{
    item_t *it = calloc(1, sizeof(*it));
    if (!it)
        return NULL;
    it->kind = ITEM_SPAN;
    it->cam0 = m->last_cam_end;   /* where it starts on the camera clock (NOPTS: unknown yet) */
    /* the references of the GOP before, to decode the span's first B pictures against */
    if (m->prev_pass_ok) {
        for (int i = 0; i < m->warm.n; i++) {
            pic_t c;
            if (pic_copy(&c, &m->warm.p[i]) == 0 && pics_push(&it->job.pics, &c) < 0)
                free(c.data);
        }
    }
    it->job.slot = malloc(((size_t)it->job.pics.n + 1) * sizeof(int64_t));
    if (!it->job.slot) {
        pics_free(&it->job.pics);
        free(it);
        return NULL;
    }
    for (int i = 0; i < it->job.pics.n; i++) {
        it->job.slot[i] = TS_REF_ONLY;
        it->bytes += it->job.pics.p[i].len;
    }
    return it;
}

/* Moves the pictures of g into the open span (which takes their data). */
static void span_add(hdvmux_t *m, pics_t *g)
{
    job_t *j = &m->span->job;
    for (int i = 0; i < g->n; i++) {
        if (pics_push(&j->pics, &g->p[i]) < 0) {
            free(g->p[i].data);
            continue;
        }
        int64_t *ns = realloc(j->slot, (size_t)j->pics.n * sizeof(*ns));
        if (ns) {
            j->slot = ns;
            j->slot[j->pics.n - 1] = NOPTS;
        }
        m->span->bytes += g->p[i].len;
    }
    /* where the camera clock has got to: the latest picture's time, else the pictures counted */
    int64_t end = NOPTS;
    for (int i = 0; i < g->n; i++)
        if (g->p[i].pts != NOPTS && (end == NOPTS || g->p[i].pts + (int64_t)llround(m->dur) > end))
            end = g->p[i].pts + (int64_t)llround(m->dur);
    if (end != NOPTS && (m->last_cam_end == NOPTS || end > m->last_cam_end))
        m->last_cam_end = end;
    else if (m->last_cam_end != NOPTS)
        m->last_cam_end += (int64_t)llround(g->n * m->dur);
    g->n = 0;
    m->span_gops++;
}

/* --------------------------------------------------------------- GOPs --- */

/* Drops the leading B pictures of a GOP (first `lead` in display order) and makes it closed. */
static void gop_drop_lead(pics_t *g, int lead)
{
    int w = 0;
    for (int i = 0; i < g->n; i++) {
        pic_t *p = &g->p[i];
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

static void gop_done(hdvmux_t *m)
{
    pics_t *g = &m->gop;
    if (g->n == 0)
        return;
    double fps = (double)m->seq.fps_num / m->seq.fps_den;
    int first_of_file = m->gops_seen++ == 0;

    /* whole? an I picture first, temporal_references 0..n-1, decodable in this order */
    int valid = m->seq.valid && g->p[0].type == 1;
    unsigned char seen[MAX_GOP_PICS + 1];
    memset(seen, 0, sizeof(seen));
    for (int i = 0; i < g->n && valid; i++) {
        const pic_t *p = &g->p[i];
        if (p->damaged || p->type < 1 || p->type > 3 || p->tr >= g->n || seen[p->tr] ||
            p->tr < i - 1 || (i > 0 && p->type == 1))
            valid = 0;
        else
            seen[p->tr] = 1;
    }
    int lead = valid ? g->p[0].tr : 0;
    if (valid && !m->gop_len)
        m->gop_len = g->n;
    int closed = g->p[0].gop_off >= 0 && g->p[0].closed_gop;
    int64_t cam = valid && g->p[0].pts != NOPTS ? g->p[0].pts - (int64_t)llround(lead * m->dur) : NOPTS;

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

    {
        /* the --debug log: each GOP, its pictures in decode order (type, temporal_reference,
         * ! damaged, ? no PTS) */
        char t[MAX_GOP_PICS * 6 + 1];
        size_t tl = 0;
        for (int i = 0; i < g->n && tl + 7 < sizeof(t); i++)
            tl += (size_t)snprintf(t + tl, sizeof(t) - tl, " %c%d%s%s", " IPB????"[g->p[i].type & 7],
                                   g->p[i].tr, g->p[i].damaged ? "!" : "",
                                   g->p[i].pts == NOPTS ? "?" : "");
        t[tl] = 0;
        pin_logf(PIN_LOG_DEBUG, "sink_hdvmux: GOP %d at frame %lld:%s%s%s%s%s\n", m->gops_seen,
                 (long long)m->frame_pos, t, valid ? "" : " (damaged)", gap ? " (pictures missing before)" : "",
                 jump ? " (clock jump)" : "", closed ? " (closed)" : "");
    }
    if (!valid || gap) {
        if (!m->span) {
            m->span = span_new(m);
            m->span_gops = 0;
            if (!m->span) {
                pics_free(g);
                set_error(m, PIN_ERR_NOMEM);
                return;
            }
        }
        span_add(m, g);
        m->prev_pass_ok = 0;
        /* a long run of damage goes to the workers in parts */
        if (m->span->job.pics.n >= SPAN_MAX_SECONDS * fps)
            span_close(m, NOPTS, NULL, 0);
        return;
    }

    int drop = 0;
    if (m->span) {
        if (!jump && lead > 0 && !closed) {
            span_close(m, cam, g->p, lead + 1);  /* the I picture and the leading B pictures */
            drop = lead;
            m->span_took_lead = 1;
        } else {
            span_close(m, jump ? NOPTS : cam, NULL, 0);
            if (lead > 0 && !closed)
                drop = lead;
        }
    } else if ((first_of_file || jump || !m->prev_pass_ok) && lead > 0 && !closed) {
        drop = lead;
    }
    if (drop) {
        gop_drop_lead(g, drop);
        if (!m->span_took_lead)
            m->pics_dropped += (uint64_t)drop;
    }
    m->span_took_lead = 0;

    item_t *it = calloc(1, sizeof(*it));
    if (!it) {
        pics_free(g);
        set_error(m, PIN_ERR_NOMEM);
        return;
    }
    it->kind = ITEM_PASS;
    it->pics = *g;
    memset(g, 0, sizeof(*g));
    it->frames = it->pics.n;
    it->out_start = m->frame_pos;
    it->cam0 = cam != NOPTS ? cam + (int64_t)llround(drop * m->dur) : NOPTS;
    it->bytes = pics_bytes(&it->pics);
    m->frame_pos += it->frames;
    m->last_cam_end = it->cam0 != NOPTS ? it->cam0 + (int64_t)llround(it->frames * m->dur) : NOPTS;
    keep_warm(m, &it->pics);
    m->prev_pass_ok = 1;
    enqueue(m, it);
}

/* The last GOP of a file may end in a picture the capture cut short, which no packet error
 * shows: decode it (after the references of the GOP before) and call it damaged if the decoder
 * had to conceal anything or gave fewer pictures back. */
static void check_last_gop(hdvmux_t *m)
{
    pics_t *g = &m->gop;
    if (g->n == 0 || !m->seq.valid)
        return;
    const AVCodec *dc = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    AVCodecContext *d = dc ? avcodec_alloc_context3(dc) : NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    int bad = 0, got = 0, want = 0;
    if (!d || !pkt || !fr || avcodec_open2(d, dc, NULL) < 0) {
        bad = 1;
        goto out;
    }
    int nw = m->prev_pass_ok ? m->warm.n : 0;
    for (int i = 0; i <= nw + g->n; i++) {
        if (i < nw + g->n) {
            const pic_t *p = i < nw ? &m->warm.p[i] : &g->p[i - nw];
            if (av_new_packet(pkt, (int)p->len) < 0) {
                bad = 1;
                goto out;
            }
            memcpy(pkt->data, p->data, p->len);
            /* leading B pictures without the GOP before: not decodable, not counted */
            int skip = i < nw || (!nw && p->type == 3 && g->p[0].type == 1 && p->tr < g->p[0].tr);
            pkt->pts = skip ? TS_REF_ONLY : i;
            want += !skip;
            avcodec_send_packet(d, pkt);
            av_packet_unref(pkt);
        } else {
            avcodec_send_packet(d, NULL);
        }
        while (avcodec_receive_frame(d, fr) == 0) {
            if (fr->pts != TS_REF_ONLY) {
                got++;
                if (fr->decode_error_flags || (fr->flags & AV_FRAME_FLAG_CORRUPT))
                    bad = 1;
            }
            av_frame_unref(fr);
        }
    }
    if (got < want)
        bad = 1;
out:
    if (bad)
        g->p[g->n - 1].damaged = 1;
    avcodec_free_context(&d);
    av_packet_free(&pkt);
    av_frame_free(&fr);
}

static void on_picture(hdvmux_t *m, pic_t *p)
{
    if (m->gop.n > 0 && (p->gop_off >= 0 || p->seq || p->type == 1 || m->gop.n >= MAX_GOP_PICS))
        gop_done(m);
    if (!m->seq.valid) {
        free(p->data);    /* nothing can be written before the first sequence header */
        return;
    }
    if (pics_push(&m->gop, p) < 0)
        free(p->data);
}

/* --------------------------------------------- elementary stream -> AUs -- */

static void au_emit(hdvmux_t *m, size_t len)
{
    const uint8_t *d = m->es.d;
    pic_t p;
    memset(&p, 0, sizeof(p));
    p.gop_off = -1;
    p.damaged = m->au_dmg;
    p.pts = m->au_pts;
    p.pic_off = m->au_has_pic ? m->au_pic_off : -1;
    if (p.pic_off < 0 || (size_t)p.pic_off + 6 > len) {
        p.damaged = 1;
        p.type = 0;
    } else {
        p.tr = (d[p.pic_off + 4] << 2) | (d[p.pic_off + 5] >> 6);
        p.type = (d[p.pic_off + 5] >> 3) & 7;
    }
    size_t hdr_end = p.pic_off >= 0 ? (size_t)p.pic_off : len;
    for (size_t i = 0; i + 8 <= hdr_end; i++) {
        if (d[i] || d[i + 1] || d[i + 2] != 1)
            continue;
        if (d[i + 3] == 0xB3) {
            p.seq = 1;
            if (!m->seq.valid && !p.damaged) {
                parse_seq(d + i, hdr_end - i, &m->seq);
                if (m->seq.valid)
                    m->dur = 90000.0 * m->seq.fps_den / m->seq.fps_num;
            }
        } else if (d[i + 3] == 0xB8 && p.gop_off < 0) {
            p.gop_off = (int)i;
            p.closed_gop = (d[i + 7] >> 6) & 1;
            p.broken_link = (d[i + 7] >> 5) & 1;
        }
    }
    if (m->seq.valid && !m->picext_seen && !p.damaged && p.pic_off >= 0) {
        int dc = 0, qst = 0, ivlc = 0, alt = 0, tff = 1;
        parse_picext(d + p.pic_off, len - p.pic_off, &dc, &qst, &ivlc, &alt, &tff);
        m->seq.intra_dc = dc;
        m->seq.q_scale_type = qst;
        m->seq.intra_vlc = ivlc;
        m->seq.alt_scan = alt;
        m->seq.tff = m->params.interlaced ? m->params.top_field_first : tff;
        m->picext_seen = 1;
    }
    p.data = malloc(len ? len : 1);
    if (!p.data) {
        set_error(m, PIN_ERR_NOMEM);
        return;
    }
    memcpy(p.data, d, len);
    p.len = len;
    on_picture(m, &p);
}

/* Appends a PES payload of the video PID and cuts pictures off the front. */
static void es_feed(hdvmux_t *m, const uint8_t *data, size_t len, int loss, int64_t pts)
{
    size_t old = m->es.n;         /* this payload starts here */
    if (buf_add(&m->es, data, len) < 0) {
        set_error(m, PIN_ERR_NOMEM);
        return;
    }
    int in_cur = 1;               /* the AU being assembled gets bytes of this payload */
    int ts_pending = pts != NOPTS;
    size_t from = m->es_scan;
    size_t cut = 0;               /* the current AU starts here in es */
    uint8_t *d = m->es.d;
    for (size_t i = from; i + 3 < m->es.n; i++) {
        if (d[i] || d[i + 1] || d[i + 2] != 1)
            continue;
        uint8_t code = d[i + 3];
        int boundary = 0;
        if (code == 0xB3 || code == 0xB8) {
            boundary = m->au_has_pic;
        } else if (code == 0x00) {
            if (m->au_has_pic) {
                /* the second field of a field-picture frame stays with the first */
                int st = parse_picext(d + cut + m->au_pic_off, i - cut - m->au_pic_off, NULL, NULL,
                                      NULL, NULL, NULL);
                if (st != 3 && m->au_fields == 1)
                    m->au_fields = 2;
                else
                    boundary = 1;
            }
        }
        if (boundary) {
            size_t au_len = i - cut;
            if (in_cur && i > old)
                m->au_dmg |= loss;    /* it ends inside this payload */
            in_cur = 0;
            /* emit d[cut..i): move it to the front temporarily */
            if (cut) {
                memmove(d, d + cut, m->es.n - cut);
                m->es.n -= cut;
                i -= cut;
                old = old > cut ? old - cut : 0;
                cut = 0;
            }
            au_emit(m, au_len);
            d = m->es.d;
            cut = au_len;
            m->au_has_pic = 0;
            m->au_fields = 0;
            m->au_pts = NOPTS;
            m->au_dmg = loss;   /* it ends in this payload */
        }
        if (code == 0x00 && !m->au_has_pic) {
            m->au_has_pic = 1;
            m->au_fields = 1;
            m->au_pic_off = (int)(i - cut);
            if (ts_pending) {     /* the PES's PTS is the first picture's starting in it */
                m->au_pts = pts;
                ts_pending = 0;
            }
        }
        i += 3;
    }
    if (in_cur)
        m->au_dmg |= loss;
    if (cut) {
        memmove(m->es.d, m->es.d + cut, m->es.n - cut);
        m->es.n -= cut;
    }
    m->es_scan = m->es.n > 3 ? m->es.n - 3 : 0;
}

static void es_flush(hdvmux_t *m)
{
    if (m->es.n && m->au_has_pic)
        au_emit(m, m->es.n);
    m->es.n = 0;
    m->es_scan = 0;
    m->au_has_pic = 0;
    m->au_dmg = 0;
    m->au_pts = NOPTS;
}

/* ----------------------------------------------------------------- PES -- */

static int64_t unwrap(hdvmux_t *m, int64_t v)
{
    if (m->last_ts == NOPTS) {
        m->last_ts = v;
        return v;
    }
    int64_t d = (v - m->last_ts) & ((1LL << 33) - 1);
    if (d >= (1LL << 32))
        d -= 1LL << 33;
    m->last_ts += d;
    return m->last_ts;
}

/* The PES header at d: its payload offset, and its PTS (NOPTS none). -1 if it is not one. */
static int pes_header(const uint8_t *d, size_t n, int64_t *pts)
{
    *pts = NOPTS;
    if (n < 9 || d[0] || d[1] || d[2] != 1)
        return -1;
    int hl = d[8];
    if ((size_t)9 + hl > n)
        return -1;
    if ((d[7] & 0x80) && hl >= 5) {
        const uint8_t *t = d + 9;
        *pts = ((int64_t)(t[0] & 0x0E) << 29) | (t[1] << 22) | ((t[2] & 0xFE) << 14) | (t[3] << 7) |
               (t[4] >> 1);
    }
    return 9 + hl;
}

static void video_pes_done(hdvmux_t *m)
{
    if (!m->vpes_on)
        return;
    m->vpes_on = 0;
    int64_t pts;
    int off = pes_header(m->vpes.d, m->vpes.n, &pts);
    if (off < 0) {
        /* no usable header: the picture it held is lost; what is assembled so far is damaged */
        m->au_dmg = 1;
        m->vpes.n = 0;
        return;
    }
    if (pts != NOPTS)
        pts = unwrap(m, pts);
    es_feed(m, m->vpes.d + off, m->vpes.n - (size_t)off, m->vpes_loss, pts);
    m->vpes.n = 0;
}

static void audio_frame(hdvmux_t *m, const uint8_t *d, int len, int64_t cam)
{
    if (m->aq_n == m->aq_cap) {
        int cap = m->aq_cap ? m->aq_cap * 2 : 64;
        aframe_t *q = realloc(m->aq, (size_t)cap * sizeof(*q));
        if (!q)
            return;
        m->aq = q;
        m->aq_cap = cap;
    }
    uint8_t *c = malloc((size_t)len);
    if (!c)
        return;
    memcpy(c, d, (size_t)len);
    m->aq[m->aq_n++] = (aframe_t){ c, len, cam };
}

static void audio_pes_done(hdvmux_t *m)
{
    if (!m->apes_on)
        return;
    m->apes_on = 0;
    int64_t pts;
    int off = pes_header(m->apes.d, m->apes.n, &pts);
    long limit = (long)m->apes.n;
    if (m->apes_loss_at >= 0 && m->apes_loss_at < limit)
        limit = m->apes_loss_at;      /* only the frames in front of a gap */
    if (off < 0 || off > limit) {
        m->acarry.n = 0;
        m->a_next_cam = NOPTS;
        m->apes.n = 0;
        return;
    }
    if (pts != NOPTS)
        pts = unwrap(m, pts);
    /* a frame cut by the end of the last PES goes on here (only if nothing was lost) */
    buf_t w;
    memset(&w, 0, sizeof(w));
    int64_t cam = NOPTS;
    if (m->acarry.n && m->a_next_cam != NOPTS) {
        buf_add(&w, m->acarry.d, m->acarry.n);
        cam = m->a_next_cam;
    }
    size_t carried = w.n;
    buf_add(&w, m->apes.d + off, (size_t)(limit - off));
    m->acarry.n = 0;
    m->a_next_cam = NOPTS;
    size_t pos = 0;
    int pts_used = 0;
    while (pos + 4 <= w.n) {
        int sr, ch, kbps;
        int fl = mp2_header(w.d + pos, &sr, &ch, &kbps);
        if (!fl) {
            pos++;
            cam = NOPTS;     /* not a frame where one should be: no time for what follows */
            continue;
        }
        if (pos >= carried && !pts_used) {
            pts_used = 1;
            if (pts != NOPTS)
                cam = pts;   /* the PES's PTS is its first frame's */
        }
        if (pos + (size_t)fl > w.n) {
            if (limit == (long)m->apes.n && cam != NOPTS) {
                buf_add(&m->acarry, w.d + pos, w.n - pos);
                m->a_next_cam = cam;
            }
            break;
        }
        if (!m->a_sr) {
            m->a_sr = sr;
            m->a_ch = ch;
            m->a_kbps = kbps;
        }
        if (cam != NOPTS && sr == m->a_sr && ch == m->a_ch)
            audio_frame(m, w.d + pos, fl, cam);
        if (cam != NOPTS)
            cam += 1152LL * 90000 / sr;
        pos += (size_t)fl;
    }
    buf_free(&w);
    m->apes.n = 0;
}

static void parse_psi(hdvmux_t *m, const uint8_t *pkt, int pid)
{
    if (!(pkt[1] & 0x40))
        return;
    int off = 4;
    if (pkt[3] & 0x20)
        off += 1 + pkt[4];
    if (off >= TSP)
        return;
    off += 1 + pkt[off];          /* pointer field */
    if (off + 8 > TSP)
        return;
    const uint8_t *s = pkt + off;
    int slen = ((s[1] & 0x0F) << 8) | s[2];
    int end = off + 3 + slen - 4; /* without the CRC */
    if (end > TSP)
        end = TSP;
    if (pid == 0 && s[0] == 0x00) {
        for (int i = off + 8; i + 4 <= end; i += 4) {
            int prog = (pkt[i] << 8) | pkt[i + 1];
            if (prog != 0) {
                m->pmt_pid = ((pkt[i + 2] & 0x1F) << 8) | pkt[i + 3];
                break;
            }
        }
    } else if (pid == m->pmt_pid && s[0] == 0x02 && off + 12 <= TSP) {
        int pil = ((s[10] & 0x0F) << 8) | s[11];
        int vpid = -1, apid = -1;
        for (int i = off + 12 + pil; i + 5 <= end;) {
            int type = pkt[i], epid = ((pkt[i + 1] & 0x1F) << 8) | pkt[i + 2];
            int eil = ((pkt[i + 3] & 0x0F) << 8) | pkt[i + 4];
            if (type == 0x02 && vpid < 0)
                vpid = epid;
            else if ((type == 0x03 || type == 0x04) && apid < 0)
                apid = epid;
            i += 5 + eil;
        }
        if (vpid > 0 && vpid != m->vpid) {
            m->vpid = vpid;
            m->vpes_on = 0;
            m->vpes.n = 0;
        }
        if (apid > 0 && apid != m->apid) {
            m->apid = apid;
            m->apes_on = 0;
            m->apes.n = 0;
        }
    }
}

static void ts_packet(hdvmux_t *m, const uint8_t *pkt)
{
    if (pkt[0] != 0x47) {
        /* a lost sync: whatever PID it was, both streams may have lost a packet */
        m->vpes_loss = 1;
        if (m->apes_loss_at < 0)
            m->apes_loss_at = (long)m->apes.n;
        return;
    }
    int pid = ((pkt[1] & 0x1F) << 8) | pkt[2];
    int tei = pkt[1] & 0x80, pusi = pkt[1] & 0x40;
    int afc = (pkt[3] >> 4) & 3, cc = pkt[3] & 0x0F;
    if (pid == 0 || (m->pmt_pid > 0 && pid == m->pmt_pid)) {
        if (!tei)
            parse_psi(m, pkt, pid);
        return;
    }
    int is_v = pid == m->vpid, is_a = pid == m->apid;
    if (!is_v && !is_a)
        return;
    int off = 4;
    if (afc & 2)
        off += 1 + pkt[4];
    int has_payload = (afc & 1) && off < TSP;
    int loss = tei != 0;
    if (has_payload) {
        int last = m->cc[pid];
        if (last >= 0 && cc == last && !tei)
            return;                   /* a duplicate packet */
        if (last >= 0 && cc != ((last + 1) & 0x0F))
            loss = 1;
        m->cc[pid] = cc;
    }
    if (tei) {
        /* its payload cannot be trusted: as if it were lost */
        if (is_v)
            m->vpes_loss = 1;
        else if (m->apes_loss_at < 0)
            m->apes_loss_at = (long)m->apes.n;
        return;
    }
    if (!has_payload)
        return;
    if (is_v) {
        if (loss)
            m->vpes_loss = 1;
        if (pusi) {
            video_pes_done(m);
            m->vpes_on = 1;
            m->vpes_loss = 0;
            m->vpes.n = 0;
        } else if (!m->vpes_on) {
            m->au_dmg = 1;            /* the start of this PES is lost */
            return;
        }
        if (buf_add(&m->vpes, pkt + off, (size_t)(TSP - off)) < 0)
            set_error(m, PIN_ERR_NOMEM);
    } else {
        if (loss && m->apes_loss_at < 0)
            m->apes_loss_at = (long)m->apes.n;
        if (pusi) {
            audio_pes_done(m);
            m->apes_on = 1;
            m->apes_loss_at = -1;
            m->apes.n = 0;
        } else if (!m->apes_on) {
            return;
        }
        if (buf_add(&m->apes, pkt + off, (size_t)(TSP - off)) < 0)
            set_error(m, PIN_ERR_NOMEM);
    }
}

/* --------------------------------------------------------------- sink --- */

static pin_status_t hdvmux_open(pin_sink_t *s, const char *path, const pin_sink_params_t *params)
{
    hdvmux_t *m = s->priv;
    m->params = *params;
    if (strlen(path) >= sizeof(m->path))
        return PIN_ERR_ARG;
    snprintf(m->path, sizeof(m->path), "%s", path);
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

    int cpus = av_cpu_count();
    m->nthr = cpus >= 8 ? 4 : cpus >= 4 ? 2 : 1;
    m->enc_threads = cpus / m->nthr > 1 ? cpus / m->nthr : 1;
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
    if (m->st.last_error != PIN_OK)
        return m->st.last_error;
    for (size_t i = 0; i + TSP <= len; i += TSP)
        ts_packet(m, data + i);
    m->st.units_written++;
    drain(m, 0);
    if (m->st.last_error == PIN_OK && m->pend_bytes > m->pend_limit) {
        m->st.reencode_behind = 1;
        set_error(m, PIN_ERR_CODEC);
        pin_logf(PIN_LOG_ERROR, "sink_hdvmux: repairing damaged pictures cannot keep up with the "
                                "capture (%.0f MB waiting)\n", m->pend_bytes / 1e6);
    }
    return m->st.last_error;
}

static void free_items(hdvmux_t *m)
{
    while (m->head) {
        item_t *it = m->head;
        m->head = it->next;
        pics_free(&it->pics);
        pics_free(&it->job.pics);
        free(it->job.slot);
        for (int i = 0; i < it->job.nout; i++)
            av_packet_free(&it->job.out[i]);
        free(it->job.out);
        free(it);
    }
    m->tail = NULL;
}

static pin_status_t hdvmux_close(pin_sink_t *s)
{
    hdvmux_t *m = s->priv;
    if (m->fmt && m->st.last_error == PIN_OK) {
        video_pes_done(m);
        audio_pes_done(m);
        es_flush(m);
        check_last_gop(m);
        gop_done(m);
        span_close(m, NOPTS, NULL, 0);
        drain(m, 1);
        drain_audio(m, NOPTS, 1);
    }
    pthread_mutex_lock(&m->lock);
    m->quit = 1;
    pthread_cond_broadcast(&m->wake);
    pthread_mutex_unlock(&m->lock);
    for (int i = 0; i < m->nthr; i++)
        pthread_join(m->thr[i], NULL);
    m->nthr = 0;

    pin_status_t rc = PIN_OK;
    if (m->fmt) {
        if (m->header_written && av_write_trailer(m->fmt) < 0)
            set_error(m, PIN_ERR_IO);
        if (m->fault_io)
            pin_sink_fault_avio_close(&m->fmt->pb);
        else if (m->fmt->pb)
            avio_closep(&m->fmt->pb);
        avformat_free_context(m->fmt);
        if (!m->any_written) {
            pin_logf(PIN_LOG_WARN, "sink_hdvmux: no HDV pictures captured, leaving no output file\n");
            remove(m->path);
        }
    }
    if (m->spans)
        pin_logf(PIN_LOG_INFO, "sink_hdvmux: %s: %llu damaged span(s), %llu picture(s) re-encoded, "
                               "%llu leading B picture(s) without their reference dropped\n",
                 m->path, (unsigned long long)m->spans, (unsigned long long)m->pics_reencoded,
                 (unsigned long long)m->pics_dropped);
    rc = m->st.last_error;
    free_items(m);
    for (int i = 0; i < m->aq_n; i++)
        free(m->aq[i].data);
    free(m->aq);
    pics_free(&m->gop);
    pics_free(&m->warm);
    buf_free(&m->vpes);
    buf_free(&m->apes);
    buf_free(&m->es);
    buf_free(&m->acarry);
    free(m->silence);
    pthread_mutex_destroy(&m->lock);
    pthread_cond_destroy(&m->wake);
    pthread_cond_destroy(&m->done_cv);
    free(m);
    free(s);
    return rc;
}

static void hdvmux_get_status(pin_sink_t *s, pin_sink_status_t *out)
{
    hdvmux_t *m = s->priv;
    *out = m->st;
    out->units_damaged = m->pics_reencoded;
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
    m->pmt_pid = -1;
    m->vpid = -1;
    m->apid = -1;
    for (int i = 0; i < 8192; i++)
        m->cc[i] = -1;
    m->apes_loss_at = -1;
    m->last_ts = NOPTS;
    m->au_pts = NOPTS;
    m->last_cam_end = NOPTS;
    m->a_next_cam = NOPTS;
    m->dur = 3600;
    m->pend_limit = PEND_LIMIT;
    pthread_mutex_init(&m->lock, NULL);
    pthread_cond_init(&m->wake, NULL);
    pthread_cond_init(&m->done_cv, NULL);
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
