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
 * ctest: the two shared stages of the HDV pipeline on their own (hdv_demux.h, hdv_decode.h),
 * as any HDV sink that decodes (sink_hdvmux.c, a future H.264 / HEVC one) uses them:
 *   - a clean stream (hdv-repair.ts, 6 s): every picture in a GOP, every GOP back from the
 *     decoder in order with every picture decoded and ok, the camera clock on each picture
 *     one frame after the one before in display order, ~250 MP2 frames;
 *   - the same with bits flipped inside 20 video packets (no packet error): the GOP comes back
 *     with pictures the decoder concealed (hdv_pic_ok() 0), the packets say nothing;
 *   - runs of TS packets of any length give the same GOPs as whole-file feeding.
 * Usage: test_hdv_stages <hdv-repair.ts>
 */

#include "test_util.h"
#include "../../src/sinks/hdv_demux.h"
#include "../../src/sinks/hdv_decode.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TSP 188

typedef struct {
    hdv_decoder_t *dec;
    int gops, pics, ok, damaged_pkts, audio, out_of_order;
    int64_t last_first_id;
    int64_t tr_sum;
} ctx_t;

static void take(ctx_t *c, hdv_gop_t *g)
{
    if (g->first_id <= c->last_first_id)
        c->out_of_order++;
    c->last_first_id = g->first_id;
    c->gops++;
    c->pics += g->pics.n;
    for (int i = 0; i < g->pics.n; i++) {
        c->ok += hdv_pic_ok(g, i);
        c->damaged_pkts += g->pics.p[i].damaged;
    }
    hdv_gop_free(g);
}

static void on_gop(void *user, hdv_gop_t *g)
{
    ctx_t *c = user;
    CHECK(g->seq.valid && g->seq.width == 1440 && g->seq.height == 1080);
    CHECK(g->seq.fps_num == 25 && g->seq.fps_den == 1);
    hdv_decoder_feed(c->dec, g);
    hdv_gop_t *r;
    while ((r = hdv_decoder_ready(c->dec)) != NULL)
        take(c, r);
}

static void on_audio(void *user, const uint8_t *f, int len, int64_t cam, int sr, int ch, int kbps)
{
    ctx_t *c = user;
    (void)f;
    (void)cam;
    CHECK(len > 0 && sr == 48000 && ch == 2 && kbps == 384);
    c->audio++;
}

static void on_error(void *user, pin_status_t e)
{
    (void)user;
    (void)e;
    CHECK(!"no error expected");
}

static ctx_t run(const uint8_t *ts, size_t n, size_t chunk_packets)
{
    ctx_t c;
    memset(&c, 0, sizeof(c));
    c.last_first_id = -1;
    c.dec = hdv_decoder_create(2);
    CHECK(c.dec);
    hdv_demux_cb_t cb = { on_gop, on_audio, on_error, &c };
    hdv_demux_t *d = hdv_demux_create(&cb, 1);
    CHECK(d);
    for (size_t i = 0; i < n; i += chunk_packets)
        hdv_demux_feed(d, ts + i * TSP, (i + chunk_packets <= n ? chunk_packets : n - i) * TSP);
    hdv_demux_flush(d);
    hdv_decoder_flush(c.dec);
    hdv_gop_t *r;
    while ((r = hdv_decoder_ready(c.dec)) != NULL)
        take(&c, r);
    hdv_demux_free(d);
    hdv_decoder_free(c.dec);
    printf("  %zu-packet units: %d GOPs, %d pictures, %d decoded ok, %d with packet damage, "
           "%d audio frames\n", chunk_packets, c.gops, c.pics, c.ok, c.damaged_pkts, c.audio);
    return c;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    if (!pin_test_have_fixture(argv[1]))
        return 0;
    FILE *f = fopen(argv[1], "rb");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    size_t bytes = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *ts = malloc(bytes);
    CHECK(ts && fread(ts, 1, bytes, f) == bytes);
    fclose(f);
    size_t n = bytes / TSP;

    ctx_t a = run(ts, n, 1), b = run(ts, n, 1000);
    CHECK(a.gops == 13 && a.pics == 150 && a.ok == 150 && a.damaged_pkts == 0);
    CHECK(a.out_of_order == 0);
    CHECK(a.audio >= 245 && a.audio <= 252);
    CHECK(b.gops == a.gops && b.pics == a.pics && b.ok == a.ok && b.audio == a.audio);

    /* bits flipped inside 20 video packets, no packet error: only the decoder sees it */
    size_t k = 0;
    for (size_t i = (n * 2) / 5; i < n && k < 20; i++) {
        uint8_t *q = ts + i * TSP;
        if ((((q[1] & 0x1F) << 8) | q[2]) != 0x810 || (q[1] & 0x40) || (q[3] & 0x20))
            continue;
        for (int j = 30; j < TSP; j += 11)
            q[j] ^= 0xA5;
        k++;
    }
    ctx_t c = run(ts, n, 200);
    CHECK(c.pics == 150 && c.damaged_pkts == 0);
    CHECK(c.ok < 150 && c.ok >= 130);
    free(ts);
    printf("OK\n");
    return 0;
}
