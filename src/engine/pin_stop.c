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

uint64_t pin_stop_disk_reserve(uint64_t margin, uint64_t writer_backlog)
{
    return (margin ? margin : PIN_STOP_DISK_MARGIN) + writer_backlog;
}

uint64_t pin_stop_file_cap(uint64_t limit)
{
    return limit - limit / 64;
}

/* "4 GiB", or a test's limit in MB */
static void limit_size(uint64_t limit, char *out, size_t cap)
{
    if (limit == PIN_FAT32_FILE_MAX)
        snprintf(out, cap, "4 GiB");
    else
        snprintf(out, cap, "%.1f MB", (double)limit / 1e6);
}

void pin_stop_file_limit_text(uint64_t limit, char *out, size_t cap)
{
    if (!out || !cap)
        return;
    char size[32];
    limit_size(limit, size, sizeof(size));
    snprintf(out, cap, "the file reached the %s size limit of the FAT32 drive", size);
}

void pin_stop_file_limit_note(uint64_t limit, int analog, double minutes, char *out, size_t cap)
{
    if (!out || !cap)
        return;
    char size[32], when[80];
    limit_size(limit, size, sizeof(size));
    long m = minutes > 0 ? (long)minutes : 0;
    if (analog && m < 1)
        snprintf(when, sizeof(when), "stop in less than a minute");
    else if (analog)
        snprintf(when, sizeof(when), "stop after about %ld minute%s", m, m == 1 ? "" : "s");
    else if (m < 1)
        snprintf(when, sizeof(when), "go on in a new file more than once a minute");
    else if (m == 1)
        snprintf(when, sizeof(when), "go on in a new file about every minute");
    else
        snprintf(when, sizeof(when), "go on in a new file about every %ld minutes", m);
    snprintf(out, cap, "This is a FAT32 drive: its files cannot pass %s, so the capture will %s.",
             size, when);
}

int pin_stop_abnormal(pin_stop_reason_t reason)
{
    switch (reason) {
    case PIN_STOP_DEVICE_LOST:
    case PIN_STOP_CAMERA_LOST:
    case PIN_STOP_DISK_FULL:
    case PIN_STOP_WRITE_ERROR:
    case PIN_STOP_ERROR:
    case PIN_STOP_PIPE_SLOW:
    case PIN_STOP_PIPE_CLOSED:
    case PIN_STOP_DISK_SLOW:
        return 1;
    default:
        return 0;
    }
}

void pin_stop_format_duration(double seconds, char *out, size_t cap)
{
    if (!out || !cap)
        return;
    long n = seconds > 0 ? (long)seconds : 0;
    if (n >= 3600)
        snprintf(out, cap, "%ldh%02ldm%02lds", n / 3600, (n / 60) % 60, n % 60);
    else if (n >= 60)
        snprintf(out, cap, "%ldm%02lds", n / 60, n % 60);
    else
        snprintf(out, cap, "%lds", n);
}

static const char *default_detail(pin_stop_reason_t reason)
{
    switch (reason) {
    case PIN_STOP_NO_SIGNAL:   return "there was no signal for too long";
    case PIN_STOP_TIME_LIMIT:  return "the time limit was reached";
    case PIN_STOP_END_OF_TAPE: return "the end of the tape was reached";
    case PIN_STOP_DEVICE_LOST: return "the capture device was disconnected";
    case PIN_STOP_CAMERA_LOST: return "the FireWire connection to the camera was interrupted";
    case PIN_STOP_DISK_FULL:   return "the output drive is almost full";
    case PIN_STOP_WRITE_ERROR: return "writing the file failed";
    case PIN_STOP_ERROR:       return "of an error";
    case PIN_STOP_PIPE_SLOW:   return "the program reading the output can't keep up";
    case PIN_STOP_PIPE_CLOSED: return "the program reading the output exited";
    case PIN_STOP_DISK_SLOW:   return "the output drive could not keep up";
    default:                   return NULL;
    }
}

void pin_stop_message_no_video(pin_stop_reason_t reason, const char *path, const char *detail,
                               char *out, size_t cap)
{
    if (!out || !cap)
        return;
    int n;
    if (!path || !path[0] || (path[0] == '-' && !path[1]))
        n = snprintf(out, cap, "No video received; nothing was captured to standard output.");
    else   /* a script hands over "name." (extension stripped: it is not known yet) */
        n = snprintf(out, cap, "No video received; nothing was captured to %.*s.",
                     (int)(strlen(path) - (path[strlen(path) - 1] == '.' ? 1 : 0)), path);
    if (!detail || !detail[0])
        detail = default_detail(reason);
    if (reason != PIN_STOP_USER && detail && n > 0 && (size_t)n < cap)
        snprintf(out + n, cap - (size_t)n, " The capture stopped because %s.", detail);
}

void pin_stop_message(pin_stop_reason_t reason, double captured_s, const char *detail,
                      char *out, size_t cap)
{
    if (!out || !cap)
        return;
    char dur[32];
    pin_stop_format_duration(captured_s, dur, sizeof(dur));
    if (!detail || !detail[0])
        detail = default_detail(reason);
    if (reason == PIN_STOP_USER || !detail)
        snprintf(out, cap, "Capture stopped after capturing %s.", dur);
    else
        snprintf(out, cap, "Capture stopped after capturing %s, because %s.", dur, detail);
}
