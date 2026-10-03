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
 * marvin-core: the exported ABI (pin_api.h). Every function here is a
 * thin wrapper over src/engine (mostly pin_session.h) and the hardware-free
 * engine modules (pin_settings.h, pin_script.h) -- no logic of its own
 * beyond argument checking and translating between this library's frozen
 * public types and the engine's internal ones. See src/engine/pin_session.h
 * for what actually happens.
 */

#include "pin_api.h"
#include "../engine/pin_session.h"
#include "../engine/pin_session_priv.h" /* pin_session_push_event(NULL, ...) and s->preview */
#include "../engine/pin_deck.h"
#include "../engine/pin_settings.h"
#include "../engine/pin_script.h"
#include "../engine/pin_stop.h"
#include "../engine/pin_usb_topology.h"
#include "../core/pinnacle_enum.h"
#include "../core/pinnacle_lock.h"
#include "../core/pin_log.h"

#include <math.h>
#include <stddef.h> /* offsetof */
#include <pthread.h>
#include <stdio.h>
#include <unistd.h> /* getpid (MinGW provides it too) */
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */

/* ========================================================================
 * version / errors
 * ==================================================================== */

uint32_t pin_api_version(void) { return PIN_API_VERSION; }

const char *pin_version_string(void)
{
#ifndef PIN_GIT_DESCRIBE
#define PIN_GIT_DESCRIBE "unknown"
#endif
    return "marvin-core 0.1 (" PIN_GIT_DESCRIBE ")";
}

const char *pin_strerror(pin_status_t s)
{
    switch (s) {
    case PIN_OK: return "ok";
    case PIN_ERR_ABI: return "ABI/struct size mismatch";
    case PIN_ERR_ARG: return "invalid argument";
    case PIN_ERR_STATE: return "not allowed in the current state";
    case PIN_ERR_NOT_FOUND: return "device not found";
    case PIN_ERR_BUSY: return "device is in use by another process";
    case PIN_ERR_NO_DRIVER: return "device driver not bound (Windows: use Zadig)";
    case PIN_ERR_USB: return "USB error";
    case PIN_ERR_FIRMWARE: return "FPGA bitstream problem";
    case PIN_ERR_NOT_READY: return "device needs a power cycle (replug)";
    case PIN_ERR_NO_CAMERA: return "no camera/deck found on the 1394 bus";
    case PIN_ERR_DECK: return "deck command rejected or timed out";
    case PIN_ERR_IO: return "file system error";
    case PIN_ERR_EXISTS: return "output file already exists";
    case PIN_ERR_DISK_FULL: return "disk full";
    case PIN_ERR_CODEC: return "muxer/encoder error";
    case PIN_ERR_NOMEM: return "out of memory";
    case PIN_ERR_INTERNAL: return "internal error";
    }
    return "?";
}

pin_status_t pin_set_firmware_dir(const char *utf8_dir)
{
    return pin_session_set_firmware_dir(utf8_dir);
}

/* ========================================================================
 * devices / enumeration
 * ==================================================================== */

/* GUIDs of free devices, probed once per plug-in. The key includes the USB
 * device address, which the OS assigns anew on every plug-in, so a cached
 * GUID can't end up on a different unit moved to the same port. */
typedef struct {
    char id[PINNACLE_ENUM_ID_MAX];
    uint8_t usb_address;
    int probed;             /* tried, whether or not it answered */
    uint32_t guid_hi, guid_lo;
    int guid_known;
} guid_cache_entry_t;

static guid_cache_entry_t g_guid_cache[16];
static pthread_mutex_t g_guid_cache_mtx = PTHREAD_MUTEX_INITIALIZER;

static guid_cache_entry_t *guid_cache_slot(const pinnacle_enum_entry_t *e)
{
    guid_cache_entry_t *free_slot = NULL;
    for (int i = 0; i < 16; i++) {
        guid_cache_entry_t *c = &g_guid_cache[i];
        if (c->id[0] && strcmp(c->id, e->id) == 0) {
            if (c->usb_address != e->usb_address) { /* replugged: forget it */
                memset(c, 0, sizeof(*c));
                snprintf(c->id, sizeof(c->id), "%s", e->id);
                c->usb_address = e->usb_address;
            }
            return c;
        }
        if (!c->id[0] && !free_slot)
            free_slot = c;
    }
    if (!free_slot)
        free_slot = &g_guid_cache[0];
    memset(free_slot, 0, sizeof(*free_slot));
    snprintf(free_slot->id, sizeof(free_slot->id), "%s", e->id);
    free_slot->usb_address = e->usb_address;
    return free_slot;
}

/* Reads a free device's GUID under the cross-process lock, so it can never
 * run while another window is bringing the device up or capturing. Takes
 * a few milliseconds; a device that doesn't answer is just left without a
 * serial. */
static void probe_guid(guid_cache_entry_t *c)
{
    c->probed = 1;
    pinnacle_lock_t *lock = NULL;
    if (pinnacle_lock_acquire(c->id, &lock) != PINNACLE_OK)
        return;
    pinnacle_device_t dev;
    memset(&dev, 0, sizeof(dev));
    if (pinnacle_open_by_id(&dev, c->id) == PINNACLE_OK) {
        if (pinnacle_read_guid(&dev, &c->guid_hi, &c->guid_lo) == PINNACLE_OK)
            c->guid_known = 1;
        pinnacle_close(&dev);
    }
    pinnacle_lock_release(lock);
}

/* The caller's array stride: its sizeof(pin_device_info_t) as compiled
 * (out[0].size), so a caller built before an appended field still gets its
 * own layout; 0 or anything below the oldest layout means ours. */
static size_t device_info_stride(const pin_device_info_t *out)
{
    size_t s = out->size;
    return s >= offsetof(pin_device_info_t, hub_depth) ? s : sizeof(pin_device_info_t);
}

/* Copies a filled entry into the caller's slot, no more than it has room for. */
static void device_info_store(pin_device_info_t *out, int index, size_t stride, pin_device_info_t *d)
{
    d->size = (uint32_t)stride;
    memcpy((char *)out + (size_t)index * stride, d, stride < sizeof(*d) ? stride : sizeof(*d));
}

const char *pin_usb_hub_hint(void) { return pin_usb_hub_hint_text(); }

int pin_enumerate(pin_device_info_t *out, int max)
{
    pinnacle_enum_entry_t entries[16];
    int n = pinnacle_enumerate(entries, 16);
    int total = n > 16 ? 16 : n;
    int written = 0;
    size_t stride = out && max > 0 ? device_info_stride(out) : sizeof(pin_device_info_t);
    if (!out)
        max = 0;

    for (int i = 0; i < total && written < max; i++) {
        pin_device_info_t dev_buf;
        pin_device_info_t *d = &dev_buf;
        memset(d, 0, sizeof(*d));
        strncpy(d->id, entries[i].id, sizeof(d->id) - 1);
        strncpy(d->name, entries[i].name, sizeof(d->name) - 1);
        d->vid = entries[i].vid;
        d->pid = entries[i].pid;
        d->owner_pid = entries[i].owner_pid;
        d->tested = (uint32_t)entries[i].tested;
        d->hub_depth = pin_usb_hub_depth(entries[i].ports, entries[i].port_count);
        switch (entries[i].state) {
        case PINNACLE_ENUM_READY: d->state = PIN_DEV_READY; break;
        case PINNACLE_ENUM_IN_USE: d->state = PIN_DEV_IN_USE; break;
        case PINNACLE_ENUM_NO_DRIVER: d->state = PIN_DEV_NO_DRIVER; break;
        case PINNACLE_ENUM_NO_PERMISSION: d->state = PIN_DEV_NO_DRIVER; break;
        case PINNACLE_ENUM_UNSUPPORTED: d->state = PIN_DEV_UNSUPPORTED; break;
        }

        /* Serial: the owner publishes the GUID in the lock record; a free
         * unit is probed once per plug-in. */
        uint32_t ghi = 0, glo = 0;
        int gknown = 0;
        pinnacle_lock_info_t li;
        if (entries[i].state == PINNACLE_ENUM_IN_USE &&
            pinnacle_lock_query(entries[i].id, &li) == PINNACLE_OK && li.held) {
            if (li.owner_pid == (uint32_t)getpid())
                d->state = PIN_DEV_OPEN_HERE;
            else if (li.state == PINNACLE_LOCK_PREPARING)
                d->state = PIN_DEV_PREPARING;
            if (li.guid_known) { ghi = li.guid_hi; glo = li.guid_lo; gknown = 1; }
        }
        if (!gknown && entries[i].state != PINNACLE_ENUM_UNSUPPORTED &&
            entries[i].state != PINNACLE_ENUM_NO_DRIVER) {
            pthread_mutex_lock(&g_guid_cache_mtx);
            guid_cache_entry_t *c = guid_cache_slot(&entries[i]);
            if (!c->probed && entries[i].state == PINNACLE_ENUM_READY)
                probe_guid(c);
            if (c->guid_known) { ghi = c->guid_hi; glo = c->guid_lo; gknown = 1; }
            pthread_mutex_unlock(&g_guid_cache_mtx);
        }
        if (gknown)
            snprintf(d->serial, sizeof(d->serial), "%08X%08X", (unsigned)ghi, (unsigned)glo);
        device_info_store(out, written, stride, d);
        written++;
    }
    int total_out = total;

    /* Replay virtual device: PIN_REPLAY=<file>, or pin_set_replay_file(),
     * shows up as one extra "replay:<basename>" entry in state READY -- see
     * the plan's "Replay/virtual device". */
    const char *replay = getenv("PIN_REPLAY");
    char global_replay[PIN_PATH_MAX] = { 0 };
    if ((!replay || !replay[0]) && pin_session_get_replay_file(global_replay, sizeof(global_replay)) == 0)
        replay = global_replay;
    if (replay && replay[0]) {
        total_out++;
        if (written < max) {
            pin_device_info_t dev_buf;
            pin_device_info_t *d = &dev_buf;
            memset(d, 0, sizeof(*d));
            const char *base = strrchr(replay, '/');
            const char *base2 = strrchr(replay, '\\');
            if (base2 && (!base || base2 > base)) base = base2;
            base = base ? base + 1 : replay;
            /* Just the file name: the id field is short, and pin_open()
             * maps it back to the configured full path. */
            snprintf(d->id, sizeof(d->id), "replay:%.55s", base);
            snprintf(d->name, sizeof(d->name), "Replay: %.55s", base);
            strncpy(d->serial, "REPLAY", sizeof(d->serial) - 1);
            d->state = PIN_DEV_READY;
            d->tested = 1;
            d->hub_depth = -1;   /* a file, not a USB device */
            device_info_store(out, written, stride, d);
            written++;
        }
    }
    return total_out;
}

void pin_set_replay_file(const char *path) { pin_session_set_replay_file(path); }

int pin_devices_wait(int timeout_ms) { return pin_session_devices_wait(timeout_ms); }

void pin_devices_wake(void) { pin_session_devices_wake(); }

/* ========================================================================
 * sessions
 * ==================================================================== */

pin_status_t pin_open(const char *device_id, pin_session_t **out)
{
    return pin_session_open(device_id, out);
}

void pin_close(pin_session_t *s) { pin_session_close(s); }

pin_status_t pin_set_input(pin_session_t *s, pin_input_t input)
{
    return pin_session_set_input(s, input);
}

/* ========================================================================
 * analog controls
 * ==================================================================== */

const char *pin_std_name(pin_std_t std)
{
    static const char *names[PIN_STD_COUNT] = {
        "Auto", "PAL", "NTSC", "PAL-M", "PAL-N", "PAL-60", "NTSC 4.43", "NTSC-J", "SECAM",
    };
    return ((unsigned)std < PIN_STD_COUNT) ? names[std] : "?";
}

pin_status_t pin_set_standard(pin_session_t *s, pin_std_t std)
{
    return pin_session_set_standard(s, std);
}

pin_status_t pin_get_control(pin_session_t *s, pin_control_t c, pin_control_info_t *out)
{
    return pin_session_get_control(s, c, out);
}

pin_status_t pin_set_control(pin_session_t *s, pin_control_t c, int32_t value)
{
    return pin_session_set_control(s, c, value);
}

/* ========================================================================
 * deck
 * ==================================================================== */

pin_status_t pin_deck(pin_session_t *s, pin_deck_cmd_t cmd)
{
    return pin_session_deck(s, cmd);
}

/* ========================================================================
 * formats
 * ==================================================================== */

int pin_formats(pin_kind_t kind, pin_format_info_t *out, int max)
{
    return pin_session_formats(kind, out, max);
}

pin_status_t pin_format_info(pin_format_t f, pin_format_info_t *out)
{
    return pin_session_format_info(f, out);
}

/* ========================================================================
 * capture
 * ==================================================================== */

void pin_capture_opts_defaults(pin_capture_opts_t *o)
{
    memset(o, 0, sizeof(*o));
    o->size = sizeof(*o);
    o->format_analog = PIN_FMT_ANALOG_AVI;
    o->format_dv = PIN_FMT_DV_RAW;
    o->format_hdv = PIN_FMT_HDV_TS;
    o->aspect = PIN_ASPECT_AUTO;
    o->passes = 1;
}

int pin_capture_passes_allowed(int idle_stop_minutes, int max_duration_minutes)
{
    return idle_stop_minutes > 0 || max_duration_minutes > 0;
}

int pin_capture_opts_normalize(pin_capture_opts_t *o)
{
    if (!o)
        return 0;
    int want = o->passes < 1 ? 1 : o->passes;
    if (want > 1 && !pin_capture_passes_allowed(o->idle_stop_minutes, o->max_duration_minutes))
        want = 1;
    int changed = want != o->passes;
    o->passes = want;
    return changed;
}

pin_status_t pin_check_output(pin_session_t *s, const pin_capture_opts_t *o,
                               pin_output_check_t *out)
{
    return pin_session_check_output(s, o, out);
}

pin_status_t pin_set_output_hint(pin_session_t *s, const pin_capture_opts_t *o)
{
    return pin_session_set_output_hint(s, o);
}

pin_status_t pin_capture_start(pin_session_t *s, const pin_capture_opts_t *o, int overwrite)
{
    return pin_session_capture_start(s, o, overwrite);
}

pin_status_t pin_capture_stop(pin_session_t *s)
{
    return pin_session_capture_stop(s, PIN_STOP_DECK_AS_STARTED);
}

pin_status_t pin_capture_stop_ex(pin_session_t *s, pin_stop_deck_t stop_deck)
{
    return pin_session_capture_stop(s, stop_deck);
}

/* ========================================================================
 * status / events
 * ==================================================================== */

pin_status_t pin_get_status(pin_session_t *s, pin_status_snapshot_t *out)
{
    return pin_session_get_status(s, out);
}

void pin_format_remaining(double seconds, char *out, size_t cap)
{
    if (!out || !cap)
        return;
    long n = seconds > 0 ? (long)(seconds + 0.999) : 0;
    if (n >= 3600)
        snprintf(out, cap, "%ldh%02ldm%02lds", n / 3600, (n / 60) % 60, n % 60);
    else if (n >= 60)
        snprintf(out, cap, "%ldm%02lds", n / 60, n % 60);
    else
        snprintf(out, cap, "%lds", n);
}

int pin_stop_reason_abnormal(pin_stop_reason_t reason)
{
    return pin_stop_abnormal(reason);
}

static double clamp01(double v)
{
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

pin_progress_mode_t pin_status_progress(const pin_status_snapshot_t *st, double idle_total_s,
                                        double duration_total_s, double *fraction)
{
    double f = 0;
    pin_progress_mode_t m = PIN_PROGRESS_NONE;
    if (st) {
        switch (st->state) {
        case PIN_STATE_ERROR:
            m = PIN_PROGRESS_ERROR;
            f = 1;
            break;
        case PIN_STATE_PREPARING:
            if (st->progress_percent >= 0) {
                m = PIN_PROGRESS_NORMAL;
                f = clamp01(st->progress_percent / 100.0);
            } else {
                m = PIN_PROGRESS_INDETERMINATE;
            }
            break;
        case PIN_STATE_STOPPING:
            m = PIN_PROGRESS_INDETERMINATE;
            break;
        case PIN_STATE_REWINDING:
            m = PIN_PROGRESS_PAUSED;
            f = 1;
            break;
        case PIN_STATE_CAPTURING:
            if (!st->signal && idle_total_s > 0 && st->idle_stop_remaining_s >= 0) {
                m = PIN_PROGRESS_NORMAL;
                f = clamp01(st->idle_stop_remaining_s / idle_total_s);
            } else if (duration_total_s > 0 && st->duration_remaining_s >= 0) {
                m = PIN_PROGRESS_NORMAL;
                f = clamp01(1.0 - st->duration_remaining_s / duration_total_s);
            } else if (!st->signal) {
                m = PIN_PROGRESS_PAUSED;
                f = 1;
            } else {
                m = PIN_PROGRESS_INDETERMINATE;
            }
            /* Low disk space turns the bar yellow, keeping the value a bar mode had
             * (a countdown or limit stays readable), else full. Only while writing. */
            if (st->disk_low) {
                if (m != PIN_PROGRESS_NORMAL)
                    f = 1;
                m = PIN_PROGRESS_PAUSED;
            }
            break;
        default:
            break;
        }
    }
    if (fraction)
        *fraction = f;
    return m;
}

static const char *state_name(pin_state_t st)
{
    switch (st) {
    case PIN_STATE_CLOSED: return "Closed";
    case PIN_STATE_PREPARING: return "Preparing";
    case PIN_STATE_READY: return "Ready";
    case PIN_STATE_CAPTURING: return "Capturing";
    case PIN_STATE_STOPPING: return "Stopping";
    case PIN_STATE_REWINDING: return "Rewinding";
    case PIN_STATE_ERROR: return "Error";
    }
    return "?";
}

static const char *deck_name(pin_deck_state_t d)
{
    switch (d) {
    case PIN_DECK_UNKNOWN: return "";
    case PIN_DECK_STOPPED: return "Stopped";
    case PIN_DECK_PLAYING: return "Playing";
    case PIN_DECK_PAUSED: return "Paused";
    case PIN_DECK_FAST_FORWARD: return "FF";
    case PIN_DECK_REWINDING: return "REW";
    case PIN_DECK_RECORDING: return "Recording";
    case PIN_DECK_NO_TAPE: return "No tape";
    }
    return "";
}

void pin_format_status_line(const pin_status_snapshot_t *st, char *out, size_t cap)
{
    if (!st || !out || cap == 0)
        return;
    char tc[32] = "";
    if (st->timecode[0])
        snprintf(tc, sizeof(tc), " %s", st->timecode);
    char deck[24] = "";
    if (st->deck != PIN_DECK_UNKNOWN)
        snprintf(deck, sizeof(deck), " [%s]", deck_name(st->deck));

    if (st->state == PIN_STATE_CAPTURING) {
        double mb = st->bytes_written / (1024.0 * 1024.0);
        snprintf(out, cap, "REC%s%s  pass %d/%d  scene %d  %.1f MB  %.0f s  drops %llu",
                 tc, deck, st->pass, st->passes, st->scene, mb, st->elapsed_s,
                 (unsigned long long)(st->frames_dropped + st->write_dropped));
    } else if (st->state == PIN_STATE_ERROR) {
        snprintf(out, cap, "Error: %s", st->error_text);
    } else if (st->state == PIN_STATE_PREPARING && st->detail[0]) {
        snprintf(out, cap, "Preparing: %s", st->detail);
    } else {
        snprintf(out, cap, "%s%s%s%s", state_name(st->state), deck, tc,
                 st->signal ? "" : "  (no signal)");
    }
}

void pin_format_window_title(const pin_status_snapshot_t *st, const char *device_name,
                              char *out, size_t cap)
{
    if (!st || !out || cap == 0)
        return;
    /* "Capturing <timecode> - <device> - MarvinCapture"; the leading part is dropped when empty */
    char lead[48] = "";
    snprintf(lead, sizeof(lead), "%s%s%s",
             st->state == PIN_STATE_CAPTURING ? "Capturing" : "",
             (st->state == PIN_STATE_CAPTURING && st->timecode[0]) ? " " : "", st->timecode);
    snprintf(out, cap, "%s%s%s - MarvinCapture", lead, lead[0] ? " - " : "",
             device_name ? device_name : "Pinnacle 500-USB");
}

int pin_poll_event(pin_session_t *s, pin_event_t *out)
{
    return pin_session_poll_event(s, out);
}

static int g_log_level = 1;

void pin_set_log_level(int level) { g_log_level = level; }

static void api_log_sink(pin_log_level_t level, const char *msg, void *user)
{
    (void)user;
    if ((int)level < g_log_level)
        return;
    pin_session_push_event(NULL, PIN_EVT_LOG, (int32_t)level, msg);
}

__attribute__((constructor))
static void pin_api_init(void)
{
    pin_log_set_sink(api_log_sink, NULL);
}

/* ========================================================================
 * preview
 * ==================================================================== */

int pin_preview_wait(pin_session_t *s, uint64_t after_seq, int timeout_ms)
{
    return s ? pin_previewer_wait(s->preview, after_seq, timeout_ms) : -1;
}

pin_status_t pin_preview_lock(pin_session_t *s, pin_frame_t *out)
{
    if (!s || !out) return PIN_ERR_ARG;
    out->size = sizeof(*out);
    return pin_previewer_lock(s->preview, out);
}

void pin_preview_unlock(pin_session_t *s) { if (s) pin_previewer_unlock(s->preview); }

void pin_preview_enable(pin_session_t *s, int enabled) { if (s) pin_previewer_enable(s->preview, enabled); }

void pin_set_aspect(pin_session_t *s, pin_aspect_t aspect) { pin_session_set_aspect(s, aspect); }

void pin_fit_rect(int dar_num, int dar_den, int w, int h, int *x, int *y, int *rw, int *rh)
{
    if (dar_num <= 0 || dar_den <= 0 || w <= 0 || h <= 0) {
        if (x) *x = 0;
        if (y) *y = 0;
        if (rw) *rw = w;
        if (rh) *rh = h;
        return;
    }
    double target = (double)dar_num / dar_den;
    double surface = (double)w / h;
    int out_w, out_h;
    if (surface > target) {
        out_h = h;
        out_w = (int)(h * target + 0.5);
    } else {
        out_w = w;
        out_h = (int)(w / target + 0.5);
    }
    if (x) *x = (w - out_w) / 2;
    if (y) *y = (h - out_h) / 2;
    if (rw) *rw = out_w;
    if (rh) *rh = out_h;
}

/* BT.601/BT.709 Y'CbCr -> R'G'B', limited or full range, as a 3x4
 * row-major matrix expecting 0..1 normalised Y'CbCr samples (Cb/Cr already
 * shifted so 0.5 is neutral). Standard ITU-R conversion constants. */
void pin_yuv_to_rgb_matrix(pin_matrix_t m, int full_range, float out[12])
{
    double kb, kr;
    if (m == PIN_MATRIX_BT709) { kb = 0.0722; kr = 0.2126; }
    else { kb = 0.114; kr = 0.299; } /* BT.601 */
    double kg = 1.0 - kb - kr;

    double y_scale = full_range ? 1.0 : 255.0 / 219.0;
    double y_off = full_range ? 0.0 : -16.0 / 255.0 * y_scale;
    double c_scale = full_range ? 1.0 : 255.0 / 224.0;

    double r_v = 2.0 * (1.0 - kr) * c_scale;
    double b_u = 2.0 * (1.0 - kb) * c_scale;
    double g_u = -2.0 * (1.0 - kb) * kb / kg * c_scale;
    double g_v = -2.0 * (1.0 - kr) * kr / kg * c_scale;

    /* rows: R, G, B; columns: Y, Cb, Cr, offset. The samples go in as they
     * are, 0..1 (pin_api.h), so centring the chroma on 0.5 is folded into
     * the offset column rather than left to each GUI's shader. */
    out[0] = (float)y_scale; out[1] = 0.0f;         out[2] = (float)r_v;
    out[3] = (float)(y_off - 0.5 * r_v);
    out[4] = (float)y_scale; out[5] = (float)g_u;    out[6] = (float)g_v;
    out[7] = (float)(y_off - 0.5 * (g_u + g_v));
    out[8] = (float)y_scale; out[9] = (float)b_u;    out[10] = 0.0f;
    out[11] = (float)(y_off - 0.5 * b_u);
}

/* ========================================================================
 * audio monitoring
 * ==================================================================== */

void pin_monitor_enable(pin_session_t *s, int enabled) { pin_session_monitor_enable(s, enabled); }

int pin_monitor_read(pin_session_t *s, int16_t *out, int max_frames)
{
    return pin_session_monitor_read(s, out, max_frames);
}

int pin_monitor_available(pin_session_t *s)
{
    return pin_session_monitor_available(s);
}

/* ========================================================================
 * settings
 * ==================================================================== */

pin_status_t pin_settings_get(const char *key, char *out, size_t cap)
{
    if (!key || !out || cap == 0)
        return PIN_ERR_ARG;
    char path[PIN_PATH_MAX];
    if (pin_settings_default_path(path, sizeof(path)) != 0)
        return PIN_ERR_IO;
    pin_settings_t st;
    pin_settings_init(&st);
    pin_settings_load(&st, path); /* missing file: st stays empty, not an error */

    char section[PIN_NAME_MAX] = "General";
    const char *dot = strchr(key, '.');
    const char *k = key;
    if (dot) {
        size_t n = (size_t)(dot - key);
        if (n < sizeof(section)) { memcpy(section, key, n); section[n] = 0; }
        k = dot + 1;
    }
    const char *v = pin_settings_get_string(&st, section, k, "");
    strncpy(out, v, cap - 1);
    out[cap - 1] = 0;
    pin_settings_free(&st);
    return PIN_OK;
}

pin_status_t pin_device_settings_key(const char *serial, const char *id, const char *key,
                                    char *out, size_t cap)
{
    if (!out || cap == 0)
        return PIN_ERR_ARG;
    return pin_settings_device_key(serial, id, key, out, cap) == 0 ? PIN_OK : PIN_ERR_ARG;
}

pin_status_t pin_settings_set(const char *key, const char *value)
{
    if (!key || !value)
        return PIN_ERR_ARG;
    char path[PIN_PATH_MAX];
    if (pin_settings_default_path(path, sizeof(path)) != 0)
        return PIN_ERR_IO;
    pin_settings_t st;
    pin_settings_init(&st);
    pin_settings_load(&st, path);

    char section[PIN_NAME_MAX] = "General";
    const char *dot = strchr(key, '.');
    const char *k = key;
    if (dot) {
        size_t n = (size_t)(dot - key);
        if (n < sizeof(section)) { memcpy(section, key, n); section[n] = 0; }
        k = dot + 1;
    }
    pin_settings_set_string(&st, section, k, value);
    int rc = pin_settings_save(&st, path);
    pin_settings_free(&st);
    return rc == 0 ? PIN_OK : PIN_ERR_IO;
}

/* ========================================================================
 * command line: settings + actions (engine/pin_script*.c)
 * ==================================================================== */

pin_status_t pin_script_parse(int argc, const char *const *argv, pin_script_t **out, char *err,
                              size_t err_cap)
{
    return pin_script_parse_args(argc, argv, out, err, err_cap);
}

void pin_script_free(pin_script_t *sc) { pin_script_destroy(sc); }

const char *pin_script_help(void) { return pin_script_help_text(); }

int pin_script_help_requested(const pin_script_t *sc) { return sc && sc->help; }

const char *pin_script_device(const pin_script_t *sc)
{
    return sc && sc->has_device ? sc->device : NULL;
}

int pin_script_debug(const pin_script_t *sc) { return sc && sc->debug; }

int pin_script_step_count(const pin_script_t *sc) { return sc ? sc->nsteps : 0; }

pin_status_t pin_script_step_text(const pin_script_t *sc, int index, char *out, size_t cap)
{
    return pin_script_step_description(sc, index, out, cap);
}

int pin_script_needs_session(const pin_script_t *sc) { return sc && sc->nsteps > 0; }

pin_status_t pin_script_settings(const pin_script_t *sc, pin_script_settings_t *out)
{
    if (!sc || !out)
        return PIN_ERR_ARG;
    if (out->size != sizeof(*out))
        return PIN_ERR_ABI;
    *out = sc->initial;
    out->size = sizeof(*out);
    return PIN_OK;
}

pin_status_t pin_script_run(pin_session_t *s, const pin_script_t *sc)
{
    return pin_session_script_run(s, sc);
}

pin_status_t pin_script_cancel(pin_session_t *s)
{
    return pin_session_script_cancel(s);
}
