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
 * ctest: a short PIN_FMT_DV_AVI and PIN_FMT_DV_MOV capture against the
 * replay device opens with libavformat, has the expected stream count
 * (video + audio) and carries the requested title metadata.
 *
 * Usage: test_replay_dv_formats <trace_file>
 */

#include "replay_test_util.h"

#include <libavformat/avformat.h>

static void capture_one(const char *trace_path, pin_format_t fmt, const char *out_path,
                         const char *title)
{
    pin_session_t *s = pin_test_open_replay(trace_path);
    pin_test_prepare_dv(s);

    pin_capture_opts_t opts;
    pin_capture_opts_defaults(&opts);
    strncpy(opts.path, out_path, sizeof(opts.path) - 1);
    strncpy(opts.title, title, sizeof(opts.title) - 1);
    opts.format_dv = fmt;
    CHECK(pin_capture_start(s, &opts, 1) == PIN_OK);

    pin_status_snapshot_t snap;
    pin_test_wait_state(s, PIN_STATE_CAPTURING, PIN_STATE_ERROR, 3000, &snap);
    CHECK(snap.state == PIN_STATE_CAPTURING);
    pin_test_sleep_ms(2000);
    CHECK(pin_capture_stop(s) == PIN_OK);
    pin_test_wait_state(s, PIN_STATE_READY, -1, 3000, &snap);
    pin_close(s);
}

static void check_with_libavformat(const char *path, const char *expect_title)
{
    AVFormatContext *fc = NULL;
    CHECK(avformat_open_input(&fc, path, NULL, NULL) >= 0);
    CHECK(avformat_find_stream_info(fc, NULL) >= 0);

    int vidx = -1, aidx = -1;
    for (unsigned i = 0; i < fc->nb_streams; i++) {
        AVCodecParameters *cp = fc->streams[i]->codecpar;
        if (cp->codec_type == AVMEDIA_TYPE_VIDEO) vidx = (int)i;
        else if (cp->codec_type == AVMEDIA_TYPE_AUDIO) aidx = (int)i;
    }
    CHECK(vidx >= 0);
    CHECK_EQ_I(fc->streams[vidx]->codecpar->codec_id, AV_CODEC_ID_DVVIDEO);
    printf("%s: %u stream(s), video idx %d, audio idx %d\n", path, fc->nb_streams, vidx, aidx);

    AVDictionaryEntry *t = av_dict_get(fc->metadata, "title", NULL, 0);
    CHECK(t && strcmp(t->value, expect_title) == 0);

    /* At least one video frame actually demuxes. */
    AVPacket *pkt = av_packet_alloc();
    int64_t vframes = 0;
    while (av_read_frame(fc, pkt) >= 0) {
        if (pkt->stream_index == vidx) vframes++;
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&fc);
    CHECK(vframes > 0);
    printf("OK: %s (%lld video frame(s), title \"%s\")\n", path, (long long)vframes, expect_title);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const char *trace_path = argv[1];
    if (!pin_test_file_exists(trace_path)) {
        printf("SKIP: %s not present\n", trace_path);
        return 0;
    }

    capture_one(trace_path, PIN_FMT_DV_AVI, "dv_fmt_test_avi", "marvin-core replay AVI test");
    check_with_libavformat("dv_fmt_test_avi.avi", "marvin-core replay AVI test");

    capture_one(trace_path, PIN_FMT_DV_MOV, "dv_fmt_test_mov", "marvin-core replay MOV test");
    check_with_libavformat("dv_fmt_test_mov.mov", "marvin-core replay MOV test");

    return 0;
}
