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

#include "pin_holdback.h"
#include <stdlib.h>
#include <string.h>

void pin_holdback_init(pin_holdback_t *q, void (*commit)(void *, const uint8_t *, size_t, long),
                       void *user)
{
    memset(q, 0, sizeof(*q));
    q->commit = commit;
    q->user = user;
    q->max_bytes = PIN_HOLD_MAX_BYTES;
}

static pin_hold_item_t *at(pin_holdback_t *q, size_t i)
{
    return &q->items[(q->head + i) % q->cap];
}

/* Commits and frees the oldest unit. */
static void pop_commit(pin_holdback_t *q)
{
    pin_hold_item_t *it = at(q, 0);
    if (q->commit)
        q->commit(q->user, it->data, it->len, it->frame_index);
    free(it->data);
    it->data = NULL;
    q->bytes -= it->len;
    q->head = (q->head + 1) % q->cap;
    q->count--;
    if (q->count == 0)
        q->head = 0;
}

static int grow(pin_holdback_t *q)
{
    size_t ncap = q->cap ? q->cap * 2 : 64;
    pin_hold_item_t *n = malloc(ncap * sizeof(*n));
    if (!n)
        return -1;
    for (size_t i = 0; i < q->count; i++)
        n[i] = *at(q, i);
    free(q->items);
    q->items = n;
    q->cap = ncap;
    q->head = 0;
    return 0;
}

void pin_holdback_push(pin_holdback_t *q, const uint8_t *data, size_t len, long frame_index,
                       unsigned flags)
{
    uint8_t *copy = NULL;
    if (q->count < q->cap || grow(q) == 0)
        copy = malloc(len ? len : 1);
    if (!copy) { /* out of memory: give up holding, keep the data and its order */
        pin_holdback_flush(q);
        if (q->commit)
            q->commit(q->user, data, len, frame_index);
        return;
    }
    memcpy(copy, data, len);
    pin_hold_item_t *it = at(q, q->count);
    it->data = copy;
    it->len = len;
    it->frame_index = frame_index;
    it->flags = flags;
    q->count++;
    q->bytes += len;
    while (q->max_bytes && q->bytes > q->max_bytes && q->count > 1)
        pop_commit(q);
}

void pin_holdback_keep_last(pin_holdback_t *q, size_t n)
{
    while (q->count > n)
        pop_commit(q);
}

void pin_holdback_release_before(pin_holdback_t *q, long frame_index)
{
    while (q->count && at(q, 0)->frame_index < frame_index)
        pop_commit(q);
}

void pin_holdback_cut(pin_holdback_t *q, long frame_index, void (*at_cut)(void *user, long index),
                      void *user)
{
    /* the frame the new segment starts with: the first one a file may start on */
    long cut_frame = -1;
    for (size_t i = 0; i < q->count; i++) {
        const pin_hold_item_t *it = at(q, i);
        if (it->frame_index >= frame_index && (it->flags & PIN_HOLD_CUT_OK)) {
            cut_frame = it->frame_index;
            break;
        }
    }
    if (cut_frame < 0) /* already released: as close to it as what is held allows */
        cut_frame = frame_index;
    while (q->count && at(q, 0)->frame_index < cut_frame)
        pop_commit(q);
    if (at_cut)
        at_cut(user, cut_frame);
    pin_holdback_flush(q);
}

void pin_holdback_flush(pin_holdback_t *q)
{
    while (q->count)
        pop_commit(q);
}

void pin_holdback_free(pin_holdback_t *q)
{
    for (size_t i = 0; i < q->count; i++)
        free(at(q, i)->data);
    free(q->items);
    q->items = NULL;
    q->head = q->count = q->cap = 0;
    q->bytes = 0;
}
