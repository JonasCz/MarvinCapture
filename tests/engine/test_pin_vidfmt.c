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

#include "../test_fixture.h"

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
    CHECK(pin_vidfmt_hdv_interlaced(1080, -1) && !pin_vidfmt_hdv_interlaced(720, -1),
          "no sequence extension yet: interlace by height");
    CHECK(pin_vidfmt_hdv_interlaced(1080, 0) && !pin_vidfmt_hdv_interlaced(1080, 1) &&
          !pin_vidfmt_hdv_interlaced(720, 1), "progressive_sequence decides (1080p is progressive)");
    CHECK(pin_vidfmt_mpeg2_dar(3, 1440, 1080, &n, &d) && n == 16 && d == 9, "aspect 3 = 16:9");
    CHECK(pin_vidfmt_mpeg2_dar(2, 1440, 1080, &n, &d) && n == 4 && d == 3, "aspect 2 = 4:3");
    CHECK(pin_vidfmt_mpeg2_dar(1, 1280, 720, &n, &d) && n == 16 && d == 9, "square 1280x720 = 16:9");
    CHECK(!pin_vidfmt_mpeg2_dar(0, 1440, 1080, &n, &d) && !n && !d, "aspect 0 forbidden");
    CHECK(!pin_vidfmt_mpeg2_dar(7, 1440, 1080, &n, &d) && !n, "aspect 7 reserved");
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

/* Two video packets: a PES start, a sequence header, a sequence extension whose start code
 * is cut by the packet boundary, a picture header and a picture coding extension. */
static void ext_unit(uint8_t *p, int w, int h, int code, int progressive_seq, int tff, int progressive_frame)
{
    memset(p, 0xFF, 2 * 188);
    for (int k = 0; k < 2; k++) {
        p[k * 188] = 0x47;
        p[k * 188 + 1] = (uint8_t)((k ? 0 : 0x40) | (VPID >> 8));
        p[k * 188 + 2] = VPID & 0xFF;
        p[k * 188 + 3] = (uint8_t)(0x10 | k);
    }
    uint8_t *q = p + 4;
    q[0] = 0; q[1] = 0; q[2] = 1; q[3] = 0xE0; q[4] = 0; q[5] = 0; q[6] = 0x80; q[7] = 0; q[8] = 0;
    q += 9;
    q[0] = 0; q[1] = 0; q[2] = 1; q[3] = 0xB3;
    q[4] = (uint8_t)(w >> 4);
    q[5] = (uint8_t)(((w & 15) << 4) | (h >> 8));
    q[6] = (uint8_t)h;
    q[7] = (uint8_t)(0x30 | code);
    p[186] = 0; p[187] = 0;                 /* 00 00 | 01 B5: the start code spans the packets */
    q = p + 188 + 4;
    q[0] = 1; q[1] = 0xB5;
    /* id 1, Main@High-1440 (0x48), progressive_sequence, 4:2:0, marker; frame_rate_ext 0 */
    q[2] = 0x14; q[3] = (uint8_t)(0x82 | (progressive_seq << 3)); q[4] = 0; q[5] = 1; q[6] = 0; q[7] = 0;
    q += 8;
    q[0] = 0; q[1] = 0; q[2] = 1; q[3] = 0; q[4] = 0; q[5] = 1 << 3;  /* picture, I */
    q += 6;
    /* picture coding extension: id 8, f_codes 15, frame picture (3), top_field_first,
     * frame_pred_frame_dct and chroma_420_type = progressive_frame, progressive_frame */
    q[0] = 0; q[1] = 0; q[2] = 1; q[3] = 0xB5;
    q[4] = 0x8F; q[5] = 0xFF; q[6] = 0xF3;
    q[7] = (uint8_t)((tff << 7) | (progressive_frame ? 0x41 : 0));
    q[8] = (uint8_t)(progressive_frame << 7);
    q += 9;
    q[0] = 0; q[1] = 0; q[2] = 1; q[3] = 0x01;   /* the first slice */
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
    CHECK(!e.seqext_found && !e.picext_found, "no extensions");
    u[4 + 9 + 3] = 0xB8; /* not a sequence header any more */
    hdv_error_analyze(&st, u, 1, VPID, &e);
    CHECK(!e.seq_found, "no sequence header");

    /* the extensions: 720p59.94 progressive, then 1080i29.97 top field first */
    uint8_t u2[2 * 188];
    char lab[24];
    int num, den;
    hdv_error_init(&st);
    ext_unit(u2, 1280, 720, 7, 1, 0, 1);
    hdv_error_analyze(&st, u2, 2, VPID, &e);
    CHECK(!e.frame_error && e.pic_type == 1, "a clean I picture");
    CHECK(e.seq_found && e.seq_width == 1280 && e.seq_height == 720 && e.seq_aspect == 3, "720p header");
    CHECK(e.seqext_found && e.seq_progressive == 1 && e.seq_fr_ext_n == 0 && e.seq_fr_ext_d == 0,
          "720p sequence extension across the packet boundary");
    CHECK(e.picext_found && e.pic_structure == 3 && e.pic_progressive == 1 && e.pic_tff == 0,
          "720p picture coding extension");
    pin_vidfmt_rate_from_code(e.seq_frame_rate_code, &num, &den);
    pin_vidfmt_label(e.seq_height, num, den, pin_vidfmt_hdv_interlaced(e.seq_height, e.seq_progressive),
                     lab, sizeof(lab));
    CHECK(!strcmp(lab, "720p59.94"), "720p59.94 label");

    ext_unit(u2, 1440, 1080, 4, 0, 1, 0);
    u2[188 + 4 + 7] = 0x2A;      /* frame_rate_extension_n 1, _d 10 */
    hdv_error_analyze(&st, u2, 2, VPID, &e);
    CHECK(e.seqext_found && e.seq_progressive == 0 && e.seq_fr_ext_n == 1 && e.seq_fr_ext_d == 10,
          "1080i sequence extension");
    CHECK(e.picext_found && e.pic_structure == 3 && e.pic_progressive == 0 && e.pic_tff == 1,
          "1080i picture coding extension: top field first");
    pin_vidfmt_rate_from_code(e.seq_frame_rate_code, &num, &den);
    pin_vidfmt_label(e.seq_height, num, den, pin_vidfmt_hdv_interlaced(e.seq_height, e.seq_progressive),
                     lab, sizeof(lab));
    CHECK(!strcmp(lab, "1080i29.97"), "1080i29.97 label");
}

#ifdef TEST_DATA_DIR
static void test_fixture(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/hdv.ts", TEST_DATA_DIR);
    FILE *fp = fopen(path, "rb");
    if (!fp) { pin_test_have_fixture(path); return; } /* made at test time: SKIP, or fail in CI */
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
    CHECK(e.seq_width == 1440 && e.seq_height == 1080 && e.seq_aspect == 3, "fixture is 1440x1080 16:9");
    CHECK(e.seqext_found && !e.seq_progressive, "fixture is interlaced");
    CHECK(e.picext_found && e.pic_tff && !e.pic_progressive, "fixture is top field first");
    int num, den;
    char lab[24];
    pin_vidfmt_rate_from_code(e.seq_frame_rate_code, &num, &den);
    pin_vidfmt_label(e.seq_height, num, den, pin_vidfmt_hdv_interlaced(e.seq_height, e.seq_progressive),
                     lab, sizeof(lab));
    printf("hdv.ts: %s\n", lab);
    CHECK(!strcmp(lab, "1080i25"), "fixture label");
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
