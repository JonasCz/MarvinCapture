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
 * IEC 61883 transmit packetizer: what a DV or HDV camcorder puts on the bus, one isochronous
 * packet per 1394 bus cycle (8000 a second). The receiving side of the same format is
 * src/core/dv_reassembler.c, and the loopback test checks this file against it.
 *
 * A caller hands in one "frame period" at a time: pout_cip_frame() produces exactly that
 * period's cycles (320 at 25 fps; 266 or 267 at 29.97 fps, from an exact rational
 * accumulator so the average is 8000*1001/30000 and never drifts), data packets spread evenly
 * among empty (CIP-header-only) ones. pout_cip_blank() produces a period of empty packets.
 *
 * DV (61883-2): DBS 0x78 (120 quadlets = 480 bytes = 6 DIF blocks = one data block), FMT 0,
 *   FDF 0x80 for 625/50, 0x00 for 525/60. PAL sends 300 data packets per frame, NTSC 250.
 *   SYT is set in the first data packet of a frame (the one holding DIF block 0, the Dseq 0
 *   header), 0xFFFF in all the others.
 * HDV (61883-4): DBS 6, FN 3 (a 192-byte source packet is 8 data blocks of 24 bytes), SPH 1,
 *   FMT 0x20, FDF 0, SYT 0. A packet carries 0..3 source packets (4-byte timestamp + one
 *   188-byte TS packet), at most 8 + 576 = 584 bytes.
 * DBC counts data blocks (1 per DV packet, 8 per HDV source packet) modulo 256; an empty
 * packet carries the DBC of the next data packet.
 *
 * Guesses (no device has been tested): the transfer delay between the cycle a packet is sent
 * in and its presentation time (SYT / source packet timestamp) is POUT_CIP_DELAY_CYCLES; the
 * timestamp offsets of several source packets in one cycle are spread over the cycle; SID
 * is whatever the caller sets (our node's phy id).
 */

#ifndef POUT_CIP_H
#define POUT_CIP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* IEC 61883-1 default TRANSFER_DELAY is about 354 us = 2.8 cycles; 3 cycles is used here. */
#define POUT_CIP_DELAY_CYCLES 3u
/* The cycle timer counts seconds modulo 128, 8000 cycles each. */
#define POUT_CIP_CYCLES_WRAP (128u * 8000u)
#define POUT_CIP_DV_FRAME_PAL 144000u
#define POUT_CIP_DV_FRAME_NTSC 120000u
#define POUT_CIP_TS_PACKET 188u
#define POUT_CIP_HDV_MAX_SRC 3u /* source packets per cycle */

typedef enum { POUT_CIP_DV = 0, POUT_CIP_HDV } pout_cip_kind_t;

typedef struct {
    pout_cip_kind_t kind;
    int is_625;
    unsigned sid;       /* 6 bits */
    unsigned dbc;       /* DBC of the next data block */
    uint32_t cycle;     /* bus cycle of the next packet: seconds*8000 + cycle, modulo POUT_CIP_CYCLES_WRAP */
    unsigned ntsc_acc;  /* NTSC cycle accumulator, in 1/15 cycle */
    unsigned long packets, data_packets; /* statistics */
} pout_cip_t;

/* The callback gets one packet (CIP header + payload); nonzero return aborts with -1. */
typedef int (*pout_pkt_cb)(void *user, const uint8_t *pkt, unsigned len);

/* is_625: PAL/50 Hz (25 fps); otherwise 525/60 (30000/1001 fps). For HDV the same two rates. */
void pout_cip_init(pout_cip_t *c, pout_cip_kind_t kind, int is_625, unsigned sid);
void pout_cip_set_sid(pout_cip_t *c, unsigned sid);
/* Sets the bus cycle the next packet goes out in (seconds*8000 + cycle, i.e. the OHCI
 * IsochronousCycleTimer's 7-bit seconds and 13-bit count). */
void pout_cip_set_cycle(pout_cip_t *c, uint32_t cycle);
uint32_t pout_cip_cycle(const pout_cip_t *c);

/* Cycles the next frame period will take (320; 266 or 267), without advancing. */
unsigned pout_cip_next_period(const pout_cip_t *c);

/* One frame period. DV: len must be 144000 (625) / 120000 (525) bytes of DIF. HDV: len a
 * multiple of 188 (0 allowed), at most 3 per cycle of the period. Returns the number of
 * packets (cycles) produced, or -1 for malformed input or a failing callback. */
int pout_cip_frame(pout_cip_t *c, const uint8_t *data, size_t len, pout_pkt_cb cb, void *user);
/* One frame period of empty packets. Returns the packets produced or -1. */
int pout_cip_blank(pout_cip_t *c, pout_pkt_cb cb, void *user);

/* Size of the largest packet (buffer size for receivers). */
unsigned pout_cip_max_packet(void);

#ifdef __cplusplus
}
#endif

#endif /* POUT_CIP_H */
