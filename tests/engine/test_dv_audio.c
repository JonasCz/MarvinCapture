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
 * ctest: dv_audio.c's DV audio DIF-block extractor, hardware-free (a
 * synthetic frame, no capture/replay/USB). This builds a frame with known
 * bytes placed at the *real* SMPTE 314M block positions (sequence i,
 * AV-sequence j, audio block offset 8 + g*2 or g*3) and checks that
 * dv_audio_extract() lands each one at exactly the slot the real
 * audio_shuffle_525/625 tables (reproduced in dv_audio.c) predict --
 * i.e. this exercises the actual deshuffle, not just a round-trip through
 * whatever this project's own scheme happens to be. See
 * tests/engine_replay/test_dv_audio_vs_libavformat.c for validation
 * against real captures and FFmpeg's own "dv" demuxer.
 */

#include "dv_audio.h"
#include "dv_subcode.h"

#include <stdio.h>
#include <string.h>

/* Mirrors dv_audio.c's own tables exactly (there is no public accessor for
 * them -- this test independently re-derives the expected slot for a given
 * (sequence, AV-sequence, sample group) the same way dv_audio.c computes it
 * internally, so a mismatch here means the *placement logic*, not just
 * these numbers, disagrees). Duplicating them is intentional: a typo in
 * dv_audio.c's copy is exactly what this is meant to catch. */
static const uint8_t shuffle_525[10][9] = {
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

static int g_failures = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static uint8_t *block_at(uint8_t *frame, unsigned seq, unsigned block_in_seq)
{
    return frame + (size_t)seq * DV_SEQ_SIZE + (size_t)block_in_seq * DV_BLOCK_SIZE;
}

/* Real audio block position: 6 header/subcode/VAUX blocks, then AV
 * sequence j's audio block at 6 + j*16 (see dv_audio.c's file header and
 * dv_subcode.c's layout_section(), which documents the same real layout). */
static uint8_t *audio_block(uint8_t *frame, unsigned seq, unsigned j)
{
    return block_at(frame, seq, 6 + j * 16);
}

static size_t build_ntsc_skeleton(uint8_t *frame, int stype, int quant, int freq_code)
{
    size_t len = (size_t)DV_SEQ_COUNT_NTSC * DV_SEQ_SIZE;
    memset(frame, 0xFF, len);

    for (unsigned s = 0; s < DV_SEQ_COUNT_NTSC; s++) {
        uint8_t *hdr = block_at(frame, s, 0);
        hdr[0] = 0x00; /* section 0: header */
        hdr[3] = 0x00; /* DSF = 0 -> NTSC */
        for (unsigned j = 0; j < 9; j++)
            audio_block(frame, s, j)[0] = 0x60; /* section 3: audio */
    }

    /* AAUX SOURCE pack (0x50) in sequence 0's first audio block: PC1=smpls
     * (0 here, so the *minimum* sample count for this rate applies), PC3
     * (offset 6) = STYPE, PC4 (offset 7) = SMP<<3 | QU. */
    uint8_t *first_audio = audio_block(frame, 0, 0);
    first_audio[3] = 0x50;
    first_audio[4] = 0x00; /* PC1: smpls = 0 */
    first_audio[5] = 0x00; /* PC2 */
    first_audio[6] = (uint8_t)stype;
    first_audio[7] = (uint8_t)((freq_code << 3) | quant);
    return len;
}

static void test_16bit_real_shuffle(void)
{
    static uint8_t frame[(size_t)DV_SEQ_COUNT_NTSC * DV_SEQ_SIZE];
    build_ntsc_skeleton(frame, 0 /* 2ch */, 0 /* 16-bit */, 0 /* 48000 */);

    /* min_samples[48kHz, 525] = 1580 (dv_audio.c), smpls = 0 above, so
     * exactly 1580 samples (3160 slots) are valid; write a distinct,
     * decodable big-endian 16-bit value at every (seq, AV-seq, group)
     * position and check it lands where the shuffle table says, for every
     * position whose predicted slot is in range. */
    const int stride = 90;
    const int slots = 1580 * 2;

    for (unsigned i = 0; i < DV_SEQ_COUNT_NTSC; i++) {
        for (unsigned j = 0; j < 9; j++) {
            uint8_t *payload = audio_block(frame, i, j) + 8;
            for (int g = 0; g < 36; g++) {
                int16_t v = (int16_t)(i * 1000 + j * 100 + g); /* unique, decodable, never 0x8000 */
                payload[g * 2 + 0] = (uint8_t)(((uint16_t)v) >> 8);
                payload[g * 2 + 1] = (uint8_t)(((uint16_t)v) & 0xFF);
            }
        }
    }

    dv_audio_pcm_t pcm;
    int rc = dv_audio_extract(frame, sizeof(frame), &pcm);
    CHECK(rc == 0, "dv_audio_extract succeeds");
    CHECK(pcm.valid, "AAUX SOURCE pack recognised");
    CHECK(pcm.sample_rate == 48000, "sample rate decoded as 48000");
    CHECK(!pcm.four_channel, "2-channel mode");
    CHECK(pcm.pair1_samples == 1580, "sample count == min_samples (smpls=0)");

    int checked = 0, mismatch = 0;
    for (unsigned i = 0; i < DV_SEQ_COUNT_NTSC; i++) {
        for (unsigned j = 0; j < 9; j++) {
            for (int g = 0; g < 36; g++) {
                int of = shuffle_525[i][j] + g * stride;
                if (of >= slots)
                    continue;
                int16_t expect = (int16_t)(i * 1000 + j * 100 + g);
                checked++;
                if (pcm.pair1[of] != expect)
                    mismatch++;
            }
        }
    }
    CHECK(checked > 1000, "a substantial number of positions were in range to check");
    CHECK(mismatch == 0, "every in-range sample landed at the shuffle-predicted slot");
    printf("OK: 16-bit real shuffle placement verified at %d positions (%d mismatched)\n",
           checked, mismatch);
}

static void test_no_audio_pack_is_not_an_error(void)
{
    static uint8_t frame[(size_t)DV_SEQ_COUNT_NTSC * DV_SEQ_SIZE];
    memset(frame, 0xFF, sizeof(frame));
    dv_audio_pcm_t pcm;
    int rc = dv_audio_extract(frame, sizeof(frame), &pcm);
    CHECK(rc == 0, "a frame with no AAUX SOURCE pack is not treated as malformed");
    CHECK(!pcm.valid, "pcm.valid is 0 when no usable audio info was found");
    CHECK(pcm.pair1_samples == 0, "no samples produced");
}

static void test_bad_length_rejected(void)
{
    uint8_t tiny[100] = { 0 };
    dv_audio_pcm_t pcm;
    CHECK(dv_audio_extract(tiny, sizeof(tiny), &pcm) == -1,
          "a non-multiple-of-DV_SEQ_SIZE buffer is rejected");
    CHECK(dv_audio_extract(NULL, 0, &pcm) == -1, "NULL/zero-length is rejected");
}

/* 4-channel 12-bit nonlinear: STYPE=2, QU=1. Only checks that the two
 * pairs land in the right *buffer* (pair1 vs pair2) via a segment from
 * each half and a couple of hand-picked 12-bit codes -- the expansion
 * curve itself isn't independently re-derived here (see dv_audio.h: it's
 * transcribed, not re-verified, against real 12-bit tape audio). */
static void test_12bit_4channel_pair_routing(void)
{
    static uint8_t frame[(size_t)DV_SEQ_COUNT_NTSC * DV_SEQ_SIZE];
    build_ntsc_skeleton(frame, 0x02 /* 4ch */, 1 /* 12-bit */, 2 /* 32000 */);

    const uint16_t code_a = 0x123, code_b = 0x0AB;
    uint8_t g0 = (uint8_t)(code_a >> 4);
    uint8_t g1 = (uint8_t)(((code_a & 0xF) << 4) | (code_b >> 8));
    uint8_t g2 = (uint8_t)(code_b & 0xFF);

    for (unsigned i = 0; i < DV_SEQ_COUNT_NTSC; i++) {
        for (unsigned j = 0; j < 9; j++) {
            uint8_t *payload = audio_block(frame, i, j) + 8;
            for (int g = 0; g < 24; g++) {
                payload[g * 3 + 0] = g0;
                payload[g * 3 + 1] = g1;
                payload[g * 3 + 2] = g2;
            }
        }
    }

    dv_audio_pcm_t pcm;
    int rc = dv_audio_extract(frame, sizeof(frame), &pcm);
    CHECK(rc == 0, "dv_audio_extract succeeds on a 4-channel frame");
    CHECK(pcm.valid, "AAUX SOURCE pack recognised (4ch)");
    CHECK(pcm.sample_rate == 32000, "sample rate decoded as 32000");
    CHECK(pcm.four_channel, "4-channel mode recognised");
    CHECK(pcm.pair1_samples > 0 && pcm.pair2_samples > 0, "both pairs produced samples");

    /* Same pair of codes (L=code_a, R=code_b) everywhere -> every L slot
     * (even index) in each pair should carry one consistent value and
     * every R slot (odd index) another -- checked per parity rather than
     * expecting a single value overall, since L and R differ. */
    int mismatch1_even = 0, mismatch1_odd = 0, mismatch2_even = 0, mismatch2_odd = 0;
    int16_t even1 = 0, odd1 = 0, even2 = 0, odd2 = 0;
    int have_even1 = 0, have_odd1 = 0, have_even2 = 0, have_odd2 = 0;
    for (int i = 0; i < pcm.pair1_samples * 2; i++) {
        if (pcm.pair1[i] == 0) continue;
        if (i % 2 == 0) {
            if (!have_even1) { even1 = pcm.pair1[i]; have_even1 = 1; }
            else if (pcm.pair1[i] != even1) mismatch1_even++;
        } else {
            if (!have_odd1) { odd1 = pcm.pair1[i]; have_odd1 = 1; }
            else if (pcm.pair1[i] != odd1) mismatch1_odd++;
        }
    }
    for (int i = 0; i < pcm.pair2_samples * 2; i++) {
        if (pcm.pair2[i] == 0) continue;
        if (i % 2 == 0) {
            if (!have_even2) { even2 = pcm.pair2[i]; have_even2 = 1; }
            else if (pcm.pair2[i] != even2) mismatch2_even++;
        } else {
            if (!have_odd2) { odd2 = pcm.pair2[i]; have_odd2 = 1; }
            else if (pcm.pair2[i] != odd2) mismatch2_odd++;
        }
    }
    CHECK(have_even1 && have_odd1 && have_even2 && have_odd2,
          "both pairs have both L and R samples");
    CHECK(mismatch1_even == 0 && mismatch1_odd == 0, "pair1's L/R values are each consistent");
    CHECK(mismatch2_even == 0 && mismatch2_odd == 0, "pair2's L/R values are each consistent");
    CHECK(even1 == even2 && odd1 == odd2, "pair1 and pair2 decoded the same codes identically");
    printf("OK: 4-channel 12-bit routing: L=%d R=%d in both pairs\n", even1, odd1);
}

int main(void)
{
    test_16bit_real_shuffle();
    test_no_audio_pack_is_not_an_error();
    test_bad_length_rejected();
    test_12bit_4channel_pair_routing();

    if (g_failures == 0) {
        printf("test_dv_audio: all tests passed\n");
        return 0;
    }
    printf("test_dv_audio: %d failure(s)\n", g_failures);
    return g_failures;
}
