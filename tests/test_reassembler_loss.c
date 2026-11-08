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
 * dv_reassembler on a stream that loses data in transit (tests/ep88_synth.h: sequences that
 * are not sent while the CIP DBC counter goes on). Needs no fixture files.
 *   - frames lost whole are counted (frames_lost, on_loss), the others come out whole;
 *   - a frame that loses its first sequence comes out with that one empty, not lost;
 *   - a frame that loses its tail followed by a frame that loses its start are two
 *     frames, both padded, not one hybrid;
 *   - the last complete frame comes out at dv_reassembler_finish();
 *   - the DBC loss reaches on_loss as blocks.
 *
 * Usage: test_reassembler_loss
 */

#include "dv_reassembler.h"
#include "ep88_synth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1); \
        } \
    } while (0)

#define CHECK_EQ(a, b) \
    do { \
        long long _a = (long long)(a), _b = (long long)(b); \
        if (_a != _b) { \
            fprintf(stderr, "%s:%d: %s (%lld) != %s (%lld)\n", __FILE__, __LINE__, #a, _a, #b, _b); \
            exit(1); \
        } \
    } while (0)

#define MAX_FRAMES 64

typedef struct {
    int frames;
    size_t len[MAX_FRAMES];
    uint8_t first[MAX_FRAMES][16 * 12]; /* byte 4 of each sequence, 0 = empty */
    unsigned long loss_blocks, loss_frames;
} got_t;

static int on_write(const uint8_t *data, size_t len, void *user)
{
    got_t *g = user;
    CHECK(g->frames < MAX_FRAMES);
    g->len[g->frames] = len;
    for (size_t d = 0; d < len / EP88_SEQ_BYTES; d++)
        g->first[g->frames][d] = data[d * EP88_SEQ_BYTES + 4];
    g->frames++;
    return 0;
}

static void on_loss(unsigned long blocks, unsigned long frames, void *user)
{
    got_t *g = user;
    g->loss_blocks += blocks;
    g->loss_frames += frames;
}

#define FILL(f) ((uint8_t)(0x41 + (f)))

/* lost[f] is the mask of the sequences of frame f that are not sent. */
static void run(int pal, int frames, const unsigned *lost, got_t *g, dv_reassembler_t *r,
                int *emitted_before_finish)
{
    unsigned n = pal ? 12 : 10;
    ep88_synth_t w;
    memset(&w, 0, sizeof(w));
    uint8_t seq[EP88_SEQ_BYTES];
    for (int f = 0; f < frames; f++) {
        for (unsigned d = 0; d < n; d++) {
            ep88_make_seq(seq, d, pal, FILL(f));
            ep88_sequence(&w, seq, (lost[f] >> d) & 1);
        }
    }
    memset(g, 0, sizeof(*g));
    dv_output_t out = { .write = on_write, .on_loss = on_loss, .user = g };
    CHECK(dv_reassembler_init(r, &out) == 0);
    /* in USB-sized pieces */
    for (size_t off = 0; off < w.len;) {
        size_t k = w.len - off < 16384 ? w.len - off : 16384;
        CHECK(dv_reassembler_feed(r, w.data + off, k) == 0);
        off += k;
    }
    *emitted_before_finish = g->frames;
    dv_reassembler_finish(r);
    free(w.data);
}

#define ALL10 0x3FFu
#define ALL12 0xFFFu

static void test_clean(void)
{
    unsigned lost[8] = { 0 };
    got_t g;
    dv_reassembler_t r;
    int before;
    run(0, 8, lost, &g, &r, &before);
    CHECK_EQ(g.frames, 8);
    CHECK_EQ(before, 7);              /* the last complete frame waits for the next one... */
    CHECK_EQ(r.frames_written, 8);    /* ...and finish() emits it */
    CHECK_EQ(r.frames_lost, 0);
    CHECK_EQ(r.sequences_dropped, 0);
    CHECK_EQ(r.dbc_gaps, 0);
    CHECK_EQ(g.loss_blocks, 0);
    for (int f = 0; f < 8; f++)
        for (int d = 0; d < 10; d++)
            CHECK_EQ(g.first[f][d], FILL(f));
}

static void test_whole_frames_lost(void)
{
    unsigned lost[10] = { 0 };
    lost[4] = lost[5] = lost[6] = ALL10;
    got_t g;
    dv_reassembler_t r;
    int before;
    run(0, 10, lost, &g, &r, &before);
    CHECK_EQ(g.frames, 7);
    CHECK_EQ(r.frames_lost, 3);
    CHECK_EQ(g.loss_frames, 3);
    CHECK(g.loss_blocks > 0);
    CHECK_EQ(g.loss_blocks, r.dbc_lost_blocks);
    CHECK_EQ(r.sequences_dropped, 0); /* nothing padded: the frames around are whole */
    int want[7] = { 0, 1, 2, 3, 7, 8, 9 };
    for (int i = 0; i < 7; i++)
        for (int d = 0; d < 10; d++)
            CHECK_EQ(g.first[i][d], FILL(want[i]));
}

static void test_long_run_lost(void)
{
    unsigned lost[30] = { 0 };
    for (int f = 3; f < 23; f++)
        lost[f] = ALL10;
    got_t g;
    dv_reassembler_t r;
    int before;
    run(0, 30, lost, &g, &r, &before);
    CHECK_EQ(g.frames, 10);
    CHECK_EQ(r.frames_lost, 20);
}

static void test_first_sequence_lost(void)
{
    unsigned lost[10] = { 0 };
    lost[4] = 1; /* dseq 0 of frame 4 */
    got_t g;
    dv_reassembler_t r;
    int before;
    run(0, 10, lost, &g, &r, &before);
    CHECK_EQ(g.frames, 10);           /* the frame is not lost: it comes out, its start empty */
    CHECK_EQ(r.frames_lost, 0);
    CHECK_EQ(r.sequences_dropped, 1);
    CHECK_EQ(g.first[4][0], 0);
    for (int d = 1; d < 10; d++)
        CHECK_EQ(g.first[4][d], FILL(4));
    CHECK_EQ(g.first[3][9], FILL(3));
    CHECK_EQ(g.first[5][0], FILL(5));
    CHECK(g.loss_blocks > 0);
}

/* Frame 4 loses its sequences 7-9 and frame 5 its sequence 0: two frames, each with its
 * hole, not one made of both. */
static void test_hybrid_tail_head(void)
{
    unsigned lost[10] = { 0 };
    lost[4] = 0x380; /* 7, 8, 9 */
    lost[5] = 0x001;
    got_t g;
    dv_reassembler_t r;
    int before;
    run(0, 10, lost, &g, &r, &before);
    CHECK_EQ(g.frames, 10);
    CHECK_EQ(r.frames_lost, 0);
    CHECK_EQ(r.sequences_dropped, 4);
    for (int d = 0; d < 7; d++)
        CHECK_EQ(g.first[4][d], FILL(4));
    for (int d = 7; d < 10; d++)
        CHECK_EQ(g.first[4][d], 0);
    CHECK_EQ(g.first[5][0], 0);
    for (int d = 1; d < 10; d++)
        CHECK_EQ(g.first[5][d], FILL(5));
}

/* Exactly one frame's worth of sequences lost across the boundary, so the numbering of the
 * sequences runs on without a jump: still two frames. */
static void test_hybrid_contiguous(void)
{
    unsigned lost[10] = { 0 };
    lost[4] = 0x3C0; /* 6..9 */
    lost[5] = 0x03F; /* 0..5 */
    got_t g;
    dv_reassembler_t r;
    int before;
    run(0, 10, lost, &g, &r, &before);
    CHECK_EQ(g.frames, 10);
    CHECK_EQ(r.frames_lost, 0);
    CHECK_EQ(r.sequences_dropped, 10);
    for (int d = 0; d < 6; d++) {
        CHECK_EQ(g.first[4][d], FILL(4));
        CHECK_EQ(g.first[5][d], 0);
    }
    for (int d = 6; d < 10; d++) {
        CHECK_EQ(g.first[4][d], 0);
        CHECK_EQ(g.first[5][d], FILL(5));
    }
}

/* Frame 4 loses its tail, frame 5 is lost whole: the padded frame and one lost frame. */
static void test_tail_then_whole(void)
{
    unsigned lost[10] = { 0 };
    lost[4] = 0x300; /* 8, 9 */
    lost[5] = ALL10;
    got_t g;
    dv_reassembler_t r;
    int before;
    run(0, 10, lost, &g, &r, &before);
    CHECK_EQ(g.frames, 9);
    CHECK_EQ(r.frames_lost, 1);
    CHECK_EQ(r.sequences_dropped, 2);
    CHECK_EQ(g.first[4][7], FILL(4));
    CHECK_EQ(g.first[4][8], 0);
    CHECK_EQ(g.first[5][0], FILL(6));
}

static void test_pal(void)
{
    unsigned lost[8] = { 0 };
    lost[3] = ALL12;
    got_t g;
    dv_reassembler_t r;
    int before;
    run(1, 8, lost, &g, &r, &before);
    CHECK_EQ(g.frames, 7);
    CHECK_EQ(r.frames_lost, 1);
    CHECK_EQ(g.len[2], 144000);
    CHECK_EQ(g.len[3], 144000);
    CHECK_EQ(g.first[3][0], FILL(4));
}

int main(void)
{
    test_clean();
    test_whole_frames_lost();
    test_long_run_lost();
    test_first_sequence_lost();
    test_hybrid_tail_head();
    test_hybrid_contiguous();
    test_tail_then_whole();
    test_pal();
    printf("OK: lost frames counted, lost starts and hybrids padded\n");
    return 0;
}
