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

/* pout_tape: the fixtures joined with themselves, mixed media, HDV slices,
 * timecode search, and small synthetic DV files in a temp dir. */

#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include "pout_tape.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_failures = 0;
#define CHECK(cond, msg)                                                         \
    do {                                                                         \
        if (!(cond)) {                                                           \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                 \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

static char g_dir[512];

static void fixture(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s/%s", g_dir, name);
}

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc((size_t)sz + 1);
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static int tc_is(pout_tape_t *t, int64_t frame, int h, int m, int s, int f)
{
    pin_tc_t tc;
    if (pout_tape_tc_of_frame(t, frame, &tc) != 0)
        return 0;
    return tc.h == h && tc.m == m && tc.s == s && tc.f == f;
}

static int64_t frame_of(pout_tape_t *t, int h, int m, int s, int f)
{
    pin_tc_t tc = {h, m, s, f, ':'};
    return pout_tape_frame_of_tc(t, &tc);
}

static pout_tape_t *open_n(const char *const *p, int n, char *err, size_t cap)
{
    pout_tape_t *t = NULL;
    if (pout_tape_open(&t, p, n, err, cap) != 0)
        return NULL;
    return t;
}

/* DV: a file joined with itself k times; bytes, counts, timecode */
static void test_dv(const char *name, int is625, int64_t nframes, int k)
{
    char path[600], err[256];
    fixture(path, sizeof(path), name);
    const char *paths[3] = {path, path, path};
    size_t fsz;
    uint8_t *file = slurp(path, &fsz);
    CHECK(file != NULL, "fixture readable");
    if (!file)
        return;
    pout_tape_t *t = open_n(paths, k, err, sizeof(err));
    CHECK(t != NULL, err);
    if (!t) {
        free(file);
        return;
    }
    size_t fb = is625 ? 144000 : 120000;
    CHECK(pout_tape_media(t) == POUT_MEDIA_DV, "media dv");
    CHECK(pout_tape_is_625(t) == is625, "standard");
    CHECK(pout_tape_frames(t) == nframes * k, "frame count");
    CHECK(pout_tape_max_frame_bytes(t) == fb, "max frame bytes");
    CHECK(pout_tape_file_count(t) == k, "file count");
    CHECK(pout_tape_fps(t) > 24.9 && pout_tape_fps(t) < 30.1, "fps");

    uint8_t *buf = (uint8_t *)malloc(fb);
    for (int64_t i = 0; i < nframes * k; i++) {
        size_t len = 0;
        if (pout_tape_read_frame(t, i, buf, fb, &len) != 0 || len != fb ||
            memcmp(buf, file + (size_t)(i % nframes) * fb, fb) != 0) {
            CHECK(0, "read_frame bytes equal the file");
            break;
        }
    }
    size_t len;
    CHECK(pout_tape_read_frame(t, nframes * k, buf, fb, &len) != 0, "read past the end fails");
    CHECK(pout_tape_read_frame(t, 0, buf, fb - 1, &len) != 0, "small buffer fails");

    /* the fixtures' timecode starts at 00:10:00:00, each file again */
    int nom = is625 ? 25 : 30;
    CHECK(tc_is(t, 0, 0, 10, 0, 0), "first timecode");
    CHECK(tc_is(t, nframes - 1, 0, 10, 0, 0) == 0 || nframes == 1, "last frame differs from first");
    CHECK(tc_is(t, nframes - 1, 0, 10, (int)((nframes - 1) / nom), (int)((nframes - 1) % nom)), "last timecode");
    CHECK(k == 1 || tc_is(t, nframes, 0, 10, 0, 0), "second file restarts at 00:10:00:00");
    for (int64_t i = 0; i < nframes * k; i++) {
        pin_tc_t tc;
        pout_tape_tc_of_frame(t, i, &tc);
        if (pout_tape_frame_of_tc(t, &tc) != i % nframes) {
            CHECK(0, "frame_of_tc(tc_of_frame(i)) is the first occurrence");
            break;
        }
    }
    CHECK(frame_of(t, 0, 20, 0, 0) == -1, "timecode not on the tape");
    CHECK(frame_of(t, 0, 0, 0, 0) == -1, "timecode before the tape");
    int idx = -1;
    CHECK(pout_tape_file_of_frame(t, nframes * (k - 1), &idx) == 0 && idx == k - 1, "file_of_frame");
    CHECK(pout_tape_file_of_frame(t, -1, &idx) != 0, "file_of_frame range");
    CHECK(pout_tape_file_path(t, 0) && strcmp(pout_tape_file_path(t, 0), path) == 0, "file path");
    CHECK(pout_tape_file_path(t, k) == NULL, "file path range");
    CHECK(pout_tape_play_start(t, 7) == 7, "DV play_start is the frame");

    char d[128];
    pout_tape_describe(t, d, sizeof(d));
    CHECK(strstr(d, is625 ? "DV PAL" : "DV NTSC") != NULL, d);

    pout_tape_ops_t ops;
    pout_tape_ops(t, &ops);
    pin_tc_t tc;
    CHECK(ops.frames == nframes * k && ops.tc_of_frame(ops.ctx, 0, &tc) == 0 && tc.m == 10, "ops");
    CHECK(ops.frame_of_tc(ops.ctx, &tc) == 0, "ops frame_of_tc");

    free(buf);
    pout_tape_close(t);
    free(file);
}

/* a DV file made of frames [0,10) and [20,30) of dv-pal.dv: a timecode gap */
static void test_dv_gap(void)
{
    char src[600], gap[600], err[256];
    fixture(src, sizeof(src), "dv-pal.dv");
    snprintf(gap, sizeof(gap), "%s/pout_tape_gap.dv", g_dir);
    size_t n;
    uint8_t *b = slurp(src, &n);
    if (!b) {
        CHECK(0, "dv-pal.dv");
        return;
    }
    FILE *f = fopen(gap, "wb");
    if (!f) {
        printf("SKIP gap file cannot be written\n");
        free(b);
        return;
    }
    fwrite(b, 144000, 10, f);
    fwrite(b + 20 * 144000, 144000, 10, f);
    fclose(f);
    free(b);
    const char *p[1] = {gap};
    pout_tape_t *t = open_n(p, 1, err, sizeof(err));
    CHECK(t != NULL, err);
    if (t) {
        CHECK(pout_tape_frames(t) == 20, "gap frames");
        CHECK(frame_of(t, 0, 10, 0, 5) == 5, "before the gap");
        CHECK(frame_of(t, 0, 10, 0, 20) == 10, "after the gap");
        CHECK(tc_is(t, 10, 0, 10, 0, 20), "timecode after the gap");
        CHECK(frame_of(t, 0, 10, 0, 12) == 0, "in the gap: first frame of the file");
        CHECK(frame_of(t, 0, 10, 1, 5) == -1, "past the range");
        pout_tape_close(t);
    }
    remove(gap);
}

/* synthetic DV: header + subcode blocks only, no timecode */
static int write_dv(const char *path, int is625, int frames)
{
    size_t fb = is625 ? 144000 : 120000;
    uint8_t *fr = (uint8_t *)calloc(1, fb);
    if (!fr)
        return -1;
    for (size_t q = 0; q < fb / 12000; q++) {
        uint8_t *s = fr + q * 12000;
        s[0] = 0x1F;
        s[3] = is625 ? 0x80 : 0x00;
        s[80] = 0x3F;
        s[160] = 0x3F;
        s[240] = 0x5F;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(fr);
        return -1;
    }
    for (int i = 0; i < frames; i++)
        fwrite(fr, 1, fb, f);
    fclose(f);
    free(fr);
    return 0;
}

static void test_dv_synth(void)
{
    char a[600], b[600], big[600], err[256];
    snprintf(a, sizeof(a), "%s/pout_tape_s1.dv", g_dir);
    snprintf(b, sizeof(b), "%s/pout_tape_s2.dv", g_dir);
    snprintf(big, sizeof(big), "%s/pout_tape_ntsc.dv", g_dir);
    if (write_dv(a, 1, 10) != 0 || write_dv(b, 1, 20) != 0) {
        printf("SKIP synthetic files cannot be written\n");
        return;
    }
    const char *p[2] = {a, b};
    pout_tape_t *t = open_n(p, 2, err, sizeof(err));
    CHECK(t != NULL, err);
    if (t) {
        pin_tc_t tc;
        CHECK(pout_tape_frames(t) == 30, "synthetic frames");
        CHECK(tc_is(t, 0, 0, 0, 0, 0), "synthesized timecode starts at zero");
        CHECK(tc_is(t, 9, 0, 0, 0, 9), "synthesized, first file end");
        CHECK(tc_is(t, 10, 0, 0, 0, 10), "second file continues the first");
        CHECK(tc_is(t, 29, 0, 0, 1, 4), "synthesized, second file end");
        CHECK(frame_of(t, 0, 0, 1, 0) == 25, "synthesized frame_of_tc");
        pout_tape_tc_of_frame(t, 3, &tc);
        CHECK(tc.sep == ':', "PAL is non drop");
        pout_tape_close(t);
    }

    /* NTSC, sparse (the zero-filled frames need no disk space): drop frame */
    if (write_dv(big, 0, 1) == 0) {
        FILE *f = fopen(big, "r+b");
        if (f) {
            fseek(f, (long)(120000L * 1900L - 1), SEEK_SET);
            fputc(0, f);
            fclose(f);
            const char *q[1] = {big};
            t = open_n(q, 1, err, sizeof(err));
            CHECK(t != NULL, err);
            if (t) {
                pin_tc_t tc;
                CHECK(pout_tape_frames(t) == 1900, "ntsc synthetic frames");
                pout_tape_tc_of_frame(t, 1800, &tc);
                CHECK(tc.h == 0 && tc.m == 1 && tc.s == 0 && tc.f == 2 && tc.sep == ';', "drop frame minute");
                CHECK(frame_of(t, 0, 1, 0, 2) == 1800, "drop frame frame_of_tc");
                pout_tape_tc_of_frame(t, 1799, &tc);
                CHECK(tc.m == 0 && tc.s == 59 && tc.f == 29, "last frame before the dropped labels");
                pout_tape_close(t);
            }
        }
    }
    remove(a);
    remove(b);
    remove(big);
}

static void test_mixed(void)
{
    char pal[600], ntsc[600], hdv[600], hdv720[600], err[256];
    fixture(pal, sizeof(pal), "dv-pal.dv");
    fixture(ntsc, sizeof(ntsc), "dv-ntsc.dv");
    fixture(hdv, sizeof(hdv), "hdv.ts");
    fixture(hdv720, sizeof(hdv720), "hdv-720p.ts");
    const char *pn[2] = {pal, ntsc}, *dh[2] = {pal, hdv}, *hh[2] = {hdv, hdv720}, *hd[3] = {hdv, hdv, pal};
    pout_tape_t *t = NULL;
    err[0] = 0;
    CHECK(pout_tape_open(&t, pn, 2, err, sizeof(err)) != 0 && t == NULL, "PAL + NTSC rejected");
    CHECK(strstr(err, "dv-ntsc.dv") != NULL, err);
    err[0] = 0;
    CHECK(pout_tape_open(&t, dh, 2, err, sizeof(err)) != 0, "DV + HDV rejected");
    CHECK(strstr(err, "hdv.ts") != NULL, err);
    err[0] = 0;
    CHECK(pout_tape_open(&t, hh, 2, err, sizeof(err)) != 0, "1080i + 720p rejected");
    CHECK(strstr(err, "hdv-720p.ts") != NULL, err);
    err[0] = 0;
    CHECK(pout_tape_open(&t, hd, 3, err, sizeof(err)) != 0, "HDV, HDV, DV rejected");
    CHECK(strstr(err, "dv-pal.dv") != NULL, err);

    char empty[600], nofile[600];
    snprintf(empty, sizeof(empty), "%s/pout_tape_empty.dv", g_dir);
    snprintf(nofile, sizeof(nofile), "%s/pout_tape_nonexistent.dv", g_dir);
    FILE *f = fopen(empty, "wb");
    if (f) {
        fclose(f);
        const char *e[1] = {empty};
        err[0] = 0;
        CHECK(pout_tape_open(&t, e, 1, err, sizeof(err)) != 0 && strstr(err, "pout_tape_empty") != NULL, "empty file");
        remove(empty);
    }
    const char *nf[1] = {nofile};
    err[0] = 0;
    CHECK(pout_tape_open(&t, nf, 1, err, sizeof(err)) != 0 && strstr(err, "pout_tape_nonexistent") != NULL, "missing file");
    CHECK(pout_tape_open(&t, nf, 0, err, sizeof(err)) != 0, "no files");
}

/* HDV: the frame slices hold every packet once, in order */
static void test_hdv(const char *name, int is625)
{
    char path[600], err[256];
    fixture(path, sizeof(path), name);
    size_t fsz;
    uint8_t *file = slurp(path, &fsz);
    CHECK(file != NULL, "hdv fixture readable");
    if (!file)
        return;
    size_t npk = fsz / 188;
    CHECK(fsz % 188 == 0, "fixture is whole packets");
    const char *paths[3] = {path, path, path};
    int64_t single = 0;
    for (int k = 1; k <= 3; k++) {
        pout_tape_t *t = open_n(paths, k, err, sizeof(err));
        CHECK(t != NULL, err);
        if (!t)
            break;
        CHECK(pout_tape_media(t) == POUT_MEDIA_HDV, "media hdv");
        CHECK(pout_tape_is_625(t) == is625, "hdv standard");
        int64_t nf = pout_tape_frames(t);
        if (k == 1) {
            single = nf;
            printf("%s: %lld frames\n", name, (long long)nf);
            CHECK(nf > 5, "some frames");
        }
        CHECK(nf == single * k, "frames add up over files");
        size_t mx = pout_tape_max_frame_bytes(t);
        uint8_t *buf = (uint8_t *)malloc(mx + 188);
        size_t pos = 0;
        int ok = 1;
        for (int64_t i = 0; i < nf && ok; i++) {
            size_t len = 99;
            if (pout_tape_read_frame(t, i, buf, mx, &len) != 0 || len % 188 || len > mx) {
                ok = 0;
                break;
            }
            for (size_t o = 0; o < len; o += 188, pos += 188) {
                if (pos >= fsz * (size_t)k || memcmp(buf + o, file + pos % fsz, 188) != 0 || buf[o] != 0x47) {
                    ok = 0;
                    break;
                }
            }
        }
        CHECK(ok, "slices are the TS packets in order");
        CHECK(pos == npk * 188 * (size_t)k, "slices cover every packet exactly once");

        /* timecode round trips: first occurrence in tape order */
        int bad = 0;
        for (int64_t i = 0; i < nf; i++) {
            pin_tc_t tc;
            if (pout_tape_tc_of_frame(t, i, &tc) != 0 || pout_tape_frame_of_tc(t, &tc) != i % single)
                bad++;
        }
        CHECK(bad == 0, "frame_of_tc(tc_of_frame(i)) round trip");
        pin_tc_t t0, t1;
        pout_tape_tc_of_frame(t, 0, &t0);
        pout_tape_tc_of_frame(t, 1, &t1);
        CHECK(t0.h == 0 && (t0.m == 10 || t0.m == 9), "GOP timecode near 00:10:00:00");
        CHECK(t1.s * 100 + t1.f != t0.s * 100 + t0.f, "timecode advances");
        if (k == 1)
            printf("%s: tc[0]=%02d:%02d:%02d:%02d\n", name, t0.h, t0.m, t0.s, t0.f);

        /* play_start: at or before, idempotent, stays in the file */
        for (int64_t i = 0; i < nf; i++) {
            int64_t ps = pout_tape_play_start(t, i);
            int f1 = -1, f2 = -1;
            pout_tape_file_of_frame(t, i, &f1);
            pout_tape_file_of_frame(t, ps, &f2);
            if (ps > i || ps < 0 || f1 != f2 || pout_tape_play_start(t, ps) != ps) {
                CHECK(0, "play_start");
                break;
            }
        }
        CHECK(pout_tape_play_start(t, single) == single || k == 1, "second file's first GOP");
        char d[128];
        pout_tape_describe(t, d, sizeof(d));
        CHECK(strstr(d, "HDV") != NULL, d);
        if (k == 1)
            printf("%s\n", d);
        free(buf);
        pout_tape_close(t);
    }
    free(file);
}

/* M2TS (192 byte packets) of hdv.ts: same slices, prefix dropped */
static void test_m2ts(void)
{
    char path[600], m2[600], err[256];
    fixture(path, sizeof(path), "hdv.ts");
    snprintf(m2, sizeof(m2), "%s/pout_tape.m2ts", g_dir);
    size_t n;
    uint8_t *b = slurp(path, &n);
    FILE *f = b ? fopen(m2, "wb") : NULL;
    if (!f) {
        free(b);
        return;
    }
    for (size_t o = 0; o + 188 <= n; o += 188) {
        const uint8_t pre[4] = {0, 0, 0, 0};
        fwrite(pre, 1, 4, f);
        fwrite(b + o, 1, 188, f);
    }
    fclose(f);
    const char *a[1] = {path}, *c[1] = {m2};
    pout_tape_t *t1 = open_n(a, 1, err, sizeof(err)), *t2 = open_n(c, 1, err, sizeof(err));
    CHECK(t1 && t2, err);
    if (t1 && t2) {
        CHECK(pout_tape_frames(t1) == pout_tape_frames(t2), "m2ts frame count");
        size_t mx = pout_tape_max_frame_bytes(t1);
        uint8_t *x = (uint8_t *)malloc(mx), *y = (uint8_t *)malloc(mx);
        int same = 1;
        for (int64_t i = 0; i < pout_tape_frames(t1) && same; i++) {
            size_t lx = 0, ly = 1;
            if (pout_tape_read_frame(t1, i, x, mx, &lx) || pout_tape_read_frame(t2, i, y, mx, &ly) || lx != ly ||
                memcmp(x, y, lx))
                same = 0;
        }
        CHECK(same, "m2ts slices equal ts slices");
        free(x);
        free(y);
    }
    pout_tape_close(t1);
    pout_tape_close(t2);
    free(b);
    remove(m2);
}

int main(void)
{
    const char *d = getenv("PIN_TEST_DATA");
    char probe[600];
    FILE *f;
    if (!d || !*d) {
        printf("SKIP: PIN_TEST_DATA not set\n");
        return 77;
    }
    snprintf(g_dir, sizeof(g_dir), "%s", d);
    const char *need[] = {"dv-pal.dv", "dv-ntsc.dv", "hdv.ts", "hdv-720p.ts"};
    for (int i = 0; i < 4; i++) {
        fixture(probe, sizeof(probe), need[i]);
        f = fopen(probe, "rb");
        if (!f) {
            printf("SKIP: fixture %s missing\n", need[i]);
            return 77;
        }
        fclose(f);
    }
    test_dv("dv-pal.dv", 1, 50, 1);
    test_dv("dv-pal.dv", 1, 50, 3);
    test_dv("dv-ntsc.dv", 0, 30, 2);
    test_dv_gap();
    test_dv_synth();
    test_mixed();
    test_hdv("hdv.ts", 1);
    test_hdv("hdv-720p.ts", 0);
    test_m2ts();
    if (g_failures) {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
