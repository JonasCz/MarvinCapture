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

#include "dv_error.h"
#include "dv_subcode.h"
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

/* A clean frame: valid block IDs, STA 0, audio with distinct samples. */
static uint8_t *make_frame(unsigned nseq)
{
    uint8_t *f = calloc(nseq, DV_SEQ_SIZE);
    for (unsigned si = 0; si < nseq; si++) {
        unsigned vdbn = 0, adbn = 0;
        for (unsigned b = 0; b < 150; b++) {
            uint8_t *p = f + si * DV_SEQ_SIZE + b * 80;
            unsigned sct, dbn;
            if (b == 0) { sct = 0; dbn = 0; }
            else if (b < 3) { sct = 1; dbn = b - 1; }
            else if (b < 6) { sct = 2; dbn = b - 3; }
            else if ((b - 6) % 16 == 0) { sct = 3; dbn = adbn++; }
            else { sct = 4; dbn = vdbn++; }
            p[0] = (uint8_t)((sct << 5) | 0x1F);
            p[1] = (uint8_t)((si << 4) | 7);
            p[2] = (uint8_t)dbn;
            if (sct == 3)
                for (int i = 0; i < 36; i++) {
                    p[8 + 2 * i] = (uint8_t)(si + 1);
                    p[9 + 2 * i] = (uint8_t)(i * 3 + dbn + 1);
                }
        }
    }
    return f;
}

static uint8_t *blk(uint8_t *f, unsigned si, unsigned b) { return f + si * DV_SEQ_SIZE + b * 80; }

static void test_synthetic(unsigned nseq)
{
    size_t len = nseq * DV_SEQ_SIZE;
    uint8_t *f = make_frame(nseq);
    dv_frame_errors_t e;
    CHECK(dv_error_analyze(f, len, &e) == 0, "analyze ok");
    CHECK(e.blocks == nseq * 150 && !e.frame_error && !e.frame_dropped, "clean frame");
    CHECK(e.video_blocks == nseq * 135 && e.audio_blocks == nseq * 9, "block counts");
    CHECK(dv_error_analyze(f, len - 1, &e) == -1, "bad length rejected");

    blk(f, 2, 20)[3] = 0x70; /* STA 7: error */
    dv_error_analyze(f, len, &e);
    CHECK(e.frame_error && e.video_err == 1 && e.video_concealed == 0, "STA 7");
    blk(f, 2, 20)[3] = 0x00;
    blk(f, 3, 30)[3] = 0x20; /* STA 2: concealed */
    dv_error_analyze(f, len, &e);
    CHECK(e.frame_error && e.video_concealed == 1 && e.video_err == 0, "STA 2");
    blk(f, 3, 30)[3] = 0x0F; /* QNO nibble must not count */
    dv_error_analyze(f, len, &e);
    CHECK(!e.frame_error, "QNO nibble ignored");

    for (int i = 0; i < 36; i++) { blk(f, 1, 6)[8 + 2 * i] = 0x80; blk(f, 1, 6)[9 + 2 * i] = 0; }
    dv_error_analyze(f, len, &e);
    CHECK(e.frame_error && e.audio_err == 1, "0x8000 audio error fill");
    free(f);

    /* a lost (zero-padded) sequence */
    f = make_frame(nseq);
    memset(f + 4 * DV_SEQ_SIZE, 0, DV_SEQ_SIZE);
    dv_error_analyze(f, len, &e);
    CHECK(e.frame_error && e.missing_blocks == 150 && !e.frame_dropped, "zero-padded sequence");
    memset(f, 0, len); /* everything lost = dropped */
    dv_error_analyze(f, len, &e);
    CHECK(e.frame_error && e.frame_dropped, "all zero = dropped");
    free(f);

    /* mute quirk: some, not all, first-pair audio blocks all zero */
    f = make_frame(nseq);
    memset(blk(f, 0, 6) + 8, 0, 72);
    dv_error_analyze(f, len, &e);
    CHECK(e.frame_error && e.audio_mute == 1, "partial mute");
    free(f);
    /* an all-zero first pair is silence */
    f = make_frame(nseq);
    for (unsigned si = 0; si < nseq; si++)
        for (unsigned j = 0; j < 9; j++)
            memset(blk(f, si, 6 + 16 * j) + 8, 0, 72);
    dv_error_analyze(f, len, &e);
    CHECK(!e.frame_error, "all-silent audio is not an error");
    free(f);
    /* zeros in the second channel pair (4-channel 32 kHz) are normal */
    f = make_frame(nseq);
    for (unsigned si = nseq / 2; si < nseq; si++)
        for (unsigned j = 0; j < 9; j += 2)
            memset(blk(f, si, 6 + 16 * j) + 8, 0, 72);
    dv_error_analyze(f, len, &e);
    CHECK(!e.frame_error, "zeros in the second channel pair are not an error");
    free(f);

    /* Samsung-style full re-encode: every video block STA 14 */
    f = make_frame(nseq);
    for (unsigned si = 0; si < nseq; si++)
        for (unsigned b = 0; b < 150; b++)
            if (blk(f, si, b)[0] >> 5 == 4) blk(f, si, b)[3] = 0xE0;
    dv_error_analyze(f, len, &e);
    CHECK(e.frame_error && e.reencoded, "STA 14 re-encode flagged");
    free(f);
}

#ifdef TEST_DATA_DIR
static void test_fixture(const char *name, unsigned long frame_len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", TEST_DATA_DIR, name);
    FILE *fp = fopen(path, "rb");
    if (!fp) { printf("skip %s (not present)\n", name); return; }
    uint8_t *buf = malloc(frame_len);
    int n = 0, bad = 0;
    while (fread(buf, 1, frame_len, fp) == frame_len) {
        dv_frame_errors_t e;
        dv_error_analyze(buf, frame_len, &e);
        if (e.frame_error) bad++;
        n++;
    }
    CHECK(n > 0, "fixture has frames");
    CHECK(bad == 0, "real camera frames are error-free");
    dv_frame_errors_t e; /* damage the last frame read: one STA nibble */
    buf[7 * 80 + 3] |= 0x70;
    dv_error_analyze(buf, frame_len, &e);
    CHECK(e.frame_error && e.video_err >= 1, "damaged real frame detected");
    free(buf);
    fclose(fp);
}
#endif

int main(void)
{
    test_synthetic(10);
    test_synthetic(12);
#ifdef TEST_DATA_DIR
    test_fixture("dv-ntsc.dv", 120000);
    test_fixture("dv-ntsc-32k.dv", 120000);
#endif
    if (g_failures == 0) printf("test_dv_error: ok\n");
    return g_failures;
}
