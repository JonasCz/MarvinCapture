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

/*
 * Loopback: packetize the fixtures with pout_cip, push the packets through pout_out_file (EP 0x88
 * framing -> dv_reassembler) and compare what comes out with what went in. The receive side is the
 * independent checker, so this is the proof the packet stream is what a capture program accepts.
 *
 *  - DV: the output must equal the input byte for byte (PAL and NTSC).
 *  - HDV: the reassembler writes nothing until a video picture with a sequence header (a GOP
 *    start) arrives, then replays PAT/PMT in front, and drops the picture still in progress at the
 *    end. So the output is [PAT, PMT, input from some GOP start k up to a picture boundary]: we
 *    compare the packets after the first two against the input from the matching offset.
 */

#include "pout_cip.h"
#include "pout_out.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    size_t cap = 1 << 20, n = 0;
    uint8_t *b = (uint8_t *)malloc(cap);
    size_t k;
    while ((k = fread(b + n, 1, cap - n, f)) > 0) {
        n += k;
        if (n == cap)
            b = (uint8_t *)realloc(b, cap *= 2);
    }
    fclose(f);
    *len = n;
    return b;
}

static char tmp_path[1024];
static const char *tmp_name(const char *suffix)
{
    const char *d = getenv("TMPDIR");
    snprintf(tmp_path, sizeof tmp_path, "%s/test_pout_loopback_%d%s", d && *d ? d : ".", (int)(size_t)&failures & 0xFFF, suffix);
    return tmp_path;
}

static int run_dv(const char *dir, const char *name, int pal)
{
    char in[1024];
    snprintf(in, sizeof in, "%s/%s", dir, name);
    size_t len;
    uint8_t *src = slurp(in, &len);
    if (!src)
        return 77;
    size_t fsz = pal ? 144000 : 120000;
    CHECK(len % fsz == 0 && len > 0);
    const char *out_path = tmp_name(".dv");
    char err[256] = "";
    pout_out_t *o = pout_out_file_create(out_path, err, sizeof err);
    CHECK(o != NULL);
    if (!o) {
        fprintf(stderr, "%s\n", err);
        free(src);
        return 1;
    }
    CHECK(o->start(o) == 0);
    pout_cip_t c;
    pout_cip_init(&c, POUT_CIP_DV, pal, 1);
    pout_cip_set_cycle(&c, 0);
    pout_cip_blank(&c, pout_out_send, o); /* idle cycles before the first frame, as on a real bus */
    for (size_t off = 0; off + fsz <= len; off += fsz)
        CHECK(pout_cip_frame(&c, src + off, fsz, pout_out_send, o) > 0);
    o->stop(o);
    o->destroy(o);
    size_t olen;
    uint8_t *got = slurp(out_path, &olen);
    CHECK(got != NULL);
    CHECK(olen == len);
    CHECK(got && olen == len && memcmp(got, src, len) == 0);
    remove(out_path);
    free(got);
    free(src);
    return 0;
}

static int run_hdv(const char *dir)
{
    char in[1024];
    snprintf(in, sizeof in, "%s/hdv.ts", dir);
    size_t len;
    uint8_t *src = slurp(in, &len);
    if (!src)
        return 77;
    CHECK(len % 188 == 0);
    size_t npk = len / 188;
    const char *out_path = tmp_name(".ts");
    char err[256] = "";
    pout_out_t *o = pout_out_file_create(out_path, err, sizeof err);
    CHECK(o != NULL);
    if (!o) {
        free(src);
        return 1;
    }
    CHECK(o->start(o) == 0);
    pout_cip_t c;
    pout_cip_init(&c, POUT_CIP_HDV, 1, 1);
    pout_cip_set_cycle(&c, 0);
    /* a frame period of 25 fps holds at most 960 source packets; the fixture's rate is ~ 25 Mbit/s
     * at most, slice it by packet count (ragged on purpose) */
    for (size_t i = 0, k = 0; i < npk;) {
        size_t n = 40 + (k++ * 37) % 300;
        if (n > npk - i)
            n = npk - i;
        CHECK(pout_cip_frame(&c, src + i * 188, n * 188, pout_out_send, o) > 0);
        i += n;
    }
    o->stop(o);
    o->destroy(o);
    size_t olen;
    uint8_t *got = slurp(out_path, &olen);
    CHECK(got != NULL && olen % 188 == 0 && olen > 0);
    if (got && olen % 188 == 0 && olen > 0) {
        size_t m = olen / 188;
        CHECK(m >= 3 && m <= npk + 2);
        /* leading PAT (PID 0) and PMT replay, then input from the first GOP start */
        CHECK(got[0] == 0x47 && (got[1] & 0x1F) == 0 && got[2] == 0);
        CHECK(m > 2 && got[188] == 0x47);
        size_t k = npk;
        for (size_t i = 0; i < npk && m > 2; i++)
            if (memcmp(src + i * 188, got + 2 * 188, 188) == 0) {
                k = i;
                break;
            }
        CHECK(k < npk);
        if (k < npk) {
            size_t rest = m - 2;
            CHECK(k + rest <= npk);
            CHECK(k + rest <= npk && memcmp(src + k * 188, got + 2 * 188, rest * 188) == 0);
            /* most of the stream must survive: only the lead-in before the first GOP and the last picture go */
            CHECK(rest * 2 > npk);
            fprintf(stderr, "hdv: %zu of %zu TS packets, from input packet %zu\n", rest, npk, k);
        }
    }
    remove(out_path);
    free(got);
    free(src);
    return 0;
}

int main(void)
{
    const char *dir = getenv("PIN_TEST_DATA");
    if (!dir || !*dir) {
        fprintf(stderr, "PIN_TEST_DATA not set, skipping\n");
        return 77;
    }
    int r1 = run_dv(dir, "dv-pal.dv", 1);
    int r2 = run_dv(dir, "dv-ntsc.dv", 0);
    int r3 = run_hdv(dir);
    if (r1 == 77 || r2 == 77 || r3 == 77) {
        fprintf(stderr, "fixture missing, skipping\n");
        return 77;
    }
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_pout_loopback: ok\n");
    return 0;
}
