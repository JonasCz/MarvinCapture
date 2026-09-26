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
 * DV streaming control and raw capture for the Pinnacle 500-USB.
 *
 * pinnacle_stream_read_loop() delivers raw bytes off EP 0x88 to a callback;
 * it does not know about DV framing. DIF reassembly lives in
 * dv_reassembler.h so this module stays reusable for the analog path and
 * for a future GUI preview that wants the same raw feed.
 */

#ifndef PINNACLE_STREAM_H
#define PINNACLE_STREAM_H

#include "pinnacle_device.h"
#include "pinnacle_1394.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

pinnacle_status_t pinnacle_stream_start(pinnacle_device_t *dev);
pinnacle_status_t pinnacle_stream_stop(pinnacle_device_t *dev);

/* Called with each chunk read from EP 0x88, in order. Return 0 to keep
 * reading, non-zero to stop the loop. */
typedef int (*pinnacle_data_cb)(const uint8_t *data, size_t len, void *user);

/* Reads from EP 0x88 until the callback returns non-zero or *stop_flag
 * becomes non-zero (checked between transfers; safe to set from a signal
 * handler as a sig_atomic_t). Returns PINNACLE_OK on a clean stop. */
pinnacle_status_t pinnacle_stream_read_loop(pinnacle_device_t *dev,
                                             pinnacle_data_cb cb, void *user,
                                             volatile int *stop_flag);

/* Called once per event-loop iteration (roughly every 50 ms, more often
 * when data is flowing), after any new EP 0x84 bytes for this pass have
 * already been fed to link (if non-NULL). Used by engine/pin_session.c to
 * drive pin_deck's async AV/C (p1394_avc_poll) and the ~1 Hz transport-state
 * poll while a DV/HDV capture is running -- see docs/deck-control.md and
 * the plan's "Deck control during capture". */
typedef void (*pinnacle_stream_tick_fn)(void *user);

/* Same as pinnacle_stream_read_loop(), plus:
 *   - if link is non-NULL, every chunk read off EP 0x84 (the FCP-response
 *     drain already needed to keep the command channel alive while
 *     streaming, see pinnacle_stream.c's back-pressure notes) is also
 *     handed to p1394_parse_ep84(link, ...), so p1394_avc_begin/poll() work
 *     while this loop is running;
 *   - if tick is non-NULL, it is called once per iteration with tick_user.
 * pinnacle_stream_read_loop() is this with link = tick = NULL: unchanged
 * behaviour for every existing caller. */
pinnacle_status_t pinnacle_stream_read_loop_ex(pinnacle_device_t *dev,
                                                pinnacle_data_cb cb, void *user,
                                                volatile int *stop_flag,
                                                pinnacle_1394_t *link,
                                                pinnacle_stream_tick_fn tick, void *tick_user);

#ifdef __cplusplus
}
#endif

#endif /* PINNACLE_STREAM_H */
