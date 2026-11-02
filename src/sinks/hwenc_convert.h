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
 * The few pixel format conversions sink_hwenc.c needs, written out by hand
 * instead of pulling in libswscale: what arrives is analog YUY2 (4:2:2), DV
 * PAL (4:2:0) or DV NTSC (4:1:1), and every hardware encoder takes 4:2:0,
 * as NV12 (semi-planar) or YUV420P (planar).
 *
 * Interlaced pictures need their chroma subsampled per field. In 4:2:0 that
 * is the usual "interlaced" layout (MPEG-2, DV PAL): chroma rows alternate
 * between the fields, row 2k from the top field's luma rows 4k and 4k+2,
 * row 2k+1 from the bottom field's 4k+1 and 4k+3. Averaging neighbouring
 * frame rows instead would mix the two fields, 1/50 s apart, into every
 * chroma sample: colour smears on motion and the result can no longer be
 * deinterlaced cleanly. A progressive picture (bwdif's output) averages rows
 * 2c and 2c+1.
 */

#ifndef HWENC_CONVERT_H
#define HWENC_CONVERT_H

#include <libavutil/frame.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* YUYV (Y0 U0 Y1 V0) of width x height into dst, an allocated AV_PIX_FMT_YUV422P frame
 * of the same size: a deinterleave, no resampling (sink_ffv1 does the same). */
void pin_hwenc_yuyv_to_422p(const uint8_t *yuyv, int width, int height, AVFrame *dst);

/* src (AV_PIX_FMT_YUV422P, YUV420P or YUV411P) into dst, an allocated AV_PIX_FMT_NV12 or
 * YUV420P frame of the same size. field_aware: src is an interlaced frame, so 4:2:2 and
 * 4:1:1 chroma is subsampled per field (see above); 0 for a progressive one. 4:2:0 input
 * is copied as it is either way (DV PAL's chroma is already per field). 4:1:1 chroma is
 * doubled horizontally (every other sample the mean of its neighbours). Width and height
 * must be multiples of 4. Returns 0, or -1 for formats or sizes it does not handle. */
int pin_hwenc_to_420(const AVFrame *src, AVFrame *dst, int field_aware);

#ifdef __cplusplus
}
#endif

#endif /* HWENC_CONVERT_H */
