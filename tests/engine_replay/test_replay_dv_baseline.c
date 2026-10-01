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
 * ctest: PIN_REPLAY=tests/data/ep88-pal.bin, `pin_capture_start` with
 * PIN_FMT_DV_RAW for a couple of seconds (through pin_api.h, same calls
 * pinctl's "capture" subcommand makes) must produce whole 144000/120000-byte
 * DV frames whose bytes are a prefix-aligned slice of the Phase-1 baseline
 * (tests/baseline_sha256.txt's frozen dv_reassembler output) -- at minimum,
 * every frame the capture wrote must appear somewhere in the baseline.
 *
 * The baseline is produced fresh in the build dir by running
 * tests/replay_reassembler.c (the same tool tests/check_baseline.cmake uses)
 * over the same trace; its path is passed in by CMake ($<TARGET_FILE:...>)
 * so this test never hardcodes a build layout.
 *
 * Usage: test_replay_dv_baseline <replay_reassembler_exe> <trace_file>
 */

#include "replay_test_util.h"

#include <stdio.h>
#include <stdlib.h>

static unsigned char *read_whole_file(const char *path, long *out_len)
{
    FILE *f = fopen(path, "rb");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n);
    CHECK(buf);
    CHECK(fread(buf, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    *out_len = n;
    return buf;
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    const char *reassembler_exe = argv[1];
    const char *trace_path = argv[2];

    if (!pin_test_file_exists(trace_path)) {
        printf("SKIP: %s not present\n", trace_path);
        return 0;
    }

    /* 1. Produce the baseline fresh, in the current (build) directory. */
    const char *baseline_path = "dv_baseline_test.out";
    char cmd[2048];
#if defined(_WIN32)
    /* cmd.exe mis-parses "prog" "arg1" "arg2" (several quoted tokens) run
     * through system() -- it needs the whole line wrapped in one more pair
     * of quotes, a well-known cmd.exe/CRT system() quirk. */
    snprintf(cmd, sizeof(cmd), "\"\"%s\" \"%s\" \"%s\"\"", reassembler_exe, trace_path, baseline_path);
#else
    snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\" \"%s\"", reassembler_exe, trace_path, baseline_path);
#endif
    CHECK(system(cmd) == 0);
    long baseline_len = 0;
    unsigned char *baseline = read_whole_file(baseline_path, &baseline_len);

    long frame_size = 0;
    if (baseline_len % 144000 == 0) frame_size = 144000;
    else if (baseline_len % 120000 == 0) frame_size = 120000;
    CHECK(frame_size != 0);
    printf("baseline: %ld bytes, %ld frames of %ld bytes\n", baseline_len,
           baseline_len / frame_size, frame_size);

    /* 2. Capture a few seconds through the public API against the same
     * trace via the replay device. */
    pin_session_t *s = pin_test_open_replay(trace_path);
    pin_test_prepare_dv(s);

    pin_capture_opts_t opts;
    pin_capture_opts_defaults(&opts);
    strncpy(opts.path, "dv_baseline_test_cap", sizeof(opts.path) - 1);
    opts.format_dv = PIN_FMT_DV_RAW;
    CHECK(pin_capture_start(s, &opts, 1) == PIN_OK);
    pin_status_snapshot_t snap;
    pin_test_wait_state(s, PIN_STATE_CAPTURING, PIN_STATE_ERROR, 3000, &snap);
    CHECK(snap.state == PIN_STATE_CAPTURING);

    pin_test_sleep_ms(2500); /* a couple of seconds of frames at ~30 fps pacing */
    CHECK(pin_capture_stop(s) == PIN_OK);
    pin_test_wait_state(s, PIN_STATE_READY, -1, 3000, &snap);
    printf("DV stats: frames=%llu error=%llu dropped=%llu clip=%llu/%llu\n",
           (unsigned long long)snap.frames, (unsigned long long)snap.frames_error,
           (unsigned long long)snap.frames_dropped, (unsigned long long)snap.clip_frames,
           (unsigned long long)snap.clip_frames_error);
    /* The ep88 dump is a raw bus capture that joined a running stream, so its
     * frames carry real losses (shifted DIF blocks): only check the counters
     * are consistent here; clean-frame behaviour is in test_dv_error. */
    CHECK(snap.frames > 0 && snap.frames_error <= snap.frames && snap.frames_dropped <= snap.frames_error);
    CHECK(snap.clip_frames == snap.frames);
    pin_close(s);

    /* 3. Every whole frame the capture wrote must appear as a frame-aligned
     * slice somewhere in the baseline. */
    long cap_len = 0;
    unsigned char *cap = read_whole_file("dv_baseline_test_cap.dv", &cap_len);
    CHECK(cap_len > 0);
    CHECK(cap_len % frame_size == 0);
    long n_cap_frames = cap_len / frame_size;
    long n_base_frames = baseline_len / frame_size;
    printf("capture: %ld bytes, %ld frame(s)\n", cap_len, n_cap_frames);
    CHECK(n_cap_frames > 0);

    for (long i = 0; i < n_cap_frames; i++) {
        const unsigned char *frame = cap + i * frame_size;
        int found = 0;
        for (long j = 0; j < n_base_frames; j++) {
            if (memcmp(frame, baseline + j * frame_size, (size_t)frame_size) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "capture frame %ld has no match in the baseline\n", i);
            exit(1);
        }
    }

    printf("OK: all %ld captured frame(s) matched the baseline\n", n_cap_frames);
    free(baseline);
    free(cap);
    return 0;
}
