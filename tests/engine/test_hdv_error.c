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

#include "hdv_error.h"
#include "hdv_aux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

#define VPID 0x1011
#define APID 0x1100

static void pkt(uint8_t *p, int pid, int pusi, int cc, const uint8_t *pl)
{
    memset(p, 0xFF, 188);
    p[0] = 0x47;
    p[1] = (uint8_t)((pusi ? 0x40 : 0) | (pid >> 8));
    p[2] = (uint8_t)pid;
    p[3] = (uint8_t)(0x10 | (cc & 15));
    memcpy(p + 4, pl, 184);
}

static int cc_v, cc_a;

/* One picture: PES start + (GOP header) + picture header, 3 video packets and
 * 1 audio packet. type 1/2/3 = I/P/B. */
static void make_pic(uint8_t *out, int type, int gop, int closed)
{
    uint8_t pl[184];
    memset(pl, 0x55, sizeof(pl));
    int o = 0;
    pl[o++] = 0; pl[o++] = 0; pl[o++] = 1; pl[o++] = 0xE0; pl[o++] = 0; pl[o++] = 0;
    pl[o++] = 0x80; pl[o++] = 0; pl[o++] = 0;
    if (gop) {
        pl[o++] = 0; pl[o++] = 0; pl[o++] = 1; pl[o++] = 0xB8;
        pl[o++] = 0; pl[o++] = 0; pl[o++] = 0; pl[o++] = (uint8_t)(closed ? 0x40 : 0);
    }
    pl[o++] = 0; pl[o++] = 0; pl[o++] = 1; pl[o++] = 0x00;
    pl[o++] = 0; pl[o++] = (uint8_t)(type << 3);
    pkt(out, VPID, 1, cc_v++, pl);
    memset(pl, 0x55, sizeof(pl));
    pkt(out + 188, VPID, 0, cc_v++, pl);
    pkt(out + 376, APID, 0, cc_a++, pl);
    pkt(out + 564, VPID, 0, cc_v++, pl);
}

static void test_synthetic(void)
{
    hdv_err_state_t st;
    hdv_error_init(&st);
    cc_v = 3; cc_a = 9;
    uint8_t u[4 * 188];
    hdv_unit_errors_t e;

    make_pic(u, 1, 1, 0);
    hdv_error_analyze(&st, u, 4, VPID, &e);
    CHECK(!e.frame_error && e.pic_type == 1, "clean I");
    make_pic(u, 2, 0, 0);
    hdv_error_analyze(&st, u, 4, VPID, &e);
    CHECK(!e.frame_error && e.pic_type == 2, "clean P");
    make_pic(u, 3, 0, 0);
    hdv_error_analyze(&st, u, 4, VPID, &e);
    CHECK(!e.frame_error && e.pic_type == 3, "clean B");

    /* lose a video packet in the next P: it, the following B and the P after
     * that are bad, until the next I */
    make_pic(u, 2, 0, 0);
    memmove(u + 188, u + 376, 188 * 2); /* drop the 2nd video packet */
    hdv_error_analyze(&st, u, 3, VPID, &e);
    CHECK(e.frame_error && e.cc_errors == 1 && e.video_damaged, "P with a lost packet");
    make_pic(u, 3, 0, 0);
    hdv_error_analyze(&st, u, 4, VPID, &e);
    CHECK(e.frame_error && e.tainted && !e.video_damaged, "B after damaged refs is tainted");
    make_pic(u, 2, 0, 0);
    hdv_error_analyze(&st, u, 4, VPID, &e);
    CHECK(e.frame_error && e.tainted, "P after damaged P is tainted");
    make_pic(u, 1, 1, 0);
    hdv_error_analyze(&st, u, 4, VPID, &e);
    CHECK(!e.frame_error, "I recovers");
    make_pic(u, 3, 0, 0);
    hdv_error_analyze(&st, u, 4, VPID, &e);
    CHECK(e.frame_error && e.tainted, "open-GOP B after I still uses the damaged previous ref");
    make_pic(u, 2, 0, 0);
    hdv_error_analyze(&st, u, 4, VPID, &e);
    CHECK(!e.frame_error, "P after the new I is fine");
    make_pic(u, 1, 1, 1);
    hdv_error_analyze(&st, u, 4, VPID, &e);
    make_pic(u, 3, 0, 0);
    hdv_error_analyze(&st, u, 4, VPID, &e);
    CHECK(!e.frame_error, "clean closed-GOP B");

    /* audio-only loss: the unit has an error, references untouched */
    make_pic(u, 2, 0, 0);
    cc_a += 2;
    u[2 * 188 + 3] = (uint8_t)(0x10 | (cc_a & 15));
    cc_a++;
    hdv_error_analyze(&st, u, 4, VPID, &e);
    CHECK(e.frame_error && !e.video_damaged && e.cc_errors == 1, "audio gap counted");
    make_pic(u, 3, 0, 0);
    hdv_error_analyze(&st, u, 4, VPID, &e);
    CHECK(!e.frame_error, "audio gap does not taint");

    /* TEI */
    make_pic(u, 2, 0, 0);
    u[188 + 1] |= 0x80;
    hdv_error_analyze(&st, u, 4, VPID, &e);
    CHECK(e.frame_error && e.tei == 1 && e.video_damaged, "TEI");

    /* no picture header at all */
    make_pic(u, 1, 1, 0);
    memset(u + 4 + 9, 0x55, 30);
    hdv_error_analyze(&st, u, 4, VPID, &e);
    CHECK(e.frame_error && e.pes_errors, "no picture header");
}

#ifdef TEST_DATA_DIR
static void test_fixture(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/hdv.ts", TEST_DATA_DIR);
    FILE *fp = fopen(path, "rb");
    if (!fp) { printf("skip hdv.ts (not present)\n"); return; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)sz);
    size_t n = fread(d, 1, (size_t)sz, fp) / 188;
    fclose(fp);
    hdv_pid_map_t map;
    memset(&map, 0, sizeof(map));
    map.pmt_pid = map.video_pid = map.aux_pid = map.audio_pid = -1;
    hdv_scan_pat_pmt(d, n, &map);
    CHECK(map.video_pid > 0, "video pid found");
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) { /* drop one video packet in the middle of the stream */
            size_t i = n / 2;
            while (((d[i * 188 + 1] & 0x1F) << 8 | d[i * 188 + 2]) != map.video_pid ||
                   (d[i * 188 + 1] & 0x40))
                i++;
            memmove(d + i * 188, d + (i + 1) * 188, (n - i - 1) * 188);
            n--;
        }
        hdv_err_state_t st;
        hdv_error_init(&st);
        size_t start = 0;
        int units = 0, bad = 0;
        for (size_t i = 0; i <= n; i++) {
            int is_start = i < n && (d[i * 188 + 1] & 0x40) &&
                           (((d[i * 188 + 1] & 0x1F) << 8) | d[i * 188 + 2]) == map.video_pid;
            if (i == n || is_start) {
                if (i > start && units > 0) { /* skip what precedes the first picture */
                    hdv_unit_errors_t e;
                    hdv_error_analyze(&st, d + start * 188, i - start, map.video_pid, &e);
                    if (e.frame_error) bad++;
                }
                if (is_start) units++;
                start = i;
            }
        }
        if (pass == 0) CHECK(bad == 0, "real HDV stream is error-free");
        else CHECK(bad >= 1, "dropped packet in the real stream is detected");
    }
    free(d);
}
#endif

int main(void)
{
    test_synthetic();
#ifdef TEST_DATA_DIR
    test_fixture();
#endif
    if (g_failures == 0) printf("test_hdv_error: ok\n");
    return g_failures;
}
