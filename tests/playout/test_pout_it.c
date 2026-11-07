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

/* pout_it: golden vectors for the first packets of the EP 0x06 stream, derived by hand from
 * MarvinBus64.sys (FUN_0002db20 and its helpers; see the comments in pout_it.h). Every expected
 * quadlet names where it comes from. Setup: channel 5, tag 1, S400, sy 0, max packet 488 (DV),
 * status_every 0 for the vectors (s bit checked separately).
 *   ring blocks   0x16b0 0x16e0 0x1710 0x1740
 *   payload bufs  0x1770 0x1958 0x1b40 0x1d28   (0x1770 + n * 0x1e8)
 *   immediate q0  0xa0 | 5 << 8 | 1 << 14 | 2 << 16 = 0x000245a0 */

#include "pout_it.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Compares the next quadlets of the stream with the expected ones; cursor-based. */
typedef struct { const uint8_t *p; size_t n, o; } cur_t;

static void expect(cur_t *c, uint32_t want, const char *what)
{
    if (c->o + 4 > c->n) {
        printf("FAIL: stream ended before %s\n", what);
        g_failures++;
        c->o = c->n;
        return;
    }
    uint32_t got = rd32(c->p + c->o);
    c->o += 4;
    if (got != want) {
        printf("FAIL %s: got 0x%08x want 0x%08x\n", what, got, want);
        g_failures++;
    }
}

static void expect_bytes(cur_t *c, const uint8_t *want, unsigned n, const char *what)
{
    if (c->o + n > c->n || memcmp(c->p + c->o, want, n) != 0) {
        printf("FAIL %s: payload bytes differ or missing\n", what);
        g_failures++;
    }
    c->o += n;
}

static void make_pkt(uint8_t *b, unsigned len, unsigned salt)
{
    for (unsigned i = 0; i < len; i++)
        b[i] = (uint8_t)(i * 7 + salt);
}

/* the block message common part: header, then 12 words */
static void expect_block(cur_t *c, unsigned blk, uint32_t w2, uint32_t len, uint32_t w8, uint32_t w9,
                         uint32_t w10, const char *n)
{
    expect(c, 0x80000000u | (0x30u << 16) | blk, "block msg header (FUN_0002b600: 0x80000000|0x30<<16|addr)");
    expect(c, 0x02000008u, "MORE_IMMEDIATE word0 (FUN_000362f0: key 2, reqCount 8)");
    expect(c, 0, "MORE_IMMEDIATE word1");
    expect(c, w2, "MORE_IMMEDIATE branch|3 (FUN_000362f0 param_3|3)");
    expect(c, 0, "MORE_IMMEDIATE word3");
    expect(c, 0x000245a0u, "immediate q0 (FUN_0002db20 local_38)");
    expect(c, len << 16, "immediate q1 dataLength<<16 (local_34)");
    expect(c, 0, "immediate pad 0");
    expect(c, 0, "immediate pad 1");
    expect(c, w8, "OUTPUT_LAST word0 (FUN_00036400: cmd 1, b 3, reqCount)");
    expect(c, w9, "OUTPUT_LAST dataAddress|vendor bits (bit 31, seq<<24)");
    expect(c, w10, "OUTPUT_LAST branch|Z");
    expect(c, 0, "OUTPUT_LAST status word");
    (void)n;
}

static void test_first_packets(void)
{
    pout_it_t it;
    pout_it_init(&it, 5, 2, 488);
    it.status_every = 0;
    CHECK(pout_it_max_bytes_per_packet(&it) == 4 + 488 + 4 + 0x30 + 16 + 4, "max bytes per packet");
    CHECK(pout_it_buffer_addr(&it, 3) == 0x1d28 && pout_it_block_addr(3) == 0x1740, "ring addresses");

    uint8_t data[488], empty[8], out[1024];
    make_pkt(data, 488, 1);
    make_pkt(empty, 8, 9);

    /* packet 0: data, slot 0 */
    size_t n = pout_it_packet(&it, data, 488, out, sizeof(out));
    CHECK(n == 4 + 488 + 4 + 0x30, "packet 0 size");
    cur_t c = { out, n, 0 };
    expect(&c, 0x81e81770u, "p0 payload msg header (0x80000000|488<<16|0x1770)");
    expect_bytes(&c, data, 488, "p0 payload");
    expect_block(&c, 0x16b0, 0x000016b3u, 488, 0x100c01e8u, 0x80001770u, 0x000016e3u, "p0");
    CHECK(c.o == n, "p0 no more messages (no patch, no start, no type 11)");

    /* packet 1: CIP-only (8 bytes), slot 1: the MORE_IMMEDIATE branch names the next block */
    n = pout_it_packet(&it, empty, 8, out, sizeof(out));
    CHECK(n == 4 + 8 + 4 + 0x30, "packet 1 size");
    c = (cur_t){ out, n, 0 };
    expect(&c, 0x80081958u, "p1 payload msg header (0x80000000|8<<16|0x1958)");
    expect_bytes(&c, empty, 8, "p1 payload");
    expect_block(&c, 0x16e0, 0x00001713u, 8, 0x100c0008u, 0x81001958u, 0x00001713u, "p1");
    CHECK(c.o == n, "p1 no more messages");

    /* packet 2: data, slot 2, still chained to block 3 */
    n = pout_it_packet(&it, data, 488, out, sizeof(out));
    c = (cur_t){ out, n, 0 };
    expect(&c, 0x81e81b40u, "p2 payload msg header");
    expect_bytes(&c, data, 488, "p2 payload");
    expect_block(&c, 0x1710, 0x00001713u, 488, 0x100c01e8u, 0x82001b40u, 0x00001743u, "p2");
    CHECK(c.o == n, "p2 no more messages");

    /* packet 3: slot 3, the end of the program (branch 0); start + first type 11 */
    n = pout_it_packet(&it, data, 488, out, sizeof(out));
    c = (cur_t){ out, n, 0 };
    expect(&c, 0x81e81d28u, "p3 payload msg header");
    expect_bytes(&c, data, 488, "p3 payload");
    expect_block(&c, 0x1740, 0x00001743u, 488, 0x100c01e8u, 0x83001d28u, 0, "p3");
    expect(&c, 0x2001020cu, "start: reg write IT0.CommandPtr (0x10000+0x20c) (FUN_0002eb80 via FUN_0002b720)");
    expect(&c, 0x000016b0u, "start: CommandPtr = block 0, Z = 0 (FUN_00018de0 passes ring[0])");
    expect(&c, 0x20010200u, "start: reg write ContextControlSet (FUN_0002e3c0)");
    expect(&c, 0x00009000u, "start: run|wake");
    expect(&c, 0xb4000000u, "type 11: 0xb0000000 | first<<26 | (3-3) (FUN_0002c400)");
    CHECK(c.o == n, "p3 ends there");
    CHECK(it.started, "started");

    /* packet 4: slot 0 again; patch block 3's branch (0x1740+0x28), wake, type 11 (no first) */
    n = pout_it_packet(&it, data, 488, out, sizeof(out));
    c = (cur_t){ out, n, 0 };
    expect(&c, 0x81e81770u, "p4 payload msg header");
    expect_bytes(&c, data, 488, "p4 payload");
    expect_block(&c, 0x16b0, 0x000016b3u, 488, 0x100c01e8u, 0x84001770u, 0, "p4");
    expect(&c, 0x80041768u, "patch msg header: 0x80000000|4<<16|(0x1740+8+0x20)");
    expect(&c, 0x000016b3u, "patch data: block 0 | Z 3");
    expect(&c, 0x20010200u, "wake: ContextControlSet");
    expect(&c, 0x00009000u, "wake: 0x9000");
    expect(&c, 0xb0000001u, "type 11 seq 1, not first");
    CHECK(c.o == n, "p4 ends there");

    /* the sequence number wraps at 64 in the vendor bits and in type 11 */
    for (int i = 5; i < 70; i++) {
        n = pout_it_packet(&it, empty, 8, out, sizeof(out));
        if (i == 69) {   /* slot 1, previous block 0x16b0 */
            c = (cur_t){ out, n, 0 };
            expect(&c, 0x80081958u, "p69 payload msg header (slot 1)");
            c.o += 8;
            expect(&c, 0x803016e0u, "p69 block msg header");
            c.o += 8 * 4;   /* MORE_IMMEDIATE + immediate */
            expect(&c, 0x100c0008u, "p69 OUTPUT_LAST word0");
            expect(&c, 0x80000000u | (5u << 24) | 0x1958u, "p69 vendor seq = 69 % 64 = 5");
            c.o += 8;       /* branch, status */
            expect(&c, 0x80000000u | (4u << 16) | (0x16b0u + 0x28u), "p69 patch header");
            expect(&c, 0x000016e3u, "p69 patch data");
            c.o += 8;       /* wake */
            expect(&c, 0xb0000000u | (69 - 3) % 64, "p69 type 11 seq (69-3) & 0x3f = 2");
            CHECK(c.o == n, "p69 ends there");
        }
    }
}

static void test_status_and_limits(void)
{
    pout_it_t it;
    uint8_t data[488], out[1024];
    make_pkt(data, 488, 3);
    pout_it_init(&it, 63, 0, 488);
    CHECK(it.status_every == 64 && it.start_z == 0, "defaults");
    /* s bit (bit 27 of OUTPUT_LAST word 0) on packet 63 only */
    for (int i = 0; i < 65; i++) {
        size_t n = pout_it_packet(&it, data, 488, out, sizeof(out));
        uint32_t w8 = rd32(out + 4 + 488 + 4 + 8 * 4);
        CHECK(n > 0, "packet built");
        CHECK(((w8 >> 27) & 1) == (i == 63), "s bit only every 64th packet");
        CHECK((w8 & 0xffff) == 488, "reqCount");
    }
    /* refusals leave the state alone */
    uint64_t before = it.count;
    CHECK(pout_it_packet(&it, data, 492, out, sizeof(out)) == 0, "longer than max_packet");
    CHECK(pout_it_packet(&it, data, 6, out, sizeof(out)) == 0, "shorter than a CIP header");
    CHECK(pout_it_packet(&it, data, 10, out, sizeof(out)) == 0, "not a quadlet multiple");
    CHECK(pout_it_packet(&it, data, 488, out, 100) == 0, "cap too small");
    CHECK(it.count == before, "state unchanged by refusals");

    /* speed, channel, tag, sy end up in the immediate quadlet */
    pout_it_init(&it, 17, 1, 584);
    it.tag = 2; it.sy = 3;
    pout_it_packet(&it, data, 8, out, sizeof(out));
    uint32_t q0 = rd32(out + 4 + 8 + 4 + 4 * 4);
    CHECK(q0 == (0xa0u | (17u << 8) | (2u << 14) | (1u << 16) | 3u), "immediate q0 fields");
    CHECK(pout_it_ram_end(&it) == 0x1770 + 4 * 584, "HDV layout reaches 0x20b0");

    /* stop: ContextControlClear = run, then a fresh run */
    size_t n = pout_it_stop(&it, out, sizeof(out));
    CHECK(n == 8 && rd32(out) == 0x20010204u && rd32(out + 4) == 0x8000u, "stop message");
    CHECK(it.count == 0 && !it.started && it.first_pending, "stop resets the run");
    CHECK(pout_it_stop(&it, out, 4) == 0, "stop: cap too small");
}

int main(void)
{
    test_first_packets();
    test_status_and_limits();
    if (g_failures) {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_pout_it: OK\n");
    return 0;
}
