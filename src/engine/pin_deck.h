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
 * AV/C deck control, factored out of src/cli/pindeck.c so pin_session.c can
 * drive the same commands. Two flavours, matching the plan's "Deck control
 * during capture":
 *
 *   - pin_deck_cmd_sync() uses the blocking pinnacle_1394.h calls directly.
 *     Only safe when nothing else is reading EP 0x84 -- i.e. while READY,
 *     not streaming.
 *   - pin_deck_async_t is a small state machine built on
 *     p1394_avc_begin()/p1394_avc_poll() (pinnacle_1394.h) so a command can
 *     be driven from pinnacle_stream_read_loop_ex()'s tick hook while a
 *     capture is running: begin() fires the command, poll() is called every
 *     tick and returns once it's done (retrying a NOT_IMPLEMENTED/busy
 *     response up to 5 times at 0.7 s, the same rule pindeck.c has always
 *     used for a camera whose mechanism is still changing mode).
 *
 * docs/deck-control.md has the AV/C opcodes this wraps.
 */

#ifndef PIN_DECK_H
#define PIN_DECK_H

#include "../api/pin_api.h"
#include "../core/pinnacle_1394.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int hour, minute, second, frame;
    int drop_frame;
    int valid;
} pin_deck_timecode_t;

/* Fills *out from an AV/C response's transport opcode/mode byte pair
 * (resp[2], resp[3] after a TRANSPORT STATE or PLAY/WIND command/response;
 * see pindeck.c's transport_name() for the table this mirrors). */
pin_deck_state_t pin_deck_state_from_avc(uint8_t transport_op, uint8_t mode);

/* Builds the raw AV/C command bytes for cmd into out (caller-sized, >= 8
 * bytes). Returns the length. */
unsigned pin_deck_build_cmd(pin_deck_cmd_t cmd, uint8_t *out);

/* Blocking, retries NOT_IMPLEMENTED up to 5x/0.7s like pindeck.c. Only call
 * while nothing else owns EP 0x84 (i.e. not while pinnacle_stream_read_loop*
 * is running). Returns PIN_OK with *resp_len/resp filled (resp may be NULL
 * to discard), PIN_ERR_DECK if the camera rejected it or never answered,
 * PIN_ERR_NO_CAMERA if node is 0. */
pin_status_t pin_deck_cmd_sync(pinnacle_1394_t *link, uint16_t node, pin_deck_cmd_t cmd,
                                uint8_t *resp, unsigned resp_max, int *resp_len);

/* TRANSPORT STATE / timecode queries, same retry rule, blocking. */
pin_status_t pin_deck_query_state_sync(pinnacle_1394_t *link, uint16_t node,
                                        pin_deck_state_t *out);
pin_status_t pin_deck_query_timecode_sync(pinnacle_1394_t *link, uint16_t node,
                                           pin_deck_timecode_t *out);

/* Parses a TIME CODE status response (0c/0b 20 51 71 FF SS MM HH, BCD).
 * Returns 0 with *out filled (valid = 1), -1 if it is not a usable answer
 * (wrong response, or the deck has no readable time code right now). */
int pin_deck_parse_timecode(const uint8_t *resp, int resp_len, pin_deck_timecode_t *out);

/* --- async, driven from the stream loop's tick hook ---------------------- */

typedef enum {
    PIN_DECK_ASYNC_IDLE = 0,
    PIN_DECK_ASYNC_RUNNING,
    PIN_DECK_ASYNC_DONE,
    PIN_DECK_ASYNC_FAILED,
} pin_deck_async_status_t;

typedef struct {
    pinnacle_1394_t *link;
    uint16_t node;
    uint8_t cmd[8];
    unsigned cmd_len;
    int attempt;
    double next_send_at;      /* monotonic seconds; 0 = send now */
    pin_deck_async_status_t status;
    uint8_t resp[512];
    int resp_len;
} pin_deck_async_t;

typedef enum { PIN_DECK_QUERY_STATE = 0, PIN_DECK_QUERY_TIMECODE } pin_deck_query_t;

/* Same, for a status query (TRANSPORT STATE or TIME CODE) instead of a command. */
void pin_deck_async_start_query(pin_deck_async_t *a, pinnacle_1394_t *link, uint16_t node,
                                 pin_deck_query_t q, double now_s);

/* now_s: caller's monotonic clock (seconds), so this file needs no time.h
 * porting concerns of its own. */
void pin_deck_async_start(pin_deck_async_t *a, pinnacle_1394_t *link, uint16_t node,
                           pin_deck_cmd_t cmd, double now_s);

/* Call every tick (after the stream loop has fed new EP 0x84 bytes into
 * link via p1394_parse_ep84()). Advances the retry state machine; returns
 * the current status. Once DONE or FAILED, a->resp/resp_len holds the last
 * response (if any) and the caller should not poll again until the next
 * pin_deck_async_start(). */
pin_deck_async_status_t pin_deck_async_poll(pin_deck_async_t *a, double now_s);

#ifdef __cplusplus
}
#endif

#endif /* PIN_DECK_H */
