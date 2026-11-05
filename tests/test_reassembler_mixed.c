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
 * dv_reassembler on tapes that mix formats: the EP 0x88 dumps make_ep88 makes of
 * fixtures back to back (tests/make_fixtures.cmake), fed in USB-sized chunks.
 *   - PAL then NTSC: the output is the two .dv files again, byte for byte (each
 *     frame at its own size from the first NTSC frame on, none padded);
 *   - DV, HDV, DV: the DV frames of both pieces whole, and between them the TS from
 *     PAT, PMT and a picture with a sequence header; two format switches;
 *   - a burst of HDV packets too short to switch: dropped, the DV around it whole;
 *   - HDV whose video PID changes (and the PMT with it): the new PID is followed,
 *     its first picture behind a fresh PAT/PMT and with a sequence header.
 *
 * Usage: test_reassembler_mixed <data dir>
 */

#include "dv_reassembler.h"
#include "test_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1); \
        } \
    } while (0)

#define TS 188

typedef struct {
    uint8_t *data;
    size_t len;
} buf_t;

static void buf_add(buf_t *b, const uint8_t *d, size_t n)
{
    b->data = realloc(b->data, b->len + n);
    CHECK(b->data);
    memcpy(b->data + b->len, d, n);
    b->len += n;
}

static buf_t read_file(const char *dir, const char *name)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    buf_t b = { NULL, 0 };
    FILE *f = fopen(path, "rb");
    CHECK(f);
    uint8_t chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        buf_add(&b, chunk, n);
    fclose(f);
    return b;
}

/* What the reassembler handed out: the bytes, and each unit's length and kind. */
typedef struct {
    buf_t out;
    size_t unit_len[4096];
    int unit_hdv[4096];
    int units;
} sink_t;

static int on_write(const uint8_t *data, size_t len, void *user)
{
    sink_t *k = user;
    buf_add(&k->out, data, len);
    CHECK(k->units < 4096);
    k->unit_len[k->units] = len;
    k->unit_hdv[k->units] = data[0] == 0x47 && len % TS == 0;
    k->units++;
    return 0;
}

static void reassemble(const char *dir, const char *dump, sink_t *k, dv_reassembler_t *r)
{
    buf_t in = read_file(dir, dump);
    memset(k, 0, sizeof(*k));
    dv_output_t out = { .write = on_write, .on_unit = NULL, .user = k };
    CHECK(dv_reassembler_init(r, &out) == 0);
    for (size_t off = 0; off < in.len; off += 16384) /* as the USB read loop does */
        CHECK(dv_reassembler_feed(r, in.data + off, in.len - off < 16384 ? in.len - off : 16384) == 0);
    dv_reassembler_finish(r);
    free(in.data);
}

static int pkt_pid(const uint8_t *p) { return ((p[1] & 0x1F) << 8) | p[2]; }

/* a video PES start whose payload carries a sequence header */
static int gop_start(const uint8_t *p)
{
    if (!(p[1] & 0x40) || !(p[3] & 0x10))
        return 0;
    size_t po = (p[3] & 0x20) ? 5u + p[4] : 4u;
    if (po + 4 > TS || p[po] || p[po + 1] || p[po + 2] != 1 || (p[po + 3] & 0xF0) != 0xE0)
        return 0;
    for (size_t i = po; i + 4 <= TS; i++)
        if (!p[i] && !p[i + 1] && p[i + 2] == 1 && p[i + 3] == 0xB3)
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const char *dir = argv[1];
    const char *need[] = { "ep88-pal-ntsc.bin", "ep88-dv-hdv-dv.bin", "ep88-dv-stray.bin",
                           "ep88-hdv-pid.bin" };
    for (size_t i = 0; i < sizeof(need) / sizeof(need[0]); i++) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir, need[i]);
        if (!pin_test_have_fixture(path))
            return 0;
    }
    buf_t pal = read_file(dir, "dv-pal.dv"), ntsc = read_file(dir, "dv-ntsc.dv"),
          pal32 = read_file(dir, "dv-pal-32k.dv");
    static sink_t k;
    static dv_reassembler_t r;

    /* PAL then NTSC: both files again, every frame at its own size */
    reassemble(dir, "ep88-pal-ntsc.bin", &k, &r);
    printf("pal-ntsc: %d frames, %lu sequences dropped, %lu standard switch(es)\n", k.units,
           r.sequences_dropped, r.std_switches);
    CHECK(k.out.len == pal.len + ntsc.len);
    CHECK(!memcmp(k.out.data, pal.data, pal.len));
    CHECK(!memcmp(k.out.data + pal.len, ntsc.data, ntsc.len));
    CHECK(r.sequences_dropped == 0 && r.std_switches == 1 && r.format_switches == 0);
    free(k.out.data);

    /* DV, HDV, DV */
    reassemble(dir, "ep88-dv-hdv-dv.bin", &k, &r);
    printf("dv-hdv-dv: %d units, %lu format switch(es)\n", k.units, r.format_switches);
    CHECK(r.format_switches == 2);
    CHECK(k.out.len > pal32.len + ntsc.len);
    CHECK(!memcmp(k.out.data, pal32.data, pal32.len));                       /* every PAL frame */
    CHECK(!memcmp(k.out.data + k.out.len - ntsc.len, ntsc.data, ntsc.len));  /* every NTSC frame */
    int u = 0;
    for (size_t at = 0; at < pal32.len; u++)
        at += k.unit_len[u];
    CHECK(k.unit_hdv[u]);
    const uint8_t *ts = k.out.data + pal32.len;
    size_t ts_len = k.out.len - pal32.len - ntsc.len;
    CHECK(ts_len % TS == 0 && ts_len > 3 * TS);
    CHECK(pkt_pid(ts) == 0 && pkt_pid(ts + TS) == 0x81);  /* PAT, PMT (the fixture's PMT PID) */
    CHECK(pkt_pid(ts + 2 * TS) == 0x810 && gop_start(ts + 2 * TS));
    for (size_t i = 0; i < ts_len; i += TS)
        CHECK(ts[i] == 0x47);
    free(k.out.data);

    /* a burst of HDV too short to switch: dropped, the DV on both sides whole */
    reassemble(dir, "ep88-dv-stray.bin", &k, &r);
    printf("dv-stray: %d frames, %lu other-format packets dropped\n", k.units, r.fmt_dropped_packets);
    CHECK(r.format_switches == 0 && r.fmt_dropped_packets > 0);
    CHECK(k.out.len == 2 * pal32.len);
    CHECK(!memcmp(k.out.data, pal32.data, pal32.len) && !memcmp(k.out.data + pal32.len, pal32.data, pal32.len));
    free(k.out.data);

    /* HDV, video PID 0x810 then 0x100 */
    reassemble(dir, "ep88-hdv-pid.bin", &k, &r);
    printf("hdv-pid: %d units, %lu PID switch(es), %lu overflow(s)\n", k.units, r.ts_pid_switches,
           r.ts_pic_overflows);
    CHECK(r.ts_pid_switches == 1 && r.ts_pic_overflows == 0);
    size_t first_new = 0;
    for (size_t i = 0; i < k.out.len; i += TS) {
        if (pkt_pid(k.out.data + i) == 0x100) {
            first_new = i;
            break;
        }
    }
    CHECK(first_new >= 2 * TS);
    CHECK(pkt_pid(k.out.data + first_new - 2 * TS) == 0);       /* the new PAT */
    CHECK(pkt_pid(k.out.data + first_new - TS) == 0x1000);      /* the new PMT */
    CHECK(gop_start(k.out.data + first_new));
    free(k.out.data);

    free(pal.data);
    free(ntsc.data);
    free(pal32.data);
    printf("OK\n");
    return 0;
}
