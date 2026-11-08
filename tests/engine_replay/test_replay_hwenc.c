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
 * ctest: the H.264 / HEVC formats through the whole engine on the replay device: listed by
 * pin_formats() once the encoder probe finds an encoder, captured from DV (deinterlaced, and
 * with a caller that passes the pin_capture_opts_t of before the format settings), refused
 * with PIN_ERR_CODEC and a message when the encoder is missing, and the time-left estimate
 * following the quality setting. Linux has no H.264 encoder, so FFV1 stands in for it
 * (PIN_HWENC_ENCODER, tests only) wherever the probe finds none.
 *
 * Argument: the PAL DV recording to replay (dv-pal.dv, made at test time, see
 * tests/data/README.md).
 */

#include "replay_test_util.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#include <math.h>
#include <stddef.h>

static int listed(pin_kind_t kind, pin_format_t f)
{
    pin_format_info_t l[PIN_FMT_COUNT];
    for (int i = 0; i < PIN_FMT_COUNT; i++) {
        memset(&l[i], 0, sizeof(l[i]));
        l[i].size = sizeof(l[i]);
    }
    int n = pin_formats(kind, l, PIN_FMT_COUNT);
    for (int i = 0; i < n; i++)
        if (l[i].format == f)
            return 1;
    return 0;
}

/* Captures to the end of the recording; returns once the session is READY again. */
/* Captures whose status was read while they ran: the replay device keeps the capture time and
 * the bytes on disk up to date, like the hardware loops. */
static int live_checks;

static void capture(pin_session_t *s, const pin_capture_opts_t *o)
{
    CHECK(pin_capture_start(s, o, 1) == PIN_OK);
    pin_status_snapshot_t snap;
    pin_test_wait_state(s, PIN_STATE_CAPTURING, PIN_STATE_ERROR, 5000, &snap);
    CHECK(snap.state == PIN_STATE_CAPTURING);
    pin_test_sleep_ms(700);
    memset(&snap, 0, sizeof(snap));
    snap.size = sizeof(snap);
    CHECK(pin_get_status(s, &snap) == PIN_OK);
    if (snap.state == PIN_STATE_CAPTURING) {   /* not yet at the end of the recording */
        printf("  while capturing: %.2f s, %llu bytes\n", snap.elapsed_s, (unsigned long long)snap.bytes_written);
        CHECK(snap.elapsed_s >= 0.3 && snap.elapsed_s < 5);
        CHECK(snap.bytes_written > 0 && snap.total_bytes_written >= snap.bytes_written);
        live_checks++;
    }
    pin_test_sleep_ms(800);
    CHECK(pin_capture_stop(s) == PIN_OK);
    pin_test_wait_state(s, PIN_STATE_READY, PIN_STATE_ERROR, 10000, &snap);
    CHECK(snap.state == PIN_STATE_READY);
    CHECK(snap.stop_reason == PIN_STOP_USER || snap.stop_reason == PIN_STOP_END_OF_TAPE);
}

/* The file: pictures 1/50 s apart (deinterlaced PAL), FLAC as long as the video, the title.
 * The capture runs to the end of the recording at most, wherever the replay was. */
static void check_file(const char *path, const char *title, int64_t min_pictures)
{
    AVFormatContext *in = NULL;
    CHECK(avformat_open_input(&in, path, NULL, NULL) == 0);
    CHECK(avformat_find_stream_info(in, NULL) >= 0);
    int vi = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int ai = av_find_best_stream(in, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    CHECK(vi >= 0 && ai >= 0);
    CHECK(in->streams[ai]->codecpar->codec_id == AV_CODEC_ID_FLAC);
    CHECK(in->streams[vi]->codecpar->field_order == AV_FIELD_PROGRESSIVE);
    AVDictionaryEntry *t = av_dict_get(in->metadata, "title", NULL, 0);
    CHECK(t && strcmp(t->value, title) == 0);
    const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_FLAC);
    AVCodecContext *dec = avcodec_alloc_context3(c);
    avcodec_parameters_to_context(dec, in->streams[ai]->codecpar);
    CHECK(avcodec_open2(dec, c, NULL) == 0);
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    int64_t pictures = 0, samples = 0, last = 0;
    while (av_read_frame(in, pkt) >= 0) {
        if (pkt->stream_index == vi) {
            pictures++;
            if (pkt->pts > last)
                last = pkt->pts;
        } else if (pkt->stream_index == ai) {
            CHECK(avcodec_send_packet(dec, pkt) == 0);
            while (avcodec_receive_frame(dec, fr) == 0) {
                samples += fr->nb_samples;
                av_frame_unref(fr);
            }
        }
        av_packet_unref(pkt);
    }
    double video_s = (last * av_q2d(in->streams[vi]->time_base)) + 0.02;
    double audio_s = (double)samples / in->streams[ai]->codecpar->sample_rate;
    printf("  %s: %lld pictures, %.3f s of video, %.3f s of audio\n", path, (long long)pictures, video_s, audio_s);
    CHECK(pictures >= min_pictures && pictures % 2 == 0);   /* two per DV frame */
    CHECK(fabs(video_s - pictures * 0.02) < 0.03);          /* 50 per second */
    CHECK(fabs(audio_s - video_s) <= 0.04 + 1e-6);          /* within a DV frame */
    av_frame_free(&fr);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&in);
}

static double est_bph(pin_session_t *s, pin_capture_opts_t *o)
{
    CHECK(pin_set_output_hint(s, o) == PIN_OK);
    pin_status_snapshot_t st;
    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    CHECK(pin_get_status(s, &st) == PIN_OK);
    return st.est_bytes_per_hour;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const char *trace = argv[1];
    pin_test_require_fixture(trace);
    int m = pin_encoders_probe();
    const char *standin = NULL;
    if (!(m & PIN_ENC_H264)) {
        standin = "ffv1";
        pin_test_setenv("PIN_HWENC_ENCODER", standin);
        m = pin_encoders_probe();
    }
    CHECK(m & PIN_ENC_H264);
    CHECK(listed(PIN_KIND_DV, PIN_FMT_DV_H264_MKV));
    printf("replaying %s, encoder %s\n", trace, standin ? "ffv1 (standing in)" : "the system's");

    pin_session_t *s = pin_test_open_replay(trace);
    pin_test_prepare_dv(s);
    pin_test_sleep_ms(300);     /* a frame or two, so the session knows it is DV */

    /* deinterlaced, quality 23 */
    pin_capture_opts_t o;
    pin_capture_opts_defaults(&o);
    snprintf(o.path, sizeof(o.path), "replay_hwenc");
    snprintf(o.title, sizeof(o.title), "replay to H.264");
    o.format_dv = PIN_FMT_DV_H264_MKV;
    o.deinterlace = PIN_DEINTERLACE_DOUBLE_RATE;
    o.video_quality = 23;
    pin_output_check_t chk;
    memset(&chk, 0, sizeof(chk));
    chk.size = sizeof(chk);
    CHECK(pin_check_output(s, &o, &chk) == PIN_OK);
    CHECK(strstr(chk.first_path, "replay_hwenc.mkv") != NULL);
    capture(s, &o);
    check_file("replay_hwenc.mkv", "replay to H.264", 10);

    /* the time left follows the format settings */
    o.video_quality = 18;
    double q18 = est_bph(s, &o);
    o.video_quality = 30;
    double q30 = est_bph(s, &o);
    o.deinterlace = PIN_DEINTERLACE_KEEP_FIELDS;
    double q30f = est_bph(s, &o);
    printf("  estimates: %.2f GB/h at 18, %.2f at 30, %.2f at 30 keeping the fields\n", q18 / 1e9, q30 / 1e9,
           q30f / 1e9);
    CHECK(q18 > 2e9 && q18 < 8e9 && q30 < q18 && q30f < q30);
    o.format_dv = PIN_FMT_DV_RAW;
    CHECK(fabs(est_bph(s, &o) - 13e9) < 1e6);

    /* a caller built before the format settings: its old-size struct gets their defaults
     * (deinterlaced, 18) */
    unsigned char buf[sizeof(pin_capture_opts_t) + 16];
    memset(buf, 0xAA, sizeof(buf));
    pin_capture_opts_t *old = (pin_capture_opts_t *)buf;
    old->size = (uint32_t)offsetof(pin_capture_opts_t, deinterlace);
    pin_capture_opts_defaults(old);
    snprintf(old->path, sizeof(old->path), "replay_hwenc_old");
    snprintf(old->title, sizeof(old->title), "old caller");
    old->format_dv = PIN_FMT_DV_HEVC_MKV;
    capture(s, old);
    check_file("replay_hwenc_old.mkv", "old caller", 2);

    /* an encoder that fails during the capture ends it with an error (PIN_HWENC_FAIL_AFTER is
     * read when the file is opened). A new session: the replay stopped at the end of the
     * recording. */
    pin_close(s);
    s = pin_test_open_replay(trace);
    pin_test_prepare_dv(s);
    pin_test_sleep_ms(300);
    pin_test_setenv("PIN_HWENC_FAIL_AFTER", "4");
    o.format_dv = PIN_FMT_DV_H264_MKV;
    snprintf(o.path, sizeof(o.path), "replay_hwenc_fail");
    CHECK(pin_capture_start(s, &o, 1) == PIN_OK);
    pin_status_snapshot_t fs;
    pin_test_wait_state(s, PIN_STATE_CAPTURING, PIN_STATE_ERROR, 5000, &fs);
    pin_test_wait_state(s, PIN_STATE_READY, PIN_STATE_ERROR, 15000, &fs);
    pin_test_setenv("PIN_HWENC_FAIL_AFTER", "");
    printf("  encoder failing: %s\n", fs.stop_text);
    CHECK(fs.state == PIN_STATE_READY);
    CHECK(fs.stop_reason == PIN_STOP_ERROR && pin_stop_reason_abnormal(fs.stop_reason));
    CHECK(strstr(fs.stop_text, "video encoder failed") != NULL);

    /* writes that fail (a full or pulled drive, PIN_TEST_WRITE_FAIL_AFTER) end the capture with
     * WRITE_ERROR while it runs, not as a normal end */
    pin_close(s);
    s = pin_test_open_replay(trace);
    pin_test_prepare_dv(s);
    pin_test_sleep_ms(300);
    pin_test_setenv("PIN_TEST_WRITE_FAIL_AFTER", "100000");
    snprintf(o.path, sizeof(o.path), "replay_hwenc_wfail");
    CHECK(pin_capture_start(s, &o, 1) == PIN_OK);
    pin_test_wait_state(s, PIN_STATE_CAPTURING, PIN_STATE_ERROR, 5000, &fs);
    pin_test_wait_state(s, PIN_STATE_READY, PIN_STATE_ERROR, 15000, &fs);
    pin_test_setenv("PIN_TEST_WRITE_FAIL_AFTER", "");
    printf("  writes failing: %s\n", fs.stop_text);
    CHECK(fs.state == PIN_STATE_READY);
    CHECK(fs.stop_reason == PIN_STOP_WRITE_ERROR);

    /* no encoder: refused up front, with the reason */
    pin_test_setenv("PIN_HWENC_ENCODER", "no-such-encoder");
    CHECK(pin_encoders_probe() == 0);
    CHECK(!listed(PIN_KIND_DV, PIN_FMT_DV_H264_MKV));
    o.format_dv = PIN_FMT_DV_H264_MKV;
    memset(&chk, 0, sizeof(chk));
    chk.size = sizeof(chk);
    CHECK(pin_check_output(s, &o, &chk) == PIN_ERR_CODEC);
    CHECK(strstr(chk.message, "H.264") != NULL);
    CHECK(pin_capture_start(s, &o, 1) == PIN_ERR_CODEC);
    pin_status_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.size = sizeof(snap);
    pin_get_status(s, &snap);
    CHECK(snap.state == PIN_STATE_READY);
    printf("  refused without an encoder: %s\n", chk.message);

    pin_close(s);
    CHECK(live_checks > 0);
    pin_test_setenv("PIN_HWENC_ENCODER", standin ? standin : "");
    printf("test_replay_hwenc: ok\n");
    return 0;
}
