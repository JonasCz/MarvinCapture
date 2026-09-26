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
 * Extracts PCM audio from one already-reassembled DV frame's audio DIF
 * blocks (SMPTE 314M / IEC 61834), for live monitoring/metering
 * (pin_session.c's audio_peak_db/rms and the monitor ring, see pin_api.h's
 * pin_monitor_read()) -- *not* for the archival output file, which always
 * gets the raw DV bytes byte-for-byte via the existing writer/sink path
 * regardless of anything in this file.
 *
 * This implements the real SMPTE 314M / IEC 61834 audio deshuffle -- the
 * same algorithm FFmpeg's libavformat/dv.c dv_extract_audio() and
 * libavcodec/dv_profile.c's audio_shuffle525/625 tables and
 * libavformat/dv.c's dv_audio_12to16() implement, read from the FFmpeg
 * source tree scripts/build-ffmpeg.sh unpacks (third_party/ffmpeg-src) and
 * reimplemented here in this project's own style so pinnacle_engine_pure
 * (hardware-free, no FFmpeg) can extract audio without linking libavformat.
 * Validated sample-for-sample against libavformat's own "dv" demuxer on
 * real captures -- see tests/engine_replay/test_dv_audio_vs_libavformat.c
 * -- for every trace this project has to hand: all of tests/data/dv-ntsc.dv
 * (9,557 real frames, ~10.2 million 16-bit stereo samples), exact match,
 * 0 mismatches, including the full 5-minute/8,967-frame capture. All of
 * those are 48 kHz/16-bit/2-channel/NTSC; no PAL or 32 kHz/12-bit-nonlinear
 * real capture was available, so dv_audio_12to16()'s expansion, while
 * transcribed from the same reference, is unverified against real tape --
 * see that test's own header comment for exactly what was and wasn't
 * checked, and re-run it if/when a 12-bit or PAL sample turns up.
 *
 * What this does *not* implement (the coordinator confirmed neither is
 * needed for this driver): 50 Mbit/s profiles (DVCPRO50, n_difchan > 1) and
 * more than two stereo pairs (the STYPE=3 "8 channel" mode).
 */

#ifndef DV_AUDIO_H
#define DV_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Generous per-frame cap; the real maximum per SMPTE 314M's audio_min_samples
 * + smpls is 1959 (625/50, 48 kHz: 1896 + 63). */
#define DV_AUDIO_MAX_PAIR_SAMPLES 2048

typedef struct {
    int16_t pair1[DV_AUDIO_MAX_PAIR_SAMPLES * 2]; /* interleaved stereo, CH1/CH2 */
    int pair1_samples;
    int16_t pair2[DV_AUDIO_MAX_PAIR_SAMPLES * 2]; /* interleaved stereo, CH3/CH4;
                                                       only populated in the 4-channel
                                                       12-bit nonlinear mode */
    int pair2_samples;
    int sample_rate;   /* 48000/44100/32000 from the AAUX pack, 0 if unknown */
    int four_channel;  /* 1 if pair2 is populated */
    int valid;         /* 0 if no AAUX SOURCE pack / no audio blocks were found at all */
} dv_audio_pcm_t;

/* frame_len must be an exact multiple of DV_SEQ_SIZE (10 or 12 sequences),
 * same contract as dv_subcode_parse_frame(). Returns 0 on success (pcm.valid
 * may still be 0 for a frame with no usable audio), -1 on a malformed
 * buffer. */
int dv_audio_extract(const uint8_t *frame, size_t frame_len, dv_audio_pcm_t *pcm);

#ifdef __cplusplus
}
#endif

#endif /* DV_AUDIO_H */
