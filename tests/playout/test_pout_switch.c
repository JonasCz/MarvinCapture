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

/* pout_switch: which kind of switch two formats make, the default table, and the override syntax. */

#include "pout_script.h"
#include "pout_switch.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static pout_frame_fmt_t fmt(pout_media_t m, int is625, int p720)
{
    pout_frame_fmt_t f;
    f.media = m;
    f.is_625 = is625;
    f.fps = is625 ? 25.0 : 30000.0 / 1001.0;
    f.hdv_720p = p720;
    f.max_packet = m == POUT_MEDIA_DV ? POUT_DV_MAX_PACKET : POUT_HDV_MAX_PACKET;
    return f;
}

int main(void)
{
    pout_frame_fmt_t pal = fmt(POUT_MEDIA_DV, 1, 0), ntsc = fmt(POUT_MEDIA_DV, 0, 0);
    pout_frame_fmt_t i50 = fmt(POUT_MEDIA_HDV, 1, 0), i60 = fmt(POUT_MEDIA_HDV, 0, 0);
    pout_frame_fmt_t p50 = fmt(POUT_MEDIA_HDV, 1, 1), p60 = fmt(POUT_MEDIA_HDV, 0, 1);

    /* classify */
    CHECK(pout_switch_classify(&pal, &pal) == POUT_SWITCH_NONE);
    CHECK(pout_switch_classify(&i50, &i50) == POUT_SWITCH_NONE);
    CHECK(pout_switch_classify(&pal, &ntsc) == POUT_SWITCH_STD);
    CHECK(pout_switch_classify(&ntsc, &pal) == POUT_SWITCH_STD);
    CHECK(pout_switch_classify(&i50, &i60) == POUT_SWITCH_STD);
    CHECK(pout_switch_classify(&i50, &p50) == POUT_SWITCH_HDV_FMT);
    CHECK(pout_switch_classify(&p60, &i60) == POUT_SWITCH_HDV_FMT);
    CHECK(pout_switch_classify(&i50, &p60) == POUT_SWITCH_HDV_FMT);    /* both change: the more severe */
    CHECK(pout_switch_classify(&pal, &i50) == POUT_SWITCH_KIND);
    CHECK(pout_switch_classify(&i60, &ntsc) == POUT_SWITCH_KIND);
    CHECK(pout_switch_classify(&pal, &p60) == POUT_SWITCH_KIND);
    CHECK(pout_switch_classify(NULL, &pal) == POUT_SWITCH_NONE);

    /* the defaults */
    pout_switch_reset();
    const pout_switch_policy_t *k = pout_switch_policy(POUT_SWITCH_KIND);
    CHECK(k->gap_s == 1.0 && k->blank_s == 0.2 && k->bus_reset == 0 && k->restart_stream == 1);
    const pout_switch_policy_t *s = pout_switch_policy(POUT_SWITCH_STD);
    CHECK(s->gap_s == 0.0 && s->blank_s == 0.5 && s->bus_reset == 0 && s->restart_stream == 0);
    const pout_switch_policy_t *h = pout_switch_policy(POUT_SWITCH_HDV_FMT);
    CHECK(h->gap_s == 0.5 && h->blank_s == 0.2 && h->bus_reset == 0 && h->restart_stream == 0);
    const pout_switch_policy_t *n = pout_switch_policy(POUT_SWITCH_NONE);
    CHECK(n->gap_s == 0 && n->blank_s == 0 && !n->bus_reset && !n->restart_stream);
    CHECK(pout_switch_policy((pout_switch_kind_t)99) == pout_switch_policy(POUT_SWITCH_NONE));

    /* parse does not change the table; keys not mentioned keep their value */
    char err[200] = "";
    pout_switch_kind_t pk;
    pout_switch_policy_t pp;
    CHECK(pout_switch_parse("kind=gap:2.5,reset:1", &pk, &pp, err, sizeof err) == 0);
    CHECK(pk == POUT_SWITCH_KIND && pp.gap_s == 2.5 && pp.blank_s == 0.2 && pp.bus_reset == 1 && pp.restart_stream == 1);
    CHECK(pout_switch_policy(POUT_SWITCH_KIND)->gap_s == 1.0);

    /* set */
    CHECK(pout_switch_set("std=gap:0.25,blank:0,restart:1", err, sizeof err) == 0);
    s = pout_switch_policy(POUT_SWITCH_STD);
    CHECK(s->gap_s == 0.25 && s->blank_s == 0 && s->restart_stream == 1 && s->bus_reset == 0);
    CHECK(pout_switch_set("hdv-format=gap:0,blank:0,reset:0,restart:0", err, sizeof err) == 0);
    CHECK(pout_switch_policy(POUT_SWITCH_HDV_FMT)->gap_s == 0 && pout_switch_policy(POUT_SWITCH_HDV_FMT)->blank_s == 0);
    CHECK(pout_switch_policy(POUT_SWITCH_KIND)->gap_s == 1.0);          /* untouched */
    pout_switch_reset();
    CHECK(pout_switch_policy(POUT_SWITCH_STD)->gap_s == 0.0 && pout_switch_policy(POUT_SWITCH_STD)->restart_stream == 0);

    /* errors name the problem and change nothing */
    const char *bad[] = { "", "kind", "kind=", "none=gap:1", "bogus=gap:1", "kind=gap", "kind=gap:", "kind=gap:x",
                          "kind=gap:-1", "kind=gap:61", "kind=reset:2", "kind=wait:1", "kind=gap:1,,blank:1", "kind=gap:nan" };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        err[0] = 0;
        CHECK(pout_switch_set(bad[i], err, sizeof err) != 0);
        CHECK(err[0] != 0);
    }
    CHECK(pout_switch_policy(POUT_SWITCH_KIND)->gap_s == 1.0);
    CHECK(strcmp(pout_switch_name(POUT_SWITCH_HDV_FMT), "hdv-format") == 0);

    /* the command line: validated at parse time, kept for the caller to apply, several allowed */
    {
        pout_script_t *sc = NULL;
        const char *a[] = { "x.dv", "--switch", "kind=gap:0,blank:0", "--switch=std=blank:1.5", "--dry-run", "o.dv" };
        CHECK(pout_script_parse(6, a, &sc, err, sizeof err) == PIN_OK);
        if (sc) {
            CHECK(sc->nswitches == 2 && strcmp(sc->switches[0], "kind=gap:0,blank:0") == 0 &&
                  strcmp(sc->switches[1], "std=blank:1.5") == 0);
            CHECK(pout_switch_policy(POUT_SWITCH_KIND)->gap_s == 1.0);      /* parsing applies nothing */
            pout_script_destroy(sc);
        }
        const char *b[] = { "x.dv", "--switch", "kind=gap:abc" };
        err[0] = 0;
        CHECK(pout_script_parse(3, b, &sc, err, sizeof err) == PIN_ERR_ARG && strstr(err, "abc") != NULL);
        const char *c[] = { "x.dv", "--switch" };
        CHECK(pout_script_parse(2, c, &sc, err, sizeof err) == PIN_ERR_ARG);
        const char *d[] = { "x.dv", "--play", "--switch", "kind=gap:1" };
        CHECK(pout_script_parse(4, d, &sc, err, sizeof err) == PIN_ERR_ARG);
        CHECK(strstr(pout_script_help_text(), "--switch") != NULL);
    }

    if (failures)
        return 1;
    printf("test_pout_switch: ok\n");
    return 0;
}
