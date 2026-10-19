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
 * ctest: synthetic PAL-geometry YUYV frames + a sine tone through
 * sink_avi (PIN_FMT_ANALOG_AVI) and sink_ffv1 (PIN_FMT_ANALOG_FFV1_MKV),
 * reopened and checked with libavformat. Also measures sink_ffv1's
 * throughput on noise-ish frames (worst case for an intra lossless codec)
 * and reports it -- see the task report for the number, this test only
 * asserts it's not degenerate (elapsed > 0, at least a few fps), since the
 * actual number depends on the build machine.
 */

#include "test_util.h"
#include "../../src/sinks/pin_sink.h"
#include "../../src/sinks/sinks_internal.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/md5.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
static double now_s(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
#else
static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
#endif

#define W 720
#define H 576
#define FPS_NUM 25
#define FPS_DEN 1
#define ARATE 48000
#define ACHAN 2
#define SAMPLES_PER_FRAME (ARATE * FPS_DEN / FPS_NUM)  /* 1920 for PAL */

static void fill_gradient_frame(uint8_t *yuyv, int frame_idx)
{
    for (int y = 0; y < H; y++) {
        uint8_t *row = yuyv + (size_t)y * W * 2;
        for (int x = 0; x < W / 2; x++) {
            uint8_t lum = (uint8_t)((x + y + frame_idx * 4) & 0xff);
            row[4 * x + 0] = lum;
            row[4 * x + 1] = 128;
            row[4 * x + 2] = lum;
            row[4 * x + 3] = 128;
        }
    }
}

static void fill_noise_frame(uint8_t *yuyv, unsigned *seed)
{
    size_t n = (size_t)W * H * 2;
    for (size_t i = 0; i < n; i++) {
        *seed = *seed * 1103515245u + 12345u;
        yuyv[i] = (uint8_t)(*seed >> 16);
    }
}

static void fill_tone(int16_t *pcm, int frames, int frame_idx)
{
    for (int i = 0; i < frames; i++) {
        double t = (double)(frame_idx * frames + i) / ARATE;
        int16_t s = (int16_t)(8000.0 * sin(2 * M_PI * 440.0 * t));
        pcm[2 * i] = s;
        pcm[2 * i + 1] = s;
    }
}

static void common_params(pin_sink_params_t *p, const char *title)
{
    memset(p, 0, sizeof(*p));
    p->kind = PIN_KIND_ANALOG;
    p->width = W;
    p->height = H;
    p->fps_num = FPS_NUM;
    p->fps_den = FPS_DEN;
    p->interlaced = 1;
    p->top_field_first = 1;
    p->aspect = PIN_ASPECT_4_3;
    pin_sink_sar_for(PIN_KIND_ANALOG, 1, p->aspect, W, &p->sample_aspect_num, &p->sample_aspect_den);
    p->colour_matrix = PIN_MATRIX_BT601;
    p->audio_rate = ARATE;
    p->audio_channels = ACHAN;
    snprintf(p->title, sizeof(p->title), "%s", title);
}

static void test_avi(void)
{
    printf("== sink_avi ==\n");
    pin_sink_params_t p;
    common_params(&p, "marvin-core analog AVI test");

    pin_sink_t *s = pin_sink_create(PIN_FMT_ANALOG_AVI);
    CHECK(s);
    CHECK(s->open(s, "test_analog.avi", &p) == PIN_OK);

    const int N = 10;
    uint8_t *yuyv = malloc((size_t)W * H * 2);
    int16_t *pcm = malloc((size_t)SAMPLES_PER_FRAME * ACHAN * sizeof(int16_t));
    for (int i = 0; i < N; i++) {
        fill_gradient_frame(yuyv, i);
        fill_tone(pcm, SAMPLES_PER_FRAME, i);
        CHECK(s->write_video(s, yuyv, (size_t)W * H * 2) == PIN_OK);
        CHECK(s->write_audio(s, pcm, SAMPLES_PER_FRAME) == PIN_OK);
    }
    free(yuyv);
    free(pcm);
    CHECK(s->close(s) == PIN_OK);

    AVFormatContext *fc = NULL;
    CHECK(avformat_open_input(&fc, "test_analog.avi", NULL, NULL) >= 0);
    CHECK(avformat_find_stream_info(fc, NULL) >= 0);
    int vidx = -1, aidx = -1;
    for (unsigned i = 0; i < fc->nb_streams; i++) {
        if (fc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            vidx = (int)i;
        else if (fc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            aidx = (int)i;
    }
    CHECK(vidx >= 0 && aidx >= 0);
    CHECK_EQ_I(fc->streams[vidx]->codecpar->width, W);
    CHECK_EQ_I(fc->streams[vidx]->codecpar->height, H);
    CHECK_EQ_I(fc->streams[vidx]->codecpar->codec_tag, MKTAG('Y', 'U', 'Y', '2'));
    CHECK_EQ_I(fc->streams[aidx]->codecpar->sample_rate, ARATE);
    CHECK_EQ_I(fc->streams[aidx]->codecpar->ch_layout.nb_channels, ACHAN);
    AVDictionaryEntry *t = av_dict_get(fc->metadata, "title", NULL, 0);
    CHECK(t && strcmp(t->value, p.title) == 0);

    int64_t vframes = 0;
    AVPacket *pkt = av_packet_alloc();
    while (av_read_frame(fc, pkt) >= 0) {
        if (pkt->stream_index == vidx)
            vframes++;
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    CHECK_EQ_I(vframes, N);
    avformat_close_input(&fc);
    printf("  OK: %d frames, %d Hz / %d ch audio, title \"%s\"\n", N, ARATE, ACHAN, p.title);
}

/* A non-AUTO aspect override (PIN_ASPECT_16_9, e.g. from pin_set_aspect()
 * for widescreen VHS/LaserDisc sources the SAA7113 can't detect on its
 * own) must reach the file's SAR/DAR from every sink that writes one --
 * this checks sink_avi's OpenDML vprp DAR and sink_ffv1's Matroska
 * DisplayWidth/Height-derived SAR both honour it, using
 * pin_sink_sar_for()'s own 16:9 PAL value (64:45) as the expected figure
 * so this test and the sinks can never silently agree on the wrong
 * number. */
static void test_aspect_override(void)
{
    printf("== aspect override (16:9) ==\n");
    pin_sink_params_t p;
    common_params(&p, "aspect override test");
    p.aspect = PIN_ASPECT_16_9;
    int want_num, want_den;
    pin_sink_sar_for(PIN_KIND_ANALOG, 1, PIN_ASPECT_16_9, W, &want_num, &want_den);
    CHECK_EQ_I(want_num, 64);   /* PAL 16:9, 720-wide: the documented 64:45 */
    CHECK_EQ_I(want_den, 45);
    /* sample_aspect_num/den intentionally left at whatever common_params()
     * set for 4:3 (or even garbage): the point of this test is that the
     * sinks derive SAR from params->aspect themselves rather than trusting
     * a caller-precomputed value that may not match. */

    uint8_t *yuyv = malloc((size_t)W * H * 2);
    fill_gradient_frame(yuyv, 0);

    pin_sink_t *avi = pin_sink_create(PIN_FMT_ANALOG_AVI);
    CHECK(avi && avi->open(avi, "test_aspect.avi", &p) == PIN_OK);
    CHECK(avi->write_video(avi, yuyv, (size_t)W * H * 2) == PIN_OK);
    CHECK(avi->close(avi) == PIN_OK);

    AVFormatContext *fc = NULL;
    CHECK(avformat_open_input(&fc, "test_aspect.avi", NULL, NULL) >= 0);
    CHECK(avformat_find_stream_info(fc, NULL) >= 0);
    int vidx = -1;
    for (unsigned i = 0; i < fc->nb_streams; i++)
        if (fc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            vidx = (int)i;
    CHECK(vidx >= 0);
    /* avi_writer writes the whole-frame DAR (16:9) into the OpenDML vprp
     * box; ffmpeg's avidec.c divides that back out by width:height on
     * read, so what comes back on AVStream.sample_aspect_ratio is already
     * a per-pixel SAR -- and for a 720x576 PAL frame at 16:9 that's
     * exactly pin_sink_sar_for()'s own 64:45, so it can be compared
     * directly against want_num/want_den like the FFV1/MKV case below. */
    AVRational dar = fc->streams[vidx]->sample_aspect_ratio;
    CHECK_EQ_I(dar.num, want_num);
    CHECK_EQ_I(dar.den, want_den);
    avformat_close_input(&fc);
    printf("  sink_avi: SAR read back as %d:%d (16:9 DAR correctly round-tripped)\n", dar.num, dar.den);

    pin_sink_t *ffv1 = pin_sink_create(PIN_FMT_ANALOG_FFV1_MKV);
    CHECK(ffv1 && ffv1->open(ffv1, "test_aspect.mkv", &p) == PIN_OK);
    CHECK(ffv1->write_video(ffv1, yuyv, (size_t)W * H * 2) == PIN_OK);
    CHECK(ffv1->close(ffv1) == PIN_OK);
    free(yuyv);

    CHECK(avformat_open_input(&fc, "test_aspect.mkv", NULL, NULL) >= 0);
    CHECK(avformat_find_stream_info(fc, NULL) >= 0);
    vidx = -1;
    for (unsigned i = 0; i < fc->nb_streams; i++)
        if (fc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            vidx = (int)i;
    CHECK(vidx >= 0);
    CHECK_EQ_I(fc->streams[vidx]->sample_aspect_ratio.num, want_num);
    CHECK_EQ_I(fc->streams[vidx]->sample_aspect_ratio.den, want_den);
    avformat_close_input(&fc);
    printf("  sink_ffv1: SAR read back as %d:%d (matches pin_sink_sar_for exactly)\n",
           want_num, want_den);
}

static void test_ffv1_correctness(void)
{
    printf("== sink_ffv1 (correctness) ==\n");
    pin_sink_params_t p;
    common_params(&p, "marvin-core FFV1 test");

    pin_sink_t *s = pin_sink_create(PIN_FMT_ANALOG_FFV1_MKV);
    CHECK(s);
    CHECK(s->open(s, "test_analog.mkv", &p) == PIN_OK);

    const int N = 10;
    uint8_t *yuyv = malloc((size_t)W * H * 2);
    int16_t *pcm = malloc((size_t)SAMPLES_PER_FRAME * ACHAN * sizeof(int16_t));
    for (int i = 0; i < N; i++) {
        fill_gradient_frame(yuyv, i);
        fill_tone(pcm, SAMPLES_PER_FRAME, i);
        CHECK(s->write_video(s, yuyv, (size_t)W * H * 2) == PIN_OK);
        CHECK(s->write_audio(s, pcm, SAMPLES_PER_FRAME) == PIN_OK);
        /* Give the encoder thread time to keep the bounded queue from
         * overflowing under a synthetic, no-USB-pacing test loop -- a real
         * capture is naturally paced by the device at 25/30 fps. */
#if defined(_WIN32)
        Sleep(15);
#else
        struct timespec ts = { 0, 15000000L };
        nanosleep(&ts, NULL);
#endif
    }
    free(yuyv);
    free(pcm);

    pin_sink_status_t st;
    s->get_status(s, &st);
    CHECK_EQ_I(st.last_error, PIN_OK);
    CHECK(s->close(s) == PIN_OK);
    printf("  encoder_behind flag during the paced correctness run: %d\n", st.encoder_behind);

    AVFormatContext *fc = NULL;
    CHECK(avformat_open_input(&fc, "test_analog.mkv", NULL, NULL) >= 0);
    CHECK(avformat_find_stream_info(fc, NULL) >= 0);
    int vidx = -1, aidx = -1;
    for (unsigned i = 0; i < fc->nb_streams; i++) {
        if (fc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            vidx = (int)i;
        else if (fc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            aidx = (int)i;
    }
    CHECK(vidx >= 0 && aidx >= 0);
    CHECK_EQ_I(fc->streams[vidx]->codecpar->codec_id, AV_CODEC_ID_FFV1);
    CHECK_EQ_I(fc->streams[vidx]->codecpar->format, AV_PIX_FMT_YUV422P);
    CHECK_EQ_I(fc->streams[vidx]->codecpar->width, W);
    CHECK_EQ_I(fc->streams[vidx]->codecpar->height, H);
    CHECK_EQ_I(fc->streams[vidx]->codecpar->field_order, AV_FIELD_TT);
    CHECK_EQ_I(fc->streams[vidx]->codecpar->color_space, AVCOL_SPC_BT470BG);
    /* SAR lives on AVStream (both matroskaenc's DisplayWidth/Height and
     * matroskadec's reader use st->sample_aspect_ratio), not on
     * AVCodecParameters -- see test_dv_rewrap.c's verify_container for the
     * same point made about the avi/mov pair. */
    CHECK_EQ_I(fc->streams[vidx]->sample_aspect_ratio.num, p.sample_aspect_num);
    CHECK_EQ_I(fc->streams[vidx]->sample_aspect_ratio.den, p.sample_aspect_den);
    CHECK_EQ_I(fc->streams[aidx]->codecpar->codec_id, AV_CODEC_ID_FLAC);
    CHECK_EQ_I(fc->streams[aidx]->codecpar->ch_layout.nb_channels, ACHAN);
    CHECK_EQ_I(fc->streams[aidx]->codecpar->sample_rate, ARATE);
    AVDictionaryEntry *t = av_dict_get(fc->metadata, "title", NULL, 0);
    CHECK(t && strcmp(t->value, p.title) == 0);

    int64_t vframes = 0;
    AVPacket *pkt = av_packet_alloc();
    while (av_read_frame(fc, pkt) >= 0) {
        if (pkt->stream_index == vidx)
            vframes++;
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    CHECK_EQ_I(vframes, N);
    avformat_close_input(&fc);
    printf("  OK: %d frames, FFV1 yuv422p %dx%d, BT.470BG, TFF, FLAC audio\n", N, W, H);
}

/* ---- FLAC audio bit-exactness and long-capture timestamps ---- */

/* Deterministic, non-trivial stereo signal as a function of the sample index
 * alone: a different tone plus differently-sized hash noise per channel, so
 * a L/R swap, an off-by-one or a dropped/duplicated block all show up. */
static uint32_t hash32(uint32_t x, uint32_t salt)
{
    x = x * 2654435761u + salt;
    x ^= x >> 15;
    x *= 0x2c1b3c6du;
    x ^= x >> 12;
    x *= 0x297a2d39u;
    x ^= x >> 15;
    return x;
}

static void gen_signal(int16_t *pcm, size_t frames)
{
    for (size_t j = 0; j < frames; j++) {
        double t = (double)j / ARATE;
        pcm[2 * j] = (int16_t)((int)(7000.0 * sin(2 * M_PI * 440.0 * t)) +
                               (int)(hash32((uint32_t)j, 1) & 0x1ff) - 256);
        pcm[2 * j + 1] = (int16_t)((int)(9000.0 * sin(2 * M_PI * 1234.5 * t)) +
                                   (int)(hash32((uint32_t)j, 2) & 0xfff) - 2048);
    }
}

/* Deliberately awkward write_audio() sizes: 1 frame, primes, one FLAC block
 * (4608 at level 8) +/- 1, several blocks, and (below) once more than the
 * whole 4 s ring. */
static const size_t chunk_sizes[] = { 1, 7, 333, 1920, 4999, 2, 5000, 641, 4608, 4609,
                                      9300, 128, 1023, 17, 4607, 3 };
#define N_CHUNKS ((int)(sizeof(chunk_sizes) / sizeof(chunk_sizes[0])))

/* Pulls the STREAMINFO block out of Matroska CodecPrivate, which may be the
 * bare 34 bytes, the 4-byte metadata block header + 34 bytes, or "fLaC" +
 * header + 34 bytes. */
static const uint8_t *find_streaminfo(const uint8_t *ex, int size)
{
    if (size >= 4 && memcmp(ex, "fLaC", 4) == 0) {
        ex += 4;
        size -= 4;
    }
    if (size >= 38 && (ex[0] & 0x7f) == 0 && ex[1] == 0 && ex[2] == 0 && ex[3] == 34)
        return ex + 4;
    if (size == 34)
        return ex;
    return NULL;
}

/* Writes nvideo frames and naudio sample frames (odd chunk sizes, audio
 * roughly interleaved with the video), starting the sink's timelines at
 * vstart / astart, then reopens the file and checks everything:
 *  - exact video frame count, strictly increasing pts, first pts and the
 *    40 ms spacing exact (25 fps in a 1 ms time base);
 *  - FLAC stream parameters, STREAMINFO total_samples + MD5 vs the input;
 *  - every decoded sample identical to the input, sample count exact;
 *  - audio pts strictly increasing, each within 1 ms of the exact position
 *    (astart + samples decoded before it) / rate. Exact equality isn't
 *    possible: Matroska's 1 ms time base rounds each packet independently. */
static void roundtrip(const char *path, int64_t vstart, int64_t astart, int nvideo,
                      size_t naudio, int big_call)
{
    pin_sink_params_t p;
    common_params(&p, "flac roundtrip");

    int16_t *in = malloc(naudio * ACHAN * sizeof(int16_t));
    CHECK(in);
    gen_signal(in, naudio);
    uint8_t *yuyv = malloc((size_t)W * H * 2);
    CHECK(yuyv);
    fill_gradient_frame(yuyv, 3);   /* static content: only the audio is under test */

    pin_sink_t *s = pin_sink_create(PIN_FMT_ANALOG_FFV1_MKV);
    CHECK(s);
    if (vstart || astart)
        sink_ffv1_test_set_start(s, vstart, astart);
    CHECK(s->open(s, path, &p) == PIN_OK);

    size_t done = 0;
    int call = 0;
    for (int i = 0; i < nvideo; i++) {
        CHECK(s->write_video(s, yuyv, (size_t)W * H * 2) == PIN_OK);
        size_t target = (size_t)(i + 1) * SAMPLES_PER_FRAME;
        if (target > naudio)
            target = naudio;
        while (done < target) {
            size_t n = (big_call && call == 40) ? 250000 : chunk_sizes[call % N_CHUNKS];
            call++;
            if (n > naudio - done)
                n = naudio - done;
            CHECK(s->write_audio(s, in + done * ACHAN, n) == PIN_OK);
            done += n;
        }
    }
    while (done < naudio) {   /* audio tail beyond the last video frame */
        size_t n = chunk_sizes[call++ % N_CHUNKS];
        if (n > naudio - done)
            n = naudio - done;
        CHECK(s->write_audio(s, in + done * ACHAN, n) == PIN_OK);
        done += n;
    }
    free(yuyv);

    pin_sink_status_t st;
    s->get_status(s, &st);
    CHECK_EQ_I(st.last_error, PIN_OK);
    CHECK(s->close(s) == PIN_OK);

    AVFormatContext *fc = NULL;
    CHECK(avformat_open_input(&fc, path, NULL, NULL) >= 0);
    CHECK(avformat_find_stream_info(fc, NULL) >= 0);
    int vidx = -1, aidx = -1;
    for (unsigned i = 0; i < fc->nb_streams; i++) {
        if (fc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            vidx = (int)i;
        else if (fc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            aidx = (int)i;
    }
    CHECK(vidx >= 0 && aidx >= 0);
    AVStream *ast = fc->streams[aidx], *vst = fc->streams[vidx];
    CHECK_EQ_I(ast->codecpar->codec_id, AV_CODEC_ID_FLAC);
    CHECK_EQ_I(ast->codecpar->ch_layout.nb_channels, ACHAN);
    CHECK_EQ_I(ast->codecpar->sample_rate, ARATE);

    /* STREAMINFO: total_samples (36 bits) and MD5 must have been written back
     * by the trailer, since the header was written before any audio existed. */
    const uint8_t *si = find_streaminfo(ast->codecpar->extradata, ast->codecpar->extradata_size);
    CHECK(si);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | si[10 + i];   /* rate(20) ch-1(3) bps-1(5) total(36) */
    CHECK_EQ_I((v >> 44) & 0xfffff, ARATE);
    CHECK_EQ_I(((v >> 41) & 7) + 1, ACHAN);
    CHECK_EQ_I(((v >> 36) & 31) + 1, 16);
    CHECK_EQ_I(v & ((1ULL << 36) - 1), (long long)naudio);
    uint8_t want_md5[16];
    struct AVMD5 *m = av_md5_alloc();
    CHECK(m);
    av_md5_init(m);
    av_md5_update(m, (const uint8_t *)in, naudio * ACHAN * sizeof(int16_t));   /* s16le */
    av_md5_final(m, want_md5);
    av_free(m);
    static const uint8_t zero_md5[16];
    CHECK(memcmp(si + 18, zero_md5, 16) != 0);
    CHECK(memcmp(si + 18, want_md5, 16) == 0);

    const AVCodec *dec = avcodec_find_decoder(AV_CODEC_ID_FLAC);
    CHECK(dec);
    AVCodecContext *dc = avcodec_alloc_context3(dec);
    CHECK(dc);
    CHECK(avcodec_parameters_to_context(dc, ast->codecpar) >= 0);
    CHECK(avcodec_open2(dc, dec, NULL) >= 0);

    size_t decoded = 0;
    int64_t vcount = 0, acount = 0, vprev = INT64_MIN, aprev = INT64_MIN, adur_prev = 0;
    int64_t vfirst = 0, afirst = 0;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    while (av_read_frame(fc, pkt) >= 0) {
        if (pkt->stream_index == vidx) {
            int64_t ms = av_rescale_q(pkt->pts, vst->time_base, (AVRational){ 1, 1000 });
            if (vcount == 0)
                vfirst = ms;
            CHECK(ms > vprev);
            /* 25 fps = exactly 40 ms per frame, no rounding at all */
            CHECK_EQ_I(ms, (vstart + vcount) * 40);
            vprev = ms;
            vcount++;
        } else if (pkt->stream_index == aidx) {
            int64_t ms = av_rescale_q(pkt->pts, ast->time_base, (AVRational){ 1, 1000 });
            if (acount == 0)
                afirst = ms;
            CHECK(ms > aprev);
            if (adur_prev > 0)   /* contiguous: next pts = previous pts + its duration */
                CHECK(llabs((long long)(ms - (aprev + adur_prev))) <= 1);
            int64_t exact = (astart + (int64_t)decoded) * 1000 / ARATE;
            CHECK(llabs((long long)(ms - exact)) <= 1);
            aprev = ms;
            adur_prev = av_rescale_q(pkt->duration, ast->time_base, (AVRational){ 1, 1000 });
            acount++;
            CHECK(avcodec_send_packet(dc, pkt) >= 0);
            while (avcodec_receive_frame(dc, fr) >= 0) {
                CHECK_EQ_I(fr->format, AV_SAMPLE_FMT_S16);
                CHECK_EQ_I(fr->ch_layout.nb_channels, ACHAN);
                CHECK(decoded + (size_t)fr->nb_samples <= naudio);
                CHECK(memcmp(fr->data[0], in + decoded * ACHAN,
                             (size_t)fr->nb_samples * ACHAN * sizeof(int16_t)) == 0);
                decoded += (size_t)fr->nb_samples;
            }
        }
        av_packet_unref(pkt);
    }
    CHECK(avcodec_send_packet(dc, NULL) >= 0);
    while (avcodec_receive_frame(dc, fr) >= 0) {
        CHECK(decoded + (size_t)fr->nb_samples <= naudio);
        CHECK(memcmp(fr->data[0], in + decoded * ACHAN,
                     (size_t)fr->nb_samples * ACHAN * sizeof(int16_t)) == 0);
        decoded += (size_t)fr->nb_samples;
    }
    av_frame_free(&fr);
    av_packet_free(&pkt);
    avcodec_free_context(&dc);
    avformat_close_input(&fc);

    CHECK_EQ_I(vcount, nvideo);
    CHECK(acount > 1);
    CHECK_EQ_I(decoded, naudio);
    /* First pts of each stream is the configured start (ms rounding), and the
     * two streams begin within a frame of each other. */
    CHECK(llabs((long long)(vfirst - vstart * 40)) <= 1);
    CHECK(llabs((long long)(afirst - astart * 1000 / ARATE)) <= 1);
    CHECK(llabs((long long)(afirst - vfirst)) <= 40);
    printf("  %d video frames, %zu audio samples (%lld packets) bit-exact; first pts v=%.3f s a=%.3f s\n",
           nvideo, naudio, (long long)acount, vfirst / 1000.0, afirst / 1000.0);
    free(in);
}

/* ~16 s of audio through a 4 s ring (four wraps), awkward call sizes
 * including one larger than the ring, total length not a multiple of the
 * FLAC block size. */
static void test_ffv1_audio_exact(void)
{
    printf("== sink_ffv1 (FLAC bit-exactness) ==\n");
    size_t naudio = 16 * (size_t)ARATE + 777;
    CHECK(naudio % 4608 != 0 && naudio % 4096 != 0 && naudio % 1152 != 0);
    roundtrip("test_flac_exact.mkv", 0, 0, 400, naudio, 1);
}

/* A capture that has already run 30 h (audio past 2^32 samples: 5.184e9), and
 * one whose written range straddles 2^31 samples (12.43 h). Nothing may go
 * through a 32-bit count; pts must continue from the offset. */
static void test_ffv1_long_capture(void)
{
    printf("== sink_ffv1 (long-capture timestamps) ==\n");
    int64_t a30 = 30LL * 3600 * ARATE;
    CHECK(a30 > 4294967295LL);
    roundtrip("test_flac_30h.mkv", 30LL * 3600 * FPS_NUM, a30, 100, 100 * (size_t)SAMPLES_PER_FRAME + 321, 0);

    /* start 5 s before 2^31 samples, on a video frame boundary, write ~10 s */
    int64_t a31 = ((2147483648LL - 5 * ARATE) / SAMPLES_PER_FRAME) * SAMPLES_PER_FRAME;
    CHECK(a31 < 2147483648LL && a31 + 10 * (int64_t)ARATE > 2147483648LL);
    roundtrip("test_flac_2p31.mkv", a31 / SAMPLES_PER_FRAME, a31, 250, 250 * (size_t)SAMPLES_PER_FRAME + 55, 0);
}

static double measure_ffv1_fps(void)
{
    printf("== sink_ffv1 (throughput, noise-ish frames) ==\n");
    pin_sink_params_t p;
    common_params(&p, "");

    pin_sink_t *s = pin_sink_create(PIN_FMT_ANALOG_FFV1_MKV);
    CHECK(s);
    CHECK(s->open(s, "test_analog_fps.mkv", &p) == PIN_OK);

    const int N = 100;
    uint8_t *yuyv = malloc((size_t)W * H * 2);
    unsigned seed = 12345;

    double t0 = now_s();
    for (int i = 0; i < N; i++) {
        fill_noise_frame(yuyv, &seed);
        /* unpaced: write_video() waits whenever the encoder falls behind */
        CHECK(s->write_video(s, yuyv, (size_t)W * H * 2) == PIN_OK);
    }
    free(yuyv);
    CHECK(s->close(s) == PIN_OK);   /* blocks until the encoder drains */
    double elapsed = now_s() - t0;

    /* Re-open: every one of the N frames must be in the file. */
    AVFormatContext *fc = NULL;
    CHECK(avformat_open_input(&fc, "test_analog_fps.mkv", NULL, NULL) >= 0);
    CHECK(avformat_find_stream_info(fc, NULL) >= 0);
    int vidx = -1;
    for (unsigned i = 0; i < fc->nb_streams; i++)
        if (fc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            vidx = (int)i;
    int64_t vframes = 0;
    AVPacket *pkt = av_packet_alloc();
    while (av_read_frame(fc, pkt) >= 0) {
        if (pkt->stream_index == vidx)
            vframes++;
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&fc);

    double fps = elapsed > 0 ? vframes / elapsed : 0;
    printf("  %lld of %d frames encoded in %.3f s -> %.1f fps (%dx%d yuv422p, level3, "
           "slices16, slicecrc1)\n", (long long)vframes, N, elapsed, fps, W, H);
    CHECK(vframes == N);
    CHECK(elapsed > 0);
    return fps;
}

int main(void)
{
    test_avi();
    test_aspect_override();
    test_ffv1_correctness();
    test_ffv1_audio_exact();
    test_ffv1_long_capture();
    measure_ffv1_fps();
    printf("OK\n");
    return 0;
}
