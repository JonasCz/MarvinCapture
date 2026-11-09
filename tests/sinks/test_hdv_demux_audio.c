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
 * hdv_demux audio on synthetic transport streams (PAT, PMT, MP2 audio PES; no video):
 *   - a frame the PES end cuts 1 to 4 bytes into it goes on in the next PES, with its time;
 *   - a PES whose last packet was lost does not glue its cut frame onto the next PES: the
 *     next PES's frames come out with their own times;
 *   - a continuity jump on a packet with the discontinuity indicator is no loss.
 * The frames are silent Layer II frames made by hand, each numbered in its data bytes.
 */

#include "test_util.h"
#include "../../src/sinks/hdv_demux.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TSP 188
#define PMT_PID 0x100
#define VIDEO_PID 0x810
#define AUDIO_PID 0x814
#define FRAME_BYTES 576   /* MPEG-1 Layer II, 192 kbit/s, 48 kHz, no padding */
#define FRAME_TICKS 2160  /* 1152 samples at 48 kHz in 90 kHz units */
#define PTS0 900000

typedef struct {
    int n;
    int idx[64];
    int64_t cam[64];
} got_t;

static void on_gop(void *user, hdv_gop_t *g)
{
    (void)user;
    hdv_gop_free(g);
}

static void on_audio(void *user, const uint8_t *f, int len, int64_t cam, int sr, int ch, int kbps)
{
    got_t *g = user;
    CHECK(len == FRAME_BYTES && sr == 48000 && ch == 2 && kbps == 192);
    CHECK(g->n < 64);
    g->idx[g->n] = f[8];
    g->cam[g->n++] = cam;
}

static void on_error(void *user, pin_status_t e)
{
    (void)user;
    (void)e;
    CHECK(!"no error expected");
}

typedef struct {
    uint8_t buf[256 * TSP];
    size_t n;     /* packets */
    unsigned cc;  /* audio PID */
} ts_t;

static uint8_t *ts_new(ts_t *t)
{
    CHECK(t->n < 256);
    uint8_t *p = t->buf + t->n++ * TSP;
    memset(p, 0xFF, TSP);
    return p;
}

static void psi(ts_t *t)
{
    uint8_t *p = ts_new(t);
    static const uint8_t pat[] = { 0x47, 0x40, 0x00, 0x10, 0x00,
                                   0x00, 0xB0, 0x0D, 0x00, 0x01, 0xC1, 0x00, 0x00,
                                   0x00, 0x01, 0xE0 | (PMT_PID >> 8), PMT_PID & 0xFF,
                                   0, 0, 0, 0 };
    memcpy(p, pat, sizeof(pat));
    p = ts_new(t);
    static const uint8_t pmt[] = { 0x47, 0x40 | (PMT_PID >> 8), PMT_PID & 0xFF, 0x10, 0x00,
                                   0x02, 0xB0, 0x17, 0x00, 0x01, 0xC1, 0x00, 0x00,
                                   0xE0 | (VIDEO_PID >> 8), VIDEO_PID & 0xFF, 0xF0, 0x00,
                                   0x02, 0xE0 | (VIDEO_PID >> 8), VIDEO_PID & 0xFF, 0xF0, 0x00,
                                   0x03, 0xE0 | (AUDIO_PID >> 8), AUDIO_PID & 0xFF, 0xF0, 0x00,
                                   0, 0, 0, 0 };
    memcpy(p, pmt, sizeof(pmt));
}

/* An audio PES with PTS `pts` carrying es[0..len), as many packets as it takes (the last one
 * stuffed). drop_last: its last packet is lost (the CC goes on). disc_at > 0: that packet
 * carries the discontinuity indicator and its CC jumps by 5. */
static void pes(ts_t *t, const uint8_t *es, size_t len, int64_t pts, int drop_last, int disc_at)
{
    uint8_t hdr[14] = { 0, 0, 1, 0xC0, 0, 0, 0x80, 0x80, 5 };
    hdr[9] = (uint8_t)(0x21 | ((pts >> 29) & 0x0E));
    hdr[10] = (uint8_t)(pts >> 22);
    hdr[11] = (uint8_t)(0x01 | ((pts >> 14) & 0xFE));
    hdr[12] = (uint8_t)(pts >> 7);
    hdr[13] = (uint8_t)(0x01 | ((pts << 1) & 0xFE));
    uint8_t all[8192];
    CHECK(len + sizeof(hdr) <= sizeof(all));
    memcpy(all, hdr, sizeof(hdr));
    memcpy(all + sizeof(hdr), es, len);
    size_t left = len + sizeof(hdr), at = 0;
    for (int k = 0; left > 0; k++) {
        int disc = disc_at > 0 && k == disc_at;
        size_t cap = disc ? 182 : 184;
        size_t take = left < cap ? left : cap;
        int last = take == left;
        if (disc)
            t->cc += 5;
        if (last && drop_last) {
            t->cc++;
            break;
        }
        uint8_t *p = ts_new(t);
        p[0] = 0x47;
        p[1] = (uint8_t)((k == 0 ? 0x40 : 0) | (AUDIO_PID >> 8));
        p[2] = AUDIO_PID & 0xFF;
        uint8_t *pl = p + 4;
        size_t af = 184 - take; /* adaptation field bytes, length byte included */
        if (af > 0) {
            p[3] = (uint8_t)(0x30 | (t->cc++ & 0x0F));
            pl[0] = (uint8_t)(af - 1);
            if (af > 1)
                pl[1] = disc ? 0x80 : 0x00;
            pl += af;
        } else {
            p[3] = (uint8_t)(0x10 | (t->cc++ & 0x0F));
        }
        memcpy(pl, all + at, take);
        at += take;
        left -= take;
    }
}

static void frames(uint8_t *es, int first, int n)
{
    memset(es, 0, (size_t)n * FRAME_BYTES);
    for (int i = 0; i < n; i++) {
        uint8_t *f = es + (size_t)i * FRAME_BYTES;
        f[0] = 0xFF;
        f[1] = 0xFD;
        f[2] = 0xA4;
        f[3] = 0x00;
        f[8] = (uint8_t)(first + i);
    }
}

static got_t demux(const ts_t *t)
{
    got_t g;
    memset(&g, 0, sizeof(g));
    hdv_demux_cb_t cb = { on_gop, on_audio, on_error, &g };
    hdv_demux_t *d = hdv_demux_create(&cb, 1);
    CHECK(d);
    hdv_demux_feed(d, t->buf, t->n * TSP);
    hdv_demux_flush(d);
    hdv_demux_free(d);
    return g;
}

/* Each frame once, in order, at its own time. */
static void expect(const got_t *g, const int *idx, int n)
{
    CHECK(g->n == n);
    for (int i = 0; i < n && i < g->n; i++) {
        if (g->idx[i] != idx[i] || g->cam[i] != PTS0 + (int64_t)idx[i] * FRAME_TICKS)
            fprintf(stderr, "  frame %d: index %d at %lld, want %d\n", i, g->idx[i],
                    (long long)g->cam[i], idx[i]);
        CHECK(g->idx[i] == idx[i]);
        CHECK(g->cam[i] == PTS0 + (int64_t)idx[i] * FRAME_TICKS);
    }
}

static void test_cut_tail(void)
{
    static const int all[] = { 0, 1, 2, 3, 4, 5 };
    uint8_t es[6 * FRAME_BYTES];
    frames(es, 0, 6);
    for (size_t cut = 1; cut <= 5; cut++) {
        ts_t *t = calloc(1, sizeof(*t));
        CHECK(t);
        psi(t);
        size_t first = 2 * FRAME_BYTES + cut;
        pes(t, es, first, PTS0, 0, 0);
        pes(t, es + first, sizeof(es) - first, PTS0 + 3 * FRAME_TICKS, 0, 0);
        got_t g = demux(t);
        printf("  cut %zu byte(s) into frame 2: %d frames\n", cut, g.n);
        expect(&g, all, 6);
        free(t);
    }
}

static void test_lost_last_packet(void)
{
    static const int want[] = { 0, 1, 3, 4, 5 };
    uint8_t es[6 * FRAME_BYTES];
    frames(es, 0, 6);
    ts_t *t = calloc(1, sizeof(*t));
    CHECK(t);
    psi(t);
    size_t first = 2 * FRAME_BYTES + 300; /* the lost last packet holds part of frame 2 */
    pes(t, es, first, PTS0, 1, 0);
    pes(t, es + first, sizeof(es) - first, PTS0 + 3 * FRAME_TICKS, 0, 0);
    got_t g = demux(t);
    printf("  last packet of a PES lost: %d frames\n", g.n);
    expect(&g, want, 5);
    free(t);
}

static void test_discontinuity_indicator(void)
{
    static const int all[] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    uint8_t es[8 * FRAME_BYTES];
    frames(es, 0, 8);
    ts_t *t = calloc(1, sizeof(*t));
    CHECK(t);
    psi(t);
    pes(t, es, 4 * FRAME_BYTES, PTS0, 0, 5);
    pes(t, es + 4 * FRAME_BYTES, 4 * FRAME_BYTES, PTS0 + 4 * FRAME_TICKS, 0, 0);
    got_t g = demux(t);
    printf("  continuity jump with the discontinuity indicator: %d frames\n", g.n);
    expect(&g, all, 8);
    free(t);
}

int main(void)
{
    test_cut_tail();
    test_lost_last_packet();
    test_discontinuity_indicator();
    printf("test_hdv_demux_audio: ok\n");
    return 0;
}
