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

/* Bytes that must stay free for the open file to be finished: the margin,
 * what the writer has queued but not written yet, and remux_bytes for a
 * format that copies the whole file when it is closed (HDV to MOV/MKV
 * writes a temp .ts and remuxes it at close). margin 0 = PIN_STOP_DISK_MARGIN. */
uint64_t pin_stop_disk_reserve(uint64_t margin, uint64_t writer_backlog, uint64_t remux_bytes);

/* "12m30s" / "1h02m05s" / "45s", rounded down (what was captured). */
void pin_stop_format_duration(double seconds, char *out, size_t cap);

/* "Capture stopped after capturing 12m30s, because <detail>." detail may be
 * NULL / "" for the reason's default wording; PIN_STOP_USER has none. */
void pin_stop_message(pin_stop_reason_t reason, double captured_s, const char *detail,
                      char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PIN_STOP_H */
