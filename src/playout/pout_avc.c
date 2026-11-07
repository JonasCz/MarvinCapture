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

/* The AV/C target. See pout_avc.h; frame layouts follow src/engine/pin_deck.c. */

#include "pout_avc.h"

#include <string.h>

/* ctype of a command frame */
enum { CT_CONTROL = 0, CT_STATUS = 1, CT_SPECIFIC_INQUIRY = 2, CT_NOTIFY = 3, CT_GENERAL_INQUIRY = 4 };

#define SUBUNIT_UNIT 0xff
#define SUBUNIT_VCR  0x20       /* tape recorder/player (type 4), id 0: what Canon and the DV camcorder answer */

/* Unit opcodes */
#define OP_UNIT_INFO      0x30
#define OP_SUBUNIT_INFO   0x31
#define OP_PLUG_INFO      0x02
#define OP_OUT_PLUG_FMT   0x18
#define OP_IN_PLUG_FMT    0x19
/* VCR opcodes */
#define OP_RECORD         0xc2
#define OP_PLAY           0xc3
#define OP_WIND           0xc4
#define OP_TRANSPORT      0xd0
#define OP_TIMECODE       0x51
#define OP_ATN            0x52
#define OP_RTC            0x57
#define OP_MEDIUM_INFO    0xda

/* PLAY / WIND modes, the bytes pin_deck_build_cmd() sends */
#define PLAY_FORWARD        0x75
#define PLAY_FORWARD_PAUSE  0x7d
#define PLAY_REVERSE_PAUSE  0x6d
#define WIND_STOP           0x60
#define WIND_REWIND         0x65
#define WIND_HIGH_SPEED_REW 0x45
#define WIND_FAST_FORWARD   0x75

void pout_avc_init(pout_avc_t *t, pout_avc_media_t media)
{
    memset(t, 0, sizeof(*t));
    t->media = media;
    t->company_id = 0x000000;
}

void pout_avc_set_media(pout_avc_t *t, pout_avc_media_t media)
{
    t->media = media;
}

static int is_inquiry(unsigned ctype)
{
    return ctype == CT_SPECIFIC_INQUIRY || ctype == CT_GENERAL_INQUIRY;
}

/* The response must be at least n bytes long (status answers fill in the
 * 0xff placeholders of the command, and some controllers send fewer). */
static int grow(uint8_t *resp, unsigned *rl, unsigned n, unsigned cap)
{
    if (n > cap)
        return 0;
    if (n > *rl) {
        memset(resp + *rl, 0xff, n - *rl);
        *rl = n;
    }
    return 1;
}

static uint8_t bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static int unbcd(uint8_t b, int max_tens, int *out)
{
    int hi = (b >> 4) & 0x7, lo = b & 0xf;
    if (lo > 9 || hi > max_tens)
        return 0;
    *out = hi * 10 + lo;
    return 1;
}

/* TIME CODE status: 0c 20 51 71 FF SS MM HH, BCD, the layout pin_deck_parse_timecode() reads
 * (bit 7 of the frame byte = drop frame). */
static unsigned timecode_answer(pout_deck_t *d, double now, uint8_t *resp, unsigned *rl, unsigned cap, int with_df)
{
    pin_tc_t tc;
    if (pout_deck_timecode(d, now, &tc) != 0)
        return POUT_AVC_REJECTED;   /* what the cameras say when they have no readable time code */
    if (!grow(resp, rl, 8, cap))
        return 0;
    resp[3] = 0x71;
    resp[4] = (uint8_t)(bcd(tc.f) | (with_df && tc.sep == ';' ? 0x80 : 0));
    resp[5] = bcd(tc.s);
    resp[6] = bcd(tc.m);
    resp[7] = bcd(tc.h);
    return POUT_AVC_IMPLEMENTED;
}

/* FMT/FDF of the plug signal format (OUTPUT/INPUT PLUG SIGNAL FORMAT operands 1..4 of the
 * frame: EOH|FMT, FDF0..2). DV: FMT 0x00, FDF0 bit 7 = 50 Hz (IEC 61883-2); the two FDF bytes
 * after it are unspecified for DV: 0xff is a guess. HDV: the CIP quadlet of docs/hdv.md,
 * a0 00 00 00 (FMT 0x20). */
static void plug_format(const pout_avc_t *t, uint8_t f[4])
{
    if (t->media == POUT_AVC_HDV) {
        f[0] = 0xa0; f[1] = f[2] = f[3] = 0x00;
    } else {
        f[0] = 0x80;
        f[1] = t->media == POUT_AVC_DV_625 ? 0x80 : 0x00;
        f[2] = f[3] = 0xff;
    }
}

static unsigned unit_cmd(pout_avc_t *t, unsigned ctype, uint8_t op, uint8_t *resp, unsigned *rl, unsigned cap)
{
    switch (op) {
    case OP_UNIT_INFO:
        if (is_inquiry(ctype)) return POUT_AVC_IMPLEMENTED;
        if (ctype != CT_STATUS || !grow(resp, rl, 8, cap)) return POUT_AVC_NOT_IMPLEMENTED;
        /* 0c ff 30 07 20 00 00 85 is the Canon's; unit type 4 (tape recorder) << 3 | unit 0 */
        resp[3] = 0x07;
        resp[4] = 0x20;
        resp[5] = (uint8_t)(t->company_id >> 16);
        resp[6] = (uint8_t)(t->company_id >> 8);
        resp[7] = (uint8_t)t->company_id;
        return POUT_AVC_IMPLEMENTED;
    case OP_SUBUNIT_INFO:
        if (is_inquiry(ctype)) return POUT_AVC_IMPLEMENTED;
        if (ctype != CT_STATUS || *rl < 4 || !grow(resp, rl, 8, cap)) return POUT_AVC_NOT_IMPLEMENTED;
        if (((resp[3] >> 4) & 7) != 0)
            return POUT_AVC_REJECTED;   /* page 0 holds all we have */
        /* the Canon lists the VCR and a panel (20 38 ff ff); we are the VCR only */
        resp[4] = SUBUNIT_VCR;
        resp[5] = resp[6] = resp[7] = 0xff;
        return POUT_AVC_IMPLEMENTED;
    case OP_PLUG_INFO:
        if (is_inquiry(ctype)) return POUT_AVC_IMPLEMENTED;
        if (ctype != CT_STATUS || *rl < 4 || !grow(resp, rl, 8, cap)) return POUT_AVC_NOT_IMPLEMENTED;
        if (resp[3] != 0x00)
            return POUT_AVC_REJECTED;   /* only the serial bus subfunction */
        /* one isochronous input plug (iPCR), one output plug (oPCR), no external plugs (guess:
         * a camcorder's DV jack counts as the serial bus pair) */
        resp[4] = 1; resp[5] = 1; resp[6] = 0; resp[7] = 0;
        return POUT_AVC_IMPLEMENTED;
    case OP_OUT_PLUG_FMT:
    case OP_IN_PLUG_FMT: {
        uint8_t f[4];
        plug_format(t, f);
        if (ctype == CT_GENERAL_INQUIRY) return POUT_AVC_IMPLEMENTED;
        if (*rl < 5)
            return POUT_AVC_REJECTED;
        if (resp[3] != 0x00)
            return POUT_AVC_REJECTED;   /* plug 0 only */
        if (ctype == CT_SPECIFIC_INQUIRY)
            return (*rl >= 6 && resp[4] == f[0] && resp[5] == f[1]) ? POUT_AVC_IMPLEMENTED : POUT_AVC_NOT_IMPLEMENTED;
        if (ctype == CT_STATUS) {
            if (!grow(resp, rl, 8, cap)) return POUT_AVC_NOT_IMPLEMENTED;
            memcpy(resp + 4, f, 4);
            return POUT_AVC_IMPLEMENTED;
        }
        return POUT_AVC_REJECTED;       /* control: the format is fixed by the tape */
    }
    default:
        return POUT_AVC_NOT_IMPLEMENTED;
    }
}

/* PLAY modes: 0 = not supported, 1 = forward play, 2 = pause */
static int play_mode(uint8_t m)
{
    if (m == PLAY_FORWARD)
        return 1;
    if (m == PLAY_FORWARD_PAUSE || m == PLAY_REVERSE_PAUSE)
        return 2;       /* a reverse pause is a pause: there is no reverse play to come from */
    /* Next frame (0x30), slow and fast forward (0x31..0x3f) are played at normal speed: a
     * receiver that asks for slow motion still gets a picture and TRANSPORT STATE says
     * PLAY forward. Reverse play and the reverse speeds (0x41..0x4f, 0x65) are not done. */
    if (m >= 0x30 && m <= 0x3f)
        return 1;
    return 0;
}

static unsigned vcr_cmd(pout_deck_t *d, double now, unsigned ctype, uint8_t op, uint8_t *resp, unsigned *rl,
                        unsigned cap)
{
    int inq = is_inquiry(ctype);
    switch (op) {
    case OP_TRANSPORT: {
        if (inq) return POUT_AVC_IMPLEMENTED;
        if (ctype != CT_STATUS || !grow(resp, rl, 4, cap)) return POUT_AVC_NOT_IMPLEMENTED;
        /* the response replaces opcode and operand with the transport state (docs/deck-control.md) */
        switch (pout_deck_state(d, now)) {
        case POUT_DECK_PLAY:  resp[2] = OP_PLAY; resp[3] = PLAY_FORWARD; break;
        case POUT_DECK_PAUSE: resp[2] = OP_PLAY; resp[3] = PLAY_FORWARD_PAUSE; break;
        case POUT_DECK_FF:    resp[2] = OP_WIND; resp[3] = WIND_FAST_FORWARD; break;
        case POUT_DECK_REW:   resp[2] = OP_WIND; resp[3] = WIND_REWIND; break;
        default:              resp[2] = OP_WIND; resp[3] = WIND_STOP; break;
        }
        return pout_deck_in_transition(d, now) ? POUT_AVC_IN_TRANSITION : POUT_AVC_IMPLEMENTED;
    }
    case OP_PLAY: {
        if (ctype == CT_GENERAL_INQUIRY) return POUT_AVC_IMPLEMENTED;
        if (*rl < 4) return POUT_AVC_REJECTED;
        int m = play_mode(resp[3]);
        if (inq) return m ? POUT_AVC_IMPLEMENTED : POUT_AVC_NOT_IMPLEMENTED;
        if (ctype == CT_STATUS) {
            pout_deck_mode_t s = pout_deck_state(d, now);
            if (s != POUT_DECK_PLAY && s != POUT_DECK_PAUSE) return POUT_AVC_REJECTED;
            resp[3] = s == POUT_DECK_PLAY ? PLAY_FORWARD : PLAY_FORWARD_PAUSE;
            return POUT_AVC_IMPLEMENTED;
        }
        if (!m) return POUT_AVC_NOT_IMPLEMENTED;
        if (m == 1) pout_deck_play(d, now); else pout_deck_pause(d, now);
        return POUT_AVC_ACCEPTED;
    }
    case OP_WIND: {
        if (ctype == CT_GENERAL_INQUIRY) return POUT_AVC_IMPLEMENTED;
        if (*rl < 4) return POUT_AVC_REJECTED;
        uint8_t m = resp[3];
        int known = m == WIND_STOP || m == WIND_REWIND || m == WIND_HIGH_SPEED_REW || m == WIND_FAST_FORWARD;
        if (inq) return known ? POUT_AVC_IMPLEMENTED : POUT_AVC_NOT_IMPLEMENTED;
        if (ctype == CT_STATUS) {
            pout_deck_mode_t s = pout_deck_state(d, now);
            if (s != POUT_DECK_STOP && s != POUT_DECK_FF && s != POUT_DECK_REW) return POUT_AVC_REJECTED;
            resp[3] = s == POUT_DECK_FF ? WIND_FAST_FORWARD : s == POUT_DECK_REW ? WIND_REWIND : WIND_STOP;
            return POUT_AVC_IMPLEMENTED;
        }
        if (!known) return POUT_AVC_NOT_IMPLEMENTED;
        if (m == WIND_STOP) pout_deck_wind_stop(d, now);
        else if (m == WIND_FAST_FORWARD) pout_deck_ff(d, now);
        else pout_deck_rew(d, now);     /* high speed rewind is the same here */
        return POUT_AVC_ACCEPTED;
    }
    case OP_TIMECODE:
        if (ctype == CT_GENERAL_INQUIRY) return POUT_AVC_IMPLEMENTED;
        if (*rl < 4) return POUT_AVC_REJECTED;
        if (ctype == CT_STATUS)
            return resp[3] == 0x71 ? timecode_answer(d, now, resp, rl, cap, 1) : POUT_AVC_NOT_IMPLEMENTED;
        if (inq) return resp[3] == 0x20 || resp[3] == 0x71 ? POUT_AVC_IMPLEMENTED : POUT_AVC_NOT_IMPLEMENTED;
        if (resp[3] != 0x20) return POUT_AVC_NOT_IMPLEMENTED;
        {
            /* search: 00 20 51 20 FF SS MM HH. ACCEPTED means received (as everywhere in AV/C);
             * TRANSPORT STATE then shows the search as FF or REW, IN_TRANSITION, and ends PAUSED. */
            pin_tc_t tc;
            int f, s, m, h;
            if (*rl < 8 || !unbcd(resp[4] & 0x7f, 3, &f) || !unbcd(resp[5], 5, &s) || !unbcd(resp[6], 5, &m) ||
                !unbcd(resp[7], 2, &h) || h > 23)
                return POUT_AVC_REJECTED;
            tc.h = h; tc.m = m; tc.s = s; tc.f = f;
            tc.sep = (resp[4] & 0x80) ? ';' : ':';
            return pout_deck_search_tc(d, now, &tc) == 0 ? POUT_AVC_ACCEPTED : POUT_AVC_REJECTED;
        }
    case OP_RTC:
        /* RELATIVE TIME COUNTER mirrors the time code (guess: a real deck counts from where its tape
         * was reset; this one has no reset) */
        if (ctype == CT_GENERAL_INQUIRY || (inq && *rl >= 4 && resp[3] == 0x71)) return POUT_AVC_IMPLEMENTED;
        if (ctype == CT_STATUS && *rl >= 4 && resp[3] == 0x71)
            return timecode_answer(d, now, resp, rl, cap, 0);
        return POUT_AVC_NOT_IMPLEMENTED;
    case OP_ATN: {
        if (ctype == CT_GENERAL_INQUIRY || (inq && *rl >= 4 && resp[3] == 0x71)) return POUT_AVC_IMPLEMENTED;
        if (ctype != CT_STATUS || *rl < 4 || resp[3] != 0x71 || !grow(resp, rl, 8, cap))
            return POUT_AVC_NOT_IMPLEMENTED;     /* control: NOT_IMPLEMENTED on both real decks */
        /* Absolute track number: tracks per frame x frame, 31 bits big endian, bit 31 = blank flag
         * (guess at the layout; the Canon only answered, we never saw the bytes). */
        uint32_t atn = (uint32_t)pout_deck_position(d, now) * (d->ops.fps > 26.0 ? 10u : 12u);
        resp[4] = (uint8_t)((atn >> 24) & 0x7f);
        resp[5] = (uint8_t)(atn >> 16);
        resp[6] = (uint8_t)(atn >> 8);
        resp[7] = (uint8_t)atn;
        return POUT_AVC_IMPLEMENTED;
    }
    case OP_MEDIUM_INFO:
        if (inq) return POUT_AVC_IMPLEMENTED;
        if (ctype != CT_STATUS || !grow(resp, rl, 5, cap)) return POUT_AVC_NOT_IMPLEMENTED;
        /* guess: cassette present, write protected / medium type unknown; the layout is not known
         * to us, only that the controller asked (Windows did) and a tape is in */
        resp[3] = 0x31;
        resp[4] = 0x3f;
        return POUT_AVC_IMPLEMENTED;
    case OP_RECORD:
        return inq ? POUT_AVC_NOT_IMPLEMENTED : POUT_AVC_REJECTED;      /* a player: it cannot record */
    default:
        return POUT_AVC_NOT_IMPLEMENTED;    /* includes vendor dependent (0x00) */
    }
}

int pout_avc_handle(pout_avc_t *t, pout_deck_t *d, double now, const uint8_t *cmd, unsigned len, uint8_t *resp,
                    unsigned cap)
{
    if (!cmd || !resp || len < 3 || len > cap || cmd[0] > CT_GENERAL_INQUIRY)
        return 0;       /* not a command frame (a response has ctype >= 8) */
    memcpy(resp, cmd, len);
    unsigned rl = len, code = POUT_AVC_NOT_IMPLEMENTED;
    if (cmd[0] == CT_NOTIFY)
        code = POUT_AVC_NOT_IMPLEMENTED;        /* no notifications, not even TRANSPORT STATE */
    else if (cmd[1] == SUBUNIT_UNIT)
        code = unit_cmd(t, cmd[0], cmd[2], resp, &rl, cap);
    else if (cmd[1] == SUBUNIT_VCR)
        code = vcr_cmd(d, now, cmd[0], cmd[2], resp, &rl, cap);
    if (code == 0)
        return 0;       /* resp too small for the answer */
    resp[0] = (uint8_t)code;
    return (int)rl;
}
