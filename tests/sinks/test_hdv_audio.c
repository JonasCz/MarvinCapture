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

/* HDV monitor audio: an MP2 frame that runs across a picture boundary (its head ends one push, its
 * tail starts the next, before that push's first PES start) is decoded, not dropped. The frames are
 * silent Layer II frames made by hand (header + zero bit allocation), so no encoder is needed.
 * Includes the module's source to reach its internals. */

#include "../../src/engine/pin_hdv_audio.c"

#include <stdio.h>

static int g_failures = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

#define AUDIO_PID 0x1100
#define FRAME_BYTES 576   /* MPEG-1 Layer II, 192 kbit/s, 48 kHz, no padding */

static unsigned g_samples;
static void on_pcm(void *user, const int16_t *pcm, unsigned frames)
{
    (void)user; (void)pcm;
    g_samples += frames;
}

static void make_frames(uint8_t *out, int n)
{
    memset(out, 0, (size_t)n * FRAME_BYTES);
    for (int i = 0; i < n; i++) {
        out[i * FRAME_BYTES + 0] = 0xFF;
        out[i * FRAME_BYTES + 1] = 0xFD;
        out[i * FRAME_BYTES + 2] = 0xA4;
        out[i * FRAME_BYTES + 3] = 0x00;
    }
}

/* TS packets carrying es[0..len): the first starts a PES when `pes_start`. The last packet is
 * filled up with adaptation-field stuffing. */
static size_t ts_packets(uint8_t *out, const uint8_t *es, size_t len, int pes_start)
{
    size_t n = 0;
    uint8_t cc = 0;
    do {
        uint8_t *p = out + n * HDV_TS_PKT;
        memset(p, 0xFF, HDV_TS_PKT);
        p[0] = 0x47;
        p[1] = (uint8_t)((pes_start ? 0x40 : 0) | (AUDIO_PID >> 8));
        p[2] = AUDIO_PID & 0xFF;
        size_t cap = 184 - (pes_start ? 9 : 0);
        size_t take = len < cap ? len : cap;
        size_t stuff = cap - take;
        uint8_t *pl = p + 4;
        if (stuff > 0) {
            p[3] = (uint8_t)(0x30 | (cc++ & 0xF));
            pl[0] = (uint8_t)(stuff - 1);
            if (stuff > 1)
                pl[1] = 0;
            pl += stuff;
        } else {
            p[3] = (uint8_t)(0x10 | (cc++ & 0xF));
        }
        if (pes_start) {
            static const uint8_t hdr[9] = { 0, 0, 1, 0xC0, 0, 0, 0x80, 0x00, 0x00 };
            memcpy(pl, hdr, 9);
            pl += 9;
            pes_start = 0;
        }
        memcpy(pl, es, take);
        es += take;
        len -= take;
        n++;
    } while (len > 0);
    return n;
}

int main(void)
{
    pin_hdv_audio_t *a = pin_hdv_audio_create(on_pcm, NULL);
    CHECK(a != NULL, "create");
    uint8_t es[6 * FRAME_BYTES];
    make_frames(es, 6);
    static uint8_t buf[64 * HDV_TS_PKT];
    size_t n;

    n = ts_packets(buf, es, FRAME_BYTES + 100, 1);              /* frame 0, head of frame 1 */
    pin_hdv_audio_push(a, buf, n, AUDIO_PID);
    n = ts_packets(buf, es + FRAME_BYTES + 100, FRAME_BYTES - 100, 0);   /* tail of frame 1: no PES start */
    pin_hdv_audio_push(a, buf, n, AUDIO_PID);
    n = ts_packets(buf, es + 2 * FRAME_BYTES, 4 * FRAME_BYTES, 1);       /* frames 2..5 */
    pin_hdv_audio_push(a, buf, n, AUDIO_PID);

    pin_hdv_audio_destroy(a);   /* drains the queue */
    CHECK(g_samples == 6u * 1152u, "frame across the boundary decoded");
    if (g_failures == 0) printf("test_hdv_audio: ok\n");
    return g_failures;
}
