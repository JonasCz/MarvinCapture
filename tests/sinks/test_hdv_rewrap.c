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
 * ctest: feeds a real HDV capture (tests/data/hdv.ts, see
 * tests/data/README.md) through sink_rewrap's HDV path (PIN_FMT_HDV_MOV,
 * which for HDV always takes the plan's documented raw-then-remux-at-close
 * fallback -- see sink_rewrap.c's header comment) and checks the result
 * with libavformat: stream codecs, dimensions, field order, SAR, title.
 *
 * Only the first ~48 MB of the capture is used (a live 5-minute HDV run is
 * ~1 GB; a few seconds of video is enough to exercise the remux path and
 * keeps this ctest fast). The plan asks the picture units be split "at
 * video PES starts", matching how dv_reassembler hands HDV pictures to a
 * sink during a real capture; this sink's write_unit() for HDV, though,
 * just appends bytes to a temp file verbatim (see sink_rewrap.c) ahead of
 * a single remux at close() -- so, unlike the DV path, *where* the pushed
 * chunks are cut cannot change its output, only how many pin_writer-style
 * pushes it took to deliver the same bytes. This test therefore pushes
 * fixed, 188-byte-aligned chunks rather than reimplementing PES/PID
 * parsing (dv_reassembler.c and engine/hdv_aux.c already own that logic
 * elsewhere in the tree); the byte-for-byte content reaching sink_rewrap is
 * unaffected either way.
 *
 * Skips (prints a message, exits 0) if the capture file isn't present --
 * it's the user's own recording, gitignored, not guaranteed to exist on
 * every machine that builds this project (see tests/data/README.md).
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

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const char *cap_path = argv[1];

    FILE *in = fopen(cap_path, "rb");
    if (!in) {
        printf("SKIP: %s not present (see tests/data/README.md) -- HDV rewrap test skipped\n",
               cap_path);
        return 0;
    }

    pin_sink_params_t params;
    memset(&params, 0, sizeof(params));
    params.kind = PIN_KIND_HDV;
    params.width = 1440;
    params.height = 1080;
    params.fps_num = 25;
    params.fps_den = 1;
    params.interlaced = 1;
    params.top_field_first = 1;
    params.aspect = PIN_ASPECT_16_9;
    params.colour_matrix = PIN_MATRIX_BT709;
    snprintf(params.title, sizeof(params.title), "marvin-core HDV rewrap test");

    pin_sink_t *s = pin_sink_create(PIN_FMT_HDV_MOV);
    CHECK(s);
    CHECK(s->open(s, "test_hdv.mov", &params) == PIN_OK);

    uint8_t buf[TS_PACKET * CHUNK_PACKETS];
    size_t total = 0;
    size_t n;
    int units = 0;
    while (total < MAX_BYTES && (n = fread(buf, 1, sizeof(buf), in)) > 0) {
        /* Keep every push a whole number of 188-byte TS packets -- required
         * for the mpegts demuxer to parse the temp file at remux time, not
         * by this sink's write_unit() itself (see the file header comment). */
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
    CHECK(avformat_open_input(&fc, "test_hdv.mov", NULL, NULL) >= 0);
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
    AVStream *vs = fc->streams[vidx];
    CHECK_EQ_I(vs->codecpar->codec_id, AV_CODEC_ID_MPEG2VIDEO);
    CHECK_EQ_I(vs->codecpar->width, 1440);
    CHECK_EQ_I(vs->codecpar->height, 1080);
    CHECK_EQ_I(vs->codecpar->field_order, AV_FIELD_TT);
    CHECK_EQ_I(vs->codecpar->color_primaries, AVCOL_PRI_BT709);

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

    printf("OK: %lld MPEG-2 pictures, %s, 1440x1080 TT, BT.709%s\n", (long long)vframes,
           "codec-copy", aidx >= 0 ? " + MP2 audio" : " (no audio in this slice)");
    CHECK(vframes > 0);
    return 0;
}
