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

#include "pinnacle_1394.h"
#include "pin_log.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
static unsigned long long now_ms(void) { return GetTickCount64(); }
static void sleep_ms(unsigned ms) { Sleep(ms); }
#else
#include <time.h>
static unsigned long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000u + (unsigned long long)ts.tv_nsec / 1000000u;
}
static void sleep_ms(unsigned ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

/* EP 0x02 message types (header bits 31:28) */
#define MSG_REG_WRITE 0x2u
#define MSG_REG_READ  0x3u
#define MSG_VENDOR_WRITE 0x4u     /* FPGA USB-side register, see p1394_link_init */
#define MSG_VENDOR_READ  0x5u
#define MSG_EXT_WRITE    0x6u     /* FPGA register bank mirroring OHCI offsets */
#define MSG_RAM_WRITE 0x8u
#define MSG_RAM_DATA  0x9u        /* device -> host: a DMA write into device RAM */
#define MSG_INT_EVENT 0xau
#define MSG_REPLY     (1u << 27)  /* reply wanted; tag in bits 26:20 */
#define OHCI_BASE     0x10000u    /* register address = OHCI_BASE + offset */

/* OHCI registers used here */
#define OHCI_NODE_ID        0x0e8
#define OHCI_SELF_ID_COUNT  0x068
#define OHCI_ATREQ_SET      0x180
#define OHCI_ATREQ_CLEAR    0x184
#define OHCI_ATREQ_CMDPTR   0x18c
#define CTX_RUN    0x8000u
#define CTX_WAKE   0x1000u
#define CTX_ACTIVE 0x0400u

/* Device RAM layout set up by the link init (see docs/startup.md) */
#define RAM_AR_REQ_LO  0x3000u    /* AR request buffer, 8 KiB */
#define RAM_AR_RSP_LO  0x5000u    /* AR response buffer, 8 KiB */
#define RAM_AR_RSP_HI  0x7000u
#define RAM_AT_LO      0x11c0u    /* AT descriptor slots used by the vendor driver */
#define RAM_AT_HI      0x1240u
#define AT_SLOT        0x11c0u    /* the one slot we use */

#define TC_WRITE_QUADLET 0x0u
#define TC_WRITE_BLOCK   0x1u
#define TC_WRITE_RESP    0x2u
#define TC_READ_QUADLET  0x4u
#define TC_READ_QUADLET_RESP 0x6u
#define TC_LOCK          0x9u
#define TC_LOCK_RESP     0xbu
#define EXT_COMPARE_SWAP 0x2u

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t bswap32(uint32_t w)
{
    return (w >> 24) | ((w >> 8) & 0xff00u) | ((w << 8) & 0xff0000u) | (w << 24);
}

static uint32_t get32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

void p1394_init(pinnacle_1394_t *l, pinnacle_device_t *dev)
{
    memset(l, 0, sizeof(*l));
    l->dev = dev;
    l->local_node = 0xffc0;
    l->next_tl = 2;
    l->at_evt = -1;
}

/* --- receiving ----------------------------------------------------------
 *
 * Everything the device sends on EP 0x84 is a sequence of messages:
 *   type 9  DMA write into device RAM: AT xferStatus writebacks (0x11c0..),
 *           received packets (AR request buffer 0x3000.., AR response buffer
 *           0x5000..), AR descriptor resCount writebacks (0x110c ...)
 *   type 3  reply to an OHCI register read
 *   type 10 IntEvent report
 * A received packet is the OHCI AR format: header quadlets (host order),
 * payload (wire order), then a trailer quadlet whose bits 20:16 are the ack
 * our link sent for it. */

static void handle_ar_packet(pinnacle_1394_t *l, uint16_t ram, const uint8_t *d, unsigned n)
{
    if (n < 16)
        return;
    uint32_t q0 = get32(d), q1 = get32(d + 4), q2 = get32(d + 8), q3 = get32(d + 12);
    unsigned tcode = (q0 >> 4) & 0xf, tl = (q0 >> 10) & 0x3f;
    unsigned ack = (get32(d + n - 4) >> 16) & 0x1f;
    uint16_t src = (uint16_t)(q1 >> 16);

    if (ram >= RAM_AR_RSP_LO) {
        /* a response to one of our requests */
        l->rsp_tl = tl;
        l->rsp_tcode = tcode;
        l->rsp_rcode = (q1 >> 12) & 0xf;
        l->rsp_src = src;
        memset(l->rsp_data, 0, sizeof(l->rsp_data));
        if (tcode == TC_READ_QUADLET_RESP)
            memcpy(l->rsp_data, d + 12, 4);
        else if (tcode == TC_LOCK_RESP && n >= 20 && (q3 >> 16) >= 4)
            memcpy(l->rsp_data, d + 16, 4);
        l->rsp_seq++;
        return;
    }

    /* a request to us: the only one we expect is an FCP response */
    if ((tcode == TC_WRITE_QUADLET || tcode == TC_WRITE_BLOCK) &&
        (q1 & 0xffff) == 0xffff && q2 == P1394_CSR_FCP_RESPONSE) {
        const uint8_t *pay = tcode == TC_WRITE_QUADLET ? d + 12 : d + 16;
        unsigned len = tcode == TC_WRITE_QUADLET ? 4 : (q3 >> 16);
        if (len > 512 || (tcode == TC_WRITE_BLOCK && 16 + len > n))
            len = 0;
        int slot = l->fcp_seq % P1394_FCP_SLOTS;
        memcpy(l->fcp[slot], pay, len);
        l->fcp_len[slot] = (int)len;
        l->fcp_seq++;
    } else if (l->verbose && tcode != 0xe) {   /* 0xe: PHY packets (self-IDs) */
        pin_logf(PIN_LOG_DEBUG, "p1394: unexpected request tcode=%x from %04x to %04x%08x\n",
                tcode, src, q1 & 0xffff, q2);
    }
    /* our link acked it "pending": the sender now waits for a write response */
    if ((tcode == TC_WRITE_QUADLET || tcode == TC_WRITE_BLOCK) &&
        ack == P1394_EVT_ACK_PENDING && l->n_owed < 64) {
        l->owed[l->n_owed].tl = (uint8_t)tl;
        l->owed[l->n_owed].src = src;
        l->n_owed++;
    }
}

void p1394_parse_ep84(pinnacle_1394_t *l, const uint8_t *p, int len)
{
    int o = 0;
    while (o + 4 <= len) {
        uint32_t h = get32(p + o);
        unsigned type = h >> 28;
        if (type == MSG_RAM_DATA) {
            unsigned n = (h >> 16) & 0x7ff;
            uint16_t ram = (uint16_t)(h & 0xffff);
            const uint8_t *d = p + o + 4;
            if (o + 4 + (int)n > len)
                break;
            if (ram >= RAM_AR_REQ_LO && ram < RAM_AR_RSP_HI)
                handle_ar_packet(l, ram, d, n);
            else if (ram >= RAM_AT_LO && ram < RAM_AT_HI && n >= 4 && ram == l->at_status_addr)
                l->at_evt = (int)((get32(d) >> 16) & 0x1f);
            o += 4 + (int)n;
        } else if (type == MSG_REG_READ) {
            if (o + 8 <= len) {
                l->reg_addr = h & 0xfffff;
                l->reg_val = get32(p + o + 4);
                l->reg_seq++;
            }
            o += 8;
        } else if (type == MSG_VENDOR_READ) {
            if (o + 8 <= len) {
                l->vreg_val = get32(p + o + 4);
                l->vreg_seq++;
            }
            o += 8;
        } else if (type == MSG_INT_EVENT) {
            if (l->verbose > 1 && o + 8 <= len)
                pin_logf(PIN_LOG_DEBUG, "p1394: IntEvent 0x%08x\n", get32(p + o + 4));
            o += 8;
        } else {
            if (l->verbose)
                pin_logf(PIN_LOG_DEBUG, "p1394: unparsed EP 0x84 message type %x\n", type);
            break;
        }
    }
}

int p1394_pump(pinnacle_1394_t *l, unsigned timeout_ms)
{
    uint8_t buf[2048];
    int n = 0;
    int rc = libusb_bulk_transfer(l->dev->handle, PINNACLE_EP_CMD_IN, buf, sizeof(buf), &n,
                                  timeout_ms);
    if (rc == LIBUSB_ERROR_TIMEOUT)
        return 0;
    if (rc != 0)
        return -1;
    if (l->verbose > 1) {
        /* One line, not a sequence of formats: same reasoning as ep84_cb in
         * pinnacle_stream.c -- interleaved partial writes are unparseable. */
        char line[2 + 2 * sizeof(buf) + 16];
        int off = snprintf(line, sizeof(line), "p1394: ep84 ");
        for (int i = 0; i < n && off + 3 < (int)sizeof(line); i++)
            off += snprintf(line + off, sizeof(line) - off, "%02x", buf[i]);
        snprintf(line + off, sizeof(line) - off, "\n");
        pin_logf(PIN_LOG_DEBUG, "%s", line);
    }
    p1394_parse_ep84(l, buf, n);
    return n;
}

/* --- sending ------------------------------------------------------------ */

int p1394_send(pinnacle_1394_t *l, const uint8_t *msg, int len)
{
    int n = 0;
    int rc = libusb_bulk_transfer(l->dev->handle, PINNACLE_EP_CMD_OUT, (uint8_t *)msg, len, &n, 2000);
    if (rc != 0 || n != len) {
        pin_logf(PIN_LOG_ERROR, "p1394: EP 0x02 write failed: %s (%d/%d)\n", libusb_error_name(rc), n, len);
        return -1;
    }
    return 0;
}

static int put_reg_write(uint8_t *p, uint16_t off, uint32_t val)
{
    put32(p, (MSG_REG_WRITE << 28) | (OHCI_BASE + off));
    put32(p + 4, val);
    return 8;
}

int p1394_reg_write(pinnacle_1394_t *l, uint16_t off, uint32_t val)
{
    uint8_t pkt[8];
    put_reg_write(pkt, off, val);
    return p1394_send(l, pkt, 8);
}

int p1394_reg_read(pinnacle_1394_t *l, uint16_t off, uint32_t *val)
{
    uint8_t pkt[8] = { 0 };
    put32(pkt, (MSG_REG_READ << 28) | MSG_REPLY | (1u << 20) | (OHCI_BASE + off));
    int seq0 = l->reg_seq;
    if (p1394_send(l, pkt, 8) != 0)
        return -1;
    unsigned long long deadline = now_ms() + 500;
    while (now_ms() < deadline) {
        if (p1394_pump(l, 50) < 0)
            return -1;
        if (l->reg_seq != seq0 && l->reg_addr == OHCI_BASE + off) {
            *val = l->reg_val;
            return 0;
        }
    }
    return -1;
}

int p1394_reg_read_begin(pinnacle_1394_t *l, uint16_t off)
{
    uint8_t pkt[8] = { 0 };
    put32(pkt, (MSG_REG_READ << 28) | MSG_REPLY | (1u << 20) | (OHCI_BASE + off));
    l->reg_seq_before = l->reg_seq;
    return p1394_send(l, pkt, 8);
}

int p1394_reg_read_poll(pinnacle_1394_t *l, uint16_t off, uint32_t *val)
{
    if (l->reg_seq == l->reg_seq_before || l->reg_addr != OHCI_BASE + off)
        return 0;
    *val = l->reg_val;
    return 1;
}

int p1394_at_reset(pinnacle_1394_t *l)
{
    uint32_t v = 0;
    if (p1394_reg_write(l, OHCI_ATREQ_CLEAR, CTX_RUN) != 0)
        return -1;
    for (int i = 0; i < 50; i++) {
        if (p1394_reg_read(l, OHCI_ATREQ_SET, &v) == 0 && !(v & CTX_ACTIVE))
            return 0;
        sleep_ms(2);
    }
    pin_logf(PIN_LOG_WARN, "p1394: AT request context still active (0x%08x)\n", v);
    return -1;
}

int p1394_read_topology(pinnacle_1394_t *l)
{
    uint32_t id = 0, cnt = 0;
    for (int i = 0; i < 50; i++) {
        /* NodeID bit 31 iDValid, bits 5:0 nodeNumber */
        if (p1394_reg_read(l, OHCI_NODE_ID, &id) == 0 && (id & 0x80000000u))
            break;
        sleep_ms(20);
    }
    if (!(id & 0x80000000u) || p1394_reg_read(l, OHCI_SELF_ID_COUNT, &cnt) != 0) {
        pin_logf(PIN_LOG_ERROR, "p1394: no valid node ID (NodeID 0x%08x)\n", id);
        return -1;
    }
    l->local_node = (uint16_t)(0xffc0u | (id & 0x3f));
    /* SelfIDCount bits 10:2 = quadlets in the self-ID buffer: one header
     * quadlet, then a packet + its inverse per node (nodes with more than
     * three ports send extra packets, which would overcount; fine for the
     * one- or two-node buses this device sees). */
    unsigned q = (cnt >> 2) & 0x1ff;
    l->node_count = q > 1 ? (int)((q - 1) / 2) : 0;
    if (l->verbose)
        pin_logf(PIN_LOG_DEBUG, "p1394: NodeID 0x%08x SelfIDCount 0x%08x -> node %u of %d\n",
                id, cnt, id & 0x3f, l->node_count);
    return 0;
}

/* One AT packet, started cold from a single RAM slot:
 *   ATreq.ContextControlClear = run   (the context is idle; lets us rewrite CommandPtr)
 *   type-8 write of the descriptor block into RAM at AT_SLOT
 *   ATreq.CommandPtr = AT_SLOT | Z
 *   ATreq.ContextControlSet = run|wake
 * then the xferStatus writeback arrives on EP 0x84. The vendor driver chains
 * blocks through three slots instead; since we wait for each status anyway
 * chaining buys nothing.
 *
 * Blocks (Z = number of 16-byte descriptors):
 *   no payload: OUTPUT_LAST-Immediate (key 2, i=0, b=3) carrying the 12- or
 *               16-byte header, Z = 2, xferStatus at base+0x0c
 *   payload:    OUTPUT_MORE-Immediate (16-byte header) + OUTPUT_LAST pointing
 *               at the payload at base+0x30, Z = 3, xferStatus at base+0x2c */
int p1394_submit(pinnacle_1394_t *l, const uint32_t hdr[4], unsigned hdr_len,
                 const uint8_t *payload, unsigned plen)
{
    uint8_t pkt[256];
    int o = 0;
    uint32_t desc[12];
    unsigned nq;
    uint16_t status;

    if (plen > 128)
        return -1;
    if (!payload) {
        desc[0] = 0x120c0000u | hdr_len; desc[1] = 0; desc[2] = 0; desc[3] = 0;
        memcpy(&desc[4], hdr, 16);
        nq = 8;
        status = AT_SLOT + 0x0c;
    } else {
        desc[0] = 0x02000000u | 16; desc[1] = 0; desc[2] = 0; desc[3] = 0;
        memcpy(&desc[4], hdr, 16);
        desc[8] = 0x100c0000u | plen; desc[9] = AT_SLOT + 0x30u; desc[10] = 0; desc[11] = 0;
        nq = 12;
        status = AT_SLOT + 0x2c;
    }

    o += put_reg_write(pkt + o, OHCI_ATREQ_CLEAR, CTX_RUN);
    unsigned padded = payload ? ((plen + 3) & ~3u) : 0;
    put32(pkt + o, (MSG_RAM_WRITE << 28) | ((nq * 4 + padded) << 16) | AT_SLOT); o += 4;
    for (unsigned i = 0; i < nq; i++) { put32(pkt + o, desc[i]); o += 4; }
    if (payload) {
        memset(pkt + o, 0, padded);
        memcpy(pkt + o, payload, plen);
        o += (int)padded;
    }
    o += put_reg_write(pkt + o, OHCI_ATREQ_CMDPTR, AT_SLOT | (payload ? 3u : 2u));
    o += put_reg_write(pkt + o, OHCI_ATREQ_SET, CTX_RUN | CTX_WAKE);

    int evt = -1;
    for (int attempt = 0; attempt < 8; attempt++) {
        l->at_status_addr = status;
        l->at_evt = -1;
        if (p1394_send(l, pkt, o) != 0)
            return -1;
        unsigned long long deadline = now_ms() + 500;
        while (l->at_evt < 0 && now_ms() < deadline)
            if (p1394_pump(l, 20) < 0)
                return -1;
        evt = l->at_evt;
        if (evt < 0x14 || evt > 0x16)          /* ack_busy_X/A/B: retry */
            break;
        sleep_ms(10);
    }
    l->at_status_addr = 0;
    return evt;
}

/* First header quadlet of a request: rotating tlabel 2..63, rt = retry_1. */
static uint32_t req_q0(pinnacle_1394_t *l, unsigned tcode)
{
    unsigned tl = l->next_tl;
    l->next_tl = l->next_tl >= 63 ? 2 : l->next_tl + 1;
    return (tl << 10) | (tcode << 4);
}

static int wait_response(pinnacle_1394_t *l, int seq0, unsigned tl, uint16_t node,
                         unsigned tcode, unsigned timeout_ms)
{
    unsigned long long deadline = now_ms() + timeout_ms;
    while (now_ms() < deadline) {
        if (l->rsp_seq != seq0 && l->rsp_tl == tl && l->rsp_src == node && l->rsp_tcode == tcode)
            return l->rsp_rcode == 0 ? 0 : -1;
        if (p1394_pump(l, 20) < 0)
            return -1;
    }
    return -1;
}

int p1394_read_quadlet(pinnacle_1394_t *l, uint16_t node, uint32_t off, uint32_t *val)
{
    uint32_t hdr[4] = { req_q0(l, TC_READ_QUADLET), ((uint32_t)node << 16) | 0xffff, off, 0 };
    unsigned tl = (hdr[0] >> 10) & 0x3f;
    int seq0 = l->rsp_seq;
    int evt = p1394_submit(l, hdr, 12, NULL, 0);
    if (evt != P1394_EVT_ACK_PENDING && evt != P1394_EVT_ACK_COMPLETE)
        return -1;
    if (wait_response(l, seq0, tl, node, TC_READ_QUADLET_RESP, 300) != 0)
        return -1;
    *val = get32be(l->rsp_data);
    return 0;
}

int p1394_compare_swap(pinnacle_1394_t *l, uint16_t node, uint32_t off,
                       uint32_t arg, uint32_t data, uint32_t *old)
{
    uint8_t pay[8] = {
        (uint8_t)(arg >> 24), (uint8_t)(arg >> 16), (uint8_t)(arg >> 8), (uint8_t)arg,
        (uint8_t)(data >> 24), (uint8_t)(data >> 16), (uint8_t)(data >> 8), (uint8_t)data,
    };
    uint32_t hdr[4] = { req_q0(l, TC_LOCK), ((uint32_t)node << 16) | 0xffff, off,
                        (8u << 16) | EXT_COMPARE_SWAP };
    unsigned tl = (hdr[0] >> 10) & 0x3f;
    int seq0 = l->rsp_seq;
    int evt = p1394_submit(l, hdr, 16, pay, 8);
    if (evt != P1394_EVT_ACK_PENDING && evt != P1394_EVT_ACK_COMPLETE)
        return -1;
    if (wait_response(l, seq0, tl, node, TC_LOCK_RESP, 300) != 0)
        return -1;
    *old = get32be(l->rsp_data);
    return 0;
}

void p1394_answer_owed(pinnacle_1394_t *l)
{
    while (l->n_owed > 0) {
        uint8_t tl = l->owed[0].tl;
        uint16_t src = l->owed[0].src;
        memmove(l->owed, l->owed + 1, sizeof(l->owed[0]) * (size_t)--l->n_owed);
        /* write response: tcode 2, rt 1, rcode 0 (complete). Its header is
         * 12 bytes; sending 16 wedges the AT context. */
        uint32_t hdr[4] = { ((uint32_t)tl << 10) | (1u << 8) | (TC_WRITE_RESP << 4),
                            (uint32_t)src << 16, 0, 0 };
        int evt = p1394_submit(l, hdr, 12, NULL, 0);
        if (l->verbose)
            pin_logf(PIN_LOG_DEBUG, "p1394: write response tl=%u -> event 0x%02x\n", tl, evt);
    }
}

int p1394_avc(pinnacle_1394_t *l, uint16_t node, const uint8_t *cmd, unsigned len,
              uint8_t *resp, unsigned resp_max, unsigned timeout_ms)
{
    /* A camera waiting for our write response answers new commands with
     * ack_busy, so finish those first. */
    p1394_answer_owed(l);
    int seq0 = l->fcp_seq;
    int evt;
    if (len == 4) {
        uint32_t q = get32(cmd);   /* data quadlet: bytes in wire order */
        uint32_t hdr[4] = { req_q0(l, TC_WRITE_QUADLET), ((uint32_t)node << 16) | 0xffff,
                            P1394_CSR_FCP_COMMAND, q };
        evt = p1394_submit(l, hdr, 16, NULL, 0);
    } else {
        uint32_t hdr[4] = { req_q0(l, TC_WRITE_BLOCK), ((uint32_t)node << 16) | 0xffff,
                            P1394_CSR_FCP_COMMAND, len << 16 };
        evt = p1394_submit(l, hdr, 16, cmd, len);
    }
    if (evt != P1394_EVT_ACK_PENDING && evt != P1394_EVT_ACK_COMPLETE) {
        if (l->verbose)
            pin_logf(PIN_LOG_DEBUG, "p1394: FCP command not delivered (event 0x%02x)\n", evt);
        return -1;
    }

    unsigned long long deadline = now_ms() + timeout_ms;
    while (now_ms() < deadline) {
        p1394_answer_owed(l);
        if (l->fcp_seq - seq0 > P1394_FCP_SLOTS)
            seq0 = l->fcp_seq - P1394_FCP_SLOTS;
        while (seq0 != l->fcp_seq) {
            const uint8_t *f = l->fcp[seq0 % P1394_FCP_SLOTS];
            int flen = l->fcp_len[seq0 % P1394_FCP_SLOTS];
            seq0++;
            /* match subunit + opcode; TRANSPORT STATE (0xd0) answers with
             * the transport opcode (0xc1..0xc4) in its place */
            if (flen < 3 || f[1] != cmd[1] ||
                !(f[2] == cmd[2] || (cmd[2] == 0xd0 && f[2] >= 0xc1 && f[2] <= 0xc4))) {
                if (l->verbose)
                    pin_logf(PIN_LOG_DEBUG, "p1394: ignoring unrelated FCP response %02x %02x %02x\n",
                            f[0], f[1], f[2]);
                continue;
            }
            if (f[0] == 0x0f) {          /* INTERIM: the final response follows */
                deadline = now_ms() + 10000;
                continue;
            }
            unsigned n = (unsigned)flen < resp_max ? (unsigned)flen : resp_max;
            memcpy(resp, f, n);
            p1394_answer_owed(l);
            return (int)n;
        }
        if (p1394_pump(l, 20) < 0)
            return -1;
    }
    p1394_answer_owed(l);
    return -1;
}

/* --- async AV/C (for use while something else owns EP 0x84) --------------
 *
 * p1394_avc() above blocks inside p1394_submit()/p1394_pump(), each of which
 * does its own synchronous libusb_bulk_transfer() read of EP 0x84 -- fine
 * when nothing else is streaming, wrong once pinnacle_stream_read_loop() is
 * running its own async queue on that same endpoint (only one reader may
 * ever have URBs outstanding on an endpoint at a time).
 *
 * This split version only ever *writes* EP 0x02 (p1394_send(), same as
 * p1394_submit() but without its wait-for-ack-writeback loop) and reads its
 * state from whatever already fed p1394_parse_ep84() -- the stream loop's
 * own EP 0x84 completion handler, via its tick hook (pinnacle_stream.h).
 * One outstanding async command at a time per link, which is all a deck
 * control caller ever needs (see engine/pin_deck.c). */
int p1394_avc_begin(pinnacle_1394_t *l, uint16_t node, const uint8_t *cmd, unsigned len)
{
    if (len != 4 && (len < 1 || len > 128))
        return -1;

    uint8_t pkt[256];
    int o = 0;
    uint32_t desc[12];
    unsigned nq;

    uint32_t hdr[4];
    if (len == 4) {
        uint32_t q = get32(cmd);
        hdr[0] = req_q0(l, TC_WRITE_QUADLET);
        hdr[1] = ((uint32_t)node << 16) | 0xffff;
        hdr[2] = P1394_CSR_FCP_COMMAND;
        hdr[3] = q;
        desc[0] = 0x120c0000u | 16; desc[1] = 0; desc[2] = 0; desc[3] = 0;
        memcpy(&desc[4], hdr, 16);
        nq = 8;
        l->avc_status_addr = AT_SLOT + 0x0c;
    } else {
        hdr[0] = req_q0(l, TC_WRITE_BLOCK);
        hdr[1] = ((uint32_t)node << 16) | 0xffff;
        hdr[2] = P1394_CSR_FCP_COMMAND;
        hdr[3] = len << 16;
        desc[0] = 0x02000000u | 16; desc[1] = 0; desc[2] = 0; desc[3] = 0;
        memcpy(&desc[4], hdr, 16);
        unsigned padded = (len + 3) & ~3u;
        desc[8] = 0x100c0000u | len; desc[9] = AT_SLOT + 0x30u; desc[10] = 0; desc[11] = 0;
        nq = 12;
        l->avc_status_addr = AT_SLOT + 0x2c;
        (void)padded;
    }

    o += put_reg_write(pkt + o, OHCI_ATREQ_CLEAR, CTX_RUN);
    unsigned padded = len == 4 ? 0 : ((len + 3) & ~3u);
    put32(pkt + o, (MSG_RAM_WRITE << 28) | ((nq * 4 + padded) << 16) | AT_SLOT); o += 4;
    for (unsigned i = 0; i < nq; i++) { put32(pkt + o, desc[i]); o += 4; }
    if (len != 4) {
        memset(pkt + o, 0, padded);
        memcpy(pkt + o, cmd, len);
        o += (int)padded;
    }
    o += put_reg_write(pkt + o, OHCI_ATREQ_CMDPTR, AT_SLOT | (len == 4 ? 2u : 3u));
    o += put_reg_write(pkt + o, OHCI_ATREQ_SET, CTX_RUN | CTX_WAKE);

    if (p1394_send(l, pkt, o) != 0) {
        l->avc_status_addr = 0;
        return -1;
    }

    l->avc_pending = 1;
    l->avc_match_subunit = cmd[1];
    l->avc_match_opcode = cmd[2];
    l->avc_fcp_seq0 = l->fcp_seq;
    l->avc_deadline_ms = now_ms() + 3000;
    l->at_status_addr = (uint16_t)l->avc_status_addr; /* so parse_ep84 also updates at_evt for us */
    l->at_evt = -1;
    return 0;
}

/* Call every tick (after feeding new EP 0x84 bytes to p1394_parse_ep84()).
 * Returns >0 = response length in resp (command complete, whether accepted
 * or rejected -- the caller checks resp[0] same as p1394_avc()), 0 = still
 * pending, -1 = timed out or no async command in flight. Retrying a
 * NOT_IMPLEMENTED (camera busy) is the caller's job, same rule as
 * pindeck/pin_deck's synchronous retry. */
int p1394_avc_poll(pinnacle_1394_t *l, uint8_t *resp, unsigned resp_max)
{
    if (!l->avc_pending)
        return -1;

    p1394_answer_owed(l);

    int seq0 = l->avc_fcp_seq0;
    if (l->fcp_seq - seq0 > P1394_FCP_SLOTS)
        seq0 = l->fcp_seq - P1394_FCP_SLOTS;
    while (seq0 != l->fcp_seq) {
        const uint8_t *f = l->fcp[seq0 % P1394_FCP_SLOTS];
        int flen = l->fcp_len[seq0 % P1394_FCP_SLOTS];
        seq0++;
        if (flen < 3 || f[1] != l->avc_match_subunit ||
            !(f[2] == l->avc_match_opcode ||
              (l->avc_match_opcode == 0xd0 && f[2] >= 0xc1 && f[2] <= 0xc4)))
            continue;
        l->avc_fcp_seq0 = seq0;
        if (f[0] == 0x0f) {                 /* INTERIM: keep waiting, longer */
            l->avc_deadline_ms = now_ms() + 10000;
            continue;
        }
        unsigned n = (unsigned)flen < resp_max ? (unsigned)flen : resp_max;
        memcpy(resp, f, n);
        l->avc_pending = 0;
        l->avc_status_addr = 0;
        return (int)n;
    }
    l->avc_fcp_seq0 = seq0;

    if (now_ms() >= l->avc_deadline_ms) {
        l->avc_pending = 0;
        l->avc_status_addr = 0;
        return -1;
    }
    return 0;
}

int p1394_avc_cancel(pinnacle_1394_t *l)
{
    int was_pending = l->avc_pending;
    l->avc_pending = 0;
    l->avc_status_addr = 0;
    return was_pending;
}

/* --- link bring-up --------------------------------------------------------
 *
 * What MarvinBus64.sys does between selecting alt setting 1 and the first
 * 1394 transaction, as named steps. Each group is sent in one USB transfer,
 * as the vendor driver batches it, with the gaps observed in
 * a usbmon capture of the vendor driver's cold boot. Where each value comes
 * from (function addresses, registry defaults) is in docs/startup.md. */

/* FPGA USB-side register 0 (message type 4, index 0). MarvinBus64 keeps a
 * shadow copy and always sends the whole word:
 *   bits 25:16  ChangeModeTimeout (registry, default 180, range 20..1000).
 *               The other hardware back-end writes the same value to its
 *               register named "Async rx idle time".
 *   bit 9       adaptive buffer levels enabled (registry DisableLevels; the
 *               default disables them, so 0)
 *   bit 8       isochronous data to USB off: cleared when a listen starts,
 *               set when it stops */
#define USBCFG_IDLE_TIME(t)   ((uint32_t)(t) << 16)
#define USBCFG_ISOCH_OFF      0x100u
#define USBCFG_DEFAULT        USBCFG_IDLE_TIME(180)

/* OHCI */
#define OHCI_ATRETRIES       0x008
#define OHCI_CONFIG_ROM_HDR  0x018
#define OHCI_BUS_ID          0x01c
#define OHCI_BUS_OPTIONS     0x020
#define OHCI_GUID_HI         0x024
#define OHCI_GUID_LO         0x028
#define OHCI_CONFIG_ROM_MAP  0x034
#define OHCI_VENDOR_ID       0x040
#define OHCI_HC_SET          0x050
#define OHCI_HC_CLEAR        0x054
#define OHCI_SELF_ID_BUFFER  0x064
#define OHCI_INT_EVENT_CLEAR 0x084
#define OHCI_INT_MASK_CLEAR  0x08c
#define OHCI_LINK_SET        0x0e0
#define OHCI_LINK_CLEAR      0x0e4
#define OHCI_PHY_CONTROL     0x0ec
#define OHCI_ASYNC_FILTER_HI_SET   0x100
#define OHCI_ASYNC_FILTER_HI_CLEAR 0x104
#define OHCI_ASYNC_FILTER_LO_CLEAR 0x10c
#define OHCI_ATRSP_CLEAR     0x1a4
#define OHCI_ARREQ_SET       0x1c0
#define OHCI_ARREQ_CMDPTR    0x1cc
#define OHCI_ARRSP_SET       0x1e0
#define OHCI_ARRSP_CMDPTR    0x1ec
#define OHCI_IR0_SET         0x400
#define OHCI_IR0_CLEAR       0x404
#define OHCI_IR0_CMDPTR      0x40c
#define OHCI_IR0_MATCH       0x410

#define HC_SOFT_RESET      0x00010000u
#define HC_LINK_ENABLE     0x00020000u
#define HC_LPS             0x00080000u
#define HC_NO_BYTE_SWAP    0x40000000u
#define LINK_RCV_PHY_PKT   0x00000200u
#define LINK_RCV_SELF_ID   0x00000400u
#define LINK_CYCLE_TIMER   0x00100000u
#define LINK_CYCLE_MASTER  0x00200000u
#define INT_BUS_RESET      0x00020000u
#define IR_BUFFER_FILL     0x80000000u
#define IR_ISOCH_HEADER    0x40000000u
#define IR_CYCLE_MATCH     0x20000000u
#define IR_MULTI_CHAN      0x10000000u
#define IR_DUAL_BUFFER     0x08000000u

/* Our own node's configuration ROM, laid out as MarvinBus64 publishes it:
 * bus info block, root directory, unit directory and two text leaves, as
 * big-endian quadlets. Only the GUID differs between units; it is read from
 * the device (see pinnacle_init_hardware). Decoded in docs/startup.md.
 *
 * MarvinBus64 computes the IEEE 1212 CRC-16 (CRC-CCITT, polynomial 0x1021)
 * over each quadlet's bytes in little-endian order rather than the
 * big-endian order the standard specifies; we do the same, so the ROM is
 * byte-identical to the vendor driver's. */
#define ROM_GUID_HI 3
#define ROM_GUID_LO 4
#define ROM_WORDS   38

static uint16_t rom_crc(const uint32_t *q, unsigned n)
{
    uint16_t c = 0;
    for (unsigned i = 0; i < n; i++)
        for (unsigned byte = 0; byte < 4; byte++) {         /* little-endian byte order */
            c ^= (uint16_t)(((q[i] >> (8 * byte)) & 0xff) << 8);
            for (int b = 0; b < 8; b++)
                c = (uint16_t)(c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1);
        }
    return c;
}

static void build_config_rom(uint32_t rom[ROM_WORDS], uint32_t guid_hi, uint32_t guid_lo)
{
    uint32_t vendor = guid_hi >> 8;                   /* node_vendor_ID = GUID bits 63:40 */
    static const uint32_t text_pinnacle[] = {         /* "Pinnacle Systems", UTF-16LE */
        0x80000000, 0x00000409, 0x50006900, 0x6e006e00, 0x61006300, 0x6c006500,
        0x20005300, 0x79007300, 0x74006500, 0x6d007300, 0x00000000,
    };
    static const uint32_t text_marvin[] = {           /* "Marvin Series  ", UTF-16LE */
        0x80000000, 0x00000409, 0x4d006100, 0x72007600, 0x69006e00, 0x20005300,
        0x65007200, 0x69006500, 0x73002000, 0x20000000,
    };
    unsigned i = 0;
    /* bus info block: "1394", irmc|cmc|isc|bmc (0xf), cyc_clk_acc 0, max_rec
     * 0xa (2048 bytes), generation 0, max_rom 0, link speed S400 (2) */
    rom[i++] = 0x04040000;                            /* info_length 4, crc_length 4 */
    rom[i++] = 0x31333934;
    rom[i++] = 0xf000a002;
    rom[i++] = guid_hi;
    rom[i++] = guid_lo;
    rom[0] |= rom_crc(&rom[1], 4);
    /* root directory */
    rom[i++] = 0x00040000;
    rom[i++] = 0x0c0083c0;                            /* node capabilities */
    rom[i++] = 0x03000000 | vendor;                   /* module vendor ID */
    rom[i++] = 0x81000007;                            /* text leaf, +7 quadlets */
    rom[i++] = 0xd1000001;                            /* unit directory, +1 quadlet */
    rom[5] |= rom_crc(&rom[6], 4);
    /* unit directory */
    rom[i++] = 0x00040000;
    rom[i++] = 0x12000000 | vendor;                   /* specifier ID */
    rom[i++] = 0x13000000;                            /* version 0 */
    rom[i++] = 0x17000000;                            /* model 0 */
    rom[i++] = 0x8100000d;                            /* text leaf, +13 quadlets */
    rom[10] |= rom_crc(&rom[11], 4);
    /* text leaves */
    rom[i] = 0x000b0000 | rom_crc(text_pinnacle, 11);
    memcpy(&rom[i + 1], text_pinnacle, sizeof(text_pinnacle));
    i += 12;
    rom[i] = 0x000a0000 | rom_crc(text_marvin, 10);
    memcpy(&rom[i + 1], text_marvin, sizeof(text_marvin));
}

/* GUID of the development unit, used only if it could not be read */
#define FALLBACK_GUID_HI 0xfb82ad03u
#define FALLBACK_GUID_LO 0x128d3000u
#define CONFIG_ROM_RAM 0x1000u

/* A batch of messages sent as one USB transfer. */
struct batch { uint8_t b[512]; int n; };

static void b_reg(struct batch *m, uint16_t off, uint32_t val)
{
    m->n += put_reg_write(m->b + m->n, off, val);
}

static void b_msg(struct batch *m, uint32_t hdr, uint32_t val)
{
    put32(m->b + m->n, hdr); put32(m->b + m->n + 4, val); m->n += 8;
}

/* type-8 block write into device RAM; words are sent as LE u32 */
static void b_ram(struct batch *m, uint16_t addr, const uint32_t *w, unsigned nw)
{
    put32(m->b + m->n, (MSG_RAM_WRITE << 28) | ((nw * 4) << 16) | addr); m->n += 4;
    for (unsigned i = 0; i < nw; i++) { put32(m->b + m->n, w[i]); m->n += 4; }
}

static int b_send(pinnacle_1394_t *l, struct batch *m, unsigned delay_before_ms)
{
    if (delay_before_ms)
        sleep_ms(delay_before_ms);
    int rc = p1394_send(l, m->b, m->n);
    m->n = 0;
    /* process whatever the device reports straight away, so its EP 0x84
     * buffer never fills while we are still sending */
    while (p1394_pump(l, 5) > 0)
        ;
    return rc;
}

static int vendor_write(pinnacle_1394_t *l, unsigned idx, uint32_t val, unsigned delay)
{
    struct batch m = { .n = 0 };
    b_msg(&m, (MSG_VENDOR_WRITE << 28) | idx, val);
    return b_send(l, &m, delay);
}

/* Type 5, index 0: a status read. MarvinBus64 reads it once before init
 * (FUN_0002cd00 and its resume twin only proceed if the low byte is below
 * 0x7a) and then 20 times in a row after writing the config ROM, ignoring
 * the answers (op-table entry 2, FUN_0002b5a0). Meaning unknown. */
static int vendor_read(pinnacle_1394_t *l, unsigned idx, uint32_t *val)
{
    uint8_t pkt[8] = { 0 };
    put32(pkt, (MSG_VENDOR_READ << 28) | MSG_REPLY | (1u << 20) | idx);
    int seq0 = l->vreg_seq;
    if (p1394_send(l, pkt, 8) != 0)
        return -1;
    unsigned long long deadline = now_ms() + 500;
    while (now_ms() < deadline) {
        if (p1394_pump(l, 20) < 0)
            return -1;
        if (l->vreg_seq != seq0) {
            *val = l->vreg_val;
            return 0;
        }
    }
    return -1;
}

/* Type 6: the FPGA's extension register bank. It uses OHCI-style offsets
 * but is a separate register file (MarvinBus64 FUN_0002bd80). */
static void b_ext(struct batch *m, uint16_t off, uint32_t val)
{
    b_msg(m, (MSG_EXT_WRITE << 28) | OHCI_BASE | off, val);
}

/* PHY register access through PhyControl: rdReg (bit 15) + regAddr, and the
 * value comes back in PhyControl bits 23:16 once rdDone (bit 31) is set;
 * wrReg (bit 14) + regAddr + data. */
static int phy_read(pinnacle_1394_t *l, unsigned reg, uint8_t *val)
{
    uint32_t v = 0;
    if (p1394_reg_write(l, OHCI_PHY_CONTROL, 0x8000u | (reg << 8)) != 0)
        return -1;
    for (int i = 0; i < 10; i++) {
        if (p1394_reg_read(l, OHCI_PHY_CONTROL, &v) == 0 && (v & 0x80000000u)) {
            *val = (uint8_t)(v >> 16);
            return 0;
        }
        sleep_ms(2);
    }
    return -1;
}

static int phy_write(pinnacle_1394_t *l, unsigned reg, uint8_t val, unsigned delay)
{
    struct batch m = { .n = 0 };
    b_reg(&m, OHCI_PHY_CONTROL, 0x4000u | (reg << 8) | val);
    return b_send(l, &m, delay);
}

/* An AR context: two INPUT_MORE descriptors (s=1, b=3) that both fill the
 * same 8 KiB buffer and branch to each other, so the device DMAs received
 * packets into it forever; each packet is also reported on EP 0x84. */
static int ar_start(pinnacle_1394_t *l, uint16_t desc, uint32_t buf, uint16_t cmdptr_reg,
                    uint16_t set_reg)
{
    struct batch m = { .n = 0 };
    uint32_t d0[4] = { 0x280c2000u, buf, (uint32_t)(desc + 0x10) | 1u, 0x00200000u };
    uint32_t d1[4] = { 0x280c2000u, buf, (uint32_t)desc | 1u, 0x00200000u };
    b_ram(&m, desc, d0, 4);
    if (b_send(l, &m, 16) != 0) return -1;
    b_ram(&m, (uint16_t)(desc + 0x10), d1, 4);
    if (b_send(l, &m, 14) != 0) return -1;
    b_reg(&m, cmdptr_reg, (uint32_t)desc | 1u);
    if (b_send(l, &m, 16) != 0) return -1;
    b_reg(&m, set_reg, CTX_RUN | CTX_WAKE);
    if (b_send(l, &m, 14) != 0) return -1;
    /* bit 31 of AsynchronousRequestFilterHi: accept requests from every
     * node (asynReqResourceAll) */
    b_reg(&m, OHCI_ASYNC_FILTER_HI_SET, 0x80000000u);
    return b_send(l, &m, 16);
}

int p1394_link_init(pinnacle_1394_t *l)
{
    struct batch m = { .n = 0 };
    uint32_t v;
    uint8_t phy;

    /* FPGA USB side. MarvinBus64 first writes register 0 with its default
     * and then with 0 (its shadow being initialised); leaving both out made
     * no difference, and register 0 gets its value further down. */
    if (vendor_read(l, 0, &v) == 0 && l->verbose)
        pin_logf(PIN_LOG_DEBUG, "p1394: vendor status 0x%08x\n", v);
    /* index 1 is computed from the registry value LengthOfIsochBuffer
     * (default 0x5000): high byte (0x5000 / 0x1d4a) * 2 + 0xfa, low byte
     * 0x5000 / 0x4096. What the fields do is unknown; leaving it out made no
     * difference to HDV capture, kept for parity. */
    if (vendor_write(l, 1, 0xfe01, 1) != 0) return -1;

    /* Extension bank: literal constants in FUN_0002cd00, meaning unknown.
     * Required: without them the link init fails. */
    b_ext(&m, 0x040, 0);           if (b_send(l, &m, 14) != 0) return -1;
    b_ext(&m, 0x010, 0x00010000);  if (b_send(l, &m, 16) != 0) return -1;
    b_ext(&m, 0x004, 6);           if (b_send(l, &m, 16) != 0) return -1;
    b_ext(&m, 0x00c, 0x2008);      if (b_send(l, &m, 16) != 0) return -1;

    /* OHCI soft reset, then link power status on */
    b_reg(&m, OHCI_HC_SET, HC_SOFT_RESET);  if (b_send(l, &m, 16) != 0) return -1;
    b_ext(&m, 0x040, 0);                    if (b_send(l, &m, 46) != 0) return -1;
    b_reg(&m, OHCI_HC_SET, HC_LPS);         if (b_send(l, &m, 16) != 0) return -1;

    /* the 13-entry init table of FUN_0002cd00 */
    b_reg(&m, OHCI_LINK_CLEAR, 0xffffffffu);
    b_reg(&m, OHCI_ASYNC_FILTER_HI_CLEAR, 0xffffffffu);
    b_reg(&m, OHCI_ASYNC_FILTER_LO_CLEAR, 0xffffffffu);
    b_reg(&m, OHCI_LINK_SET, LINK_CYCLE_TIMER);
    b_reg(&m, OHCI_LINK_SET, LINK_CYCLE_MASTER);
    b_reg(&m, OHCI_INT_MASK_CLEAR, 0xffffffffu);
    b_reg(&m, OHCI_INT_EVENT_CLEAR, 0xffffffffu);
    b_reg(&m, OHCI_SELF_ID_BUFFER, 0x2000);      /* self-ID packets go to RAM 0x2000 */
    b_reg(&m, OHCI_LINK_SET, LINK_RCV_SELF_ID | LINK_RCV_PHY_PKT);
    b_reg(&m, OHCI_HC_CLEAR, HC_NO_BYTE_SWAP);   /* payloads in wire byte order */
    /* FPGA registers beyond the OHCI map, meaning unknown. 0x7000 is where
     * the isochronous receive buffer starts (IR0 below uses
     * 0x807000..0x80ffff). Required: without them isochronous data starts
     * and the command channel stalls within a second. */
    b_reg(&m, 0x800, 0x00007000);
    b_reg(&m, 0x808, 0x00107000);
    b_reg(&m, 0x840, 0x10000020);
    if (b_send(l, &m, 16) != 0) return -1;
    p1394_reg_read(l, OHCI_LINK_SET, &v);

    /* maxATReqRetries = maxATRespRetries = maxPhysRespRetries = 15,
     * cycleLimit 0x200 */
    b_reg(&m, OHCI_ATRETRIES, 0x40000fffu);
    if (b_send(l, &m, 14) != 0) return -1;
    p1394_reg_read(l, OHCI_ATRETRIES, &v);

    /* PHY register 4: set Contender (bit 6). Link-active (bit 7) reads back
     * already set (0x80 -> 0xc0 in every trace). */
    sleep_ms(4);
    if (phy_read(l, 4, &phy) != 0)
        phy = 0x80;
    if (phy_write(l, 4, (uint8_t)(phy | 0x40), 4) != 0) return -1;
    /* Port-status page: reg 7 = page 0 | port n, then reg 8 = 0x01
     * (Disabled). FUN_0002cc80 disables PHY ports 1 and 2; only port 0 is
     * wired to the socket (on the 500-USB -- other Marvin models may wire
     * more). */
    if (phy_write(l, 7, 0x01, 12) != 0) return -1;
    if (phy_write(l, 8, 0x01, 14) != 0) return -1;
    if (phy_write(l, 7, 0x02, 16) != 0) return -1;
    if (phy_write(l, 8, 0x01, 16) != 0) return -1;

    if (vendor_write(l, 0, USBCFG_DEFAULT, 16) != 0) return -1;

    /* receive contexts: AR request -> RAM 0x3000, AR response -> 0x5000 */
    if (ar_start(l, 0x1100, RAM_AR_REQ_LO, OHCI_ARREQ_CMDPTR, OHCI_ARREQ_SET) != 0) return -1;
    if (ar_start(l, 0x1140, RAM_AR_RSP_LO, OHCI_ARRSP_CMDPTR, OHCI_ARRSP_SET) != 0) return -1;

    /* Our configuration ROM, built around the GUID read from the device.
     * MarvinBus64 (FUN_0002c770) first mirrors the GUID, byte-swapped, into
     * the extension bank. */
    uint32_t guid_hi = l->dev->have_guid ? l->dev->guid_hi : FALLBACK_GUID_HI;
    uint32_t guid_lo = l->dev->have_guid ? l->dev->guid_lo : FALLBACK_GUID_LO;
    if (!l->dev->have_guid)
        pin_logf(PIN_LOG_WARN, "p1394: device GUID not read; publishing the development unit's\n");
    uint32_t rom[ROM_WORDS], be[ROM_WORDS];
    build_config_rom(rom, guid_hi, guid_lo);
    if (l->verbose > 1) {
        char line[16 + 9 * ROM_WORDS];
        int off = snprintf(line, sizeof(line), "p1394: config ROM");
        for (unsigned i = 0; i < ROM_WORDS; i++)
            off += snprintf(line + off, sizeof(line) - off, " %08x", rom[i]);
        snprintf(line + off, sizeof(line) - off, "\n");
        pin_logf(PIN_LOG_DEBUG, "%s", line);
    }
    b_ext(&m, 0x074, bswap32(guid_lo));
    if (b_send(l, &m, 13) != 0) return -1;
    b_ext(&m, 0x070, bswap32(guid_hi));
    if (b_send(l, &m, 18) != 0) return -1;
    for (unsigned i = 0; i < ROM_WORDS; i++)          /* stored big-endian */
        be[i] = bswap32(rom[i]);
    b_ram(&m, CONFIG_ROM_RAM, be, ROM_WORDS);
    if (b_send(l, &m, 13) != 0) return -1;
    b_reg(&m, OHCI_CONFIG_ROM_HDR, rom[0]);
    b_reg(&m, OHCI_BUS_ID, rom[1]);
    b_reg(&m, OHCI_BUS_OPTIONS, rom[2]);
    b_reg(&m, OHCI_GUID_HI, guid_hi);
    b_reg(&m, OHCI_GUID_LO, guid_lo);
    b_reg(&m, OHCI_VENDOR_ID, guid_hi >> 8);
    b_reg(&m, OHCI_CONFIG_ROM_MAP, CONFIG_ROM_RAM);
    b_reg(&m, OHCI_HC_SET, HC_LINK_ENABLE | HC_LPS);
    if (b_send(l, &m, 15) != 0) return -1;

    /* MarvinBus64 now reads the status register 20 times and ignores the
     * answers (op-table entry 2, FUN_0002b5a0), a ~40 ms settle delay.
     * Leaving it out made no difference. */

    /* PHY register 1: initiate bus reset (IBR, bit 6), gap count 63 */
    sleep_ms(4);
    if (phy_read(l, 1, &phy) != 0)
        phy = 0x3f;
    if (phy_write(l, 1, (uint8_t)(phy | 0x7f), 4) != 0) return -1;

    /* a bus reset stops the transmit contexts; make that explicit */
    b_reg(&m, OHCI_ATREQ_CLEAR, CTX_RUN);
    b_reg(&m, OHCI_ATRSP_CLEAR, CTX_RUN);
    if (b_send(l, &m, 2) != 0) return -1;
    sleep_ms(12);
    if (p1394_read_topology(l) != 0)
        return -1;
    b_reg(&m, OHCI_INT_EVENT_CLEAR, INT_BUS_RESET);
    return b_send(l, &m, 13);
}

int p1394_find_camera(pinnacle_1394_t *l, uint16_t *node, uint32_t *ompr)
{
    for (int n = 0; n < l->node_count && n < 63; n++) {
        uint16_t id = (uint16_t)(0xffc0u | (unsigned)n);
        uint32_t v;
        if (id == l->local_node)
            continue;
        if (p1394_read_quadlet(l, id, P1394_CSR_OMPR, &v) == 0) {
            *node = id;
            if (ompr)
                *ompr = v;
            return 0;
        }
    }
    return -1;
}

/* oPCR: bit 31 on-line, bit 30 broadcast connection, bits 29:24
 * point-to-point connection count, 21:16 channel, 15:14 data rate, 9:0
 * payload in quadlets. A connection is added or removed with a
 * compare-swap on the count, retried with the value the lock returns if
 * the register changed in between (IEC 61883-1). */
static int pcr_adjust(pinnacle_1394_t *l, uint16_t node, unsigned plug, int delta, uint32_t *pcr)
{
    uint32_t off = P1394_CSR_OPCR(plug), cur, old;
    if (p1394_read_quadlet(l, node, off, &cur) != 0)
        return -1;
    *pcr = cur;
    for (int i = 0; i < 4; i++) {
        unsigned p2p = (cur >> 24) & 0x3f;
        if ((delta < 0 && p2p == 0) || (delta > 0 && p2p == 0x3f))
            return -1;
        uint32_t want = (cur & ~(0x3fu << 24)) | ((uint32_t)((int)p2p + delta) << 24);
        if (p1394_compare_swap(l, node, off, cur, want, &old) != 0)
            return -1;
        if (old == cur) {
            *pcr = want;
            return 0;
        }
        cur = *pcr = old;
    }
    return -1;
}

int p1394_connect(pinnacle_1394_t *l, uint16_t node, uint32_t *opcr)
{
    return pcr_adjust(l, node, 0, +1, opcr);
}

int p1394_disconnect(pinnacle_1394_t *l, uint16_t node, uint32_t *opcr)
{
    return pcr_adjust(l, node, 0, -1, opcr);
}

/* Isochronous receive on IR context 0: a closed ring of two INPUT_MORE
 * descriptors that both fill the same 36 KiB buffer at 0x807000..0x80ffff,
 * in buffer-fill mode with the isochronous headers kept. The descriptors
 * live at RAM 0x1180/0x1190 but are addressed as 0x80118x: bit 23 is set on
 * every address of the isochronous path, apparently selecting the memory
 * that is forwarded to EP 0x88. */
int p1394_ir_start(pinnacle_1394_t *l, unsigned channel)
{
    struct batch m = { .n = 0 };
    if (vendor_write(l, 0, USBCFG_DEFAULT, 0) != 0)    /* isochronous-to-USB on */
        return -1;
    b_reg(&m, OHCI_IR0_MATCH, 0x20000000u);            /* tag1 = CIP */
    b_reg(&m, OHCI_IR0_SET, IR_BUFFER_FILL | IR_ISOCH_HEADER);
    b_reg(&m, OHCI_IR0_CLEAR, IR_CYCLE_MATCH | IR_MULTI_CHAN | IR_DUAL_BUFFER);
    if (b_send(l, &m, 2) != 0) return -1;
    b_reg(&m, OHCI_IR0_MATCH, 0x20000000u | (channel & 0x3f));
    if (b_send(l, &m, 2) != 0) return -1;
    uint32_t d0[4] = { 0x280c9000u, 0x00807000u, 0x00801191u, 0x9000u };
    uint32_t d1[4] = { 0x280c9000u, 0x00807000u, 0x00801181u, 0x9000u };
    b_ram(&m, 0x1180, d0, 4);
    if (b_send(l, &m, 0) != 0) return -1;
    b_ram(&m, 0x1190, d1, 4);
    if (b_send(l, &m, 2) != 0) return -1;
    b_reg(&m, OHCI_IR0_CMDPTR, 0x00801181u);
    if (b_send(l, &m, 0) != 0) return -1;
    b_reg(&m, OHCI_IR0_SET, CTX_RUN | CTX_WAKE);
    return b_send(l, &m, 1);
}

/* Only sends the stop messages; the caller must keep EP 0x88 drained. */
int p1394_ir_stop(pinnacle_1394_t *l)
{
    struct batch m = { .n = 0 };
    b_msg(&m, (MSG_VENDOR_WRITE << 28) | 0, USBCFG_DEFAULT | USBCFG_ISOCH_OFF);
    b_reg(&m, OHCI_IR0_CLEAR, CTX_RUN);
    return b_send(l, &m, 0);
}
