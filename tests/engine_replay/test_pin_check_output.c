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
 * ctest: pin_check_output() against the replay device -- no collision when
 * the target name is free, free_bytes > 0, and a collision once a file
 * matching the naming pattern already exists on disk.
 *
 * Usage: test_pin_check_output <trace_file>
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

    remove("check_output_test.dv"); /* leftover from a previous run of this test, if any */

    pin_session_t *s = pin_test_open_replay(trace_path);

    /* pin_enumerate() must list the replay device with serial "REPLAY" (a
     * fixed, recognisable marker distinct from a real 1394 GUID -- see
     * pin_api.c's pin_enumerate()). */
    pin_device_info_t devs[16];
    for (int i = 0; i < 16; i++) { memset(&devs[i], 0, sizeof(devs[i])); devs[i].size = sizeof(devs[i]); }
    int n = pin_enumerate(devs, 16);
    int found_replay = 0;
    for (int i = 0; i < n && i < 16; i++) {
        if (strncmp(devs[i].id, "replay:", 7) == 0) {
            found_replay = 1;
            CHECK(strcmp(devs[i].serial, "REPLAY") == 0);
        }
    }
    CHECK(found_replay);
    printf("OK: replay device enumerated with serial \"REPLAY\"\n");

    pin_test_prepare_dv(s);
    pin_test_sleep_ms(200); /* let a frame or two flow so stream_kind settles to DV */

    pin_capture_opts_t opts;
    pin_capture_opts_defaults(&opts);
    strncpy(opts.path, "check_output_test", sizeof(opts.path) - 1);
    opts.format_dv = PIN_FMT_DV_RAW;

    pin_output_check_t chk;
    memset(&chk, 0, sizeof(chk));
    chk.size = sizeof(chk);
    CHECK(pin_check_output(s, &opts, &chk) == PIN_OK);
    CHECK(!chk.collision);
    CHECK(chk.free_bytes > 0);
    CHECK(strcmp(chk.first_path, "check_output_test.dv") == 0);
    printf("OK: no collision, free_bytes=%llu, first_path=%s\n",
           (unsigned long long)chk.free_bytes, chk.first_path);

    /* Now create that exact file and check again. */
    FILE *f = fopen("check_output_test.dv", "wb");
    CHECK(f);
    fputc('x', f);
    fclose(f);

    memset(&chk, 0, sizeof(chk));
    chk.size = sizeof(chk);
    CHECK(pin_check_output(s, &opts, &chk) == PIN_OK);
    CHECK(chk.collision);
    CHECK(chk.message[0] != '\0');
    printf("OK: collision detected: %s\n", chk.message);

    pin_close(s);
    return 0;
}
