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

/* Hardware-free helpers behind the "what every GUI shows and which buttons are
 * enabled" functions of pin_api.h (pin_format_bytes(), pin_deck_cmd_allowed(),
 * pin_next_file_number(), ...): the API wraps these, so each GUI gets the same
 * texts and rules without duplicating them. The functions that need a status
 * snapshot or device info live in pin_api.c. */

#ifndef PIN_UI_TEXT_H
#define PIN_UI_TEXT_H

#include "../api/pin_api.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "123 B", "1.5 GB": units B KB MB GB TB in 1024 steps, one decimal above bytes. */
void pin_ui_format_bytes(uint64_t bytes, char *out, size_t cap);

/* "2 h 05 min left" / "45 min left", whole minutes truncated, never negative. */
void pin_ui_format_time_left(double seconds, char *out, size_t cap);

/* n with ',' thousands separators: "1,234,567". */
void pin_ui_format_count(uint64_t n, char *out, size_t cap);

/* One number higher than the highest "<name>-<digits>.<ext>" file in the directory of
 * `path` (see pin_next_file_number()). exts: the extensions (no dot) a trailing
 * ".ext" of path's name is stripped for; may be NULL with n_exts 0. */
uint32_t pin_ui_next_file_number(const char *path, const char *const *exts, int n_exts);

/* Rules of pin_deck_cmd_allowed() / pin_capture_action_allowed(). */
int pin_ui_deck_cmd_allowed(pin_state_t state, int deck_available, pin_deck_state_t deck,
                            pin_deck_cmd_t cmd);
int pin_ui_capture_action_allowed(pin_state_t state, int deck_available,
                                  pin_capture_action_t action);

#ifdef __cplusplus
}
#endif

#endif /* PIN_UI_TEXT_H */
