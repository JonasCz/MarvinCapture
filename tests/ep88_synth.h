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
 * Builds a raw EP 0x88 byte stream in memory from DIF sequences, one by one, so a test can
 * lose any of them the way the transport does: the sequence is not written but the CIP
 * DBC counter goes on (make_ep88 only wraps whole files without losses). Each sequence is
 * 25 isochronous packets of 480 bytes (6 DIF blocks, DBS 0x78), each packet one
 * type-9 message holding one OHCI buffer-fill record.
 */

#ifndef PIN_TEST_EP88_SYNTH_H
#define PIN_TEST_EP88_SYNTH_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define EP88_SEQ_BYTES 12000
#define EP88_PACKET_BYTES 480
#define EP88_PACKETS_PER_SEQ 25

typedef struct {
    uint8_t *data;
    size_t len, cap;
    unsigned dbc;
} ep88_synth_t;

static inline void ep88_put(ep88_synth_t *w, const void *p, size_t n)
{
    if (w->len + n > w->cap) {
        w->cap = (w->len + n) * 2 + 4096;
        w->data = (uint8_t *)realloc(w->data, w->cap);
        if (!w->data)
            abort();
    }
    memcpy(w->data + w->len, p, n);
    w->len += n;
}

static inline void ep88_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* One 480-byte packet with the given DBC, as a type-9 message into the receive ring. */
static inline void ep88_packet(ep88_synth_t *w, const uint8_t *blocks, unsigned dbc)
{
    uint8_t rec[4 + 8 + EP88_PACKET_BYTES + 4];
    uint8_t msg[4];
    ep88_le32(rec, ((uint32_t)(8 + EP88_PACKET_BYTES) << 16) | (1u << 14) | (63u << 8) | (0xAu << 4));
    rec[4] = 0x01; rec[5] = 0x78; rec[6] = 0x00; rec[7] = (uint8_t)dbc;
    rec[8] = 0x80; rec[9] = 0x00; rec[10] = 0xFF; rec[11] = 0xFF;
    memcpy(rec + 12, blocks, EP88_PACKET_BYTES);
    ep88_le32(rec + 12 + EP88_PACKET_BYTES, 0x84110000u);
    ep88_le32(msg, (9u << 28) | ((uint32_t)sizeof(rec) << 16) | 0x7000u);
    ep88_put(w, msg, 4);
    ep88_put(w, rec, sizeof(rec));
}

/* A sequence (12,000 bytes) sent, or lost in transit: nothing written, the DBC goes on. */
static inline void ep88_sequence(ep88_synth_t *w, const uint8_t *seq, int lost)
{
    for (int i = 0; i < EP88_PACKETS_PER_SEQ; i++) {
        if (!lost)
            ep88_packet(w, seq + (size_t)i * EP88_PACKET_BYTES, w->dbc);
        w->dbc = (w->dbc + 1) & 0xFF;
    }
}

/* A DIF sequence number `dseq` of a frame: header block (DSF bit for PAL), the rest filled
 * with `fill`, so a test can tell which frame each sequence of the output came from. */
static inline void ep88_make_seq(uint8_t *seq, unsigned dseq, int pal, uint8_t fill)
{
    memset(seq, fill, EP88_SEQ_BYTES);
    seq[0] = 0x1F;
    seq[1] = (uint8_t)((dseq << 4) | 0x07);
    seq[2] = 0x00;
    seq[3] = pal ? 0x80 : 0x00;
}

#endif /* PIN_TEST_EP88_SYNTH_H */
