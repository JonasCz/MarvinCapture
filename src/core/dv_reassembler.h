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
 * Turns the raw EP 0x88 byte stream into a frame-aligned raw DV (.dv)
 * elementary stream that ffmpeg/VLC can read directly.
 *
 * EP 0x88 is NOT raw DV. The FPGA implements an OHCI-1394 host controller and
 * tunnels its isochronous-receive DMA over USB, so the stream has two layers
 * of framing on top of the DIF data (see docs/command-channel-findings.md):
 *
 *   1. Type-9 messages — the same command framing the command channel uses:
 *        u32 LE header = (9 << 28) | flags | (len << 16) | ram_addr
 *      followed by `len` bytes. ram_addr 0x7000..0xFFFF is the isochronous
 *      receive ring buffer; 0x118C / 0x119C are IR descriptor status
 *      writebacks and carry no stream data. Message lengths vary and do not
 *      align to anything in the payload, so a header can land in the middle
 *      of a DIF block — which is exactly why a naive "search for a DIF
 *      header, then take 12,000 bytes" reassembly produced misaligned video.
 *
 *   2. Inside the ring, OHCI buffer-fill records:
 *        [isoch header u32][payload, quadlet-padded][trailer u32]
 *      isoch header = dataLength<<16 | tag<<14 | chan<<8 | tcode<<4 | sy,
 *      trailer = xferStatus<<16 | timestamp. The payload is an IEC 61883
 *      packet: an 8-byte CIP header followed by the DIF data (480 bytes for
 *      DV, or nothing at all for the empty packets DV sends to pad its rate).
 *
 * Stripping both layers yields a byte-exact DIF stream: sequence headers land
 * exactly 12,000 bytes apart and every block ID, Dseq and DBN checks out.
 *
 * A DIF sequence is 150 * 80 = 12,000 bytes; a frame is 10 sequences (NTSC)
 * or 12 (PAL), and Dseq==0 starts a new frame. If a sequence is missing we
 * write 12,000 zero bytes in its place so every frame stays a fixed, aligned
 * size — without that, one dropped sequence would desync every later frame.
 */

#ifndef DV_REASSEMBLER_H
#define DV_REASSEMBLER_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    FILE *out;

    /* Layer 1: type-9 message demux over the raw EP 0x88 stream. */
    uint8_t msg_hdr[4];
    unsigned msg_hdr_have;
    unsigned msg_left;   /* payload bytes of the current message not yet consumed */
    int msg_is_ring;     /* does that payload belong to the isoch ring? */

    /* Layer 2: OHCI buffer-fill records over the ring byte stream. */
    uint8_t iso_hdr[12]; /* isoch header (4) + CIP header (8) */
    unsigned iso_hdr_have;
    unsigned iso_dif_left;  /* DIF bytes of this packet still to emit */
    unsigned iso_skip_left; /* quadlet padding + trailer still to skip */

    /* Layer 3: DIF sequence / frame assembly. */
    uint8_t *buf;
    size_t buf_len;
    size_t buf_cap;

    unsigned system_seq_count; /* 10 = NTSC, 12 = PAL; starts at 10, upgrades if seen */
    unsigned expected_dseq;
    int frame_started;
    /* State snapshotted at the start of the in-progress frame, so a capture
     * stopped mid-frame can drop that frame rather than leave a zero-padded
     * partial one — and so the counters stay honest when it does. */
    long frame_start_off;
    unsigned long frame_start_written;
    unsigned long frame_start_dropped;

    unsigned long frames_written;
    unsigned long sequences_written;
    unsigned long sequences_dropped;
    unsigned long bytes_fed;      /* raw EP 0x88 bytes in */
    unsigned long dif_bytes;      /* DIF bytes recovered */
    unsigned long iso_packets;    /* isochronous packets carrying DIF */
    unsigned long iso_empty;      /* empty (CIP-only) isochronous packets */
    unsigned long msg_resyncs;    /* layer-1 resynchronisations */
    unsigned long iso_resyncs;    /* layer-2 resynchronisations */

    /* CIP continuity (DBC). The IEC 61883 CIP header carries a data block
     * continuity counter that increments once per 480-byte DIF block sent,
     * modulo 256, across every packet in the connection. It is the only
     * lossless-delivery check available to us: the DIF layer can't tell a
     * dropped sequence from a camera that genuinely sent nothing, but a DBC
     * that jumps means data was lost in transit. Empty (CIP-only) packets
     * carry the *next* expected DBC and do not advance it. */
    int dbc_valid;                /* have we seen a first DBC to anchor on? */
    unsigned dbc_next;            /* DBC we expect on the next data block */
    unsigned long dbc_blocks;     /* DIF blocks seen with a DBC */
    unsigned long dbc_gaps;       /* discontinuities detected after lock-on */
    unsigned long dbc_lost_blocks;/* blocks implied missing by those gaps */
    unsigned long dbc_joins;      /* discontinuities seen while still syncing
                                   * in to the already-running ring; expected,
                                   * not loss */
} dv_reassembler_t;

int dv_reassembler_init(dv_reassembler_t *r, FILE *out);

/* Feed raw bytes as read off EP 0x88, in order. Writes completed,
 * frame-aligned DIF sequences to `out` as they're found. Returns 0 on
 * success, -1 on a write error. */
int dv_reassembler_feed(dv_reassembler_t *r, const uint8_t *data, size_t len);

/* Flushes any in-progress frame (zero-padding missing trailing sequences)
 * and frees internal buffers. Call once at end of capture. */
void dv_reassembler_finish(dv_reassembler_t *r);

#ifdef __cplusplus
}
#endif

#endif /* DV_REASSEMBLER_H */
