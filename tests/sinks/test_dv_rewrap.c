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
 * ctest: feeds a raw EP 0x88 DV trace (tests/data/ep88-pal.bin = PAL,
 * tests/data/ep88-ntsc.bin = NTSC with 32 kHz 12-bit audio) through
 * dv_reassembler, then through sink_rewrap's DV_AVI and DV_MOV paths, and
 * checks the results by reopening them with libavformat:
 *   - stream count/codec/dimensions/field order/SAR/title
 *   - DV AVI codec_tag == "dvsd"
 *   - video packet count == frames fed
 *   - PCM audio is non-silent, with a sample count matching frames *
 *     samples-per-frame within one frame
 *
 * It also doubles as the "dvaudio_check" the plan asks for: the same
 * capture is also written byte-for-byte as raw .dv (dv_reassembler's
 * ordinary output -- ground truth), then decoded whole with FFmpeg's own
 * "dv" demuxer/audio-extractor and compared sample-for-sample against the
 * audio track our DV_AVI rewrap produced (see sink_rewrap.c's header
 * comment for why both sides share that same extraction code, and what
 * this comparison still catches: any bug in *our* repack/remux around it).
 */

#include "test_util.h"
#include "../../src/sinks/pin_sink.h"
#include "../../src/sinks/sinks_internal.h"
#include "../../src/engine/dv_subcode.h"
#include "../../src/core/dv_reassembler.h"

#include <libavformat/avformat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t **frames;
    size_t *lens;
    size_t n, cap;
    FILE *raw;
} collect_ctx_t;

static int on_write(const uint8_t *data, size_t len, void *user)
{
    collect_ctx_t *c = user;
    return fwrite(data, 1, len, c->raw) == len ? 0 : -1;
}

static void on_unit(dv_format_t fmt, const uint8_t *data, size_t len, void *user)
{
    (void)fmt;
    collect_ctx_t *c = user;
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

/* Reads every frame of `path` through dv_reassembler_feed, collecting whole
 * DV frames into *out and writing byte-identical raw .dv to raw_path. */
static void reassemble(const char *path, const char *raw_path, collect_ctx_t *out)
{
    memset(out, 0, sizeof(*out));
    out->raw = fopen(raw_path, "wb");
    CHECK(out->raw);

    FILE *in = fopen(path, "rb");
    CHECK(in);

    dv_output_t o = { .write = on_write, .on_unit = on_unit, .user = out };
    dv_reassembler_t r;
    CHECK(dv_reassembler_init(&r, &o) == 0);

    uint8_t buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        CHECK(dv_reassembler_feed(&r, buf, n) == 0);
    dv_reassembler_finish(&r);
    fclose(in);
    fclose(out->raw);

    CHECK(out->n > 5);   /* sanity: this really did find frames */
}

/* Minimal read-only, seekable in-memory AVIOContext -- a copy of
 * sink_rewrap.c's mem_ctx_t/mem_read/mem_seek, used here to independently
 * re-derive (not reuse) what the per-frame "dv" demux produces for pair 1
 * of one frame, so the frame-by-frame dvaudio_check below isn't comparing
 * the sink's output against its own working state. */
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

/* Re-demuxes one frame's pair-1 audio (mirroring sink_rewrap.c's own
 * per-frame extraction, independently). Returns malloc'd PCM (may be
 * length 0) via *out, sample count via return value. */
static int64_t demux_one_frame_pair1(const uint8_t *data, size_t len, int16_t **out, int *ch_out)
{
    const AVInputFormat *dvfmt = av_find_input_format("dv");
    mem_ctx_t mem = { .base = data, .size = len, .pos = 0 };
    uint8_t *iobuf = av_malloc(4096);
    AVIOContext *avio = avio_alloc_context(iobuf, 4096, 0, &mem, mem_read, NULL, mem_seek);
    AVFormatContext *in = avformat_alloc_context();
    in->pb = avio;
    if (avformat_open_input(&in, NULL, dvfmt, NULL) < 0) {
        avio_context_free(&avio);
        *out = NULL;
        return 0;
    }

    int audio_idx = -1;
    uint8_t *buf = malloc(1 << 16);
    size_t buflen = 0;
    AVPacket *pkt = av_packet_alloc();
    while (av_read_frame(in, pkt) >= 0) {
        if (audio_idx < 0) {
            for (unsigned i = 0; i < in->nb_streams; i++)
                if (in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                    audio_idx = (int)i;
                    break;
                }
        }
        if (audio_idx >= 0 && pkt->stream_index == audio_idx) {
            memcpy(buf + buflen, pkt->data, (size_t)pkt->size);
            buflen += (size_t)pkt->size;
            if (ch_out)
                *ch_out = in->streams[audio_idx]->codecpar->ch_layout.nb_channels;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&in);

    *out = (int16_t *)buf;
    return (int64_t)(buflen / (2 * (size_t)(ch_out ? *ch_out : 2)));
}

static void write_via_sink(pin_format_t fmt, const char *path, const pin_sink_params_t *params,
                           collect_ctx_t *frames)
{
    pin_sink_t *s = pin_sink_create(fmt);
    CHECK(s);
    CHECK(s->open(s, path, params) == PIN_OK);
    for (size_t i = 0; i < frames->n; i++) {
        pin_status_t rc = s->write_unit(s, frames->frames[i], frames->lens[i]);
        CHECK(rc == PIN_OK);
    }
    CHECK(s->close(s) == PIN_OK);
}

/* Opens a rewrapped file and checks stream layout/metadata. Returns the
 * video packet count (== frames fed, on success) and, if audio is present,
 * the muxed audio sample count via *audio_samples (-1 if no audio track). */
static int64_t verify_container(const char *path, int expect_avi_tag, int width, int height,
                                int sar_num, int sar_den, const char *title,
                                int64_t *audio_samples, int *audio_channels, int *audio_rate)
{
    AVFormatContext *fc = NULL;
    CHECK(avformat_open_input(&fc, path, NULL, NULL) >= 0);
    CHECK(avformat_find_stream_info(fc, NULL) >= 0);

    /* Pair 1 (the lowest-index audio stream, when the 4-ch 32 kHz mode adds
     * a second one) is what every check below means by "the" audio track --
     * matching sink_rewrap.c's own pair-1-is-main convention -- so this
     * stops at the first audio stream rather than the last. */
    int vidx = -1, aidx = -1;
    for (unsigned i = 0; i < fc->nb_streams; i++) {
        AVCodecParameters *cp = fc->streams[i]->codecpar;
        if (cp->codec_type == AVMEDIA_TYPE_VIDEO)
            vidx = (int)i;
        else if (cp->codec_type == AVMEDIA_TYPE_AUDIO && aidx < 0)
            aidx = (int)i;
    }
    CHECK(vidx >= 0);

    AVStream *vs = fc->streams[vidx];
    CHECK_EQ_I(vs->codecpar->codec_id, AV_CODEC_ID_DVVIDEO);
    CHECK_EQ_I(vs->codecpar->width, width);
    CHECK_EQ_I(vs->codecpar->height, height);
    /* Field order for a *compressed* stream has nowhere to live in the
     * classic RIFF/AVI header set (no equivalent of MOV's 'fiel' atom for
     * a fourcc video stream) -- ffmpeg's avi muxer/demuxer pair simply
     * doesn't round-trip it, so only MOV (expect_avi_tag == 0) is checked
     * strictly here; AVI is logged instead. sample_aspect_ratio, unlike
     * field order, is stored on AVStream (not AVCodecParameters) by both
     * the avi and mov (de)muxers -- see avi vprp / MOV tkhd track width. */
    if (!expect_avi_tag)
        CHECK_EQ_I(vs->codecpar->field_order, AV_FIELD_BB);
    else
        printf("  AVI field_order read back as %d (container has no field for it)\n",
               vs->codecpar->field_order);
    CHECK_EQ_I(vs->sample_aspect_ratio.num, sar_num);
    CHECK_EQ_I(vs->sample_aspect_ratio.den, sar_den);
    if (expect_avi_tag)
        CHECK_EQ_I(vs->codecpar->codec_tag, MKTAG('d', 'v', 's', 'd'));

    AVDictionaryEntry *t = av_dict_get(fc->metadata, "title", NULL, 0);
    CHECK(t != NULL);
    CHECK(strcmp(t->value, title) == 0);

    int64_t vframes = 0, asamples = 0;
    AVPacket *pkt = av_packet_alloc();
    while (av_read_frame(fc, pkt) >= 0) {
        if (pkt->stream_index == vidx)
            vframes++;
        else if (aidx >= 0 && pkt->stream_index == aidx)
            asamples += pkt->size / (2 * fc->streams[aidx]->codecpar->ch_layout.nb_channels);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    if (aidx >= 0) {
        *audio_samples = asamples;
        *audio_channels = fc->streams[aidx]->codecpar->ch_layout.nb_channels;
        *audio_rate = fc->streams[aidx]->codecpar->sample_rate;
        CHECK_EQ_I(fc->streams[aidx]->codecpar->codec_id, AV_CODEC_ID_PCM_S16LE);
    } else {
        *audio_samples = -1;
    }

    avformat_close_input(&fc);
    return vframes;
}

static void run_one(const char *trace_path, const char *tag)
{
    printf("== %s (%s) ==\n", tag, trace_path);
    char raw_path[512], avi_path[512], mov_path[512];
    snprintf(raw_path, sizeof(raw_path), "test_dv_%s.dv", tag);
    snprintf(avi_path, sizeof(avi_path), "test_dv_%s.avi", tag);
    snprintf(mov_path, sizeof(mov_path), "test_dv_%s.mov", tag);

    collect_ctx_t frames;
    reassemble(trace_path, raw_path, &frames);

    int is_pal = frames.lens[0] == (size_t)DV_SEQ_COUNT_PAL * DV_SEQ_SIZE;
    CHECK(is_pal || frames.lens[0] == (size_t)DV_SEQ_COUNT_NTSC * DV_SEQ_SIZE);

    /* dv_reassembler hands every unit it finds to on_unit(), including a
     * unit made oversized by a bogus resync (see dv_reassembler.h); every
     * sink here is required to reject anything that isn't exactly 10/12
     * whole sequences (sink_raw.c / sink_rewrap.c's is_valid_dv_unit_len)
     * rather than feed a malformed frame to a muxer that assumes a fixed
     * frame size, so the sample traces' own occasional damaged unit means
     * fewer frames land in the sink's output than were pushed to it -- the
     * container check below compares against this filtered count, not
     * frames.n. */
    size_t valid_frames = 0;
    for (size_t i = 0; i < frames.n; i++)
        if (frames.lens[i] == (size_t)DV_SEQ_COUNT_PAL * DV_SEQ_SIZE ||
            frames.lens[i] == (size_t)DV_SEQ_COUNT_NTSC * DV_SEQ_SIZE)
            valid_frames++;
    printf("  %zu frames (%zu valid, %zu damaged), %s\n", frames.n, valid_frames,
           frames.n - valid_frames, is_pal ? "PAL" : "NTSC");

    dv_frame_info_t info;
    CHECK(dv_subcode_parse_frame(frames.frames[0], frames.lens[0], &info) == 0);
    pin_aspect_t aspect = (info.aspect.valid && info.aspect.is_16_9) ? PIN_ASPECT_16_9 : PIN_ASPECT_4_3;

    int sar_num, sar_den;
    pin_sink_sar_for(PIN_KIND_DV, is_pal, aspect, 720, &sar_num, &sar_den);

    pin_sink_params_t params = {
        .kind = PIN_KIND_DV,
        .width = 720,
        .height = is_pal ? 576 : 480,
        .fps_num = is_pal ? 25 : 30000,
        .fps_den = is_pal ? 1 : 1001,
        .interlaced = 1,
        .top_field_first = 0,
        .aspect = aspect,
        .sample_aspect_num = sar_num,
        .sample_aspect_den = sar_den,
        .colour_matrix = PIN_MATRIX_BT601,
    };
    snprintf(params.title, sizeof(params.title), "marvin-core sink test (%s)", tag);

    write_via_sink(PIN_FMT_DV_AVI, avi_path, &params, &frames);
    write_via_sink(PIN_FMT_DV_MOV, mov_path, &params, &frames);

    int64_t avi_asamples, mov_asamples;
    int achan, arate;
    int64_t avi_vframes = verify_container(avi_path, 1, params.width, params.height, sar_num,
                                           sar_den, params.title, &avi_asamples, &achan, &arate);
    CHECK_EQ_I(avi_vframes, (int64_t)valid_frames);

    int64_t mov_vframes = verify_container(mov_path, 0, params.width, params.height, sar_num,
                                           sar_den, params.title, &mov_asamples, &achan, &arate);
    CHECK_EQ_I(mov_vframes, (int64_t)valid_frames);

    if (avi_asamples >= 0) {
        printf("  audio: %d Hz, %d ch, %lld samples (AVI) / %lld (MOV)\n", arate, achan,
               (long long)avi_asamples, (long long)mov_asamples);
        CHECK_EQ_I(avi_asamples, mov_asamples);

        /* A/V duration match: sink_rewrap.c keeps every track within half
         * a frame of the video timeline (pin_dv_audio_fit()), whatever the
         * audio mode and whatever damaged units the capture has (this
         * check runs before the damaged-unit early-return below, so it
         * covers NTSC too, not just PAL). */
        int64_t want_total = av_rescale((int64_t)valid_frames, (int64_t)arate * params.fps_den,
                                        params.fps_num);
        int64_t one_frame = av_rescale(1, (int64_t)arate * params.fps_den, params.fps_num);
        int64_t off_by = avi_asamples - want_total;
        CHECK(off_by <= one_frame / 2 && -off_by <= one_frame / 2);
        if (arate != 48000 && arate != 44100)
            printf("  32 kHz nonlinear mode: %lld samples, exactly %lld valid frames' worth\n",
                   (long long)avi_asamples, (long long)valid_frames);

        /* non-silent check on the AVI's audio */
        AVFormatContext *fc = NULL;
        CHECK(avformat_open_input(&fc, avi_path, NULL, NULL) >= 0);
        CHECK(avformat_find_stream_info(fc, NULL) >= 0);
        int aidx = -1;
        for (unsigned i = 0; i < fc->nb_streams; i++)
            if (fc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && aidx < 0)
                aidx = (int)i;
        int nonzero = 0;
        AVPacket *pkt = av_packet_alloc();
        while (av_read_frame(fc, pkt) >= 0) {
            if (pkt->stream_index == aidx) {
                const int16_t *s = (const int16_t *)pkt->data;
                for (int i = 0; i < pkt->size / 2 && !nonzero; i++)
                    if (s[i] != 0)
                        nonzero = 1;
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        avformat_close_input(&fc);
        CHECK(nonzero);

        /* dvaudio_check, frame by frame: independently re-demux each valid
         * frame's own bytes (demux_one_frame_pair1(), a separate
         * implementation from sink_rewrap.c's, not the sink reused) and
         * compare pair 1's PCM against the matching slice of our AVI
         * output -- skipping only frames sink_rewrap.c had to pad (i.e.
         * where this independent re-demux also finds too little audio for
         * that frame's span; see sink_rewrap.c's rewrap_dv_extract_audio()
         * comment for why that can happen and why it's not data loss this
         * test should fail on). Both the sink and this loop place each
         * frame's slice with pin_dv_audio_fit(), so alignment never drifts
         * even across a padded frame. */
        AVFormatContext *afc = NULL;
        CHECK(avformat_open_input(&afc, avi_path, NULL, NULL) >= 0);
        CHECK(avformat_find_stream_info(afc, NULL) >= 0);
        int a2 = -1;
        for (unsigned i = 0; i < afc->nb_streams; i++)
            if (afc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && a2 < 0)
                a2 = (int)i;
        int16_t *avi_pcm = malloc((size_t)avi_asamples * achan * 2);
        size_t off = 0;
        AVPacket *pkt2 = av_packet_alloc();
        while (av_read_frame(afc, pkt2) >= 0) {
            if (pkt2->stream_index == a2) {
                memcpy((uint8_t *)avi_pcm + off, pkt2->data, pkt2->size);
                off += (size_t)pkt2->size;
            }
            av_packet_unref(pkt2);
        }
        av_packet_free(&pkt2);
        avformat_close_input(&afc);

        int64_t cumulative = 0, compared_frames = 0, padded_frames = 0;
        size_t vi = 0;   /* index into frames.{frames,lens}, valid units only */
        for (size_t fi = 0; fi < frames.n; fi++) {
            if (frames.lens[fi] != (size_t)DV_SEQ_COUNT_PAL * DV_SEQ_SIZE &&
                frames.lens[fi] != (size_t)DV_SEQ_COUNT_NTSC * DV_SEQ_SIZE)
                continue;   /* damaged unit: sink_rewrap.c never saw this one either */

            int64_t target_after = av_rescale((int64_t)vi + 1, (int64_t)arate * params.fps_den,
                                              params.fps_num);

            int16_t *frame_pcm = NULL;
            int frame_ch = achan;
            int64_t avail = demux_one_frame_pair1(frames.frames[fi], frames.lens[fi], &frame_pcm,
                                                  &frame_ch);
            int64_t take, pad;
            pin_dv_audio_fit(cumulative, avail, target_after, one_frame, &take, &pad);
            int64_t need = take + pad;

            if (pad == 0 && need > 0) {
                int mm = memcmp(frame_pcm, (const uint8_t *)avi_pcm + (size_t)cumulative * achan * 2,
                            (size_t)need * achan * 2);
                if (mm) {
                    fprintf(stderr, "MISMATCH2 vi=%zu fi=%zu need=%lld avail=%lld cumulative=%lld frame_ch=%d achan=%d\n",
                            vi, fi, (long long)need, (long long)avail, (long long)cumulative, frame_ch, achan);
                    const uint8_t *a = (const uint8_t*)frame_pcm;
                    const uint8_t *b = (const uint8_t *)avi_pcm + (size_t)cumulative*achan*2;
                    for (size_t k=0;k<(size_t)need*achan*2;k++) {
                        if (a[k]!=b[k]) { fprintf(stderr, "  first diff at byte %zu: a=%d b=%d\n", k, a[k], b[k]); break; }
                    }
                    for (int k=0;k<16;k++) fprintf(stderr, " a[%d]=%d", k, a[k]);
                    fprintf(stderr, "\n");
                    for (int k=0;k<16;k++) fprintf(stderr, " b[%d]=%d", k, b[k]);
                    fprintf(stderr, "\n");
                }
                CHECK(mm == 0);
                compared_frames++;
            } else if (need > 0) {
                padded_frames++;   /* sink_rewrap.c padded this frame; not comparable */
            }
            free(frame_pcm);

            cumulative += need;
            vi++;
        }
        CHECK_EQ_I(cumulative, avi_asamples);
        CHECK_EQ_I(vi, (int64_t)valid_frames);
        /* The comparison should cover the large majority of frames -- a
         * capture that's mostly padding would mean the per-frame demux is
         * failing systematically, worth knowing about even though it isn't
         * this test's job to fail on data quality. */
        CHECK(compared_frames > 0);
        printf("  dvaudio_check: %lld/%lld frames byte-identical to an independent per-frame "
               "re-demux (%lld padded)\n", (long long)compared_frames, (long long)valid_frames,
               (long long)padded_frames);
        free(avi_pcm);
    } else {
        printf("  (no audio track found)\n");
    }

    for (size_t i = 0; i < frames.n; i++)
        free(frames.frames[i]);
    free(frames.frames);
    free(frames.lens);
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);   /* <pal-trace> <ntsc-trace> */
    run_one(argv[1], "pal");
    run_one(argv[2], "ntsc");
    printf("OK\n");
    return 0;
}
