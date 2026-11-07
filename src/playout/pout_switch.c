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

#include "pout_switch.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The guess, see the header. Indexed by pout_switch_kind_t. */
static const pout_switch_policy_t k_defaults[POUT_SWITCH_COUNT] = {
    { 0.0, 0.0, 0, 0 },     /* NONE */
    { 0.0, 0.5, 0, 0 },     /* STD: PAL <-> NTSC */
    { 0.5, 0.2, 0, 0 },     /* HDV_FMT: 1080i <-> 720p */
    { 1.0, 0.2, 0, 1 },     /* KIND: DV <-> HDV */
};

static pout_switch_policy_t g_table[POUT_SWITCH_COUNT];
static int g_init;

static void init(void)
{
    if (!g_init) {
        memcpy(g_table, k_defaults, sizeof g_table);
        g_init = 1;
    }
}

void pout_switch_reset(void)
{
    g_init = 0;
    init();
}

const char *pout_switch_name(pout_switch_kind_t k)
{
    switch (k) {
    case POUT_SWITCH_STD: return "std";
    case POUT_SWITCH_HDV_FMT: return "hdv-format";
    case POUT_SWITCH_KIND: return "kind";
    default: return "none";
    }
}

pout_switch_kind_t pout_switch_classify(const pout_frame_fmt_t *from, const pout_frame_fmt_t *to)
{
    if (!from || !to)
        return POUT_SWITCH_NONE;
    if (from->media != to->media)
        return POUT_SWITCH_KIND;
    int hdv = to->media == POUT_MEDIA_HDV;
    if (hdv && (from->hdv_720p != 0) != (to->hdv_720p != 0))
        return POUT_SWITCH_HDV_FMT;
    if ((from->is_625 != 0) != (to->is_625 != 0))
        return POUT_SWITCH_STD;
    return POUT_SWITCH_NONE;
}

const pout_switch_policy_t *pout_switch_policy(pout_switch_kind_t k)
{
    init();
    if ((int)k < 0 || k >= POUT_SWITCH_COUNT)
        k = POUT_SWITCH_NONE;
    return &g_table[k];
}

static int bad(char *err, size_t cap, const char *fmt, const char *a)
{
    if (err && cap)
        snprintf(err, cap, fmt, a);
    return -1;
}

int pout_switch_parse(const char *spec, pout_switch_kind_t *kind, pout_switch_policy_t *policy, char *err,
                      size_t err_cap)
{
    if (!spec || !*spec)
        return bad(err, err_cap, "%s", "--switch needs KIND=gap:SEC,blank:SEC,reset:0|1,restart:0|1");
    const char *eq = strchr(spec, '=');
    if (!eq)
        return bad(err, err_cap, "--switch \"%s\": expected KIND=key:value,... (KIND: std, hdv-format, kind)", spec);
    size_t kn = (size_t)(eq - spec);
    pout_switch_kind_t k = POUT_SWITCH_NONE;
    for (int i = POUT_SWITCH_STD; i < POUT_SWITCH_COUNT; i++)
        if (strlen(pout_switch_name((pout_switch_kind_t)i)) == kn &&
            strncmp(spec, pout_switch_name((pout_switch_kind_t)i), kn) == 0)
            k = (pout_switch_kind_t)i;
    if (k == POUT_SWITCH_NONE)
        return bad(err, err_cap, "--switch \"%s\": the kind must be std, hdv-format or kind", spec);
    pout_switch_policy_t p = *pout_switch_policy(k);
    const char *c = eq + 1;
    if (!*c)
        return bad(err, err_cap, "--switch \"%s\": no settings after '='", spec);
    while (*c) {
        const char *end = strchr(c, ',');
        size_t n = end ? (size_t)(end - c) : strlen(c);
        char item[64];
        if (n == 0 || n >= sizeof item)
            return bad(err, err_cap, "--switch \"%s\": empty or too long setting", spec);
        memcpy(item, c, n);
        item[n] = 0;
        char *colon = strchr(item, ':');
        if (!colon || !colon[1])
            return bad(err, err_cap, "--switch: \"%s\" is not key:value", item);
        *colon = 0;
        const char *key = item, *val = colon + 1;
        char *vend;
        double v = strtod(val, &vend);
        if (*vend || !isfinite(v))
            return bad(err, err_cap, "--switch: \"%s\" is not a number", val);
        if (strcmp(key, "gap") == 0 || strcmp(key, "blank") == 0) {
            if (v < 0 || v > 60)
                return bad(err, err_cap, "--switch: %s must be seconds from 0 to 60", key);
            if (key[0] == 'g')
                p.gap_s = v;
            else
                p.blank_s = v;
        } else if (strcmp(key, "reset") == 0 || strcmp(key, "restart") == 0) {
            if (v != 0 && v != 1)
                return bad(err, err_cap, "--switch: %s must be 0 or 1", key);
            if (strcmp(key, "reset") == 0)
                p.bus_reset = (int)v;
            else
                p.restart_stream = (int)v;
        } else {
            return bad(err, err_cap, "--switch: unknown setting \"%s\" (gap, blank, reset, restart)", key);
        }
        c += n;
        if (*c == ',')
            c++;
    }
    if (kind)
        *kind = k;
    if (policy)
        *policy = p;
    return 0;
}

int pout_switch_set(const char *spec, char *err, size_t err_cap)
{
    pout_switch_kind_t k;
    pout_switch_policy_t p;
    if (pout_switch_parse(spec, &k, &p, err, err_cap) != 0)
        return -1;
    init();
    g_table[k] = p;
    return 0;
}
