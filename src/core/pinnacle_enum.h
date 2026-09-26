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
 * Device enumeration: which Marvin-family (VID 0x2304) units are on the USB
 * bus right now, without opening any of them long-term. Cheap enough to
 * call on every OS device-change notification or a 2 s GUI poll.
 *
 * Model support lives in one table (pinnacle_model_table) so adding a
 * future Marvin model is a one-line change here, never something baked
 * into a specific unit's data path -- see docs/analog.md, "Models
 * (from marvinavs64.inf)", for where the PID/name list comes from.
 */

#ifndef PINNACLE_ENUM_H
#define PINNACLE_ENUM_H

#include <libusb-1.0/libusb.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PINNACLE_ENUM_ID_MAX 64
#define PINNACLE_ENUM_NAME_MAX 64

typedef enum {
    /* Present, not held by another process (per pinnacle_lock_query), and,
     * where we can tell (see below), openable. */
    PINNACLE_ENUM_READY = 0,
    /* pinnacle_lock_query() says another process holds it; owner_pid is
     * that process's pid. This is the authoritative "in use" signal on
     * every platform -- see pinnacle_enum.c for the Windows fallback used
     * only when the lock itself can't explain an open failure. */
    PINNACLE_ENUM_IN_USE,
    /* Windows only: the device has no WinUSB/libusbK/libusb-win32 driver
     * bound, so libusb_open() can't succeed until it's bound (Zadig, or the
     * future installer). Detected by a quick libusb_open() probe -- see
     * pinnacle_enum.c for exactly which libusb error code this is and why.
     * Never produced on POSIX (there is no equivalent "wrong driver"
     * state there; a permissions problem is PINNACLE_ENUM_NO_PERMISSION
     * instead, and pinnacle_enumerate() doesn't probe for it -- see that
     * value below). */
    PINNACLE_ENUM_NO_DRIVER,
    /* POSIX only, and not produced by pinnacle_enumerate() itself: reserved
     * for a caller that made its own open attempt (e.g. pinnacle_open_by_id
     * returning PINNACLE_ERR_USB_OPEN from a libusb_open() that failed with
     * LIBUSB_ERROR_ACCESS) to report, when the device's lock said nobody
     * else holds it -- so the ACCESS can't be "another process has it
     * open", only "we aren't allowed to". pinnacle_enumerate() never opens
     * a device on POSIX (the lock record is authoritative for IN_USE), so
     * it always reports PINNACLE_ENUM_READY for an unlocked, supported
     * device there; a udev permissions problem only shows up once
     * something actually tries to open it. The hint to show alongside it
     * is "install the udev rule (see README) and replug, or run as root". */
    PINNACLE_ENUM_NO_PERMISSION,
    /* A recognised sibling model (see pinnacle_model_table) this driver
     * doesn't talk to yet. */
    PINNACLE_ENUM_UNSUPPORTED,
} pinnacle_enum_state_t;

typedef struct {
    char id[PINNACLE_ENUM_ID_MAX];     /* "usb:<bus>-<port>[.<port>...]"; stable while plugged into this port */
    char name[PINNACLE_ENUM_NAME_MAX]; /* "Pinnacle Studio 500-USB" */
    uint16_t vid, pid;
    pinnacle_enum_state_t state;
    uint32_t owner_pid;                /* PINNACLE_ENUM_IN_USE: the other process; else 0 */
    uint8_t usb_address;               /* changes on every replug: with id, keys a per-plug cache */
} pinnacle_enum_entry_t;

/* One row per known Marvin-family model. "supported" is 1 only for models
 * this driver actually drives (0213 today); the rest are listed so they
 * show up as PINNACLE_ENUM_UNSUPPORTED instead of being invisible. */
typedef struct {
    uint16_t pid;
    const char *name;
    int supported;
} pinnacle_model_t;

extern const pinnacle_model_t pinnacle_model_table[];
extern const int pinnacle_model_table_count;

/* Looks up a PID (under VID 0x2304) in pinnacle_model_table. Returns NULL
 * if it isn't a recognised Marvin-family model at all. */
const pinnacle_model_t *pinnacle_model_lookup(uint16_t pid);

/* Builds the "usb:<bus>-<port>.<port>..." id pinnacle_enumerate() reports
 * for this device, from its USB topology alone (bus number + hub port
 * path) -- never opens it. Exposed so pinnacle_open_by_id() can match a
 * caller-supplied id against the live device list with the exact same
 * logic pinnacle_enumerate() used to report it. out_cap should be at least
 * PINNACLE_ENUM_ID_MAX. */
void pinnacle_enum_build_id(libusb_device *dev, char *out, size_t out_cap);

/* Lists Marvin-family devices (VID 0x2304, any PID in pinnacle_model_table)
 * present on the bus right now. Uses its own long-lived libusb_context
 * (separate from any session's) and is serialised process-wide, so it may be
 * called from several threads. Never opens a device
 * for longer than a single probe call (Windows, unsupported-vs-no-driver
 * classification only -- see pinnacle_enum.c). Fills up to max entries in
 * out (each left untouched beyond that); returns how many devices exist,
 * which may be larger than max, or 0 if libusb itself couldn't be
 * initialised (indistinguishable here from "no devices": a GUI polling
 * this at 2 Hz has nothing useful to do with a transient libusb_init()
 * failure beyond trying again next poll). */
int pinnacle_enumerate(pinnacle_enum_entry_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* PINNACLE_ENUM_H */
