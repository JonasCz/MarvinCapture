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

/* pinnacle_1394 without a device: EP 0x84 records are built here and fed to
 * p1394_parse_ep84(), and p1394_send() goes to a hook that records what would
 * have been written to EP 0x02. Covers response slots by transaction label,
 * the owed write-response table and its overflow, message skipping and split
 * messages, AV/C response matching, and the async AT queue: owed write
 * responses before a queued AV/C command, ack_busy resent, a refused ack
 * failing the command. Also pinnacle_cfg's reply matching. */

#include "pinnacle_1394.h"
#include "pinnacle_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, msg)                                                          \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);         \
            exit(1);                                                              \
        }                                                                         \
    } while (0)

#define AT_STATUS_NOPAY 0x11cc    /* AT_SLOT + 0x0c */
#define AT_STATUS_PAY   0x11ec    /* AT_SLOT + 0x2c */
#define CAMERA          0xffc1u

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* type-9 message: RAM write of n bytes at ram */
static int msg9(uint8_t *out, uint16_t ram, const uint8_t *d, unsigned n)
{
    put32(out, (9u << 28) | (n << 16) | ram);
    memcpy(out + 4, d, n);
    return 4 + (int)n;
}

/* AR response packet (read quadlet response) for label tl with data quadlet v */
static int ar_read_resp(uint8_t *out, unsigned tl, uint16_t src, uint32_t v_be)
{
    uint8_t d[20];
    put32(d, (tl << 10) | (6u << 4));
    put32(d + 4, ((uint32_t)src << 16));
    put32(d + 8, 0);
    d[12] = (uint8_t)(v_be >> 24); d[13] = (uint8_t)(v_be >> 16);
    d[14] = (uint8_t)(v_be >> 8); d[15] = (uint8_t)v_be;
    put32(d + 16, 0x11u << 16);
    return msg9(out, 0x5000, d, sizeof(d));
}

/* FCP response from the camera (block write to FCP_RESPONSE), our link acked with ack */
static int ar_fcp(uint8_t *out, unsigned tl, const uint8_t *frame, unsigned len, unsigned ack)
{
    uint8_t d[16 + 64 + 4];
    put32(d, (tl << 10) | (1u << 4));
    put32(d + 4, ((uint32_t)CAMERA << 16) | 0xffff);
    put32(d + 8, P1394_CSR_FCP_RESPONSE);
    put32(d + 12, len << 16);
    memcpy(d + 16, frame, len);
    unsigned padded = (len + 3) & ~3u;
    memset(d + 16 + len, 0, padded - len);
    put32(d + 16 + padded, ack << 16);
    return msg9(out, 0x3000, d, 16 + padded + 4);
}

/* xferStatus writeback of our AT packet with event evt */
static int at_status(uint8_t *out, uint16_t addr, unsigned evt)
{
    uint8_t d[4];
    put32(d, (0x8400u | evt) << 16);
    return msg9(out, addr, d, 4);
}

static void feed(pinnacle_1394_t *l, const uint8_t *p, int n)
{
    CHECK(p1394_parse_ep84(l, p, n) == n, "a whole record is used");
}

/* --- the send hook ------------------------------------------------------ */

struct sent {
    int count;
    uint32_t desc0[32];   /* first descriptor quadlet of each AT packet sent */
    uint32_t hdr0[32];    /* first header quadlet */
};

static int hook(void *user, const uint8_t *msg, int len)
{
    struct sent *s = user;
    CHECK(len >= 24, "an AT packet");
    if (s->count < 32) {
        s->desc0[s->count] = get32(msg + 12);
        s->hdr0[s->count] = get32(msg + 28);
    }
    s->count++;
    return 0;
}

static int is_write_resp(uint32_t desc0, uint32_t hdr0)
{
    return desc0 == (0x120c0000u | 12) && ((hdr0 >> 4) & 0xf) == 2;
}

static int is_avc_quadlet(uint32_t desc0, uint32_t hdr0)
{
    return desc0 == (0x120c0000u | 16) && ((hdr0 >> 4) & 0xf) == 0;
}

static void test_matches(void)
{
    const uint8_t play_fwd[4] = { 0x00, 0x20, 0xc3, 0x75 };
    const uint8_t r_fwd[4] = { 0x09, 0x20, 0xc3, 0x75 };
    const uint8_t r_rev[4] = { 0x09, 0x20, 0xc3, 0x65 };
    const uint8_t r_other_sub[4] = { 0x09, 0x21, 0xc3, 0x75 };
    CHECK(p1394_avc_response_matches(play_fwd, 4, r_fwd, 4), "same opcode and operand");
    CHECK(!p1394_avc_response_matches(play_fwd, 4, r_rev, 4), "another operand is another command");
    CHECK(!p1394_avc_response_matches(play_fwd, 4, r_other_sub, 4), "another subunit");
    CHECK(!p1394_avc_response_matches(play_fwd, 4, r_fwd, 3), "a response too short for the operand");

    const uint8_t state[4] = { 0x01, 0x20, 0xd0, 0x7f };
    const uint8_t r_state[4] = { 0x0c, 0x20, 0xc3, 0x75 };
    CHECK(p1394_avc_response_matches(state, 4, r_state, 4), "TRANSPORT STATE answers with the mode");

    const uint8_t tc[8] = { 0x01, 0x20, 0x51, 0x71, 0xff, 0xff, 0xff, 0xff };
    const uint8_t r_tc[8] = { 0x0c, 0x20, 0x51, 0x71, 0x00, 0x12, 0x34, 0x05 };
    const uint8_t r_tc_other[8] = { 0x0c, 0x20, 0x51, 0x70, 0x00, 0x12, 0x34, 0x05 };
    CHECK(p1394_avc_response_matches(tc, 8, r_tc, 8), "timecode status");
    CHECK(!p1394_avc_response_matches(tc, 8, r_tc_other, 8), "timecode with another subfunction");

    const uint8_t open_op[4] = { 0x01, 0xff, 0x30, 0xff };
    const uint8_t r_open[4] = { 0x0c, 0xff, 0x30, 0x07 };
    CHECK(p1394_avc_response_matches(open_op, 4, r_open, 4), "an open (0xff) operand is filled in");
    printf("ok: AV/C response matching\n");
}

static void test_parse(void)
{
    pinnacle_1394_t l;
    p1394_init(&l, NULL);
    uint8_t buf[4096];
    int n = 0;

    /* responses for two labels arrive; neither overwrites the other */
    n += ar_read_resp(buf + n, 5, CAMERA, 0x11223344u);
    n += ar_read_resp(buf + n, 7, CAMERA, 0x55667788u);
    feed(&l, buf, n);
    CHECK(l.rsp[5].valid && l.rsp[7].valid, "both responses kept");
    CHECK(l.rsp[5].data[0] == 0x11 && l.rsp[7].data[0] == 0x55, "each under its own label");

    /* an unknown message type is skipped as one two-quadlet message */
    n = 0;
    put32(buf + n, 0x70000000u); put32(buf + n + 4, 0xdeadbeefu); n += 8;
    put32(buf + n, (3u << 28) | 0x10400u); put32(buf + n + 4, 0x8000u); n += 8;
    int seq = l.reg_seq;
    feed(&l, buf, n);
    CHECK(l.reg_seq == seq + 1 && l.reg_val == 0x8000u, "the message after an unknown one is read");

    /* a message cut off at the end is left for the next transfer */
    n = ar_read_resp(buf, 9, CAMERA, 0x01020304u);
    CHECK(p1394_parse_ep84(&l, buf, n - 6) == 0, "an incomplete record is not used");
    CHECK(!l.rsp[9].valid, "and not taken");
    CHECK(p1394_parse_ep84(&l, buf, n) == n && l.rsp[9].valid, "complete, it is taken");
    put32(buf, (3u << 28) | 0x10400u);
    CHECK(p1394_parse_ep84(&l, buf, 6) == 0, "a cut two-quadlet message is not used");

    /* FCP responses acked pending are owed; past the table they are counted */
    const uint8_t frame[4] = { 0x09, 0x20, 0xc3, 0x75 };
    for (int i = 0; i < 70; i++) {
        n = ar_fcp(buf, (unsigned)(i % 60) + 2, frame, 4, P1394_EVT_ACK_PENDING);
        feed(&l, buf, n);
    }
    CHECK(l.n_owed == 64, "the owed table is full");
    CHECK(l.owed_dropped == 6, "the overflow is counted");
    CHECK(l.fcp_seq == 70, "every FCP response is kept in the ring");
    printf("ok: EP 0x84 parsing\n");
}

static void test_async(void)
{
    pinnacle_1394_t l;
    struct sent s;
    uint8_t buf[512], resp[64];
    const uint8_t play[4] = { 0x00, 0x20, 0xc3, 0x75 };

    /* no owed response: the command goes out at once; ack_busy resends it */
    p1394_init(&l, NULL);
    memset(&s, 0, sizeof(s));
    l.send_hook = hook;
    l.send_hook_user = &s;
    CHECK(p1394_avc_begin(&l, CAMERA, play, 4) == 0, "begin");
    CHECK(s.count == 1 && is_avc_quadlet(s.desc0[0], s.hdr0[0]), "the command is sent at once");
    CHECK(p1394_avc_poll(&l, resp, sizeof(resp)) == 0, "waiting for the ack");
    feed(&l, buf, at_status(buf, AT_STATUS_NOPAY, 0x15));     /* ack_busy_A */
    CHECK(p1394_avc_poll(&l, resp, sizeof(resp)) == 0, "still waiting");
    CHECK(s.count == 2 && is_avc_quadlet(s.desc0[1], s.hdr0[1]), "resent after ack_busy");
    feed(&l, buf, at_status(buf, AT_STATUS_NOPAY, P1394_EVT_ACK_COMPLETE));
    CHECK(p1394_avc_poll(&l, resp, sizeof(resp)) == 0 && l.avc_state == P1394_AVC_DELIVERED,
          "delivered");

    /* a late answer to PLAY reverse is not this command's; then the real one, acked
     * pending: answered by a write response from the async service */
    const uint8_t r_rev[4] = { 0x09, 0x20, 0xc3, 0x65 };
    const uint8_t r_fwd[4] = { 0x09, 0x20, 0xc3, 0x75 };
    feed(&l, buf, ar_fcp(buf, 3, r_rev, 4, P1394_EVT_ACK_COMPLETE));
    CHECK(p1394_avc_poll(&l, resp, sizeof(resp)) == 0, "the other command's answer is ignored");
    feed(&l, buf, ar_fcp(buf, 4, r_fwd, 4, P1394_EVT_ACK_PENDING));
    CHECK(l.n_owed == 1, "a write response is owed");
    int rl = p1394_avc_poll(&l, resp, sizeof(resp));
    CHECK(rl == 4 && resp[0] == 0x09 && resp[3] == 0x75, "the matching answer completes the command");
    CHECK(s.count == 3 && is_write_resp(s.desc0[2], s.hdr0[2]) && ((s.hdr0[2] >> 10) & 0x3f) == 4,
          "the owed write response went out");
    feed(&l, buf, at_status(buf, AT_STATUS_NOPAY, P1394_EVT_ACK_COMPLETE));
    p1394_async_service(&l);
    CHECK(l.n_owed == 0 && l.at_async == 0, "the write response is done");

    /* owed responses go before a queued command */
    feed(&l, buf, ar_fcp(buf, 5, r_fwd, 4, P1394_EVT_ACK_PENDING));
    feed(&l, buf, ar_fcp(buf, 6, r_fwd, 4, P1394_EVT_ACK_PENDING));
    int before = s.count;
    CHECK(p1394_avc_begin(&l, CAMERA, play, 4) == 0, "begin while responses are owed");
    CHECK(s.count == before + 1 && is_write_resp(s.desc0[before], s.hdr0[before]),
          "the first owed write response goes first");
    feed(&l, buf, at_status(buf, AT_STATUS_NOPAY, P1394_EVT_ACK_COMPLETE));
    p1394_async_service(&l);
    CHECK(s.count == before + 2 && is_write_resp(s.desc0[before + 1], s.hdr0[before + 1]),
          "then the second");
    CHECK(l.avc_state == P1394_AVC_QUEUED, "the command still waits");
    feed(&l, buf, at_status(buf, AT_STATUS_NOPAY, P1394_EVT_ACK_COMPLETE));
    CHECK(p1394_avc_poll(&l, resp, sizeof(resp)) == 0, "poll");
    CHECK(s.count == before + 3 && is_avc_quadlet(s.desc0[before + 2], s.hdr0[before + 2]),
          "then the command");

    /* a refused ack (no ack from the camera) fails the command */
    feed(&l, buf, at_status(buf, AT_STATUS_NOPAY, P1394_EVT_MISSING_ACK));
    CHECK(p1394_avc_poll(&l, resp, sizeof(resp)) == -1, "a missing ack fails the command");
    CHECK(!l.avc_pending, "nothing pending afterwards");
    printf("ok: async AT queue\n");
}

static void test_cfg(void)
{
    const uint8_t op05[2] = { 0x05, 0x00 };
    const uint8_t r05[2] = { 0x05, 0x01 };
    const uint8_t r06[2] = { 0x06, 0x01 };
    CHECK(pinnacle_cfg_reply_matches(op05, 2, r05, 2), "05 answers 05");
    CHECK(!pinnacle_cfg_reply_matches(op05, 2, r06, 2), "a late 06 reply does not answer 05");
    CHECK(!pinnacle_cfg_reply_matches(op05, 2, r05, 0), "an empty reply answers nothing");
    const uint8_t op08[2] = { 0x08, 0x00 };
    const uint8_t r08[1] = { 0x08 };
    CHECK(pinnacle_cfg_reply_matches(op08, 2, r08, 1), "08's one-byte reply");

    const uint8_t guid[10] = { 0x80, 0x03, 0x08 };
    uint8_t r[20] = { 0x80, 0x03, 0x08, 0x00 };
    CHECK(pinnacle_cfg_reply_matches(guid, 10, r, 20), "80 03 08 answered");
    /* what the 500-USB and 510-USB really send (2026-11-19): 80 03 01 00, then the GUID */
    const uint8_t real[20] = { 0x80, 0x03, 0x01, 0x00, 0x0e, 0x73, 0x35, 0x00, 0x12, 0x8d, 0x30, 0x00 };
    CHECK(pinnacle_cfg_reply_matches(guid, 10, real, 20), "the real GUID reply answers 80 03 08");
    CHECK(!pinnacle_cfg_reply_matches(guid, 10, r, 8), "too short for the 8 bytes");
    r[1] = 0x00;
    CHECK(!pinnacle_cfg_reply_matches(guid, 10, r, 20), "the reply for index 0 is not index 3's");
    printf("ok: config reply matching\n");
}

int main(void)
{
    test_matches();
    test_parse();
    test_async();
    test_cfg();
    printf("all pinnacle_1394 tests passed\n");
    return 0;
}
