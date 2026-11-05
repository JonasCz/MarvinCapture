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
 * Hold-back FIFO: the units (DV frames, HDV pictures, PAT/PMT) that have
 * arrived but are not written yet, so a cut that is only confirmed later (a
 * scene break or a format change, pin_fmtwatch.h, that has to persist for a
 * while before it counts) can still be back-dated to the unit where it really
 * began.
 *
 * The caller decides how much to hold, after each unit:
 *   - pin_holdback_keep_last(n): the last n units (a fixed window, as the DV
 *     scene split uses: its detector confirms within a known number of frames);
 *   - pin_holdback_release_before(index): everything older than the earliest
 *     unit a pending cut may still land on (the HDV scene split: hold from the
 *     detector's pending run, nothing while none is open). Several detectors
 *     pending at once pass the smallest of their start indexes.
 * Released units go to commit() in stream order (the session's commit is the
 * split lookahead, pin_split.h, then the file).
 *
 * A cut (pin_holdback_cut) lands on a unit flagged PIN_HOLD_CUT_OK, where a
 * file may start: every DV frame; an HDV picture that opens a GOP (it carries
 * the GOP header and, on HDV, the sequence header in front of it), since a
 * transport stream that starts elsewhere does not decode from its start.
 *
 * max_bytes bounds the memory: beyond it the oldest units are committed even
 * if a pending cut would still need them (it then lands on the oldest
 * CUT_OK unit still held). The module is pure (callbacks only).
 */

#ifndef PIN_HOLDBACK_H
#define PIN_HOLDBACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PIN_HOLD_CUT_OK 1u /* a new file may start with this unit */

/* 32 MiB: about 10 s of HDV (25 Mbit/s) or DV (3.6 MB/s), far more than a
 * pending scene cut or format change holds (DV 6 frames, HDV 2-3 GOPs: about
 * 1 s). */
#define PIN_HOLD_MAX_BYTES (32u * 1024 * 1024)

typedef struct {
    uint8_t *data;
    size_t len;
    long frame_index; /* the frame/picture it belongs to; -1 = carries no time (PAT/PMT) */
    unsigned flags;
} pin_hold_item_t;

typedef struct {
    /* writes one unit on (in stream order); frame_index as it was pushed */
    void (*commit)(void *user, const uint8_t *data, size_t len, long frame_index);
    void *user;
    size_t max_bytes;

    pin_hold_item_t *items; /* ring, oldest at head */
    size_t head, count, cap;
    size_t bytes;           /* data held */
} pin_holdback_t;

/* max_bytes = PIN_HOLD_MAX_BYTES. A zeroed struct is "not initialised". */
void pin_holdback_init(pin_holdback_t *q, void (*commit)(void *, const uint8_t *, size_t, long),
                       void *user);

/* Holds a copy of one unit (after the ones held). Out of memory: everything
 * held and then this unit are committed at once (no loss, no reorder; only the
 * back-dating is lost). */
void pin_holdback_push(pin_holdback_t *q, const uint8_t *data, size_t len, long frame_index,
                       unsigned flags);

/* Commits the oldest units until at most n are held. */
void pin_holdback_keep_last(pin_holdback_t *q, size_t n);

/* Commits the units from the head whose frame_index < frame_index (stops at
 * the first one that is not, so the order stays). */
void pin_holdback_release_before(pin_holdback_t *q, long frame_index);

/* A cut confirmed at frame_index: the units before it are committed, then
 * at_cut(user, index) runs (start the new segment there), then the units from
 * the cut on are committed; nothing stays held. The cut lands on the first
 * PIN_HOLD_CUT_OK unit with frame_index >= frame_index, together with the
 * units of the same frame pushed before it; index is that frame. If no held
 * unit qualifies (the cut unit was already released), it lands on the first
 * unit with frame_index >= frame_index, else after everything held. */
void pin_holdback_cut(pin_holdback_t *q, long frame_index, void (*at_cut)(void *user, long index),
                      void *user);

/* Commits everything held (end of a file, pass or capture). */
void pin_holdback_flush(pin_holdback_t *q);

/* Drops everything held without committing it and frees the memory (teardown). */
void pin_holdback_free(pin_holdback_t *q);

#ifdef __cplusplus
}
#endif

#endif /* PIN_HOLDBACK_H */
