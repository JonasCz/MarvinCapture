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
 * ctest: a capture stopped while it waits for its first frame (the replay holds the first byte
 * back for 60 s, there is no timeout for that wait). The API shows it as capturing, the
 * stop takes effect at once: state READY, stop reason USER, no video, no file on disk.
 *
 * Usage: test_replay_stop_waiting <dv-ntsc.dv>
 */

#include "replay_test_util.h"

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    pin_test_require_fixture(argv[1]);

    pin_test_setenv("PIN_REPLAY_HOLD_MS", "60000");
    pin_session_t *s = pin_test_open_replay(argv[1]);
    pin_test_prepare_dv(s);
    pin_capture_opts_t opts;
    pin_capture_opts_defaults(&opts);
    strncpy(opts.path, "stop_waiting_cap", sizeof(opts.path) - 1);
    opts.format_dv = PIN_FMT_DV_RAW;
    CHECK(pin_capture_start(s, &opts, 1) == PIN_OK);

    pin_status_snapshot_t snap;
    pin_test_wait_state(s, PIN_STATE_CAPTURING, -1, 3000, &snap);
    CHECK(snap.state == PIN_STATE_CAPTURING);
    pin_test_sleep_ms(500); /* still waiting for the first frame */
    pin_get_status(s, &snap);
    CHECK(snap.state == PIN_STATE_CAPTURING);
    CHECK_EQ_I(snap.frames, 0);

    CHECK(pin_capture_stop(s) == PIN_OK);
    pin_test_wait_state(s, PIN_STATE_READY, PIN_STATE_ERROR, 3000, &snap);
    CHECK(snap.state == PIN_STATE_READY);
    CHECK(snap.stop_reason == PIN_STOP_USER);
    CHECK(snap.stop_no_video);
    CHECK(snap.stop_text[0] != 0);
    printf("stop text: %s\n", snap.stop_text);
    CHECK(!pin_test_file_exists("stop_waiting_cap.dv"));
    CHECK(!pin_test_file_exists("stop_waiting_cap"));

    pin_close(s);
    printf("OK: stopped while waiting for the first frame\n");
    return 0;
}
