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

/* Why a capture ended, as one sentence every GUI shows the same way, and the
 * disk space a capture keeps free so its file can still be finished. */

#ifndef PIN_STOP_H
#define PIN_STOP_H

#include "../api/pin_api.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Space kept free on the output drive beyond the writer's queue: room for the
 * container's index / trailer when the file is closed (an AVI idx1 or a MOV
 * moov is ~16-32 bytes per frame, a few MB per hour). */
#define PIN_STOP_DISK_MARGIN (64ull << 20)

/* Bytes that must stay free for the open file to be finished: the margin and
 * what the writer has queued but not written yet. margin 0 = PIN_STOP_DISK_MARGIN. */
uint64_t pin_stop_disk_reserve(uint64_t margin, uint64_t writer_backlog);

/* The largest file FAT32 takes: 4 GiB less a byte. Larger writes fail (EFBIG). */
#define PIN_FAT32_FILE_MAX 0xFFFFFFFFull

/* How big a file may grow on a drive whose files cannot pass `limit` bytes (FAT32), counting what
 * the capture writes into it: limit less 1/64 (64 MiB of FAT32's 4 GiB), which leaves room for
 * what a container adds when it is closed (an AVI index, a MOV's moov: a few MB) and for the
 * units on their way while a cut waits for a place a file may start. 0 for no limit. */
uint64_t pin_stop_file_cap(uint64_t limit);

/* "the file reached the 4 GiB size limit of the FAT32 drive" (a limit other than FAT32's own,
 * from a test, in MB): the detail of a capture that had to stop there (analog; DV and HDV go on
 * in a new file). */
void pin_stop_file_limit_text(uint64_t limit, char *out, size_t cap);

/* The output check's note for a FAT32 drive (pin_output_check_t.message): "This is a FAT32 drive:
 * its files cannot pass 4 GiB, so the capture will go on in a new file about every 19 minutes."
 * (DV and HDV), or for analog "... so the capture will stop after about 3 minutes.". minutes:
 * how long a file lasts at the format's rate. */
void pin_stop_file_limit_note(uint64_t limit, int analog, double minutes, char *out, size_t cap);

/* pin_stop_reason_abnormal(): device / camera gone, disk full, write or other error. */
int pin_stop_abnormal(pin_stop_reason_t reason);

/* "12m30s" / "1h02m05s" / "45s", rounded down (what was captured). */
void pin_stop_format_duration(double seconds, char *out, size_t cap);

/* "Capture stopped after capturing 12m30s, because <detail>." detail may be
 * NULL / "" for the reason's default wording; PIN_STOP_USER has none. */
void pin_stop_message(pin_stop_reason_t reason, double captured_s, const char *detail,
                      char *out, size_t cap);

/* "No video received; nothing was captured to PATH." for a capture that ended
 * without a single frame (path "-" or NULL: "... to standard output"), then, unless
 * reason is PIN_STOP_USER, " The capture stopped because <detail>." (detail as for
 * pin_stop_message()). */
void pin_stop_message_no_video(pin_stop_reason_t reason, const char *path, const char *detail,
                               char *out, size_t cap);

/* The reason and wording for a capture whose writer failed (the file's sink reported
 * last_error). A codec error of a sink that encodes (H.264 / HEVC, FFV1), or of the HDV
 * repair that cannot keep up or fails, is PIN_STOP_ERROR with its own sentence in detail;
 * anything else is PIN_STOP_WRITE_ERROR with an empty detail. */
pin_stop_reason_t pin_stop_writer_failure(pin_format_t fmt, pin_status_t last_error,
                                          int reencode_behind, char *detail, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PIN_STOP_H */
