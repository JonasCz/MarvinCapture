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

#include "pin_deck.h"

#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

/* Same table pindeck.c has always used. */
unsigned pin_deck_build_cmd(pin_deck_cmd_t cmd, uint8_t *out)
{
    static const struct { uint8_t len, b[4]; } table[] = {
        [PIN_DECK_CMD_PLAY]  = { 4, { 0x00, 0x20, 0xc3, 0x75 } }, /* PLAY forward */
        [PIN_DECK_CMD_PAUSE] = { 4, { 0x00, 0x20, 0xc3, 0x7d } }, /* PLAY forward-pause */
        [PIN_DECK_CMD_STOP]  = { 4, { 0x00, 0x20, 0xc4, 0x60 } }, /* WIND stop */
        [PIN_DECK_CMD_FF]    = { 4, { 0x00, 0x20, 0xc4, 0x75 } }, /* WIND fast-forward */
        [PIN_DECK_CMD_REW]   = { 4, { 0x00, 0x20, 0xc4, 0x65 } }, /* WIND rewind */
    };
    if ((unsigned)cmd >= sizeof(table) / sizeof(table[0]))
        return 0;
    memcpy(out, table[cmd].b, table[cmd].len);
    return table[cmd].len;
}

static const uint8_t CMD_STATE[4]    = { 0x01, 0x20, 0xd0, 0x7f };
static const uint8_t CMD_TIMECODE[8] = { 0x01, 0x20, 0x51, 0x71, 0xff, 0xff, 0xff, 0xff };

pin_deck_state_t pin_deck_state_from_avc(uint8_t op, uint8_t mode)
{
    if (op == 0xc3) {
        switch (mode) {
        case 0x75: case 0x6d: return PIN_DECK_PLAYING;
        case 0x7d: case 0x65: return PIN_DECK_PAUSED;
        default: return PIN_DECK_PLAYING;
        }
    }
    if (op == 0xc4) {
        switch (mode) {
        case 0x60: return PIN_DECK_STOPPED;
        case 0x65: case 0x45: return PIN_DECK_REWINDING;
        case 0x75: return PIN_DECK_FAST_FORWARD;
        default: return PIN_DECK_UNKNOWN;
        }
    }
    if (op == 0xc2)
        return PIN_DECK_RECORDING;
    return PIN_DECK_UNKNOWN;
}

/* NOT_IMPLEMENTED (0x08) on a transport opcode means "busy, mechanism
 * changing mode" per pindeck.c's transport-command comment; retried 5x at
 * 0.7 s there and here. */
static int is_transport_cmd(const uint8_t *cmd, unsigned len)
{
    return len >= 3 && cmd[1] == 0x20 && (cmd[2] == 0xc3 || cmd[2] == 0xc4 || cmd[2] == 0xd0);
}

pin_status_t pin_deck_cmd_sync(pinnacle_1394_t *link, uint16_t node, pin_deck_cmd_t cmd,
                                uint8_t *resp, unsigned resp_max, int *resp_len)
{
    if (!node)
        return PIN_ERR_NO_CAMERA;
    uint8_t cmdbuf[8];
    unsigned clen = pin_deck_build_cmd(cmd, cmdbuf);
    if (!clen)
        return PIN_ERR_ARG;

    uint8_t local_resp[512];
    uint8_t *r = resp ? resp : local_resp;
    unsigned rmax = resp ? resp_max : sizeof(local_resp);

    int rl = -1;
    for (int attempt = 0; attempt < 5; attempt++) {
        rl = p1394_avc(link, node, cmdbuf, clen, r, rmax, 3000);
        if (rl < 0)
            return PIN_ERR_DECK;
        if (!(is_transport_cmd(cmdbuf, clen) && r[0] == 0x08))
            break;
        /* busy: sleep handled by caller loop cadence in the sync CLI path;
         * here we just retry immediately, p1394_avc's own INTERIM handling
         * already waits out the slow case. A short sleep mirrors pindeck.c
         * (700 ms) so we don't hammer a camera that is mid-transition. */
#if defined(_WIN32)
        Sleep(700);
#else
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 700000000L };
        nanosleep(&ts, NULL);
#endif
    }
    if (resp_len)
        *resp_len = rl;
    if (rl < 4 || r[0] == 0x0a)   /* REJECTED */
        return PIN_ERR_DECK;
    return PIN_OK;
}

pin_status_t pin_deck_query_state_sync(pinnacle_1394_t *link, uint16_t node,
                                        pin_deck_state_t *out)
{
    *out = PIN_DECK_UNKNOWN;
    if (!node)
        return PIN_ERR_NO_CAMERA;
    uint8_t resp[512];
    int rl = p1394_avc(link, node, CMD_STATE, sizeof(CMD_STATE), resp, sizeof(resp), 3000);
    if (rl < 4 || resp[1] != 0x20)
        return PIN_ERR_DECK;
    *out = pin_deck_state_from_avc(resp[2], resp[3]);
    return PIN_OK;
}

int pin_deck_parse_timecode(const uint8_t *resp, int rl, pin_deck_timecode_t *out)
{
    memset(out, 0, sizeof(*out));
    /* STABLE (0c) or IN_TRANSITION (0b: the tape is moving) answers carry data */
    if (rl < 8 || resp[1] != 0x20 || resp[2] != 0x51 || (resp[0] != 0x0c && resp[0] != 0x0b))
        return -1;
    /* frame/second/minute/hour, each one BCD byte, LSB-first in the reply
     * (see pindeck.c's "%02x:%02x:%02x:%02x", resp[7],resp[6],resp[5],resp[4]) */
    uint8_t f = resp[4], s = resp[5], m = resp[6], h = resp[7];
    /* 0xff / non-BCD = "no time code" (blank tape, or not readable while winding) */
    if (((f & 0xf) > 9) || ((s & 0xf) > 9) || ((m & 0xf) > 9) || ((h & 0xf) > 9) ||
        ((f >> 4) & 0x7) > 5 || ((s >> 4) & 0x7) > 5 || ((m >> 4) & 0x7) > 5 || ((h >> 4) & 0xf) > 2)
        return -1;
    out->drop_frame = (f & 0x80) != 0;
    out->frame  = ((f >> 4) & 0x3) * 10 + (f & 0xf);
    out->second = ((s >> 4) & 0x7) * 10 + (s & 0xf);
    out->minute = ((m >> 4) & 0x7) * 10 + (m & 0xf);
    out->hour   = ((h >> 4) & 0x3) * 10 + (h & 0xf);
    out->valid = 1;
    return 0;
}

pin_status_t pin_deck_query_timecode_sync(pinnacle_1394_t *link, uint16_t node,
                                           pin_deck_timecode_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!node)
        return PIN_ERR_NO_CAMERA;
    uint8_t resp[512];
    int rl = p1394_avc(link, node, CMD_TIMECODE, sizeof(CMD_TIMECODE), resp, sizeof(resp), 3000);
    return pin_deck_parse_timecode(resp, rl, out) == 0 ? PIN_OK : PIN_ERR_DECK;
}

/* --- async ---------------------------------------------------------------- */

void pin_deck_async_start(pin_deck_async_t *a, pinnacle_1394_t *link, uint16_t node,
                           pin_deck_cmd_t cmd, double now_s)
{
    memset(a, 0, sizeof(*a));
    a->link = link;
    a->node = node;
    a->cmd_len = pin_deck_build_cmd(cmd, a->cmd);
    a->attempt = 0;
    a->next_send_at = now_s;
    a->status = PIN_DECK_ASYNC_RUNNING;
    if (!node || !a->cmd_len)
        a->status = PIN_DECK_ASYNC_FAILED;
}

void pin_deck_async_start_query(pin_deck_async_t *a, pinnacle_1394_t *link, uint16_t node,
                                 pin_deck_query_t q, double now_s)
{
    memset(a, 0, sizeof(*a));
    a->link = link;
    a->node = node;
    if (q == PIN_DECK_QUERY_TIMECODE) {
        memcpy(a->cmd, CMD_TIMECODE, sizeof(CMD_TIMECODE));
        a->cmd_len = sizeof(CMD_TIMECODE);
    } else {
        memcpy(a->cmd, CMD_STATE, sizeof(CMD_STATE));
        a->cmd_len = sizeof(CMD_STATE);
    }
    a->next_send_at = now_s;
    a->status = node ? PIN_DECK_ASYNC_RUNNING : PIN_DECK_ASYNC_FAILED;
}

pin_deck_async_status_t pin_deck_async_poll(pin_deck_async_t *a, double now_s)
{
    if (a->status != PIN_DECK_ASYNC_RUNNING)
        return a->status;

    /* Nothing in flight yet, or time to (re)send. p1394_avc_begin() with
     * another command already pending would silently do nothing useful, so
     * only call it when p1394_avc_poll() isn't already tracking one -- the
     * l->avc_pending flag is link-private, so this state machine tracks its
     * own "have I sent yet for this attempt" via next_send_at <= now_s. */
    if (now_s >= a->next_send_at) {
        if (p1394_avc_begin(a->link, a->node, a->cmd, a->cmd_len) != 0) {
            a->status = PIN_DECK_ASYNC_FAILED;
            return a->status;
        }
        a->next_send_at = now_s + 3600.0; /* don't re-send until poll tells us to */
    }

    int rl = p1394_avc_poll(a->link, a->resp, sizeof(a->resp));
    if (rl == 0)
        return a->status; /* still waiting */
    if (rl < 0) {
        a->status = PIN_DECK_ASYNC_FAILED;
        return a->status;
    }
    a->resp_len = rl;
    if (is_transport_cmd(a->cmd, a->cmd_len) && a->resp[0] == 0x08 && a->attempt < 5) {
        /* busy: retry at +0.7s */
        a->attempt++;
        a->next_send_at = now_s + 0.7;
        return a->status; /* still RUNNING */
    }
    a->status = (rl >= 4 && a->resp[0] != 0x0a) ? PIN_DECK_ASYNC_DONE : PIN_DECK_ASYNC_FAILED;
    return a->status;
}
