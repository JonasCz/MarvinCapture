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
 * ctest: sink_hdvmux (HDV to MOV / MKV, live, with repair of damaged GOPs) on
 * transport streams damaged the way a tape dropout damages them: runs of TS
 * packets lost (the deck sends nothing), garbled (no sync byte), flagged with
 * the transport error bit, audio packets lost alone, the start and the end
 * of a recording cut mid-GOP, and a jump of the camera clock. The streams
 * are made by ffmpeg at test time (hdv-repair*.ts, tests/data/README.md) and
 * damaged here.
 *
 * Each output is read back with libavformat and decoded with libavcodec and
 * must
 *   - decode without a single decoder error or corrupt frame,
 *   - show every frame of the source where it was (each output frame is
 *     compared with the clean source's frame at the same time; a frame lost
 *     on "tape" repeats a neighbour, so the file keeps the tape's length),
 *   - pass undamaged GOPs through bit for bit (a clean stream comes out
 *     identical, nothing re-encoded),
 *   - have audio that is, block for block, either the source's audio at the
 *     same time or silence: nothing damaged, nothing a frame early or late
 *     (the tone is 997 Hz, see the fixture), and as long as the video.
 * Plus the MOV sample description (hdv3), Matroska's CodecPrivate, and the
 * capture-ending error when the repair cannot keep up (sink_hdvmux_test_set).
 *
 * Arguments: hdv-repair.ts hdv-repair-closed.ts hdv-repair-jump.ts hdv-720p.ts <out dir>
 */

#include "test_util.h"
#include "../../src/sinks/pin_sink.h"
#include "../../src/sinks/sinks_internal.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TSP 188
#define DS 4                      /* the frames are compared at a quarter of their size */
#define VPID 0x810
#define APID 0x814

typedef struct {
    uint8_t *d;
    size_t n;                     /* bytes */
} blob_t;

static blob_t load(const char *path)
{
    blob_t b = { 0 };
    FILE *f = fopen(path, "rb");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b.d = malloc((size_t)n);
    CHECK(b.d);
    CHECK(fread(b.d, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    b.n = (size_t)n;
    CHECK(b.n % TSP == 0);
    return b;
}

static blob_t copy(blob_t a)
{
    blob_t b = { malloc(a.n), a.n };
    CHECK(b.d);
    memcpy(b.d, a.d, a.n);
    return b;
}

static size_t npk(blob_t b)
{
    return b.n / TSP;
}

static int pid_of(const uint8_t *p)
{
    return ((p[1] & 0x1F) << 8) | p[2];
}

/* Removes packets [from, from + n) (of pid only, if pid >= 0). */
static blob_t drop(blob_t a, size_t from, size_t n, int pid)
{
    blob_t b = { malloc(a.n), 0 };
    CHECK(b.d);
    size_t k = 0;
    for (size_t i = 0; i < npk(a); i++) {
        const uint8_t *p = a.d + i * TSP;
        int in = i >= from && k < n && (pid < 0 || pid_of(p) == pid);
        if (in) {
            k++;
            continue;
        }
        memcpy(b.d + b.n, p, TSP);
        b.n += TSP;
    }
    free(a.d);
    return b;
}

/* Overwrites packets [from, from + n) with zeros (no sync byte: a garbled run). */
static void zero(blob_t a, size_t from, size_t n)
{
    for (size_t i = from; i < from + n && i < npk(a); i++)
        memset(a.d + i * TSP, 0, TSP);
}

/* Sets the transport error bit on n video packets from `from` and garbles their payload. */
static void tei(blob_t a, size_t from, size_t n)
{
    size_t k = 0;
    for (size_t i = from; i < npk(a) && k < n; i++) {
        uint8_t *p = a.d + i * TSP;
        if (pid_of(p) != VPID)
            continue;
        p[1] |= 0x80;
        for (int j = 20; j < TSP; j += 7)
            p[j] ^= 0x5A;
        k++;
    }
}

static blob_t cat(blob_t a, blob_t b)
{
    blob_t c = { malloc(a.n + b.n), a.n + b.n };
    CHECK(c.d);
    memcpy(c.d, a.d, a.n);
    memcpy(c.d + a.n, b.d, b.n);
    free(a.d);
    return c;
}

/* First packet index at or after `from` that starts a video PES. */
static size_t next_pes(blob_t a, size_t from)
{
    for (size_t i = from; i < npk(a); i++) {
        const uint8_t *p = a.d + i * TSP;
        if (pid_of(p) == VPID && (p[1] & 0x40))
            return i;
    }
    return npk(a);
}

/* ------------------------------------------------------------------ media -- */

typedef struct {
    int n, cap;
    uint8_t **y;                  /* frames, downscaled luma */
    int w, h;                     /* their size */
    int fw, fh;                   /* the stream's */
    float *a;                     /* audio, mono, on the video's timeline (sample 0 = frame 0) */
    long an;
    int sr;
    double fps;
    int errors;                   /* decoder errors and corrupt frames */
    int pts_bad;                  /* output: frames not one frame after the one before */
    uint32_t tag;
    int extradata;
    int field_order;
    double vstart, astart;        /* first video / audio time (s) */
} media_t;

static int g_count_errors, g_errors;
static void (*g_prev_log)(void *, int, const char *, va_list);

static void log_cb(void *avcl, int level, const char *fmt, va_list vl)
{
    if (g_count_errors && level <= AV_LOG_ERROR) {
        g_errors++;
        va_list c;
        va_copy(c, vl);
        fprintf(stderr, "  decoder: ");
        vfprintf(stderr, fmt, c);
        va_end(c);
    }
    (void)avcl;
}

static void add_frame(media_t *m, const AVFrame *f)
{
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 64;
        m->y = realloc(m->y, (size_t)m->cap * sizeof(*m->y));
        CHECK(m->y);
    }
    m->w = f->width / DS;
    m->h = f->height / DS;
    uint8_t *y = malloc((size_t)m->w * m->h);
    CHECK(y);
    for (int r = 0; r < m->h; r++)
        for (int c = 0; c < m->w; c++)
            y[r * m->w + c] = f->data[0][(size_t)r * DS * f->linesize[0] + c * DS];
    m->y[m->n++] = y;
}

static void add_audio(media_t *m, const AVFrame *f, double t)
{
    long at = lround((t - m->vstart) * f->sample_rate);
    if (at < 0)
        return;
    long end = at + f->nb_samples;
    if (end > m->an) {
        m->a = realloc(m->a, (size_t)end * sizeof(float));
        CHECK(m->a);
        memset(m->a + m->an, 0, (size_t)(end - m->an) * sizeof(float));
        m->an = end;
    }
    int ch = f->ch_layout.nb_channels;
    for (int i = 0; i < f->nb_samples; i++) {
        float s = 0;
        for (int c = 0; c < ch; c++) {
            if (f->format == AV_SAMPLE_FMT_FLTP)
                s += ((const float *)f->extended_data[c])[i];
            else if (f->format == AV_SAMPLE_FMT_S16P)
                s += ((const int16_t *)f->extended_data[c])[i] / 32768.0f;
            else if (f->format == AV_SAMPLE_FMT_S16)
                s += ((const int16_t *)f->data[0])[i * ch + c] / 32768.0f;
            else if (f->format == AV_SAMPLE_FMT_FLT)
                s += ((const float *)f->data[0])[i * ch + c];
        }
        m->a[at + i] = s / ch;
    }
}

/* Decodes a file. output: count decoder errors and check the timestamps run on frame by frame. */
static void decode(const char *path, media_t *m, int output)
{
    memset(m, 0, sizeof(*m));
    AVFormatContext *fc = NULL;
    CHECK(avformat_open_input(&fc, path, NULL, NULL) >= 0);
    CHECK(avformat_find_stream_info(fc, NULL) >= 0);
    int vi = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int ai = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    CHECK(vi >= 0);
    AVStream *vs = fc->streams[vi];
    m->tag = vs->codecpar->codec_tag;
    m->extradata = vs->codecpar->extradata_size;
    m->field_order = vs->codecpar->field_order;
    m->fw = vs->codecpar->width;
    m->fh = vs->codecpar->height;
    m->fps = av_q2d(vs->avg_frame_rate.num ? vs->avg_frame_rate : vs->r_frame_rate);
    AVCodecContext *vd = avcodec_alloc_context3(avcodec_find_decoder(vs->codecpar->codec_id));
    avcodec_parameters_to_context(vd, vs->codecpar);
    vd->thread_count = 1;
    CHECK(avcodec_open2(vd, vd->codec, NULL) >= 0);
    AVCodecContext *ad = NULL;
    if (ai >= 0) {
        ad = avcodec_alloc_context3(avcodec_find_decoder(fc->streams[ai]->codecpar->codec_id));
        avcodec_parameters_to_context(ad, fc->streams[ai]->codecpar);
        CHECK(avcodec_open2(ad, ad->codec, NULL) >= 0);
        m->sr = ad->sample_rate;
    }
    g_errors = 0;
    g_count_errors = output;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *f = av_frame_alloc();
    int64_t last_pts = AV_NOPTS_VALUE;
    m->vstart = -1;
    double tbv = av_q2d(vs->time_base);
    /* audio waits for the first video frame (its time is the timeline's 0) */
    AVFrame **held = NULL;
    double *held_t = NULL;
    int nheld = 0;
    for (int eof = 0; !eof;) {
        int rc = av_read_frame(fc, pkt);
        if (rc < 0) {
            eof = 1;
            avcodec_send_packet(vd, NULL);
            if (ad)
                avcodec_send_packet(ad, NULL);
        } else if (pkt->stream_index == vi) {
            if (avcodec_send_packet(vd, pkt) < 0 && output)
                g_errors++;
        } else if (ad && pkt->stream_index == ai) {
            avcodec_send_packet(ad, pkt);
        }
        if (rc >= 0)
            av_packet_unref(pkt);
        while (avcodec_receive_frame(vd, f) == 0) {
            if (f->decode_error_flags || (f->flags & AV_FRAME_FLAG_CORRUPT))
                m->errors += output;
            int64_t t = f->best_effort_timestamp;
            if (m->vstart < 0)
                m->vstart = t * tbv;
            if (output && last_pts != AV_NOPTS_VALUE &&
                llabs(llround((t - last_pts) * tbv * m->fps) - 1) != 0)
                m->pts_bad++;
            last_pts = t;
            add_frame(m, f);
            av_frame_unref(f);
        }
        while (ad && avcodec_receive_frame(ad, f) == 0) {
            double t = f->best_effort_timestamp * av_q2d(fc->streams[ai]->time_base);
            if (m->astart == 0 && nheld == 0 && m->an == 0)
                m->astart = t;
            if (m->vstart < 0) {
                held = realloc(held, (size_t)(nheld + 1) * sizeof(*held));
                held_t = realloc(held_t, (size_t)(nheld + 1) * sizeof(*held_t));
                held[nheld] = av_frame_clone(f);
                held_t[nheld++] = t;
            } else {
                for (int i = 0; i < nheld; i++) {
                    add_audio(m, held[i], held_t[i]);
                    av_frame_free(&held[i]);
                }
                nheld = 0;
                add_audio(m, f, t);
            }
            av_frame_unref(f);
        }
    }
    m->errors += g_errors;
    g_count_errors = 0;
    for (int i = 0; i < nheld; i++)
        av_frame_free(&held[i]);
    free(held);
    free(held_t);
    av_frame_free(&f);
    av_packet_free(&pkt);
    avcodec_free_context(&vd);
    avcodec_free_context(&ad);
    avformat_close_input(&fc);
}

static void media_free(media_t *m)
{
    for (int i = 0; i < m->n; i++)
        free(m->y[i]);
    free(m->y);
    free(m->a);
    memset(m, 0, sizeof(*m));
}

/* Appends b's frames and audio after a's (the clean "tape" a jump joins). */
static void media_join(media_t *a, media_t *b)
{
    long at = lround(a->n / a->fps * a->sr);
    for (int i = 0; i < b->n; i++) {
        if (a->n == a->cap) {
            a->cap *= 2;
            a->y = realloc(a->y, (size_t)a->cap * sizeof(*a->y));
        }
        a->y[a->n++] = b->y[i];
    }
    b->n = 0;
    a->a = realloc(a->a, (size_t)(at + b->an) * sizeof(float));
    if (at > a->an)
        memset(a->a + a->an, 0, (size_t)(at - a->an) * sizeof(float));
    memcpy(a->a + at, b->a, (size_t)b->an * sizeof(float));
    a->an = at + b->an;
}

static double psnr(const media_t *m, const uint8_t *a, const uint8_t *b)
{
    double se = 0;
    long n = (long)m->w * m->h;
    for (long i = 0; i < n; i++) {
        double d = (double)a[i] - b[i];
        se += d * d;
    }
    if (se == 0)
        return 99;
    return 10 * log10(255.0 * 255.0 / (se / n));
}

/* --------------------------------------------------------------- scenarios -- */

typedef struct {
    int exact;               /* every frame identical (nothing re-encoded) */
    int full_length;         /* the output ends where the source does, no frame missing */
    int max_off;             /* the output may start this many frames into the source */
    int max_bad;             /* frames below 25 dB against the source at their time */
    int reencoded;           /* -1 any, 0 none, 1 some */
    int max_abad;            /* audio blocks neither the source's nor silence */
    double min_amatch;       /* share of audio blocks that are the source's */
    int min_silent;          /* audio blocks of silence at least */
} expect_t;

static pin_sink_params_t params_for(int w, int h, int fps_num, int fps_den, int interlaced)
{
    pin_sink_params_t p;
    memset(&p, 0, sizeof(p));
    p.kind = PIN_KIND_HDV;
    p.width = w;
    p.height = h;
    p.fps_num = fps_num;
    p.fps_den = fps_den;
    p.interlaced = interlaced;
    p.top_field_first = interlaced;
    p.aspect = PIN_ASPECT_16_9;
    p.colour_matrix = PIN_MATRIX_BT709;
    snprintf(p.title, sizeof(p.title), "HDV repair test");
    return p;
}

/* Feeds ts to a sink in the units a capture would (here: runs of 1 to 400 packets). */
static uint64_t feed(blob_t ts, pin_format_t fmt, const char *out, const pin_sink_params_t *p)
{
    pin_sink_t *s = pin_sink_create(fmt);
    CHECK(s);
    CHECK(s->open(s, out, p) == PIN_OK);
    size_t i = 0, k = 0;
    unsigned seed = 12345;
    while (i < npk(ts)) {
        seed = seed * 1103515245u + 12345u;
        size_t n = 1 + (seed >> 16) % 400;
        if (i + n > npk(ts))
            n = npk(ts) - i;
        pin_status_t rc = s->write_unit(s, ts.d + i * TSP, n * TSP);
        if (rc != PIN_OK) {
            fprintf(stderr, "write_unit failed (%d) at packet %zu\n", rc, i);
            exit(1);
        }
        i += n;
        k++;
    }
    pin_sink_status_t st;
    s->get_status(s, &st);
    CHECK_EQ_I(s->close(s), PIN_OK);
    return st.units_damaged;
}

static void scenario(const char *name, blob_t ts, pin_format_t fmt, const pin_sink_params_t *p,
                     const media_t *ref, const char *dir, expect_t e)
{
    char out[1024];
    snprintf(out, sizeof(out), "%s/repair-%s.%s", dir, name, fmt == PIN_FMT_HDV_MOV ? "mov" : "mkv");
    printf("== %s\n", name);
    uint64_t reenc = feed(ts, fmt, out, p);
    media_t m;
    decode(out, &m, 1);

    /* where the output starts in the source */
    int off = 0;
    double best = -1;
    for (int o = 0; o <= e.max_off && o < ref->n; o++) {
        double q = 0;
        for (int k = 0; k < 3 && k < m.n && k + o < ref->n; k++)
            q += psnr(ref, m.y[k], ref->y[k + o]);
        if (q > best) {
            best = q;
            off = o;
        }
    }
    int bad = 0, nonexact = 0, beyond = 0;
    for (int k = 0; k < m.n; k++) {
        if (k + off >= ref->n) {
            beyond++;
            continue;
        }
        double q = psnr(ref, m.y[k], ref->y[k + off]);
        if (q < 25)
            bad++;
        if (q < 99)
            nonexact++;
    }
    /* audio, block by block against the source at the same time */
    int blocks = 0, match = 0, silent = 0, abad = 0;
    long shift = lround(off / ref->fps * ref->sr);
    /* (the first and the last block: the source's audio does not quite start and end with its
     * video, so they are compared with a part of a block) */
    for (long s = 1152; s + 2 * 1152 <= m.an; s += 1152) {
        double eo = 0, ed = 0, er = 0;
        for (long i = s; i < s + 1152; i++) {
            double o = m.a[i];
            double r = i + shift < ref->an ? ref->a[i + shift] : 0;
            eo += o * o;
            er += r * r;
            ed += (o - r) * (o - r);
        }
        blocks++;
        if (eo < 1152 * 1e-6)
            silent++;
        else if (ed < 0.05 * er)
            match++;
        else {
            abad++;
            if (getenv("HDV_REPAIR_VERBOSE"))
                printf("   audio block %ld (%.3f s) neither: out %.4f ref %.4f diff %.4f\n", s / 1152,
                       s / 48000.0, sqrt(eo / 1152), sqrt(er / 1152), sqrt(ed / 1152));
        }
    }
    double vdur = m.n / m.fps, adur = (double)m.an / (m.sr ? m.sr : 1);
    printf("   %d frames (source %d, from %d), %d below 25 dB, %d not identical, %d past the "
           "source; %llu re-encoded; decode errors %d, timestamp steps off %d\n",
           m.n, ref->n, off, bad, nonexact, beyond, (unsigned long long)reenc, m.errors, m.pts_bad);
    printf("   audio %.3f s (video %.3f s): %d blocks, %d the source's, %d silent, %d neither\n",
           adur, vdur, blocks, match, silent, abad);

    CHECK_EQ_I(m.errors, 0);
    CHECK_EQ_I(m.pts_bad, 0);
    CHECK_EQ_I(beyond, 0);
    CHECK(bad <= e.max_bad);
    if (e.exact)
        CHECK_EQ_I(nonexact, 0);
    if (e.full_length)
        CHECK_EQ_I(m.n + off, ref->n);
    if (e.reencoded == 0)
        CHECK_EQ_I(reenc, 0);
    else if (e.reencoded == 1)
        CHECK(nonexact > 0);   /* (a span made at close is not counted in reenc yet) */
    CHECK(m.sr == 48000);
    CHECK(fabs(adur - vdur) <= 2 * 1152.0 / m.sr);
    CHECK(abad <= e.max_abad);
    CHECK(match >= e.min_amatch * blocks);
    CHECK(silent >= e.min_silent);
    if (fmt == PIN_FMT_HDV_MOV) {
        if (m.fw == 1440)
            CHECK_EQ_I(m.tag, MKTAG('h', 'd', 'v', '3'));
    } else {
        CHECK(m.extradata >= 12);
    }
    media_free(&m);
}

static const expect_t k_clean = { .exact = 1, .full_length = 1, .max_off = 0, .max_bad = 0,
                                  .reencoded = 0, .max_abad = 1, .min_amatch = 0.95 };

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr, "usage: %s hdv-repair.ts hdv-repair-closed.ts hdv-repair-jump.ts "
                        "hdv-720p.ts <out dir>\n", argv[0]);
        return 2;
    }
    for (int i = 1; i <= 4; i++)
        if (!pin_test_have_fixture(argv[i]))
            return 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    av_log_set_callback(log_cb);
    (void)g_prev_log;
    const char *dir = argv[5];

    blob_t src = load(argv[1]), closed = load(argv[2]), jump = load(argv[3]), p720 = load(argv[4]);
    media_t ref, refc, refj, ref720;
    decode(argv[1], &ref, 0);
    decode(argv[2], &refc, 0);
    decode(argv[3], &refj, 0);
    decode(argv[4], &ref720, 0);
    printf("source: %d frames, %.3f s of audio; %zu packets\n", ref.n, (double)ref.an / ref.sr, npk(src));
    CHECK(ref.n == 150);
    pin_sink_params_t p1080 = params_for(1440, 1080, 25, 1, 1);
    pin_sink_params_t q720 = params_for(1280, 720, 60000, 1001, 0);
    size_t N = npk(src);

    /* nothing wrong: passed through bit for bit, in both containers */
    scenario("clean-mov", copy(src), PIN_FMT_HDV_MOV, &p1080, &ref, dir, k_clean);
    scenario("clean-mkv", copy(src), PIN_FMT_HDV_MKV, &p1080, &ref, dir, k_clean);
    scenario("closed-clean", copy(closed), PIN_FMT_HDV_MOV, &p1080, &refc, dir, k_clean);
    {
        expect_t e = k_clean;
        scenario("720p-clean", copy(p720), PIN_FMT_HDV_MKV, &q720, &ref720, dir, e);
    }

    /* a short dropout inside a picture: 40 packets (video and audio) gone */
    {
        expect_t e = { .full_length = 1, .max_bad = 6, .reencoded = 1, .max_abad = 3,
                       .min_amatch = 0.9 };
        scenario("dropout-short", drop(copy(src), N / 4, 40, -1), PIN_FMT_HDV_MOV, &p1080, &ref, dir, e);
    }
    /* a long dropout: 0.6 s of the stream gone (whole pictures, a GOP and more): the file keeps
     * its length (frames repeated), the audio its sync (silence in the gap) */
    {
        expect_t e = { .full_length = 1, .max_bad = 30, .reencoded = 1, .max_abad = 4,
                       .min_amatch = 0.75, .min_silent = 15 };
        size_t n = (size_t)(0.6 * N / 6.0);
        scenario("dropout-long", drop(copy(src), N / 2, n, -1), PIN_FMT_HDV_MKV, &p1080, &ref, dir, e);
    }
    /* a garbled run: 500 packets without a sync byte */
    {
        blob_t b = copy(src);
        zero(b, N / 3, 500);
        expect_t e = { .full_length = 1, .max_bad = 12, .reencoded = 1, .max_abad = 4,
                       .min_amatch = 0.85 };
        scenario("garbled", b, PIN_FMT_HDV_MOV, &p1080, &ref, dir, e);
    }
    /* the deck flags packets it could not read (transport error bit) */
    {
        blob_t b = copy(src);
        tei(b, (N * 2) / 3, 30);
        expect_t e = { .full_length = 1, .max_bad = 6, .reencoded = 1, .max_abad = 1,
                       .min_amatch = 0.95 };
        scenario("tei", b, PIN_FMT_HDV_MOV, &p1080, &ref, dir, e);
    }
    /* audio packets lost alone: the video passes through untouched, silence fills the gap
     * and the audio is back right after it */
    {
        expect_t e = { .exact = 1, .full_length = 1, .reencoded = 0, .max_abad = 2,
                       .min_amatch = 0.85, .min_silent = 1 };
        scenario("audio-loss", drop(copy(src), N / 5, 12, APID), PIN_FMT_HDV_MOV, &p1080, &ref, dir, e);
    }
    /* several dropouts close together, one in each of three GOPs in a row */
    {
        blob_t b = copy(src);
        size_t at = N / 2;
        for (int i = 0; i < 3; i++)
            b = drop(b, at + (size_t)i * 1600, 25, -1);
        expect_t e = { .full_length = 1, .max_bad = 20, .reencoded = 1, .max_abad = 6,
                       .min_amatch = 0.85 };
        scenario("dropouts-close", b, PIN_FMT_HDV_MKV, &p1080, &ref, dir, e);
    }
    /* a recording that starts mid-GOP and ends mid-picture (a capture joining a running tape) */
    {
        blob_t b = drop(copy(src), 0, N / 20, -1);
        b.n -= 333 * TSP;
        expect_t e = { .max_off = 30, .max_bad = 4, .reencoded = -1, .max_abad = 3,
                       .min_amatch = 0.9 };
        scenario("cut-start-end", b, PIN_FMT_HDV_MOV, &p1080, &ref, dir, e);
    }
    /* a dropout right at the start of the first GOP */
    {
        size_t at = next_pes(src, 0);
        expect_t e = { .max_off = 30, .max_bad = 4, .reencoded = -1, .max_abad = 3,
                       .min_amatch = 0.85 };
        scenario("dropout-first-gop", drop(copy(src), at + 3, 30, -1), PIN_FMT_HDV_MOV, &p1080,
                 &ref, dir, e);
    }
    /* closed GOPs with a dropout */
    {
        size_t Nc = npk(closed);
        expect_t e = { .full_length = 1, .max_bad = 8, .reencoded = 1, .max_abad = 3,
                       .min_amatch = 0.85 };
        scenario("closed-dropout", drop(copy(closed), Nc / 2, 60, -1), PIN_FMT_HDV_MOV, &p1080,
                 &refc, dir, e);
    }
    /* the camera clock jumps (another recording, no file split): the output just goes on,
     * no gap, audio of each part with its part. (The continuity counters jump there too, so
     * the last picture before it may have lost its end: that GOP is re-encoded.) */
    {
        media_t refjoin;
        decode(argv[2], &refjoin, 0);
        media_join(&refjoin, &refj);
        expect_t e = { .full_length = 1, .max_bad = 0, .reencoded = -1, .max_abad = 3,
                       .min_amatch = 0.9 };
        scenario("clock-jump", cat(copy(closed), jump), PIN_FMT_HDV_MOV, &p1080, &refjoin, dir, e);
        media_free(&refjoin);
    }
    /* damage everywhere (a tape that is bad all along): every GOP re-encoded, in parts of a
     * few seconds handed to the workers; the file keeps its length and stays decodable */
    {
        blob_t b = copy(src);
        for (size_t at = 700; at + 3 < N; at += 1500)
            zero(b, at, 3);
        expect_t e = { .full_length = 1, .max_off = 0, .max_bad = 16, .reencoded = 1,
                       .max_abad = 60, .min_amatch = 0.5 };
        clock_t t0 = clock();
        scenario("all-damaged", b, PIN_FMT_HDV_MOV, &p1080, &ref, dir, e);
        printf("   (%.1f s of CPU for 6 s of video, the checks included)\n",
               (double)(clock() - t0) / CLOCKS_PER_SEC);
    }
    /* 720p with a dropout */
    {
        expect_t e = { .full_length = 1, .max_bad = 8, .reencoded = 1, .max_abad = 3,
                       .min_amatch = 0.6 };
        scenario("720p-dropout", drop(copy(p720), npk(p720) / 2, 30, -1), PIN_FMT_HDV_MKV, &q720,
                 &ref720, dir, e);
    }

    /* the repair cannot keep up: the sink fails, as the capture then ends */
    {
        printf("== behind\n");
        blob_t b = copy(src);
        for (int i = 1; i < 6; i++)
            zero(b, (N * (size_t)i) / 6, 50);
        char out[1024];
        snprintf(out, sizeof(out), "%s/repair-behind.mov", dir);
        pin_sink_t *s = pin_sink_create(PIN_FMT_HDV_MOV);
        sink_hdvmux_test_set(s, 4u << 20, 1500);
        CHECK(s->open(s, out, &p1080) == PIN_OK);
        pin_status_t rc = PIN_OK;
        for (size_t i = 0; i < npk(b) && rc == PIN_OK; i += 200)
            rc = s->write_unit(s, b.d + i * TSP, (i + 200 <= npk(b) ? 200 : npk(b) - i) * TSP);
        pin_sink_status_t st;
        s->get_status(s, &st);
        printf("   write_unit: %d, reencode_behind %d\n", rc, st.reencode_behind);
        CHECK_EQ_I(rc, PIN_ERR_CODEC);
        CHECK_EQ_I(st.reencode_behind, 1);
        CHECK_EQ_I(s->close(s), PIN_ERR_CODEC);
        free(b.d);
    }

    free(src.d);
    free(closed.d);
    free(p720.d);
    media_free(&ref);
    media_free(&refc);
    media_free(&refj);
    media_free(&ref720);
    printf("OK\n");
    return 0;
}
