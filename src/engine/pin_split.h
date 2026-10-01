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
 * Split lookahead: a content-triggered file split (scene cut) is only
 * committed once the new segment is "real", i.e. has reached both
 * PIN_SPLIT_MIN_BYTES and PIN_SPLIT_MIN_SECONDS. Until then the new
 * segment's units are held in memory; if the capture ends, or another
 * split triggers, before that, the held units are written to the CURRENT
 * (previous) file and the pending split is dropped. This keeps a few odd
 * frames with a weird timecode at the end of a capture/scene/tape from
 * producing a tiny useless file.
 *
 * Only content triggers go through here. Size-based splits (none exist
 * today; they would be hard limits like FAT32's 4 GiB) must NOT.
 *
 * The module is pure (callbacks only), so it is unit tested without a
 * session.
 */

#ifndef PIN_SPLIT_H
#define PIN_SPLIT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PIN_SPLIT_MIN_BYTES (1024 * 1024)
#define PIN_SPLIT_MIN_SECONDS 1.0

typedef struct {
    uint8_t *data;
    size_t len;
} pin_split_item_t;

typedef struct {
    /* write one unit to the file that is currently open */
    void (*commit)(void *user, const uint8_t *data, size_t len);
    /* close the current file and open the next one; commit() then writes to it */
    void (*split)(void *user);
    void *user;
    size_t min_bytes;
    double min_seconds;

    uint64_t file_units; /* units committed to the current file */
    int pending;
    pin_split_item_t *items;
    size_t count, cap;
    size_t pend_bytes;
    double pend_seconds;
    long last_frame_index;
} pin_split_t;

void pin_split_init(pin_split_t *h, void (*commit)(void *, const uint8_t *, size_t),
                    void (*split)(void *), void *user);

/* One unit, in stream order. frame_index identifies the frame/GOP (several
 * units may share one, e.g. PAT/PMT before a picture; pass -1 for units that
 * carry no time); unit_seconds is the duration of a NEW frame index. Writes
 * straight to the file, or holds it while a split is pending (and performs
 * the split once the segment is big and long enough). */
void pin_split_push(pin_split_t *h, const uint8_t *data, size_t len, long frame_index,
                    double unit_seconds);

/* Cancel a pending split: held units go to the current file. */
void pin_split_cancel(pin_split_t *h);

/* A content split triggers at the next unit pushed. Returns 1 if a split is
 * now pending, 0 if ignored (the current file is still empty, so a new file
 * would only orphan it). Cancel first if one may already be pending. */
int pin_split_begin(pin_split_t *h);

/* End of capture/pass: same as cancel. */
void pin_split_flush(pin_split_t *h);

/* Drop everything held (session teardown). */
void pin_split_free(pin_split_t *h);

/* The caller opened a fresh file by other means (new pass): counters reset. */
void pin_split_new_file(pin_split_t *h);

#ifdef __cplusplus
}
#endif

#endif /* PIN_SPLIT_H */
