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
 * IEEE 1394 link layer over the Pinnacle's USB tunnel.
 *
 * The FPGA is an OHCI-1394 controller whose registers and DMA RAM are reached
 * through messages on EP 0x02 (see docs/protocol.md). This
 * module turns that into ordinary 1394 operations: OHCI register access,
 * asynchronous transactions (quadlet read, lock, write, write response) and
 * FCP/AV-C commands. docs/startup.md and docs/deck-control.md describe the
 * wire formats.
 *
 * EP 0x84 is read only from inside these calls (p1394_pump), so none of this
 * is thread-safe and nothing else may read EP 0x84 while it is in use. It
 * does not read EP 0x88: while an isochronous receive context is running,
 * something else must drain it or the command channel stalls.
 */

#ifndef PINNACLE_1394_H
#define PINNACLE_1394_H

#include "pinnacle_device.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CSR addresses (IEEE 1212 / IEC 61883-1 / AV/C), offset within 0xFFFF_xxxx_xxxx */
#define P1394_CSR_CONFIG_ROM   0xf0000400u
#define P1394_CSR_OMPR         0xf0000900u   /* output master plug register */
#define P1394_CSR_OPCR(n)      (0xf0000904u + 4u * (n))
#define P1394_CSR_IMPR         0xf0000980u
#define P1394_CSR_FCP_COMMAND  0xf0000b00u
#define P1394_CSR_FCP_RESPONSE 0xf0000d00u

/* OHCI event codes (xferStatus bits 4:0 / AR trailer) */
#define P1394_EVT_MISSING_ACK  0x03
#define P1394_EVT_ACK_COMPLETE 0x11
#define P1394_EVT_ACK_PENDING  0x12

#define P1394_FCP_SLOTS 16

typedef struct {
    pinnacle_device_t *dev;
    int verbose;
    uint16_t local_node;       /* 0xffc0 | our node number, from NodeID */
    int node_count;            /* from SelfIDCount */
    unsigned next_tl;
    const char *step;          /* what p1394_link_init()/p1394_ir_start() was doing; for error reports */
    int last_usb_rc;           /* last libusb error seen by p1394_send/p1394_pump (0 = none) */

    /* filled in by p1394_pump() */
    uint16_t at_status_addr;
    int at_evt;                /* -1 until the xferStatus writeback arrives */
    int reg_seq;
    int reg_seq_before;        /* reg_seq when p1394_reg_read_begin() sent its request */
    uint32_t reg_addr, reg_val;
    int vreg_seq;              /* type-5 (vendor register) read replies */
    uint32_t vreg_val;
    /* last response packet received into the AR response buffer */
    int rsp_seq;
    unsigned rsp_tl, rsp_tcode, rsp_rcode;
    uint16_t rsp_src;
    uint8_t rsp_data[8];
    /* FCP responses written to us (ring), and write responses we owe */
    int fcp_seq;
    uint8_t fcp[P1394_FCP_SLOTS][512];
    int fcp_len[P1394_FCP_SLOTS];
    struct { uint8_t tl; uint16_t src; } owed[64];
    int n_owed;

    /* async AV/C (p1394_avc_begin/poll), one outstanding command at a time */
    int avc_pending;
    uint8_t avc_match_subunit, avc_match_opcode;
    int avc_fcp_seq0;
    unsigned avc_status_addr;
    unsigned long long avc_deadline_ms;
} pinnacle_1394_t;

void p1394_init(pinnacle_1394_t *l, pinnacle_device_t *dev);

/* Reads EP 0x84 once (up to timeout_ms) and processes what arrived.
 * Returns the number of bytes read, 0 on timeout, <0 on a USB error. */
int p1394_pump(pinnacle_1394_t *l, unsigned timeout_ms);

int p1394_send(pinnacle_1394_t *l, const uint8_t *msg, int len);    /* raw EP 0x02 */
int p1394_reg_write(pinnacle_1394_t *l, uint16_t ohci_off, uint32_t val);
int p1394_reg_read(pinnacle_1394_t *l, uint16_t ohci_off, uint32_t *val);

/* Stops the AT request context and waits for it to go idle. */
/* OHCI SelfIDCount: selfIDGeneration (23:16) changes with every bus reset, so
 * a change of this register means the bus topology may have changed. */
#define P1394_OHCI_SELF_ID_COUNT 0x068

/* Non-blocking OHCI register read for use while a read loop owns EP 0x84
 * (it feeds the reply to p1394_parse_ep84): _begin sends the request, _poll
 * returns 1 and fills *val once the reply has arrived, else 0. */
int p1394_reg_read_begin(pinnacle_1394_t *l, uint16_t off);
int p1394_reg_read_poll(pinnacle_1394_t *l, uint16_t off, uint32_t *val);
int p1394_at_reset(pinnacle_1394_t *l);

/* Reads NodeID and SelfIDCount into local_node / node_count. Waits (up to
 * ~1 s) for the NodeID to become valid after a bus reset. */
int p1394_read_topology(pinnacle_1394_t *l);

/* Sends one AT packet and returns its xferStatus event code (-1 on
 * timeout). hdr_len 16 for requests, 12 for write responses and quadlet
 * reads. payload (wire byte order) may be NULL. ack_busy is retried. */
int p1394_submit(pinnacle_1394_t *l, const uint32_t hdr[4], unsigned hdr_len,
                 const uint8_t *payload, unsigned plen);

/* Asynchronous transactions to node (0xffc0 | n). Values are the quadlet
 * as the 1394 spec writes it (big-endian on the wire). 0 on success. */
int p1394_read_quadlet(pinnacle_1394_t *l, uint16_t node, uint32_t off, uint32_t *val);
int p1394_compare_swap(pinnacle_1394_t *l, uint16_t node, uint32_t off,
                       uint32_t arg, uint32_t data, uint32_t *old);

/* Answers every FCP response the camera wrote to us with a write response
 * (it retries until it gets one). */
void p1394_answer_owed(pinnacle_1394_t *l);

/* Sends an AV/C command frame to node's FCP_COMMAND register and waits up
 * to timeout_ms for the matching final response (INTERIM is waited past).
 * Returns the response length, or -1. */
int p1394_avc(pinnacle_1394_t *l, uint16_t node, const uint8_t *cmd, unsigned len,
              uint8_t *resp, unsigned resp_max, unsigned timeout_ms);

/* Processes bytes already read from EP 0x84 by someone else (the streaming
 * read loop's own async queue, see pinnacle_stream_read_loop_ex()). Same
 * parsing p1394_pump() does after its own blocking read; exposed separately
 * so exactly one thing ever issues the EP 0x84 libusb read while streaming
 * is active. */
void p1394_parse_ep84(pinnacle_1394_t *l, const uint8_t *p, int len);

/* Non-blocking AV/C: begin() writes the command (one EP 0x02 send, no wait)
 * and returns at once; poll() (call it every tick, after new EP 0x84 bytes
 * have gone through p1394_parse_ep84()) reports 0 while still waiting, the
 * response length once one arrives (matching p1394_avc()'s ctype-byte
 * convention, including NOT_IMPLEMENTED == busy, the caller's job to
 * retry), or -1 on send failure / timeout. One outstanding command per
 * link; begin() while another is still pending fails it silently (the
 * caller in engine/pin_deck.c never does this). */
int p1394_avc_begin(pinnacle_1394_t *l, uint16_t node, const uint8_t *cmd, unsigned len);
int p1394_avc_poll(pinnacle_1394_t *l, uint8_t *resp, unsigned resp_max);
/* Gives up on the outstanding async command, if any (e.g. the session is
 * closing). Returns 1 if one was in fact pending. */
int p1394_avc_cancel(pinnacle_1394_t *l);

/* Brings the link up the way MarvinBus64.sys does, ending with a bus reset
 * and a topology read. docs/startup.md explains every step. */
int p1394_link_init(pinnacle_1394_t *l);

/* Finds the first other node that has an output master plug register. */
int p1394_find_camera(pinnacle_1394_t *l, uint16_t *node, uint32_t *ompr);

/* Adds / removes our point-to-point connection on the node's oPCR[0].
 * *opcr receives the register value (channel in bits 21:16). */
int p1394_connect(pinnacle_1394_t *l, uint16_t node, uint32_t *opcr);
int p1394_disconnect(pinnacle_1394_t *l, uint16_t node, uint32_t *opcr);

/* Starts / stops isochronous receive of a channel on IR context 0 (the
 * data arrives on EP 0x88). */
int p1394_ir_start(pinnacle_1394_t *l, unsigned channel);
int p1394_ir_stop(pinnacle_1394_t *l);

#ifdef __cplusplus
}
#endif

#endif /* PINNACLE_1394_H */
