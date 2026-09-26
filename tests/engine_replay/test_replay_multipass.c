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
 * ctest: capture_opts.passes = 2 against the replay device produces
 * base.dv (pass 1) and base-pass-2.dv (pass 2) -- the replay source's EOT
 * (see pin_session.c's replay_handle_eot()) drives the same pass-advance
 * logic the real async transport-state poll would on hardware. The sample
 * trace is short, so it naturally hits EOT (and loops) well within a
 * couple of seconds -- no need to wait out a whole tape.
 *
 * Usage: test_replay_multipass <trace_file>
 */

#include "replay_test_util.h"

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

    pin_capture_opts_t opts;
    pin_capture_opts_defaults(&opts);
    strncpy(opts.path, "mp_base", sizeof(opts.path) - 1);
    opts.format_dv = PIN_FMT_DV_RAW;
    opts.passes = 2;
    CHECK(pin_capture_start(s, &opts, 1) == PIN_OK);

    pin_status_snapshot_t snap;
    pin_test_wait_state(s, PIN_STATE_CAPTURING, PIN_STATE_ERROR, 3000, &snap);
    CHECK(snap.state == PIN_STATE_CAPTURING);

    /* Wait for pass 2 to start (PIN_EVT_PASS / snap.pass == 2), or for the
     * whole 2-pass capture to finish on its own if the trace is short
     * enough to run through both passes before we notice. */
    int saw_pass2 = 0;
    for (int i = 0; i < 500; i++) { /* up to ~10 s */
        pin_get_status(s, &snap);
        if (snap.pass >= 2) { saw_pass2 = 1; break; }
        if (snap.state != PIN_STATE_CAPTURING && snap.state != PIN_STATE_REWINDING)
            break;
        pin_test_sleep_ms(20);
    }
    CHECK(saw_pass2);

    pin_test_sleep_ms(1000); /* let pass 2 write a little */
    CHECK(pin_capture_stop(s) == PIN_OK);
    pin_test_wait_state(s, PIN_STATE_READY, -1, 3000, &snap);
    pin_close(s);

    CHECK(pin_test_file_exists("mp_base.dv"));
    CHECK(pin_test_file_exists("mp_base-pass-2.dv"));
    printf("OK: mp_base.dv and mp_base-pass-2.dv both exist\n");
    return 0;
}
