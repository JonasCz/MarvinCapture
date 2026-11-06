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
 * HDV for a sink that does more than store the transport stream: the stream cut into
 * pictures, GOPs and audio frames, with the damage the packets show and the camera clock.
 *
 * The HDV pipeline is three stages, the first two shared by any such sink:
 *
 *   hdv_demux   (this file; the sink's write_unit thread)
 *       transport stream -> GOPs of coded pictures (hdv_gop_t) + MP2 audio frames
 *   hdv_decode  (hdv_decode.h; a thread of the sink's own)
 *       GOPs -> the same GOPs with every picture decoded (error concealment on) and a
 *       verdict per picture, in order, once all of a GOP's frames are back
 *   the output  (the sink)
 *       sink_hdvmux.c: MPEG-2 into MOV / MKV, a GOP passed through when whole, else its
 *       decoded pictures encoded again (the repair).
 *       A future HDV to H.264 / HEVC sink (sink_hwenc.c does analog and DV today) takes the
 *       decoded frames of every GOP to the hardware encoder instead: same demux, same
 *       decoder, no passthrough. What it must do as sink_hdvmux.c does, and which should
 *       then move out of sink_hdvmux.c into a shared piece rather than be copied:
 *         - the timeline: a frame per picture at the time its PES PTS says (the camera
 *           clock against where the file has got to), a lost picture's frame repeating its
 *           neighbour, the first GOP's leading B pictures left out (they refer to nothing in
 *           the file), a clock jump taken up without a gap (span_close(), gop_decide());
 *         - the audio on that timeline: each MP2 frame where its PTS falls, silence where it
 *           is missing (drain_audio(); an H.264 sink would decode the MP2 to PCM for FLAC and
 *           put PCM silence instead of MP2 silence);
 *         - the counts: frames "with error" = frames whose picture the decoder concealed or
 *           that repeat a lost one, or whose audio was silenced (frames_error / _repeated).
 *       The verdict hdv_gop_t gives (hdv_pic_ok()) is what decides "with error" there too.
 *
 * Nothing here depends on the output format. Not thread-safe: one thread feeds a demuxer.
 */

#ifndef HDV_DEMUX_H
#define HDV_DEMUX_H

#include "../api/pin_api.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct AVFrame;

/* One coded picture (a frame picture, or both fields of a field-picture frame), with the
 * sequence / GOP headers in front of it. */
typedef struct {
    uint8_t *data;
    size_t len;
    int type;          /* 1 I, 2 P, 3 B, 0 unknown */
    int tr;            /* temporal_reference */
    int damaged;       /* a packet of it was lost, garbled or flagged (the transport stream says) */
    int pic_off;       /* offset of the (first) picture start code in data */
    int gop_off;       /* offset of a GOP header in data, -1 none */
    int seq;           /* a sequence header is in front of it */
    int closed_gop, broken_link;
    int64_t pts;       /* camera clock, 90 kHz, unwrapped (no 33-bit wrap); AV_NOPTS_VALUE none */
} hdv_pic_t;

typedef struct {
    hdv_pic_t *p;
    int n, cap;
} hdv_pics_t;

int hdv_pics_push(hdv_pics_t *l, const hdv_pic_t *pic);
void hdv_pics_free(hdv_pics_t *l);
size_t hdv_pics_bytes(const hdv_pics_t *l);

/* The stream's video values: the first good sequence header (and extension), and the first
 * picture coding extension. */
typedef struct {
    int valid;
    int width, height;
    int aspect_code, frame_rate_code;
    int fps_num, fps_den;
    int64_t bit_rate;          /* bit/s, 0 = variable */
    int vbv_bits;
    int profile_level;         /* profile_and_level_indication, 0 unknown */
    int progressive_seq;
    int chroma;                /* 1 = 4:2:0 */
    /* from the first picture coding extension (tff: the engine's field order if it knows it) */
    int intra_dc, q_scale_type, intra_vlc, alt_scan, tff;
    uint8_t hdr[256];          /* sequence header + extensions (Matroska's CodecPrivate) */
    int hdr_len;
} hdv_seq_t;

/* A GOP: from a picture with a GOP or sequence header, or an I picture, to the next one (or at
 * most HDV_MAX_GOP_PICS pictures, a GOP whose headers were lost). hdv_decode fills frames. */
#define HDV_MAX_GOP_PICS 120
typedef struct hdv_gop {
    hdv_pics_t pics;
    hdv_seq_t seq;             /* the stream's values when it was cut */
    int64_t first_id;          /* the decoder's id of pics.p[0]; picture i is first_id + i */
    struct AVFrame **frames;   /* hdv_decode: per picture, what the decoder gave back (NULL: nothing) */
    int dec_error;             /* hdv_decode: the decoder refused a picture */
    /* for the consumer's own queue */
    int eof;
    size_t bytes;
    struct hdv_gop *next;
} hdv_gop_t;

void hdv_gop_free(hdv_gop_t *g);

typedef struct {
    /* a GOP, the consumer takes it (hdv_gop_free()) */
    void (*gop)(void *user, hdv_gop_t *g);
    /* one MP2 frame and the camera time of its first sample (90 kHz, unwrapped); never a frame
     * with a lost or garbled byte (a PES with a gap keeps only the frames in front of it) */
    void (*audio)(void *user, const uint8_t *frame, int len, int64_t cam, int sample_rate,
                  int channels, int kbps);
    /* out of memory: the stream can no longer be followed */
    void (*error)(void *user, pin_status_t err);
    void *user;
} hdv_demux_cb_t;

typedef struct hdv_demux hdv_demux_t;

/* tff: the field order the engine read from the stream (1 top first, 0 bottom first, -1 take
 * the picture coding extension's). NULL if out of memory. */
hdv_demux_t *hdv_demux_create(const hdv_demux_cb_t *cb, int tff);
/* Whole 188-byte TS packets (the reassembler's units, or any run of them). */
void hdv_demux_feed(hdv_demux_t *d, const uint8_t *ts, size_t len);
/* The end of the stream: the last PES, picture and GOP come out. */
void hdv_demux_flush(hdv_demux_t *d);
void hdv_demux_free(hdv_demux_t *d);

#ifdef __cplusplus
}
#endif

#endif /* HDV_DEMUX_H */
