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
 * The analog assembler (pinnacle_analog.c, included here for its static functions) on
 * synthetic packets, no device: gaps in the video filled to the audio timeline (the last
 * frame for a short gap, black for one over a second), gaps in the audio filled with
 * silence, and a gap in both streams (the signal went, or the device restarted) left alone.
 *
 * Usage: test_analog_assembler
 */

#include "../src/core/pinnacle_analog.c"

#define CHECK_EQ(a, b) \
    do { \
        long long _a = (long long)(a), _b = (long long)(b); \
        if (_a != _b) { \
            fprintf(stderr, "%s:%d: %s (%lld) != %s (%lld)\n", __FILE__, __LINE__, #a, _a, #b, _b); \
            exit(1); \
        } \
    } while (0)

#define W 16
#define H 8
#define FRAME_TICKS 400000u   /* PAL: 1920 samples */
#define MAX_ITEMS 4000

typedef struct {
    int nv, na;
    long total_v, total_a;
    int v_repeated[MAX_ITEMS], v_black[MAX_ITEMS];
    uint8_t v_first[MAX_ITEMS];   /* the first byte of the frame */
    int a_silence[MAX_ITEMS], a_padded[MAX_ITEMS];
    unsigned a_samples[MAX_ITEMS];
    uint32_t next_index;
    int index_bad;
} got_t;

static int got_video(const pinnacle_video_frame_t *f, void *user)
{
    got_t *g = user;
    if (f->index != g->next_index)
        g->index_bad++;
    g->next_index = f->index + 1;
    g->total_v++;
    if (g->nv < MAX_ITEMS) {
        g->v_repeated[g->nv] = f->repeated;
        g->v_black[g->nv] = f->black;
        g->v_first[g->nv] = f->yuyv[0];
        g->nv++;
    }
    return 0;
}

static int got_audio(const pinnacle_audio_block_t *b, void *user)
{
    got_t *g = user;
    g->total_a++;
    if (g->na < MAX_ITEMS) {
        g->a_padded[g->na] = b->padded;
        g->a_samples[g->na] = b->samples;
        g->a_silence[g->na++] = b->silence;
    }
    return 0;
}

typedef struct {
    assembler_t s;
    pinnacle_capture_stats_t st;
    got_t got;
    pinnacle_capture_sink_t sink;
    volatile int stop;
} rig_t;

static void rig_open_wh(rig_t *r, unsigned w, unsigned h)
{
    pinnacle_analog_t a;
    memset(&a, 0, sizeof(a));
    memset(&r->got, 0, sizeof(r->got));
    a.width = w;
    a.height = h;
    a.audio_samples_per_packet = 1920;
    a.cfg.standard = PINNACLE_STD_PAL;
    r->sink = (pinnacle_capture_sink_t){ .video = got_video, .audio = got_audio, .user = &r->got };
    r->stop = 0;
    CHECK_EQ(assembler_open(&r->s, &a, &r->sink, &r->st, &r->stop), 0);
}

static void rig_open(rig_t *r) { rig_open_wh(r, W, H); }

/* Device ticks per frame (and per PAL audio packet): the device's clock runs a little fast. */
static uint64_t g_ft = FRAME_TICKS;
/* Device time of video frame n. The audio packet of frame n is 680 ticks in. */
static uint64_t vt(unsigned n) { return (uint64_t)n * g_ft; }

static void header(uint8_t *p, uint16_t seq, uint64_t t)
{
    p[0] = 0xff;
    p[1] = 0;
    p[2] = (uint8_t)seq;
    p[3] = (uint8_t)(seq >> 8);
    for (int i = 0; i < 8; i++)
        p[4 + i] = (uint8_t)(t >> (8 * i));
}

/* A complete frame whose pixels all hold `fill`. */
static void video(rig_t *r, uint16_t seq, uint64_t t, uint8_t fill)
{
    uint8_t buf[PACKET_HEADER + W * H * 2];
    header(buf, seq, t);
    memset(buf + PACKET_HEADER, fill, W * H * 2);
    CHECK_EQ(assembler_cb(PINNACLE_EP_VIDEO_IN, buf, sizeof(buf), &r->s), 0);
}

static void audio(rig_t *r, uint16_t seq, uint64_t t)
{
    static uint8_t buf[PACKET_HEADER + 1920 * 4];
    header(buf, seq, t);
    assembler_cb(PINNACLE_EP_AUDIO_IN, buf, sizeof(buf), &r->s);
}

/* Frames from..to-1 with their audio, as the device sends them. */
static void run(rig_t *r, unsigned from, unsigned to)
{
    for (unsigned n = from; n < to; n++) {
        video(r, (uint16_t)n, vt(n), (uint8_t)(n % 200 + 1));
        audio(r, (uint16_t)n, vt(n) + 680);
    }
}

static void test_clean(void)
{
    rig_t r;
    rig_open(&r);
    run(&r, 1, 31);
    assembler_close(&r.s);
    CHECK_EQ(r.st.frames, 30);
    CHECK_EQ(r.st.audio_blocks, 30);
    CHECK_EQ(r.st.frames_missing, 0);
    CHECK_EQ(r.st.audio_missing, 0);
}

static void test_short_gap_repeats(void)
{
    rig_t r;
    rig_open(&r);
    run(&r, 1, 11);
    for (unsigned n = 11; n < 16; n++)
        audio(&r, (uint16_t)n, vt(n) + 680);   /* 5 frames lost, audio goes on */
    run(&r, 16, 21);
    assembler_close(&r.s);
    CHECK_EQ(r.st.frames, 20);
    CHECK_EQ(r.st.frames_missing, 5);
    CHECK_EQ(r.st.frames_black, 0);
    CHECK_EQ(r.st.audio_blocks, 20);
    for (int i = 10; i < 15; i++) {
        CHECK_EQ(r.got.v_repeated[i], 1);
        CHECK_EQ(r.got.v_black[i], 0);
        CHECK_EQ(r.got.v_first[i], 10 + 1);   /* the last good frame (frame 10) */
    }
}

static void gap_black(unsigned lost)
{
    rig_t r;
    rig_open(&r);
    run(&r, 1, 11);
    for (unsigned n = 11; n < 11 + lost; n++)
        audio(&r, (uint16_t)n, vt(n) + 680);
    run(&r, 11 + lost, 11 + lost + 10);
    assembler_close(&r.s);
    CHECK_EQ(r.st.frames, 20 + lost);
    CHECK_EQ(r.st.frames_missing, lost);
    CHECK_EQ(r.st.frames_black, lost);
    CHECK_EQ(r.st.audio_blocks, 20 + lost);
    CHECK_EQ(r.st.audio_missing, 0);
    for (unsigned i = 10; i < 10 + lost; i++) {
        CHECK_EQ(r.got.v_repeated[i], 1);
        CHECK_EQ(r.got.v_black[i], 1);
        CHECK_EQ(r.got.v_first[i], 0x10);   /* Y of black; U/V (0x80) alternate after it */
    }
    CHECK_EQ(r.got.v_first[10 + lost], (11 + lost) % 200 + 1);
}

static void test_both_lost(int audio_first)
{
    rig_t r;
    rig_open(&r);
    run(&r, 1, 11);
    /* both streams jump 400 frames ahead: no signal for that long, then it returns */
    if (audio_first) {
        audio(&r, 410, vt(410) + 680);
        video(&r, 410, vt(410), 7);
    } else {
        video(&r, 410, vt(410), 7);
        audio(&r, 410, vt(410) + 680);
    }
    run(&r, 411, 416);
    assembler_close(&r.s);
    CHECK_EQ(r.st.frames, 16);
    CHECK_EQ(r.st.frames_missing, 0);
    CHECK_EQ(r.st.audio_missing, 0);
    CHECK_EQ(r.st.audio_blocks, 16);
}

static void test_audio_lost_video_ran(void)
{
    rig_t r;
    rig_open(&r);
    run(&r, 1, 11);
    for (unsigned n = 11; n < 411; n++)   /* 400 audio packets lost, video goes on */
        video(&r, (uint16_t)n, vt(n), 9);
    audio(&r, 411, vt(411) + 680);
    video(&r, 411, vt(411), 9);
    assembler_close(&r.s);
    CHECK_EQ(r.st.audio_missing, 400);
    CHECK_EQ(r.st.frames_missing, 0);
    int silent = 0;
    for (int i = 0; i < r.got.na; i++)
        silent += r.got.a_silence[i];
    CHECK_EQ(silent, 400);
}

static void test_video_stops_audio_runs_to_stop(void)
{
    rig_t r;
    rig_open(&r);
    run(&r, 1, 31);
    for (unsigned n = 31; n < 81; n++)   /* the video is gone, the audio is not */
        audio(&r, (uint16_t)n, vt(n) + 680);
    r.stop = 1;
    audio(&r, 81, vt(81) + 680);        /* past the last frame: the audio is caught up */
    assembler_close(&r.s);
    CHECK_EQ(r.st.audio_blocks, 80);
    CHECK_EQ(r.st.frames, 80);
    CHECK_EQ(r.st.frames_black, 50);
}

static void test_normal_stop_adds_nothing(void)
{
    rig_t r;
    rig_open(&r);
    run(&r, 1, 31);
    audio(&r, 31, vt(31) + 680);         /* the next frame's audio, its video never came */
    r.stop = 1;
    audio(&r, 32, vt(32) + 680);
    assembler_close(&r.s);
    CHECK_EQ(r.st.frames, 30);
    CHECK_EQ(r.st.frames_missing, 0);
}

/* Both streams of a capture have the same length in time: frames and 1920-sample blocks. */
static void check_equal_spans(rig_t *r)
{
    CHECK_EQ(r->st.frames, r->st.audio_blocks);
    CHECK_EQ(r->got.total_v, r->got.total_a);
    CHECK_EQ(r->got.index_bad, 0);
}

/* The device clock jumps by `jump` ticks while the frame counter runs on. Whatever the order the
 * two endpoints are polled in, video and audio fill the same span. */
static void test_clock_jump(uint64_t jump, int audio_first)
{
    rig_t r;
    rig_open(&r);
    run(&r, 1, 11);
    if (audio_first) {
        audio(&r, 11, vt(11) + 680 + jump);
        video(&r, 11, vt(11) + jump, 5);
    } else {
        video(&r, 11, vt(11) + jump, 5);
        audio(&r, 11, vt(11) + 680 + jump);
    }
    for (unsigned n = 12; n < 22; n++) {
        video(&r, (uint16_t)n, vt(n) + jump, 5);
        audio(&r, (uint16_t)n, vt(n) + 680 + jump);
    }
    assembler_close(&r.s);
    check_equal_spans(&r);
    uint64_t lost = jump / FRAME_TICKS;
    if (lost < 250)
        CHECK_EQ(r.st.frames, 21 + lost);   /* a short jump is filled in both */
    else
        CHECK_EQ(r.st.frames, 21);          /* a long one is a restart in both */
}

/* The host stalled and the device lost 300 frames of video; their audio arrived, but the read
 * loop hands over five frames after the gap before it polls the audio endpoint. */
static void test_video_gap_before_audio(void)
{
    rig_t r;
    rig_open(&r);
    run(&r, 1, 11);
    for (unsigned n = 311; n < 316; n++)
        video(&r, (uint16_t)n, vt(n), 3);
    for (unsigned n = 11; n < 316; n++)
        audio(&r, (uint16_t)n, vt(n) + 680);
    run(&r, 316, 321);
    assembler_close(&r.s);
    CHECK_EQ(r.st.frames_missing, 300);
    CHECK_EQ(r.st.frames, 320);
    check_equal_spans(&r);
    CHECK_EQ(r.got.v_black[10], 1);          /* the fill went before the frames held behind it */
    CHECK_EQ(r.got.v_first[310], 3);
}

/* The same, but the audio had the gap too: a restart, also with the frames held first. */
static void test_both_gap_video_first_held(void)
{
    rig_t r;
    rig_open(&r);
    run(&r, 1, 11);
    for (unsigned n = 311; n < 316; n++)
        video(&r, (uint16_t)n, vt(n), 3);
    for (unsigned n = 311; n < 316; n++)
        audio(&r, (uint16_t)n, vt(n) + 680);
    run(&r, 316, 321);
    assembler_close(&r.s);
    CHECK_EQ(r.st.frames_missing, 0);
    CHECK_EQ(r.st.frames, 20);
    check_equal_spans(&r);
}

/* With the clock 110 ppm fast, a gap of 14,000 frames (the audio ran on) is filled exactly. */
static void test_long_gap_fast_clock(void)
{
    g_ft = 400044;
    rig_t r;
    rig_open(&r);
    run(&r, 1, 601);
    for (unsigned n = 601; n < 14601; n++)
        audio(&r, (uint16_t)n, vt(n) + 680);
    run(&r, 14601, 14611);
    assembler_close(&r.s);
    g_ft = FRAME_TICKS;
    CHECK_EQ(r.st.frames_missing, 14000);
    CHECK_EQ(r.st.frames_black, 14000);
    CHECK_EQ(r.st.audio_missing, 0);
    check_equal_spans(&r);
}

/* A short audio packet is padded to a full one with silence, and counted. */
static void test_short_audio_padded(void)
{
    rig_t r;
    rig_open(&r);
    run(&r, 1, 6);
    static uint8_t buf[PACKET_HEADER + 1920 * 4];
    video(&r, 6, vt(6), 1);
    header(buf, 6, vt(6) + 680);
    memset(buf + PACKET_HEADER, 0x11, 1000 * 4);
    assembler_cb(PINNACLE_EP_AUDIO_IN, buf, PACKET_HEADER + 1000 * 4, &r.s);
    run(&r, 7, 11);
    assembler_close(&r.s);
    CHECK_EQ(r.st.audio_short, 1);
    CHECK_EQ(r.st.audio_missing, 0);
    CHECK_EQ(r.got.a_padded[5], 1);
    CHECK_EQ(r.got.a_samples[5], 1920);
    check_equal_spans(&r);
}

/* A frame the device cut short that ends on a whole USB packet: ended by a zero-length packet,
 * or (without one) by the next header at a packet boundary in the same transfer. */
static void test_truncated_on_packet_boundary(int zlp)
{
    rig_t r;
    rig_open_wh(&r, 64, 16);                /* 2048 bytes a frame */
    static uint8_t buf[4096];
    header(buf, 1, vt(1));
    memset(buf + PACKET_HEADER, 0x20, 512 - PACKET_HEADER);
    if (zlp) {
        CHECK_EQ(assembler_cb(PINNACLE_EP_VIDEO_IN, buf, 512, &r.s), 0);
        CHECK_EQ(assembler_cb(PINNACLE_EP_VIDEO_IN, buf, 0, &r.s), 0);
        CHECK_EQ(r.st.frames, 1);
        header(buf, 2, vt(2));
        memset(buf + PACKET_HEADER, 0x30, 2048);
        CHECK_EQ(assembler_cb(PINNACLE_EP_VIDEO_IN, buf, PACKET_HEADER + 2048, &r.s), 0);
    } else {
        header(buf + 512, 2, vt(2));                 /* 500 bytes of frame 2 follow */
        memset(buf + 512 + PACKET_HEADER, 0x30, 512 - PACKET_HEADER);
        CHECK_EQ(assembler_cb(PINNACLE_EP_VIDEO_IN, buf, 1024, &r.s), 0);
        memset(buf, 0x30, 2048 - 500);               /* the rest, ending on a short packet */
        CHECK_EQ(assembler_cb(PINNACLE_EP_VIDEO_IN, buf, 2048 - 500, &r.s), 0);
    }
    audio(&r, 1, vt(1) + 680);
    audio(&r, 2, vt(2) + 680);
    assembler_close(&r.s);
    CHECK_EQ(r.st.frames, 2);
    CHECK_EQ(r.st.frames_truncated, 1);
    CHECK_EQ(r.st.resyncs, 0);
    CHECK_EQ(r.got.v_repeated[0], 1);       /* the short one stands in as the last full frame */
    CHECK_EQ(r.got.v_repeated[1], 0);
    CHECK_EQ(r.got.v_first[1], 0x30);
}

int main(void)
{
    test_clean();
    test_short_gap_repeats();
    gap_black(40);      /* over a second, under 250 frames */
    gap_black(400);     /* 250 frames or more, audio went on */
    test_both_lost(0);
    test_both_lost(1);
    test_audio_lost_video_ran();
    test_video_stops_audio_runs_to_stop();
    test_normal_stop_adds_nothing();
    test_clock_jump(5ull * DEVICE_HZ, 0);
    test_clock_jump(5ull * DEVICE_HZ, 1);
    test_clock_jump(120ull * DEVICE_HZ, 0);
    test_clock_jump(120ull * DEVICE_HZ, 1);
    test_video_gap_before_audio();
    test_both_gap_video_first_held();
    test_long_gap_fast_clock();
    test_short_audio_padded();
    test_truncated_on_packet_boundary(1);
    test_truncated_on_packet_boundary(0);
    printf("test_analog_assembler: ok\n");
    return 0;
}
