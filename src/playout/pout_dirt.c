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

/* Test seam POUT_TEST_DIRT: see pout_dirt.h. */

#include "pout_dirt.h"
#include "pin_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SPANS 32

typedef struct {
    int64_t from, to;   /* tape frames, inclusive */
    int reads;          /* damaged on this many reads of each frame; 0 = every read */
    int *count;         /* reads so far, per frame */
} span_t;

static span_t g_spans[MAX_SPANS];
static int g_nspans = -1;   /* -1: not parsed yet */

static void parse(void)
{
    g_nspans = 0;
    const char *v = getenv("POUT_TEST_DIRT");
    if (!v || !*v)
        return;
    const char *p = v;
    while (*p && g_nspans < MAX_SPANS) {
        char *end;
        long long a = strtoll(p, &end, 10);
        if (end == p)
            break;
        long long b = a;
        int reads = 1;
        p = end;
        if (*p == '-') {
            b = strtoll(p + 1, &end, 10);
            p = end;
        }
        if (*p == ':') {
            reads = (int)strtol(p + 1, &end, 10);
            p = end;
        }
        if (b >= a && b - a < 100000) {
            span_t *s = &g_spans[g_nspans];
            s->from = a;
            s->to = b;
            s->reads = reads < 0 ? 0 : reads;
            s->count = calloc((size_t)(b - a + 1), sizeof(int));
            if (s->count)
                g_nspans++;
        }
        while (*p && *p != ',')
            p++;
        if (*p == ',')
            p++;
    }
    for (int i = 0; i < g_nspans; i++)
        pin_logf(PIN_LOG_WARN, "playout: test seam POUT_TEST_DIRT: frames %lld-%lld damaged on %s\n",
                 (long long)g_spans[i].from, (long long)g_spans[i].to,
                 g_spans[i].reads ? "their first reads" : "every read");
}

/* DV: STA 7 (an error nobody concealed) in every fifth video block. */
static void damage_dv(uint8_t *buf, size_t len)
{
    size_t nblk = len / 80;
    for (size_t i = 0; i < nblk; i++) {
        size_t b = i % 150;
        if (b < 6 || (b - 6) % 16 == 0)
            continue;   /* header, subcode, VAUX, audio */
        if (i % 5 == 0)
            buf[i * 80 + 3] = (uint8_t)(0x70 | (buf[i * 80 + 3] & 0x0F));
    }
}

/* HDV: transport_error_indicator on every packet of the frame's slot. */
static void damage_ts(uint8_t *buf, size_t len)
{
    for (size_t i = 0; i + 188 <= len; i += 188)
        if (buf[i] == 0x47)
            buf[i + 1] |= 0x80;
}

void pout_dirt_apply(int64_t frame, uint8_t *buf, size_t len)
{
    if (g_nspans < 0)
        parse();
    for (int i = 0; i < g_nspans; i++) {
        span_t *s = &g_spans[i];
        if (frame < s->from || frame > s->to)
            continue;
        int n = ++s->count[frame - s->from];
        if (s->reads && n > s->reads)
            return;
        if (len % 188 == 0 && len && buf[0] == 0x47)
            damage_ts(buf, len);
        else
            damage_dv(buf, len);
        return;
    }
}
