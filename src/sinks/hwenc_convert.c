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

/* Hand-written YUY2 / 4:2:2 / 4:1:1 / 4:2:0 -> NV12 / YUV420P; see hwenc_convert.h. */

#include "hwenc_convert.h"

#include <libavutil/pixfmt.h>
#include <string.h>

void pin_hwenc_yuyv_to_422p(const uint8_t *yuyv, int width, int height, AVFrame *dst)
{
    for (int y = 0; y < height; y++) {
        const uint8_t *src = yuyv + (size_t)y * width * 2;
        uint8_t *py = dst->data[0] + (size_t)y * dst->linesize[0];
        uint8_t *pu = dst->data[1] + (size_t)y * dst->linesize[1];
        uint8_t *pv = dst->data[2] + (size_t)y * dst->linesize[2];
        for (int x = 0; x < width / 2; x++) {
            py[2 * x] = src[4 * x + 0];
            pu[x] = src[4 * x + 1];
            py[2 * x + 1] = src[4 * x + 2];
            pv[x] = src[4 * x + 3];
        }
    }
}

/* Source chroma rows that make output chroma row c (of height / 2 rows). */
static void source_rows(int c, int height, int field_aware, int *r0, int *r1)
{
    if (field_aware) {
        int k = c >> 1, f = c & 1;
        *r0 = 4 * k + f;
        *r1 = 4 * k + 2 + f;
    } else {
        *r0 = 2 * c;
        *r1 = 2 * c + 1;
    }
    if (*r1 >= height)
        *r1 = *r0;
}

/* Writes one output chroma row (ccw samples of U and of V) into dst's row c. */
static void store_chroma(AVFrame *dst, int c, const uint8_t *u, const uint8_t *v, int ccw)
{
    if (dst->format == AV_PIX_FMT_NV12) {
        uint8_t *uv = dst->data[1] + (size_t)c * dst->linesize[1];
        for (int i = 0; i < ccw; i++) {
            uv[2 * i] = u[i];
            uv[2 * i + 1] = v[i];
        }
    } else {
        memcpy(dst->data[1] + (size_t)c * dst->linesize[1], u, (size_t)ccw);
        memcpy(dst->data[2] + (size_t)c * dst->linesize[2], v, (size_t)ccw);
    }
}

/* 4:1:1 row (n samples) to 4:2:0 width (2n): the 4:1:1 sample sits on luma 4i, the 4:2:0
 * ones on luma 2j (left-sited, as in MPEG-2), so every other output sample is the input
 * sample itself and the ones between are the mean of their neighbours. */
static void double_row(const uint8_t *in, int n, uint8_t *out)
{
    for (int i = 0; i < n; i++) {
        int next = i + 1 < n ? in[i + 1] : in[i];
        out[2 * i] = in[i];
        out[2 * i + 1] = (uint8_t)((in[i] + next + 1) >> 1);
    }
}

int pin_hwenc_to_420(const AVFrame *src, AVFrame *dst, int field_aware)
{
    const int w = src->width, h = src->height;
    if (w <= 0 || h <= 0 || (w & 3) || (h & 3) || dst->width != w || dst->height != h)
        return -1;
    if (dst->format != AV_PIX_FMT_NV12 && dst->format != AV_PIX_FMT_YUV420P)
        return -1;
    int hshift;            /* source chroma width = w >> hshift */
    int vfull;             /* source chroma has every row (4:2:2, 4:1:1) */
    switch (src->format) {
    case AV_PIX_FMT_YUV422P: hshift = 1; vfull = 1; break;
    case AV_PIX_FMT_YUV411P: hshift = 2; vfull = 1; break;
    case AV_PIX_FMT_YUV420P: hshift = 1; vfull = 0; break;
    default: return -1;
    }

    for (int y = 0; y < h; y++)
        memcpy(dst->data[0] + (size_t)y * dst->linesize[0],
               src->data[0] + (size_t)y * src->linesize[0], (size_t)w);

    const int scw = w >> hshift;    /* source chroma width */
    const int ccw = w / 2;          /* output chroma width */
    uint8_t ut[1024], vt[1024], ud[1024], vd[1024];
    if (ccw > (int)sizeof(ud))
        return -1;
    for (int c = 0; c < h / 2; c++) {
        const uint8_t *u, *v;
        if (!vfull) {
            u = src->data[1] + (size_t)c * src->linesize[1];
            v = src->data[2] + (size_t)c * src->linesize[2];
        } else {
            int r0, r1;
            source_rows(c, h, field_aware, &r0, &r1);
            const uint8_t *u0 = src->data[1] + (size_t)r0 * src->linesize[1];
            const uint8_t *u1 = src->data[1] + (size_t)r1 * src->linesize[1];
            const uint8_t *v0 = src->data[2] + (size_t)r0 * src->linesize[2];
            const uint8_t *v1 = src->data[2] + (size_t)r1 * src->linesize[2];
            for (int i = 0; i < scw; i++) {
                ut[i] = (uint8_t)((u0[i] + u1[i] + 1) >> 1);
                vt[i] = (uint8_t)((v0[i] + v1[i] + 1) >> 1);
            }
            u = ut;
            v = vt;
        }
        if (hshift == 2) {
            double_row(u, scw, ud);
            double_row(v, scw, vd);
            u = ud;
            v = vd;
        }
        store_chroma(dst, c, u, v, ccw);
    }
    return 0;
}
