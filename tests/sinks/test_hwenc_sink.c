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
 * ctest: sink_hwenc (PIN_FMT_ANALOG_ / DV_ H264_MKV, HEVC_MKV) end to end, files read back
 * with libavformat: frame count (doubled when deinterlacing), frame rate, duration, audio
 * samples against the video's length, SAR, progressive flag, title, and the FFV1 compression
 * levels of sink_ffv1.
 *
 * Linux has no H.264 encoder, so where the encoder probe finds none this sets
 * PIN_HWENC_ENCODER=ffv1 (pin_hwenc.h: tests only) and the whole path runs with FFV1 standing
 * in: everything but the codec is the same, and since FFV1 is lossless the converted pictures
 * are checked pixel for pixel (chroma subsampled per field when the fields are kept). Where a
 * real encoder answers (macOS, Windows, Android) it is used, and the pixel check is skipped.
 *
 * Arguments: DV recordings (made at test time, tests/data/README.md), raw .dv files or EP
 * 0x88 dumps (.bin), each written keeping the fields and deinterlaced. One missing (no ffmpeg
 * to make it): skipped, or failed when CI is set.
 */

#include "test_util.h"
#include "../../src/sinks/pin_sink.h"
#include "../../src/sinks/sinks_internal.h"
#include "../../src/sinks/pin_hwenc.h"
#include "../../src/core/dv_reassembler.h"
#include "../../src/core/pin_log.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
static void set_env(const char *n, const char *v) { _putenv_s(n, v ? v : ""); }
#else
static void set_env(const char *n, const char *v)
{
    if (v)
        setenv(n, v, 1);
    else
        unsetenv(n);
}
#endif

static int g_standin;   /* FFV1 stands in for the encoder: lossless, pixels can be checked */

/* ---- reading a file back ----------------------------------------------------------- */

typedef struct {
    int nvideo, naudio;
    enum AVCodecID vcodec;
    int width, height;
    AVRational sar;
    enum AVFieldOrder field_order;
    int64_t vpackets;
    double vfirst, vlast;       /* seconds */
    int64_t asamples[2];
    int arate[2];
    char title[256];
    AVFrame *first;             /* the first picture, decoded (FFV1 only) */
} readback_t;

static void readback(const char *path, readback_t *r)
{
    memset(r, 0, sizeof(*r));
    AVFormatContext *in = NULL;
    CHECK(avformat_open_input(&in, path, NULL, NULL) == 0);
    CHECK(avformat_find_stream_info(in, NULL) >= 0);
    AVDictionaryEntry *t = av_dict_get(in->metadata, "title", NULL, 0);
    if (t)
        snprintf(r->title, sizeof(r->title), "%s", t->value);

    int vidx = -1, aidx[2] = { -1, -1 };
    AVCodecContext *adec[2] = { NULL, NULL }, *vdec = NULL;
    for (unsigned i = 0; i < in->nb_streams; i++) {
        AVCodecParameters *cp = in->streams[i]->codecpar;
        if (cp->codec_type == AVMEDIA_TYPE_VIDEO) {
            r->nvideo++;
            vidx = (int)i;
            r->vcodec = cp->codec_id;
            r->width = cp->width;
            r->height = cp->height;
            r->sar = in->streams[i]->sample_aspect_ratio.num ? in->streams[i]->sample_aspect_ratio
                                                             : cp->sample_aspect_ratio;
            r->field_order = cp->field_order;
            if (cp->codec_id == AV_CODEC_ID_FFV1) {
                const AVCodec *c = avcodec_find_decoder(cp->codec_id);
                vdec = avcodec_alloc_context3(c);
                avcodec_parameters_to_context(vdec, cp);
                CHECK(avcodec_open2(vdec, c, NULL) == 0);
            }
        } else if (cp->codec_type == AVMEDIA_TYPE_AUDIO && r->naudio < 2) {
            CHECK(cp->codec_id == AV_CODEC_ID_FLAC);
            int k = r->naudio++;
            aidx[k] = (int)i;
            r->arate[k] = cp->sample_rate;
            const AVCodec *c = avcodec_find_decoder(cp->codec_id);
            adec[k] = avcodec_alloc_context3(c);
            avcodec_parameters_to_context(adec[k], cp);
            CHECK(avcodec_open2(adec[k], c, NULL) == 0);
        }
    }
    CHECK(vidx >= 0);
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    int64_t first_pts = AV_NOPTS_VALUE, last_pts = AV_NOPTS_VALUE;
    while (av_read_frame(in, pkt) >= 0) {
        if (pkt->stream_index == vidx) {
            r->vpackets++;
            if (first_pts == AV_NOPTS_VALUE || pkt->pts < first_pts)
                first_pts = pkt->pts;
            if (last_pts == AV_NOPTS_VALUE || pkt->pts > last_pts)
                last_pts = pkt->pts;
            if (vdec && !r->first) {
                CHECK(avcodec_send_packet(vdec, pkt) == 0);
                if (avcodec_receive_frame(vdec, fr) == 0) {
                    r->first = av_frame_clone(fr);
                    av_frame_unref(fr);
                }
            }
        } else {
            for (int k = 0; k < 2; k++) {
                if (pkt->stream_index != aidx[k])
                    continue;
                CHECK(avcodec_send_packet(adec[k], pkt) == 0);
                while (avcodec_receive_frame(adec[k], fr) == 0) {
                    r->asamples[k] += fr->nb_samples;
                    av_frame_unref(fr);
                }
            }
        }
        av_packet_unref(pkt);
    }
    for (int k = 0; k < 2; k++) {
        if (!adec[k])
            continue;
        avcodec_send_packet(adec[k], NULL);
        while (avcodec_receive_frame(adec[k], fr) == 0) {
            r->asamples[k] += fr->nb_samples;
            av_frame_unref(fr);
        }
        avcodec_free_context(&adec[k]);
    }
    AVRational tb = in->streams[vidx]->time_base;
    r->vfirst = first_pts * av_q2d(tb);
    r->vlast = last_pts * av_q2d(tb);
    av_frame_free(&fr);
    av_packet_free(&pkt);
    avcodec_free_context(&vdec);
    avformat_close_input(&in);
}

/* The common checks: count, spacing (1 / fps_out), audio length against the video's. */
static void check_file(const readback_t *r, int64_t frames, int mult, double fps, pin_format_t fmt)
{
    CHECK(r->nvideo == 1);
    if (g_standin)
        CHECK(r->vcodec == AV_CODEC_ID_FFV1);
    else
        CHECK(r->vcodec == (fmt == PIN_FMT_ANALOG_HEVC_MKV || fmt == PIN_FMT_DV_HEVC_MKV ? AV_CODEC_ID_HEVC
                                                                                        : AV_CODEC_ID_H264));
    CHECK_EQ_I(r->vpackets, frames * mult);
    double step = 1.0 / (fps * mult);
    double spacing = r->vpackets > 1 ? (r->vlast - r->vfirst) / (double)(r->vpackets - 1) : step;
    if (fabs(spacing - step) > 0.002) {
        fprintf(stderr, "frame spacing %.5f s, expected %.5f s\n", spacing, step);
        CHECK(0);
    }
    CHECK(fabs(r->vfirst) < 0.002);   /* starts at 0 */
    CHECK(r->field_order == AV_FIELD_PROGRESSIVE);
}

/* ---- analog ------------------------------------------------------------------------- */

static int chroma_u(int y, int j) { return (y & 1 ? 200 : 50) + (j & 7); }
static int chroma_v(int y, int j) { return 100 + (y % 5) + (j & 3); }

static void fill_frame(uint8_t *yuyv, int w, int h, int i)
{
    for (int y = 0; y < h; y++) {
        uint8_t *row = yuyv + (size_t)y * w * 2;
        for (int j = 0; j < w / 2; j++) {
            row[4 * j + 0] = (uint8_t)((2 * j + y + 4 * i) & 0xff);
            row[4 * j + 1] = (uint8_t)chroma_u(y, j);
            row[4 * j + 2] = (uint8_t)((2 * j + 1 + y + 4 * i) & 0xff);
            row[4 * j + 3] = (uint8_t)chroma_v(y, j);
        }
    }
}

/* frame 0 as the stand-in encoded it: luma as it was, chroma subsampled within each field */
static void check_pixels(const AVFrame *f, int w, int h)
{
    CHECK(f && f->format == AV_PIX_FMT_YUV420P && f->width == w && f->height == h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            CHECK(f->data[0][y * f->linesize[0] + x] == (uint8_t)((x + y) & 0xff));
    for (int c = 0; c < h / 2; c++) {
        int r0 = 4 * (c >> 1) + (c & 1), r1 = r0 + 2;
        for (int j = 0; j < w / 2; j++) {
            int u = (chroma_u(r0, j) + chroma_u(r1, j) + 1) >> 1;
            int v = (chroma_v(r0, j) + chroma_v(r1, j) + 1) >> 1;
            if (f->data[1][c * f->linesize[1] + j] != u || f->data[2][c * f->linesize[2] + j] != v) {
                fprintf(stderr, "chroma row %d col %d: %d/%d, expected %d/%d\n", c, j,
                        f->data[1][c * f->linesize[1] + j], f->data[2][c * f->linesize[2] + j], u, v);
                CHECK(0);
            }
        }
    }
}

static void analog_params(pin_sink_params_t *p, int pal, int deint)
{
    memset(p, 0, sizeof(*p));
    p->kind = PIN_KIND_ANALOG;
    p->width = 720;
    p->height = pal ? 576 : 480;
    p->fps_num = pal ? 25 : 30000;
    p->fps_den = pal ? 1 : 1001;
    p->interlaced = 1;
    p->top_field_first = 1;
    p->aspect = PIN_ASPECT_4_3;
    p->colour_matrix = PIN_MATRIX_BT601;
    p->audio_rate = 48000;
    p->audio_channels = 2;
    p->deinterlace = deint ? PIN_DEINTERLACE_DOUBLE_RATE : PIN_DEINTERLACE_KEEP_FIELDS;
    p->video_quality = 18;
    snprintf(p->title, sizeof(p->title), "analog %s %s", pal ? "PAL" : "NTSC", deint ? "50p" : "fields");
}

static void analog(pin_format_t fmt, int pal, int deint, const char *path)
{
    pin_sink_params_t prm;
    analog_params(&prm, pal, deint);
    const int frames = pal ? 50 : 60;
    pin_sink_t *s = pin_sink_create(fmt);
    CHECK(s);
    CHECK(s->open(s, path, &prm) == PIN_OK);
    size_t vlen = (size_t)prm.width * prm.height * 2;
    uint8_t *yuyv = malloc(vlen);
    int16_t pcm[2 * 2000];
    int64_t fed = 0;
    for (int i = 0; i < frames; i++) {
        fill_frame(yuyv, prm.width, prm.height, i);
        CHECK(s->write_video(s, yuyv, vlen) == PIN_OK);
        /* the exact share of 48 kHz per frame, 1601 / 1602 at 29.97 */
        int64_t upto = av_rescale(i + 1, 48000LL * prm.fps_den, prm.fps_num);
        int n = (int)(upto - fed);
        for (int k = 0; k < n; k++)
            pcm[2 * k] = pcm[2 * k + 1] = (int16_t)(8000.0 * sin((double)(fed + k) * 0.05));
        CHECK(s->write_audio(s, pcm, (size_t)n) == PIN_OK);
        fed += n;
    }
    free(yuyv);
    pin_sink_status_t st;
    s->get_status(s, &st);
    CHECK(s->close(s) == PIN_OK);

    readback_t r;
    readback(path, &r);
    double fps = (double)prm.fps_num / prm.fps_den;
    check_file(&r, frames, deint ? 2 : 1, fps, fmt);
    CHECK(r.width == prm.width && r.height == prm.height);
    CHECK(r.naudio == 1 && r.arate[0] == 48000);
    CHECK_EQ_I(r.asamples[0], fed);                                 /* FLAC is lossless */
    int sn, sd;
    pin_sink_sar_for(PIN_KIND_ANALOG, pal, PIN_ASPECT_4_3, 720, pal ? 576 : 480, &sn, &sd);
    CHECK(av_cmp_q(r.sar, (AVRational){ sn, sd }) == 0);
    CHECK(strcmp(r.title, prm.title) == 0);
    if (g_standin && !deint)
        check_pixels(r.first, prm.width, prm.height);
    if (r.first) {
        AVFrame *f = r.first;
        av_frame_free(&f);
    }
    printf("  %s: %lld pictures at %.3f fps, %lld audio samples, %s\n", path, (long long)r.vpackets,
           fps * (deint ? 2 : 1), (long long)r.asamples[0], st.encoder_behind ? "encoder fell behind" : "kept up");
}

/* ---- the encoder is missing --------------------------------------------------------- */

static void unavailable(const char *keep)
{
    set_env("PIN_HWENC_ENCODER", "no-such-encoder");
    CHECK(pin_hwenc_probe() == 0);
    pin_sink_params_t prm;
    analog_params(&prm, 1, 1);
    remove("hwenc_missing.mkv");
    pin_sink_t *s = pin_sink_create(PIN_FMT_ANALOG_H264_MKV);
    CHECK(s);
    CHECK(s->open(s, "hwenc_missing.mkv", &prm) == PIN_ERR_CODEC);
    s->close(s);                    /* after a failed open, as the session does */
    FILE *f = fopen("hwenc_missing.mkv", "rb");
    CHECK(f == NULL);               /* nothing created */
    s = pin_sink_create(PIN_FMT_DV_HEVC_MKV);
    prm.kind = PIN_KIND_DV;
    CHECK(s->open(s, "hwenc_missing.mkv", &prm) == PIN_ERR_CODEC);
    s->close(s);
    set_env("PIN_HWENC_ENCODER", keep);
    pin_hwenc_probe();
    printf("  a missing encoder: open() fails with PIN_ERR_CODEC\n");
}

/* ---- the encoder fails mid-capture --------------------------------------------------- */

static void fails_midway(void)
{
    set_env("PIN_HWENC_FAIL_AFTER", "5");
    pin_sink_params_t prm;
    analog_params(&prm, 1, 0);
    pin_sink_t *s = pin_sink_create(PIN_FMT_ANALOG_H264_MKV);
    CHECK(s->open(s, "hwenc_fails.mkv", &prm) == PIN_OK);
    set_env("PIN_HWENC_FAIL_AFTER", NULL);
    size_t vlen = (size_t)prm.width * prm.height * 2;
    uint8_t *yuyv = calloc(1, vlen);
    pin_status_t st = PIN_OK;
    int i;
    for (i = 0; i < 200 && st == PIN_OK; i++)
        st = s->write_video(s, yuyv, vlen);     /* the writer learns of it on a later call */
    free(yuyv);
    CHECK(st == PIN_ERR_CODEC);
    pin_sink_status_t ss;
    s->get_status(s, &ss);
    CHECK(ss.last_error == PIN_ERR_CODEC);
    CHECK(s->close(s) == PIN_ERR_CODEC);
    readback_t r;
    readback("hwenc_fails.mkv", &r);       /* what was written before is a valid file */
    CHECK_EQ_I(r.vpackets, 5);
    if (r.first) {
        AVFrame *f = r.first;
        av_frame_free(&f);
    }
    printf("  an encoder failing after 5 pictures: write_video() fails after %d frames, the file "
           "keeps the 5\n", i);
}

/* ---- FFV1 compression levels -------------------------------------------------------- */

static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static void ffv1_levels(void)
{
    long size[PIN_FFV1_LEVEL_COUNT];
    for (int level = 0; level < PIN_FFV1_LEVEL_COUNT; level++) {
        char path[64];
        snprintf(path, sizeof(path), "ffv1_level%d.mkv", level);
        pin_sink_params_t prm;
        analog_params(&prm, 1, 0);
        prm.ffv1_compression = level;
        pin_sink_t *s = pin_sink_create(PIN_FMT_ANALOG_FFV1_MKV);
        CHECK(s->open(s, path, &prm) == PIN_OK);
        size_t vlen = (size_t)prm.width * prm.height * 2;
        uint8_t *yuyv = malloc(vlen);
        unsigned seed = 1;
        int16_t pcm[2 * 1920];
        memset(pcm, 0, sizeof(pcm));
        for (int i = 0; i < 10; i++) {
            fill_frame(yuyv, prm.width, prm.height, i);
            for (size_t k = 0; k < vlen; k += 2) {      /* a little noise on the luma, as tape has */
                seed = seed * 1103515245u + 12345u;
                yuyv[k] = (uint8_t)(yuyv[k] + ((seed >> 16) & 7));
            }
            CHECK(s->write_video(s, yuyv, vlen) == PIN_OK);
            CHECK(s->write_audio(s, pcm, 1920) == PIN_OK);
        }
        CHECK(s->close(s) == PIN_OK);

        /* lossless whatever the level: the last frame decodes to exactly what went in */
        AVFormatContext *in = NULL;
        CHECK(avformat_open_input(&in, path, NULL, NULL) == 0);
        CHECK(avformat_find_stream_info(in, NULL) >= 0);
        int vi = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        CHECK(vi >= 0 && in->streams[vi]->codecpar->codec_id == AV_CODEC_ID_FFV1);
        const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_FFV1);
        AVCodecContext *dec = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(dec, in->streams[vi]->codecpar);
        CHECK(avcodec_open2(dec, c, NULL) == 0);
        AVPacket *pkt = av_packet_alloc();
        AVFrame *fr = av_frame_alloc();
        int decoded = 0;
        while (av_read_frame(in, pkt) >= 0) {
            if (pkt->stream_index == vi) {
                CHECK(avcodec_send_packet(dec, pkt) == 0);
                while (avcodec_receive_frame(dec, fr) == 0) {
                    decoded++;
                    if (decoded == 10) {
                        CHECK(fr->format == AV_PIX_FMT_YUV422P);
                        for (int y = 0; y < prm.height; y++)
                            for (int j = 0; j < prm.width / 2; j++) {
                                const uint8_t *p = yuyv + (size_t)y * prm.width * 2 + 4 * j;
                                CHECK(fr->data[0][y * fr->linesize[0] + 2 * j] == p[0]);
                                CHECK(fr->data[1][y * fr->linesize[1] + j] == p[1]);
                                CHECK(fr->data[0][y * fr->linesize[0] + 2 * j + 1] == p[2]);
                                CHECK(fr->data[2][y * fr->linesize[2] + j] == p[3]);
                            }
                    }
                    av_frame_unref(fr);
                }
            }
            av_packet_unref(pkt);
        }
        CHECK_EQ_I(decoded, 10);
        free(yuyv);
        av_frame_free(&fr);
        av_packet_free(&pkt);
        avcodec_free_context(&dec);
        avformat_close_input(&in);
        size[level] = file_size(path);
    }
    printf("  FFV1 fast %ld, smaller %ld, smallest %ld bytes\n", size[0], size[1], size[2]);
    /* the range coder wins on these pictures; the large context model's gain depends on the
     * material (see sink_ffv1.c), so only its validity is checked */
    CHECK(size[PIN_FFV1_SMALLER] < size[PIN_FFV1_FAST]);
    CHECK(size[PIN_FFV1_SMALLEST] < size[PIN_FFV1_FAST]);
}

/* ---- DV ------------------------------------------------------------------------------ */

typedef struct {
    uint8_t **frames;
    size_t *lens;
    size_t n, cap;
} frames_t;

static void add_frame(frames_t *c, const uint8_t *data, size_t len)
{
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 64;
        c->frames = realloc(c->frames, c->cap * sizeof(*c->frames));
        c->lens = realloc(c->lens, c->cap * sizeof(*c->lens));
    }
    c->frames[c->n] = malloc(len);
    memcpy(c->frames[c->n], data, len);
    c->lens[c->n] = len;
    c->n++;
}

static int on_write(const uint8_t *data, size_t len, void *user)
{
    (void)data; (void)len; (void)user;
    return 0;
}

static void on_unit(dv_format_t fmt, const uint8_t *data, size_t len, void *user)
{
    if (fmt == DV_FORMAT_DV)
        add_frame(user, data, len);
}

static int ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

/* A .dv file is whole DIF frames (the DSF bit of the first header says 12 or 10 sequences);
 * anything else is an EP 0x88 dump for dv_reassembler. */
static void load_dv(const char *path, frames_t *out)
{
    memset(out, 0, sizeof(*out));
    FILE *in = fopen(path, "rb");
    CHECK(in);
    if (ends_with(path, ".dv")) {
        uint8_t *buf = malloc(144000);
        for (;;) {
            if (fread(buf, 1, 80, in) != 80)
                break;
            size_t len = (buf[3] & 0x80) ? 144000 : 120000;
            if (fread(buf + 80, 1, len - 80, in) != len - 80)
                break;
            add_frame(out, buf, len);
        }
        free(buf);
    } else {
        dv_output_t o = { .write = on_write, .on_unit = on_unit, .user = out };
        dv_reassembler_t r;
        CHECK(dv_reassembler_init(&r, &o) == 0);
        uint8_t buf[16384];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
            CHECK(dv_reassembler_feed(&r, buf, n) == 0);
        dv_reassembler_finish(&r);
    }
    fclose(in);
}

static void dv(const char *src, const frames_t *fr, pin_format_t fmt, int deint, const char *path)
{
    int pal = fr->lens[0] == 144000;
    pin_sink_params_t prm;
    memset(&prm, 0, sizeof(prm));
    prm.kind = PIN_KIND_DV;
    prm.width = 720;
    prm.height = pal ? 576 : 480;
    prm.fps_num = pal ? 25 : 30000;
    prm.fps_den = pal ? 1 : 1001;
    prm.interlaced = 1;
    prm.top_field_first = 0;
    prm.aspect = PIN_ASPECT_4_3;
    prm.colour_matrix = PIN_MATRIX_BT601;
    prm.audio_rate = 48000;     /* what the session guesses; the sink takes the stream's */
    prm.audio_channels = 2;
    prm.deinterlace = deint ? PIN_DEINTERLACE_DOUBLE_RATE : PIN_DEINTERLACE_KEEP_FIELDS;
    prm.video_quality = 18;
    snprintf(prm.title, sizeof(prm.title), "DV %s", pal ? "PAL" : "NTSC");

    pin_sink_t *s = pin_sink_create(fmt);
    CHECK(s);
    CHECK(s->open(s, path, &prm) == PIN_OK);
    for (size_t i = 0; i < fr->n; i++)
        CHECK(s->write_unit(s, fr->frames[i], fr->lens[i]) == PIN_OK);
    pin_sink_status_t st;
    s->get_status(s, &st);
    CHECK(s->close(s) == PIN_OK);

    readback_t r;
    readback(path, &r);
    int64_t good = (int64_t)fr->n - (int64_t)st.units_damaged;
    double fps = (double)prm.fps_num / prm.fps_den;
    CHECK(good > 0 && st.units_damaged <= fr->n / 10);
    check_file(&r, good, deint ? 2 : 1, fps, fmt);
    CHECK(r.width == 720 && r.height == prm.height);
    int sn, sd;
    pin_sink_sar_for(PIN_KIND_DV, pal, PIN_ASPECT_4_3, 720, pal ? 576 : 480, &sn, &sd);
    CHECK(av_cmp_q(r.sar, (AVRational){ sn, sd }) == 0);
    CHECK(strcmp(r.title, prm.title) == 0);
    /* every track as long as the video, give or take a frame (padded where a frame had none) */
    CHECK(r.naudio >= 1);
    for (int k = 0; k < r.naudio; k++) {
        double want = (double)fr->n * r.arate[k] / fps;
        double frame = r.arate[k] / fps;
        if (fabs((double)r.asamples[k] - want) > frame) {
            fprintf(stderr, "%s track %d: %lld samples, expected %.0f +- %.0f\n", path, k,
                    (long long)r.asamples[k], want, frame);
            CHECK(0);
        }
    }
    if (r.first) {
        AVFrame *f = r.first;
        av_frame_free(&f);
    }
    printf("  %s (%s): %zu DV frames -> %lld pictures, %d audio track(s) at %d Hz, %lld samples, "
           "%llu padded\n", path, src, fr->n, (long long)r.vpackets, r.naudio, r.arate[0],
           (long long)r.asamples[0], (unsigned long long)st.audio_padded_samples);
}

/* the core's debug lines (every encoder attempt) would drown the output */
static void log_sink(pin_log_level_t level, const char *msg, void *user)
{
    (void)user;
    if (level >= PIN_LOG_WARN)
        fputs(msg, stderr);
}

int main(int argc, char **argv)
{
    pin_log_set_sink(log_sink, NULL);
    int real = pin_hwenc_probe() & (1 << PIN_HWENC_H264);
    if (!real) {
        set_env("PIN_HWENC_ENCODER", "ffv1");
        CHECK(pin_hwenc_probe() == 3);
        g_standin = 1;
    }
    printf("encoder: %s\n", g_standin ? "FFV1 standing in (PIN_HWENC_ENCODER=ffv1)"
                                      : pin_hwenc_encoder_name(PIN_HWENC_H264));

    analog(PIN_FMT_ANALOG_H264_MKV, 1, 0, "hwenc_pal_fields.mkv");
    analog(PIN_FMT_ANALOG_H264_MKV, 1, 1, "hwenc_pal_50p.mkv");
    analog(PIN_FMT_ANALOG_H264_MKV, 0, 0, "hwenc_ntsc_fields.mkv");
    analog(PIN_FMT_ANALOG_H264_MKV, 0, 1, "hwenc_ntsc_60p.mkv");
    if (pin_hwenc_probe() & (1 << PIN_HWENC_HEVC))
        analog(PIN_FMT_ANALOG_HEVC_MKV, 1, 1, "hwenc_pal_50p_hevc.mkv");
    unavailable(g_standin ? "ffv1" : NULL);
    fails_midway();
    ffv1_levels();

    for (int i = 1; i < argc; i++) {
        if (!pin_test_have_fixture(argv[i]))
            continue;
        frames_t fr;
        load_dv(argv[i], &fr);
        CHECK(fr.n > 5);
        char path[64];
        snprintf(path, sizeof(path), "hwenc_dv%d_fields.mkv", i);
        dv(argv[i], &fr, PIN_FMT_DV_H264_MKV, 0, path);
        snprintf(path, sizeof(path), "hwenc_dv%d_double.mkv", i);
        dv(argv[i], &fr, PIN_FMT_DV_H264_MKV, 1, path);
        for (size_t k = 0; k < fr.n; k++)
            free(fr.frames[k]);
        free(fr.frames);
        free(fr.lens);
    }
    printf("test_hwenc_sink: ok\n");
    return 0;
}
