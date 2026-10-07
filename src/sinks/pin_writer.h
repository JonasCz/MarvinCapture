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
 * Portable producer/consumer queue for the sinks layer, generalising the
 * SINK_BYTES ring buffer that used to live in src/cli/pincli.c (there:
 * sink_t). The producer is always the USB thread (or, for analog, the
 * capture-assembly thread) and must never block on disk I/O -- a filesystem
 * stall must never make the FPGA's receive FIFO overrun. So pin_writer_push()
 * either copies the caller's bytes in and returns immediately, or -- if the
 * queue is already full -- drops them and counts an overflow. It never waits.
 *
 * Unlike pincli's raw byte ring, this queue is unit-aware: a push carries a
 * `kind` and monotonic `index` alongside the bytes, so the consumer thread
 * (which drives a pin_sink_t) always sees whole frames/pictures/audio
 * blocks, never a partial one split across the ring's wraparound. Each
 * unit's bytes are still stored in one shared circular byte buffer (a
 * separate malloc per unit would be needless allocator traffic at capture
 * rates); a small ring of per-unit headers records where each one starts.
 */

#ifndef PIN_WRITER_H
#define PIN_WRITER_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PIN_UNIT_VIDEO = 0,
    PIN_UNIT_AUDIO,
    PIN_UNIT_RAW,      /* sink_raw / sink_rewrap: one DV frame or one HDV picture */
} pin_unit_kind_t;

#define PIN_WRITER_DEFAULT_CAPACITY (64u << 20)
#define PIN_WRITER_PIPE_CAPACITY (256u << 20)   /* queue in front of a pipe (stdout) */
#define PIN_WRITER_ANALOG_CAPACITY (512u << 20) /* analog: raw frames at ~21 MB/s, so ~25 s;
                                                   64 MiB would ride out only ~3 s of disk stall */
#define PIN_WRITER_MAX_QUEUED_UNITS 4096

typedef struct pin_writer pin_writer_t;

/* Called on the consumer thread, once per queued unit, in push order.
 * Returning non-zero marks the writer failed; no more units are delivered
 * after that (pin_writer_stop() still joins the thread). */
typedef int (*pin_writer_consume_fn)(pin_unit_kind_t kind, uint64_t index,
                                     const uint8_t *data, size_t len, void *user);

typedef struct {
    uint64_t backlog_bytes;     /* queued now */
    uint64_t backlog_high_water;
    uint64_t overflow_count;    /* units dropped because the queue was full */
    uint64_t units_pushed;
    uint64_t units_consumed;
    uint64_t bytes_pushed;
    int failed;                 /* the consumer returned an error: nothing more reaches the file */
    int overflowed;             /* PIN_WRITER_OVERFLOW_FATAL: the queue was full once; nothing was
                                   queued since (a hole in the stream must not be papered over) */
} pin_writer_stats_t;

/* pin_writer_start_ex() flags. */
#define PIN_WRITER_OVERFLOW_FATAL 1u   /* a full queue is an error, not a counted drop: a gap
                                          would corrupt a pipe's stream, and in a file it would
                                          shift the audio against the video for the rest of it
                                          (the sinks count frames, not time). The units queued
                                          before the overflow are still delivered; later pushes
                                          are refused. */

/* capacity_bytes == 0 means PIN_WRITER_DEFAULT_CAPACITY. Starts the consumer
 * thread; returns NULL on allocation/thread-creation failure. */
pin_writer_t *pin_writer_start(size_t capacity_bytes, pin_writer_consume_fn consume, void *user);
pin_writer_t *pin_writer_start_ex(size_t capacity_bytes, pin_writer_consume_fn consume, void *user,
                                  unsigned flags);

/* Never blocks. Copies data in and wakes the consumer, or -- if there is not
 * room for it (byte capacity or PIN_WRITER_MAX_QUEUED_UNITS units already
 * queued) -- drops the whole unit and counts one overflow. Returns 1 if
 * queued, 0 if dropped. Safe to call after the consumer has failed (it will
 * just keep counting overflow, cheaply) but not after pin_writer_stop(). */
int pin_writer_push(pin_writer_t *w, pin_unit_kind_t kind, uint64_t index,
                    const uint8_t *data, size_t len);

/* Snapshot, cheap, any thread. */
void pin_writer_get_stats(pin_writer_t *w, pin_writer_stats_t *out);

/* Lets the consumer drain everything already queued, joins its thread, frees
 * *w. Returns the consume callback's failure status: 0 if every unit that
 * was queued was consumed without the callback ever returning non-zero, -1
 * otherwise. Safe with NULL. */
int pin_writer_stop(pin_writer_t *w);

/* Discards what is still queued and refuses further pushes; the consumer thread
 * ends after the unit it is in. For a pipe whose reader stalled: draining the
 * queue into it would block the capture's end for as long as the reader sleeps.
 * pin_writer_stop() still has to be called. Safe with NULL. */
void pin_writer_abort(pin_writer_t *w);

#ifdef __cplusplus
}
#endif

#endif /* PIN_WRITER_H */
