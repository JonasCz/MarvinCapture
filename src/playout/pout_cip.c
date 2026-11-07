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

#include "pout_cip.h"

#include <string.h>

#define DV_PACKET_BYTES 480u
#define SRC_PACKET_BYTES 192u
#define CIP_BYTES 8u

/* 8000*1001/30000 = 4004/15 cycles per NTSC frame */
#define NTSC_NUM 4004u
#define NTSC_DEN 15u

unsigned pout_cip_max_packet(void)
{
    return CIP_BYTES + POUT_CIP_HDV_MAX_SRC * SRC_PACKET_BYTES;
}

void pout_cip_init(pout_cip_t *c, pout_cip_kind_t kind, int is_625, unsigned sid)
{
    memset(c, 0, sizeof *c);
    c->kind = kind;
    c->is_625 = is_625 != 0;
    c->sid = sid & 0x3F;
}

void pout_cip_set_sid(pout_cip_t *c, unsigned sid)
{
    c->sid = sid & 0x3F;
}

void pout_cip_set_cycle(pout_cip_t *c, uint32_t cycle)
{
    c->cycle = cycle % POUT_CIP_CYCLES_WRAP;
}

uint32_t pout_cip_cycle(const pout_cip_t *c)
{
    return c->cycle;
}

unsigned pout_cip_next_period(const pout_cip_t *c)
{
    if (c->is_625)
        return 320;
    return (c->ntsc_acc + NTSC_NUM) / NTSC_DEN;
}

/* Consumes the period: the cycles it takes, advancing the NTSC accumulator. */
static unsigned take_period(pout_cip_t *c)
{
    unsigned n = pout_cip_next_period(c);
    if (!c->is_625)
        c->ntsc_acc = (c->ntsc_acc + NTSC_NUM) % NTSC_DEN;
    return n;
}

static void put_cip(const pout_cip_t *c, uint8_t *p, unsigned syt)
{
    p[0] = (uint8_t)(c->sid & 0x3F); /* EOH0 = 0 */
    p[3] = (uint8_t)c->dbc;
    if (c->kind == POUT_CIP_DV) {
        p[1] = 0x78;
        p[2] = 0x00;                               /* FN 0, QPC 0, SPH 0 */
        p[4] = 0x80;                               /* EOH1 = 1, FMT 0x00 */
        p[5] = c->is_625 ? 0x80 : 0x00;            /* FDF: 50/60 bit, STYPE 0 */
        p[6] = (uint8_t)(syt >> 8);
        p[7] = (uint8_t)syt;
    } else {
        p[1] = 0x06;
        p[2] = (3u << 6) | (1u << 2);              /* FN 3, QPC 0, SPH 1 */
        p[4] = 0x80 | 0x20;                        /* FMT 0x20 */
        p[5] = 0x00;                               /* FDF: TSF 0 */
        p[6] = 0x00;                               /* SYT 0 */
        p[7] = 0x00;
    }
}

static void advance_cycle(pout_cip_t *c)
{
    c->cycle = (c->cycle + 1) % POUT_CIP_CYCLES_WRAP;
    c->packets++;
}

static int emit_empty(pout_cip_t *c, pout_pkt_cb cb, void *user)
{
    uint8_t pkt[CIP_BYTES];
    put_cip(c, pkt, 0xFFFF);
    advance_cycle(c);
    return cb(user, pkt, CIP_BYTES) ? -1 : 0;
}

int pout_cip_blank(pout_cip_t *c, pout_pkt_cb cb, void *user)
{
    unsigned n = take_period(c);
    for (unsigned i = 0; i < n; i++)
        if (emit_empty(c, cb, user) < 0)
            return -1;
    return (int)n;
}

static int frame_dv(pout_cip_t *c, const uint8_t *data, size_t len, pout_pkt_cb cb, void *user)
{
    size_t want = c->is_625 ? POUT_CIP_DV_FRAME_PAL : POUT_CIP_DV_FRAME_NTSC;
    if (len != want || !data)
        return -1;
    unsigned d = (unsigned)(want / DV_PACKET_BYTES);
    unsigned n = take_period(c);
    uint8_t pkt[CIP_BYTES + DV_PACKET_BYTES];
    unsigned sent = 0;
    for (unsigned i = 0; i < n; i++) {
        /* a data packet at cycle i when floor(i*d/n) steps: (i*d) mod n < d */
        if (((unsigned long)i * d) % n < d && sent < d) {
            unsigned syt = 0xFFFF;
            if (sent == 0) {
                unsigned pc = (c->cycle + POUT_CIP_DELAY_CYCLES) % 8000u;
                syt = ((pc & 0xFu) << 12); /* cycle offset 0 */
            }
            put_cip(c, pkt, syt);
            memcpy(pkt + CIP_BYTES, data + (size_t)sent * DV_PACKET_BYTES, DV_PACKET_BYTES);
            c->dbc = (c->dbc + 1) & 0xFF;
            c->data_packets++;
            sent++;
            advance_cycle(c);
            if (cb(user, pkt, CIP_BYTES + DV_PACKET_BYTES))
                return -1;
        } else if (emit_empty(c, cb, user) < 0) {
            return -1;
        }
    }
    return (int)n;
}

static int frame_hdv(pout_cip_t *c, const uint8_t *data, size_t len, pout_pkt_cb cb, void *user)
{
    if (len % POUT_CIP_TS_PACKET || (len && !data))
        return -1;
    unsigned long total = len / POUT_CIP_TS_PACKET;
    unsigned n = pout_cip_next_period(c);
    if (total > (unsigned long)n * POUT_CIP_HDV_MAX_SRC)
        return -1;
    take_period(c);
    uint8_t pkt[CIP_BYTES + POUT_CIP_HDV_MAX_SRC * SRC_PACKET_BYTES];
    unsigned long done = 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned long upto = (unsigned long)(i + 1) * total / n;
        unsigned k = (unsigned)(upto - done);
        if (k == 0) {
            if (emit_empty(c, cb, user) < 0)
                return -1;
            continue;
        }
        put_cip(c, pkt, 0);
        unsigned pc = (c->cycle + POUT_CIP_DELAY_CYCLES) % 8000u;
        uint8_t *p = pkt + CIP_BYTES;
        for (unsigned j = 0; j < k; j++, p += SRC_PACKET_BYTES) {
            /* 25-bit timestamp: 13-bit cycle count, 12-bit offset (0..3071) spread over the cycle */
            uint32_t ts = ((uint32_t)pc << 12) | (j * (3072u / POUT_CIP_HDV_MAX_SRC));
            p[0] = (uint8_t)(ts >> 24);
            p[1] = (uint8_t)(ts >> 16);
            p[2] = (uint8_t)(ts >> 8);
            p[3] = (uint8_t)ts;
            memcpy(p + 4, data + (size_t)(done + j) * POUT_CIP_TS_PACKET, POUT_CIP_TS_PACKET);
        }
        done = upto;
        c->dbc = (c->dbc + 8u * k) & 0xFF;
        c->data_packets++;
        advance_cycle(c);
        if (cb(user, pkt, CIP_BYTES + k * SRC_PACKET_BYTES))
            return -1;
    }
    return (int)n;
}

int pout_cip_frame(pout_cip_t *c, const uint8_t *data, size_t len, pout_pkt_cb cb, void *user)
{
    return c->kind == POUT_CIP_DV ? frame_dv(c, data, len, cb, user) : frame_hdv(c, data, len, cb, user);
}
