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
 * ctest: the FFV1 sink (it writes the file from its own threads) on a drive that stalls.
 * PIN_TEST_WRITE_STALL makes one write stop for a while, PIN_TEST_STALL_TIMEOUT_MS shortens
 * the sink's watchdog and close()'s patience.
 *   - a stall longer than the watchdog while a write is in progress is waited for: the capture
 *     goes on and the file is finished;
 *   - a write that does not come back in time at close: close() gives up with an I/O error
 *     instead of waiting for it forever.
 */

#include "test_util.h"
#include "../../src/sinks/pin_sink.h"
#include "../../src/sinks/sinks_internal.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void set_env(const char *k, const char *v)
{
#if defined(_WIN32)
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

static pin_sink_params_t params(void)
{
    pin_sink_params_t p;
    memset(&p, 0, sizeof(p));
    p.kind = PIN_KIND_ANALOG;
    p.width = 720;
    p.height = 576;
    p.fps_num = 25;
    p.fps_den = 1;
    p.interlaced = 1;
    p.top_field_first = 1;
    p.aspect = PIN_ASPECT_4_3;
    p.audio_rate = 48000;
    p.audio_channels = 2;
    p.colour_matrix = PIN_MATRIX_BT601;
    return p;
}

/* Writes n frames (video and its audio); returns the first error. */
static pin_status_t feed(pin_sink_t *s, int n)
{
    static uint8_t yuyv[720 * 576 * 2];
    static int16_t pcm[1920 * 2];
    for (int i = 0; i < n; i++) {
        for (size_t k = 0; k < sizeof(yuyv); k++)
            yuyv[k] = (uint8_t)(k * 7 + i * 13);   /* not flat: FFV1 frames of some size */
        pin_status_t rc = s->write_video(s, yuyv, sizeof(yuyv));
        if (rc == PIN_OK)
            rc = s->write_audio(s, pcm, 1920);
        if (rc != PIN_OK)
            return rc;
    }
    return PIN_OK;
}

int main(void)
{
    pin_sink_params_t p = params();

    /* a 1.5 s stall against a 0.3 s watchdog: waited for */
    set_env("PIN_TEST_STALL_TIMEOUT_MS", "300");
    set_env("PIN_TEST_WRITE_STALL", "1000000:1500");
    pin_sink_t *s = pin_sink_create(PIN_FMT_ANALOG_FFV1_MKV);
    CHECK(s && s->open(s, "test_stall_wait.mkv", &p) == PIN_OK);
    CHECK_EQ_I(feed(s, 60), PIN_OK);
    CHECK_EQ_I(s->close(s), PIN_OK);
    printf("OK: a drive that stalls longer than the watchdog is waited for\n");

    /* a write still stuck at close: given up after the patience, not waited for */
    set_env("PIN_TEST_WRITE_STALL", "100000:8000");
    s = pin_sink_create(PIN_FMT_ANALOG_FFV1_MKV);
    CHECK(s && s->open(s, "test_stall_close.mkv", &p) == PIN_OK);
    feed(s, 3);
    double t0 = now_s();
    CHECK_EQ_I(s->close(s), PIN_ERR_IO);
    double took = now_s() - t0;
    printf("OK: close gave up on a stuck write after %.1f s\n", took);
    CHECK(took < 4.0);
    /* the stuck thread still owns its state: the process ends without waiting for it */
    return 0;
}
