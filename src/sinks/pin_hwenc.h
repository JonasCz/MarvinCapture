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
 * The operating system's H.264 / HEVC encoder, through FFmpeg's wrappers:
 * which one each platform has, how the quality setting maps onto it, the
 * fallbacks to try when it refuses a configuration, and the once-per-process
 * probe behind pin_encoders_probe() / pin_formats(). Used by sink_hwenc.c.
 *
 *   Android  h264_mediacodec / hevc_mediacodec, through the NDK (ndk_codec=1:
 *            FFmpeg dlopens libmediandk.so, no JVM needed). VBR at the bit rate
 *            of pin_est_video_bitrate(): the constant-quality mode is rare on
 *            phone encoders and the NDK cannot ask whether it is there.
 *            Fallbacks: NV12 or YUV420P input (an encoder must take one of
 *            them, which one is up to the device), with or without the High /
 *            Main profile.
 *   macOS    h264_videotoolbox / hevc_videotoolbox, hardware only (allow_sw 0).
 *            Apple Silicon: constant quality (AV_CODEC_FLAG_QSCALE +
 *            global_quality, pin_hwenc_vt_quality()), falling back to the bit
 *            rate; Intel: the bit rate.
 *   Windows  h264_mf / hevc_mf: the hardware MFT (hw_encoding=1), else
 *            Microsoft's software one. Constant QP: rate_control=quality with
 *            global_quality = the quality value as the QP
 *            (CODECAPI_AVEncVideoEncodeQP, which applies in the quality
 *            rate-control mode); the bit rate is set too, for an MFT that
 *            ignores the QP. scenario=camera_record keeps the frame rate
 *            constant (the MFT drops frames in its default VFR mode).
 *   Linux    none (no LGPL-safe encoder of its own): nothing is offered.
 *
 * Every platform: keyframe every second, no B-frames (MediaCodec gives no
 * DTS; FFmpeg's pts_as_dts then uses the PTS, which is only right without
 * reordering), H.264 High / HEVC Main, BT.601 limited range, progressive.
 *
 * Tests only: the environment variable PIN_HWENC_ENCODER names an encoder to
 * use for both codecs instead (e.g. "ffv1", so the whole sink runs on Linux,
 * which has no H.264 encoder). Not for users: the file then holds that codec.
 * PIN_HWENC_FAIL_AFTER=<pictures> makes sink_hwenc's encoder fail after that
 * many pictures, as a hardware encoder that dies mid-capture would.
 */

#ifndef PIN_HWENC_H
#define PIN_HWENC_H

#include <libavcodec/avcodec.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PIN_HWENC_H264 = 0,
    PIN_HWENC_HEVC,
    PIN_HWENC_CODEC_COUNT
} pin_hwenc_codec_t;

/* What the encoder is opened with (sink_hwenc.c fills it from pin_sink_params_t). */
typedef struct {
    int width, height;
    int fps_num, fps_den;       /* of the encoded stream: twice the source's when deinterlacing */
    int doubled;                /* the frame rate was doubled by deinterlacing (bit rate mapping) */
    int quality;                /* 0..51, lower = better (pin_estimate.h) */
    int sar_num, sar_den;
    int is_625;                 /* 625 lines / 50 Hz: BT.470 B/G colour; else SMPTE 170M (525) */
    int global_header;          /* the muxer wants the parameter sets as extradata (Matroska) */
} pin_hwenc_config_t;

/* The encoder this platform uses for codec (PIN_HWENC_ENCODER, if set, for both), or NULL. */
const char *pin_hwenc_encoder_name(pin_hwenc_codec_t c);

/* Opens codec's encoder for cfg, trying the platform's fallbacks (the one the probe found
 * first). NULL if none opens. The context's pix_fmt is the input format it takes (NV12 or
 * YUV420P). Open, use and free it on one thread: Media Foundation initialises COM on the
 * thread that opens it and uninitialises it on the one that frees it. */
AVCodecContext *pin_hwenc_open(pin_hwenc_codec_t c, const pin_hwenc_config_t *cfg);

/* The probe (pin_api.h pin_encoders_probe()): opens each codec's encoder once at 720 x 576,
 * 25 fps, on a thread of its own, and caches the result for the process (a different
 * PIN_HWENC_ENCODER makes it probe again: tests). Returns a bitmask of 1 << pin_hwenc_codec_t.
 * Blocking; thread-safe (a second caller waits for the first one's result). */
int pin_hwenc_probe(void);

/* 1 if codec's encoder opened in the probe (running it first if needed). */
int pin_hwenc_available(pin_hwenc_codec_t c);

/* VideoToolbox's constant quality (0..100, higher = better) for a quality value (0..51,
 * lower = better): 100 - 1.9 q, at least 1. 18 -> 65.8, 23 -> 56.3, 51 -> 3.1. Picked so the
 * default lands where VideoToolbox's output looks like the source and its size is near the
 * bit rate mapping's; Apple does not document the scale. */
double pin_hwenc_vt_quality(int q);

#ifdef __cplusplus
}
#endif

#endif /* PIN_HWENC_H */
