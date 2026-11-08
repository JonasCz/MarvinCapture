/*
 * dv_reassembler on streams that lose arbitrary 480-byte packets (not whole sequences, as
 * test_reassembler_loss does), the way the FX2 / FPGA drops them, and on data that looks
 * like DIF headers. Synthetic, needs no fixture files.
 *   - 1..60 packets lost at every offset: no lost frames are invented, no frame is split,
 *     every frame arrives (the damaged ones padded) and the intact ones are untouched;
 *   - a hole in the last sequence of a frame does not swallow the next frame;
 *   - header-like bytes in video data do not start sequences or frames;
 *   - whole frames lost at any alignment are counted, and holes of 128..255 blocks report
 *     their length.
 *
 * Usage: test_reassembler_holes
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
            fprintf(stderr, "%s:%d: %s (%lld) != %s (%lld) [%s]\n", __FILE__, __LINE__, #a, _a, \
                    #b, _b, ctx); \
            exit(1); \
        } \
    } while (0)

#define MAX_FRAMES 64
#define FILL(f) ((uint8_t)(0x41 + (f)))

static char ctx[128];

typedef struct {
    int frames;
    size_t len[MAX_FRAMES];
    uint8_t first[MAX_FRAMES][12]; /* byte 4 of each sequence, 0 = empty */
    unsigned long loss_blocks, loss_frames;
} got_t;

static int on_write(const uint8_t *data, size_t len, void *user)
{
    got_t *g = user;
    CHECK(g->frames < MAX_FRAMES);
    g->len[g->frames] = len;
    for (size_t d = 0; d < len / EP88_SEQ_BYTES && d < 12; d++)
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

/* Planted into the video data of every sequence of the frames flagged in `plant`. */
typedef struct {
    unsigned plant_mask; /* frames */
} opts_t;

/* `frames` frames, the packets numbered from 0 over the whole stream; packets
 * [lose_at, lose_at + lose_n) are not sent (the DBC goes on). */
static void run(int pal, int frames, int lose_at, int lose_n, const opts_t *o, got_t *g,
                dv_reassembler_t *r)
{
    unsigned n = pal ? 12 : 10;
    ep88_synth_t w;
    memset(&w, 0, sizeof(w));
    uint8_t seq[EP88_SEQ_BYTES];
    int pk = 0;
    for (int f = 0; f < frames; f++) {
        for (unsigned d = 0; d < n; d++) {
            ep88_make_seq(seq, d, pal, FILL(f));
            if (o && ((o->plant_mask >> f) & 1)) {
                /* header lookalikes at block starts (not followed by a subcode block) and
                 * at odd offsets, dseq 0, 5 and 13 */
                static const unsigned offs[] = { 80 * 7, 80 * 20 + 1, 80 * 33 + 40, 12000 - 200,
                                                 80 * 90, 80 * 120 + 3, 5000 };
                for (unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
                    unsigned ds = i % 3 == 0 ? 0 : i % 3 == 1 ? 5 : 13;
                    seq[offs[i]] = 0x1F;
                    seq[offs[i] + 1] = (uint8_t)((ds << 4) | 7);
                    seq[offs[i] + 2] = 0x00;
                }
                /* one that is followed by a 0x3F block with the wrong sequence number */
                seq[80 * 50] = 0x1F; seq[80 * 50 + 1] = 0x07; seq[80 * 50 + 2] = 0x00;
                seq[80 * 51] = 0x3F; seq[80 * 51 + 1] = 0x97;
            }
            for (int i = 0; i < EP88_PACKETS_PER_SEQ; i++, pk++) {
                if (pk >= lose_at && pk < lose_at + lose_n)
                    w.dbc = (w.dbc + 1) & 0xFF;
                else {
                    ep88_packet(&w, seq + (size_t)i * EP88_PACKET_BYTES, w.dbc);
                    w.dbc = (w.dbc + 1) & 0xFF;
                }
            }
        }
    }
    memset(g, 0, sizeof(*g));
    dv_output_t out = { .write = on_write, .on_loss = on_loss, .user = g };
    CHECK(dv_reassembler_init(r, &out) == 0);
    for (size_t off = 0; off < w.len;) {
        size_t k = w.len - off < 16384 ? w.len - off : 16384;
        CHECK(dv_reassembler_feed(r, w.data + off, k) == 0);
        off += k;
    }
    dv_reassembler_finish(r);
    free(w.data);
}

static int frame_intact(const got_t *g, int idx, int f, unsigned n)
{
    if (g->len[idx] != n * EP88_SEQ_BYTES)
        return 0;
    for (unsigned d = 0; d < n; d++)
        if (g->first[idx][d] != FILL(f))
            return 0;
    return 1;
}

/* 1..60 packets lost starting at every packet of frame 2 (and the last sequence's span): no
 * phantom lost frames, no split frames, every frame present, the far ones intact. */
static void test_sweep(void)
{
    const int F = 7;
    for (int pal = 0; pal < 2; pal++) {
        int n = pal ? 12 : 10;
        for (int L = 1; L <= 60; L++) {
            for (int k = 0; k < n * EP88_PACKETS_PER_SEQ; k++) {
                got_t g;
                dv_reassembler_t r;
                snprintf(ctx, sizeof(ctx), "pal=%d lost %d at offset %d", pal, L, k);
                run(pal, F, 2 * n * EP88_PACKETS_PER_SEQ + k, L, NULL, &g, &r);
                CHECK_EQ(r.frames_lost, 0);
                CHECK_EQ(g.loss_frames, 0);
                CHECK_EQ(g.loss_blocks, L);
                CHECK_EQ(g.frames, F);
                for (int f = 0; f < F; f++) {
                    if (f == 2 || f == 3)
                        continue; /* may hold the hole */
                    CHECK(frame_intact(&g, f, f, n));
                }
            }
        }
    }
}

/* A hole in the last sequence of a frame, at every packet and length, leaves the next
 * frame whole. */
static void test_last_sequence(void)
{
    const int F = 8;
    for (int pal = 0; pal < 2; pal++) {
        int n = pal ? 12 : 10;
        for (int L = 1; L <= 24; L++) {
            for (int k = 0; k < EP88_PACKETS_PER_SEQ - L; k++) {
                got_t g;
                dv_reassembler_t r;
                snprintf(ctx, sizeof(ctx), "pal=%d lost %d at offset %d of the last sequence",
                         pal, L, k);
                int at = (3 * n + n - 1) * EP88_PACKETS_PER_SEQ + k;
                run(pal, F, at, L, NULL, &g, &r);
                CHECK_EQ(r.frames_lost, 0);
                CHECK_EQ(g.frames, F);
                CHECK(frame_intact(&g, 4, 4, n));
                CHECK(frame_intact(&g, 5, 5, n));
                CHECK(g.first[3][n - 2] == FILL(3));
                CHECK(r.sequences_dropped <= 1);
            }
        }
    }
}

/* Header-like bytes in the video data: no extra frames, no frame cut. */
static void test_false_headers(void)
{
    const int F = 6;
    opts_t o = { .plant_mask = 0x3F };
    for (int pal = 0; pal < 2; pal++) {
        int n = pal ? 12 : 10;
        got_t g;
        dv_reassembler_t r;
        snprintf(ctx, sizeof(ctx), "pal=%d clean", pal);
        run(pal, F, 0, 0, &o, &g, &r);
        CHECK_EQ(g.frames, F);
        CHECK_EQ(r.sequences_dropped, 0);
        CHECK_EQ(r.frames_lost, 0);
        for (int f = 0; f < F; f++) {
            CHECK_EQ(g.len[f], (size_t)n * EP88_SEQ_BYTES);
            CHECK(frame_intact(&g, f, f, n));
        }
        /* and with a lost packet, which makes the reassembler search */
        for (int k = 0; k < 2 * EP88_PACKETS_PER_SEQ; k++) {
            snprintf(ctx, sizeof(ctx), "pal=%d one packet lost at %d", pal, k);
            run(pal, F, 2 * n * EP88_PACKETS_PER_SEQ + k, 1, &o, &g, &r);
            CHECK_EQ(g.frames, F);
            CHECK_EQ(r.frames_lost, 0);
            for (int f = 0; f < F; f++)
                CHECK_EQ(g.len[f], (size_t)n * EP88_SEQ_BYTES);
            CHECK(frame_intact(&g, 5, 5, n));
        }
    }
}

/* Whole frames lost, aligned to a sequence or not. */
static void test_frames_lost_unaligned(void)
{
    const int F = 12;
    for (int pal = 0; pal < 2; pal++) {
        int n = pal ? 12 : 10;
        int per = n * EP88_PACKETS_PER_SEQ;
        for (int fr = 1; fr <= 3; fr++) {
            for (int extra = -24; extra <= 24; extra++) {
                int L = fr * per + extra;
                if (L < 1 || L % 256 == 0)
                    continue; /* a hole of a multiple of 256 blocks leaves the DBC unchanged */
                for (int k = 0; k < per; k += 13) {
                    got_t g;
                    dv_reassembler_t r;
                    snprintf(ctx, sizeof(ctx), "pal=%d lost %d at %d", pal, L, k);
                    run(pal, F, 3 * per + k, L, NULL, &g, &r);
                    /* a frame is lost when the hole took the first packet of each of its
                     * sequences; a damaged one that kept a header is still a frame */
                    int whole = 0;
                    for (int f = 0; f < F; f++) {
                        int all = 1;
                        for (int d = 0; d < n; d++) {
                            int hp = f * per + d * EP88_PACKETS_PER_SEQ;
                            if (hp < 3 * per + k || hp >= 3 * per + k + L)
                                all = 0;
                        }
                        whole += all;
                    }
                    CHECK_EQ(r.frames_lost, whole);
                    if (L < 256)
                        CHECK_EQ(g.loss_blocks, L);
                    CHECK(frame_intact(&g, g.frames - 1, F - 1, n));
                }
            }
        }
    }
}

/* lost_blocks of holes of 128..255 blocks is their length. */
static void test_large_holes(void)
{
    static const int lens[] = { 127, 128, 129, 200, 250, 255 };
    for (int pal = 0; pal < 2; pal++) {
        int per = (pal ? 12 : 10) * EP88_PACKETS_PER_SEQ;
        for (unsigned i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
            got_t g;
            dv_reassembler_t r;
            snprintf(ctx, sizeof(ctx), "pal=%d hole %d", pal, lens[i]);
            run(pal, 10, 3 * per + 40, lens[i], NULL, &g, &r);
            CHECK_EQ(g.loss_blocks, lens[i]);
            CHECK_EQ(r.dbc_lost_blocks, lens[i]);
            CHECK_EQ(r.dbc_gaps, 1);
        }
    }
}

int main(void)
{
    test_large_holes();
    test_false_headers();
    test_last_sequence();
    test_frames_lost_unaligned();
    test_sweep();
    printf("OK: arbitrary holes counted exactly, no phantom or vanishing frames\n");
    return 0;
}
