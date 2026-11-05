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
 * ctest: the output drive gets full, fails or is slow during a DV capture (replay device),
 * through the test seams (pin_session.c's test_seams_read(), pin_sink_fault.c):
 *   - a drive with little room (PIN_TEST_FREE_SPACE, and PIN_TEST_WRITE_FAIL_AFTER at the same
 *     size): the capture stops with PIN_STOP_DISK_FULL while there is room to finish the file:
 *     no write fails, raw DV is whole frames, every one counted; the DV AVI has its index and
 *     libavformat reads every frame;
 *   - writes that fail (PIN_TEST_WRITE_FAIL_AFTER alone, the drive still showing room): the
 *     capture stops with PIN_STOP_WRITE_ERROR at once, the file is closed and libavformat reads
 *     what reached it (an AVI whose index could not be written is read without one);
 *   - a slow drive (PIN_TEST_SINK_DELAY_MS, a small PIN_TEST_WRITER_QUEUE): the queue fills, the
 *     capture stops with PIN_STOP_DISK_SLOW, and the file holds every frame up to the last one
 *     queued, in order, without a hole (compared with the source).
 *
 * Usage: test_replay_disk <dv-pal.dv>
 */

#include "replay_test_util.h"

#include <libavformat/avformat.h>

#define PAL_FRAME 144000

static unsigned char *read_file(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        *size = -1;
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(n > 0 ? (size_t)n : 1);
    CHECK(buf && fread(buf, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    *size = n;
    return buf;
}

static int contains(const unsigned char *buf, long n, const char *needle)
{
    size_t len = strlen(needle);
    for (long i = 0; i + (long)len <= n; i++)
        if (memcmp(buf + i, needle, len) == 0)
            return 1;
    return 0;
}

typedef struct {
    pin_status_snapshot_t snap;
    unsigned long long frames; /* the capture's, from stop_captured_s (snap.frames counts on
                                  after the capture: the replay keeps playing) */
    int closed_status;   /* PIN_EVT_FILE_CLOSED's status, -1 if none came */
} result_t;

/* One capture of the replay file to base.<format's extension>, with the seams in env set
 * ("NAME=value" pairs, NULL-terminated) and cleared again afterwards; it must end on its own. */
static void capture(const char *trace, const char *base, pin_format_t fmt, const char *const *env,
                    result_t *r)
{
    for (int i = 0; env[i]; i += 2)
        pin_test_setenv(env[i], env[i + 1]);
    pin_session_t *s = pin_test_open_replay(trace);
    pin_test_prepare_dv(s);

    pin_capture_opts_t opts;
    pin_capture_opts_defaults(&opts);
    strncpy(opts.path, base, sizeof(opts.path) - 1);
    opts.format_dv = fmt;
    CHECK(pin_capture_start(s, &opts, 1) == PIN_OK);
    pin_test_wait_state(s, PIN_STATE_CAPTURING, PIN_STATE_ERROR, 3000, &r->snap);
    CHECK(r->snap.state == PIN_STATE_CAPTURING);
    /* it ends by itself (the replay file's end is 2 s away at most); no hang while finishing */
    int waited = 0;
    do {
        pin_test_sleep_ms(20);
        waited += 20;
        pin_get_status(s, &r->snap);
    } while (r->snap.stop_reason == PIN_STOP_NONE && waited < 10000);
    pin_test_wait_state(s, PIN_STATE_READY, -1, 10000, &r->snap);
    CHECK(r->snap.state == PIN_STATE_READY);

    r->frames = (unsigned long long)(r->snap.stop_captured_s * 25.0 + 0.5);
    r->closed_status = -1;
    pin_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.size = sizeof(ev);
    while (pin_poll_event(s, &ev)) {
        if (ev.kind == PIN_EVT_FILE_CLOSED)
            r->closed_status = ev.a;
        ev.size = sizeof(ev);
    }
    pin_close(s);
    for (int i = 0; env[i]; i += 2)
        pin_test_setenv(env[i], "");
    printf("%s: stop %d \"%s\", %llu frame(s), %llu byte(s) written, %llu dropped, file closed %d\n",
           base, (int)r->snap.stop_reason, r->snap.stop_text, r->frames,
           (unsigned long long)r->snap.total_bytes_written,
           (unsigned long long)r->snap.write_dropped, r->closed_status);
}

/* A DV file (raw or AVI) as libavformat reads it: video packets, audio seconds, and the
 * frame count in the header (an AVI's is written with its index when the file is finished). */
static long lavf_frames(const char *path, double *audio_s, long long *header_frames)
{
    AVFormatContext *fc = NULL;
    CHECK(avformat_open_input(&fc, path, NULL, NULL) >= 0);
    CHECK(avformat_find_stream_info(fc, NULL) >= 0);
    int vidx = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int aidx = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    CHECK(vidx >= 0 && aidx >= 0);
    long frames = 0;
    int64_t samples = 0;
    AVPacket *pkt = av_packet_alloc();
    while (av_read_frame(fc, pkt) >= 0) {
        if (pkt->stream_index == vidx)
            frames++;
        else if (pkt->stream_index == aidx)
            samples += pkt->size / 4;   /* s16 stereo */
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    *audio_s = (double)samples / fc->streams[aidx]->codecpar->sample_rate;
    *header_frames = fc->streams[vidx]->nb_frames;
    printf("  %s: %ld video frame(s), %.3f s of audio, header says %lld frame(s)\n", path, frames,
           *audio_s, *header_frames);
    avformat_close_input(&fc);
    return frames;
}

/* The drive has little room: DISK_FULL before any write fails, the file finished. */
static void test_disk_full(const char *trace)
{
    /* 1.5 MB beyond the 64 MiB the session keeps free: it stops a second into the capture */
    char room[32];
    snprintf(room, sizeof(room), "%llu", (64ull << 20) + 1500000ull);
    const char *env[] = { "PIN_TEST_FREE_SPACE", room, "PIN_TEST_WRITE_FAIL_AFTER", room, NULL };
    result_t r;

    capture(trace, "disk_full_raw", PIN_FMT_DV_RAW, env, &r);
    CHECK(r.snap.stop_reason == PIN_STOP_DISK_FULL);
    CHECK(strstr(r.snap.stop_text, "almost full") != NULL);
    CHECK(r.closed_status == PIN_OK);
    long n;
    free(read_file("disk_full_raw.dv", &n));
    CHECK(n > 0 && n % PAL_FRAME == 0);
    CHECK((unsigned long long)n == r.frames * PAL_FRAME);
    CHECK((unsigned long long)n == r.snap.total_bytes_written);

    capture(trace, "disk_full_avi", PIN_FMT_DV_AVI, env, &r);
    CHECK(r.snap.stop_reason == PIN_STOP_DISK_FULL);
    CHECK(r.closed_status == PIN_OK);
    unsigned char *buf = read_file("disk_full_avi.avi", &n);
    CHECK(buf && contains(buf, n, "idx1"));   /* the index is there */
    free(buf);
    double audio_s;
    long long header_frames;
    long frames = lavf_frames("disk_full_avi.avi", &audio_s, &header_frames);
    CHECK((unsigned long long)frames == r.frames && header_frames == frames);
    CHECK(audio_s > (frames - 1) * 0.04 && audio_s < (frames + 1) * 0.04);
}

/* Writes fail on a drive that still shows room (an I/O error, a removed drive): WRITE_ERROR at
 * once. What reached the file stays readable: the frames before the failed write, the cut one
 * damaged; an AVI's index cannot be written any more, libavformat reads it without one. */
static void test_write_error(const char *trace)
{
    const char *env[] = { "PIN_TEST_WRITE_FAIL_AFTER", "1000000", NULL };   /* 6.9 frames */
    result_t r;
    double audio_s;
    long long header_frames;

    capture(trace, "write_error_raw", PIN_FMT_DV_RAW, env, &r);
    CHECK(r.snap.stop_reason == PIN_STOP_WRITE_ERROR);
    CHECK(strstr(r.snap.stop_text, "writing the file failed") != NULL);
    CHECK(r.closed_status != PIN_OK && r.closed_status != -1);   /* reported closed, with the error */
    long n;
    free(read_file("write_error_raw.dv", &n));
    CHECK(n == 1000000);   /* all that the drive took */
    CHECK(lavf_frames("write_error_raw.dv", &audio_s, &header_frames) >= 6);

    capture(trace, "write_error_avi", PIN_FMT_DV_AVI, env, &r);
    CHECK(r.snap.stop_reason == PIN_STOP_WRITE_ERROR);
    CHECK(r.closed_status != PIN_OK && r.closed_status != -1);
    CHECK(lavf_frames("write_error_avi.avi", &audio_s, &header_frames) >= 6);
}

/* The drive is slower than the stream: DISK_SLOW, and the file has no hole. */
static void test_disk_slow(const char *trace)
{
    /* a 1 MB queue (6 frames) drained at 5 frames a second fills in about half a second */
    const char *env[] = { "PIN_TEST_SINK_DELAY_MS", "200", "PIN_TEST_WRITER_QUEUE", "1000000", NULL };
    result_t r;
    capture(trace, "disk_slow_raw", PIN_FMT_DV_RAW, env, &r);
    CHECK(r.snap.stop_reason == PIN_STOP_DISK_SLOW);
    CHECK(strstr(r.snap.stop_text, "could not keep up") != NULL);
    CHECK(r.closed_status == PIN_OK);
    CHECK(r.snap.write_dropped > 0);

    long n, src_n;
    unsigned char *out = read_file("disk_slow_raw.dv", &n);
    unsigned char *src = read_file(trace, &src_n);
    CHECK(out && src && n > 0 && n % PAL_FRAME == 0 && src_n % PAL_FRAME == 0);
    CHECK((unsigned long long)n == r.snap.total_bytes_written);
    long frames = n / PAL_FRAME, src_frames = src_n / PAL_FRAME, first = -1;
    for (long k = 0; k < src_frames && first < 0; k++)
        if (memcmp(out, src + k * PAL_FRAME, PAL_FRAME) == 0)
            first = k;
    CHECK(first >= 0);
    for (long i = 0; i < frames; i++)   /* frame after frame of the source: no hole */
        CHECK(memcmp(out + i * PAL_FRAME, src + ((first + i) % src_frames) * PAL_FRAME, PAL_FRAME) == 0);
    CHECK((unsigned long long)frames + r.snap.write_dropped == r.frames);   /* the rest was refused */
    printf("  disk_slow_raw.dv: %ld frame(s) from source frame %ld on, in order\n", frames, first);
    free(out);
    free(src);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    pin_test_require_fixture(argv[1]);
    test_disk_full(argv[1]);
    test_write_error(argv[1]);
    test_disk_slow(argv[1]);
    printf("OK\n");
    return 0;
}
