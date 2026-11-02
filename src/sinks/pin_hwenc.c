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

/* The platform's H.264 / HEVC encoder, its settings and the probe; see pin_hwenc.h. */

#include "pin_hwenc.h"
#include "pin_sink.h"
#include "../engine/pin_estimate.h"
#include "../core/pin_log.h"

#include <libavutil/opt.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENV_OVERRIDE "PIN_HWENC_ENCODER"

static const char *override_name(void)
{
    const char *o = getenv(ENV_OVERRIDE);
    return o && o[0] ? o : NULL;
}

const char *pin_hwenc_encoder_name(pin_hwenc_codec_t c)
{
    const char *o = override_name();
    if (o)
        return o;
#if defined(__ANDROID__)
    return c == PIN_HWENC_HEVC ? "hevc_mediacodec" : "h264_mediacodec";
#elif defined(__APPLE__)
    return c == PIN_HWENC_HEVC ? "hevc_videotoolbox" : "h264_videotoolbox";
#elif defined(_WIN32)
    return c == PIN_HWENC_HEVC ? "hevc_mf" : "h264_mf";
#else
    (void)c;
    return NULL;
#endif
}

double pin_hwenc_vt_quality(int q)
{
    if (q < 0) q = 0;
    if (q > 51) q = 51;
    double v = 100.0 - 1.9 * q;
    return v < 1.0 ? 1.0 : v;
}

/* One way of opening the encoder; the platform's list is tried in order. */
typedef struct {
    enum AVPixelFormat pix;
    int profile;        /* ask for H.264 High / HEVC Main */
    int quality_mode;   /* VideoToolbox: constant quality instead of the bit rate */
    int hw;             /* Media Foundation: the hardware MFT */
    const char *what;   /* for the log */
} attempt_t;

#define MAX_ATTEMPTS 4

static int attempts(attempt_t *a)
{
    int n = 0;
    if (override_name()) {
        /* tests: exercises the NV12 -> YUV420P fallback with an encoder (FFV1) that has no NV12 */
        a[n++] = (attempt_t){ AV_PIX_FMT_NV12, 1, 0, 0, "nv12" };
        a[n++] = (attempt_t){ AV_PIX_FMT_YUV420P, 1, 0, 0, "yuv420p" };
        return n;
    }
#if defined(__ANDROID__)
    /* Encoders must take YUV420Flexible, of which only one of planar and semi-planar is
     * guaranteed; most take NV12. Some refuse a profile they would happily pick themselves. */
    a[n++] = (attempt_t){ AV_PIX_FMT_NV12, 1, 0, 0, "nv12, profile" };
    a[n++] = (attempt_t){ AV_PIX_FMT_YUV420P, 1, 0, 0, "yuv420p, profile" };
    a[n++] = (attempt_t){ AV_PIX_FMT_NV12, 0, 0, 0, "nv12" };
    a[n++] = (attempt_t){ AV_PIX_FMT_YUV420P, 0, 0, 0, "yuv420p" };
#elif defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
    /* constant quality exists on Apple Silicon only (FFmpeg's vtenc_qscale_enabled()) */
    a[n++] = (attempt_t){ AV_PIX_FMT_NV12, 1, 1, 0, "constant quality" };
#endif
    a[n++] = (attempt_t){ AV_PIX_FMT_NV12, 1, 0, 0, "bit rate" };
#elif defined(_WIN32)
    a[n++] = (attempt_t){ AV_PIX_FMT_NV12, 1, 0, 1, "hardware MFT" };
    a[n++] = (attempt_t){ AV_PIX_FMT_NV12, 1, 0, 0, "software MFT" };
#else
    (void)a;
#endif
    return n;
}

static AVCodecContext *try_open(const AVCodec *codec, pin_hwenc_codec_t c,
                                const pin_hwenc_config_t *cfg, const attempt_t *a)
{
    AVCodecContext *e = avcodec_alloc_context3(codec);
    if (!e)
        return NULL;
    e->width = cfg->width;
    e->height = cfg->height;
    e->pix_fmt = a->pix;
    e->time_base = (AVRational){ cfg->fps_den, cfg->fps_num };
    e->framerate = (AVRational){ cfg->fps_num, cfg->fps_den };
    e->sample_aspect_ratio = (AVRational){ cfg->sar_num, cfg->sar_den };
    e->field_order = AV_FIELD_PROGRESSIVE;
    e->color_range = AVCOL_RANGE_MPEG;
    if (cfg->is_625) {
        e->color_primaries = AVCOL_PRI_BT470BG;
        e->color_trc = AVCOL_TRC_GAMMA28;
        e->colorspace = AVCOL_SPC_BT470BG;
    } else {
        e->color_primaries = AVCOL_PRI_SMPTE170M;
        e->color_trc = AVCOL_TRC_SMPTE170M;
        e->colorspace = AVCOL_SPC_SMPTE170M;
    }
    /* a keyframe every second: seeking in an editor, and what a crash loses at the end */
    e->gop_size = (cfg->fps_num + cfg->fps_den / 2) / cfg->fps_den;
    if (e->gop_size < 1)
        e->gop_size = 1;
    e->max_b_frames = 0;
    double fps = (double)cfg->fps_num / cfg->fps_den;
    e->bit_rate = (int64_t)pin_est_video_bitrate(c == PIN_HWENC_HEVC, cfg->width, cfg->height, fps,
                                                 cfg->doubled, cfg->quality);
    if (a->profile)
        e->profile = c == PIN_HWENC_HEVC ? AV_PROFILE_HEVC_MAIN : AV_PROFILE_H264_HIGH;
    if (cfg->global_header)
        e->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (!override_name()) {
#if defined(__ANDROID__)
        av_opt_set_int(e->priv_data, "ndk_codec", 1, 0);
        av_opt_set(e->priv_data, "bitrate_mode", "vbr", 0);
#elif defined(__APPLE__)
        av_opt_set_int(e->priv_data, "allow_sw", 0, 0);
        if (a->quality_mode) {
            e->flags |= AV_CODEC_FLAG_QSCALE;
            e->global_quality = (int)(FF_QP2LAMBDA * pin_hwenc_vt_quality(cfg->quality) + 0.5);
        }
#elif defined(_WIN32)
        av_opt_set_int(e->priv_data, "hw_encoding", a->hw, 0);
        av_opt_set(e->priv_data, "rate_control", "quality", 0);
        av_opt_set(e->priv_data, "scenario", "camera_record", 0);
        e->global_quality = cfg->quality < 1 ? 1 : cfg->quality;   /* the QP; 0 means "not set" */
#endif
    }

    int rc = avcodec_open2(e, codec, NULL);
    if (rc < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(rc, err, sizeof(err));
        pin_logf(PIN_LOG_DEBUG, "hwenc: %s did not open (%s): %s\n", codec->name, a->what, err);
        avcodec_free_context(&e);
        return NULL;
    }
    pin_logf(PIN_LOG_DEBUG, "hwenc: %s opened (%s), %dx%d %d/%d fps, %lld bit/s\n", codec->name,
             a->what, cfg->width, cfg->height, cfg->fps_num, cfg->fps_den, (long long)e->bit_rate);
    return e;
}

/* Tries the attempts, `first` (the probe's winner, or -1) before the others. */
static AVCodecContext *open_attempts(pin_hwenc_codec_t c, const pin_hwenc_config_t *cfg, int first,
                                     int *which)
{
    const char *name = pin_hwenc_encoder_name(c);
    if (!name)
        return NULL;
    const AVCodec *codec = avcodec_find_encoder_by_name(name);
    if (!codec || codec->type != AVMEDIA_TYPE_VIDEO) {
        pin_logf(PIN_LOG_DEBUG, "hwenc: encoder %s is not built in\n", name);
        return NULL;
    }
    attempt_t a[MAX_ATTEMPTS];
    int n = attempts(a);
    if (first >= n)
        first = -1;
    for (int k = -1; k < n; k++) {
        int i = k < 0 ? first : k;
        if (i < 0 || (k >= 0 && k == first))
            continue;
        AVCodecContext *e = try_open(codec, c, cfg, &a[i]);
        if (e) {
            if (which)
                *which = i;
            return e;
        }
    }
    return NULL;
}

/* ---- the probe ------------------------------------------------------------------- */

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_probed;
static int g_ok[PIN_HWENC_CODEC_COUNT];
static int g_attempt[PIN_HWENC_CODEC_COUNT];
static char g_probed_override[64];      /* PIN_HWENC_ENCODER at the time of the probe */

static void *probe_thread(void *arg)
{
    (void)arg;
    /* Some attempts are expected to fail (a pixel format, a profile, the hardware MFT);
     * FFmpeg says so at error level, which is no news here. */
    pin_sink_av_log_quiet(1);
    for (int c = 0; c < PIN_HWENC_CODEC_COUNT; c++) {
        /* PAL SD at the default quality, as most captures will be */
        pin_hwenc_config_t cfg = {
            .width = 720, .height = 576, .fps_num = 25, .fps_den = 1, .doubled = 0,
            .quality = 18, .sar_num = 16, .sar_den = 15, .is_625 = 1, .global_header = 1,
        };
        int which = -1;
        AVCodecContext *e = open_attempts((pin_hwenc_codec_t)c, &cfg, -1, &which);
        g_ok[c] = e != NULL;
        g_attempt[c] = which;
        avcodec_free_context(&e);
    }
    pin_sink_av_log_quiet(0);
    return NULL;
}

int pin_hwenc_probe(void)
{
    pin_sink_av_log_install();
    pthread_mutex_lock(&g_mtx);
    const char *ov = override_name();
    if (!g_probed || strcmp(ov ? ov : "", g_probed_override) != 0) {
        snprintf(g_probed_override, sizeof(g_probed_override), "%s", ov ? ov : "");
        memset(g_ok, 0, sizeof(g_ok));
        if (pin_hwenc_encoder_name(PIN_HWENC_H264) || pin_hwenc_encoder_name(PIN_HWENC_HEVC)) {
            /* On a thread of its own, never the caller's: Media Foundation initialises COM
             * as multithreaded on the thread that opens it, which fails on a GUI thread
             * that is a single-threaded apartment. */
            pthread_t t;
            if (pthread_create(&t, NULL, probe_thread, NULL) == 0)
                pthread_join(t, NULL);
            else
                probe_thread(NULL);
        }
        g_probed = 1;
        pin_logf(PIN_LOG_DEBUG, "hwenc: H.264 encoder %s, HEVC encoder %s\n",
                 g_ok[PIN_HWENC_H264] ? "available" : "not available",
                 g_ok[PIN_HWENC_HEVC] ? "available" : "not available");
    }
    int mask = 0;
    for (int c = 0; c < PIN_HWENC_CODEC_COUNT; c++)
        if (g_ok[c])
            mask |= 1 << c;
    pthread_mutex_unlock(&g_mtx);
    return mask;
}

int pin_hwenc_available(pin_hwenc_codec_t c)
{
    if ((unsigned)c >= PIN_HWENC_CODEC_COUNT)
        return 0;
    return (pin_hwenc_probe() >> c) & 1;
}

AVCodecContext *pin_hwenc_open(pin_hwenc_codec_t c, const pin_hwenc_config_t *cfg)
{
    if ((unsigned)c >= PIN_HWENC_CODEC_COUNT || !cfg)
        return NULL;
    int first = -1;
    pthread_mutex_lock(&g_mtx);
    if (g_probed && g_ok[c])
        first = g_attempt[c];
    pthread_mutex_unlock(&g_mtx);
    return open_attempts(c, cfg, first, NULL);
}
