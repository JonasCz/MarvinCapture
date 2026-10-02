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

/* Deck-control helpers that need no hardware: the TIME CODE answer parser,
 * the transport-state mapping and the countdown formatter. */

#include "pin_deck.h"
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

int main(void)
{
    pin_deck_timecode_t tc;

    /* the Canon's answer from docs/deck-control.md: 00:01:33:06 */
    const uint8_t ok[8] = { 0x0c, 0x20, 0x51, 0x71, 0x06, 0x33, 0x01, 0x00 };
    CHECK(pin_deck_parse_timecode(ok, 8, &tc) == 0 && tc.valid, "valid answer parses");
    CHECK(tc.hour == 0 && tc.minute == 1 && tc.second == 33 && tc.frame == 6, "fields");
    CHECK(!tc.drop_frame, "not drop frame");

    /* IN_TRANSITION (tape moving) still carries the position; drop-frame flag */
    const uint8_t moving[8] = { 0x0b, 0x20, 0x51, 0x71, 0x86, 0x59, 0x59, 0x01 };
    CHECK(pin_deck_parse_timecode(moving, 8, &tc) == 0 && tc.drop_frame && tc.hour == 1 &&
          tc.minute == 59 && tc.second == 59 && tc.frame == 6, "in-transition + drop frame");

    /* not usable: not implemented, short, wrong opcode, no time code (0xff) */
    const uint8_t notimpl[8] = { 0x08, 0x20, 0x51, 0x71, 0xff, 0xff, 0xff, 0xff };
    const uint8_t blank[8] = { 0x0c, 0x20, 0x51, 0x71, 0xff, 0xff, 0xff, 0xff };
    const uint8_t wrongop[8] = { 0x0c, 0x20, 0x52, 0x71, 0x06, 0x33, 0x01, 0x00 };
    CHECK(pin_deck_parse_timecode(notimpl, 8, &tc) != 0 && !tc.valid, "not implemented");
    CHECK(pin_deck_parse_timecode(blank, 8, &tc) != 0, "0xff = no time code");
    CHECK(pin_deck_parse_timecode(ok, 7, &tc) != 0, "short");
    CHECK(pin_deck_parse_timecode(wrongop, 8, &tc) != 0, "wrong opcode");

    /* TRANSPORT STATE (replaces the opcode with the transport mode) */
    {
        pin_capture_opts_t o;
        pin_capture_opts_defaults(&o);
        CHECK(!pin_capture_passes_allowed(0, 0), "no limit: no passes");
        CHECK(pin_capture_passes_allowed(5, 0), "idle only");
        CHECK(pin_capture_passes_allowed(0, 30), "duration only");
        CHECK(pin_capture_passes_allowed(5, 30), "both");
        o.passes = 3;
        CHECK(pin_capture_opts_normalize(&o) == 1 && o.passes == 1, "forced to 1 without limits");
        o.passes = 3; o.max_duration_minutes = 10;
        CHECK(pin_capture_opts_normalize(&o) == 0 && o.passes == 3, "kept with duration");
        o.passes = 0;
        CHECK(pin_capture_opts_normalize(&o) == 1 && o.passes == 1, "passes < 1 becomes 1");
    }
    CHECK(pin_deck_state_from_avc(0xc4, 0x60) == PIN_DECK_STOPPED, "wind stop");
    CHECK(pin_deck_state_from_avc(0xc4, 0x65) == PIN_DECK_REWINDING, "rewind");
    CHECK(pin_deck_state_from_avc(0xc4, 0x75) == PIN_DECK_FAST_FORWARD, "ff");
    CHECK(pin_deck_state_from_avc(0xc3, 0x75) == PIN_DECK_PLAYING, "play");
    CHECK(pin_deck_state_from_avc(0xd0, 0x7f) == PIN_DECK_UNKNOWN, "echoed query");

    /* the 4-byte commands sent to the deck */
    uint8_t b[8];
    CHECK(pin_deck_build_cmd(PIN_DECK_CMD_REW, b) == 4 && b[2] == 0xc4 && b[3] == 0x65, "REW bytes");
    CHECK(pin_deck_build_cmd(PIN_DECK_CMD_STOP, b) == 4 && b[2] == 0xc4 && b[3] == 0x60, "STOP bytes");

    /* async queries build the right status request without a link */
    pin_deck_async_t a;
    pin_deck_async_start_query(&a, NULL, 1, PIN_DECK_QUERY_TIMECODE, 0);
    CHECK(a.cmd_len == 8 && a.cmd[0] == 0x01 && a.cmd[2] == 0x51 && a.status == PIN_DECK_ASYNC_RUNNING,
          "timecode query");
    pin_deck_async_start_query(&a, NULL, 1, PIN_DECK_QUERY_STATE, 0);
    CHECK(a.cmd_len == 4 && a.cmd[0] == 0x01 && a.cmd[2] == 0xd0, "state query");
    pin_deck_async_start_query(&a, NULL, 0, PIN_DECK_QUERY_STATE, 0);
    CHECK(a.status == PIN_DECK_ASYNC_FAILED, "no camera");

    char buf[32];
    pin_format_remaining(330.0, buf, sizeof(buf));
    CHECK(!strcmp(buf, "5m30s"), "5m30s");
    pin_format_remaining(45.2, buf, sizeof(buf));
    CHECK(!strcmp(buf, "46s"), "rounds up");
    pin_format_remaining(3725, buf, sizeof(buf));
    CHECK(!strcmp(buf, "1h02m05s"), "hours");
    pin_format_remaining(-3, buf, sizeof(buf));
    CHECK(!strcmp(buf, "0s"), "clamped");

    /* taskbar progress mode */
    {
        pin_status_snapshot_t st;
        double f;
        memset(&st, 0, sizeof(st));
        st.idle_stop_remaining_s = st.duration_remaining_s = -1;
        st.progress_percent = -1;
        st.state = PIN_STATE_READY;
        CHECK(pin_status_progress(&st, 0, 0, &f) == PIN_PROGRESS_NONE && f == 0, "ready: none");
        st.state = PIN_STATE_PREPARING;
        CHECK(pin_status_progress(&st, 0, 0, &f) == PIN_PROGRESS_INDETERMINATE, "preparing: unknown length");
        st.progress_percent = 25;
        CHECK(pin_status_progress(&st, 0, 0, &f) == PIN_PROGRESS_NORMAL && f == 0.25, "preparing: percent");
        st.state = PIN_STATE_ERROR;
        CHECK(pin_status_progress(&st, 0, 0, &f) == PIN_PROGRESS_ERROR, "error");
        st.state = PIN_STATE_CAPTURING;
        st.signal = 1;
        CHECK(pin_status_progress(&st, 300, 0, &f) == PIN_PROGRESS_INDETERMINATE, "capturing, no limit");
        st.duration_remaining_s = 750;
        CHECK(pin_status_progress(&st, 0, 1000, &f) == PIN_PROGRESS_NORMAL && f == 0.25, "elapsed / limit");
        st.signal = 0;
        st.idle_stop_remaining_s = 75;
        CHECK(pin_status_progress(&st, 300, 1000, &f) == PIN_PROGRESS_NORMAL && f == 0.25,
              "no-signal countdown wins, falls");
        st.idle_stop_remaining_s = -1;
        CHECK(pin_status_progress(&st, 300, 1000, &f) == PIN_PROGRESS_NORMAL && f == 0.25, "limit without timeout");
        st.duration_remaining_s = -1;
        CHECK(pin_status_progress(&st, 0, 0, &f) == PIN_PROGRESS_PAUSED && f == 1, "waiting for signal");
        st.state = PIN_STATE_REWINDING;
        CHECK(pin_status_progress(&st, 0, 0, NULL) == PIN_PROGRESS_PAUSED, "rewinding");
        st.state = PIN_STATE_STOPPING;
        CHECK(pin_status_progress(&st, 0, 0, &f) == PIN_PROGRESS_INDETERMINATE, "stopping");
    }

    if (g_failures == 0)
        printf("test_pin_deck: all passed\n");
    return g_failures ? 1 : 0;
}
