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
 * ctest: sink_hwenc's hand-written pixel conversions (hwenc_convert.c) on small synthetic
 * pictures: YUY2 -> 4:2:2, 4:2:2 and 4:1:1 -> 4:2:0 per field (an interlaced frame: chroma
 * rows never mix the fields) and progressively, 4:2:0 copied, into NV12 and YUV420P alike.
 */

#include "test_util.h"
#include "../../src/sinks/hwenc_convert.h"

#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

#include <stdio.h>
#include <string.h>

#define W 16
#define H 8

static AVFrame *frame(enum AVPixelFormat fmt, int w, int h)
{
    AVFrame *f = av_frame_alloc();
    CHECK(f);
    f->format = fmt;
    f->width = w;
    f->height = h;
    CHECK(av_frame_get_buffer(f, 0) >= 0);
    return f;
}

static int out_u(const AVFrame *f, int c, int i)
{
    return f->format == AV_PIX_FMT_NV12 ? f->data[1][c * f->linesize[1] + 2 * i] : f->data[1][c * f->linesize[1] + i];
}

static int out_v(const AVFrame *f, int c, int i)
{
    return f->format == AV_PIX_FMT_NV12 ? f->data[1][c * f->linesize[1] + 2 * i + 1]
                                        : f->data[2][c * f->linesize[2] + i];
}

/* Source chroma: U strongly depends on the field (row parity) and on the row, V on the column. */
static int src_u(int row, int i) { return (row & 1 ? 200 : 40) + row + i; }
static int src_v(int row, int i) { return 100 + 3 * i + (row & 1); }

static void fill(AVFrame *s, int cw, int ch)
{
    for (int y = 0; y < s->height; y++)
        for (int x = 0; x < s->width; x++)
            s->data[0][y * s->linesize[0] + x] = (uint8_t)(x * 7 + y);
    for (int r = 0; r < ch; r++)
        for (int i = 0; i < cw; i++) {
            s->data[1][r * s->linesize[1] + i] = (uint8_t)src_u(r, i);
            s->data[2][r * s->linesize[2] + i] = (uint8_t)src_v(r, i);
        }
}

static void check_luma(const AVFrame *s, const AVFrame *d)
{
    for (int y = 0; y < H; y++)
        CHECK(memcmp(s->data[0] + y * s->linesize[0], d->data[0] + y * d->linesize[0], W) == 0);
}

static int avg(int a, int b) { return (a + b + 1) >> 1; }

/* 4:2:2 and 4:1:1 (full-height chroma) */
static void full_height(enum AVPixelFormat sf, int hshift, enum AVPixelFormat df, int field_aware)
{
    AVFrame *s = frame(sf, W, H), *d = frame(df, W, H);
    int scw = W >> hshift;
    fill(s, scw, H);
    CHECK(pin_hwenc_to_420(s, d, field_aware) == 0);
    check_luma(s, d);
    for (int c = 0; c < H / 2; c++) {
        int r0 = field_aware ? 4 * (c >> 1) + (c & 1) : 2 * c;
        int r1 = r0 + (field_aware ? 2 : 1);
        if (field_aware)
            CHECK((r0 & 1) == (r1 & 1));    /* one field only */
        for (int j = 0; j < W / 2; j++) {
            int eu, ev;
            if (hshift == 1) {
                eu = avg(src_u(r0, j), src_u(r1, j));
                ev = avg(src_v(r0, j), src_v(r1, j));
            } else {
                int i = j / 2, n = i + 1 < scw ? i + 1 : i;
                int u0 = avg(src_u(r0, i), src_u(r1, i)), u1 = avg(src_u(r0, n), src_u(r1, n));
                int v0 = avg(src_v(r0, i), src_v(r1, i)), v1 = avg(src_v(r0, n), src_v(r1, n));
                eu = j & 1 ? avg(u0, u1) : u0;
                ev = j & 1 ? avg(v0, v1) : v0;
            }
            if (out_u(d, c, j) != eu || out_v(d, c, j) != ev) {
                fprintf(stderr, "fmt %d -> %d field_aware %d: row %d col %d: got %d/%d want %d/%d\n", sf, df,
                        field_aware, c, j, out_u(d, c, j), out_v(d, c, j), eu, ev);
                CHECK(0);
            }
        }
    }
    if (field_aware) {
        /* even rows from the top field (U about 40), odd from the bottom (about 200) */
        CHECK(out_u(d, 0, 0) < 100 && out_u(d, 1, 0) > 150 && out_u(d, 2, 0) < 100 && out_u(d, 3, 0) > 150);
    }
    av_frame_free(&s);
    av_frame_free(&d);
}

static void from_420(enum AVPixelFormat df)
{
    AVFrame *s = frame(AV_PIX_FMT_YUV420P, W, H), *d = frame(df, W, H);
    fill(s, W / 2, H / 2);
    for (int fa = 0; fa < 2; fa++) {
        CHECK(pin_hwenc_to_420(s, d, fa) == 0);
        check_luma(s, d);
        for (int c = 0; c < H / 2; c++)
            for (int j = 0; j < W / 2; j++)
                CHECK(out_u(d, c, j) == src_u(c, j) && out_v(d, c, j) == src_v(c, j));
    }
    av_frame_free(&s);
    av_frame_free(&d);
}

static void yuyv(void)
{
    uint8_t buf[W * H * 2];
    for (int i = 0; i < W * H * 2; i++)
        buf[i] = (uint8_t)(i * 13 + 5);
    AVFrame *d = frame(AV_PIX_FMT_YUV422P, W, H);
    pin_hwenc_yuyv_to_422p(buf, W, H, d);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W / 2; x++) {
            const uint8_t *p = buf + y * W * 2 + 4 * x;
            CHECK(d->data[0][y * d->linesize[0] + 2 * x] == p[0]);
            CHECK(d->data[1][y * d->linesize[1] + x] == p[1]);
            CHECK(d->data[0][y * d->linesize[0] + 2 * x + 1] == p[2]);
            CHECK(d->data[2][y * d->linesize[2] + x] == p[3]);
        }
    av_frame_free(&d);
}

static void refuses(void)
{
    AVFrame *s = frame(AV_PIX_FMT_YUV422P, W, H), *d = frame(AV_PIX_FMT_NV12, W, H);
    AVFrame *wrong = frame(AV_PIX_FMT_NV12, W, H + 4);
    AVFrame *odd = frame(AV_PIX_FMT_YUV422P, W, H + 2), *oddd = frame(AV_PIX_FMT_NV12, W, H + 2);
    AVFrame *rgb = frame(AV_PIX_FMT_RGB24, W, H);
    CHECK(pin_hwenc_to_420(s, wrong, 1) == -1);   /* sizes differ */
    CHECK(pin_hwenc_to_420(odd, oddd, 1) == -1);  /* height not a multiple of 4 */
    CHECK(pin_hwenc_to_420(rgb, d, 1) == -1);     /* not a YUV source it knows */
    CHECK(pin_hwenc_to_420(s, s, 1) == -1);       /* not a 4:2:0 target */
    av_frame_free(&s);
    av_frame_free(&d);
    av_frame_free(&wrong);
    av_frame_free(&odd);
    av_frame_free(&oddd);
    av_frame_free(&rgb);
}

int main(void)
{
    const enum AVPixelFormat outs[2] = { AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P };
    for (int o = 0; o < 2; o++) {
        for (int fa = 0; fa < 2; fa++) {
            full_height(AV_PIX_FMT_YUV422P, 1, outs[o], fa);
            full_height(AV_PIX_FMT_YUV411P, 2, outs[o], fa);
        }
        from_420(outs[o]);
    }
    yuyv();
    refuses();
    printf("test_hwenc_convert: ok\n");
    return 0;
}
