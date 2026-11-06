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
 * ctest: a tape with DV, then HDV, then DV again (ep88-dv-hdv-dv.bin: PAL DV 14
 * frames, HDV 1080i 50 pictures, NTSC DV 30 frames, as one EP 0x88 stream), captured
 * end to end through pin_api.h on the replay device from its start to its end:
 *   - to files (raw DV / raw HDV, no scene split): "name.dv" with every PAL frame,
 *     "name-0002.m2t" starting with PAT, PMT and a picture with a sequence header,
 *     "name-0003.dv" with the NTSC frames (the replay drops the frame still being read
 *     at the end of the file);
 *   - to standard output ("-"): a pipe carries one format, so the capture ends at the
 *     change with PIN_STOP_ERROR and says why; what went out is every PAL frame;
 *   - as DV AVI with aspect 16:9, aspect_hdv 4:3 and a title: each file gets its own kind's
 *     aspect and the title where its format has one, so both AVIs (the one opened after the HDV
 *     file too) read back (libavformat) as 16:9 with the title, and the raw .m2t has it as its
 *     DVB service name (an SDT packet behind the PAT and PMT, before the first GOP).
 * And a tape that starts on HDV (ep88-hdv-dv.bin: HDV 1080i 50 pictures, then PAL DV 50 frames),
 * the same options: the raw .m2t with the title as its service name, then a DV AVI that is 16:9
 * (aspect, not aspect_hdv) with the title. Without a title (1.) nothing is added: PAT, PMT, GOP.
 *
 * Usage: test_replay_mixed_kind <data dir>
 */

#include "replay_test_util.h"

#include <libavformat/avformat.h>
#include <libavutil/rational.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#define DUP _dup
#define DUP2 _dup2
#define CLOSE _close
#define OPEN_NEW(path) _open(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, 0600)
#else
#include <fcntl.h>
#include <unistd.h>
#define DUP dup
#define DUP2 dup2
#define CLOSE close
#define OPEN_NEW(path) open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600)
#endif

#define TS 188

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

static int pkt_pid(const unsigned char *p) { return ((p[1] & 0x1F) << 8) | p[2]; }

/* a video PES start whose payload carries a sequence header */
static int gop_start(const unsigned char *p)
{
    if (!(p[1] & 0x40) || !(p[3] & 0x10))
        return 0;
    size_t po = (p[3] & 0x20) ? 5u + p[4] : 4u;
    if (po + 4 > TS || p[po] || p[po + 1] || p[po + 2] != 1 || (p[po + 3] & 0xF0) != 0xE0)
        return 0;
    for (size_t i = po; i + 4 <= TS; i++)
        if (!p[i] && !p[i + 1] && p[i + 2] == 1 && p[i + 3] == 0xB3)
            return 1;
    return 0;
}

/* Plays the tape from its start (the replay winds back in 2 s, then PLAY and the capture
 * start together) and waits for the capture to end; returns the final status. */
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
    pin_close(s);
    return snap;
}

/* What libavformat reads from a file: the video's display aspect as the container stores it
 * (the stream's sample aspect ratio times its size), its height, its video frames and the title. */
typedef struct {
    int dar_num, dar_den, height, frames;
    char title[256];
} probed_t;

static probed_t probe(const char *path)
{
    probed_t r;
    memset(&r, 0, sizeof(r));
    AVFormatContext *fc = NULL;
    CHECK(avformat_open_input(&fc, path, NULL, NULL) >= 0);
    CHECK(avformat_find_stream_info(fc, NULL) >= 0);
    int v = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    CHECK(v >= 0);
    const AVStream *st = fc->streams[v];
    AVRational sar = st->sample_aspect_ratio;   /* the container's (AVI vprp, MKV display size) */
    CHECK(sar.num > 0 && sar.den > 0);
    r.height = st->codecpar->height;
    av_reduce(&r.dar_num, &r.dar_den, (int64_t)sar.num * st->codecpar->width,
              (int64_t)sar.den * st->codecpar->height, 1 << 20);
    AVDictionaryEntry *t = av_dict_get(fc->metadata, "title", NULL, 0);
    snprintf(r.title, sizeof(r.title), "%s", t ? t->value : "");
    AVPacket *pkt = av_packet_alloc();
    while (av_read_frame(fc, pkt) >= 0) {
        if (pkt->stream_index == v)
            r.frames++;
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&fc);
    printf("%s: %d frame(s), %d lines, display aspect %d:%d, title \"%s\"\n", path, r.frames, r.height,
           r.dar_num, r.dar_den, r.title);
    return r;
}

/* A raw HDV file with a title: PAT, PMT, the title's SDT (PID 0x11, table_id 0x42), then the
 * first GOP; libavformat reads the title back as the program's service_name, as ffprobe shows it. */
static void check_ts_title(const char *path, const char *title)
{
    file_t m = read_file(path);
    CHECK(m.len > 4 * TS && m.len % TS == 0);
    CHECK(pkt_pid(m.data) == 0 && pkt_pid(m.data + TS) == 0x81);                     /* PAT, PMT */
    CHECK(pkt_pid(m.data + 2 * TS) == 0x11 && m.data[2 * TS + 5] == 0x42);            /* the SDT */
    CHECK(pkt_pid(m.data + 3 * TS) == 0x810 && gop_start(m.data + 3 * TS));          /* a GOP */
    int sdts = 0;
    for (long i = 0; i < m.len; i += TS)
        sdts += pkt_pid(m.data + i) == 0x11;
    CHECK_EQ_I(sdts, 1);
    free(m.data);
    AVFormatContext *fc = NULL;
    CHECK(avformat_open_input(&fc, path, NULL, NULL) >= 0);
    CHECK(avformat_find_stream_info(fc, NULL) >= 0);
    CHECK(fc->nb_programs >= 1);
    AVDictionaryEntry *n = av_dict_get(fc->programs[0]->metadata, "service_name", NULL, 0);
    AVDictionaryEntry *p = av_dict_get(fc->programs[0]->metadata, "service_provider", NULL, 0);
    printf("%s: service_name \"%s\", service_provider \"%s\"\n", path, n ? n->value : "",
           p ? p->value : "");
    CHECK(n && strcmp(n->value, title) == 0);
    CHECK(p && strcmp(p->value, "MarvinCapture") == 0);
    avformat_close_input(&fc);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    char dump[4096], pal_path[4096], ntsc_path[4096];
    snprintf(dump, sizeof(dump), "%s/ep88-dv-hdv-dv.bin", argv[1]);
    snprintf(pal_path, sizeof(pal_path), "%s/dv-pal-32k.dv", argv[1]);
    snprintf(ntsc_path, sizeof(ntsc_path), "%s/dv-ntsc.dv", argv[1]);
    pin_test_require_fixture(dump);
    file_t pal = read_file(pal_path), ntsc = read_file(ntsc_path);

    /* 1. to files: .dv, .m2t, .dv */
    const char *names[] = { "mixed_kind.dv", "mixed_kind-0002.m2t", "mixed_kind-0003.dv",
                            "mixed_kind-0004.dv", "mixed_kind-0004.m2t", "mixed_kind.m2t",
                            "mixed_kind-0002.dv", "mixed_kind-0003.m2t" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        remove(names[i]);
    pin_capture_opts_t opts;
    pin_capture_opts_defaults(&opts);
    strncpy(opts.path, "mixed_kind", sizeof(opts.path) - 1);
    opts.format_dv = PIN_FMT_DV_RAW;
    opts.format_hdv = PIN_FMT_HDV_TS;
    pin_status_snapshot_t snap = capture_tape(dump, &opts);
    CHECK_EQ_I(snap.stop_reason, PIN_STOP_END_OF_TAPE);
    printf("captured %llu unit(s), %llu with errors\n", (unsigned long long)snap.frames,
           (unsigned long long)snap.frames_error);
    CHECK(snap.frames_error == 0);
    for (size_t i = 3; i < sizeof(names) / sizeof(names[0]); i++)
        CHECK(!pin_test_file_exists(names[i]));
    file_t f1 = read_file("mixed_kind.dv"), f2 = read_file("mixed_kind-0002.m2t"),
           f3 = read_file("mixed_kind-0003.dv");
    printf("files: %ld PAL frame(s), %ld TS packet(s), %ld NTSC frame(s)\n", f1.len / 144000,
           f2.len / TS, f3.len / 120000);
    CHECK(f1.len == pal.len && !memcmp(f1.data, pal.data, (size_t)pal.len));  /* every PAL frame */
    CHECK(f2.len > 3 * TS && f2.len % TS == 0);
    CHECK(pkt_pid(f2.data) == 0 && pkt_pid(f2.data + TS) == 0x81);           /* PAT, PMT */
    CHECK(pkt_pid(f2.data + 2 * TS) == 0x810 && gop_start(f2.data + 2 * TS)); /* a GOP */
    for (long i = 0; i < f2.len; i += TS)
        CHECK(f2.data[i] == 0x47);
    CHECK(f3.len == ntsc.len - 120000 && !memcmp(f3.data, ntsc.data, (size_t)f3.len));
    printf("OK: DV -> HDV -> DV gives .dv, .m2t (PAT, PMT, GOP first: no title, nothing added), .dv\n");

    /* 2. to standard output: the capture ends at the change, with the reason */
    int fd = OPEN_NEW("mixed_kind_pipe.out");
    CHECK(fd >= 0);
    fflush(stdout);
    int saved = DUP(1);
    CHECK(saved >= 0);
    CHECK(DUP2(fd, 1) >= 0);
    CLOSE(fd);
    pin_capture_opts_defaults(&opts);
    strncpy(opts.path, "-", sizeof(opts.path) - 1);
    snap = capture_tape(dump, &opts);
    fflush(stdout);
    CHECK(DUP2(saved, 1) >= 0);
    CLOSE(saved);
    printf("pipe: %s\n", snap.stop_text);
    CHECK_EQ_I(snap.stop_reason, PIN_STOP_ERROR);
    CHECK(strstr(snap.stop_text, "the input changed from DV to HDV; a pipe carries one format only"));
    file_t p = read_file("mixed_kind_pipe.out");
    CHECK(p.len == pal.len && !memcmp(p.data, pal.data, (size_t)pal.len));
    printf("OK: a pipe stops at DV -> HDV, after every DV frame\n");

    /* 3. DV AVI, aspect 16:9 for DV and 4:3 for HDV, a title: DV -> HDV -> DV, each file its own
     * kind's aspect, the title in each (the raw .m2t's as its DVB service name) */
    const char *avi_names[] = { "mixed_kind_avi.avi", "mixed_kind_avi-0002.m2t", "mixed_kind_avi-0003.avi",
                                "mixed_kind_avi-0004.avi", "mixed_kind_avi-0004.m2t" };
    for (size_t i = 0; i < sizeof(avi_names) / sizeof(avi_names[0]); i++)
        remove(avi_names[i]);
    pin_capture_opts_defaults(&opts);
    strncpy(opts.path, "mixed_kind_avi", sizeof(opts.path) - 1);
    strncpy(opts.title, "Mixed tape", sizeof(opts.title) - 1);
    opts.format_dv = PIN_FMT_DV_AVI;
    opts.format_hdv = PIN_FMT_HDV_TS;
    opts.aspect = PIN_ASPECT_16_9;
    opts.aspect_hdv = PIN_ASPECT_4_3;
    snap = capture_tape(dump, &opts);
    CHECK_EQ_I(snap.stop_reason, PIN_STOP_END_OF_TAPE);
    CHECK(snap.frames_error == 0);
    CHECK(pin_test_file_exists(avi_names[0]) && pin_test_file_exists(avi_names[1]) &&
          pin_test_file_exists(avi_names[2]));
    CHECK(!pin_test_file_exists(avi_names[3]) && !pin_test_file_exists(avi_names[4]));
    probed_t a1 = probe(avi_names[0]), a3 = probe(avi_names[2]);
    CHECK(a1.height == 576 && a1.frames == pal.len / 144000);
    CHECK(a3.height == 480 && a3.frames == ntsc.len / 120000 - 1);
    CHECK(a1.dar_num == 16 && a1.dar_den == 9 && a3.dar_num == 16 && a3.dar_den == 9);
    CHECK(strcmp(a1.title, "Mixed tape") == 0 && strcmp(a3.title, "Mixed tape") == 0);
    check_ts_title(avi_names[1], "Mixed tape");
    printf("OK: DV -> HDV -> DV as DV AVI: both AVIs 16:9 with the title, the raw HDV file's in its SDT\n");

    /* 4. a tape that starts on HDV: raw .m2t, then DV AVI with aspect 16:9 (not aspect_hdv's 4:3) */
    char hdv_dv[4096];
    snprintf(hdv_dv, sizeof(hdv_dv), "%s/ep88-hdv-dv.bin", argv[1]);
    pin_test_require_fixture(hdv_dv);
    const char *hd_names[] = { "mixed_kind_hdv.m2t", "mixed_kind_hdv-0002.avi", "mixed_kind_hdv-0003.avi",
                               "mixed_kind_hdv-0003.m2t", "mixed_kind_hdv.avi" };
    for (size_t i = 0; i < sizeof(hd_names) / sizeof(hd_names[0]); i++)
        remove(hd_names[i]);
    strncpy(opts.path, "mixed_kind_hdv", sizeof(opts.path) - 1);
    snap = capture_tape(hdv_dv, &opts);
    CHECK_EQ_I(snap.stop_reason, PIN_STOP_END_OF_TAPE);
    CHECK(snap.frames_error == 0);
    CHECK(pin_test_file_exists(hd_names[0]) && pin_test_file_exists(hd_names[1]));
    for (size_t i = 2; i < sizeof(hd_names) / sizeof(hd_names[0]); i++)
        CHECK(!pin_test_file_exists(hd_names[i]));
    check_ts_title(hd_names[0], "Mixed tape");
    probed_t h2 = probe(hd_names[1]);
    CHECK(h2.height == 576 && h2.frames == 49 && h2.dar_num == 16 && h2.dar_den == 9);
    CHECK(strcmp(h2.title, "Mixed tape") == 0);
    printf("OK: HDV -> DV: the raw HDV file has the title in its SDT, the DV AVI after it is 16:9 "
           "(aspect) with the title\n");

    free(pal.data);
    free(ntsc.data);
    free(f1.data);
    free(f2.data);
    free(f3.data);
    free(p.data);
    return 0;
}
