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
 * ctest: the `--capture -` path of the sinks layer, through the same code the
 * capture uses with fd 1 (pin_stdout_set_fd() points it at a temp file or a
 * pipe instead):
 *   - sink_raw to "-": the bytes equal what the file sink writes;
 *   - a reader that went away (closed pipe) is PIPE_CLOSED, not a disk error;
 *   - pin_writer in OVERFLOW_FATAL mode: a full queue refuses everything after
 *     it instead of silently dropping one unit and carrying on;
 *   - sink_nut: analog frames + audio as NUT, read back with libavformat.
 */

#include "test_util.h"
#include "../../src/sinks/pin_sink.h"
#include "../../src/sinks/pin_stdout.h"
#include "../../src/sinks/sinks_internal.h"
#include "../../src/sinks/pin_writer.h"
#include "../../src/engine/dv_subcode.h"

#include <libavformat/avformat.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#define PIPE(fds) _pipe(fds, 1 << 16, _O_BINARY)
#define OPEN_NEW(path) _open(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, 0600)
#define CLOSE _close
static void nap_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <signal.h>
#include <time.h>
#include <unistd.h>
#define PIPE(fds) pipe(fds)
#define OPEN_NEW(path) open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600)
#define CLOSE close
static void nap_ms(int ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n > 0 ? (size_t)n : 1);
    CHECK(b);
    CHECK(fread(b, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    *len = (size_t)n;
    return b;
}

static pin_sink_params_t params(pin_kind_t kind, int w, int h, int fn, int fd, pin_aspect_t aspect)
{
    pin_sink_params_t p;
    memset(&p, 0, sizeof(p));
    p.kind = kind;
    p.width = w;
    p.height = h;
    p.fps_num = fn;
    p.fps_den = fd;
    p.interlaced = 1;
    p.top_field_first = 1;
    p.aspect = aspect;
    p.audio_rate = 48000;
    p.audio_channels = 2;
    p.colour_matrix = PIN_MATRIX_BT601;
    return p;
}

/* ---- sink_raw to a file descriptor ------------------------------------- */

static void test_raw_to_fd(const char *tmp)
{
    const size_t frame = (size_t)DV_SEQ_COUNT_PAL * DV_SEQ_SIZE;
    uint8_t *fr = malloc(frame);
    CHECK(fr);
    for (size_t i = 0; i < frame; i++)
        fr[i] = (uint8_t)(i * 7 + 3);

    int fd = OPEN_NEW(tmp);
    CHECK(fd >= 0);
    pin_stdout_set_fd(fd);
    pin_sink_t *s = pin_sink_create_stdout(PIN_KIND_DV);
    CHECK(s);
    pin_sink_params_t p = params(PIN_KIND_DV, 720, 576, 25, 1, PIN_ASPECT_4_3);
    CHECK_EQ_I(s->open(s, "-", &p), PIN_OK);
    CHECK_EQ_I(s->write_unit(s, fr, frame), PIN_OK);
    CHECK_EQ_I(s->write_unit(s, fr, frame - 5), PIN_OK);   /* malformed: dropped and counted */
    CHECK_EQ_I(s->write_unit(s, fr, frame), PIN_OK);
    pin_sink_status_t st;
    s->get_status(s, &st);
    CHECK_EQ_I(st.units_written, 2);
    CHECK_EQ_I(st.units_damaged, 1);
    CHECK_EQ_I(st.bytes_written, 2 * frame);
    CHECK_EQ_I(s->close(s), PIN_OK);
    CLOSE(fd);
    pin_stdout_set_fd(-1);

    size_t n;
    uint8_t *got = read_file(tmp, &n);
    CHECK_EQ_I(n, 2 * frame);
    CHECK(memcmp(got, fr, frame) == 0 && memcmp(got + frame, fr, frame) == 0);
    free(got);
    remove(tmp);

    /* HDV goes through the same raw sink: arbitrary sizes pass */
    fd = OPEN_NEW(tmp);
    pin_stdout_set_fd(fd);
    s = pin_sink_create_stdout(PIN_KIND_HDV);
    CHECK(s);
    p = params(PIN_KIND_HDV, 1440, 1080, 25, 1, PIN_ASPECT_16_9);
    CHECK_EQ_I(s->open(s, "-", &p), PIN_OK);
    CHECK_EQ_I(s->write_unit(s, (const uint8_t *)"Gabc", 4), PIN_OK);
    CHECK_EQ_I(s->close(s), PIN_OK);
    CLOSE(fd);
    pin_stdout_set_fd(-1);
    got = read_file(tmp, &n);
    CHECK_EQ_I(n, 4);
    CHECK(memcmp(got, "Gabc", 4) == 0);
    free(got);
    remove(tmp);
}

/* ---- the reader goes away ---------------------------------------------- */

static void test_closed_pipe(void)
{
#if !defined(_WIN32)
    signal(SIGPIPE, SIG_IGN);
#endif
    int fds[2];
    CHECK(PIPE(fds) == 0);
    pin_stdout_set_fd(fds[1]);
    pin_sink_t *s = pin_sink_create_stdout(PIN_KIND_HDV);
    CHECK(s);
    pin_sink_params_t p = params(PIN_KIND_HDV, 1440, 1080, 25, 1, PIN_ASPECT_16_9);
    CHECK_EQ_I(s->open(s, "-", &p), PIN_OK);
    uint8_t buf[1000] = { 0x47 };
    CHECK_EQ_I(s->write_unit(s, buf, sizeof(buf)), PIN_OK);   /* fits the pipe's buffer */
    CLOSE(fds[0]);                                             /* the reading program exits */
    pin_sink_status_t st;
    pin_status_t rc = s->write_unit(s, buf, sizeof(buf));
    CHECK_EQ_I(rc, PIN_ERR_IO);
    s->get_status(s, &st);
    CHECK_EQ_I(st.pipe_closed, 1);
    s->close(s);
    CLOSE(fds[1]);
    pin_stdout_set_fd(-1);
}

/* ---- pin_writer: a full queue is fatal, not a drop --------------------- */

typedef struct {
    volatile int release;
    volatile int consumed;
} slow_t;

static int slow_consume(pin_unit_kind_t kind, uint64_t index, const uint8_t *data, size_t len, void *user)
{
    slow_t *sl = user;
    (void)kind; (void)index; (void)data; (void)len;
    while (!sl->release)
        nap_ms(1);
    sl->consumed++;
    return 0;
}

static void test_writer_overflow_fatal(void)
{
    slow_t sl = { 0, 0 };
    uint8_t unit[400] = { 0 };
    pin_writer_t *w = pin_writer_start_ex(1000, slow_consume, &sl, PIN_WRITER_OVERFLOW_FATAL);
    CHECK(w);
    CHECK_EQ_I(pin_writer_push(w, PIN_UNIT_RAW, 0, unit, sizeof(unit)), 1);
    CHECK_EQ_I(pin_writer_push(w, PIN_UNIT_RAW, 1, unit, sizeof(unit)), 1);
    pin_writer_stats_t st;
    pin_writer_get_stats(w, &st);
    CHECK_EQ_I(st.overflowed, 0);
    CHECK_EQ_I(pin_writer_push(w, PIN_UNIT_RAW, 2, unit, sizeof(unit)), 0);   /* does not fit */
    pin_writer_get_stats(w, &st);
    CHECK_EQ_I(st.overflowed, 1);
    CHECK_EQ_I(st.overflow_count, 1);
    sl.release = 1;
    nap_ms(20);
    /* room again, but the hole is already there: nothing more is queued */
    CHECK_EQ_I(pin_writer_push(w, PIN_UNIT_RAW, 3, unit, 10), 0);
    CHECK_EQ_I(pin_writer_stop(w), 0);
    CHECK_EQ_I(sl.consumed, 2);   /* the two before the overflow were delivered */

    /* the normal mode keeps counting drops and carries on */
    sl.release = 0;
    sl.consumed = 0;
    w = pin_writer_start_ex(1000, slow_consume, &sl, 0);
    CHECK(w);
    CHECK_EQ_I(pin_writer_push(w, PIN_UNIT_RAW, 0, unit, sizeof(unit)), 1);
    CHECK_EQ_I(pin_writer_push(w, PIN_UNIT_RAW, 1, unit, sizeof(unit)), 1);
    CHECK_EQ_I(pin_writer_push(w, PIN_UNIT_RAW, 2, unit, sizeof(unit)), 0);
    pin_writer_get_stats(w, &st);
    CHECK_EQ_I(st.overflowed, 0);
    sl.release = 1;
    nap_ms(20);
    CHECK_EQ_I(pin_writer_push(w, PIN_UNIT_RAW, 3, unit, sizeof(unit)), 1);
    CHECK_EQ_I(pin_writer_stop(w), 0);
    CHECK_EQ_I(sl.consumed, 3);

    /* abort: what is queued is discarded, later pushes refused */
    sl.release = 0;
    sl.consumed = 0;
    w = pin_writer_start_ex(1000, slow_consume, &sl, 0);
    CHECK(pin_writer_push(w, PIN_UNIT_RAW, 0, unit, sizeof(unit)) == 1);
    CHECK(pin_writer_push(w, PIN_UNIT_RAW, 1, unit, sizeof(unit)) == 1);
    nap_ms(20);                    /* unit 0 is in the consumer, blocked */
    pin_writer_abort(w);
    sl.release = 1;
    CHECK_EQ_I(pin_writer_push(w, PIN_UNIT_RAW, 2, unit, 10), 0);
    CHECK_EQ_I(pin_writer_stop(w), 0);
    CHECK_EQ_I(sl.consumed, 1);    /* only the unit already being written */
}

/* ---- sink_nut ------------------------------------------------------------ */

#define NW 64
#define NH 48

static void nut_case(const char *tmp, int fn, int fd, pin_aspect_t aspect)
{
    int sar_n, sar_d;
    pin_sink_sar_for(PIN_KIND_ANALOG, fn == 25, aspect, NW, &sar_n, &sar_d);
    const int frames = 12;
    const int spf = (int)(48000LL * fd / fn);   /* audio samples per frame, 1601.6 for NTSC: use the rounded block */
    uint8_t vid[NW * NH * 2];
    int16_t pcm[2 * 2000];

    int ofd = OPEN_NEW(tmp);
    CHECK(ofd >= 0);
    pin_stdout_set_fd(ofd);
    pin_sink_t *s = pin_sink_create_stdout(PIN_KIND_ANALOG);
    CHECK(s);
    pin_sink_params_t p = params(PIN_KIND_ANALOG, NW, NH, fn, fd, aspect);
    strcpy(p.title, "nut test");
    CHECK_EQ_I(s->open(s, "-", &p), PIN_OK);
    for (int i = 0; i < frames; i++) {
        for (int k = 0; k < (int)sizeof(vid); k++)
            vid[k] = (uint8_t)(i * 13 + k);
        CHECK_EQ_I(s->write_video(s, vid, sizeof(vid)), PIN_OK);
        for (int k = 0; k < 2 * spf; k++)
            pcm[k] = (int16_t)(i * 100 + k);
        CHECK_EQ_I(s->write_audio(s, pcm, (size_t)spf), PIN_OK);
    }
    CHECK_EQ_I(s->write_video(s, vid, 10), PIN_OK);   /* short frame: skipped and counted */
    pin_sink_status_t st;
    s->get_status(s, &st);
    CHECK_EQ_I(st.units_written, frames);
    CHECK_EQ_I(st.units_damaged, 1);
    CHECK_EQ_I(s->close(s), PIN_OK);
    CLOSE(ofd);
    pin_stdout_set_fd(-1);

    /* read it back */
    AVFormatContext *ic = NULL;
    CHECK(avformat_open_input(&ic, tmp, NULL, NULL) == 0);
    CHECK(strcmp(ic->iformat->name, "nut") == 0);
    CHECK(avformat_find_stream_info(ic, NULL) >= 0);
    CHECK_EQ_I(ic->nb_streams, 2);
    int vi = -1, ai = -1;
    for (unsigned i = 0; i < ic->nb_streams; i++) {
        if (ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) vi = (int)i;
        if (ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) ai = (int)i;
    }
    CHECK(vi >= 0 && ai >= 0);
    AVCodecParameters *vp = ic->streams[vi]->codecpar, *ap = ic->streams[ai]->codecpar;
    CHECK_EQ_I(vp->codec_id, AV_CODEC_ID_RAWVIDEO);
    CHECK_EQ_I(vp->codec_tag, MKTAG('Y', 'U', 'Y', '2'));   /* the demuxer maps the tag to a pixel format with the rawvideo decoder */
    CHECK_EQ_I(vp->width, NW);
    CHECK_EQ_I(vp->height, NH);
    {
        AVRational fr = av_guess_frame_rate(ic, ic->streams[vi], NULL);
        CHECK_EQ_I(fr.num * (int64_t)fd, fr.den * (int64_t)fn);
    }
    CHECK_EQ_I(ic->streams[vi]->sample_aspect_ratio.num * (int64_t)sar_d,
               ic->streams[vi]->sample_aspect_ratio.den * (int64_t)sar_n);
    CHECK_EQ_I(ap->codec_id, AV_CODEC_ID_PCM_S16LE);
    CHECK_EQ_I(ap->sample_rate, 48000);
    CHECK_EQ_I(ap->ch_layout.nb_channels, 2);
    AVDictionaryEntry *title = av_dict_get(ic->metadata, "title", NULL, 0);
    CHECK(title && strcmp(title->value, "nut test") == 0);

    AVPacket *pkt = av_packet_alloc();
    int nv = 0, na = 0;
    int64_t last_vpts = -1, apos = 0;
    while (av_read_frame(ic, pkt) >= 0) {
        if (pkt->stream_index == vi) {
            CHECK_EQ_I(pkt->size, NW * NH * 2);
            CHECK_EQ_I(pkt->data[0], (uint8_t)(nv * 13));
            CHECK_EQ_I(pkt->data[100], (uint8_t)(nv * 13 + 100));
            /* pts counted in frames: pts * time_base == nv / fps */
            /* the muxer may rewrite the stream's time base (NUT does): compare in seconds */
            double secs = (double)pkt->pts * av_q2d(ic->streams[vi]->time_base);
            CHECK(secs - (double)nv * fd / fn < 1e-6 && (double)nv * fd / fn - secs < 1e-6);
            CHECK(pkt->pts > last_vpts);
            last_vpts = pkt->pts;
            nv++;
        } else {
            CHECK_EQ_I(pkt->size, spf * 4);
            AVRational tb = ic->streams[ai]->time_base;
            CHECK_EQ_I(av_rescale_q(pkt->pts, tb, (AVRational){ 1, 48000 }), apos);
            const int16_t *sm = (const int16_t *)pkt->data;
            CHECK_EQ_I(sm[0], (int16_t)(na * 100));
            apos += spf;
            na++;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    CHECK_EQ_I(nv, frames);
    CHECK_EQ_I(na, frames);
    avformat_close_input(&ic);
    if (!getenv("PIN_KEEP_NUT"))      /* PIN_KEEP_NUT=1: leave the file for ffprobe */
        remove(tmp);
}

static int pid_for_names(void)
{
#if defined(_WIN32)
    return (int)GetCurrentProcessId();
#else
    return (int)getpid();
#endif
}

int main(void)
{
    char t1[512], t2[512];
    const char *dir = getenv("TEMP");
    if (!dir) dir = getenv("TMPDIR");
    if (!dir) dir = ".";
    snprintf(t1, sizeof(t1), "%s/pin_test_stdout_%d.bin", dir, pid_for_names());
    snprintf(t2, sizeof(t2), "%s/pin_test_stdout_%d.nut", dir, pid_for_names());

    test_raw_to_fd(t1);
    test_closed_pipe();
    test_writer_overflow_fatal();
    nut_case(t2, 25, 1, PIN_ASPECT_4_3);
    nut_case(t2, 30000, 1001, PIN_ASPECT_16_9);
    printf("test_stdout_sinks: ok\n");
    return 0;
}
