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
 * ctest: pin_preview_wait()/pin_preview_lock() against the replay device
 * (PAL DV, see traces/README.md) produce a 720x576 4:2:0 frame with a
 * (1,1) chroma shift and a 4:3 DAR, the same thing pinctl's preview-dump
 * subcommand writes out.
 *
 * Usage: test_replay_preview <trace_file>
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

    uint64_t seq = 0;
    int got = 0;
    pin_frame_t f;
    for (int i = 0; i < 20 && !got; i++) {
        if (pin_preview_wait(s, seq, 2000) != 1)
            continue;
        memset(&f, 0, sizeof(f));
        f.size = sizeof(f);
        if (pin_preview_lock(s, &f) != PIN_OK)
            continue;
        seq = f.seq;
        got = 1;
        CHECK_EQ_I(f.width, 720);
        CHECK_EQ_I(f.height, 576);
        CHECK_EQ_I(f.chroma_shift_x, 1);
        CHECK_EQ_I(f.chroma_shift_y, 1);
        CHECK_EQ_I(f.dar_num, 4);
        CHECK_EQ_I(f.dar_den, 3);
        CHECK(f.plane[0] && f.plane[1] && f.plane[2]);
        pin_preview_unlock(s);
    }
    CHECK(got);
    pin_close(s);
    printf("OK: 720x576, chroma shift (1,1), DAR 4:3\n");
    return 0;
}
