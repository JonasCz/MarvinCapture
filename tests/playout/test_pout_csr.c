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

/* pout_csr: request parsing from hand-built AR records, read / lock / write responses, compare-swap
 * semantics (including a failed compare), point-to-point count transitions, FCP command
 * extraction, the AV/C response header and the config ROM with an independent CRC-16. */

#include "pout_csr.h"

#include "pinnacle_1394.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

#define NODE_US 0xffc1u     /* our node */
#define NODE_CTL 0xffc0u    /* the controller */

static void le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static uint32_t rd_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* An OHCI AR request record: header quadlets (host order), payload (wire order, padded to a
 * quadlet), trailer with the ack in bits 20:16. Returns its length. */
static unsigned ar_rec(uint8_t *b, unsigned tcode, unsigned tl, uint32_t off_lo, uint32_t q3,
                       const uint8_t *pay, unsigned plen, unsigned ack)
{
    unsigned o = 0;
    le32(b + o, ((uint32_t)NODE_US << 16) | (tl << 10) | (tcode << 4)); o += 4;
    le32(b + o, ((uint32_t)NODE_CTL << 16) | 0xffffu); o += 4;
    le32(b + o, off_lo); o += 4;
    if (tcode != POUT_TC_READ_QUADLET) {
        if (tcode == POUT_TC_WRITE_QUADLET) {
            memcpy(b + o, pay, 4);          /* quadlet data: wire bytes */
        } else {
            le32(b + o, q3);
        }
        o += 4;
    }
    if (tcode == POUT_TC_WRITE_BLOCK || tcode == POUT_TC_LOCK) {
        unsigned padded = (plen + 3) & ~3u;
        memset(b + o, 0, padded);
        memcpy(b + o, pay, plen);
        o += padded;
    }
    le32(b + o, (ack << 16) | 0x0400u);     /* trailer: ack, some timestamp */
    return o + 4;
}

static unsigned rec_read_q(uint8_t *b, unsigned tl, uint32_t off)
{
    return ar_rec(b, POUT_TC_READ_QUADLET, tl, off, 0, NULL, 0, POUT_ACK_PENDING);
}

static unsigned rec_lock(uint8_t *b, unsigned tl, uint32_t off, uint32_t arg, uint32_t data, unsigned ext)
{
    uint8_t pay[8];
    be32(pay, arg); be32(pay + 4, data);
    return ar_rec(b, POUT_TC_LOCK, tl, off, (8u << 16) | ext, pay, 8, POUT_ACK_PENDING);
}

static int handle(pout_csr_t *c, const uint8_t *rec, unsigned len, pout_resp_t *o, pout_csr_event_t *ev,
                  pout_req_t *r)
{
    CHECK(pout_csr_parse(rec, len, r) == 0, "parse");
    return pout_csr_handle(c, r, o, ev);
}

static uint32_t read_reg(pout_csr_t *c, uint32_t off)
{
    uint8_t rec[64];
    pout_req_t r; pout_resp_t o; pout_csr_event_t ev;
    unsigned n = rec_read_q(rec, 9, off);
    CHECK(handle(c, rec, n, &o, &ev, &r) == 1, "read answered");
    CHECK(((o.hdr[1] >> 12) & 0xf) == 0, "read rcode complete");
    uint8_t b[4];
    le32(b, o.hdr[3]);          /* memory bytes of q3 are the wire bytes */
    return rd_be(b);
}

/* oPCR compare-swap through the whole path; returns the old value from the lock response */
static uint32_t lock_opcr(pout_csr_t *c, uint32_t arg, uint32_t data, pout_csr_event_t *ev)
{
    uint8_t rec[96];
    pout_req_t r; pout_resp_t o;
    unsigned n = rec_lock(rec, 20, POUT_CSR_OPCR0, arg, data, 2);
    CHECK(handle(c, rec, n, &o, ev, &r) == 1, "lock answered");
    CHECK(((o.hdr[0] >> 4) & 0xf) == POUT_TC_LOCK_RESP, "lock response tcode 0xb");
    CHECK(o.hdr[3] == ((4u << 16) | 2u) && o.plen == 4 && o.hdr_len == 16, "lock response length/ext tcode");
    return rd_be(o.payload);
}

static void test_parse(void)
{
    uint8_t rec[128];
    pout_req_t r;
    unsigned n = rec_read_q(rec, 17, POUT_CSR_OMPR);
    CHECK(n == 16, "quadlet read record is 12 + trailer");
    CHECK(pout_csr_parse(rec, n, &r) == 0, "parse quadlet read");
    CHECK(r.tcode == POUT_TC_READ_QUADLET && r.tl == 17 && r.src == NODE_CTL && r.dest == NODE_US, "ids");
    CHECK(r.offset_hi == 0xffff && r.offset == POUT_CSR_OMPR && r.length == 4 && r.data == NULL, "address");
    CHECK(r.ack == POUT_ACK_PENDING, "ack from the trailer");

    n = rec_lock(rec, 3, POUT_CSR_OPCR0, 0x11223344u, 0x55667788u, 2);
    CHECK(n == 16 + 8 + 4, "lock record");
    CHECK(pout_csr_parse(rec, n, &r) == 0 && r.tcode == POUT_TC_LOCK && r.ext_tcode == 2 && r.length == 8, "lock");
    CHECK(r.arg == 0x11223344u && r.datum == 0x55667788u, "lock arg/data are big-endian on the wire");

    uint8_t fcp[5] = { 0x01, 0xff, 0x19, 0x10, 0x00 };   /* 5 bytes: padded to 8 in the record */
    n = ar_rec(rec, POUT_TC_WRITE_BLOCK, 5, POUT_CSR_FCP_COMMAND, 5u << 16, fcp, 5, POUT_ACK_PENDING);
    CHECK(n == 16 + 8 + 4, "block write record is padded");
    CHECK(pout_csr_parse(rec, n, &r) == 0 && r.length == 5 && memcmp(r.data, fcp, 5) == 0, "block write");
    CHECK(pout_csr_parse(rec, n - 4, &r) != 0, "truncated block write");
    CHECK(pout_csr_parse(rec, 12, &r) != 0, "too short");

    n = ar_rec(rec, POUT_TC_WRITE_QUADLET, 1, POUT_CSR_FCP_COMMAND, 0, fcp, 4, POUT_ACK_COMPLETE);
    CHECK(n == 20 && pout_csr_parse(rec, n, &r) == 0 && r.length == 4 && r.ack == POUT_ACK_COMPLETE &&
          memcmp(r.data, fcp, 4) == 0, "quadlet write");

    le32(rec, ((uint32_t)NODE_US << 16) | (6u << 4));   /* a read response is not a request */
    CHECK(pout_csr_parse(rec, 20, &r) != 0, "response tcode refused");
}

static void test_registers(void)
{
    pout_csr_t c; pout_csr_cfg_t cfg;
    pout_csr_cfg_default(&cfg, 488);
    CHECK(cfg.payload_quadlets == 122, "DV payload 0x7a");
    CHECK(pout_csr_payload(584) == 146 && pout_csr_payload(100000) == 1023, "HDV payload 146, capped");
    pout_csr_init(&c, &cfg);
    CHECK(read_reg(&c, POUT_CSR_OMPR) == 0xbf000001u, "oMPR: S400, base 63, 1 output plug");
    CHECK(read_reg(&c, POUT_CSR_OPCR0) == 0x803f807au, "oPCR0: online, channel 63, S400, payload 122");
    CHECK(read_reg(&c, POUT_CSR_IMPR) == 0x80000000u, "iMPR: no input plugs");

    uint8_t rec[64];
    pout_req_t r; pout_resp_t o; pout_csr_event_t ev;
    pout_csr_cfg_t hcfg = cfg; hcfg.payload_quadlets = pout_csr_payload(584); hcfg.n_in_plugs = 1;
    pout_csr_t h; pout_csr_init(&h, &hcfg);
    CHECK((read_reg(&h, POUT_CSR_OPCR0) & 0x3ff) == 146, "HDV oPCR payload");
    CHECK(read_reg(&h, POUT_CSR_IMPR) == 0x80000001u && read_reg(&h, POUT_CSR_IPCR0) == 0x803f0000u, "iMPR/iPCR0 when present");

    /* response header of a quadlet read */
    unsigned n = rec_read_q(rec, 33, POUT_CSR_OMPR);
    CHECK(handle(&c, rec, n, &o, &ev, &r) == 1, "answered");
    CHECK(o.hdr_len == 16 && o.plen == 0, "quadlet read response header");
    CHECK(o.hdr[0] == ((33u << 10) | (1u << 8) | (6u << 4)), "q0: tlabel 33, tcode 6");
    CHECK(o.hdr[1] == ((uint32_t)NODE_CTL << 16), "q1: destination = requester, rcode complete");

    /* addresses we do not serve */
    uint32_t bad[] = { 0xf0000000u, 0xf0000908u, 0xf0000984u, 0xf0000400u, 0xf0000b00u };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        n = rec_read_q(rec, 2, bad[i]);
        CHECK(handle(&c, rec, n, &o, &ev, &r) == 1, "error response");
        CHECK(((o.hdr[1] >> 12) & 0xf) == POUT_RC_ADDRESS_ERROR && (o.hdr[0] >> 4 & 0xf) == 6, "address_error read response");
    }
    n = rec_read_q(rec, 2, POUT_CSR_OMPR);
    le32(rec + 4, ((uint32_t)NODE_CTL << 16) | 0x1234u);         /* offset_hi != 0xffff */
    CHECK(handle(&c, rec, n, &o, &ev, &r) == 1 && ((o.hdr[1] >> 12) & 0xf) == POUT_RC_ADDRESS_ERROR, "wrong high offset");

    /* block read over oMPR + oPCR0 */
    n = ar_rec(rec, POUT_TC_READ_BLOCK, 7, POUT_CSR_OMPR, 8u << 16, NULL, 0, POUT_ACK_PENDING);
    CHECK(handle(&c, rec, n, &o, &ev, &r) == 1, "block read answered");
    CHECK((o.hdr[0] >> 4 & 0xf) == POUT_TC_READ_BLOCK_RESP && o.hdr[3] == (8u << 16) && o.plen == 8, "block read response");
    CHECK(rd_be(o.payload) == 0xbf000001u && rd_be(o.payload + 4) == 0x803f807au, "block read data");
    n = ar_rec(rec, POUT_TC_READ_BLOCK, 7, POUT_CSR_OMPR, 12u << 16, NULL, 0, POUT_ACK_PENDING);
    CHECK(handle(&c, rec, n, &o, &ev, &r) == 1 && ((o.hdr[1] >> 12) & 0xf) == POUT_RC_ADDRESS_ERROR, "block read running off the plugs");
}

static void test_locks(void)
{
    pout_csr_t c; pout_csr_cfg_t cfg; pout_csr_event_t ev;
    unsigned ch, sp;
    pout_csr_cfg_default(&cfg, 488);
    pout_csr_init(&c, &cfg);
    CHECK(!pout_csr_tx(&c, &ch, &sp) && ch == 63 && sp == 2, "idle at start");

    /* a controller adds a point-to-point connection on channel 5 */
    uint32_t v0 = 0x803f807au;
    uint32_t v1 = (v0 & ~POUT_PCR_CHAN_MASK) | (5u << 16) | (1u << 24);
    CHECK(lock_opcr(&c, v0, v1, &ev) == v0, "returns the old value");
    CHECK(c.opcr == 0x8105807au, "oPCR now: count 1, channel 5");
    CHECK(pout_csr_tx(&c, &ch, &sp) == 1 && ch == 5 && sp == 2, "transmit on channel 5");
    CHECK(ev.plug_changed, "event: plug changed");

    /* a second controller loses the race: wrong arg_value, nothing changes, old value returned */
    uint32_t cur = c.opcr;
    CHECK(lock_opcr(&c, v0, v0 | (2u << 24), &ev) == cur, "failed compare returns the current value");
    CHECK(c.opcr == cur && !ev.plug_changed, "failed compare changes nothing");

    /* it retries with the value it got: count 2, still on, same channel: no change event */
    uint32_t v2 = (cur & ~POUT_PCR_P2P_MASK) | (2u << 24);
    CHECK(lock_opcr(&c, cur, v2, &ev) == cur, "retry compares equal");
    CHECK(((c.opcr >> 24) & 0x3f) == 2 && pout_csr_tx(&c, &ch, NULL) && !ev.plug_changed, "count 2, no event");

    /* the first one disconnects: count 1, still transmitting */
    uint32_t v3 = (v2 & ~POUT_PCR_P2P_MASK) | (1u << 24);
    lock_opcr(&c, v2, v3, &ev);
    CHECK(pout_csr_tx(&c, NULL, NULL) && !ev.plug_changed, "count 1: still on");
    /* the second one: count 0, transmission off */
    uint32_t v4 = v3 & ~POUT_PCR_P2P_MASK;
    lock_opcr(&c, v3, v4, &ev);
    CHECK(!pout_csr_tx(&c, NULL, NULL) && ev.plug_changed, "count 0: off, event");

    /* a controller cannot change online or the payload, only the connection fields */
    uint32_t cur2 = c.opcr;
    lock_opcr(&c, cur2, (cur2 & ~POUT_PCR_ONLINE & ~0x3ffu) | 0x155u | (1u << 24), &ev);
    CHECK((c.opcr & POUT_PCR_ONLINE) && (c.opcr & 0x3ff) == 122 && ((c.opcr >> 24) & 0x3f) == 1, "read-only bits kept");

    /* the broadcast connection also turns it on */
    pout_csr_init(&c, &cfg);
    lock_opcr(&c, 0x803f807au, 0x803f807au | POUT_PCR_BCAST, &ev);
    CHECK(pout_csr_tx(&c, &ch, NULL) && ch == 63 && ev.plug_changed, "broadcast connection: on, channel 63");

    /* a local play without a controller */
    pout_csr_init(&c, &cfg);
    pout_csr_local_connect(&c, 12, 1);
    CHECK(pout_csr_tx(&c, &ch, NULL) && ch == 12 && (c.opcr & POUT_PCR_BCAST), "local connect");
    pout_csr_local_connect(&c, 12, 0);
    CHECK(!pout_csr_tx(&c, NULL, NULL), "local disconnect");

    /* oMPR: the lock is answered, the register stays */
    uint8_t rec[96]; pout_req_t r; pout_resp_t o;
    unsigned n = rec_lock(rec, 4, POUT_CSR_OMPR, 0xbf000001u, 0x00000002u, 2);
    CHECK(handle(&c, rec, n, &o, &ev, &r) == 1 && rd_be(o.payload) == 0xbf000001u && c.ompr == 0xbf000001u, "oMPR is read only");

    /* other lock types, and locks elsewhere */
    n = rec_lock(rec, 4, POUT_CSR_OPCR0, 0, 0, 3);
    CHECK(handle(&c, rec, n, &o, &ev, &r) == 1 && ((o.hdr[1] >> 12) & 0xf) == POUT_RC_TYPE_ERROR &&
          (o.hdr[0] >> 4 & 0xf) == POUT_TC_LOCK_RESP, "fetch_add: type_error");
    n = rec_lock(rec, 4, 0xf0000220u, 0, 0, 2);
    CHECK(handle(&c, rec, n, &o, &ev, &r) == 1 && ((o.hdr[1] >> 12) & 0xf) == POUT_RC_ADDRESS_ERROR, "lock elsewhere: address_error");
}

static void test_fcp(void)
{
    pout_csr_t c; pout_csr_cfg_t cfg; pout_csr_event_t ev;
    pout_csr_cfg_default(&cfg, 488);
    pout_csr_init(&c, &cfg);
    uint8_t rec[160], cmd[8] = { 0x00, 0x20, 0xc3, 0x75, 0x00, 0x00, 0x00, 0x00 };
    pout_req_t r; pout_resp_t o;

    unsigned n = ar_rec(rec, POUT_TC_WRITE_BLOCK, 12, POUT_CSR_FCP_COMMAND, 8u << 16, cmd, 8, POUT_ACK_PENDING);
    CHECK(handle(&c, rec, n, &o, &ev, &r) == 1, "write response owed");
    CHECK(ev.fcp_len == 8 && memcmp(ev.fcp, cmd, 8) == 0 && ev.fcp_src == NODE_CTL, "frame extracted");
    CHECK(o.hdr_len == 12 && o.hdr[0] == ((12u << 10) | (1u << 8) | (2u << 4)) &&
          o.hdr[1] == ((uint32_t)NODE_CTL << 16), "write response: tcode 2, rcode complete");

    /* acked complete: nothing owed, the frame is still delivered */
    n = ar_rec(rec, POUT_TC_WRITE_BLOCK, 12, POUT_CSR_FCP_COMMAND, 8u << 16, cmd, 8, POUT_ACK_COMPLETE);
    CHECK(handle(&c, rec, n, &o, &ev, &r) == 0 && ev.fcp_len == 8, "no response after ack_complete");

    /* a 4-byte frame arrives as a quadlet write */
    n = ar_rec(rec, POUT_TC_WRITE_QUADLET, 13, POUT_CSR_FCP_COMMAND, 0, cmd, 4, POUT_ACK_PENDING);
    CHECK(handle(&c, rec, n, &o, &ev, &r) == 1 && ev.fcp_len == 4 && memcmp(ev.fcp, cmd, 4) == 0, "quadlet FCP write");

    /* writes elsewhere: no frame, an error response */
    n = ar_rec(rec, POUT_TC_WRITE_BLOCK, 14, POUT_CSR_FCP_COMMAND + 0x400u, 8u << 16, cmd, 8, POUT_ACK_PENDING);
    CHECK(handle(&c, rec, n, &o, &ev, &r) == 1 && ev.fcp_len == 0 && ((o.hdr[1] >> 12) & 0xf) == POUT_RC_ADDRESS_ERROR, "write elsewhere");
    n = ar_rec(rec, POUT_TC_WRITE_QUADLET, 14, POUT_CSR_OPCR0, 0, cmd, 4, POUT_ACK_PENDING);
    CHECK(handle(&c, rec, n, &o, &ev, &r) == 1 && ((o.hdr[1] >> 12) & 0xf) == POUT_RC_TYPE_ERROR, "write to a plug: type_error");

    /* the AV/C response write */
    uint8_t resp[8] = { 0x0c, 0x20, 0xc3, 0x75, 0x00, 0x00, 0x00, 0x00 };
    CHECK(pout_csr_fcp_write(&o, 40, NODE_CTL, resp, 8) == 0, "block response");
    CHECK(o.hdr_len == 16 && o.hdr[0] == ((40u << 10) | (1u << 4)) && o.hdr[1] == (((uint32_t)NODE_CTL << 16) | 0xffffu) &&
          o.hdr[2] == 0xf0000d00u && o.hdr[3] == (8u << 16) && o.plen == 8 && memcmp(o.payload, resp, 8) == 0, "block write header");
    CHECK(pout_csr_fcp_write(&o, 41, NODE_CTL, resp, 4) == 0, "quadlet response");
    CHECK(o.hdr[0] == (41u << 10) && o.plen == 0 && o.hdr[3] == (uint32_t)(resp[0] | resp[1] << 8 | resp[2] << 16 | (uint32_t)resp[3] << 24), "quadlet write header");
    CHECK(pout_csr_fcp_write(&o, 1, NODE_CTL, resp, 0) != 0 && pout_csr_fcp_write(&o, 1, NODE_CTL, resp, 200) != 0, "bad lengths");
}

/* IEEE 1212 8.1.4 / the Linux crc16: independent of the byte-wise code under test */
static uint16_t crc1212(const uint32_t *d, unsigned n)
{
    unsigned crc = 0;
    for (unsigned i = 0; i < n; i++)
        for (int shift = 28; shift >= 0; shift -= 4) {
            unsigned sum = ((crc >> 12) ^ (d[i] >> shift)) & 0xf;
            crc = (crc << 4) ^ (sum << 12) ^ (sum << 5) ^ sum;
        }
    return (uint16_t)crc;
}

static void test_rom(void)
{
    uint32_t rom[POUT_CSR_ROM_WORDS];
    pout_csr_config_rom(rom, 0xfb82ad03u, 0x128d3000u);
    CHECK(rom[1] == 0x31333934u && rom[3] == 0xfb82ad03u && rom[4] == 0x128d3000u, "bus info block");
    CHECK((rom[0] >> 24) == 4 && ((rom[0] >> 16) & 0xff) == 4 && (rom[0] & 0xffff) == crc1212(rom + 1, 4), "bus info CRC");
    /* root directory at 5: 4 entries, CRC over them; its leaf at 8 + 7 and unit directory at 9 + 1 */
    CHECK(((rom[5] >> 16) & 0xffff) == 4 && (rom[5] & 0xffff) == crc1212(rom + 6, 4), "root directory CRC");
    CHECK(rom[8] == 0x81000007u && 8 + 7 == 15 && rom[9] == 0xd1000001u && 9 + 1 == 10, "root entries point at the leaf and unit directory");
    CHECK(rom[7] == (0x03000000u | 0xfb82ad), "module vendor ID from the GUID");
    /* unit directory at 10 */
    CHECK(((rom[10] >> 16) & 0xffff) == 4 && (rom[10] & 0xffff) == crc1212(rom + 11, 4), "unit directory CRC");
    CHECK(rom[11] == 0x1200a02du, "unit spec ID 0x00A02D");
    CHECK(rom[12] == 0x13010001u, "unit software version 0x010001");
    CHECK(rom[14] == 0x8100000du && 14 + 13 == 27, "unit directory's text leaf");
    /* text leaves: 15 (length 11) and 27 (length 10) */
    CHECK(((rom[15] >> 16) & 0xffff) == 11 && (rom[15] & 0xffff) == crc1212(rom + 16, 11), "leaf 1 CRC");
    CHECK(((rom[27] >> 16) & 0xffff) == 10 && (rom[27] & 0xffff) == crc1212(rom + 28, 10), "leaf 2 CRC");
    CHECK(15 + 12 == 27 && 27 + 11 == POUT_CSR_ROM_WORDS, "layout is 38 quadlets");
    CHECK(p1394_rom_crc16(rom + 1, 4) == crc1212(rom + 1, 4) && p1394_rom_crc16(rom + 28, 10) == crc1212(rom + 28, 10), "core CRC helper");

    /* the capture ROM keeps the vendor's layout and CRC byte order */
    uint32_t cap[P1394_ROM_WORDS];
    p1394_config_rom_build(cap, 0xfb82ad03u, 0x128d3000u, 0);
    CHECK(cap[11] == (0x12000000u | 0xfb82ad) && cap[12] == 0x13000000u, "capture unit directory unchanged");
    uint32_t sw[4];                       /* the same words with bytes reversed per quadlet */
    for (int i = 0; i < 4; i++) {
        uint32_t w = cap[11 + i];
        sw[i] = (w >> 24) | ((w >> 8) & 0xff00u) | ((w << 8) & 0xff0000u) | (w << 24);
    }
    CHECK((cap[10] & 0xffff) == crc1212(sw, 4), "capture CRC is the vendor (little-endian byte order) one");
}

int main(void)
{
    test_parse();
    test_registers();
    test_locks();
    test_fcp();
    test_rom();
    if (g_failures) {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_pout_csr: OK\n");
    return 0;
}
