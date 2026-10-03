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

#include "pin_stop.h"
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
    char b[256];
    pin_stop_format_duration(750.9, b, sizeof(b));
    CHECK(strcmp(b, "12m30s") == 0, "minutes, rounded down");
    pin_stop_format_duration(3725, b, sizeof(b));
    CHECK(strcmp(b, "1h02m05s") == 0, "hours");
    pin_stop_format_duration(-1, b, sizeof(b));
    CHECK(strcmp(b, "0s") == 0, "negative is 0s");

    pin_stop_message(PIN_STOP_USER, 45, NULL, b, sizeof(b));
    CHECK(strcmp(b, "Capture stopped after capturing 45s.") == 0, "user stop has no reason");
    pin_stop_message(PIN_STOP_CAMERA_LOST, 750, NULL, b, sizeof(b));
    CHECK(strcmp(b, "Capture stopped after capturing 12m30s, because the FireWire connection "
                    "to the camera was interrupted.") == 0, "default wording");
    pin_stop_message(PIN_STOP_DISK_FULL, 60, "the output drive is almost full (60 MB left)", b, sizeof(b));
    CHECK(strcmp(b, "Capture stopped after capturing 1m00s, because the output drive is almost "
                    "full (60 MB left).") == 0, "caller's detail");
    pin_stop_message(PIN_STOP_ERROR, 1, "", b, sizeof(b));
    CHECK(strcmp(b, "Capture stopped after capturing 1s, because of an error.") == 0, "empty detail");

    CHECK(!pin_stop_abnormal(PIN_STOP_USER) && !pin_stop_abnormal(PIN_STOP_NO_SIGNAL) &&
              !pin_stop_abnormal(PIN_STOP_TIME_LIMIT) && !pin_stop_abnormal(PIN_STOP_END_OF_TAPE) &&
              !pin_stop_abnormal(PIN_STOP_NONE), "normal ends");
    CHECK(pin_stop_abnormal(PIN_STOP_DEVICE_LOST) && pin_stop_abnormal(PIN_STOP_CAMERA_LOST) &&
              pin_stop_abnormal(PIN_STOP_DISK_FULL) && pin_stop_abnormal(PIN_STOP_WRITE_ERROR) &&
              pin_stop_abnormal(PIN_STOP_ERROR) && pin_stop_abnormal(PIN_STOP_PIPE_SLOW) &&
              pin_stop_abnormal(PIN_STOP_PIPE_CLOSED), "abnormal ends");
    pin_stop_message(PIN_STOP_PIPE_SLOW, 30, NULL, b, sizeof(b));
    CHECK(strcmp(b, "Capture stopped after capturing 30s, because the program reading the output "
                    "can't keep up.") == 0, "pipe slow wording");
    pin_stop_message(PIN_STOP_PIPE_CLOSED, 30, NULL, b, sizeof(b));
    CHECK(strcmp(b, "Capture stopped after capturing 30s, because the program reading the output "
                    "exited.") == 0, "pipe closed wording");
    CHECK(pin_stop_disk_reserve(0, 0, 0) == PIN_STOP_DISK_MARGIN, "default margin");
    CHECK(pin_stop_disk_reserve(10, 20, 30) == 60, "margin + backlog + remux");

    if (g_failures) {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_pin_stop: OK\n");
    return 0;
}
