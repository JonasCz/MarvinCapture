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

#include "pout_csr.h"

#include "pinnacle_1394.h"   /* the ROM builder; no libusb call is made from here */

#include <string.h>

static uint32_t get32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t get32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* quadlet data in an AT header word: memory bytes = wire bytes */
static uint32_t wire_word(uint32_t v)
{
    uint8_t b[4];
    put32be(b, v);
    return get32le(b);
}

unsigned pout_csr_payload(unsigned max_packet_bytes)
{
    unsigned q = max_packet_bytes / 4;
    return q > 1023 ? 1023 : q;
}

void pout_csr_cfg_default(pout_csr_cfg_t *cfg, unsigned max_packet_bytes)
{
    cfg->rate = 2;
    cfg->bcast_base = 63;
    cfg->payload_quadlets = pout_csr_payload(max_packet_bytes);
    cfg->overhead_id = 0;
    cfg->n_in_plugs = 0;
}

void pout_csr_set_payload(pout_csr_t *c, unsigned max_packet_bytes)
{
    c->cfg.payload_quadlets = pout_csr_payload(max_packet_bytes);
    c->opcr = (c->opcr & ~POUT_PCR_PAYLOAD_MASK) | (c->cfg.payload_quadlets & POUT_PCR_PAYLOAD_MASK);
}

void pout_csr_init(pout_csr_t *c, const pout_csr_cfg_t *cfg)
{
    memset(c, 0, sizeof(*c));
    c->cfg = *cfg;
    c->cfg.rate &= 3;
    c->ompr = (c->cfg.rate << 30) | ((c->cfg.bcast_base & 0x3f) << 24) | 1u;
    c->opcr = POUT_PCR_ONLINE | (63u << POUT_PCR_CHAN_SHIFT) | (c->cfg.rate << POUT_PCR_RATE_SHIFT) |
              ((c->cfg.overhead_id & 0xf) << POUT_PCR_OVH_SHIFT) |
              (c->cfg.payload_quadlets & POUT_PCR_PAYLOAD_MASK);
    c->impr = (c->cfg.rate << 30) | (c->cfg.n_in_plugs ? 1u : 0u);
    c->ipcr = POUT_PCR_ONLINE | (63u << POUT_PCR_CHAN_SHIFT);
    c->tx_channel = 63;
}

int pout_csr_tx(const pout_csr_t *c, unsigned *channel, unsigned *speed)
{
    int on = (c->opcr & POUT_PCR_ONLINE) &&
             ((c->opcr & POUT_PCR_BCAST) || ((c->opcr & POUT_PCR_P2P_MASK) >> POUT_PCR_P2P_SHIFT) > 0);
    if (channel)
        *channel = (c->opcr & POUT_PCR_CHAN_MASK) >> POUT_PCR_CHAN_SHIFT;
    if (speed)
        *speed = (c->opcr & POUT_PCR_RATE_MASK) >> POUT_PCR_RATE_SHIFT;
    return on;
}

void pout_csr_local_connect(pout_csr_t *c, unsigned channel, int on)
{
    if (on)
        c->opcr = (c->opcr & ~POUT_PCR_CHAN_MASK) | ((channel & 0x3f) << POUT_PCR_CHAN_SHIFT) | POUT_PCR_BCAST;
    else
        c->opcr &= ~POUT_PCR_BCAST;
    c->tx_on = pout_csr_tx(c, &c->tx_channel, NULL);
}

int pout_csr_parse(const uint8_t *rec, unsigned len, pout_req_t *r)
{
    if (!rec || len < 16)
        return -1;
    uint32_t q0 = get32le(rec), q1 = get32le(rec + 4), q2 = get32le(rec + 8);
    unsigned tcode = (q0 >> 4) & 0xf;
    unsigned hdr;
    memset(r, 0, sizeof(*r));
    r->tcode = tcode;
    r->tl = (q0 >> 10) & 0x3f;
    r->rt = (q0 >> 8) & 3;
    r->dest = (uint16_t)(q0 >> 16);
    r->src = (uint16_t)(q1 >> 16);
    r->offset_hi = (uint16_t)(q1 & 0xffff);
    r->offset = q2;
    r->ack = (get32le(rec + len - 4) >> 16) & 0x1f;

    switch (tcode) {
    case POUT_TC_READ_QUADLET:
        r->length = 4;
        return 0;                                  /* 12-byte header, then the trailer */
    case POUT_TC_WRITE_QUADLET:
        if (len < 20)
            return -1;
        r->length = 4;
        r->data = rec + 12;                        /* q3 holds the data, bytes in wire order */
        return 0;
    case POUT_TC_READ_BLOCK:
        if (len < 20)
            return -1;
        r->length = get32le(rec + 12) >> 16;
        return 0;
    case POUT_TC_WRITE_BLOCK:
    case POUT_TC_LOCK: {
        hdr = 16;
        if (len < hdr + 4)
            return -1;
        uint32_t q3 = get32le(rec + 12);
        r->length = q3 >> 16;
        r->ext_tcode = q3 & 0xffff;
        if (hdr + ((r->length + 3) & ~3u) + 4 > len)
            return -1;
        r->data = rec + hdr;
        if (tcode == POUT_TC_LOCK && r->ext_tcode == 2 && r->length == 8) {
            r->arg = get32be(r->data);
            r->datum = get32be(r->data + 4);
        }
        return 0;
    }
    default:
        return -1;
    }
}

/* --- responses --- */

static void resp_head(pout_resp_t *o, const pout_req_t *r, unsigned tcode, unsigned rcode)
{
    memset(o, 0, sizeof(*o));
    o->hdr[0] = (r->tl << 10) | (1u << 8) | (tcode << 4);
    o->hdr[1] = ((uint32_t)r->src << 16) | (rcode << 12);
    o->hdr_len = 12;                               /* write response */
}

static void resp_error(pout_resp_t *o, const pout_req_t *r, unsigned rcode)
{
    switch (r->tcode) {
    case POUT_TC_READ_QUADLET:
        resp_head(o, r, POUT_TC_READ_QUADLET_RESP, rcode);
        o->hdr_len = 16;
        break;
    case POUT_TC_READ_BLOCK:
        resp_head(o, r, POUT_TC_READ_BLOCK_RESP, rcode);
        o->hdr_len = 16;
        break;
    case POUT_TC_LOCK:
        resp_head(o, r, POUT_TC_LOCK_RESP, rcode);
        o->hdr[3] = r->ext_tcode;
        o->hdr_len = 16;
        break;
    default:
        resp_head(o, r, POUT_TC_WRITE_RESP, rcode);
        break;
    }
}

/* The register a CSR address names, or NULL. *is_pcr tells plugs from masters. */
static uint32_t *reg_of(pout_csr_t *c, const pout_req_t *r, int *is_pcr)
{
    if (r->offset_hi != 0xffff)
        return NULL;
    *is_pcr = 0;
    switch (r->offset) {
    case POUT_CSR_OMPR: return &c->ompr;
    case POUT_CSR_IMPR: return &c->impr;
    case POUT_CSR_OPCR0: *is_pcr = 1; return &c->opcr;
    case POUT_CSR_IPCR0: if (c->cfg.n_in_plugs) { *is_pcr = 1; return &c->ipcr; } return NULL;
    }
    return NULL;
}

static int read_regs(pout_csr_t *c, const pout_req_t *r, uint32_t *words, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        pout_req_t q = *r;
        int pcr;
        q.offset = r->offset + 4 * i;
        uint32_t *reg = reg_of(c, &q, &pcr);
        if (!reg)
            return -1;
        words[i] = *reg;
    }
    return 0;
}

int pout_csr_handle(pout_csr_t *c, const pout_req_t *r, pout_resp_t *o, pout_csr_event_t *ev)
{
    memset(ev, 0, sizeof(*ev));
    int pcr = 0;

    switch (r->tcode) {
    case POUT_TC_READ_QUADLET: {
        uint32_t *reg = reg_of(c, r, &pcr);
        if (!reg) {
            resp_error(o, r, POUT_RC_ADDRESS_ERROR);
            return 1;
        }
        resp_head(o, r, POUT_TC_READ_QUADLET_RESP, POUT_RC_COMPLETE);
        o->hdr_len = 16;
        o->hdr[3] = wire_word(*reg);
        return 1;
    }
    case POUT_TC_READ_BLOCK: {
        uint32_t w[POUT_RESP_PAYLOAD_MAX / 4];
        unsigned n = r->length / 4;
        if (r->offset_hi != 0xffff || r->length == 0 || (r->length & 3) || r->length > sizeof(w) ||
            read_regs(c, r, w, n) != 0) {
            resp_error(o, r, POUT_RC_ADDRESS_ERROR);
            return 1;
        }
        resp_head(o, r, POUT_TC_READ_BLOCK_RESP, POUT_RC_COMPLETE);
        o->hdr_len = 16;
        o->hdr[3] = r->length << 16;
        for (unsigned i = 0; i < n; i++)
            put32be(o->payload + 4 * i, w[i]);
        o->plen = r->length;
        return 1;
    }
    case POUT_TC_LOCK: {
        uint32_t *reg = reg_of(c, r, &pcr);
        if (!reg) {
            resp_error(o, r, POUT_RC_ADDRESS_ERROR);
            return 1;
        }
        if (r->ext_tcode != 2 || r->length != 8) {          /* only compare_swap */
            resp_error(o, r, POUT_RC_TYPE_ERROR);
            return 1;
        }
        uint32_t old = *reg;
        if (old == r->arg) {
            /* masters are read only (GUESS: the swap is ignored, the old value still returned);
             * plugs take the writable bits */
            if (pcr)
                *reg = (old & ~POUT_PCR_WRITABLE) | (r->datum & POUT_PCR_WRITABLE);
        }
        resp_head(o, r, POUT_TC_LOCK_RESP, POUT_RC_COMPLETE);
        o->hdr_len = 16;
        o->hdr[3] = (4u << 16) | r->ext_tcode;
        put32be(o->payload, old);
        o->plen = 4;
        if (reg == &c->opcr) {
            unsigned ch;
            int on = pout_csr_tx(c, &ch, NULL);
            if (on != c->tx_on || (on && ch != c->tx_channel))
                ev->plug_changed = 1;
            c->tx_on = on;
            c->tx_channel = ch;
        }
        return 1;
    }
    case POUT_TC_WRITE_QUADLET:
    case POUT_TC_WRITE_BLOCK: {
        unsigned rcode;
        if (r->offset_hi == 0xffff && r->offset == POUT_CSR_FCP_COMMAND &&
            r->length >= 1 && r->length <= POUT_FCP_MAX && r->data) {
            ev->fcp = r->data;
            ev->fcp_len = r->length;
            ev->fcp_src = r->src;
            rcode = POUT_RC_COMPLETE;
        } else if (reg_of(c, r, &pcr)) {
            rcode = POUT_RC_TYPE_ERROR;                       /* plugs are changed by locks */
        } else {
            rcode = POUT_RC_ADDRESS_ERROR;
        }
        /* our link acked the write "pending" if it owes the sender a response */
        if (r->ack != POUT_ACK_PENDING)
            return 0;
        resp_head(o, r, POUT_TC_WRITE_RESP, rcode);
        return 1;
    }
    default:
        resp_error(o, r, POUT_RC_TYPE_ERROR);
        return 1;
    }
}

int pout_csr_fcp_write(pout_resp_t *o, unsigned tl, uint16_t dest, const uint8_t *frame, unsigned len)
{
    if (!frame || len == 0 || len > POUT_RESP_PAYLOAD_MAX)
        return -1;
    memset(o, 0, sizeof(*o));
    o->hdr[1] = ((uint32_t)dest << 16) | 0xffffu;
    o->hdr[2] = POUT_CSR_FCP_RESPONSE;
    o->hdr_len = 16;
    if (len == 4) {
        o->hdr[0] = (tl << 10) | (POUT_TC_WRITE_QUADLET << 4);
        o->hdr[3] = get32le(frame);                  /* bytes in wire order, as p1394_avc() */
    } else {
        o->hdr[0] = (tl << 10) | (POUT_TC_WRITE_BLOCK << 4);
        o->hdr[3] = len << 16;
        memcpy(o->payload, frame, len);
        o->plen = len;
    }
    return 0;
}

void pout_csr_config_rom(uint32_t rom[POUT_CSR_ROM_WORDS], uint32_t guid_hi, uint32_t guid_lo)
{
    p1394_config_rom_build(rom, guid_hi, guid_lo, 1);
}
