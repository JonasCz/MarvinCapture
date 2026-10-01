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
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond, msg)                                                         do {                                                                             if (!(cond)) {                                                                   printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                         g_failures++;                                                            }                                                                        } while (0)

/* Fake "files": per-file byte and unit counts, in order. */
typedef struct {
    int nfiles;
    size_t bytes[16];
    int units[16];
    pin_split_t h;
} rig_t;

static void r_commit(void *u, const uint8_t *d, size_t n)
{
    rig_t *r = u;
    (void)d;
    r->bytes[r->nfiles - 1] += n;
    r->units[r->nfiles - 1]++;
}
static void r_split(void *u) { ((rig_t *)u)->nfiles++; }

static const double FR = 0.04;       /* 25 fps frames */
static const size_t FB = 120000;     /* a DV PAL frame */
static uint8_t g_buf[120000];

static void rig_init(rig_t *r)
{
    memset(r, 0, sizeof(*r));
    r->nfiles = 1;
    pin_split_init(&r->h, r_commit, r_split, r);
}

static long g_fi;
static void frames(rig_t *r, int n)
{
    for (int i = 0; i < n; i++)
        pin_split_push(&r->h, g_buf, FB, g_fi++, FR);
}

int main(void)
{
    rig_t r;

    /* a long enough new segment becomes its own file, with all its frames */
    rig_init(&r); g_fi = 0;
    frames(&r, 100);
    CHECK(pin_split_begin(&r.h) == 1, "begin");
    frames(&r, 60); /* 2.4 s, 7 MB */
    CHECK(r.nfiles == 2, "split committed");
    CHECK(r.units[0] == 100 && r.units[1] == 60, "frames in the right files");
    pin_split_flush(&r.h);
    CHECK(r.nfiles == 2 && r.units[1] == 60, "flush after commit is a no-op");
    pin_split_free(&r.h);

    /* tape ends 10 frames after the cut: merged into the previous file */
    rig_init(&r); g_fi = 0;
    frames(&r, 100);
    pin_split_begin(&r.h);
    frames(&r, 10);
    CHECK(r.nfiles == 1 && r.units[0] == 100, "held while short");
    pin_split_flush(&r.h);
    CHECK(r.nfiles == 1 && r.units[0] == 110, "short tail appended to previous file");
    pin_split_free(&r.h);

    /* just under 1 s (24 frames at 25 fps) is still short; 25 is real */
    rig_init(&r); g_fi = 0;
    frames(&r, 10);
    pin_split_begin(&r.h);
    frames(&r, 24);
    CHECK(r.nfiles == 1, "0.96 s not enough");
    frames(&r, 1);
    CHECK(r.nfiles == 2 && r.units[1] == 25, "exactly 1 s and >1 MB splits");
    pin_split_free(&r.h);

    /* long but tiny (under 1 MB) stays held; big enough bytes releases it */
    rig_init(&r); g_fi = 0;
    frames(&r, 10);
    pin_split_begin(&r.h);
    for (int i = 0; i < 100; i++) /* 4 s, 100 kB */
        pin_split_push(&r.h, g_buf, 1000, g_fi++, FR);
    CHECK(r.nfiles == 1, "1 s but < 1 MB holds");
    pin_split_flush(&r.h);
    CHECK(r.nfiles == 1 && r.units[0] == 110, "merged");
    pin_split_free(&r.h);

    /* big (>1 MB) but under 1 s holds: 5 frames of 300 kB */
    rig_init(&r); g_fi = 0;
    frames(&r, 10);
    pin_split_begin(&r.h);
    for (int i = 0; i < 5; i++)
        for (int k = 0; k < 3; k++) /* 3 units per frame index */
            pin_split_push(&r.h, g_buf, 100000, g_fi, FR), (void)0;
    CHECK(r.nfiles == 1, "1.5 MB in 0.2 s holds (units sharing an index count once)");
    pin_split_flush(&r.h);
    CHECK(r.units[0] == 10 + 15, "all merged");
    pin_split_free(&r.h);

    /* a second trigger while pending: held units go to the old file, the new
     * trigger becomes the pending split and commits when real */
    rig_init(&r); g_fi = 0;
    frames(&r, 100);
    pin_split_begin(&r.h);
    frames(&r, 5);
    pin_split_cancel(&r.h);       /* what scene_cut does first */
    CHECK(r.nfiles == 1 && r.units[0] == 105, "pending frames merged into previous file");
    pin_split_begin(&r.h);
    frames(&r, 50);
    CHECK(r.nfiles == 2 && r.units[0] == 105 && r.units[1] == 50, "second trigger commits");
    pin_split_free(&r.h);

    /* the first file must still exist: a trigger on an empty file is ignored */
    rig_init(&r); g_fi = 0;
    CHECK(pin_split_begin(&r.h) == 0, "no split of an empty first file");
    frames(&r, 100);
    CHECK(r.nfiles == 1 && r.units[0] == 100, "frames stay in the first file");
    pin_split_free(&r.h);

    /* per-file counter restarts after a split / new pass */
    rig_init(&r); g_fi = 0;
    frames(&r, 10);
    pin_split_begin(&r.h);
    frames(&r, 40);
    CHECK(r.nfiles == 2, "split");
    pin_split_new_file(&r.h);
    CHECK(pin_split_begin(&r.h) == 0, "new empty file: begin ignored");
    pin_split_free(&r.h);

    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("test_pin_split: OK\n");
    return 0;
}
