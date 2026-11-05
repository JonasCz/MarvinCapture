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

/* pin_holdback: units come out in order, exactly once, and a cut lands on a
 * unit a file may start with. */

#include "pin_holdback.h"
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

/* What reached commit(): unit ids (the first data byte) in order, and where
 * the cut went (-1 = none): the number of units committed before it. */
typedef struct {
    int ids[1024];
    long frames[1024];
    int n;
    int cut_after;
    long cut_index;
} log_t;

static void commit(void *user, const uint8_t *data, size_t len, long frame_index)
{
    log_t *l = user;
    (void)len;
    l->frames[l->n] = frame_index;
    l->ids[l->n++] = data[0];
}

static void at_cut(void *user, long index)
{
    log_t *l = user;
    l->cut_after = l->n;
    l->cut_index = index;
}

static void reset(log_t *l, pin_holdback_t *q)
{
    memset(l, 0, sizeof(*l));
    l->cut_after = -1;
    pin_holdback_init(q, commit, l);
}

static void push(pin_holdback_t *q, int id, long frame, unsigned flags)
{
    uint8_t buf[100];
    memset(buf, 0, sizeof(buf));
    buf[0] = (uint8_t)id;
    pin_holdback_push(q, buf, sizeof(buf), frame, flags);
}

static int in_order(const log_t *l, int first, int count)
{
    if (l->n != count)
        return 0;
    for (int i = 0; i < count; i++)
        if (l->ids[i] != first + i)
            return 0;
    return 1;
}

/* DV: a fixed window of the last n frames, as the DV scene split holds */
static void test_keep_last(void)
{
    log_t l;
    pin_holdback_t q;
    reset(&l, &q);
    for (int i = 0; i < 20; i++) {
        pin_holdback_keep_last(&q, 5);
        push(&q, i, i, PIN_HOLD_CUT_OK);
        CHECK(q.count == (size_t)(i < 5 ? i + 1 : 6), "holds the last 6");
    }
    CHECK(in_order(&l, 0, 14), "the oldest go out in order");
    pin_holdback_flush(&q);
    CHECK(in_order(&l, 0, 20), "flush writes the rest, in order");
    CHECK(q.count == 0 && q.bytes == 0, "empty after flush");
    pin_holdback_free(&q);
}

/* HDV: release what no pending cut can still land on */
static void test_release_before(void)
{
    log_t l;
    pin_holdback_t q;
    reset(&l, &q);
    push(&q, 0, -1, 0); /* PAT, no time */
    push(&q, 1, -1, 0); /* PMT */
    for (int i = 0; i < 10; i++)
        push(&q, 2 + i, i, i % 4 == 0 ? PIN_HOLD_CUT_OK : 0);
    pin_holdback_release_before(&q, 4);
    CHECK(in_order(&l, 0, 6), "PAT, PMT and frames 0-3 released");
    pin_holdback_release_before(&q, 4);
    CHECK(l.n == 6, "again: nothing more");
    pin_holdback_release_before(&q, 100);
    CHECK(in_order(&l, 0, 12), "all released");
    pin_holdback_free(&q);
}

/* the cut lands on the CUT_OK unit, with the units of its frame before it */
static void test_cut_on_gop(void)
{
    log_t l;
    pin_holdback_t q;
    reset(&l, &q);
    /* frames 0..29, GOPs (CUT_OK) every 12; a PAT/PMT pair rides with frame 12 */
    int id = 0;
    for (int f = 0; f < 30; f++) {
        if (f == 12) {
            push(&q, id++, 12, 0);
            push(&q, id++, 12, 0);
        }
        push(&q, id++, f, f % 12 == 0 ? PIN_HOLD_CUT_OK : 0);
    }
    pin_holdback_cut(&q, 12, at_cut, &l);
    CHECK(in_order(&l, 0, id), "every unit once, in order");
    CHECK(l.cut_after == 12, "frames 0-11 before the cut");
    CHECK(l.cut_index == 12, "the cut is at frame 12");
    CHECK(l.ids[12] == 12 && l.frames[12] == 12, "the units of frame 12 follow the cut");
    CHECK(q.count == 0, "nothing stays held");

    /* asked for a frame that is not a GOP start: the next one */
    reset(&l, &q);
    for (int f = 0; f < 30; f++)
        push(&q, f, f, f % 12 == 0 ? PIN_HOLD_CUT_OK : 0);
    pin_holdback_cut(&q, 13, at_cut, &l);
    CHECK(in_order(&l, 0, 30) && l.cut_after == 24 && l.cut_index == 24, "rounds up to frame 24");

    /* nothing it may land on: at the asked frame */
    reset(&l, &q);
    for (int f = 0; f < 10; f++)
        push(&q, f, f, 0);
    pin_holdback_cut(&q, 5, at_cut, &l);
    CHECK(in_order(&l, 0, 10) && l.cut_after == 5 && l.cut_index == 5, "no GOP start held: at 5");

    /* asked past everything held: after it */
    reset(&l, &q);
    for (int f = 0; f < 4; f++)
        push(&q, f, f, PIN_HOLD_CUT_OK);
    pin_holdback_cut(&q, 9, at_cut, &l);
    CHECK(in_order(&l, 0, 4) && l.cut_after == 4, "all before the cut");

    /* the cut frame was released already (DV: older than the window): at the first held */
    reset(&l, &q);
    for (int f = 0; f < 10; f++)
        push(&q, f, f, PIN_HOLD_CUT_OK);
    pin_holdback_release_before(&q, 6);
    pin_holdback_cut(&q, 3, at_cut, &l);
    CHECK(in_order(&l, 0, 10) && l.cut_after == 6 && l.cut_index == 6, "lands on the oldest held");
    pin_holdback_free(&q);
}

/* the memory bound: the oldest go out even if a cut would want them */
static void test_max_bytes(void)
{
    log_t l;
    pin_holdback_t q;
    reset(&l, &q);
    q.max_bytes = 1000; /* 10 units of 100 */
    for (int f = 0; f < 25; f++)
        push(&q, f, f, PIN_HOLD_CUT_OK);
    CHECK(q.count == 10 && q.bytes == 1000, "holds at most max_bytes");
    CHECK(in_order(&l, 0, 15), "the oldest committed in order");
    pin_holdback_free(&q);
    CHECK(q.count == 0 && q.items == NULL, "free drops what is held");
    CHECK(l.n == 15, "free commits nothing");
}

/* a ring that wraps and grows keeps the order */
static void test_wrap_and_grow(void)
{
    log_t l;
    pin_holdback_t q;
    reset(&l, &q);
    int id = 0, next = 0;
    for (int round = 0; round < 6; round++) {
        for (int i = 0; i < 50 + round * 20; i++, id++)
            push(&q, id & 0xFF, id, PIN_HOLD_CUT_OK);
        pin_holdback_release_before(&q, id - 30);
    }
    pin_holdback_flush(&q);
    int ok = l.n == id;
    for (int i = 0; i < l.n && ok; i++, next++)
        ok = l.ids[i] == (next & 0xFF) && l.frames[i] == next;
    CHECK(ok, "every unit once, in order, across wraps and growth");
    pin_holdback_free(&q);
}

int main(void)
{
    test_keep_last();
    test_release_before();
    test_cut_on_gop();
    test_max_bytes();
    test_wrap_and_grow();
    if (g_failures == 0) {
        printf("test_pin_holdback: all tests passed\n");
        return 0;
    }
    printf("test_pin_holdback: %d failure(s)\n", g_failures);
    return 1;
}
