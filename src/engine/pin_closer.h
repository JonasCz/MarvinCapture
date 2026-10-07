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
 * pin_closer -- finishes output files on a thread of its own.
 *
 * Closing a file drains its writer queue and finalises the container; for
 * HDV to MOV / MKV that is a remux of the whole temp .ts, minutes for a long
 * scene. Done on the USB read thread under the session lock (as it was), a
 * scene split lost the stream for that long and froze status calls. Here a
 * file is handed over (its writer and sink) and the capture goes on into the
 * next file at once. Files close one at a time, in the order handed over.
 *
 * The closer thread never takes the session lock, so the session may wait
 * for it (pin_closer_wait()) while holding that lock.
 */
#ifndef PIN_CLOSER_H
#define PIN_CLOSER_H

#include "../api/pin_api.h"
#include "../sinks/pin_sink.h"
#include "../sinks/pin_writer.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pin_closer pin_closer_t;

/* One file, closed. */
typedef struct {
    pin_status_t status;     /* the sink's close(), or PIN_ERR_IO if a write failed while draining */
    uint64_t units;          /* video units in the file (0: it was never created) */
    int announced;           /* as handed over: PIN_EVT_FILE_OPENED was sent for it */
    const char *path;
} pin_closer_done_t;

/* Called on the closer thread, without its lock, after each file. */
typedef void (*pin_closer_done_fn)(void *user, const pin_closer_done_t *d);

pin_closer_t *pin_closer_create(pin_closer_done_fn done, void *user);

/* Hands over a file: stops `writer` (it delivers what is queued, to `sink`
 * only: its consumer must not look the sink up anywhere else), takes the
 * sink's final counts and closes it. bytes / units: what the caller has
 * already counted for it; the rest is added to pin_closer_take()'s totals.
 * reserve: disk space the close still needs (the remux output); counted in
 * pin_closer_pending_bytes() until the file is closed. If the job cannot be
 * queued, the file is closed here and now. */
void pin_closer_submit(pin_closer_t *c, pin_writer_t *writer, pin_sink_t *sink, const char *path,
                       int announced, uint64_t bytes, uint64_t units, uint64_t reserve);

/* Returns once every file handed over is closed. */
void pin_closer_wait(pin_closer_t *c);

/* Bytes and video units the closed files had beyond what submit() was told,
 * since the last call (then zeroed). */
void pin_closer_take(pin_closer_t *c, int64_t *bytes, int64_t *units);

/* Disk space the files not yet closed still need. */
uint64_t pin_closer_pending_bytes(pin_closer_t *c);

/* Closes what is still queued and ends the thread. */
void pin_closer_destroy(pin_closer_t *c);

#ifdef __cplusplus
}
#endif

#endif
