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

#include "pinnacle_enum.h"
#include "pinnacle_device.h"  /* PINNACLE_VID */
#include "pinnacle_lock.h"
#include "pin_log.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* See docs/analog.md, "Models (from marvinavs64.inf)". Only the PID
 * -> name/support mapping lives here; nothing about a specific unit
 * (serial, calibration, I2C address, FX2 firmware file, ...) belongs in
 * this table -- those stay wherever the code that needs them already
 * branches on PID (pinnacle_device.c, pinnacle_analog.c). */
const pinnacle_model_t pinnacle_model_table[] = {
    { 0x0213, "Pinnacle Studio 500-USB",    1 }, /* Marvin-Lite -- this project */
    { 0x0206, "Pinnacle MovieBox Deluxe",   0 }, /* Marvin-classic */
    { 0x0212, "Pinnacle Studio 700-USB",    0 }, /* Marvin-CR */
    { 0x0223, "Pinnacle Studio 510-USB",    0 }, /* Marvin-510 */
    { 0x0224, "Pinnacle Studio 710-USB",    0 }, /* Marvin-710 */
};
const int pinnacle_model_table_count =
    (int)(sizeof(pinnacle_model_table) / sizeof(pinnacle_model_table[0]));

const pinnacle_model_t *pinnacle_model_lookup(uint16_t pid)
{
    for (int i = 0; i < pinnacle_model_table_count; i++) {
        if (pinnacle_model_table[i].pid == pid)
            return &pinnacle_model_table[i];
    }
    return NULL;
}

void pinnacle_enum_build_id(libusb_device *dev, char *out, size_t out_cap)
{
    uint8_t bus = libusb_get_bus_number(dev);
    uint8_t ports[8]; /* USB 3.x hub depth is capped at 7; 8 is generous */
    int n = libusb_get_port_numbers(dev, ports, (int)sizeof(ports));

    int off = snprintf(out, out_cap, "usb:%u", bus);
    if (off < 0)
        off = 0;

    if (n <= 0) {
        /* No hub path at all -- shouldn't happen for a real downstream
         * device, only a root hub. Fall back to the device address so
         * callers at least get *something* unique for this enumerate()
         * call; note it is NOT stable across replugs like the port path
         * is, unlike the normal case below. */
        snprintf(out + off, out_cap - (size_t)off, "-%u", libusb_get_device_address(dev));
        return;
    }

    for (int i = 0; i < n && (size_t)off < out_cap; i++) {
        int wrote = snprintf(out + off, out_cap - (size_t)off, "%s%u",
                              i == 0 ? "-" : ".", ports[i]);
        if (wrote < 0)
            break;
        off += wrote;
    }
}

#if defined(_WIN32)
/* A quick, throwaway libusb_open()/libusb_close() to tell a driverless
 * device from a genuinely openable one. Only called for a device the lock
 * already says nobody else holds (see pinnacle_enumerate() below), so an
 * ACCESS/BUSY here means something raced us between the lock query and
 * this probe (another process opening it right now) rather than the
 * common case -- IN_USE is still the right label for it.
 *
 * Research (2026-09-25): on Windows, libusb assigns a device with no
 * recognised driver (no WinUSB/libusbK/libusb-win32/HID) the internal
 * "unsupported" API backend, whose function table entries are NULL. Every
 * winusb_* entry point starts with CHECK_SUPPORTED_API(), which for a NULL
 * entry logs "unsupported API call ... (unrecognized device driver)" and
 * returns LIBUSB_ERROR_NOT_SUPPORTED -- see libusb/os/windows_winusb.h
 * (CHECK_SUPPORTED_API macro) and windows_winusb.c (winusb_open(), which
 * calls CHECK_SUPPORTED_API(priv->apib, open) before touching the device).
 * So libusb_open() on a driverless Windows device returns
 * LIBUSB_ERROR_NOT_SUPPORTED. Some real-world reports (e.g.
 * github.com/libusb/libusb issues #370, #1307) also see
 * LIBUSB_ERROR_NOT_FOUND for the same underlying "wrong/no driver"
 * condition (typically when Windows re-enumerates the device between
 * libusb's device list snapshot and the open call, which a missing driver
 * makes more likely to be observed); both are treated as NO_DRIVER here. */
static pinnacle_enum_state_t classify_openability(libusb_device *dev)
{
    libusb_device_handle *h = NULL;
    int rc = libusb_open(dev, &h);
    if (rc == 0) {
        libusb_close(h);
        return PINNACLE_ENUM_READY;
    }
    if (rc == LIBUSB_ERROR_NOT_SUPPORTED || rc == LIBUSB_ERROR_NOT_FOUND)
        return PINNACLE_ENUM_NO_DRIVER;
    if (rc == LIBUSB_ERROR_ACCESS || rc == LIBUSB_ERROR_BUSY)
        return PINNACLE_ENUM_IN_USE;
    /* Some other transient libusb error: don't report a wrong, scarier
     * state than we actually know. */
    return PINNACLE_ENUM_READY;
}
#else
/* POSIX: never open here. The lock record above is authoritative for "in
 * use" (that's the whole point of pinnacle_lock.[ch]), and a permissions
 * problem is only meaningful once something actually tries to open the
 * device -- see PINNACLE_ENUM_NO_PERMISSION's doc comment. */
static pinnacle_enum_state_t classify_openability(libusb_device *dev)
{
    (void)dev;
    return PINNACLE_ENUM_READY;
}
#endif

/* One enumeration at a time per process, on one long-lived context: a GUI
 * thread and the pin_devices_wait() watcher may both enumerate, and
 * concurrent libusb_init()/libusb_exit() churn on Windows crashed inside
 * libusb. The context is never exited (process lifetime, no device state). */
static pthread_mutex_t g_enum_mtx = PTHREAD_MUTEX_INITIALIZER;
static libusb_context *g_enum_ctx;

int pinnacle_enumerate(pinnacle_enum_entry_t *out, int max)
{
    pthread_mutex_lock(&g_enum_mtx);
    if (!g_enum_ctx && libusb_init(&g_enum_ctx) != 0) {
        g_enum_ctx = NULL;
        pthread_mutex_unlock(&g_enum_mtx);
        pin_logf(PIN_LOG_WARN, "pinnacle_enumerate: libusb_init failed\n");
        return 0;
    }
    libusb_context *ctx = g_enum_ctx;

    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(ctx, &list);
    int count = 0;

    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != 0)
            continue;
        if (desc.idVendor != PINNACLE_VID)
            continue;
        const pinnacle_model_t *model = pinnacle_model_lookup(desc.idProduct);
        if (!model)
            continue; /* our VID, but not a Marvin PID we know about at all */

        pinnacle_enum_entry_t entry;
        memset(&entry, 0, sizeof(entry));
        pinnacle_enum_build_id(list[i], entry.id, sizeof(entry.id));
        snprintf(entry.name, sizeof(entry.name), "%s", model->name);
        entry.vid = desc.idVendor;
        entry.pid = desc.idProduct;
        entry.usb_address = libusb_get_device_address(list[i]);

        if (!model->supported) {
            entry.state = PINNACLE_ENUM_UNSUPPORTED;
        } else {
            pinnacle_lock_info_t linfo;
            if (pinnacle_lock_query(entry.id, &linfo) == PINNACLE_OK && linfo.held) {
                entry.state = PINNACLE_ENUM_IN_USE;
                entry.owner_pid = linfo.owner_pid;
            } else {
                entry.state = classify_openability(list[i]);
            }
        }

        if (count < max)
            out[count] = entry;
        count++;
    }

    if (list)
        libusb_free_device_list(list, 1);
    pthread_mutex_unlock(&g_enum_mtx);
    return count;
}
