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
 * PIN_FMT_ANALOG_H264_MKV / _HEVC_MKV and PIN_FMT_DV_H264_MKV / _HEVC_MKV:
 * analog or DV to H.264 / HEVC with the operating system's own encoder
 * (pin_hwenc.h says which one and how it is set up) + FLAC, muxed live as
 * Matroska, so a crash or a flat battery leaves a playable file and nothing
 * is remuxed at stop.
 *
 * Threads, as in sink_ffv1.c: the pin_writer consumer thread calls
 * write_video / write_audio (analog) or write_unit (DV). It only does the
 * cheap part (YUY2 -> planar 4:2:2; DV: a copy of the frame, and its audio
 * pulled out with the per-frame dv demux) and hands the picture to a bounded
 * queue. The encoder thread decodes DV (FFmpeg's dvvideo, already built in
 * for the preview), deinterlaces if asked, converts to the encoder's 4:2:0
 * layout (hwenc_convert.h) and encodes. Each audio track has its own FLAC
 * thread behind a ring. mux_lock serialises the threads' writes into the one
 * AVFormatContext. A full queue makes the writer wait, as in sink_ffv1.c
 * (pin_writer's much larger ring is in front of it); a wait that sees no
 * progress for STALL_TIMEOUT_S, a hung hardware encoder, fails the sink
 * instead of blocking the writer for good, and the capture ends with an
 * error.
 *
 * HDV (not done yet; PIN_FMT_HDV_H264_MKV / _HEVC_MKV would be the formats): the shared
 * HDV stages do the input side (hdv_demux.h has the plan). write_unit feeds an hdv_demux_t;
 * its GOPs go to an hdv_decoder_t on the encoder thread, in place of the DV decode here, whose
 * frames (4:2:0 already, 1440x1080 or 1280x720, interlaced or not) go through the same
 * deinterlace / convert / encode path; its MP2 frames are decoded to PCM for the FLAC thread.
 * Frame timing, lost pictures, audio placement and the error counts follow sink_hdvmux.c's
 * rules, which should move into a shared HDV timeline piece then, not be copied.
 *
 * The encoder is opened, used and freed on the encoder thread only: Media
 * Foundation ties COM to the thread that opens it. open() waits for that
 * thread to report whether the encoder opened, so a missing encoder fails
 * open() (PIN_ERR_CODEC), not the first frame.
 *
 * Interlacing (pin_sink_params_t.deinterlace):
 *   keep fields  each frame is encoded as one progressive picture and flagged
 *                progressive (none of the encoders can code fields: MediaCodec
 *                has no field mode, h264_mf forces progressive). Chroma is
 *                subsampled per field so the fields do not bleed into each
 *                other; the encoder still compresses the two fields as one
 *                picture, which costs quality on motion (the GUIs show
 *                pin_format_options()'s warning).
 *   double rate  bwdif (libavfilter, LGPL) in send_field mode on the frame as
 *                it is (4:2:2 analog, 4:2:0 / 4:1:1 DV), field order from the
 *                source (analog TFF, DV BFF): 25i -> 50p, 29.97i -> 59.94p.
 *                The progressive result is then subsampled to 4:2:0.
 *
 * Timestamps: int64 frame counts (video, at the encoder's rate) and sample
 * counts (audio), rescaled by libavformat, as in sink_ffv1.c. DV audio goes
 * through sink_rewrap.c's rule (pin_dv_audio_fit): every sample a frame
 * carries is kept while the track is within half a frame of the video, and
 * a frame whose audio did not demux is padded with silence.
 *
 * The Matroska header needs every stream and the video's parameter sets.
 * Analog knows its audio at open(); DV only once a frame's audio has been
 * demuxed (32 / 44.1 / 48 kHz, one or two stereo pairs), so up to
 * DV_HOLD_MAX frames are held back until one has audio, then all of them go
 * through. An encoder that hands out its parameter sets only with the first
 * picture (some Media Foundation hardware encoders) has them taken from that
 * picture's packet. Until the header is out, packets wait in a short list.
 */

#include "pin_sink.h"
#include "sinks_internal.h"
#include "pin_hwenc.h"
#include "hwenc_convert.h"
#include "../core/pin_log.h"
#include "../engine/dv_subcode.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/cpu.h>
#include <libavutil/opt.h>

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#define QUEUE_DEPTH 8
#define AUDIO_RING_SECONDS 4
#define FLAC_LEVEL 8             /* as sink_ffv1.c: the highest within the FLAC subset */
#define DV_HOLD_MAX 8            /* DV frames held back while looking for one with audio */
#define STALL_TIMEOUT_S 10       /* no progress for this long: the encoder is taken as hung */
#define MAX_PENDING 1024         /* packets waiting for the header */

typedef struct hwenc_priv hwenc_priv_t;

/* One FLAC track: analog's one, or DV's pair 1 and (32 kHz four-channel mode) pair 2. */
typedef struct {
    hwenc_priv_t *p;
    AVStream *st;
    AVCodecContext *enc;
    int channels, rate;
    int64_t apts;           /* samples handed to the ring (the writer thread's DV timeline) */
    int64_t enc_pts;        /* samples encoded (the audio thread's) */
    int16_t *ring;          /* interleaved s16, positions in sample frames */
    size_t ring_frames, head, count;
    int closing;
    pthread_mutex_t lock;
    pthread_cond_t wake, space;
    pthread_t thread;
    int started;
} track_t;

/* A picture on its way to the encoder thread: analog a YUV422P frame, DV the DIF bytes. */
typedef struct {
    AVFrame *frame;
    uint8_t *dif;
    size_t dif_len;
    int64_t pts;            /* source frame number */
} item_t;

typedef struct {
    AVPacket *pkt;
    AVRational tb;          /* of pkt's timestamps */
} pending_t;

struct hwenc_priv {
    pin_format_t format;
    pin_hwenc_codec_t codec;
    int dv;                 /* input is DV (write_unit) */
    pin_sink_params_t params;
    int deint;              /* bwdif, double rate */
    int is_625;
    int sar_num, sar_den;
    int fps_num, fps_den;   /* the source's */

    AVFormatContext *fmt;
    AVStream *vst;
    pthread_mutex_t mux_lock;
    int declared;           /* every stream exists (DV: after the first frame with audio) */
    int vparams_ready;      /* the video stream has its parameter sets (or needs none) */
    int header_written, header_failed;
    int fault_io;           /* fmt->pb is pin_sink_fault_avio_open()'s (PIN_TEST_WRITE_FAIL_AFTER) */
    pending_t *pending;
    int npending;
    int warned_no_extradata;

    track_t trk[2];
    int ntracks;
    int warned_late_pair;

    /* bounded queue -> encoder thread */
    item_t queue[QUEUE_DEPTH];
    int qhead, qcount;
    int closing;
    pthread_mutex_t qlock;
    pthread_cond_t qwake, qspace;
    pthread_t enc_thread;
    int enc_started;
    int enc_state;          /* 0 starting, 1 encoder open, -1 it did not open */
    pthread_cond_t enc_cond;
    int64_t vpts;           /* source frames queued (writer thread) */

    /* encoder thread only */
    AVCodecContext *venc;
    AVCodecContext *dvdec;
    AVFilterGraph *graph;
    AVFilterContext *bsrc, *bsink;
    int graph_failed;
    int enc_failed;
    int warned_size;
    int64_t pictures_sent;
    int64_t fail_after;     /* PIN_HWENC_FAIL_AFTER (tests): the encoder "fails" after this many */

    /* DV frames held until their audio is known (writer thread) */
    uint8_t *hold[DV_HOLD_MAX];
    size_t hold_len[DV_HOLD_MAX];
    int nhold;

    pin_sink_status_t st;
    pthread_mutex_t st_lock;
};

static int is_hevc_format(pin_format_t f)
{
    return f == PIN_FMT_ANALOG_HEVC_MKV || f == PIN_FMT_DV_HEVC_MKV;
}

static int is_dv_format(pin_format_t f)
{
    return f == PIN_FMT_DV_H264_MKV || f == PIN_FMT_DV_HEVC_MKV;
}

static void set_error(hwenc_priv_t *p, pin_status_t e)
{
    pthread_mutex_lock(&p->st_lock);
    if (p->st.last_error == PIN_OK)
        p->st.last_error = e;
    pthread_mutex_unlock(&p->st_lock);
}

static pin_status_t get_error(hwenc_priv_t *p)
{
    pthread_mutex_lock(&p->st_lock);
    pin_status_t e = p->st.last_error;
    pthread_mutex_unlock(&p->st_lock);
    return e;
}

static void count_damaged(hwenc_priv_t *p)
{
    pthread_mutex_lock(&p->st_lock);
    p->st.units_damaged++;
    pthread_mutex_unlock(&p->st_lock);
}

static void set_behind(hwenc_priv_t *p)
{
    pthread_mutex_lock(&p->st_lock);
    p->st.encoder_behind = 1;
    pthread_mutex_unlock(&p->st_lock);
}

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void sleep_ms(int ms)
{
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

/* pthread_cond_timedwait() takes a CLOCK_REALTIME deadline. Returns ETIMEDOUT or 0. */
static int timed_wait(pthread_cond_t *c, pthread_mutex_t *m, int seconds)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += seconds;
    return pthread_cond_timedwait(c, m, &ts);
}

/* ---- muxing ------------------------------------------------------------------------ */

static int is_param_set(pin_hwenc_codec_t c, const uint8_t *nal)
{
    if (c == PIN_HWENC_HEVC) {
        int t = (nal[0] >> 1) & 0x3f;
        return t == 32 || t == 33 || t == 34;   /* VPS, SPS, PPS */
    }
    int t = nal[0] & 0x1f;
    return t == 7 || t == 8;                    /* SPS, PPS */
}

/* The parameter-set NAL units of an Annex B packet, with their start codes, for an encoder
 * that did not hand them out as extradata. Returns the size written to *out (0: none). */
static int param_sets_from_packet(pin_hwenc_codec_t c, const uint8_t *d, int n, uint8_t **out)
{
    *out = NULL;
    uint8_t *buf = av_mallocz((size_t)n + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!buf)
        return 0;
    int len = 0;
    int i = 0;
    while (i + 3 < n) {
        if (!(d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)) {
            i++;
            continue;
        }
        int start = i + 3;
        int end = start;
        while (end + 2 < n && !(d[end] == 0 && d[end + 1] == 0 && (d[end + 2] == 1 ||
                                (d[end + 2] == 0 && end + 3 < n && d[end + 3] == 1))))
            end++;
        if (end + 2 >= n)
            end = n;
        if (start < end && is_param_set(c, d + start)) {
            static const uint8_t sc[4] = { 0, 0, 0, 1 };
            memcpy(buf + len, sc, 4);
            memcpy(buf + len + 4, d + start, (size_t)(end - start));
            len += 4 + (end - start);
        }
        i = end;
    }
    if (len == 0) {
        av_free(buf);
        return 0;
    }
    *out = buf;
    return len;
}

static void write_now_locked(hwenc_priv_t *p, AVPacket *pkt, AVRational tb)
{
    AVStream *st = p->fmt->streams[pkt->stream_index];
    av_packet_rescale_ts(pkt, tb, st->time_base);
    size_t size = (size_t)pkt->size;
    int is_video = st == p->vst;
    int rc = av_interleaved_write_frame(p->fmt, pkt);
    if (rc < 0) {
        pin_logf(PIN_LOG_ERROR, "sink_hwenc: mux write failed (%d)\n", rc);
        set_error(p, PIN_ERR_IO);
        return;
    }
    pthread_mutex_lock(&p->st_lock);
    p->st.bytes_written += size;
    if (is_video)
        p->st.units_written++;
    pthread_mutex_unlock(&p->st_lock);
}

/* Writes the header once every stream and the video's parameter sets are known, then the
 * packets that waited for it. Caller holds mux_lock. */
static void try_header_locked(hwenc_priv_t *p)
{
    /* no pb: open() failed before the file was created, and close() is tidying up */
    if (p->header_written || p->header_failed || !p->declared || !p->vparams_ready || !p->fmt->pb)
        return;
    int rc = avformat_write_header(p->fmt, NULL);
    if (rc < 0) {
        pin_logf(PIN_LOG_ERROR, "sink_hwenc: avformat_write_header failed (%d)\n", rc);
        p->header_failed = 1;
        set_error(p, PIN_ERR_CODEC);
    } else {
        p->header_written = 1;
    }
    for (int i = 0; i < p->npending; i++) {
        if (p->header_written)
            write_now_locked(p, p->pending[i].pkt, p->pending[i].tb);
        av_packet_free(&p->pending[i].pkt);
    }
    p->npending = 0;
}

/* Muxes pkt (taken over), or keeps it until the header is out. */
static void mux_packet(hwenc_priv_t *p, AVPacket *pkt, AVRational tb)
{
    pthread_mutex_lock(&p->mux_lock);
    if (p->header_written) {
        write_now_locked(p, pkt, tb);
        av_packet_free(&pkt);
    } else if (p->header_failed || p->npending >= MAX_PENDING) {
        if (!p->header_failed)
            pin_logf(PIN_LOG_ERROR, "sink_hwenc: too many packets before the header could be written\n");
        set_error(p, PIN_ERR_CODEC);
        av_packet_free(&pkt);
    } else {
        if (!p->pending)
            p->pending = calloc(MAX_PENDING, sizeof(*p->pending));
        if (p->pending) {
            p->pending[p->npending].pkt = pkt;
            p->pending[p->npending].tb = tb;
            p->npending++;
        } else {
            set_error(p, PIN_ERR_NOMEM);
            av_packet_free(&pkt);
        }
    }
    pthread_mutex_unlock(&p->mux_lock);
}

/* ---- video: the encoder thread ----------------------------------------------------- */

/* The first video packet of an encoder without extradata carries the parameter sets. */
static void video_params_from_packet(hwenc_priv_t *p, const AVPacket *pkt)
{
    pthread_mutex_lock(&p->mux_lock);
    if (!p->vparams_ready) {
        uint8_t *ps = NULL;
        int n = param_sets_from_packet(p->codec, pkt->data, pkt->size, &ps);
        if (n > 0) {
            av_freep(&p->vst->codecpar->extradata);
            p->vst->codecpar->extradata = ps;
            p->vst->codecpar->extradata_size = n;
            pin_logf(PIN_LOG_DEBUG, "sink_hwenc: parameter sets (%d bytes) taken from the first packet\n", n);
        } else {
            pin_logf(PIN_LOG_WARN, "sink_hwenc: the encoder gave no parameter sets; players have to "
                                   "find them in the stream\n");
        }
        p->vparams_ready = 1;
        try_header_locked(p);
    }
    pthread_mutex_unlock(&p->mux_lock);
}

/* Receives what the encoder has ready and muxes it. flushing: after the NULL frame, waits
 * (up to STALL_TIMEOUT_S without output) for the encoder to say it is done. Returns the
 * number of packets, or <0 on an error. */
static int drain_packets(hwenc_priv_t *p, int flushing)
{
    int got = 0;
    double last = mono_now();
    for (;;) {
        AVPacket *pkt = av_packet_alloc();
        if (!pkt)
            return AVERROR(ENOMEM);
        int rc = avcodec_receive_packet(p->venc, pkt);
        if (rc == AVERROR(EAGAIN) && flushing) {
            av_packet_free(&pkt);
            if (mono_now() - last > STALL_TIMEOUT_S) {
                pin_logf(PIN_LOG_WARN, "sink_hwenc: the encoder did not finish its last frames\n");
                return got;
            }
            sleep_ms(2);
            continue;
        }
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
            av_packet_free(&pkt);
            return got;
        }
        if (rc < 0) {
            av_packet_free(&pkt);
            pin_logf(PIN_LOG_ERROR, "sink_hwenc: avcodec_receive_packet failed (%d)\n", rc);
            return rc;
        }
        last = mono_now();
        if (pkt->dts == AV_NOPTS_VALUE)
            pkt->dts = pkt->pts;    /* no B-frames: decode order is presentation order */
        if (!p->vparams_ready)
            video_params_from_packet(p, pkt);
        pkt->stream_index = p->vst->index;
        mux_packet(p, pkt, p->venc->time_base);
        got++;
    }
}

/* Encodes one picture (NULL flushes). An encoder that takes no input and gives no output for
 * STALL_TIMEOUT_S counts as failed. */
static int encode_frame(hwenc_priv_t *p, AVFrame *f)
{
    double last = mono_now();
    for (;;) {
        int rc = avcodec_send_frame(p->venc, f);
        if (rc == 0 || (rc == AVERROR_EOF && !f))
            break;
        if (rc != AVERROR(EAGAIN)) {
            pin_logf(PIN_LOG_ERROR, "sink_hwenc: avcodec_send_frame failed (%d)\n", rc);
            return rc;
        }
        int got = drain_packets(p, 0);      /* full: take output first */
        if (got < 0)
            return got;
        if (got > 0) {
            last = mono_now();
        } else {
            if (mono_now() - last > STALL_TIMEOUT_S) {
                pin_logf(PIN_LOG_ERROR, "sink_hwenc: the encoder stopped taking pictures\n");
                return AVERROR(ETIMEDOUT);
            }
            sleep_ms(2);
        }
    }
    int got = drain_packets(p, f == NULL);
    return got < 0 ? got : 0;
}

/* Converts a source-rate picture (field_aware) or a deinterlaced one to the encoder's
 * layout and encodes it. pts in the encoder's time base. */
static int encode_converted(hwenc_priv_t *p, const AVFrame *src, int field_aware, int64_t pts)
{
    AVFrame *f = av_frame_alloc();
    if (!f)
        return AVERROR(ENOMEM);
    f->format = p->venc->pix_fmt;
    f->width = p->venc->width;
    f->height = p->venc->height;
    int rc = av_frame_get_buffer(f, 0);
    if (rc >= 0 && pin_hwenc_to_420(src, f, field_aware) < 0)
        rc = AVERROR(EINVAL);
    if (rc < 0) {
        av_frame_free(&f);
        return rc;
    }
    f->pts = pts;
    f->sample_aspect_ratio = (AVRational){ p->sar_num, p->sar_den };
    f->color_range = p->venc->color_range;
    f->color_primaries = p->venc->color_primaries;
    f->color_trc = p->venc->color_trc;
    f->colorspace = p->venc->colorspace;
    if (p->fail_after > 0 && p->pictures_sent >= p->fail_after) {
        /* It still hands out the pictures it has taken (a hardware encoder holds a few), so the
         * file keeps exactly fail_after of them on every encoder, then it "dies". */
        encode_frame(p, NULL);
        pin_logf(PIN_LOG_ERROR, "sink_hwenc: PIN_HWENC_FAIL_AFTER: the encoder fails now\n");
        rc = AVERROR_EXTERNAL;
    } else {
        rc = encode_frame(p, f);
        p->pictures_sent++;
    }
    av_frame_free(&f);
    return rc;
}

/* bwdif on frames like `first` (its pixel format and size). */
static int graph_init(hwenc_priv_t *p, const AVFrame *first)
{
    p->graph = avfilter_graph_alloc();
    if (!p->graph)
        return AVERROR(ENOMEM);
    int threads = av_cpu_count();
    p->graph->nb_threads = threads > 4 ? 4 : threads;

    const AVFilter *src = avfilter_get_by_name("buffer");
    const AVFilter *sink = avfilter_get_by_name("buffersink");
    const AVFilter *bwdif = avfilter_get_by_name("bwdif");
    if (!src || !sink || !bwdif)
        return AVERROR_FILTER_NOT_FOUND;

    p->bsrc = avfilter_graph_alloc_filter(p->graph, src, "in");
    if (!p->bsrc)
        return AVERROR(ENOMEM);
    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
    if (!par)
        return AVERROR(ENOMEM);
    par->format = first->format;
    par->width = first->width;
    par->height = first->height;
    par->time_base = (AVRational){ p->fps_den, p->fps_num };
    par->frame_rate = (AVRational){ p->fps_num, p->fps_den };
    par->sample_aspect_ratio = (AVRational){ p->sar_num, p->sar_den };
    par->color_space = first->colorspace;
    par->color_range = first->color_range;
    int rc = av_buffersrc_parameters_set(p->bsrc, par);
    av_free(par);
    if (rc < 0 || (rc = avfilter_init_str(p->bsrc, NULL)) < 0)
        return rc;

    /* send_field: one picture per field. The field order is the source's, not guessed from
     * frame flags (DV is bottom field first, analog capture top field first). */
    char args[96];
    snprintf(args, sizeof(args), "mode=send_field:parity=%s:deint=all",
             p->params.top_field_first ? "tff" : "bff");
    AVFilterContext *deint = NULL;
    rc = avfilter_graph_create_filter(&deint, bwdif, "deint", args, NULL, p->graph);
    if (rc < 0)
        return rc;
    rc = avfilter_graph_create_filter(&p->bsink, sink, "out", NULL, NULL, p->graph);
    if (rc < 0)
        return rc;
    if ((rc = avfilter_link(p->bsrc, 0, deint, 0)) < 0 ||
        (rc = avfilter_link(deint, 0, p->bsink, 0)) < 0)
        return rc;
    /* bwdif keeps the input's pixel format, so no conversion filter is needed (there is
     * none: libswscale is not built) */
    return avfilter_graph_config(p->graph, NULL);
}

/* Takes the deinterlaced pictures out of the graph and encodes them. */
static int graph_drain(hwenc_priv_t *p)
{
    AVRational tb = av_buffersink_get_time_base(p->bsink);
    for (;;) {
        AVFrame *out = av_frame_alloc();
        if (!out)
            return AVERROR(ENOMEM);
        int rc = av_buffersink_get_frame(p->bsink, out);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
            av_frame_free(&out);
            return 0;
        }
        if (rc < 0) {
            av_frame_free(&out);
            return rc;
        }
        int64_t pts = av_rescale_q(out->pts, tb, p->venc->time_base);
        rc = encode_converted(p, out, 0, pts);
        av_frame_free(&out);
        if (rc < 0)
            return rc;
    }
}

/* One source picture (pts = source frame number), in its native layout. */
static int process_picture(hwenc_priv_t *p, AVFrame *src, int64_t pts)
{
    if (src->width != p->venc->width || src->height != p->venc->height) {
        if (!p->warned_size) {
            pin_logf(PIN_LOG_WARN, "sink_hwenc: a %dx%d picture in a %dx%d file, skipped\n",
                     src->width, src->height, p->venc->width, p->venc->height);
            p->warned_size = 1;
        }
        count_damaged(p);
        return 0;
    }
    src->pts = pts;
    src->sample_aspect_ratio = (AVRational){ p->sar_num, p->sar_den };
    src->colorspace = p->venc->colorspace;
    src->color_range = AVCOL_RANGE_MPEG;
    if (!p->deint)
        return encode_converted(p, src, 1, pts);

    if (!p->graph && !p->graph_failed) {
        int rc = graph_init(p, src);
        if (rc < 0) {
            pin_logf(PIN_LOG_ERROR, "sink_hwenc: the deinterlacer did not start (%d)\n", rc);
            p->graph_failed = 1;
            return rc;
        }
    }
    if (p->graph_failed)
        return AVERROR(EINVAL);
    src->flags |= AV_FRAME_FLAG_INTERLACED;
    if (p->params.top_field_first)
        src->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
    else
        src->flags &= ~AV_FRAME_FLAG_TOP_FIELD_FIRST;
    int rc = av_buffersrc_add_frame_flags(p->bsrc, src, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (rc < 0)
        return rc;
    return graph_drain(p);
}

static int decode_dv(hwenc_priv_t *p, const item_t *it)
{
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    int rc = AVERROR(ENOMEM);
    if (pkt && frame) {
        pkt->data = it->dif;
        pkt->size = (int)it->dif_len;
        rc = avcodec_send_packet(p->dvdec, pkt);
        if (rc >= 0)
            rc = avcodec_receive_frame(p->dvdec, frame);
        if (rc >= 0) {
            rc = process_picture(p, frame, it->pts);
        } else {
            /* the frame stays out; its neighbours keep their timestamps, so audio stays in step */
            pin_logf(PIN_LOG_DEBUG, "sink_hwenc: DV frame %lld did not decode (%d)\n",
                     (long long)it->pts, rc);
            count_damaged(p);
            rc = 0;
        }
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    return rc;
}

static void item_free(item_t *it)
{
    av_frame_free(&it->frame);
    free(it->dif);
    it->dif = NULL;
}

static int encoder_open(hwenc_priv_t *p)
{
    pin_hwenc_config_t cfg = {
        .width = p->params.width,
        .height = p->params.height,
        .fps_num = p->deint ? p->fps_num * 2 : p->fps_num,
        .fps_den = p->fps_den,
        .doubled = p->deint,
        .quality = p->params.video_quality,
        .sar_num = p->sar_num,
        .sar_den = p->sar_den,
        .is_625 = p->is_625,
        .global_header = (p->fmt->oformat->flags & AVFMT_GLOBALHEADER) != 0,
    };
    p->venc = pin_hwenc_open(p->codec, &cfg);
    if (!p->venc) {
        pin_logf(PIN_LOG_ERROR, "sink_hwenc: the %s encoder (%s) did not open\n",
                 p->codec == PIN_HWENC_HEVC ? "HEVC" : "H.264",
                 pin_hwenc_encoder_name(p->codec) ? pin_hwenc_encoder_name(p->codec) : "none here");
        return -1;
    }
    if (p->dv) {
        const AVCodec *dec = avcodec_find_decoder(AV_CODEC_ID_DVVIDEO);
        p->dvdec = dec ? avcodec_alloc_context3(dec) : NULL;
        if (!p->dvdec)
            return -1;
        int threads = av_cpu_count();
        p->dvdec->thread_count = threads > 4 ? 4 : threads;
        p->dvdec->thread_type = FF_THREAD_SLICE;
        if (avcodec_open2(p->dvdec, dec, NULL) < 0) {
            pin_logf(PIN_LOG_ERROR, "sink_hwenc: the DV decoder did not open\n");
            return -1;
        }
    }
    return 0;
}

static void *encoder_thread_fn(void *arg)
{
    hwenc_priv_t *p = arg;
    int ok = encoder_open(p) == 0;
    pthread_mutex_lock(&p->qlock);
    p->enc_state = ok ? 1 : -1;
    pthread_cond_broadcast(&p->enc_cond);
    pthread_mutex_unlock(&p->qlock);
    if (!ok) {
        avcodec_free_context(&p->venc);
        avcodec_free_context(&p->dvdec);
        return NULL;
    }

    pthread_mutex_lock(&p->qlock);
    for (;;) {
        while (p->qcount == 0 && !p->closing)
            pthread_cond_wait(&p->qwake, &p->qlock);
        if (p->qcount == 0 && p->closing)
            break;
        item_t it = p->queue[p->qhead];
        p->qhead = (p->qhead + 1) % QUEUE_DEPTH;
        p->qcount--;
        pthread_cond_signal(&p->qspace);
        pthread_mutex_unlock(&p->qlock);

        if (!p->enc_failed) {
            int rc = it.dif ? decode_dv(p, &it) : process_picture(p, it.frame, it.pts);
            if (rc < 0) {
                /* the writer sees the error on its next call and ends the capture; the queue
                 * is still emptied so it never waits on a dead encoder */
                p->enc_failed = 1;
                set_error(p, PIN_ERR_CODEC);
            }
        }
        item_free(&it);

        pthread_mutex_lock(&p->qlock);
    }
    pthread_mutex_unlock(&p->qlock);

    if (!p->enc_failed) {
        int rc = 0;
        if (p->graph) {
            rc = av_buffersrc_add_frame(p->bsrc, NULL);     /* bwdif emits the last field */
            if (rc >= 0)
                rc = graph_drain(p);
        }
        if (rc >= 0)
            rc = encode_frame(p, NULL);
        if (rc < 0)
            set_error(p, PIN_ERR_CODEC);
    }
    avfilter_graph_free(&p->graph);
    avcodec_free_context(&p->dvdec);
    /* freed here, not in close(): Media Foundation's COM belongs to this thread */
    avcodec_free_context(&p->venc);
    return NULL;
}

/* Hands one picture to the encoder thread (taking over it); waits while the queue is full. */
static pin_status_t queue_push(hwenc_priv_t *p, item_t *it)
{
    pthread_mutex_lock(&p->qlock);
    if (p->qcount == QUEUE_DEPTH) {
        set_behind(p);
        while (p->qcount == QUEUE_DEPTH) {
            if (timed_wait(&p->qspace, &p->qlock, STALL_TIMEOUT_S) == ETIMEDOUT &&
                p->qcount == QUEUE_DEPTH) {
                pthread_mutex_unlock(&p->qlock);
                pin_logf(PIN_LOG_ERROR, "sink_hwenc: the encoder took no picture for %d s\n",
                         STALL_TIMEOUT_S);
                set_error(p, PIN_ERR_CODEC);
                item_free(it);
                return PIN_ERR_CODEC;
            }
        }
    }
    p->queue[(p->qhead + p->qcount) % QUEUE_DEPTH] = *it;
    p->qcount++;
    pthread_cond_signal(&p->qwake);
    pthread_mutex_unlock(&p->qlock);
    return PIN_OK;
}

/* ---- audio: one FLAC thread per track ---------------------------------------------- */

static void encode_audio(track_t *t, AVFrame *frame)
{
    hwenc_priv_t *p = t->p;
    int rc = avcodec_send_frame(t->enc, frame);
    if (rc < 0) {
        pin_logf(PIN_LOG_ERROR, "sink_hwenc: avcodec_send_frame(flac) failed (%d)\n", rc);
        set_error(p, PIN_ERR_CODEC);
        return;
    }
    for (;;) {
        AVPacket *pkt = av_packet_alloc();
        if (!pkt)
            return;
        rc = avcodec_receive_packet(t->enc, pkt);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
            av_packet_free(&pkt);
            break;
        }
        if (rc < 0) {
            av_packet_free(&pkt);
            pin_logf(PIN_LOG_ERROR, "sink_hwenc: avcodec_receive_packet(flac) failed (%d)\n", rc);
            set_error(p, PIN_ERR_CODEC);
            break;
        }
        pkt->stream_index = t->st->index;
        mux_packet(p, pkt, t->enc->time_base);
    }
}

static void *audio_thread_fn(void *arg)
{
    track_t *t = arg;
    const size_t block = (size_t)t->enc->frame_size;
    const size_t ch = (size_t)t->channels;

    pthread_mutex_lock(&t->lock);
    for (;;) {
        while (t->count < block && !t->closing)
            pthread_cond_wait(&t->wake, &t->lock);
        if (t->count == 0)
            break;
        size_t n = t->count < block ? t->count : block;
        pthread_mutex_unlock(&t->lock);

        AVFrame *f = av_frame_alloc();
        int ok = f != NULL;
        if (ok) {
            f->format = AV_SAMPLE_FMT_S16;
            f->sample_rate = t->rate;
            f->nb_samples = (int)n;
            ok = av_channel_layout_copy(&f->ch_layout, &t->enc->ch_layout) >= 0 &&
                 av_frame_get_buffer(f, 0) >= 0;
        }
        /* only this thread moves head; the writer appends past head + count */
        if (ok) {
            size_t first = t->ring_frames - t->head;
            if (first > n)
                first = n;
            memcpy(f->data[0], t->ring + t->head * ch, first * ch * sizeof(int16_t));
            memcpy(f->data[0] + first * ch * sizeof(int16_t), t->ring,
                   (n - first) * ch * sizeof(int16_t));
            f->pts = t->enc_pts;
        } else {
            set_error(t->p, PIN_ERR_NOMEM);
        }
        t->enc_pts += (int64_t)n;

        pthread_mutex_lock(&t->lock);
        t->head = (t->head + n) % t->ring_frames;
        t->count -= n;
        pthread_cond_signal(&t->space);
        pthread_mutex_unlock(&t->lock);

        if (ok)
            encode_audio(t, f);
        av_frame_free(&f);

        pthread_mutex_lock(&t->lock);
    }
    pthread_mutex_unlock(&t->lock);
    encode_audio(t, NULL);   /* the last packet carries the final STREAMINFO */
    return NULL;
}

/* Appends frames samples (NULL: silence) to the track's ring; waits while it is full. */
static pin_status_t ring_push(track_t *t, const int16_t *pcm, size_t frames)
{
    const size_t ch = (size_t)t->channels;
    pthread_mutex_lock(&t->lock);
    while (frames > 0) {
        if (t->count == t->ring_frames) {
            set_behind(t->p);
            while (t->count == t->ring_frames) {
                if (timed_wait(&t->space, &t->lock, STALL_TIMEOUT_S) == ETIMEDOUT &&
                    t->count == t->ring_frames) {
                    pthread_mutex_unlock(&t->lock);
                    pin_logf(PIN_LOG_ERROR, "sink_hwenc: the audio encoder took nothing for %d s\n",
                             STALL_TIMEOUT_S);
                    set_error(t->p, PIN_ERR_CODEC);
                    return PIN_ERR_CODEC;
                }
            }
        }
        size_t tail = (t->head + t->count) % t->ring_frames;
        size_t n = t->ring_frames - t->count;
        if (n > t->ring_frames - tail)
            n = t->ring_frames - tail;
        if (n > frames)
            n = frames;
        if (pcm) {
            memcpy(t->ring + tail * ch, pcm, n * ch * sizeof(int16_t));
            pcm += n * ch;
        } else {
            memset(t->ring + tail * ch, 0, n * ch * sizeof(int16_t));
        }
        t->count += n;
        frames -= n;
        pthread_cond_signal(&t->wake);
    }
    pthread_mutex_unlock(&t->lock);
    return PIN_OK;
}

/* Creates track i's FLAC encoder, stream and ring and starts its thread. Before the header. */
static pin_status_t track_start(hwenc_priv_t *p, int i, int rate, int channels)
{
    track_t *t = &p->trk[i];
    t->p = p;
    t->rate = rate;
    t->channels = channels;
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_FLAC);
    if (!codec)
        return PIN_ERR_CODEC;
    t->st = avformat_new_stream(p->fmt, NULL);
    t->enc = avcodec_alloc_context3(codec);
    if (!t->st || !t->enc)
        return PIN_ERR_NOMEM;
    t->enc->sample_fmt = AV_SAMPLE_FMT_S16;
    t->enc->sample_rate = rate;
    av_channel_layout_default(&t->enc->ch_layout, channels);
    t->enc->time_base = (AVRational){ 1, rate };
    t->enc->compression_level = FLAC_LEVEL;
    if (p->fmt->oformat->flags & AVFMT_GLOBALHEADER)
        t->enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    int rc = avcodec_open2(t->enc, codec, NULL);
    if (rc < 0) {
        pin_logf(PIN_LOG_ERROR, "sink_hwenc: avcodec_open2(flac) failed (%d)\n", rc);
        return PIN_ERR_CODEC;
    }
    avcodec_parameters_from_context(t->st->codecpar, t->enc);
    t->st->time_base = t->enc->time_base;

    t->ring_frames = (size_t)rate * AUDIO_RING_SECONDS;
    if (t->ring_frames < (size_t)t->enc->frame_size)
        t->ring_frames = (size_t)t->enc->frame_size;
    t->ring = malloc(t->ring_frames * (size_t)channels * sizeof(int16_t));
    if (!t->ring)
        return PIN_ERR_NOMEM;
    if (pthread_create(&t->thread, NULL, audio_thread_fn, t) != 0)
        return PIN_ERR_INTERNAL;
    t->started = 1;
    p->ntracks = i + 1;
    return PIN_OK;
}

/* ---- DV audio: the per-frame dv demux of sink_rewrap.c ----------------------------- */

/* A read-only, seekable in-memory AVIOContext over one DV frame. */
typedef struct { const uint8_t *base; size_t size, pos; } mem_ctx_t;

static int mem_read(void *opaque, uint8_t *buf, int want)
{
    mem_ctx_t *m = opaque;
    size_t left = m->size - m->pos;
    if (!left)
        return AVERROR_EOF;
    size_t n = (size_t)want < left ? (size_t)want : left;
    memcpy(buf, m->base + m->pos, n);
    m->pos += n;
    return (int)n;
}

static int64_t mem_seek(void *opaque, int64_t offset, int whence)
{
    mem_ctx_t *m = opaque;
    int64_t base;
    if (whence == AVSEEK_SIZE)
        return (int64_t)m->size;
    if (whence == SEEK_SET)
        base = 0;
    else if (whence == SEEK_CUR)
        base = (int64_t)m->pos;
    else if (whence == SEEK_END)
        base = (int64_t)m->size;
    else
        return -1;
    int64_t np = base + offset;
    if (np < 0 || np > (int64_t)m->size)
        return -1;
    m->pos = (size_t)np;
    return np;
}

typedef struct {
    int pairs;              /* stereo pairs this frame carries (0..2) */
    int rate[2], channels[2];
    int16_t *pcm[2];        /* interleaved */
    int64_t samples[2];
} dv_audio_t;

static void dv_audio_free(dv_audio_t *a)
{
    free(a->pcm[0]);
    free(a->pcm[1]);
    memset(a, 0, sizeof(*a));
}

/* Demuxes one DV frame with FFmpeg's "dv" demuxer (which knows the DIF audio shuffle, the
 * 12-bit nonlinear mode and the second pair) and collects each pair's s16 samples. A frame it
 * cannot read (a camera's odd header, see sink_rewrap.c) just has no audio. No
 * avformat_find_stream_info(): on one buffered frame it would eat the only audio packet. */
static void dv_demux_audio(const uint8_t *data, size_t len, dv_audio_t *out)
{
    memset(out, 0, sizeof(*out));
    const AVInputFormat *dv_fmt = av_find_input_format("dv");
    if (!dv_fmt)
        return;
    mem_ctx_t mem = { .base = data, .size = len, .pos = 0 };
    uint8_t *iobuf = av_malloc(4096);
    AVIOContext *avio = iobuf ? avio_alloc_context(iobuf, 4096, 0, &mem, mem_read, NULL, mem_seek) : NULL;
    AVFormatContext *in = avio ? avformat_alloc_context() : NULL;
    if (!in) {
        if (avio)
            av_freep(&avio->buffer);
        else
            av_free(iobuf);
        avio_context_free(&avio);
        return;
    }
    in->pb = avio;
    if (avformat_open_input(&in, NULL, dv_fmt, NULL) < 0) {
        av_freep(&avio->buffer);
        avio_context_free(&avio);
        return;
    }
    /* every packet first: the demuxer adds the audio streams as it finds them */
    AVPacket *pkts[16];
    int np = 0;
    AVPacket *spare = av_packet_alloc();
    while (spare && np < 16 && av_read_frame(in, spare) >= 0) {
        pkts[np++] = spare;
        spare = av_packet_alloc();
    }
    av_packet_free(&spare);

    int idx[2] = { -1, -1 };
    for (unsigned i = 0; i < in->nb_streams && out->pairs < 2; i++)
        if (in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            idx[out->pairs++] = (int)i;
    for (int k = 0; k < out->pairs; k++) {
        AVCodecParameters *cp = in->streams[idx[k]]->codecpar;
        out->rate[k] = cp->sample_rate;
        out->channels[k] = cp->ch_layout.nb_channels > 0 ? cp->ch_layout.nb_channels : 2;
        size_t bytes = 0;
        for (int i = 0; i < np; i++)
            if (pkts[i]->stream_index == idx[k])
                bytes += (size_t)pkts[i]->size;
        out->pcm[k] = malloc(bytes ? bytes : 1);
        if (!out->pcm[k])
            continue;
        size_t off = 0;
        for (int i = 0; i < np; i++)
            if (pkts[i]->stream_index == idx[k]) {
                memcpy((uint8_t *)out->pcm[k] + off, pkts[i]->data, (size_t)pkts[i]->size);
                off += (size_t)pkts[i]->size;
            }
        out->samples[k] = (int64_t)(bytes / (2 * (size_t)out->channels[k]));
    }
    for (int i = 0; i < np; i++)
        av_packet_free(&pkts[i]);
    avformat_close_input(&in);
    av_freep(&avio->buffer);   /* a caller-supplied pb (and its buffer) is ours to free */
    avio_context_free(&avio);
}

/* One DV frame's audio onto the tracks, fitted to the video timeline (frame p->vpts). */
static pin_status_t dv_feed_audio(hwenc_priv_t *p, const dv_audio_t *a)
{
    if (a->pairs > p->ntracks && !p->warned_late_pair) {
        pin_logf(PIN_LOG_WARN, "sink_hwenc: DV audio pair %d appeared after the file's header; "
                               "it is left out of this file\n", p->ntracks + 1);
        p->warned_late_pair = 1;
    }
    for (int k = 0; k < p->ntracks; k++) {
        track_t *t = &p->trk[k];
        int usable = k < a->pairs && a->pcm[k] && a->rate[k] == t->rate && a->channels[k] == t->channels;
        int64_t avail = usable ? a->samples[k] : 0;
        int64_t target_after = av_rescale(p->vpts + 1, (int64_t)t->rate * p->fps_den, p->fps_num);
        int64_t frame = av_rescale(1, (int64_t)t->rate * p->fps_den, p->fps_num);
        int64_t take, pad;
        pin_dv_audio_fit(t->apts, avail, target_after, frame, &take, &pad);
        pin_status_t rc = PIN_OK;
        if (take > 0)
            rc = ring_push(t, a->pcm[k], (size_t)take);
        if (rc == PIN_OK && pad > 0) {
            rc = ring_push(t, NULL, (size_t)pad);
            pthread_mutex_lock(&p->st_lock);
            p->st.audio_padded_samples += (uint64_t)pad;
            pthread_mutex_unlock(&p->st_lock);
        }
        t->apts += take + pad;
        if (rc != PIN_OK)
            return rc;
    }
    return PIN_OK;
}

/* A DV frame whose streams are known: audio onto the tracks, the frame (taken over) to the
 * encoder. */
static pin_status_t dv_process(hwenc_priv_t *p, uint8_t *dif, size_t len)
{
    dv_audio_t a;
    dv_demux_audio(dif, len, &a);
    pin_status_t rc = dv_feed_audio(p, &a);
    dv_audio_free(&a);
    if (rc != PIN_OK) {
        free(dif);
        return rc;
    }
    item_t it = { .frame = NULL, .dif = dif, .dif_len = len, .pts = p->vpts++ };
    return queue_push(p, &it);
}

/* Declares the audio tracks (from a, or none), writes the header if the video is ready, and
 * sends the held frames through. */
static pin_status_t dv_declare(hwenc_priv_t *p, const dv_audio_t *a)
{
    pin_status_t rc = PIN_OK;
    for (int k = 0; a && k < a->pairs && rc == PIN_OK; k++) {
        rc = track_start(p, k, a->rate[k], a->channels[k]);
        if (rc == PIN_OK)
            pin_logf(PIN_LOG_INFO, "sink_hwenc: DV audio pair %d: %d Hz, %d channels\n", k + 1,
                     a->rate[k], a->channels[k]);
    }
    pthread_mutex_lock(&p->mux_lock);
    p->declared = 1;
    if (rc == PIN_OK)
        try_header_locked(p);
    else
        p->header_failed = 1;
    pthread_mutex_unlock(&p->mux_lock);

    for (int i = 0; i < p->nhold; i++) {
        if (rc == PIN_OK)
            rc = dv_process(p, p->hold[i], p->hold_len[i]);
        else
            free(p->hold[i]);
        p->hold[i] = NULL;
    }
    p->nhold = 0;
    return rc;
}

/* ---- the sink ----------------------------------------------------------------------- */

static pin_status_t hwenc_open(pin_sink_t *s, const char *path, const pin_sink_params_t *params)
{
    hwenc_priv_t *p = s->priv;
    if (params->kind != (p->dv ? PIN_KIND_DV : PIN_KIND_ANALOG))
        return PIN_ERR_ARG;
    if (params->width <= 0 || params->height <= 0 || (params->width & 3) || (params->height & 3) ||
        params->fps_num <= 0 || params->fps_den <= 0)
        return PIN_ERR_ARG;
    if (!p->dv && (params->audio_channels <= 0 || params->audio_rate <= 0))
        return PIN_ERR_ARG;
    p->params = *params;
    p->deint = params->deinterlace == PIN_DEINTERLACE_DOUBLE_RATE;
    p->fps_num = params->fps_num;
    p->fps_den = params->fps_den;
    /* as sink_ffv1.c / sink_rewrap.c: the SAR from the aspect and the geometry, PAL from the
     * frame rate (analog) or the line count (DV) */
    p->is_625 = p->dv ? params->height == 576 : params->fps_num == 25 * params->fps_den;
    /* Test seam, like PIN_REPLAY_UNPLUG_AFTER: an encoder that fails during the capture. */
    const char *fail = getenv("PIN_HWENC_FAIL_AFTER");
    p->fail_after = fail && fail[0] ? strtoll(fail, NULL, 10) : 0;
    pin_sink_sar_for(params->kind, p->is_625, params->aspect, params->width, params->height,
                     &p->sar_num, &p->sar_den);

    int rc = avformat_alloc_output_context2(&p->fmt, NULL, "matroska", path);
    if (rc < 0 || !p->fmt) {
        pin_logf(PIN_LOG_ERROR, "sink_hwenc: avformat_alloc_output_context2 failed (%d)\n", rc);
        return PIN_ERR_CODEC;
    }

    /* the encoder opens on its own thread (see the top of the file); wait for the verdict */
    if (pthread_create(&p->enc_thread, NULL, encoder_thread_fn, p) != 0)
        return PIN_ERR_INTERNAL;
    p->enc_started = 1;
    pthread_mutex_lock(&p->qlock);
    while (p->enc_state == 0)
        pthread_cond_wait(&p->enc_cond, &p->qlock);
    int enc_ok = p->enc_state > 0;
    pthread_mutex_unlock(&p->qlock);
    if (!enc_ok)
        return PIN_ERR_CODEC;

    /* The encoder thread now waits for pictures and leaves venc alone, so its settings can
     * be read here. */
    p->vst = avformat_new_stream(p->fmt, NULL);
    if (!p->vst)
        return PIN_ERR_NOMEM;
    avcodec_parameters_from_context(p->vst->codecpar, p->venc);
    p->vst->codecpar->field_order = AV_FIELD_PROGRESSIVE;
    p->vst->time_base = p->venc->time_base;
    p->vst->avg_frame_rate = p->venc->framerate;
    p->vst->sample_aspect_ratio = (AVRational){ p->sar_num, p->sar_den };
    int needs_ps = p->venc->codec_id == AV_CODEC_ID_H264 || p->venc->codec_id == AV_CODEC_ID_HEVC;
    p->vparams_ready = !needs_ps || p->venc->extradata_size > 0;

    if (params->title[0])
        av_dict_set(&p->fmt->metadata, "title", params->title, 0);

    if (!p->dv) {
        pin_status_t st = track_start(p, 0, params->audio_rate, params->audio_channels);
        if (st != PIN_OK)
            return st;
    }

    uint64_t budget = pin_sink_fault_budget();
    if (budget != UINT64_MAX) {
        rc = pin_sink_fault_avio_open(&p->fmt->pb, path, budget);
        p->fault_io = rc >= 0;
    } else {
        rc = avio_open(&p->fmt->pb, path, AVIO_FLAG_WRITE);
    }
    if (rc < 0) {
        pin_logf(PIN_LOG_ERROR, "sink_hwenc: avio_open(%s) failed (%d)\n", path, rc);
        return PIN_ERR_IO;
    }
    if (!p->dv) {
        pthread_mutex_lock(&p->mux_lock);
        p->declared = 1;
        try_header_locked(p);
        pthread_mutex_unlock(&p->mux_lock);
        if (p->header_failed)
            return PIN_ERR_CODEC;
    }
    pin_logf(PIN_LOG_INFO, "sink_hwenc: %s %dx%d, %s, quality %d\n",
             pin_hwenc_encoder_name(p->codec), params->width, params->height,
             p->deint ? "deinterlaced to double rate" : "fields kept", params->video_quality);
    return PIN_OK;
}

static pin_status_t hwenc_write_video(pin_sink_t *s, const uint8_t *yuyv, size_t len)
{
    hwenc_priv_t *p = s->priv;
    if (!p->enc_started || p->dv)
        return PIN_ERR_STATE;
    pin_status_t err = get_error(p);
    if (err != PIN_OK)
        return err;
    size_t expect = (size_t)p->params.width * p->params.height * 2;
    if (len < expect) {
        pin_logf(PIN_LOG_WARN, "sink_hwenc: short video frame (%zu of %zu bytes), skipping\n", len, expect);
        count_damaged(p);
        return PIN_OK;
    }
    AVFrame *f = av_frame_alloc();
    if (!f)
        return PIN_ERR_NOMEM;
    f->format = AV_PIX_FMT_YUV422P;
    f->width = p->params.width;
    f->height = p->params.height;
    if (av_frame_get_buffer(f, 0) < 0) {
        av_frame_free(&f);
        return PIN_ERR_NOMEM;
    }
    pin_hwenc_yuyv_to_422p(yuyv, p->params.width, p->params.height, f);
    item_t it = { .frame = f, .dif = NULL, .pts = p->vpts++ };
    return queue_push(p, &it);
}

static pin_status_t hwenc_write_audio(pin_sink_t *s, const int16_t *pcm, size_t frames)
{
    hwenc_priv_t *p = s->priv;
    if (p->dv || p->ntracks < 1)
        return PIN_ERR_STATE;
    pin_status_t err = get_error(p);
    if (err != PIN_OK)
        return err;
    return ring_push(&p->trk[0], pcm, frames);
}

static pin_status_t hwenc_write_unit(pin_sink_t *s, const uint8_t *data, size_t len)
{
    hwenc_priv_t *p = s->priv;
    if (!p->enc_started || !p->dv)
        return PIN_ERR_ARG;
    pin_status_t err = get_error(p);
    if (err != PIN_OK)
        return err;
    size_t expect = (size_t)(p->params.height == 576 ? DV_SEQ_COUNT_PAL : DV_SEQ_COUNT_NTSC) * DV_SEQ_SIZE;
    if (len != expect) {
        /* not 10 / 12 whole sequences (see pin_sink.h), or the other TV standard */
        count_damaged(p);
        pin_logf(PIN_LOG_WARN, "sink_hwenc: dropping a %zu-byte DV unit\n", len);
        return PIN_OK;
    }
    uint8_t *copy = malloc(len);
    if (!copy)
        return PIN_ERR_NOMEM;
    memcpy(copy, data, len);
    if (p->declared)
        return dv_process(p, copy, len);

    /* Streams not known yet: hold the frame until one has audio (or DV_HOLD_MAX did not). */
    p->hold[p->nhold] = copy;
    p->hold_len[p->nhold] = len;
    p->nhold++;
    dv_audio_t a;
    dv_demux_audio(copy, len, &a);
    pin_status_t rc = PIN_OK;
    if (a.pairs > 0 || p->nhold == DV_HOLD_MAX) {
        if (a.pairs == 0)
            pin_logf(PIN_LOG_WARN, "sink_hwenc: no audio in the first %d DV frames; the file has none\n",
                     DV_HOLD_MAX);
        rc = dv_declare(p, &a);
    }
    dv_audio_free(&a);
    return rc;
}

static pin_status_t hwenc_close(pin_sink_t *s)
{
    hwenc_priv_t *p = s->priv;
    pin_status_t rc = PIN_OK;

    /* a short DV capture whose frames all had no audio: they still go into the file */
    if (p->dv && p->enc_started && p->enc_state > 0 && !p->declared && p->fmt && p->fmt->pb)
        dv_declare(p, NULL);
    for (int i = 0; i < p->nhold; i++)
        free(p->hold[i]);
    p->nhold = 0;

    if (p->enc_started) {
        pthread_mutex_lock(&p->qlock);
        p->closing = 1;
        pthread_cond_signal(&p->qwake);
        pthread_mutex_unlock(&p->qlock);
        pthread_join(p->enc_thread, NULL);
    }
    for (int i = 0; i < 2; i++) {
        track_t *t = &p->trk[i];
        if (!t->started)
            continue;
        pthread_mutex_lock(&t->lock);
        t->closing = 1;
        pthread_cond_signal(&t->wake);
        pthread_mutex_unlock(&t->lock);
        pthread_join(t->thread, NULL);
    }
    if (p->fmt) {
        pthread_mutex_lock(&p->mux_lock);
        if (p->fmt->pb && p->enc_state > 0) {
            /* no picture ever came out (or its parameter sets): write what there is */
            p->declared = 1;
            p->vparams_ready = 1;
            try_header_locked(p);
            if (p->header_written && av_write_trailer(p->fmt) < 0)
                rc = PIN_ERR_IO;
        }
        for (int i = 0; i < p->npending; i++)
            av_packet_free(&p->pending[i].pkt);
        pthread_mutex_unlock(&p->mux_lock);
        if (p->fault_io) {
            if (pin_sink_fault_avio_close(&p->fmt->pb) < 0)
                rc = PIN_ERR_IO;
        } else if (p->fmt->pb && avio_closep(&p->fmt->pb) < 0) {
            rc = PIN_ERR_IO;
        }
        avformat_free_context(p->fmt);
    }
    free(p->pending);
    for (int i = 0; i < 2; i++) {
        avcodec_free_context(&p->trk[i].enc);
        free(p->trk[i].ring);
        pthread_mutex_destroy(&p->trk[i].lock);
        pthread_cond_destroy(&p->trk[i].wake);
        pthread_cond_destroy(&p->trk[i].space);
    }
    while (p->qcount > 0) {     /* only after a failed start */
        item_free(&p->queue[p->qhead]);
        p->qhead = (p->qhead + 1) % QUEUE_DEPTH;
        p->qcount--;
    }
    if (p->st.last_error != PIN_OK)
        rc = p->st.last_error;

    pthread_mutex_destroy(&p->mux_lock);
    pthread_mutex_destroy(&p->qlock);
    pthread_cond_destroy(&p->qwake);
    pthread_cond_destroy(&p->qspace);
    pthread_cond_destroy(&p->enc_cond);
    pthread_mutex_destroy(&p->st_lock);
    free(p);
    free(s);
    return rc;
}

static void hwenc_get_status(pin_sink_t *s, pin_sink_status_t *out)
{
    hwenc_priv_t *p = s->priv;
    pthread_mutex_lock(&p->st_lock);
    *out = p->st;
    pthread_mutex_unlock(&p->st_lock);
}

pin_sink_t *sink_hwenc_create(pin_format_t format)
{
    if (format != PIN_FMT_ANALOG_H264_MKV && format != PIN_FMT_ANALOG_HEVC_MKV &&
        format != PIN_FMT_DV_H264_MKV && format != PIN_FMT_DV_HEVC_MKV)
        return NULL;
    pin_sink_t *s = calloc(1, sizeof(*s));
    hwenc_priv_t *p = calloc(1, sizeof(*p));
    if (!s || !p) {
        free(s);
        free(p);
        return NULL;
    }
    p->format = format;
    p->codec = is_hevc_format(format) ? PIN_HWENC_HEVC : PIN_HWENC_H264;
    p->dv = is_dv_format(format);
    /* initialised here, so close() after a failed open() never meets an uninitialised one */
    pthread_mutex_init(&p->mux_lock, NULL);
    pthread_mutex_init(&p->qlock, NULL);
    pthread_cond_init(&p->qwake, NULL);
    pthread_cond_init(&p->qspace, NULL);
    pthread_cond_init(&p->enc_cond, NULL);
    pthread_mutex_init(&p->st_lock, NULL);
    for (int i = 0; i < 2; i++) {
        pthread_mutex_init(&p->trk[i].lock, NULL);
        pthread_cond_init(&p->trk[i].wake, NULL);
        pthread_cond_init(&p->trk[i].space, NULL);
    }
    s->priv = p;
    s->open = hwenc_open;
    if (p->dv) {
        s->write_unit = hwenc_write_unit;
    } else {
        s->write_video = hwenc_write_video;
        s->write_audio = hwenc_write_audio;
    }
    s->close = hwenc_close;
    s->get_status = hwenc_get_status;
    s->supports_title = 1;
    return s;
}
