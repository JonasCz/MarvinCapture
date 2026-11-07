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

/* pout_cip: header fields, DBC, packets per frame, SYT, HDV source packet headers. */

#include "pout_cip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

typedef struct {
    unsigned n, data, empty, max_len;
    unsigned expect_dbc;     /* tracks the DBC rule */
    unsigned dbs_blocks;     /* data blocks per data unit: 1 per DV packet, 8 per HDV source packet */
    int hdv;
    unsigned syt_set;        /* DV packets with a SYT other than 0xFFFF */
    unsigned first_data_idx; /* index of the first data packet */
    unsigned first_syt;
    unsigned max_src;
    int bad;
    int keep;
    uint8_t (*pk)[600];
    unsigned *lens;
} cap_t;

static int on_pkt(void *user, const uint8_t *p, unsigned len)
{
    cap_t *c = (cap_t *)user;
    if (len < 8 || (p[0] & 0xC0) != 0 || (p[4] & 0x80) == 0)
        c->bad++;
    if (len > c->max_len)
        c->max_len = len;
    unsigned dbc = p[3];
    if (dbc != (c->expect_dbc & 0xFF))
        c->bad++;
    if (len == 8) {
        c->empty++;
    } else {
        if (c->data == 0)
            c->first_data_idx = c->n;
        c->data++;
        if (c->hdv) {
            unsigned k = (len - 8) / 192;
            if (len != 8 + k * 192 || k > 3)
                c->bad++;
            if (k > c->max_src) c->max_src = k;
            c->expect_dbc += 8 * k;
        } else {
            if (len != 488)
                c->bad++;
            c->expect_dbc += 1;
        }
    }
    if (c->keep && c->n < 1000) {
        memcpy(c->pk[c->n], p, len);
        c->lens[c->n] = len;
    }
    c->n++;
    return 0;
}

static void fill_dv(uint8_t *f, size_t len, unsigned seed)
{
    for (size_t i = 0; i < len; i++)
        f[i] = (uint8_t)(i * 7 + seed + (i >> 8));
}

static void test_dv(int pal)
{
    size_t len = pal ? 144000 : 120000;
    uint8_t *frame = (uint8_t *)malloc(len);
    pout_cip_t c;
    pout_cip_init(&c, POUT_CIP_DV, pal, 0x2A);
    pout_cip_set_cycle(&c, 0);
    static uint8_t pk[1000][600];
    static unsigned lens[1000];
    unsigned total_cycles = 0;
    for (int fr = 0; fr < 3; fr++) {
        cap_t cap;
        memset(&cap, 0, sizeof cap);
        cap.expect_dbc = (unsigned)fr * (pal ? 300 : 250);
        cap.keep = 1;
        cap.pk = pk;
        cap.lens = lens;
        fill_dv(frame, len, (unsigned)fr);
        int n = pout_cip_frame(&c, frame, len, on_pkt, &cap);
        unsigned want = pal ? 320 : (unsigned)((fr + 1) * 4004 / 15 - fr * 4004 / 15);
        CHECK(n == (int)want);
        CHECK(cap.n == want);
        CHECK(cap.data == (pal ? 300u : 250u));
        CHECK(cap.empty == want - cap.data);
        CHECK(cap.bad == 0);
        CHECK(cap.max_len == 488);
        CHECK(cap.first_data_idx == 0);
        /* header fields of the first (SYT) and of an empty one */
        const uint8_t *p = pk[0];
        CHECK(p[0] == 0x2A && p[1] == 0x78 && p[2] == 0x00);
        CHECK(p[4] == 0x80 && p[5] == (pal ? 0x80 : 0x00));
        unsigned pc = (total_cycles + POUT_CIP_DELAY_CYCLES) & 0xF;
        CHECK(p[6] == (pc << 4) && p[7] == 0x00);
        unsigned syts = 0, blk = 0;
        for (unsigned i = 0; i < cap.n; i++) {
            if (lens[i] == 488) {
                CHECK(memcmp(pk[i] + 8, frame + (size_t)blk * 480, 480) == 0);
                blk++;
            } else {
                CHECK(pk[i][6] == 0xFF && pk[i][7] == 0xFF);
            }
            if (!(pk[i][6] == 0xFF && pk[i][7] == 0xFF))
                syts++;
            else if (lens[i] == 488 && i == 0)
                CHECK(0);
            /* only packet 0 of the frame carries a SYT */
            if (i > 0)
                CHECK(pk[i][6] == 0xFF && pk[i][7] == 0xFF);
        }
        CHECK(syts == 1 && blk == cap.data);
        /* data packets are spread: no gap longer than ceil(n/d)+1 cycles */
        unsigned gap = 0, maxgap = 0;
        for (unsigned i = 0; i < cap.n; i++) {
            if (lens[i] == 488) gap = 0; else if (++gap > maxgap) maxgap = gap;
        }
        CHECK(maxgap <= 1);
        total_cycles += cap.n;
        CHECK(pout_cip_cycle(&c) == total_cycles);
    }
    /* a blank period: empty packets with the DBC of the next data packet */
    cap_t cap;
    memset(&cap, 0, sizeof cap);
    cap.expect_dbc = 3 * (pal ? 300 : 250);
    int n = pout_cip_blank(&c, on_pkt, &cap);
    CHECK(n > 0 && cap.n == (unsigned)n && cap.data == 0 && cap.bad == 0);
    CHECK(n == (int)(pal ? 320 : (3 * 4004 + 4004) / 15 - 3 * 4004 / 15));
    /* malformed */
    CHECK(pout_cip_frame(&c, frame, len - 480, on_pkt, &cap) < 0);
    CHECK(pout_cip_frame(&c, frame, pal ? 120000 : 144000, on_pkt, &cap) < 0);
    free(frame);
}

static int fail_cb(void *u, const uint8_t *p, unsigned l)
{
    (void)u; (void)p; (void)l;
    return 1;
}

static void test_ntsc_long_run(void)
{
    static uint8_t frame[120000];
    fill_dv(frame, sizeof frame, 5);
    pout_cip_t c;
    pout_cip_init(&c, POUT_CIP_DV, 0, 1);
    cap_t cap;
    memset(&cap, 0, sizeof cap);
    unsigned long cycles = 0;
    for (int fr = 0; fr < 3000; fr++) {
        cap.data = cap.n = cap.empty = 0;
        int n = pout_cip_frame(&c, frame, sizeof frame, on_pkt, &cap);
        CHECK(n == 266 || n == 267);
        CHECK(cap.data == 250);
        cycles += (unsigned long)n;
        if (failures) break;
    }
    /* 3000 frames * 8000*1001/30000 cycles = 800800 exactly */
    CHECK(cycles == 800800);
    CHECK(cap.bad == 0);
    CHECK(pout_cip_frame(&c, frame, sizeof frame, fail_cb, NULL) < 0);
}

static void make_ts(uint8_t *ts, unsigned n, unsigned seed)
{
    for (unsigned i = 0; i < n; i++) {
        uint8_t *p = ts + (size_t)i * 188;
        p[0] = 0x47;
        for (unsigned j = 1; j < 188; j++)
            p[j] = (uint8_t)(i * 13 + j + seed);
    }
}

static void test_hdv(int pal)
{
    static uint8_t ts[960 * 188];
    static uint8_t pk[1000][600];
    static unsigned lens[1000];
    pout_cip_t c;
    pout_cip_init(&c, POUT_CIP_HDV, pal, 5);
    pout_cip_set_cycle(&c, 7990); /* the 13-bit cycle count wraps inside the test */
    unsigned period = pal ? 320 : 266;
    const unsigned counts[] = { 0, 1, 100, 400, 700 };
    unsigned long dbc = 0;
    uint32_t cycle = 7990;
    for (unsigned t = 0; t < sizeof counts / sizeof *counts; t++) {
        unsigned n = counts[t];
        make_ts(ts, n, t);
        cap_t cap;
        memset(&cap, 0, sizeof cap);
        cap.hdv = 1;
        cap.max_src = 0;
        cap.expect_dbc = (unsigned)dbc;
        cap.keep = 1;
        cap.pk = pk;
        cap.lens = lens;
        int r = pout_cip_frame(&c, ts, (size_t)n * 188, on_pkt, &cap);
        CHECK(r == (int)cap.n && cap.n >= period && cap.n <= period + 1);
        CHECK(cap.bad == 0);
        CHECK(cap.max_src <= 3 && cap.max_len <= 584);
        unsigned src = 0;
        for (unsigned i = 0; i < cap.n; i++) {
            const uint8_t *p = pk[i];
            CHECK(p[0] == 5 && p[1] == 0x06 && p[2] == 0xC4);
            CHECK(p[4] == 0xA0 && p[5] == 0 && p[6] == 0 && p[7] == 0);
            unsigned pc = (cycle + i + POUT_CIP_DELAY_CYCLES) % 8000;
            for (unsigned o = 8; o < lens[i]; o += 192, src++) {
                uint32_t sph = ((uint32_t)p[o] << 24) | (p[o + 1] << 16) | (p[o + 2] << 8) | p[o + 3];
                CHECK((sph >> 25) == 0);
                CHECK(((sph >> 12) & 0x1FFF) == pc);
                CHECK((sph & 0xFFF) < 3072);
                CHECK(memcmp(p + o + 4, ts + (size_t)src * 188, 188) == 0);
            }
        }
        CHECK(src == n);
        cycle += cap.n;
        dbc += 8ul * n;
    }
    CHECK(pout_cip_frame(&c, ts, 100, on_pkt, &(cap_t){ 0 }) < 0);          /* not a multiple of 188 */
    CHECK(pout_cip_frame(&c, ts, (size_t)961 * 188, on_pkt, &(cap_t){ 0 }) < 0); /* > 3 per cycle */
    CHECK(pout_cip_max_packet() == 584);
}

int main(void)
{
    test_dv(1);
    test_dv(0);
    test_ntsc_long_run();
    test_hdv(1);
    test_hdv(0);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_pout_cip: ok\n");
    return 0;
}
