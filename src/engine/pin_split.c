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

#include "pin_split.h"
#include <stdlib.h>
#include <string.h>

void pin_split_init(pin_split_t *h, void (*commit)(void *, const uint8_t *, size_t),
                    void (*split)(void *), void *user)
{
    memset(h, 0, sizeof(*h));
    h->commit = commit;
    h->split = split;
    h->user = user;
    h->min_bytes = PIN_SPLIT_MIN_BYTES;
    h->min_seconds = PIN_SPLIT_MIN_SECONDS;
    h->last_frame_index = -1;
}

static void drop_items(pin_split_t *h)
{
    for (size_t i = 0; i < h->count; i++)
        free(h->items[i].data);
    h->count = 0;
    h->pend_bytes = 0;
    h->pend_seconds = 0;
    h->pending = 0;
    h->last_frame_index = -1;
}

static void flush_to_file(pin_split_t *h)
{
    for (size_t i = 0; i < h->count; i++) {
        h->commit(h->user, h->items[i].data, h->items[i].len);
        h->file_units++;
    }
    drop_items(h);
}

void pin_split_cancel(pin_split_t *h)
{
    if (h->pending)
        flush_to_file(h);
}

void pin_split_flush(pin_split_t *h) { pin_split_cancel(h); }

void pin_split_free(pin_split_t *h)
{
    drop_items(h);
    free(h->items);
    h->items = NULL;
    h->cap = 0;
}

void pin_split_new_file(pin_split_t *h) { h->file_units = 0; }

int pin_split_begin(pin_split_t *h)
{
    if (h->pending)
        return 1;
    if (h->file_units == 0)
        return 0;
    h->pending = 1;
    return 1;
}

void pin_split_push(pin_split_t *h, const uint8_t *data, size_t len, long frame_index,
                    double unit_seconds)
{
    if (!h->pending) {
        h->commit(h->user, data, len);
        h->file_units++;
        return;
    }
    if (h->count == h->cap) {
        size_t ncap = h->cap ? h->cap * 2 : 64;
        pin_split_item_t *n = realloc(h->items, ncap * sizeof(*n));
        if (!n) { /* out of memory: give up on the lookahead, keep the data */
            flush_to_file(h);
            h->commit(h->user, data, len);
            h->file_units++;
            return;
        }
        h->items = n;
        h->cap = ncap;
    }
    uint8_t *copy = malloc(len ? len : 1);
    if (!copy) {
        flush_to_file(h);
        h->commit(h->user, data, len);
        h->file_units++;
        return;
    }
    memcpy(copy, data, len);
    h->items[h->count].data = copy;
    h->items[h->count].len = len;
    h->count++;
    h->pend_bytes += len;
    if (frame_index >= 0 && frame_index != h->last_frame_index) {
        h->pend_seconds += unit_seconds;
        h->last_frame_index = frame_index;
    }
    if (h->pend_bytes >= h->min_bytes && h->pend_seconds >= h->min_seconds) {
        /* a real segment: commit the split, then the held units go to the new file */
        h->split(h->user);
        h->file_units = 0;
        flush_to_file(h);
    }
}
