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
 * EP 0x06 message builder: isochronous transmit as the vendor driver does it. The device is an
 * OHCI-1394 controller whose IT DMA program the host writes into device RAM, packet by packet,
 * as type-8 messages on EP 0x06 (same framing as EP 0x02, docs/protocol.md). Everything here is
 * derived from MarvinBus64.sys (the decompilation, read function by function; the numbers in
 * the comments are Ghidra function names):
 *
 *   FUN_0002d7b0  IT setup: ring of 4 descriptor blocks of 0x30 bytes at RAM 0x16b0, 0x16e0,
 *                 0x1710, 0x1740; 4 payload buffers from 0x1770, spaced at the maximum packet
 *                 size rounded up to 4 bytes (the vendor forces 0x248 for HDV).
 *   FUN_0002db20  one packet (below).
 *   FUN_0002eb80  start: IT0.CommandPtr (0x20c) = block 0, then ContextControlSet (0x200) = 0x9000.
 *   FUN_0002e3c0  wake: ContextControlSet = 0x9000, a type-2 register write.
 *   FUN_0002c400  type 11.
 *
 * Packet k (0, 1, 2, ...) goes into ring slot k % 4 and sends, in this order:
 *   1. type 8: the packet (CIP header + payload, wire byte order) to payload buffer[slot]
 *   2. type 8: the 0x30-byte descriptor block for the slot: OUTPUT_MORE_IMMEDIATE (the isochronous
 *      header as immediate data) + OUTPUT_LAST, whose dataAddress carries the vendor bits: bit 31
 *      and the 6-bit sequence number k % 64 in bits 29:24. Blocks 0..2 branch to the next block
 *      (Z = 3); from block 3 on the branch is 0 (end of program) until the next block patches it.
 *   3. k >= 4: type 8, 4 bytes: patch the previous block's OUTPUT_LAST branch word to this block
 *      with Z = 3; then type 2: wake IT0 (ContextControlSet = 0x9000).
 *   4. k == 3 (the fourth packet; the decompile tests its counter == 2, which counts from -1): the
 *      one-time start: IT0.CommandPtr = block 0, ContextControlSet = run|wake. Blocks 0..3 are
 *      chained and queued by then.
 *   5. k >= 3: type 11, 0xb0000000 | first << 26 | ((k - 3) & 0x3f), first only on the first one.
 *      Most likely "stall the EP 0x06 stream until packet k-3 has been sent": the ring slot the
 *      next packet is written into is then free.
 *
 * Where the decompile disagrees with docs/handoff/playout-feasibility.md:
 *   - The isochronous header is ONE immediate quadlet, spd<<16 | tag<<14 | chan<<8 | 0xa<<4 | sy
 *     (OHCI's layout), then dataLength<<16 as the second. sy is 0 in the vendor driver.
 *   - OUTPUT_LAST has the s (status) bit only on the last packet of a client buffer that wants
 *     completion, not on every packet. Here s is set every status_every packets (default 64).
 *   - The start writes CommandPtr = 0x16b0 with Z = 0 (FUN_00018de0 passes ring[0] unmodified),
 *     not "block 0 | Z". start_z makes that adjustable; the default follows the decompile
 *     (GUESS: the FPGA ignores Z there, since it works for the vendor).
 *   - The start is at the fourth packet, not the third (see 4.).
 *
 * Byte order (the vendor's back-end B has both swap flags, DAT_0004e530 and DAT_0004e550, at 0,
 * and our link init clears HC.noByteSwapData as the vendor does): descriptor words go into the
 * message as little-endian 32-bit words, as pinnacle_1394.c's b_ram() does, and packet bytes are
 * copied unchanged, so they reach the wire in the order given: CIP header quadlets big-endian
 * (pout_cip produces them so). The immediate quadlets are descriptor words, little-endian too.
 * Messages are quadlet-aligned only if the packet length is a multiple of 4 (DV 488, HDV 8..584,
 * empty 8: always); anything else is refused.
 *
 * Nothing here has run against a device.
 */

#ifndef POUT_IT_H
#define POUT_IT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POUT_IT_RING 4u
#define POUT_IT_BLOCK_BYTES 0x30u
#define POUT_IT_RAM_BLOCKS 0x16b0u   /* FUN_0002d7b0: ring[0]; ring[i] = this + i * 0x30 */
#define POUT_IT_RAM_BUFFERS 0x1770u  /* = ring end; buffer[i] = this + i * align4(max_packet) */
#define POUT_IT_REG_CTRL_SET 0x200u  /* IT0 ContextControlSet (FUN_00014df0) */
#define POUT_IT_REG_CTRL_CLR 0x204u
#define POUT_IT_REG_CMDPTR 0x20cu
#define POUT_IT_CTX_RUN 0x8000u
#define POUT_IT_CTX_WAKE 0x1000u
#define POUT_IT_CTX_ACTIVE 0x0400u
#define POUT_IT_MAX_PACKET 0x248u    /* vendor maximum: 8 + 3 * 192 (HDV) */

typedef struct {
    /* configuration (set by pout_it_init, may be changed before the first packet) */
    unsigned channel;       /* 0..63 */
    unsigned tag;           /* 1 = CIP */
    unsigned speed;         /* OHCI spd: 0 S100, 1 S200, 2 S400 (the vendor maps its speed flags 1,2,4,8,16 to 0..4) */
    unsigned sy;            /* 0, as the vendor */
    unsigned max_packet;    /* bytes, multiple of 4: sizes the payload buffers */
    unsigned status_every;  /* s bit on every n-th packet, 0 = never */
    unsigned start_z;       /* Z of the start CommandPtr: 0 as the vendor driver (GUESS), 3 as OHCI would want */
    /* state */
    uint64_t count;         /* packets built since init: the next one is packet number `count` */
    int started;            /* the context start has been sent */
    int first_pending;      /* the next type 11 is the first one */
} pout_it_t;

/* channel/speed as above; max_packet 488 for DV, 584 for HDV (rounded up to a multiple of 4). */
void pout_it_init(pout_it_t *it, unsigned channel, unsigned speed, unsigned max_packet);

/* RAM address of descriptor block / payload buffer of a ring slot, and the end of the buffers. */
unsigned pout_it_block_addr(unsigned slot);
unsigned pout_it_buffer_addr(const pout_it_t *it, unsigned slot);
unsigned pout_it_ram_end(const pout_it_t *it);   /* the vendor's HDV layout reaches 0x20b0, past the self-ID buffer at 0x2000 (GUESS: harmless) */

/* Appends the messages for one isochronous packet (CIP header + payload, wire order). len is a
 * multiple of 4, 8..max_packet. Returns the bytes appended, 0 if the packet is malformed or cap
 * is too small (nothing is appended and the state does not advance). */
size_t pout_it_packet(pout_it_t *it, const uint8_t *pkt, unsigned len, uint8_t *out, size_t cap);

/* Upper bound of pout_it_packet() output for one packet (payload + messages). */
size_t pout_it_max_bytes_per_packet(const pout_it_t *it);

/* ContextControlClear = run (8 bytes) and the state back to "no packet yet", so the next packet
 * starts a fresh run. The caller then waits for IT0 ContextControl.active to drop (FUN_0002e450
 * writes the clear and polls the register until active and run are both 0). Returns the bytes
 * appended, 0 if cap is too small. */
size_t pout_it_stop(pout_it_t *it, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* POUT_IT_H */
