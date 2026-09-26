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
 * Cross-process per-device ownership + status record.
 *
 * One process at a time may hold a device's lock (acquired by its
 * pinnacle_enum_entry_t.id / pin_device_info_t.id port path). Any process,
 * including ones that never opened the device, can query who holds it and
 * what state they report (Preparing/Ready/Capturing), without touching USB.
 *
 * The hard requirement is that nothing stale survives a crash or a reboot:
 *
 *   - Windows: a named mutex (owned by the process, released by the kernel
 *     the instant the process dies for any reason) plus a pagefile-backed
 *     named shared-memory section for the status record. Neither is ever a
 *     file; both vanish when the last handle closes, which happens at
 *     process exit even on a crash. See pinnacle_lock.c for exactly how a
 *     query tells "held" from "free" without racing the owner.
 *   - Linux: a file in $XDG_RUNTIME_DIR (a per-user tmpfs cleared at logout
 *     or reboot), held with flock() -- which the kernel releases on crash --
 *     and unlinked on a clean release. A query that can take the shared
 *     lock knows the file is stale and removes it.
 *   - macOS: the same file scheme, in $TMPDIR (cleared at reboot).
 *
 * See docs/analog-notes.md and the plan for the multi-model background;
 * nothing here is specific to the 500-USB.
 */

#ifndef PINNACLE_LOCK_H
#define PINNACLE_LOCK_H

#include "pinnacle_device.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors the session states a lock holder reports while it owns the
 * device (a subset of the future engine's state machine -- just enough for
 * another process to show something sensible in a device list). */
typedef enum {
    PINNACLE_LOCK_PREPARING = 0, /* firmware upload / bring-up / input switch in progress */
    PINNACLE_LOCK_READY,         /* idle, device up, nothing running */
    PINNACLE_LOCK_CAPTURING,
} pinnacle_lock_state_t;

/* Opaque: the on-disk/in-memory record layout is an implementation detail
 * of pinnacle_lock.c (and is versioned there, see PINNACLE_LOCK_VERSION). */
typedef struct pinnacle_lock pinnacle_lock_t;

typedef struct {
    int held;
    uint32_t owner_pid;         /* valid iff held */
    pinnacle_lock_state_t state;/* valid iff held */
    uint32_t guid_hi, guid_lo;  /* the 1394 GUID the owner read from the device, if known */
    int guid_known;
} pinnacle_lock_info_t;

/* Takes exclusive, cross-process ownership of the device identified by
 * device_id (the same id string pinnacle_enumerate() reports -- a USB port
 * path, or a future "replay:<file>" virtual id; sanitised internally for
 * use in OS object names / a file name). Starts in PINNACLE_LOCK_PREPARING.
 *
 * Returns PINNACLE_ERR_BUSY if another live process already holds it.
 * A lock abandoned by a crashed previous owner is acquired normally (no
 * special return code -- the caller can't tell the difference, by design:
 * nothing stale is visible by the time acquire returns). *out is only
 * written on PINNACLE_OK. */
pinnacle_status_t pinnacle_lock_acquire(const char *device_id, pinnacle_lock_t **out);

/* Updates the state (and, once known, the GUID) an in-progress or later
 * query sees. guid_hi/guid_lo are only stored when at least one is
 * non-zero (a real 1394 EUI-64 is never all-zero in practice); pass 0,0
 * to update the state without touching a GUID recorded earlier. */
pinnacle_status_t pinnacle_lock_update(pinnacle_lock_t *lock, pinnacle_lock_state_t state,
                                       uint32_t guid_hi, uint32_t guid_lo);

/* Releases the lock: on Windows, closes the kernel objects (freeing them if
 * we were the last handle, which we always are); on POSIX, unlinks the
 * backing file then closes/unlocks it. Safe with NULL. *lock is invalid
 * after this call. */
void pinnacle_lock_release(pinnacle_lock_t *lock);

/* Non-blocking; never touches USB. Reports whether device_id is currently
 * held and by whom. A lock left behind by a process that crashed or was
 * killed (Windows: kernel objects already gone; POSIX: flock released by
 * the kernel, file possibly still present) is correctly reported as *not*
 * held -- on POSIX this call also removes the now-stale file so the next
 * query doesn't repeat the work. Always fills *out (zeroed on any error);
 * the return value is only non-OK for an internal failure unrelated to
 * whether the device is locked (e.g. the lock directory couldn't be
 * created), in which case *out reports held = 0 as the safe default. */
pinnacle_status_t pinnacle_lock_query(const char *device_id, pinnacle_lock_info_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PINNACLE_LOCK_H */
