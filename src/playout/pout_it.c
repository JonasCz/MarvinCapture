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

#include "pout_it.h"

#include <string.h>

/* message types (docs/protocol.md) */
#define MSG_REG_WRITE 0x2u
#define MSG_RAM_WRITE 0x8u
#define MSG_IT_WAIT   0xbu
#define OHCI_BASE     0x10000u

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static unsigned align4(unsigned n) { return (n + 3u) & ~3u; }

void pout_it_init(pout_it_t *it, unsigned channel, unsigned speed, unsigned max_packet)
{
    memset(it, 0, sizeof(*it));
    it->channel = channel & 0x3f;
    it->tag = 1;
    it->speed = speed & 7;
    it->sy = 0;
    it->max_packet = align4(max_packet < 8 ? 8 : max_packet);
    it->status_every = 64;
    it->start_z = 0;
    it->first_pending = 1;
}

unsigned pout_it_block_addr(unsigned slot)
{
    return POUT_IT_RAM_BLOCKS + (slot % POUT_IT_RING) * POUT_IT_BLOCK_BYTES;
}

unsigned pout_it_buffer_addr(const pout_it_t *it, unsigned slot)
{
    return POUT_IT_RAM_BUFFERS + (slot % POUT_IT_RING) * align4(it->max_packet);
}

unsigned pout_it_ram_end(const pout_it_t *it)
{
    return POUT_IT_RAM_BUFFERS + POUT_IT_RING * align4(it->max_packet);
}

size_t pout_it_max_bytes_per_packet(const pout_it_t *it)
{
    /* payload message (4 + len) + block message (4 + 0x30) + at most 16 bytes of patch+wake or
     * of CommandPtr+wake + a type 11 (4) */
    return 4u + it->max_packet + 4u + POUT_IT_BLOCK_BYTES + 16u + 4u;
}

static uint8_t *put_reg(uint8_t *p, unsigned reg, uint32_t val)
{
    put32(p, (MSG_REG_WRITE << 28) | (OHCI_BASE + reg));
    put32(p + 4, val);
    return p + 8;
}

size_t pout_it_packet(pout_it_t *it, const uint8_t *pkt, unsigned len, uint8_t *out, size_t cap)
{
    if (!pkt || len < 8 || len > it->max_packet || (len & 3) || len > 0x7ff ||
        cap < pout_it_max_bytes_per_packet(it))
        return 0;

    uint64_t k = it->count;
    unsigned slot = (unsigned)(k % POUT_IT_RING);
    unsigned prev = (unsigned)((k + POUT_IT_RING - 1) % POUT_IT_RING);
    unsigned next = (unsigned)((k + 1) % POUT_IT_RING);
    unsigned seq = (unsigned)(k % 64);
    uint8_t *p = out;

    /* 1. FUN_0002b600 header (0x80000000 | len << 16 | addr) + the packet bytes, unchanged */
    put32(p, 0x80000000u | (len << 16) | pout_it_buffer_addr(it, slot)); p += 4;
    memcpy(p, pkt, len); p += len;

    /* 2. the descriptor block, FUN_0002db20 + FUN_000362f0 (MORE_IMMEDIATE) + FUN_00036400 (LAST) */
    uint32_t w[12];
    memset(w, 0, sizeof(w));
    w[0] = 0x02000000u | 8u;                                  /* cmd 0, key 2 (immediate), reqCount 8 */
    /* branch word of the MORE_IMMEDIATE: unused by OHCI (b = 0); the vendor fills it with the next
     * block for a CIP-only packet and with this block otherwise, Z = 3 */
    w[2] = ((len == 8 ? pout_it_block_addr(next) : pout_it_block_addr(slot)) & ~0xfu) | 3u;
    /* immediate quadlet 0: spd << 16 | tag << 14 | channel << 8 | tcode 0xa << 4 | sy */
    w[4] = 0xa0u | ((it->channel & 0x3f) << 8) | ((it->tag & 3) << 14) | ((it->speed & 7) << 16) |
           (it->sy & 0xf);
    w[5] = len << 16;                                         /* quadlet 1: dataLength */
    /* OUTPUT_LAST: cmd 1, s, i = 0, b = 3, reqCount = len */
    int status = it->status_every && ((k + 1) % it->status_every) == 0;
    w[8] = 0x10000000u | (status ? 1u << 27 : 0u) | 0x000c0000u | len;
    w[9] = 0x80000000u | (seq << 24) | pout_it_buffer_addr(it, slot);   /* vendor bits */
    w[10] = k < 3 ? (pout_it_block_addr(next) & ~0xfu) | 3u : 0u;       /* branch: next block, else end */
    put32(p, 0x80000000u | (POUT_IT_BLOCK_BYTES << 16) | pout_it_block_addr(slot)); p += 4;
    for (unsigned i = 0; i < 12; i++, p += 4)
        put32(p, w[i]);

    /* 3. patch the previous block's OUTPUT_LAST branch (block + 8 + 0x20) to this block, wake */
    if (k >= 4) {
        put32(p, 0x80000000u | (4u << 16) | (pout_it_block_addr(prev) + 8u + 0x20u)); p += 4;
        put32(p, (pout_it_block_addr(slot) & ~0xfu) | 3u); p += 4;
        p = put_reg(p, POUT_IT_REG_CTRL_SET, POUT_IT_CTX_RUN | POUT_IT_CTX_WAKE);
    }

    /* 4. the one-time start (FUN_00018de0 -> FUN_0002eb80) */
    if (k == 3 && !it->started) {
        p = put_reg(p, POUT_IT_REG_CMDPTR, pout_it_block_addr(0) | (it->start_z & 0xf));
        p = put_reg(p, POUT_IT_REG_CTRL_SET, POUT_IT_CTX_RUN | POUT_IT_CTX_WAKE);
        it->started = 1;
    }

    /* 5. type 11 (FUN_0002c400) */
    if (k >= 3) {
        put32(p, 0xb0000000u | ((it->first_pending ? 1u : 0u) << 26) | (unsigned)((k - 3) & 0x3f));
        p += 4;
        it->first_pending = 0;
    }

    it->count++;
    return (size_t)(p - out);
}

size_t pout_it_stop(pout_it_t *it, uint8_t *out, size_t cap)
{
    if (cap < 8)
        return 0;
    put_reg(out, POUT_IT_REG_CTRL_CLR, POUT_IT_CTX_RUN);
    it->count = 0;
    it->started = 0;
    it->first_pending = 1;
    return 8;
}
