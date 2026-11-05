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
 * ctest: a DV / HDV capture onto a FAT32 drive goes on in a new file before a file would pass
 * the drive's file size limit (4 GiB). The seam PIN_TEST_FAT32_LIMIT makes the session take the
 * output for FAT32 with a 2 MB limit, and PIN_TEST_WRITE_FAIL_AFTER makes every file refuse
 * what goes beyond it, as FAT32 would. The capture (replay, 30 frames) must not fail: the
 * files are name, name-0002, name-0003, ... each under the limit and finished:
 *   - raw DV: whole frames that, file after file, are the source's frames in order: none lost
 *     or doubled at a cut;
 *   - DV AVI: each with its index (libavformat reads every frame, the header counts them all)
 *     and its audio;
 *   - HDV (.m2t): each starting with the PAT and PMT, then a GOP, so it decodes from its start;
 *     every byte written is in a file.
 * And pin_check_output() says so before the capture.
 *
 * Usage: test_replay_fat32 <dv-pal.dv> <hdv.ts>
 */

#include "replay_test_util.h"

#include <libavformat/avformat.h>

#define PAL_FRAME 144000
#define LIMIT 2000000

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

/* The k-th file of a capture to base: base.ext, then base-0002.ext, ... */
static void nth_file(const char *base, const char *ext, int k, char *out, size_t cap)
{
    if (k == 1)
        snprintf(out, cap, "%s.%s", base, ext);
    else
        snprintf(out, cap, "%s-%04d.%s", base, k, ext);
}

/* Captures 30 frames of the replay file to base (stopped, or the file's end); returns the
 * snapshot. */
static void capture(const char *trace, const char *base, pin_format_t dv_fmt,
                    pin_status_snapshot_t *snap)
{
    pin_test_setenv("PIN_TEST_FAT32_LIMIT", "2000000");
    pin_test_setenv("PIN_TEST_WRITE_FAIL_AFTER", "2000000");
    pin_session_t *s = pin_test_open_replay(trace);
    pin_test_prepare_dv(s);
    pin_capture_opts_t opts;
    pin_capture_opts_defaults(&opts);
    strncpy(opts.path, base, sizeof(opts.path) - 1);
    opts.format_dv = dv_fmt;
    opts.format_hdv = PIN_FMT_HDV_TS;
    CHECK(pin_capture_start(s, &opts, 1) == PIN_OK);
    pin_test_wait_state(s, PIN_STATE_CAPTURING, PIN_STATE_ERROR, 3000, snap);
    CHECK(snap->state == PIN_STATE_CAPTURING);
    int waited = 0;
    do {
        pin_test_sleep_ms(20);
        waited += 20;
        pin_get_status(s, snap);
    } while (snap->stop_reason == PIN_STOP_NONE && snap->frames < 30 && waited < 10000);
    if (snap->stop_reason == PIN_STOP_NONE)
        CHECK(pin_capture_stop(s) == PIN_OK);
    pin_test_wait_state(s, PIN_STATE_READY, -1, 10000, snap);
    CHECK(snap->state == PIN_STATE_READY);
    pin_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.size = sizeof(ev);
    while (pin_poll_event(s, &ev)) {
        if (ev.kind == PIN_EVT_FILE_CLOSED)
            CHECK(ev.a == PIN_OK);   /* no write failed */
        ev.size = sizeof(ev);
    }
    pin_close(s);
    pin_test_setenv("PIN_TEST_FAT32_LIMIT", "");
    pin_test_setenv("PIN_TEST_WRITE_FAIL_AFTER", "");
    printf("%s: stop %d \"%s\", %llu byte(s) written\n", base, (int)snap->stop_reason,
           snap->stop_text, (unsigned long long)snap->total_bytes_written);
    CHECK(snap->stop_reason == PIN_STOP_USER || snap->stop_reason == PIN_STOP_END_OF_TAPE);
}

/* Video packets of one file as libavformat reads it, and its header's frame count. */
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
            samples += pkt->size / 4;
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    if (audio_s)
        *audio_s = (double)samples / fc->streams[aidx]->codecpar->sample_rate;
    if (header_frames)
        *header_frames = fc->streams[vidx]->nb_frames;
    avformat_close_input(&fc);
    return frames;
}

/* The output check says what will happen on FAT32 (the GUIs show it before the capture). */
static void test_check_output(const char *trace)
{
    pin_test_setenv("PIN_TEST_FAT32_LIMIT", "2000000");
    pin_session_t *s = pin_test_open_replay(trace);
    pin_test_prepare_dv(s);
    pin_test_sleep_ms(200);   /* a frame or two, so the stream is known to be DV */
    pin_capture_opts_t opts;
    pin_capture_opts_defaults(&opts);
    strncpy(opts.path, "fat32_check", sizeof(opts.path) - 1);
    pin_output_check_t chk;
    memset(&chk, 0, sizeof(chk));
    chk.size = sizeof(chk);
    CHECK(pin_check_output(s, &opts, &chk) == PIN_OK);
    printf("check: fat32 %d, \"%s\"\n", chk.fat32, chk.message);
    CHECK(chk.fat32 && strstr(chk.message, "FAT32") && strstr(chk.message, "new file"));
    pin_close(s);
    pin_test_setenv("PIN_TEST_FAT32_LIMIT", "");
}

static void test_dv_raw(const char *trace)
{
    pin_status_snapshot_t snap;
    capture(trace, "fat32_raw", PIN_FMT_DV_RAW, &snap);
    long src_n;
    unsigned char *src = read_file(trace, &src_n);
    CHECK(src && src_n % PAL_FRAME == 0);
    long src_frames = src_n / PAL_FRAME, next = -1, total = 0;
    int files = 0;
    for (int k = 1;; k++) {
        char path[64];
        nth_file("fat32_raw", "dv", k, path, sizeof(path));
        long n;
        unsigned char *buf = read_file(path, &n);
        if (!buf)
            break;
        files++;
        CHECK(n > 0 && n <= LIMIT && n % PAL_FRAME == 0);
        for (long i = 0; i < n / PAL_FRAME; i++) {
            const unsigned char *fr = buf + i * PAL_FRAME;
            if (next < 0)   /* the first frame captured: find it in the source */
                for (long j = 0; j < src_frames && next < 0; j++)
                    if (memcmp(fr, src + j * PAL_FRAME, PAL_FRAME) == 0)
                        next = j;
            CHECK(next >= 0 && memcmp(fr, src + (next % src_frames) * PAL_FRAME, PAL_FRAME) == 0);
            next++;
        }
        total += n;
        printf("  %s: %ld frame(s)\n", path, n / PAL_FRAME);
        free(buf);
    }
    free(src);
    CHECK(files >= 3);   /* 13 frames each */
    CHECK((unsigned long long)total == snap.total_bytes_written);
    CHECK(total / PAL_FRAME == (long)(snap.stop_captured_s * 25.0 + 0.5));
}

static void test_dv_avi(const char *trace)
{
    pin_status_snapshot_t snap;
    capture(trace, "fat32_avi", PIN_FMT_DV_AVI, &snap);
    long total = 0;
    int files = 0;
    for (int k = 1;; k++) {
        char path[64];
        nth_file("fat32_avi", "avi", k, path, sizeof(path));
        long n;
        unsigned char *buf = read_file(path, &n);
        if (!buf)
            break;
        free(buf);
        files++;
        CHECK(n > 0 && n <= LIMIT);
        double audio_s;
        long long header_frames;
        long frames = lavf_frames(path, &audio_s, &header_frames);
        printf("  %s: %ld bytes, %ld frame(s), %.3f s of audio\n", path, n, frames, audio_s);
        CHECK(frames > 0 && header_frames == frames);   /* finished: its index counts them all */
        CHECK(audio_s > (frames - 1) * 0.04 && audio_s < (frames + 1) * 0.04);
        total += frames;
    }
    CHECK(files >= 3);
    CHECK(total == (long)(snap.stop_captured_s * 25.0 + 0.5));
}

static void test_hdv(const char *trace)
{
    pin_status_snapshot_t snap;
    capture(trace, "fat32_hdv", PIN_FMT_DV_RAW, &snap);
    long total = 0, pictures = 0;
    int files = 0;
    for (int k = 1;; k++) {
        char path[64];
        nth_file("fat32_hdv", "m2t", k, path, sizeof(path));
        long n;
        unsigned char *buf = read_file(path, &n);
        if (!buf)
            break;
        files++;
        CHECK(n > 0 && n <= LIMIT && n % 188 == 0);
        /* the PAT first (PID 0), so it decodes from its start */
        CHECK(buf[0] == 0x47 && (buf[1] & 0x1F) == 0 && buf[2] == 0);
        free(buf);
        long p = lavf_frames(path, NULL, NULL);
        printf("  %s: %ld bytes, %ld picture(s)\n", path, n, p);
        CHECK(p > 0);
        total += n;
        pictures += p;
    }
    CHECK(files >= 2);
    CHECK((unsigned long long)total == snap.total_bytes_written);
    /* every picture from the first GOP on (12 pictures each) */
    long captured = (long)(snap.stop_captured_s * 25.0 + 0.5);
    CHECK(pictures <= captured && pictures > captured - 12);
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    pin_test_require_fixture(argv[1]);
    pin_test_require_fixture(argv[2]);
    test_check_output(argv[1]);
    test_dv_raw(argv[1]);
    test_dv_avi(argv[1]);
    test_hdv(argv[2]);
    printf("OK\n");
    return 0;
}
