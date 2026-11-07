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
 * The target side of our 1394 node, as a virtual deck needs it: plug registers (IEC 61883-1),
 * incoming requests -> responses, FCP command frames, and the AV/C config ROM. Pure: no device
 * access. The hardware glue (pout_hw_link.c) feeds it the AR records pinnacle_1394.c hands to
 * its request hook and sends back the response headers it builds, with p1394_submit().
 *
 * Plug registers (CSR 0xFFFFF0000900..):
 *   oMPR  0x900  data rate capability 31:30, broadcast channel base 29:24 (63), number of output
 *                plugs 4:0 (1). Read only.
 *   oPCR0 0x904  online 31, broadcast connection 30, point-to-point count 29:24, channel 21:16,
 *                data rate 15:14, overhead ID 13:10, payload (quadlets, CIP header included) 9:0
 *                (122 for DV, 146 for HDV: max packet bytes / 4). A controller changes 30, 29:24,
 *                21:16, 15:14 and 13:10 with a compare-swap lock; online and payload are ours.
 *   iMPR  0x980  data rate capability, number of input plugs (0 by default: the deck only plays,
 *                RECORD is rejected, so a controller finds nothing to connect an input to; with
 *                n_in_plugs = 1 an iPCR0 at 0x984 exists, with the same writable bits).
 * Transmission is wanted while oPCR0 is online and either the broadcast bit is set or the
 * point-to-point count is above 0 (pout_csr_tx()).
 *
 * Request handling: quadlet reads (and block reads inside the plug area), compare-swap locks
 * (extended tcode 2), FCP command writes (quadlet or block, to 0xFFFFF0000B00). Everything else
 * gets a response with rcode address_error (type_error for the wrong request type on a plug
 * register, the right address with an unsupported lock). Reads of the configuration ROM and
 * the IRM registers never get here: the OHCI core answers them.
 *
 * Header formats (what p1394_submit() takes, OHCI AT format, host order):
 *   q0 = tlabel << 10 | rt << 8 | tcode << 4     (rt 1, as pinnacle_1394.c's write response)
 *   q1 = destination << 16 | rcode << 12          (responses; requests: destination << 16 | 0xffff)
 *   q2 = 0                                        (responses; requests: the offset)
 *   q3 = quadlet data (read response) or length << 16 | extended tcode (block/lock response)
 * Quadlet data in q3 is stored so its memory bytes are the wire bytes (a big-endian value is
 * byte-swapped), the way pinnacle_1394.c does it for FCP quadlet writes and reads them back.
 *
 * Guesses (no device has been tested): the AT header layout for read/lock responses and the
 * quadlet byte order above; that a controller finds the node from the AV/C unit directory
 * alone; the plug register field widths for HDV; the ROM CRCs follow IEEE 1212 (the vendor's
 * ROM uses the byte order its driver wrongly assumes).
 */

#ifndef POUT_CSR_H
#define POUT_CSR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CSR offsets (low 32 bits; the high 16 are 0xffff) */
#define POUT_CSR_OMPR 0xf0000900u
#define POUT_CSR_OPCR0 0xf0000904u
#define POUT_CSR_IMPR 0xf0000980u
#define POUT_CSR_IPCR0 0xf0000984u
#define POUT_CSR_FCP_COMMAND 0xf0000b00u
#define POUT_CSR_FCP_RESPONSE 0xf0000d00u

/* tcodes */
#define POUT_TC_WRITE_QUADLET 0x0u
#define POUT_TC_WRITE_BLOCK 0x1u
#define POUT_TC_WRITE_RESP 0x2u
#define POUT_TC_READ_QUADLET 0x4u
#define POUT_TC_READ_BLOCK 0x5u
#define POUT_TC_READ_QUADLET_RESP 0x6u
#define POUT_TC_READ_BLOCK_RESP 0x7u
#define POUT_TC_LOCK 0x9u
#define POUT_TC_LOCK_RESP 0xbu

/* response codes */
#define POUT_RC_COMPLETE 0x0u
#define POUT_RC_CONFLICT 0x4u
#define POUT_RC_DATA_ERROR 0x5u
#define POUT_RC_TYPE_ERROR 0x6u
#define POUT_RC_ADDRESS_ERROR 0x7u

/* the ack our link sent for a request, from the AR trailer (P1394_EVT_ACK_PENDING) */
#define POUT_ACK_COMPLETE 0x11u
#define POUT_ACK_PENDING 0x12u

#define POUT_FCP_MAX 512u
#define POUT_RESP_PAYLOAD_MAX 128u   /* p1394_submit() takes at most 128 payload bytes */

/* oPCR / iPCR fields */
#define POUT_PCR_ONLINE (1u << 31)
#define POUT_PCR_BCAST (1u << 30)
#define POUT_PCR_P2P_SHIFT 24
#define POUT_PCR_P2P_MASK (0x3fu << 24)
#define POUT_PCR_CHAN_SHIFT 16
#define POUT_PCR_CHAN_MASK (0x3fu << 16)
#define POUT_PCR_RATE_SHIFT 14
#define POUT_PCR_RATE_MASK (3u << 14)
#define POUT_PCR_OVH_SHIFT 10
#define POUT_PCR_OVH_MASK (0xfu << 10)
#define POUT_PCR_PAYLOAD_MASK 0x3ffu
/* what a controller may change with a lock */
#define POUT_PCR_WRITABLE (POUT_PCR_BCAST | POUT_PCR_P2P_MASK | POUT_PCR_CHAN_MASK | POUT_PCR_RATE_MASK | POUT_PCR_OVH_MASK)

typedef struct {
    unsigned rate;             /* 0 S100, 1 S200, 2 S400: capability in oMPR/iMPR, initial oPCR rate */
    unsigned bcast_base;       /* broadcast channel base, 63 */
    unsigned payload_quadlets; /* oPCR payload field, see pout_csr_payload() */
    unsigned overhead_id;      /* 0 */
    unsigned n_in_plugs;       /* 0 or 1 */
} pout_csr_cfg_t;

typedef struct {
    pout_csr_cfg_t cfg;
    uint32_t ompr, opcr, impr, ipcr;
    int tx_on;                 /* tx state at the last event (for plug_changed) */
    unsigned tx_channel;
} pout_csr_t;

/* Fills cfg with S400, base 63, payload for max_packet bytes, overhead 0, no input plug. */
void pout_csr_cfg_default(pout_csr_cfg_t *cfg, unsigned max_packet_bytes);
/* oPCR payload field for packets of up to n bytes (CIP header included): n / 4, at most 1023. */
unsigned pout_csr_payload(unsigned max_packet_bytes);

void pout_csr_init(pout_csr_t *c, const pout_csr_cfg_t *cfg);

/* 1 if the plug asks for transmission; *channel (63 if none) and *speed (oPCR rate field, 0..2)
 * are what to transmit with. Either pointer may be NULL. */
int pout_csr_tx(const pout_csr_t *c, unsigned *channel, unsigned *speed);

/* A local "play" without a controller: sets (on) or clears the broadcast connection bit and the
 * channel in oPCR0, as if a controller had made a broadcast connection. */
void pout_csr_local_connect(pout_csr_t *c, unsigned channel, int on);

/* --- incoming requests --- */

typedef struct {
    unsigned tcode, tl, rt, ack;
    uint16_t dest, src;        /* destination_ID, source_ID (bus<<6 | node) */
    uint16_t offset_hi;
    uint32_t offset;           /* destination_offset low 32 bits */
    unsigned length;           /* data bytes: 4 for quadlet requests, the block length otherwise */
    unsigned ext_tcode;        /* locks */
    const uint8_t *data;       /* write data / lock arg+data in wire order, into the record; NULL for reads */
    uint32_t arg, datum;       /* compare-swap arg_value and data_value, host values */
} pout_req_t;

/* Parses an AR request record: header quadlets (host order, little-endian in the byte stream),
 * payload in wire order, trailer quadlet. 0 on success; -1 if it is not a request this module
 * knows (tcode not 0, 1, 4, 5 or 9) or the record is too short for what its header says. */
int pout_csr_parse(const uint8_t *rec, unsigned len, pout_req_t *r);

typedef struct {
    uint32_t hdr[4];
    unsigned hdr_len;                     /* 12 or 16 bytes, for p1394_submit() */
    uint8_t payload[POUT_RESP_PAYLOAD_MAX];
    unsigned plen;                        /* 0 = none */
} pout_resp_t;

typedef struct {
    const uint8_t *fcp;        /* an FCP command frame was written: into the record, valid as long as it */
    unsigned fcp_len;
    uint16_t fcp_src;          /* who wrote it: send the response to this node's FCP_RESPONSE */
    int plug_changed;          /* a lock changed whether/where we transmit */
} pout_csr_event_t;

/* Applies a request. Returns 1 if *resp is to be sent (its header already addresses the sender),
 * 0 if nothing is owed (a write that was acked complete). *ev is cleared first. */
int pout_csr_handle(pout_csr_t *c, const pout_req_t *r, pout_resp_t *resp, pout_csr_event_t *ev);

/* The header of the AV/C response: a write of the frame to node `dest`'s FCP_RESPONSE with the
 * given tlabel (a quadlet write for 4 bytes, a block write otherwise). 0, or -1 if the frame is
 * empty or longer than POUT_RESP_PAYLOAD_MAX. */
int pout_csr_fcp_write(pout_resp_t *out, unsigned tl, uint16_t dest, const uint8_t *frame,
                       unsigned len);

/* --- configuration ROM --- */

#define POUT_CSR_ROM_WORDS 38
/* The node's ROM with the AV/C unit directory (unit spec ID 0x00A02D, software version
 * 0x010001) and IEEE 1212 CRCs; the same layout as the capture ROM, built by
 * p1394_config_rom_build() in the core. */
void pout_csr_config_rom(uint32_t rom[POUT_CSR_ROM_WORDS], uint32_t guid_hi, uint32_t guid_lo);

#ifdef __cplusplus
}
#endif

#endif /* POUT_CSR_H */
