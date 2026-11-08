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
 * ctest: a raw DV capture from an EP 0x88 dump that loses data in transit (built here from
 * the frames of dv-ntsc.dv with tests/ep88_synth.h): frames 4-6 whole, the tail (sequences
 * 7-9) of frame 10 and the first sequence of frame 11. The status must show
 *   - the 3 lost frames as dropped (frames_dropped, clip_frames_dropped) and lost_blocks,
 *   - frames 10 and 11 as the only frames with error (two frames, not one merged),
 * and the file must start with the first frame of the tape (the unit that makes the stream
 * kind known is the first one written), skip the lost frames, and hold frames 10 and 11 with
 * their empty sequences.
 *
 * Usage: test_replay_dv_loss <dv-ntsc.dv>
 */

#include "replay_test_util.h"
#include "../ep88_synth.h"

#define FRAME 120000
#define SEQ 12000
#define TAPE_FRAMES 150

static unsigned char *read_whole_file(const char *path, long *out_len)
{
    FILE *f = fopen(path, "rb");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n);
    CHECK(buf && fread(buf, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    *out_len = n;
    return buf;
}

/* the sequences of frame `f` of the tape that do not arrive */
static unsigned lost_mask(int f)
{
    if (f >= 4 && f <= 6)
        return 0x3FF;
    if (f == 10)
        return 0x380;
    if (f == 11)
        return 0x001;
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    pin_test_require_fixture(argv[1]);

    long src_len = 0;
    unsigned char *src = read_whole_file(argv[1], &src_len);
    CHECK(src_len % FRAME == 0 && (src[3] & 0x80) == 0); /* NTSC */
    long src_frames = src_len / FRAME;

    ep88_synth_t w;
    memset(&w, 0, sizeof(w));
    for (int f = 0; f < TAPE_FRAMES; f++)
        for (int d = 0; d < 10; d++)
            ep88_sequence(&w, src + (size_t)(f % src_frames) * FRAME + (size_t)d * SEQ,
                          (lost_mask(f) >> d) & 1);
    FILE *tf = fopen("dv_loss_test.bin", "wb");
    CHECK(tf && fwrite(w.data, 1, w.len, tf) == w.len);
    fclose(tf);
    free(w.data);

    /* The capture is asked for before the first frame arrives: the frame that makes the
     * stream kind known is the one that opens the file. */
    pin_test_setenv("PIN_REPLAY_HOLD_MS", "400");
    pin_session_t *s = pin_test_open_replay("dv_loss_test.bin");
    pin_test_prepare_dv(s);
    pin_capture_opts_t opts;
    pin_capture_opts_defaults(&opts);
    strncpy(opts.path, "dv_loss_test_cap", sizeof(opts.path) - 1);
    opts.format_dv = PIN_FMT_DV_RAW;
    CHECK(pin_capture_start(s, &opts, 1) == PIN_OK);

    pin_status_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.size = sizeof(snap);
    for (int waited = 0; waited < 8000; waited += 20) {
        pin_get_status(s, &snap);
        if (snap.state == PIN_STATE_ERROR || snap.clip_frames >= 24)
            break;
        pin_test_sleep_ms(20);
    }
    CHECK(snap.state == PIN_STATE_CAPTURING);
    CHECK(pin_capture_stop(s) == PIN_OK);
    pin_test_wait_state(s, PIN_STATE_READY, -1, 3000, &snap);
    printf("DV stats: frames=%llu error=%llu dropped=%llu lost_blocks=%llu clip=%llu/%llu/%llu\n",
           (unsigned long long)snap.frames, (unsigned long long)snap.frames_error,
           (unsigned long long)snap.frames_dropped, (unsigned long long)snap.lost_blocks,
           (unsigned long long)snap.clip_frames, (unsigned long long)snap.clip_frames_error,
           (unsigned long long)snap.clip_frames_dropped);
    CHECK_EQ_I(snap.frames_dropped, 3);
    CHECK_EQ_I(snap.clip_frames_dropped, 3);
    CHECK_EQ_I(snap.frames_error, 2);
    CHECK_EQ_I(snap.clip_frames_error, 2);
    CHECK(snap.lost_blocks > 0);
    pin_close(s);

    long cap_len = 0;
    unsigned char *cap = read_whole_file("dv_loss_test_cap.dv", &cap_len);
    CHECK(cap_len % FRAME == 0);
    long n = cap_len / FRAME;
    CHECK(n >= 20);
    for (long k = 0; k < n; k++) {
        /* the frame of the tape that file frame k is */
        long f = k < 4 ? k : k < 7 ? k + 3 : k + 3;
        const unsigned char *got = cap + k * FRAME;
        const unsigned char *want = src + (size_t)(f % src_frames) * FRAME;
        for (int d = 0; d < 10; d++) {
            int empty = (f == 10 && d >= 7) || (f == 11 && d == 0);
            if (empty) {
                for (int i = 0; i < SEQ; i++)
                    CHECK(got[(size_t)d * SEQ + i] == 0);
            } else if (memcmp(got + (size_t)d * SEQ, want + (size_t)d * SEQ, SEQ) != 0) {
                fprintf(stderr, "file frame %ld sequence %d differs from tape frame %ld\n", k, d, f);
                exit(1);
            }
        }
    }
    CHECK(snap.total_bytes_written == (unsigned long long)cap_len);
    printf("OK: %ld frames in the file, first frame written, 3 lost, 2 with error\n", n);
    free(src);
    free(cap);
    return 0;
}
