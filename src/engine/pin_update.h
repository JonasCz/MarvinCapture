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

/* Hardware-free half of the update check (pin_update_check() in pin_api.h): parsing
 * the published VERSION file, comparing versions and the notice text. The download
 * is pin_update_net.c. */

#ifndef PIN_UPDATE_H
#define PIN_UPDATE_H

#include "../api/pin_api.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fills latest / notes / download_url of `info` from the VERSION JSON in `json`
 * (len bytes, need not be terminated). Returns 0 when "version" is present and a
 * dotted number, else -1 (info untouched). Unknown keys are ignored; download_url
 * must be http(s) or is left "". */
int pin_update_parse(const char *json, size_t len, pin_update_info_t *info);

/* Compares dotted numeric versions ("1.0" == "1.0.0" < "1.0.1" < "1.10"):
 * <0, 0, >0. Non-digits inside a part are ignored. */
int pin_update_compare(const char *a, const char *b);

/* pin_update_check(), in pin_update_net.c (part of pinnacle_engine, not the pure
 * library). */
pin_status_t pin_update_net_check(pin_update_info_t *info, uint32_t timeout_ms);

/* pin_format_update(). */
void pin_update_format(const pin_update_info_t *info, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PIN_UPDATE_H */
