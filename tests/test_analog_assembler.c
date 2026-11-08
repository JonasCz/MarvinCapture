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
    int v_repeated[MAX_ITEMS], v_black[MAX_ITEMS];
    uint8_t v_first[MAX_ITEMS];   /* the first byte of the frame */
    int a_silence[MAX_ITEMS];
} got_t;

static int got_video(const pinnacle_video_frame_t *f, void *user)
{
    got_t *g = user;
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
    if (g->na < MAX_ITEMS)
        g->a_silence[g->na++] = b->silence;
    return 0;
}

typedef struct {
    assembler_t s;
    pinnacle_capture_stats_t st;
    got_t got;
    pinnacle_capture_sink_t sink;
    volatile int stop;
} rig_t;

static void rig_open(rig_t *r)
{
    pinnacle_analog_t a;
    memset(&a, 0, sizeof(a));
    memset(&r->got, 0, sizeof(r->got));
    a.width = W;
    a.height = H;
    a.audio_samples_per_packet = 1920;
    a.cfg.standard = PINNACLE_STD_PAL;
    r->sink = (pinnacle_capture_sink_t){ .video = got_video, .audio = got_audio, .user = &r->got };
    r->stop = 0;
    CHECK_EQ(assembler_open(&r->s, &a, &r->sink, &r->st, &r->stop), 0);
}

/* Device time of video frame n. The audio packet of frame n is 680 ticks in. */
static uint64_t vt(unsigned n) { return (uint64_t)n * FRAME_TICKS; }

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
    printf("test_analog_assembler: ok\n");
    return 0;
}
