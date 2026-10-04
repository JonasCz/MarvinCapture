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
 * ctest: the preview's jitter buffer end to end. The replay device delivers
 * DV in real time, with whatever jitter the OS's sleep has (~16 ms steps on
 * Windows); the frames must come with present_times on an even grid, and
 * pin_preview_lock_due() must not hand a frame out before its time.
 *
 * Usage: test_replay_preview_pacing <trace_file>
 */

#include "replay_test_util.h"

#include <math.h>

#ifdef _WIN32
#include <windows.h>
static void sleep_s(double s) { Sleep((DWORD)(s * 1000 + 0.5)); }
#else
#include <time.h>
static void sleep_s(double s)
{
    struct timespec ts = { (time_t)s, (long)((s - (time_t)s) * 1e9) };
    nanosleep(&ts, NULL);
}
#endif

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const char *trace_path = argv[1];
    if (!pin_test_file_exists(trace_path)) {
        printf("SKIP: %s not present\n", trace_path);
        return 0;
    }

    pin_session_t *s = pin_test_open_replay(trace_path);
    pin_test_prepare_dv(s);

    enum { N = 75, SETTLED = 25 };
    double t[N];
    uint64_t q[N];
    int got = 0, early_refused = 0;
    uint64_t seq = 0;
    while (got < N) {
        CHECK(pin_preview_wait(s, seq, 2000) == 1);
        double due;
        if (!pin_preview_next_time(s, &due))
            continue;
        pin_frame_t f;
        memset(&f, 0, sizeof(f));
        f.size = sizeof(f);
        double now = pin_clock_now();
        if (due > now + 0.001) {
            CHECK(pin_preview_lock_due(s, now, &f) == PIN_ERR_STATE);
            early_refused++;
            sleep_s(due - now);
        }
        while (pin_preview_lock_due(s, pin_clock_now(), &f) != PIN_OK)
            sleep_s(0.001);
        CHECK(f.present_time <= pin_clock_now());
        CHECK(f.seq > seq);
        seq = f.seq;
        q[got] = f.seq;
        t[got++] = f.present_time;
        pin_preview_unlock(s);
    }
    pin_close(s);

    /* once the rate is learnt every step is the frame period, to well under a
     * display refresh (a frame this loop was too slow for, and lock_due()
     * dropped, makes a step of two periods). Loose, because the replay's own
     * rate wanders with the OS's sleep; test_pin_pace checks the smoothing
     * itself against exact synthetic arrivals. */
    double mean = (t[N - 1] - t[SETTLED - 1]) / (double)(q[N - 1] - q[SETTLED - 1]);
    double worst = 0;
    for (int i = SETTLED; i < N; i++) {
        double err = fabs(t[i] - t[i - 1] - mean * (double)(q[i] - q[i - 1]));
        if (err > worst) worst = err;
    }
    printf("period %.2f ms, worst step error %.3f ms, %d early locks refused\n",
           mean * 1000, worst * 1000, early_refused);
    CHECK(mean > 0.030 && mean < 0.045);
    CHECK(worst < 0.004);
    CHECK(early_refused > 0);
    printf("OK\n");
    return 0;
}
