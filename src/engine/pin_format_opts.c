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

/* The format settings and their texts; see pin_format_opts.h. */

#include "pin_format_opts.h"
#include "pin_estimate.h"

#include <stdio.h>
#include <string.h>

int pin_fmt_is_hwenc(pin_format_t f)
{
    return f == PIN_FMT_ANALOG_H264_MKV || f == PIN_FMT_ANALOG_HEVC_MKV ||
           f == PIN_FMT_DV_H264_MKV || f == PIN_FMT_DV_HEVC_MKV;
}

int pin_fmt_is_hevc(pin_format_t f)
{
    return f == PIN_FMT_ANALOG_HEVC_MKV || f == PIN_FMT_DV_HEVC_MKV;
}

int pin_fmt_always_splits(pin_format_t f)
{
    return f == PIN_FMT_HDV_MOV || f == PIN_FMT_HDV_MKV;
}

static int is_dv(pin_format_t f)
{
    return f == PIN_FMT_DV_H264_MKV || f == PIN_FMT_DV_HEVC_MKV;
}

int pin_fmt_clamp_quality(int q)
{
    return q < PIN_QUALITY_MIN ? PIN_QUALITY_MIN : q > PIN_QUALITY_MAX ? PIN_QUALITY_MAX : q;
}

int pin_fmt_clamp_deinterlace(int d)
{
    return d == PIN_DEINTERLACE_KEEP_FIELDS || d == PIN_DEINTERLACE_DOUBLE_RATE ? d
                                                                               : PIN_DEINTERLACE_DEFAULT;
}

int pin_fmt_clamp_ffv1(int c)
{
    return c >= 0 && c < PIN_FFV1_LEVEL_COUNT ? c : PIN_FFV1_DEFAULT;
}

#define COPY(dst, text) snprintf((dst), sizeof(dst), "%s", (text))

/* GB per hour at the default settings, PAL (the warning and the note quote it) */
static double default_gb_per_hour(int hevc)
{
    return pin_est_hwenc_rate(hevc, 0, 0, 0, PIN_DEINTERLACE_DEFAULT, PIN_QUALITY_DEFAULT) * 3600.0 / 1e9;
}

pin_status_t pin_fmt_options(pin_format_t f, pin_format_options_t *out)
{
    if ((unsigned)f >= PIN_FMT_COUNT || !out)
        return PIN_ERR_ARG;
    uint32_t size = out->size;
    memset(out, 0, sizeof(*out));
    out->size = size;

    out->has_quality = out->has_deinterlace = pin_fmt_is_hwenc(f);
    out->has_ffv1_compression = f == PIN_FMT_ANALOG_FFV1_MKV;

    out->quality_min = PIN_QUALITY_MIN;
    out->quality_max = PIN_QUALITY_MAX;
    out->quality_default = PIN_QUALITY_DEFAULT;
    out->quality_lower_is_better = 1;
    COPY(out->quality_label, "Quality");
    COPY(out->quality_low_label, "Best");
    COPY(out->quality_high_label, "Smallest");
    COPY(out->quality_help,
         "Lower numbers keep more detail and make bigger files; 6 steps up halve the size, 6 down "
         "double it. 18, the default, is hard to tell from the source; 23 is about half the size "
         "and still good. It is the scale of x264's CRF: the encoder gets the closest setting it "
         "has (constant quality where it can, else a bit rate worked out from the number).");

    out->deinterlace_default = PIN_DEINTERLACE_DEFAULT;
    COPY(out->deinterlace_label, "Interlaced video");
    COPY(out->deinterlace_keep_label, "Keep fields (25 or 30 fps)");
    COPY(out->deinterlace_double_label, "Deinterlace (50 or 60 fps)");
    COPY(out->deinterlace_keep_help,
         "Each interlaced frame is stored as one progressive picture, nothing converted. The "
         "encoder cannot code the two fields apart, so it compresses them as one picture: "
         "interlaced video loses quality this way (moving edges turn soft or blocky), and players "
         "show combing on motion unless they deinterlace it themselves.");
    COPY(out->deinterlace_double_help,
         "Every field becomes a full frame (the bwdif deinterlacer): 25 interlaced frames per "
         "second become 50 progressive ones, 29.97 become 59.94. Motion stays smooth and the file "
         "plays well everywhere, phones and TVs included; it is about 1.5 times the size.");

    out->ffv1_default = PIN_FFV1_DEFAULT;
    COPY(out->ffv1_label, "Compression");
    COPY(out->ffv1_fast_label, "Fast");
    COPY(out->ffv1_smaller_label, "Smaller");
    COPY(out->ffv1_smallest_label, "Smallest");
    COPY(out->ffv1_help,
         "All three are lossless and give back exactly the same picture. Smaller uses FFV1's range "
         "coder and Smallest also its large context model: usually a few percent smaller files, "
         "how much depends on the tape, for more CPU time (a computer that cannot keep up ends "
         "the capture). Fast is the default.");

    if (pin_fmt_is_hwenc(f)) {
        if (is_dv(f))
            snprintf(out->note, sizeof(out->note),
                     "Lossy, by the hardware video encoder: about %.0f GB per hour at the default "
                     "settings instead of 13 GB for DV, but some quality is lost against the DV "
                     "recording.", default_gb_per_hour(pin_fmt_is_hevc(f)));
        else
            snprintf(out->note, sizeof(out->note),
                     "Lossy, by the hardware video encoder: about %.0f GB per hour at the default "
                     "settings instead of 30 GB for lossless FFV1, but not an archive copy.",
                     default_gb_per_hour(pin_fmt_is_hevc(f)));
    }
    return PIN_OK;
}

void pin_fmt_quality_hint(pin_format_t f, int video_quality, int deinterlace, int is_60hz,
                          char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = 0;
    if (!pin_fmt_is_hwenc(f))
        return;
    int deint = pin_fmt_clamp_deinterlace(deinterlace) == PIN_DEINTERLACE_DOUBLE_RATE;
    double bps = pin_est_hwenc_rate(pin_fmt_is_hevc(f), 0, 0, is_60hz != 0, deint,
                                    pin_fmt_clamp_quality(video_quality));
    snprintf(out, cap, "About %.1f GB per hour (%.1f Mbit/s)", bps * 3600.0 / 1e9, bps * 8.0 / 1e6);
}

void pin_fmt_hwenc_warning(pin_format_t f, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = 0;
    if (!pin_fmt_is_hwenc(f))
        return;
    const char *codec = pin_fmt_is_hevc(f) ? "HEVC" : "H.264";
    if (is_dv(f))
        snprintf(out, cap,
                 "This converts the DV video to %s, which is lossy: the file is much smaller (about "
                 "%.0f GB per hour at the default settings, DV needs 13 GB), but DV is already "
                 "compressed and some quality is lost against the recording. For an archive copy, "
                 "keep the DV itself (raw DV, AVI or MOV).", codec, default_gb_per_hour(pin_fmt_is_hevc(f)));
    else
        snprintf(out, cap,
                 "%s is lossy: the file is small (about %.0f GB per hour at the default settings, "
                 "FFV1 needs about 30 GB), but some detail is lost for good. For an archive copy, "
                 "capture to FFV1, which is lossless.", codec, default_gb_per_hour(pin_fmt_is_hevc(f)));
}

void pin_fmt_unavailable_text(pin_format_t f, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    snprintf(out, cap, "This device has no %s encoder that MarvinCapture can use; choose another "
             "output format.", pin_fmt_is_hevc(f) ? "HEVC" : "H.264");
}
