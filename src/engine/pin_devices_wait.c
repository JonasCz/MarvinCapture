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
 * pin_api.h's pin_devices_wait()/pin_devices_wake(): a process-global wait
 * so a GUI needs no device-list polling loop. One lazily-started background
 * watcher thread (kept for the process lifetime, never stopped -- there is
 * no device-side state to release) is responsible for two independent
 * signals, either of which bumps a generation counter that every blocked
 * pin_devices_wait() call is waiting to see move past the value it read on
 * entry:
 *
 *   - A native OS hotplug notification (Windows: CM_Register_Notification
 *     on GUID_DEVINTERFACE_USB_DEVICE; POSIX: libusb hotplug, when the
 *     platform's libusb build has that capability), debounced by ~300 ms
 *     of quiet time so the several interface-arrival events a single
 *     physical plug produces (Windows in particular) collapse into one
 *     generation bump, per pin_api.h's pin_devices_wait() doc comment.
 *   - Every ~500 ms, a cheap fingerprint of the lock records
 *     (pinnacle_lock_query(): state/owner) of the devices seen by the last
 *     enumeration -- no USB access at all. The USB device list itself is
 *     only re-read after a hotplug event, or every 2 s as a safety-net
 *     rescan (Windows: in case CM_Register_Notification never fires for an
 *     arrival, e.g. no device was present when it was registered; POSIX:
 *     always, when libusb has no hotplug capability).
 *
 * pin_devices_wake() is a distinct signal (not a "device list changed"
 * bump): it only unblocks a pending wait, which then reports 0 (timeout),
 * per pin_api.h -- for a GUI shutting down a thread parked in
 * pin_devices_wait(with a long/infinite timeout).
 */

#include "pin_session.h"
#include "../core/pinnacle_enum.h"
#include "../core/pinnacle_lock.h"
#include "../core/pinnacle_device.h" /* PINNACLE_VID, POSIX hotplug filter */
#include "../core/pin_log.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define INITGUID
#include <windows.h>
#include <initguid.h>
#include <cfgmgr32.h>
#include <usbiodef.h>
#else
#include <libusb-1.0/libusb.h>
#endif

static void sleep_ms(int ms)
{
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static uint64_t g_generation;      /* bumped whenever the device set / lock state may have changed */
static int g_wake_requested;       /* pin_devices_wake(): force the next check to report timeout */
static int g_watcher_started;
static pthread_t g_watcher_thread;

#if defined(_WIN32)
static volatile int g_os_event_pending;

static DWORD CALLBACK cm_notify_cb(HCMNOTIFICATION notify, PVOID context,
                                    CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA data,
                                    DWORD data_size)
{
    (void)notify; (void)context; (void)action; (void)data; (void)data_size;
    pthread_mutex_lock(&g_mtx);
    g_os_event_pending = 1;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_mtx);
    return ERROR_SUCCESS;
}
#else
static volatile int g_os_event_pending;
static int g_hotplug_has_cap;
static libusb_hotplug_callback_handle g_hotplug_handle;

static int LIBUSB_CALL hotplug_cb(libusb_context *ctx, libusb_device *dev,
                                   libusb_hotplug_event event, void *user_data)
{
    (void)ctx; (void)dev; (void)event; (void)user_data;
    pthread_mutex_lock(&g_mtx);
    g_os_event_pending = 1;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_mtx);
    return 0; /* keep the callback registered */
}
#endif

/* Cheap fingerprint of the current device set's identity + lock state
 * (Ready/Preparing/In use/owner pid) -- see this file's header comment.
 * FNV-1a over id strings folded with each device's state/owner/lock-state,
 * plus the device count so a device disappearing always changes the hash
 * even if the survivors happen to hash the same. */
static pinnacle_enum_entry_t g_known[16]; /* watcher thread only */
static int g_known_n;

static void rescan_devices(void)
{
    int n = pinnacle_enumerate(g_known, 16);
    g_known_n = n < 0 ? 0 : n > 16 ? 16 : n;
}

static uint64_t device_fingerprint(void)
{
    const pinnacle_enum_entry_t *entries = g_known;
    int n = g_known_n;

    uint64_t h = 1469598103934665603ull; /* FNV-1a offset basis */
    for (int i = 0; i < n; i++) {
        for (const char *p = entries[i].id; *p; p++) {
            h ^= (unsigned char)*p;
            h *= 1099511628211ull;
        }
        pinnacle_lock_info_t li;
        memset(&li, 0, sizeof(li));
        pinnacle_lock_query(entries[i].id, &li);
        uint32_t mix[3] = {
            (uint32_t)entries[i].state,
            li.held ? li.owner_pid : 0,
            li.held ? (uint32_t)li.state : 0xFFFFFFFFu,
        };
        for (int k = 0; k < 3; k++) {
            h ^= mix[k];
            h *= 1099511628211ull;
        }
    }
    h ^= (uint64_t)n * 0x9E3779B97F4A7C15ull;
    return h;
}

static void bump_generation_locked(void)
{
    g_generation++;
    pthread_cond_broadcast(&g_cond);
}

static void register_os_hotplug(void)
{
#if defined(_WIN32)
    CM_NOTIFY_FILTER filter;
    memset(&filter, 0, sizeof(filter));
    filter.cbSize = sizeof(filter);
    filter.Flags = CM_NOTIFY_FILTER_FLAG_ALL_INTERFACE_CLASSES;
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    filter.u.DeviceInterface.ClassGuid = GUID_DEVINTERFACE_USB_DEVICE;
    HCMNOTIFICATION notify = NULL;
    /* Registered once, lazily, and never unregistered -- kept for the
     * process lifetime, per pin_api.h's pin_devices_wait() doc comment.
     * A failure here still leaves the loop's periodic rescan (below) as a
     * fallback, so hotplug is slower but not silently broken. */
    CONFIGRET cr = CM_Register_Notification(&filter, NULL, cm_notify_cb, &notify);
    if (cr != CR_SUCCESS)
        pin_logf(PIN_LOG_WARN, "pin_devices_wait: CM_Register_Notification failed (0x%lx)\n", (unsigned long)cr);
#else
    int rc = libusb_init(NULL);
    if (rc == 0 && libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
        g_hotplug_has_cap = 1;
        libusb_hotplug_register_callback(
            NULL, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
            LIBUSB_HOTPLUG_ENUMERATE, PINNACLE_VID, LIBUSB_HOTPLUG_MATCH_ANY,
            LIBUSB_HOTPLUG_MATCH_ANY, hotplug_cb, NULL, &g_hotplug_handle);
    }
#endif
}

static void *watcher_main(void *arg)
{
    (void)arg;
    register_os_hotplug();
    rescan_devices();
    uint64_t last_fp = device_fingerprint();
    int polls = 0;

    for (;;) {
        int rescan = 0;
#if defined(_WIN32)
        /* Windows delivers hotplug via the CM callback (any thread); wait
         * up to 500 ms for one, or just fall through to the fingerprint
         * poll (also covers lock-state changes CM_Register_Notification
         * knows nothing about). A periodic rescan every ~2 s is still
         * forced below, the same as the POSIX no-hotplug-capability path,
         * so a missed or never-registered CM notification (e.g. arrival
         * while the app started with no device present) doesn't leave
         * g_known stale for the rest of the process's life. */
        pthread_mutex_lock(&g_mtx);
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 500000000L;
        if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
        while (!g_os_event_pending)
            if (pthread_cond_timedwait(&g_cond, &g_mtx, &deadline) != 0)
                break;
        int had_event = g_os_event_pending;
        g_os_event_pending = 0;
        pthread_mutex_unlock(&g_mtx);
        if (had_event) {
            sleep_ms(300); /* debounce: coalesce the burst a real plug causes */
            pthread_mutex_lock(&g_mtx);
            g_os_event_pending = 0; /* anything that arrived during the debounce too */
            pthread_mutex_unlock(&g_mtx);
            rescan = 1;
        }
        if ((++polls % 4) == 0)
            rescan = 1; /* safety-net rescan every ~2 s regardless of CM notifications */
#else
        if (g_hotplug_has_cap) {
            struct timeval tv = { 0, 500000 };
            libusb_handle_events_timeout(NULL, &tv);
            pthread_mutex_lock(&g_mtx);
            int had_event = g_os_event_pending;
            g_os_event_pending = 0;
            pthread_mutex_unlock(&g_mtx);
            if (had_event) {
                sleep_ms(300); /* debounce */
                rescan = 1;
            }
        } else {
            sleep_ms(500);
            rescan = (++polls % 4) == 0; /* no hotplug capability: rescan every 2 s */
        }
#endif
        if (rescan)
            rescan_devices();
        uint64_t fp = device_fingerprint();
        if (fp != last_fp) {
            last_fp = fp;
            pthread_mutex_lock(&g_mtx);
            bump_generation_locked();
            pthread_mutex_unlock(&g_mtx);
        }
        /* Also wake any waiter blocked only because pin_devices_wake() was
         * called, even with no fingerprint change (see pin_session_devices_wait()). */
        pthread_mutex_lock(&g_mtx);
        if (g_wake_requested)
            pthread_cond_broadcast(&g_cond);
        pthread_mutex_unlock(&g_mtx);
    }
    return NULL; /* unreachable: this thread runs for the process lifetime */
}

static void ensure_watcher_started(void)
{
    if (g_watcher_started)
        return;
    if (pthread_create(&g_watcher_thread, NULL, watcher_main, NULL) == 0) {
        pthread_detach(g_watcher_thread);
        g_watcher_started = 1;
    }
}

int pin_session_devices_wait(int timeout_ms)
{
    pthread_mutex_lock(&g_mtx);
    ensure_watcher_started();
    if (!g_watcher_started) {
        pthread_mutex_unlock(&g_mtx);
        return -1;
    }

    uint64_t seen = g_generation;
    int infinite = timeout_ms < 0;
    struct timespec deadline;
    if (!infinite) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    }

    int rc = 0;
    for (;;) {
        if (g_wake_requested) {
            g_wake_requested = 0;
            rc = 0;
            break;
        }
        if (g_generation != seen) {
            rc = 1;
            break;
        }
        int w = infinite ? pthread_cond_wait(&g_cond, &g_mtx)
                          : pthread_cond_timedwait(&g_cond, &g_mtx, &deadline);
        if (w != 0) {
            rc = 0; /* timeout, or a wait error treated the same way */
            break;
        }
    }
    pthread_mutex_unlock(&g_mtx);
    return rc;
}

void pin_session_devices_wake(void)
{
    pthread_mutex_lock(&g_mtx);
    g_wake_requested = 1;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_mtx);
}
