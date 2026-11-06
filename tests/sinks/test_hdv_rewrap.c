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

    CHECK(s->close(s) == PIN_OK);

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

int main(int argc, char **argv)
{
    CHECK(argc == 2 || argc == 3);

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
    return 0;
}
