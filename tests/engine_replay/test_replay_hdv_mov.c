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
 * ctest: HDV to MOV end to end through pin_api.h on the replay device, on a
 * tape where only the camera clock jumps: hdv-repair-closed.ts (3 s, clock at
 * 900 s, timecode from 00:20:00:00) then hdv-repair-jump.ts (1.5 s, clock at
 * 5000 s, timecode going on from 00:20:03:00), joined with the continuity
 * counters running on. Captured as HDV MOV *without* scene split asked for,
 * it must give:
 *   - two numbered files: the format always splits (scene_split_forced), and
 *     the clock jump is a break even though the timecode runs on;
 *   - each file decodes without an error, its video from 0 and frame after
 *     frame, its audio as long as its video;
 *   - all the tape's pictures between them (but the one the replay drops at
 *     the end of the tape, and those before the capture began).
 *
 * Usage: test_replay_hdv_mov <hdv-repair-closed.ts> <hdv-repair-jump.ts>
 */

#include "replay_test_util.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include <math.h>

#define TS 188

static uint8_t *read_ts(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    CHECK(size > 0 && size % TS == 0);
    uint8_t *d = malloc((size_t)size);
    CHECK(d && fread(d, 1, (size_t)size, f) == (size_t)size);
    fclose(f);
    *n = (size_t)size / TS;
    return d;
}

/* Frames, errors, first video time, video and audio length (s) of a file. */
static void probe(const char *path, int *frames, int *errors, double *v0, double *vdur, double *adur)
{
    AVFormatContext *fc = NULL;
    CHECK(avformat_open_input(&fc, path, NULL, NULL) >= 0);
    CHECK(avformat_find_stream_info(fc, NULL) >= 0);
    int vi = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int ai = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    CHECK(vi >= 0 && ai >= 0);
    AVStream *vs = fc->streams[vi], *as = fc->streams[ai];
    CHECK_EQ_I(vs->codecpar->codec_tag, MKTAG('h', 'd', 'v', '3'));
    AVCodecContext *cc = avcodec_alloc_context3(avcodec_find_decoder(vs->codecpar->codec_id));
    CHECK(cc && avcodec_parameters_to_context(cc, vs->codecpar) >= 0);
    CHECK(avcodec_open2(cc, cc->codec, NULL) >= 0);
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    *frames = *errors = 0;
    *v0 = -1;
    int64_t last = AV_NOPTS_VALUE, asamples = 0;
    for (int eof = 0; !eof;) {
        if (av_read_frame(fc, pkt) < 0) {
            eof = 1;
            avcodec_send_packet(cc, NULL);
        } else {
            if (pkt->stream_index == vi && avcodec_send_packet(cc, pkt) < 0)
                (*errors)++;
            if (pkt->stream_index == ai)
                asamples += pkt->duration;
            av_packet_unref(pkt);
        }
        while (avcodec_receive_frame(cc, fr) == 0) {
            int64_t t = fr->best_effort_timestamp;
            if (*v0 < 0)
                *v0 = t * av_q2d(vs->time_base);
            if (fr->decode_error_flags || (last != AV_NOPTS_VALUE &&
                                           llround((t - last) * av_q2d(vs->time_base) * 25) != 1))
                (*errors)++;
            last = t;
            (*frames)++;
            av_frame_unref(fr);
        }
    }
    *vdur = *frames / 25.0;
    *adur = asamples * av_q2d(as->time_base);
    av_frame_free(&fr);
    av_packet_free(&pkt);
    avcodec_free_context(&cc);
    avformat_close_input(&fc);
}

/* Plays tape `src` from its start on the replay device and captures it as HDV MOV, files `base`-NNNN.mov
 * (no scene split asked for); *out: the status when the capture has ended. */
static void capture(const char *src, const char *base, pin_status_snapshot_t *out)
{
    pin_session_t *s = pin_test_open_replay(src);
    pin_test_prepare_dv(s);
    pin_status_snapshot_t snap;
    int is_hdv = 0;
    for (int i = 0; i < 100 && !is_hdv; i++) {
        pin_get_status(s, &snap);
        is_hdv = snap.stream_kind == PIN_KIND_HDV;
        if (!is_hdv)
            pin_test_sleep_ms(50);
    }
    CHECK(is_hdv);
    CHECK(pin_deck(s, PIN_DECK_CMD_REW) == PIN_OK);
    int stopped = 0;
    for (int i = 0; i < 300 && !stopped; i++) {
        pin_test_sleep_ms(20);
        pin_get_status(s, &snap);
        stopped = snap.deck == PIN_DECK_STOPPED;
    }
    CHECK(stopped);

    char path[64];
    for (int i = 0; i <= 3; i++) {
        if (i)
            snprintf(path, sizeof(path), "%s-%04d.mov", base, i);
        else
            snprintf(path, sizeof(path), "%s.mov", base);
        remove(path);
    }
    pin_capture_opts_t opts;
    pin_capture_opts_defaults(&opts);
    strncpy(opts.path, base, sizeof(opts.path) - 1);
    opts.format_hdv = PIN_FMT_HDV_MOV;
    opts.scene_split = 0;              /* the format splits all the same */
    CHECK(pin_deck(s, PIN_DECK_CMD_PLAY) == PIN_OK);
    CHECK(pin_capture_start(s, &opts, 1) == PIN_OK);
    pin_test_wait_state(s, PIN_STATE_CAPTURING, PIN_STATE_ERROR, 3000, &snap);
    CHECK(snap.state == PIN_STATE_CAPTURING);
    pin_test_wait_state(s, PIN_STATE_READY, PIN_STATE_ERROR, 30000, &snap);
    CHECK(snap.state == PIN_STATE_READY);
    CHECK_EQ_I(snap.stop_reason, PIN_STOP_END_OF_TAPE);
    *out = snap;
    pin_close(s);

}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    pin_test_require_fixture(argv[1]);
    pin_test_require_fixture(argv[2]);

    /* the tape: both pieces, the continuity counters running on across the join */
    size_t na, nb;
    uint8_t *a = read_ts(argv[1], &na), *b = read_ts(argv[2], &nb);
    uint8_t *tape = malloc((na + nb) * TS);
    CHECK(tape);
    memcpy(tape, a, na * TS);
    memcpy(tape + na * TS, b, nb * TS);
    int last_cc[8192];
    for (int i = 0; i < 8192; i++)
        last_cc[i] = -1;
    for (size_t k = 0; k < na + nb; k++) {
        uint8_t *p = tape + k * TS;
        int pid = ((p[1] & 0x1F) << 8) | p[2];
        if (pid == 0x1FFF)
            continue;
        if (k >= na && last_cc[pid] >= 0)
            p[3] = (uint8_t)((p[3] & 0xF0) | ((last_cc[pid] + ((p[3] & 0x10) ? 1 : 0)) & 15));
        last_cc[pid] = p[3] & 15;
    }
    const char *src = "hdv_mov_tape.ts";
    FILE *f = fopen(src, "wb");
    CHECK(f && fwrite(tape, TS, na + nb, f) == na + nb);
    fclose(f);
    free(a);
    free(b);
    free(tape);

    pin_status_snapshot_t snap;
    capture(src, "hdv_mov", &snap);
    printf("captured: %llu picture(s), %llu with errors\n", (unsigned long long)snap.frames,
           (unsigned long long)snap.frames_error);
    CHECK(snap.frames_error == 0 && snap.frames_dropped == 0);

    CHECK(pin_test_file_exists("hdv_mov-0001.mov"));
    CHECK(pin_test_file_exists("hdv_mov-0002.mov"));
    CHECK(!pin_test_file_exists("hdv_mov-0003.mov"));
    CHECK(!pin_test_file_exists("hdv_mov.mov"));
    int fr1, fr2, e1, e2;
    double t1, t2, vd1, vd2, ad1, ad2;
    probe("hdv_mov-0001.mov", &fr1, &e1, &t1, &vd1, &ad1);
    probe("hdv_mov-0002.mov", &fr2, &e2, &t2, &vd2, &ad2);
    printf("file 1: %d frames from %.3f s, audio %.3f s, %d errors\n", fr1, t1, ad1, e1);
    printf("file 2: %d frames from %.3f s, audio %.3f s, %d errors\n", fr2, t2, ad2, e2);
    CHECK(e1 == 0 && e2 == 0);
    CHECK(t1 >= 0 && t1 < 0.05 && t2 >= 0 && t2 < 0.05);
    CHECK(fabs(ad1 - vd1) < 0.05 && fabs(ad2 - vd2) < 0.05);
    /* the first piece is 75 pictures (the capture may begin a GOP or so in), the second 37
     * (the replay drops the last one) */
    CHECK(fr1 >= 60 && fr1 <= 75);
    CHECK(fr2 >= 35 && fr2 <= 38);
    printf("OK: HDV MOV split at the clock jump without --split, both files clean\n");

    /* the same tape with a dropout in the first recording (40 packets lost): the status the
     * GUIs and the CLI show counts the frames the file got re-encoded or with audio silenced */
    {
        size_t n;
        uint8_t *t = read_ts(src, &n);
        size_t at = na / 2, cut = 40;
        memmove(t + at * TS, t + (at + cut) * TS, (n - at - cut) * TS);
        const char *bad = "hdv_mov_bad_tape.ts";
        FILE *fb = fopen(bad, "wb");
        CHECK(fb && fwrite(t, TS, n - cut, fb) == n - cut);
        fclose(fb);
        free(t);
        capture(bad, "hdv_bad", &snap);
        printf("damaged tape: %llu picture(s), %llu with errors, %llu dropped\n",
               (unsigned long long)snap.frames, (unsigned long long)snap.frames_error,
               (unsigned long long)snap.frames_dropped);
        /* one GOP (10 pictures) re-encoded, maybe a picture lost and repeated, a frame or two
         * with audio silenced */
        CHECK(snap.frames_error >= 10 && snap.frames_error <= 16);
        CHECK(snap.frames_dropped <= 2);
        int fb1, eb1;
        double tb1, vdb1, adb1;
        probe("hdv_bad-0001.mov", &fb1, &eb1, &tb1, &vdb1, &adb1);
        CHECK(eb1 == 0);
        printf("OK: the damaged GOP repaired, and counted as with error\n");
    }
    return 0;
}
