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
 * ctest: a DV tape whose format changes, captured end to end through pin_api.h on the
 * replay device (raw EP 0x88 dumps of fixtures back to back, tests/make_fixtures.cmake),
 * from the tape's start to its end, no scene split:
 *   - PAL then NTSC as raw .dv: two files, "name.dv" with every PAL frame and
 *     "name-0002.dv" with the NTSC frames from the first one (the replay drops the frame
 *     still being read at the end of the file), each byte for byte the fixture's, no
 *     frame padded or counted with an error;
 *   - 48 kHz then 32 kHz audio as DV AVI: two AVIs, the second's audio track at 32 kHz
 *     (libavformat reads both, with every frame).
 *
 * Usage: test_replay_mixed_dv <data dir>
 */

#include "replay_test_util.h"

#include <libavformat/avformat.h>

typedef struct {
    unsigned char *data;
    long len;
} file_t;

static file_t read_file(const char *path)
{
    file_t b = { NULL, 0 };
    FILE *f = fopen(path, "rb");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    b.len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b.data = malloc((size_t)b.len + 1);
    CHECK(b.data);
    CHECK(fread(b.data, 1, (size_t)b.len, f) == (size_t)b.len);
    fclose(f);
    return b;
}

/* Plays the tape from its start (the replay winds back in 2 s, then PLAY and the capture
 * start together) and captures it to its end; returns the final status. */
static pin_status_snapshot_t capture_tape(const char *dump, const pin_capture_opts_t *opts)
{
    pin_session_t *s = pin_test_open_replay(dump);
    pin_test_prepare_dv(s);
    pin_status_snapshot_t snap;
    CHECK(pin_deck(s, PIN_DECK_CMD_REW) == PIN_OK);
    int stopped = 0;
    for (int i = 0; i < 300 && !stopped; i++) {
        pin_test_sleep_ms(20);
        pin_get_status(s, &snap);
        stopped = snap.deck == PIN_DECK_STOPPED;
    }
    CHECK(stopped);
    CHECK(pin_deck(s, PIN_DECK_CMD_PLAY) == PIN_OK);
    CHECK(pin_capture_start(s, opts, 1) == PIN_OK);
    pin_test_wait_state(s, PIN_STATE_CAPTURING, PIN_STATE_ERROR, 3000, &snap);
    CHECK(snap.state == PIN_STATE_CAPTURING);
    pin_test_wait_state(s, PIN_STATE_READY, PIN_STATE_ERROR, 20000, &snap);
    CHECK(snap.state == PIN_STATE_READY);
    CHECK_EQ_I(snap.stop_reason, PIN_STOP_END_OF_TAPE);
    printf("captured %llu frame(s), %llu with errors; last file %s\n", (unsigned long long)snap.frames,
           (unsigned long long)snap.frames_error, snap.current_file);
    pin_close(s);
    return snap;
}

/* video frames and the audio sample rate of an AVI, read with libavformat */
static void probe(const char *path, int *frames, int *rate, int *height)
{
    AVFormatContext *fc = NULL;
    CHECK(avformat_open_input(&fc, path, NULL, NULL) >= 0);
    CHECK(avformat_find_stream_info(fc, NULL) >= 0);
    int v = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int a = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    CHECK(v >= 0 && a >= 0);
    *rate = fc->streams[a]->codecpar->sample_rate;
    *height = fc->streams[v]->codecpar->height;
    AVPacket *pkt = av_packet_alloc();
    *frames = 0;
    while (av_read_frame(fc, pkt) >= 0) {
        if (pkt->stream_index == v)
            (*frames)++;
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&fc);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    char pal_ntsc[4096], audio[4096], pal_path[4096], ntsc_path[4096];
    snprintf(pal_ntsc, sizeof(pal_ntsc), "%s/ep88-pal-ntsc.bin", argv[1]);
    snprintf(audio, sizeof(audio), "%s/ep88-pal-48k-32k.bin", argv[1]);
    snprintf(pal_path, sizeof(pal_path), "%s/dv-pal.dv", argv[1]);
    snprintf(ntsc_path, sizeof(ntsc_path), "%s/dv-ntsc.dv", argv[1]);
    pin_test_require_fixture(pal_ntsc);
    pin_test_require_fixture(audio);

    /* 1. PAL -> NTSC, raw DV: a new file at the first NTSC frame */
    remove("mixed_std.dv");
    remove("mixed_std-0002.dv");
    remove("mixed_std-0003.dv");
    pin_capture_opts_t opts;
    pin_capture_opts_defaults(&opts);
    strncpy(opts.path, "mixed_std", sizeof(opts.path) - 1);
    opts.format_dv = PIN_FMT_DV_RAW;
    pin_status_snapshot_t snap = capture_tape(pal_ntsc, &opts);
    CHECK(snap.frames_error == 0);                       /* no frame padded to the wrong size */
    CHECK(strcmp(snap.current_file, "mixed_std-0002.dv") == 0);
    CHECK(!pin_test_file_exists("mixed_std-0003.dv"));
    file_t pal = read_file(pal_path), ntsc = read_file(ntsc_path);
    file_t f1 = read_file("mixed_std.dv"), f2 = read_file("mixed_std-0002.dv");
    printf("files: %ld PAL frame(s), %ld NTSC frame(s)\n", f1.len / 144000, f2.len / 120000);
    CHECK(f1.len == pal.len && !memcmp(f1.data, pal.data, (size_t)pal.len));     /* every PAL frame */
    CHECK(f2.len == ntsc.len - 120000 && !memcmp(f2.data, ntsc.data, (size_t)f2.len));
    printf("OK: PAL -> NTSC gives two raw DV files, each frame at its own size\n");

    /* 2. 48 kHz -> 32 kHz, DV AVI: the audio track is fixed at open, so a new file */
    remove("mixed_audio.avi");
    remove("mixed_audio-0002.avi");
    remove("mixed_audio-0003.avi");
    pin_capture_opts_defaults(&opts);
    strncpy(opts.path, "mixed_audio", sizeof(opts.path) - 1);
    opts.format_dv = PIN_FMT_DV_AVI;
    snap = capture_tape(audio, &opts);
    CHECK(snap.frames_error == 0);
    CHECK(pin_test_file_exists("mixed_audio.avi") && pin_test_file_exists("mixed_audio-0002.avi"));
    CHECK(!pin_test_file_exists("mixed_audio-0003.avi"));
    int n1, r1, h1, n2, r2, h2;
    probe("mixed_audio.avi", &n1, &r1, &h1);
    probe("mixed_audio-0002.avi", &n2, &r2, &h2);
    printf("AVIs: %d frames at %d Hz, %d frames at %d Hz\n", n1, r1, n2, r2);
    CHECK(n1 == 50 && r1 == 48000 && h1 == 576);
    CHECK(n2 == 13 && r2 == 32000 && h2 == 576);
    printf("OK: 48 -> 32 kHz gives two DV AVIs with their own audio rate\n");

    free(pal.data);
    free(ntsc.data);
    free(f1.data);
    free(f2.data);
    return 0;
}
