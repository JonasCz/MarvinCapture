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
 * The real SMPTE 314M / IEC 61834 DV audio deshuffle -- see dv_audio.h for
 * what this is validated against and how. Frame layout, per DIF sequence
 * ("DIF segment" in SMPTE 314M's own terms; `seq_count` of them, 10 for
 * 525/60 (NTSC) or 12 for 625/50 (PAL)):
 *
 *   [6 header/subcode/VAUX blocks] then 9x [1 audio block + 15 video blocks]
 *
 * i.e. the j-th (0..8) audio block of sequence i sits at block index
 * 6 + j*16 within that sequence (dv_subcode.c's layout_section() documents
 * the same real layout for its own synthetic-frame builder). Its 72-byte
 * sample payload is at block offset 8..79, exactly as dv_subcode.c's own
 * AAUX pack lives at offset 3..7 right before it.
 *
 * The "shuffle" scatters each audio block's samples across the whole
 * frame's time axis at fixed intervals (`audio_stride`: 90 for 525-line,
 * 108 for 625-line) rather than storing them contiguously, so that losing
 * one physical block to a tape dropout loses scattered individual samples
 * instead of a contiguous chunk of audio. dv_audio_shuffle_525/625[i][j]
 * gives the base *sample-slot index* (already an index into an interleaved
 * stereo buffer -- rows 0..half-1 are one channel of a pair, rows
 * half..N-1 the same value + 1, landing on the other channel's slots) for
 * that audio block's very first sample; the rest of the block's samples
 * follow at multiples of `audio_stride` from there. This -- and
 * dv_audio_12to16()'s 12-bit nonlinear expansion -- is transcribed from
 * FFmpeg's libavformat/dv.c (dv_extract_audio(), dv_audio_12to16()) and
 * libavcodec/dv_profile.c (dv_audio_shuffle525/625, audio_min_samples),
 * read from the FFmpeg source tree tools/build-ffmpeg.sh unpacks, in this
 * project's own code -- pinnacle_engine_pure links no FFmpeg. See
 * dv_audio.h for how this has been validated (and, for the 12-bit mode,
 * how it has *not*).
 */

#include "dv_audio.h"
#include "dv_subcode.h"

#include <string.h>

/* [DIF segment][AV sequence 0..8] -> base sample-slot index. 525-line
 * (NTSC, 10 segments): rows 0-4 are channel 1 of whichever stereo pair
 * currently applies, rows 5-9 channel 2 (each = the row 5 above it, + 1).
 * 625-line (PAL, 12 segments): rows 0-5 / 6-11 the same way. */
static const uint8_t dv_audio_shuffle_525[10][9] = {
    {  0, 30, 60, 20, 50, 80, 10, 40, 70 },
    {  6, 36, 66, 26, 56, 86, 16, 46, 76 },
    { 12, 42, 72,  2, 32, 62, 22, 52, 82 },
    { 18, 48, 78,  8, 38, 68, 28, 58, 88 },
    { 24, 54, 84, 14, 44, 74,  4, 34, 64 },

    {  1, 31, 61, 21, 51, 81, 11, 41, 71 },
    {  7, 37, 67, 27, 57, 87, 17, 47, 77 },
    { 13, 43, 73,  3, 33, 63, 23, 53, 83 },
    { 19, 49, 79,  9, 39, 69, 29, 59, 89 },
    { 25, 55, 85, 15, 45, 75,  5, 35, 65 },
};

static const uint8_t dv_audio_shuffle_625[12][9] = {
    {  0, 36,  72, 26, 62,  98, 16, 52,  88 },
    {  6, 42,  78, 32, 68, 104, 22, 58,  94 },
    { 12, 48,  84,  2, 38,  74, 28, 64, 100 },
    { 18, 54,  90,  8, 44,  80, 34, 70, 106 },
    { 24, 60,  96, 14, 50,  86,  4, 40,  76 },
    { 30, 66, 102, 20, 56,  92, 10, 46,  82 },

    {  1, 37,  73, 27, 63,  99, 17, 53,  89 },
    {  7, 43,  79, 33, 69, 105, 23, 59,  95 },
    { 13, 49,  85,  3, 39,  75, 29, 65, 101 },
    { 19, 55,  91,  9, 45,  81, 35, 71, 107 },
    { 25, 61,  97, 15, 51,  87,  5, 41,  77 },
    { 31, 67, 103, 21, 57,  93, 11, 47,  83 },
};

/* freq code (AAUX SOURCE PC4 bits 5-3) -> Hz, and -> the format's minimum
 * per-frame sample count; the actual count is this plus the AAUX SOURCE
 * PC1 "smpls" field (dv_frame_info_t.audio.smpls). SMPTE 314M values,
 * matching FFmpeg's dv_audio_frequency[]/AVDVProfile.audio_min_samples. */
static const int dv_audio_frequency[3] = { 48000, 44100, 32000 };
static const int dv_audio_min_samples_525[3] = { 1580, 1452, 1053 };
static const int dv_audio_min_samples_625[3] = { 1896, 1742, 1264 };

/* Nonlinear 12-bit -> linear 16-bit expansion, SMPTE 314M / IEC 61834
 * (transcribed from FFmpeg's libavformat/dv.c dv_audio_12to16(), see this
 * file's header comment). `sample` is the raw 12-bit code with the sign
 * bit (bit 11) already folded in by the caller the same way FFmpeg's does
 * it: values >= 0x800 get their top nibble replicated (sign-extended into
 * a 16-bit-wide field) before this call. */
static uint16_t dv_audio_12to16(uint16_t sample)
{
    uint16_t shift, result;

    sample = (sample < 0x800) ? sample : (uint16_t)(sample | 0xf000);
    shift = (uint16_t)((sample & 0xf00) >> 8);

    if (shift < 0x2 || shift > 0xd) {
        result = sample;
    } else if (shift < 0x8) {
        shift--;
        result = (uint16_t)((sample - (256 * shift)) << shift);
    } else {
        shift = (uint16_t)(0xe - shift);
        result = (uint16_t)(((sample + ((256 * shift) + 1)) << shift) - 1);
    }
    return result;
}

int dv_audio_extract(const uint8_t *frame, size_t frame_len, dv_audio_pcm_t *pcm)
{
    if (!frame || !pcm || frame_len == 0 || frame_len % DV_SEQ_SIZE != 0)
        return -1;
    unsigned seq_count = (unsigned)(frame_len / DV_SEQ_SIZE);
    if (seq_count != DV_SEQ_COUNT_NTSC && seq_count != DV_SEQ_COUNT_PAL)
        return -1;

    memset(pcm, 0, sizeof(*pcm));

    dv_frame_info_t info;
    if (dv_subcode_parse_frame(frame, frame_len, &info) != 0 || !info.audio.valid)
        return 0; /* no usable AAUX SOURCE pack: pcm stays all-zero/invalid */

    /* dv_subcode.c's parse_audio() only exposes the resolved Hz, not the
     * raw 0/1/2 freq code the min-samples table is indexed by; the mapping
     * back is exact and unambiguous (dv_audio_frequency has no duplicates). */
    int freq_code = -1;
    for (int i = 0; i < 3; i++)
        if (dv_audio_frequency[i] == info.audio.sample_rate)
            freq_code = i;
    if (freq_code < 0)
        return 0;

    int quant = (info.audio.bits == 12) ? 1 : 0;
    int is_525 = (seq_count == DV_SEQ_COUNT_NTSC);
    const uint8_t (*shuffle)[9] = is_525 ? dv_audio_shuffle_525 : dv_audio_shuffle_625;
    const int *min_samples = is_525 ? dv_audio_min_samples_525 : dv_audio_min_samples_625;
    int stride = is_525 ? 90 : 108;
    int difseg_size = (int)seq_count;
    int half_ch = difseg_size / 2;

    int total_samples = min_samples[freq_code] + info.audio.smpls;
    if (total_samples < 0)
        total_samples = 0;
    if (total_samples > DV_AUDIO_MAX_PAIR_SAMPLES)
        total_samples = DV_AUDIO_MAX_PAIR_SAMPLES;
    int slots = total_samples * 2; /* interleaved stereo: L/R slots per pair */

    pcm->sample_rate = info.audio.sample_rate;
    pcm->four_channel = (info.audio.channels == 4) && quant == 1;
    pcm->valid = 1;
    pcm->pair1_samples = total_samples;
    pcm->pair2_samples = pcm->four_channel ? total_samples : 0;
    /* pair1[]/pair2[] are already zeroed by the memset above -- matches
     * dv_extract_audio()'s treatment of a sample slot no write ever
     * reaches (shouldn't happen on an undamaged frame; every slot in
     * [0, slots) is covered by the shuffle+stride traversal below, same as
     * the reference). */

    for (int i = 0; i < difseg_size; i++) {
        const uint8_t *seq = frame + (size_t)i * DV_SEQ_SIZE;
        int16_t *out = pcm->pair1;
        int shuffle_row_base = i;
        if (pcm->four_channel && i >= half_ch) {
            out = pcm->pair2;
            shuffle_row_base = i - half_ch; /* i % half_ch, i is always < 2*half_ch */
        }

        for (int j = 0; j < 9; j++) {
            const uint8_t *block = seq + (size_t)(6 + j * 16) * DV_BLOCK_SIZE;
            const uint8_t *payload = block + 8; /* 72 bytes, offsets 0..71 == frame[8..79] */

            if (quant == 0) {
                /* 16-bit linear: 36 big-endian samples per block, each
                 * landing at its own scattered slot. */
                for (int g = 0; g < 36; g++) {
                    int of = shuffle[i][j] + g * stride;
                    if (of >= slots)
                        continue;
                    uint16_t raw = (uint16_t)((payload[g * 2] << 8) | payload[g * 2 + 1]);
                    /* SMPTE 314M's "erroneous/muted sample" sentinel. */
                    int16_t v = (raw == 0x8000) ? 0 : (int16_t)raw;
                    out[of] = v;
                }
            } else {
                /* 12-bit nonlinear: 24 groups of 3 bytes -> 2 samples
                 * (L, R of *this* stereo pair) each, at independent
                 * scattered slots (L uses the "channel 1" shuffle rows,
                 * R the "channel 2" rows -- see dv_audio_shuffle_525/625's
                 * layout). */
                for (int g = 0; g < 24; g++) {
                    const uint8_t *p = payload + g * 3;
                    uint16_t lc = (uint16_t)((p[0] << 4) | (p[2] >> 4));
                    uint16_t rc = (uint16_t)((p[1] << 4) | (p[2] & 0x0F));
                    lc = (lc == 0x800) ? 0 : dv_audio_12to16(lc);
                    rc = (rc == 0x800) ? 0 : dv_audio_12to16(rc);

                    int of_l = shuffle[shuffle_row_base][j] + g * stride;
                    int of_r = shuffle[shuffle_row_base + half_ch][j] + g * stride;
                    if (of_l < slots)
                        out[of_l] = (int16_t)lc;
                    if (of_r < slots)
                        out[of_r] = (int16_t)rc;
                }
            }
        }
    }

    return 0;
}
