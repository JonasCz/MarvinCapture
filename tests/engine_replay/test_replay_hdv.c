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
 * ctest: HDV .ts replay (pin_session.c's replay_run_ts(), see its header
 * comment) end to end through pin_api.h -- open the replay device on a
 * plain MPEG2-TS file, let the stream settle to PIN_KIND_HDV, capture with
 * scene_split on, and check:
 *   - the stream is recognised as HDV and frames are counted;
 *   - --split produces no spurious cuts (HDV scene split only has the GOP
 *     timecode to go on -- see pin_session.c's HDV branch of dv_on_unit()
 *     -- so a real, single, uninterrupted recording must stay one file).
 *
 * Skips (prints a message, exits 0) if captures/20260925-hdv-5min.ts isn't
 * present -- it's the user's own recording, gitignored (see
 * captures/README.md). A live capture is ~1 GB; only the first ~30 MB
 * (a few seconds of 1080i/25 HDV, comfortably under this test's time
 * budget at replay_run_ts()'s ~33 ms-per-picture pacing) is copied into a
 * scratch file and replayed, so this checks "no spurious split over the
 * available slice" rather than literally the plan's "first minute".
 *
 * Usage: test_replay_hdv <capture_ts_file>
 */

#include "replay_test_util.h"

#define SLICE_BYTES (30u * 1024 * 1024)
#define TS_PACKET 188

static void make_slice(const char *src_path, const char *dst_path)
{
    FILE *in = fopen(src_path, "rb");
    CHECK(in);
    FILE *out = fopen(dst_path, "wb");
    CHECK(out);
    uint8_t buf[TS_PACKET * 512];
    size_t total = 0;
    size_t n;
    while (total < SLICE_BYTES && (n = fread(buf, 1, sizeof(buf), in)) > 0) {
        size_t whole = (n / TS_PACKET) * TS_PACKET;
        if (whole == 0)
            break;
        CHECK(fwrite(buf, 1, whole, out) == whole);
        total += whole;
    }
    fclose(in);
    fclose(out);
    CHECK(total > 0);
    printf("slice: %zu bytes copied from %s\n", total, src_path);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const char *cap_path = argv[1];
    if (!pin_test_file_exists(cap_path)) {
        printf("SKIP: %s not present (see captures/README.md)\n", cap_path);
        return 0;
    }

    const char *slice_path = "hdv_replay_slice.ts";
    make_slice(cap_path, slice_path);

    pin_session_t *s = pin_test_open_replay(slice_path);
    pin_test_prepare_dv(s);

    /* Wait for the stream kind to settle to HDV (a few PAT/PMT + picture
     * units in). */
    pin_status_snapshot_t snap;
    int is_hdv = 0;
    for (int i = 0; i < 100; i++) {
        pin_get_status(s, &snap);
        if (snap.stream_kind == PIN_KIND_HDV) { is_hdv = 1; break; }
        pin_test_sleep_ms(50);
    }
    CHECK(is_hdv);
    printf("OK: stream recognised as HDV (%dx%d)\n", snap.width, snap.height);

    pin_capture_opts_t opts;
    pin_capture_opts_defaults(&opts);
    strncpy(opts.path, "hdv_replay_out", sizeof(opts.path) - 1);
    opts.format_hdv = PIN_FMT_HDV_TS;
    opts.scene_split = 1;
    CHECK(pin_capture_start(s, &opts, 1) == PIN_OK);
    pin_test_wait_state(s, PIN_STATE_CAPTURING, PIN_STATE_ERROR, 3000, &snap);
    CHECK(snap.state == PIN_STATE_CAPTURING);

    /* Run until the (short) slice hits EOT and the single pass auto-stops,
     * or a generous timeout -- whichever comes first, then stop explicitly
     * either way so a slice too short to reach EOT still finalises files. */
    pin_test_wait_state(s, PIN_STATE_READY, PIN_STATE_ERROR, 15000, &snap);
    if (snap.state == PIN_STATE_CAPTURING) {
        pin_capture_stop(s);
        pin_test_wait_state(s, PIN_STATE_READY, -1, 3000, &snap);
    }
    CHECK(snap.state == PIN_STATE_READY);
    CHECK(snap.frames > 0);
    pin_close(s);

    CHECK(pin_test_file_exists("hdv_replay_out-0001.ts"));
    CHECK(!pin_test_file_exists("hdv_replay_out-0002.ts"));
    printf("OK: %llu HDV picture(s) captured, no spurious scene split\n",
           (unsigned long long)snap.frames);
    return 0;
}
