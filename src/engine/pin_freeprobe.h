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

/* Free-space lookups kept off the session lock. statvfs / GetDiskFreeSpaceEx on a network share
 * that has gone away can block for a minute; the lookup therefore runs on a worker thread, at most
 * one at a time per probe, and callers read the last result. */

#ifndef PIN_FREEPROBE_H
#define PIN_FREEPROBE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PIN_FREEPROBE_DIR_MAX 1024

typedef struct pin_freeprobe pin_freeprobe_t;

/* The blocking lookup: 0 = worked. */
typedef int (*pin_freeprobe_fn)(const char *dir, uint64_t *free_bytes, int *fat32);

pin_freeprobe_t *pin_freeprobe_create(pin_freeprobe_fn fn);
/* Drops the caller's reference. A lookup still blocked in the OS keeps running on its own and
 * frees the probe when it returns; this never waits for it. */
void pin_freeprobe_release(pin_freeprobe_t *p);

/* The last result for dir. If it is older than max_age_s (or for another directory) and no lookup
 * runs, starts one. Returns 1 and fills the outputs when a result for dir exists (possibly older
 * than max_age_s while a lookup runs), else 0. *known = the lookup worked. Never blocks on the
 * lookup. */
int pin_freeprobe_get(pin_freeprobe_t *p, const char *dir, double max_age_s,
                      uint64_t *free_bytes, int *known, int *fat32);

/* Waits up to ms for the running lookup (if any) to finish. Returns 1 when none runs. */
int pin_freeprobe_wait_idle(pin_freeprobe_t *p, int ms);

#ifdef __cplusplus
}
#endif

#endif
