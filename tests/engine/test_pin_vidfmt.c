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

#include "pin_vidfmt.h"
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

static void label_is(int h, int n, int d, int il, const char *want)
{
    char b[24];
    pin_vidfmt_label(h, n, d, il, b, sizeof(b));
    if (strcmp(b, want)) { printf("FAIL label %d %d/%d -> '%s', want '%s'\n", h, n, d, b, want); g_failures++; }
}

static void test_labels(void)
{
    label_is(576, 25, 1, 1, "PAL");
    label_is(480, 30000, 1001, 1, "NTSC");
    label_is(1080, 25, 1, 1, "1080i25");
    label_is(1080, 30000, 1001, 1, "1080i29.97");
    label_is(720, 50, 1, 0, "720p50");
    label_is(720, 60000, 1001, 0, "720p59.94");
    label_is(720, 24000, 1001, 0, "720p23.976");
    label_is(0, 25, 1, 1, "");
    label_is(1080, 0, 0, 1, "");
    int n, d;
    CHECK(pin_vidfmt_rate_from_code(3, &n, &d) && n == 25 && d == 1, "code 3 = 25");
    CHECK(pin_vidfmt_rate_from_code(7, &n, &d) && n == 60000 && d == 1001, "code 7 = 59.94");
    CHECK(!pin_vidfmt_rate_from_code(0, &n, &d) && !n, "code 0 unknown");
    CHECK(!pin_vidfmt_rate_from_code(9, &n, &d), "code 9 unknown");
    CHECK(pin_vidfmt_hdv_interlaced(1080) && !pin_vidfmt_hdv_interlaced(720), "interlace by height");
}

#define VPID 0x1011

/* one video packet holding a PES start, a sequence header and a picture header */
static void seq_unit(uint8_t *p, int w, int h, int code)
{
    memset(p, 0xFF, 188);
    p[0] = 0x47; p[1] = 0x40 | (VPID >> 8); p[2] = VPID & 0xFF; p[3] = 0x10;
    uint8_t *q = p + 4;
    q[0] = 0; q[1] = 0; q[2] = 1; q[3] = 0xE0; q[4] = 0; q[5] = 0; q[6] = 0x80; q[7] = 0; q[8] = 0;
    q += 9;
    q[0] = 0; q[1] = 0; q[2] = 1; q[3] = 0xB3;
    q[4] = (uint8_t)(w >> 4);
    q[5] = (uint8_t)(((w & 15) << 4) | (h >> 8));
    q[6] = (uint8_t)h;
    q[7] = (uint8_t)(0x30 | code);          /* aspect 3 (16:9), frame_rate_code */
    q += 8;
    q[0] = 0; q[1] = 0; q[2] = 1; q[3] = 0; q[4] = 0; q[5] = 1 << 3;
}

static void test_sequence_header(void)
{
    hdv_err_state_t st;
    hdv_unit_errors_t e;
    uint8_t u[188];
    hdv_error_init(&st);
    seq_unit(u, 1440, 1080, 3);
    hdv_error_analyze(&st, u, 1, VPID, &e);
    CHECK(e.seq_found && e.seq_width == 1440 && e.seq_height == 1080 && e.seq_frame_rate_code == 3,
          "1440x1080 25");
    seq_unit(u, 1280, 720, 7);
    hdv_error_analyze(&st, u, 1, VPID, &e);
    CHECK(e.seq_found && e.seq_width == 1280 && e.seq_height == 720 && e.seq_frame_rate_code == 7,
          "1280x720 59.94");
    u[4 + 9 + 3] = 0xB8; /* not a sequence header any more */
    hdv_error_analyze(&st, u, 1, VPID, &e);
    CHECK(!e.seq_found, "no sequence header");
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
    hdv_err_state_t st;
    hdv_error_init(&st);
    hdv_unit_errors_t e;
    hdv_error_analyze(&st, d, n, map.video_pid, &e);
    CHECK(e.seq_found, "fixture has a sequence header");
    CHECK(e.seq_width == 1440 && e.seq_height == 1080, "fixture is 1440x1080");
    int num, den;
    char lab[24];
    pin_vidfmt_rate_from_code(e.seq_frame_rate_code, &num, &den);
    pin_vidfmt_label(e.seq_height, num, den, pin_vidfmt_hdv_interlaced(e.seq_height), lab, sizeof(lab));
    printf("hdv.ts: %s\n", lab);
    CHECK(lab[0] == '1', "fixture label");
    free(d);
}
#endif

int main(void)
{
    test_labels();
    test_sequence_header();
#ifdef TEST_DATA_DIR
    test_fixture();
#endif
    if (g_failures == 0) printf("test_pin_vidfmt: ok\n");
    return g_failures;
}
