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
 * dv_reassembler: where the blocks of a damaged sequence land. Every DIF block of the
 * synthetic stream carries its real ID (SCT, Dseq, DBN) and a marker (frame, sequence,
 * slot), so the test checks each of the 150 slots of every output sequence: it holds its
 * own block or, if that block was lost in transit, zeros. A lost 480-byte packet must cost
 * exactly its 6 blocks, not shift the rest of the sequence. The frames with a loss are
 * still error frames (dv_error_analyze(): the missing blocks), the others clean.
 *
 * Cases (NTSC and PAL): 1..30 packets lost at every packet offset of a sequence, so at its
 * start (with its header: the blocks after the hole are still placed), middle and end, and
 * across the next header; two holes in one sequence; whole sequences lost; holes of a frame
 * and more (each output frame holds only blocks of one input frame, in their own slots).
 *
 * Usage: test_reassembler_blocks
 */

#include "dv_reassembler.h"
#include "dv_error.h"
#include "ep88_synth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s [%s]\n", __FILE__, __LINE__, #cond, ctx); \
            exit(1); \
        } \
    } while (0)

#define F 5           /* frames per run */
#define MAXF 12       /* frames per run, at most */
#define MAXSEQ 12
#define BLK 80
#define BLOCKS 150

static char ctx[160];

/* SCT and DBN of slot b (0 header, 1-2 subcode, 3-5 VAUX, 9 x (1 audio + 15 video)). */
static void slot_id(unsigned b, unsigned *sct, unsigned *dbn)
{
    if (b == 0) { *sct = 0; *dbn = 0; }
    else if (b < 3) { *sct = 1; *dbn = b - 1; }
    else if (b < 6) { *sct = 2; *dbn = b - 3; }
    else {
        unsigned r = b - 6, g = r / 16, k = r % 16;
        if (k == 0) { *sct = 3; *dbn = g; }
        else { *sct = 4; *dbn = g * 15 + (k - 1); }
    }
}

/* Sequence `d` of frame `f` with a real ID and a marker in every block. Block 1 is the
 * first subcode block, as is_seq_header() wants it. The marker bytes avoid 0x1F, so no
 * header lookalike appears in them. */
static void make_seq(uint8_t *seq, int f, unsigned d, int pal)
{
    for (unsigned b = 0; b < BLOCKS; b++) {
        uint8_t *k = seq + (size_t)b * BLK;
        unsigned sct, dbn;
        slot_id(b, &sct, &dbn);
        memset(k, 0x55, BLK);
        k[0] = (uint8_t)((sct << 5) | 0x1F);
        k[1] = (uint8_t)((d << 4) | 0x07);
        k[2] = (uint8_t)dbn;
        k[3] = 0x00; /* STA 0 for video blocks; DSF in the header block */
        k[4] = (uint8_t)(0x20 + f);
        k[5] = (uint8_t)(0x20 + d);
        k[6] = (uint8_t)b;
        if (sct == 3) /* audio: not all 36 words alike (that reads as an audio error) */
            for (int i = 8; i < BLK; i++)
                k[i] = (uint8_t)(i * 3 + b);
    }
    if (pal)
        seq[3] = 0x80;
}

typedef struct {
    int frames;
    uint8_t *data[MAXF + 4];
    size_t len[MAXF + 4];
    unsigned long loss_blocks, loss_frames, frames_lost;
} got_t;

static int on_write(const uint8_t *data, size_t len, void *user)
{
    got_t *g = user;
    CHECK(g->frames < MAXF + 4);
    g->data[g->frames] = malloc(len);
    CHECK(g->data[g->frames]);
    memcpy(g->data[g->frames], data, len);
    g->len[g->frames++] = len;
    return 0;
}

static void on_loss(unsigned long blocks, unsigned long frames, void *user)
{
    got_t *g = user;
    g->loss_blocks += blocks;
    g->loss_frames += frames;
}

/* lost[p] = packet p of the stream is not sent (the DBC goes on). */
static void run_n(int pal, int frames, const uint8_t *lost, got_t *g)
{
    unsigned n = pal ? 12 : 10;
    ep88_synth_t w;
    memset(&w, 0, sizeof(w));
    uint8_t seq[EP88_SEQ_BYTES];
    int pk = 0;
    for (int f = 0; f < frames; f++)
        for (unsigned d = 0; d < n; d++) {
            make_seq(seq, f, d, pal);
            for (int i = 0; i < EP88_PACKETS_PER_SEQ; i++, pk++) {
                if (!lost[pk])
                    ep88_packet(&w, seq + (size_t)i * EP88_PACKET_BYTES, w.dbc);
                w.dbc = (w.dbc + 1) & 0xFF;
            }
        }
    memset(g, 0, sizeof(*g));
    dv_output_t out = { .write = on_write, .on_loss = on_loss, .user = g };
    dv_reassembler_t *r = malloc(sizeof(*r));
    CHECK(r && dv_reassembler_init(r, &out) == 0);
    for (size_t off = 0; off < w.len;) {
        size_t k = w.len - off < 7000 ? w.len - off : 7000;
        CHECK(dv_reassembler_feed(r, w.data + off, k) == 0);
        off += k;
    }
    g->frames_lost = r->frames_lost;
    dv_reassembler_finish(r);
    free(r);
    free(w.data);
}

static void run(int pal, const uint8_t *lost, got_t *g)
{
    run_n(pal, F, lost, g);
}

static void got_free(got_t *g)
{
    for (int i = 0; i < g->frames; i++)
        free(g->data[i]);
}

/* Every slot of every frame: its own block if it arrived, zeros otherwise; error frames exactly where something was lost. */
static void check(int pal, const uint8_t *lost, const got_t *g)
{
    unsigned n = pal ? 12 : 10;
    CHECK(g->frames == F);
    CHECK(g->loss_frames == 0);
    uint8_t want[BLK], zero[BLK];
    memset(zero, 0, sizeof(zero));
    uint8_t seq[EP88_SEQ_BYTES];
    for (int f = 0; f < F; f++) {
        CHECK(g->len[f] == (size_t)n * EP88_SEQ_BYTES);
        unsigned missing = 0;
        for (unsigned d = 0; d < n; d++) {
            int first = (f * (int)n + (int)d) * EP88_PACKETS_PER_SEQ;
            make_seq(seq, f, d, pal);
            for (unsigned b = 0; b < BLOCKS; b++) {
                int here = !lost[first + (int)(b / 6)];
                memcpy(want, here ? seq + (size_t)b * BLK : zero, BLK);
                const uint8_t *got = g->data[f] + (size_t)d * EP88_SEQ_BYTES + (size_t)b * BLK;
                if (memcmp(got, want, BLK) != 0) {
                    snprintf(ctx + strlen(ctx), sizeof(ctx) - strlen(ctx),
                             " frame %d seq %u slot %u: has %02x %02x %02x m %d/%d/%d", f, d, b,
                             got[0], got[1], got[2], got[4] - 0x20, got[5] - 0x20, got[6]);
                    CHECK(!"block in the wrong slot");
                }
                missing += !here;
            }
        }
        dv_frame_errors_t e;
        CHECK(dv_error_analyze(g->data[f], g->len[f], &e) == 0);
        CHECK(e.missing_blocks == missing);
        CHECK(e.frame_error == (missing != 0));
    }
}

static void lose_and_check(int pal, uint8_t *lost)
{
    got_t g;
    run(pal, lost, &g);
    check(pal, lost, &g);
    got_free(&g);
}

/* One hole of L packets at every packet of frame 2 (start, middle, end of each sequence, and
 * across the next sequence's header). */
static void test_one_hole(void)
{
    static const int lens[] = { 1, 2, 3, 5, 12, 24, 25, 26, 30 };
    for (int pal = 0; pal < 2; pal++) {
        int n = pal ? 12 : 10, per = n * EP88_PACKETS_PER_SEQ;
        for (unsigned li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
            for (int k = 0; k + lens[li] <= per; k++) {
                uint8_t lost[F * 12 * EP88_PACKETS_PER_SEQ];
                memset(lost, 0, sizeof(lost));
                memset(lost + 2 * per + k, 1, (size_t)lens[li]);
                snprintf(ctx, sizeof(ctx), "pal=%d lost %d at %d", pal, lens[li], k);
                lose_and_check(pal, lost);
            }
        }
    }
}

/* Two or three holes inside one sequence, and holes in neighbouring sequences. */
static void test_several_holes(void)
{
    for (int pal = 0; pal < 2; pal++) {
        int n = pal ? 12 : 10, per = n * EP88_PACKETS_PER_SEQ;
        for (int a = 1; a < EP88_PACKETS_PER_SEQ; a++) {
            for (int b = a + 2; b < EP88_PACKETS_PER_SEQ; b += 3) {
                uint8_t lost[F * 12 * EP88_PACKETS_PER_SEQ];
                memset(lost, 0, sizeof(lost));
                int s = 1 * per + 4 * EP88_PACKETS_PER_SEQ;
                lost[s + a] = 1;
                lost[s + b] = 1;
                if (b + 1 < EP88_PACKETS_PER_SEQ)
                    lost[s + b + 1] = 1;
                lost[s + EP88_PACKETS_PER_SEQ + a] = 1; /* and the next sequence */
                snprintf(ctx, sizeof(ctx), "pal=%d holes at %d and %d", pal, a, b);
                lose_and_check(pal, lost);
            }
        }
    }
}

/* Whole frames lost (holes of about a frame and more, up to past the DBC's 256): frames can
 * no longer be matched one to one, so each output frame is checked by itself: every block in
 * it is zero or the block of one input frame in its own sequence and slot, and a frame with a
 * zero block is an error frame. The frames lost whole are counted. */
static void test_frames_lost(void)
{
    const int FR = 10;
    for (int pal = 0; pal < 2; pal++) {
        int n = pal ? 12 : 10, per = n * EP88_PACKETS_PER_SEQ;
        const int lens[] = { per - 12, per, per + 13, 255, 256 + 30, 2 * per + 7, 3 * per };
        for (unsigned li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
            for (int k = 0; k < per; k += 7) {
                int L = lens[li];
                if (L % 256 == 0)
                    continue; /* the DBC cannot show it */
                uint8_t lost[MAXF * 12 * EP88_PACKETS_PER_SEQ];
                memset(lost, 0, sizeof(lost));
                memset(lost + 2 * per + k, 1, (size_t)L);
                snprintf(ctx, sizeof(ctx), "pal=%d lost %d at %d", pal, L, k);
                got_t g;
                run_n(pal, FR, lost, &g);
                int whole = 0;
                for (int f = 0; f < FR; f++) {
                    int all = 1;
                    for (int d = 0; d < n; d++)
                        all &= lost[(f * n + d) * EP88_PACKETS_PER_SEQ];
                    whole += all;
                }
                CHECK((int)g.frames_lost == whole);
                uint8_t seq[EP88_SEQ_BYTES], zero[BLK];
                memset(zero, 0, sizeof(zero));
                for (int o = 0; o < g.frames; o++) {
                    CHECK(g.len[o] == (size_t)n * EP88_SEQ_BYTES);
                    int fr = -1, zeros = 0;
                    for (int d = 0; d < n; d++)
                        for (int b = 0; b < BLOCKS; b++) {
                            const uint8_t *got = g.data[o] + (size_t)d * EP88_SEQ_BYTES + (size_t)b * BLK;
                            if (memcmp(got, zero, BLK) == 0) {
                                zeros++;
                                continue;
                            }
                            int f = got[4] - 0x20;
                            CHECK(f >= 0 && f < FR);
                            if (fr < 0)
                                fr = f;
                            if (f != fr)
                                snprintf(ctx + strlen(ctx), sizeof(ctx) - strlen(ctx),
                                         " output %d seq %d slot %d from frame %d, not %d", o, d,
                                         b, f, fr);
                            CHECK(f == fr);
                            make_seq(seq, f, (unsigned)d, pal);
                            CHECK(memcmp(got, seq + (size_t)b * BLK, BLK) == 0);
                        }
                    dv_frame_errors_t e;
                    CHECK(dv_error_analyze(g.data[o], g.len[o], &e) == 0);
                    CHECK((int)e.missing_blocks == zeros);
                    CHECK(e.frame_error == (zeros != 0));
                }
                got_free(&g);
            }
        }
    }
}

int main(void)
{
    test_frames_lost();
    /* a clean stream comes out unchanged */
    for (int pal = 0; pal < 2; pal++) {
        uint8_t lost[F * 12 * EP88_PACKETS_PER_SEQ];
        memset(lost, 0, sizeof(lost));
        snprintf(ctx, sizeof(ctx), "pal=%d clean", pal);
        lose_and_check(pal, lost);
    }
    test_several_holes();
    test_one_hole();
    printf("OK: blocks after a hole stay in their own slots, only the lost ones are empty\n");
    return 0;
}
