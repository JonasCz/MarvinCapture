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
 * The second stage of the HDV pipeline (hdv_demux.h has the whole picture): every picture of
 * every GOP decoded with FFmpeg's MPEG-2 decoder, one decoder for the stream so open GOPs have
 * their references, error concealment on (a damaged picture comes back concealed and flagged).
 *
 * GOPs go in in order (hdv_decoder_feed()) and come back in order (hdv_decoder_ready()) once
 * the decoder has given back all of their frames: a frame of a later GOP came out (frames come
 * in display order), or the stream ended (hdv_decoder_flush()). A GOP comes back with
 * frames[i] for each picture (NULL: the decoder gave nothing for it, as for the leading B
 * pictures of the first GOP, which have no reference) and hdv_pic_ok() as the decoder's
 * verdict. Slice threads only: no frame-thread delay, the decoder's own reorder delay of one
 * picture is all.
 *
 * Users: sink_hdvmux.c (the check, and the frames for a repair); a future HDV to H.264 /
 * HEVC sink (every frame to the encoder). Not thread-safe: one thread uses a decoder.
 */

#ifndef HDV_DECODE_H
#define HDV_DECODE_H

#include "hdv_demux.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hdv_decoder hdv_decoder_t;

/* threads: slice threads (1 or more). NULL if the decoder does not open. */
hdv_decoder_t *hdv_decoder_create(int threads);
/* Decodes g's pictures; the decoder keeps g until it is ready. */
void hdv_decoder_feed(hdv_decoder_t *d, hdv_gop_t *g);
/* The next GOP whose frames are all back (the caller takes it), or NULL. */
hdv_gop_t *hdv_decoder_ready(hdv_decoder_t *d);
/* The end of the stream: the decoder's last frames; every GOP fed is ready afterwards. */
void hdv_decoder_flush(hdv_decoder_t *d);
/* Frees the decoder and the GOPs not taken. */
void hdv_decoder_free(hdv_decoder_t *d);

/* Picture i of a GOP the decoder gave back: decoded, nothing concealed, not refused. */
int hdv_pic_ok(const hdv_gop_t *g, int i);

#ifdef __cplusplus
}
#endif

#endif /* HDV_DECODE_H */
