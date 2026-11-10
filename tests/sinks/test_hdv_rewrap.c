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
 * ctest: feeds an HDV capture (hdv.ts, HDV-like TS made with ffmpeg at test
 * time, see tests/data/README.md) through the HDV MOV / MKV sink (sink_hdvmux.c,
 * written live) and checks the result with libavformat: stream codecs,
 * dimensions, field order, SAR, title. Also a 720p capture (hdv-720p.ts,
 * progressive, given as the second argument) and pin_sink_sar_for()'s HDV values.
 * The pushes are fixed, 188-byte-aligned chunks, not pictures: the sink demuxes
 * the stream itself, so where the units are cut must not matter (test_hdv_repair
 * pushes runs of random length, and damages the stream).
 *
 * Skips (prints a message, exits 0) if the capture file isn't present (no
 * ffmpeg to make it), except when CI is set: then it fails.
 */

#include "test_util.h"
#include "../../src/sinks/pin_sink.h"
#include "../../src/sinks/sinks_internal.h"

#include <libavformat/avformat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TS_PACKET 188
#define CHUNK_PACKETS 200                 /* ~37.6 KB per write_unit push */
#define MAX_BYTES (48u * 1024 * 1024)     /* first ~48 MB is plenty for a smoke test */

/* Rewraps the capture into one container and checks it. w, h, fps and interlaced are the
 * capture's format, handed over as the engine would (what the stream says). */
static void run(const char *cap_path, pin_format_t fmt, const char *out_path, int w, int h,
                int fps_num, int fps_den, int interlaced)
{
    FILE *in = fopen(cap_path, "rb");
    CHECK(in);

    pin_sink_params_t params;
    memset(&params, 0, sizeof(params));
    params.kind = PIN_KIND_HDV;
    params.width = w;
    params.height = h;
    params.fps_num = fps_num;
    params.fps_den = fps_den;
    params.interlaced = interlaced;
    params.top_field_first = interlaced;
    params.aspect = PIN_ASPECT_16_9;
    params.colour_matrix = PIN_MATRIX_BT709;
    snprintf(params.title, sizeof(params.title), "marvin-core HDV rewrap test");

    pin_sink_t *s = pin_sink_create(fmt);
    CHECK(s);
    CHECK(s->open(s, out_path, &params) == PIN_OK);

    uint8_t buf[TS_PACKET * CHUNK_PACKETS];
    size_t total = 0;
    size_t n;
    int units = 0;
    while (total < MAX_BYTES && (n = fread(buf, 1, sizeof(buf), in)) > 0) {
        /* every push a whole number of 188-byte TS packets, as the reassembler's units are */
        size_t whole = (n / TS_PACKET) * TS_PACKET;
        if (whole == 0)
            break;
        CHECK(s->write_unit(s, buf, whole) == PIN_OK);
        total += whole;
        units++;
    }
    fclose(in);
    printf("pushed %d unit(s), %zu bytes of TS\n", units, total);
    CHECK(units > 0);

    /* the size the sink reports at close is the file's, trailer (MOV moov, MKV cues) included */
    pin_sink_status_t fst;
    memset(&fst, 0, sizeof(fst));
    s->final_status = &fst;
    CHECK(s->close(s) == PIN_OK);
    {
        FILE *f = fopen(out_path, "rb");
        CHECK(f);
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fclose(f);
        CHECK_EQ_I((int64_t)fst.bytes_written, (int64_t)fsize);
    }

    AVFormatContext *fc = NULL;
    CHECK(avformat_open_input(&fc, out_path, NULL, NULL) >= 0);
    CHECK(avformat_find_stream_info(fc, NULL) >= 0);

    int vidx = -1, aidx = -1;
    for (unsigned i = 0; i < fc->nb_streams; i++) {
        AVCodecParameters *cp = fc->streams[i]->codecpar;
        if (cp->codec_type == AVMEDIA_TYPE_VIDEO)
            vidx = (int)i;
        else if (cp->codec_type == AVMEDIA_TYPE_AUDIO)
            aidx = (int)i;
    }
    CHECK(vidx >= 0);
    /* the capture's own clock starts at ~834 s (as a camera's did; the generated file's
     * does too); the rewrapped file starts at 0 (a B-frame lead-in or an audio/video
     * offset of a fraction of a second is fine) */
    printf("%s: start_time %.3f s, duration %.3f s\n", out_path, (double)fc->start_time / AV_TIME_BASE,
           (double)fc->duration / AV_TIME_BASE);
    CHECK(fc->start_time != AV_NOPTS_VALUE && fc->start_time > -AV_TIME_BASE && fc->start_time < AV_TIME_BASE);
    CHECK(fc->duration > 0 && fc->duration < 3 * AV_TIME_BASE);
    AVStream *vs = fc->streams[vidx];
    CHECK_EQ_I(vs->codecpar->codec_id, AV_CODEC_ID_MPEG2VIDEO);
    CHECK_EQ_I(vs->codecpar->width, w);
    CHECK_EQ_I(vs->codecpar->height, h);
    /* the field order follows the params: 1080i top field first, 720p progressive */
    CHECK_EQ_I(vs->codecpar->field_order, interlaced ? AV_FIELD_TT : AV_FIELD_PROGRESSIVE);
    CHECK_EQ_I(vs->codecpar->color_primaries, AVCOL_PRI_BT709);
    /* 16:9 on either raster: SAR 4:3 at 1440x1080, 1:1 at 1280x720 */
    AVRational sar = av_guess_sample_aspect_ratio(fc, vs, NULL);
    printf("%s: SAR %d:%d\n", out_path, sar.num, sar.den);
    CHECK(sar.num * w * 9 == sar.den * h * 16);   /* DAR = SAR * w / h = 16:9 */

    if (aidx >= 0)
        CHECK_EQ_I(fc->streams[aidx]->codecpar->codec_id, AV_CODEC_ID_MP2);

    AVDictionaryEntry *t = av_dict_get(fc->metadata, "title", NULL, 0);
    CHECK(t && strcmp(t->value, params.title) == 0);

    int64_t vframes = 0;
    AVPacket *pkt = av_packet_alloc();
    while (av_read_frame(fc, pkt) >= 0) {
        if (pkt->stream_index == vidx)
            vframes++;
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&fc);

    printf("OK: %lld MPEG-2 pictures, %s, %dx%d %s, BT.709%s\n", (long long)vframes,
           "codec-copy", w, h, interlaced ? "TT" : "progressive",
           aidx >= 0 ? " + MP2 audio" : " (no audio in this slice)");
    CHECK(vframes > 0);
}

#include "../../src/engine/pin_stop.h"
#include <time.h>

static int ts_payload(const uint8_t *pkt)
{
    int off = 4;
    if (pkt[3] & 0x20)
        off += 1 + pkt[4];
    return off < TS_PACKET ? off : -1;
}

/* A stream without its audio packets. video_only: its PMT says so too (the audio entry's stream
 * type changed to private data, which the demuxer ignores). The sink fails fast unless the
 * stream is video-only by its PMT. cap_path must hold more than 2 s. */
static void run_no_audio(const char *cap_path, int video_only)
{
    FILE *in = fopen(cap_path, "rb");
    CHECK(in);
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    uint8_t *ts = malloc((size_t)size);
    CHECK(ts && fread(ts, 1, (size_t)size, in) == (size_t)size);
    fclose(in);
    size_t n = (size_t)size / TS_PACKET, kept = 0;
    int apid = -1, pmt_pid = -1;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *p = ts + i * TS_PACKET;
        int pid = ((p[1] & 0x1F) << 8) | p[2], off = ts_payload(p);
        if ((p[1] & 0x40) && off > 0 && off + 4 <= TS_PACKET && p[off] == 0 && p[off + 1] == 0 &&
            p[off + 2] == 1 && p[off + 3] >= 0xC0 && p[off + 3] <= 0xDF)
            apid = pid;
        if (pid == 0 && off > 0 && pmt_pid < 0)
            pmt_pid = ((p[off + 1 + 10] & 0x1F) << 8) | p[off + 1 + 11];
    }
    CHECK(apid > 0 && pmt_pid > 0);
    for (size_t i = 0; i < n; i++) {
        uint8_t *p = ts + i * TS_PACKET;
        int pid = ((p[1] & 0x1F) << 8) | p[2];
        if (pid == apid)
            continue;
        if (video_only && pid == pmt_pid) {
            int off = ts_payload(p) + 1;   /* past the pointer field */
            const uint8_t *sec = p + off;
            int slen = ((sec[1] & 0x0F) << 8) | sec[2], pil = ((sec[10] & 0x0F) << 8) | sec[11];
            for (int e = off + 12 + pil; e + 5 <= off + 3 + slen - 4 && e + 5 <= TS_PACKET;) {
                if (p[e] == 0x03 || p[e] == 0x04)
                    p[e] = 0x06;
                e += 5 + (((p[e + 3] & 0x0F) << 8) | p[e + 4]);
            }
        }
        memmove(ts + kept * TS_PACKET, p, TS_PACKET);
        kept++;
    }

    pin_sink_params_t params;
    memset(&params, 0, sizeof(params));
    params.kind = PIN_KIND_HDV;
    params.width = 1440;
    params.height = 1080;
    params.fps_num = 25;
    params.fps_den = 1;
    params.interlaced = params.top_field_first = 1;
    params.aspect = PIN_ASPECT_16_9;
    params.colour_matrix = PIN_MATRIX_BT709;
    const char *out = video_only ? "test_hdv_videoonly.mkv" : "test_hdv_noaudio.mkv";
    remove(out);
    pin_sink_t *s = pin_sink_create(PIN_FMT_HDV_MKV);
    CHECK(s && s->open(s, out, &params) == PIN_OK);
    pin_status_t rc = PIN_OK;
    for (size_t i = 0; i < kept && rc == PIN_OK; i += CHUNK_PACKETS) {
        size_t k = kept - i < CHUNK_PACKETS ? kept - i : CHUNK_PACKETS;
        rc = s->write_unit(s, ts + i * TS_PACKET, k * TS_PACKET);
    }
    /* the decision is made by the sink's decode thread once 2 s of pictures are in */
    pin_sink_status_t st;
    for (int waited = 0; waited < 10000; waited += 20) {
        s->get_status(s, &st);
        if (st.last_error != PIN_OK || video_only)
            break;
        struct timespec ts20 = { 0, 20 * 1000000L };
        nanosleep(&ts20, NULL);
    }
    if (video_only) {
        CHECK_EQ_I(rc, PIN_OK);
        CHECK_EQ_I(s->close(s), PIN_OK);
        AVFormatContext *fc = NULL;
        CHECK(avformat_open_input(&fc, out, NULL, NULL) >= 0);
        CHECK(avformat_find_stream_info(fc, NULL) >= 0);
        int na = 0, nv = 0;
        for (unsigned i = 0; i < fc->nb_streams; i++) {
            na += fc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
            nv += fc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
        }
        avformat_close_input(&fc);
        CHECK(nv == 1 && na == 0);
        printf("OK: a stream that is video-only by its PMT is written without audio\n");
    } else {
        CHECK_EQ_I(st.last_error, PIN_ERR_CODEC);
        CHECK_EQ_I(st.fail_reason, PIN_SINK_FAIL_NO_AUDIO);
        CHECK_EQ_I(s->close(s), PIN_ERR_CODEC);
        FILE *f = fopen(out, "rb");
        CHECK(f == NULL);   /* nothing was written: no file is left */
        if (f)
            fclose(f);
        printf("OK: a stream whose audio is missing fails fast\n");
    }
    free(ts);
}

int main(int argc, char **argv)
{
    CHECK(argc >= 2 && argc <= 4);

    /* the HDV SAR comes from the raster: 1440x1080 16:9 is anamorphic, 1280x720 square */
    int sn, sd;
    pin_sink_sar_for(PIN_KIND_HDV, 1, PIN_ASPECT_16_9, 1440, 1080, &sn, &sd);
    CHECK(sn == 4 && sd == 3);
    pin_sink_sar_for(PIN_KIND_HDV, 0, PIN_ASPECT_16_9, 1280, 720, &sn, &sd);
    CHECK(sn == 1 && sd == 1);
    pin_sink_sar_for(PIN_KIND_HDV, 1, PIN_ASPECT_4_3, 1440, 1080, &sn, &sd);
    CHECK(sn == 1 && sd == 1);

    if (!pin_test_have_fixture(argv[1]))
        return 0;
    run(argv[1], PIN_FMT_HDV_MOV, "test_hdv.mov", 1440, 1080, 25, 1, 1);
    run(argv[1], PIN_FMT_HDV_MKV, "test_hdv.mkv", 1440, 1080, 25, 1, 1);
    /* 720p59.94: progressive, square samples */
    if (argc == 3 && pin_test_have_fixture(argv[2]))
        run(argv[2], PIN_FMT_HDV_MKV, "test_hdv_720p.mkv", 1280, 720, 60000, 1001, 0);
    if (argc == 4 && pin_test_have_fixture(argv[3])) {
        run_no_audio(argv[3], 0);
        run_no_audio(argv[3], 1);
    }
    return 0;
}
