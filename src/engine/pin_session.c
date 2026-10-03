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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* dladdr / Dl_info on glibc */
#endif
#include "pin_session_priv.h"
#include "pin_session.h"
#include "dv_subcode.h"
#include "dv_audio.h"
#include "hdv_aux.h"
#include "dv_error.h"
#include "hdv_error.h"
#include "pin_estimate.h"
#include "pin_stop.h"
#include "pin_vidfmt.h"
#include "pin_naming.h"
#include "pin_settings.h"
#include "../core/pinnacle_enum.h"
#include "../core/pinnacle_cfg.h"
#include "../core/pin_log.h"
#include "../sinks/sinks_internal.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h> /* dladdr */
#include <sys/statvfs.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/vfs.h>
#include <linux/magic.h>
#endif
#include <unistd.h>
#endif

/* ========================================================================
 * time / small helpers
 * ==================================================================== */

double pin_session_now(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    static int have_freq;
    if (!have_freq) { QueryPerformanceFrequency(&freq); have_freq = 1; }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
#endif
}

static void sleep_ms(int ms)
{
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

void pin_session_lock(pin_session_t *s) { pthread_mutex_lock(&s->mtx); }
void pin_session_unlock(pin_session_t *s) { pthread_mutex_unlock(&s->mtx); }

/* ========================================================================
 * event queue
 * ==================================================================== */

/* Process-wide events (s == NULL): the core's log lines, which pin_log
 * routes here through pin_api.c's sink whether or not a session is open. */
static pin_event_t g_evq[PIN_EVQ_CAP];
static unsigned g_evq_head, g_evq_count;
static pthread_mutex_t g_evq_mtx = PTHREAD_MUTEX_INITIALIZER;

static void evq_push(pin_event_t *q, unsigned *head, unsigned *count, pthread_mutex_t *mtx,
                     pin_event_kind_t kind, int32_t a, const char *text)
{
    pthread_mutex_lock(mtx);
    if (*count == PIN_EVQ_CAP) {
        /* drop the oldest LOG event if any, else the oldest event of any kind */
        unsigned victim = *head;
        for (unsigned i = 0; i < *count; i++) {
            unsigned idx = (*head + i) % PIN_EVQ_CAP;
            if (q[idx].kind == PIN_EVT_LOG) { victim = idx; break; }
        }
        /* Close the gap: everything older than the victim moves up one
         * slot, keeping its order, and the freed head slot is dropped. */
        for (unsigned i = victim; i != *head; ) {
            unsigned prev = (i + PIN_EVQ_CAP - 1) % PIN_EVQ_CAP;
            q[i] = q[prev];
            i = prev;
        }
        *head = (*head + 1) % PIN_EVQ_CAP;
        (*count)--;
    }
    unsigned tail = (*head + *count) % PIN_EVQ_CAP;
    pin_event_t *e = &q[tail];
    e->size = sizeof(*e);
    e->kind = kind;
    e->a = a;
    e->text[0] = 0;
    if (text) {
        strncpy(e->text, text, sizeof(e->text) - 1);
        e->text[sizeof(e->text) - 1] = 0;
    }
    (*count)++;
    pthread_mutex_unlock(mtx);
}

static int evq_poll(pin_event_t *q, unsigned *head, unsigned *count, pthread_mutex_t *mtx,
                    pin_event_t *out)
{
    pthread_mutex_lock(mtx);
    if (*count == 0) {
        pthread_mutex_unlock(mtx);
        return 0;
    }
    *out = q[*head];
    *head = (*head + 1) % PIN_EVQ_CAP;
    (*count)--;
    pthread_mutex_unlock(mtx);
    return 1;
}

void pin_session_push_event(pin_session_t *s, pin_event_kind_t kind, int32_t a, const char *text)
{
    if (!s)
        evq_push(g_evq, &g_evq_head, &g_evq_count, &g_evq_mtx, kind, a, text);
    else
        evq_push(s->evq, &s->evq_head, &s->evq_count, &s->evq_mtx, kind, a, text);
}

int pin_session_poll_event(pin_session_t *s, pin_event_t *out)
{
    if (!s)
        return evq_poll(g_evq, &g_evq_head, &g_evq_count, &g_evq_mtx, out);
    return evq_poll(s->evq, &s->evq_head, &s->evq_count, &s->evq_mtx, out);
}

/* How long a bring-up took last time, per input kind (DV, analog): the
 * progress bar is elapsed time over this, since the steps differ too much in
 * length (and mostly have no measurable size) for a per-step percentage. */
static double g_prepare_expected_s[2] = { 7.0, 4.0 };

static void rate_persist(pin_session_t *s);

static void set_state(pin_session_t *s, pin_state_t st)
{
    int kind = s->input == PIN_INPUT_DV ? 0 : 1;
    if (st == PIN_STATE_PREPARING && s->state != PIN_STATE_PREPARING) {
        s->prepare_start_s = pin_session_now();
    } else if (s->state == PIN_STATE_PREPARING && st == PIN_STATE_READY && s->prepare_start_s > 0) {
        double took = pin_session_now() - s->prepare_start_s;
        if (took > 0.5 && took < 60)
            g_prepare_expected_s[kind] = took;
        s->prepare_start_s = 0;
    }
    if (s->state == PIN_STATE_CAPTURING && st != PIN_STATE_CAPTURING &&
        s->stream_kind == PIN_KIND_ANALOG && s->active_format == PIN_FMT_ANALOG_FFV1_MKV)
        rate_persist(s);
    s->state = st;
    if (s->lock) {
        pinnacle_lock_state_t ls = PINNACLE_LOCK_READY;
        if (st == PIN_STATE_PREPARING || st == PIN_STATE_REWINDING)
            ls = PINNACLE_LOCK_PREPARING;
        else if (st == PIN_STATE_CAPTURING || st == PIN_STATE_STOPPING)
            ls = PINNACLE_LOCK_CAPTURING;
        pinnacle_lock_update(s->lock, ls, s->dev.guid_hi, s->dev.guid_lo);
    }
    pin_session_push_event(s, PIN_EVT_STATE, (int32_t)st, NULL);
}

static void set_error(pin_session_t *s, pin_status_t err, const char *msg)
{
    s->last_error = err;
    strncpy(s->error_text, msg ? msg : pin_strerror(err), sizeof(s->error_text) - 1);
    s->error_text[sizeof(s->error_text) - 1] = 0;
    set_state(s, PIN_STATE_ERROR);
    pin_session_push_event(s, PIN_EVT_ERROR, (int32_t)err, s->error_text);
}

/* Bring-up progress from the core (pinnacle_device_t.progress). */
static void session_progress(void *user, const char *step, int percent)
{
    pin_session_t *s = user;
    pin_session_lock(s);
    strncpy(s->step_text, step, sizeof(s->step_text) - 1);
    s->step_text[sizeof(s->step_text) - 1] = 0;
    s->progress_pct = percent;
    pin_session_unlock(s);
}

/* ========================================================================
 * firmware directory
 * ==================================================================== */

static char g_firmware_dir[PIN_PATH_MAX];
static pthread_mutex_t g_fw_mtx = PTHREAD_MUTEX_INITIALIZER;

static int exe_dir(char *out, size_t cap)
{
#if defined(_WIN32)
    wchar_t wpath[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, wpath, MAX_PATH);
    if (n == 0 || n == MAX_PATH)
        return -1;
    char path[MAX_PATH * 4];
    int len = WideCharToMultiByte(CP_UTF8, 0, wpath, (int)n, path, (int)sizeof(path), NULL, NULL);
    if (len <= 0)
        return -1;
    path[len] = 0;
    char *slash = strrchr(path, '\\');
    char *slash2 = strrchr(path, '/');
    if (slash2 && (!slash || slash2 > slash))
        slash = slash2;
    if (!slash)
        return -1;
    size_t n2 = (size_t)(slash - path);
    if (n2 + 1 > cap)
        return -1;
    memcpy(out, path, n2);
    out[n2] = 0;
    return 0;
#elif defined(__APPLE__)
    (void)out; (void)cap;
    return -1; /* _NSGetExecutablePath needs <mach-o/dyld.h>; not wired up in this Windows/Linux pass */
#else
    char path[4096];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0)
        return -1;
    path[n] = 0;
    char *slash = strrchr(path, '/');
    if (!slash)
        return -1;
    size_t n2 = (size_t)(slash - path);
    if (n2 + 1 > cap)
        return -1;
    memcpy(out, path, n2);
    out[n2] = 0;
    return 0;
#endif
}

pin_status_t pin_session_set_firmware_dir(const char *utf8_dir)
{
    pthread_mutex_lock(&g_fw_mtx);
    if (!utf8_dir) {
        g_firmware_dir[0] = 0;
    } else {
        strncpy(g_firmware_dir, utf8_dir, sizeof(g_firmware_dir) - 1);
        g_firmware_dir[sizeof(g_firmware_dir) - 1] = 0;
    }
    pthread_mutex_unlock(&g_fw_mtx);
    return PIN_OK;
}

/* ========================================================================
 * replay (virtual device) source -- process-global, see pin_api.h's
 * pin_set_replay_file(). Replaces the old settings key "replay.file";
 * PIN_REPLAY (used by ctest) is still read directly by every caller and
 * takes precedence, exactly as before.
 * ==================================================================== */

static char g_replay_file[PIN_PATH_MAX];
static pthread_mutex_t g_replay_mtx = PTHREAD_MUTEX_INITIALIZER;

void pin_session_set_replay_file(const char *path)
{
    pthread_mutex_lock(&g_replay_mtx);
    if (!path) {
        g_replay_file[0] = 0;
    } else {
        strncpy(g_replay_file, path, sizeof(g_replay_file) - 1);
        g_replay_file[sizeof(g_replay_file) - 1] = 0;
    }
    pthread_mutex_unlock(&g_replay_mtx);
}

int pin_session_get_replay_file(char *out, size_t out_size)
{
    pthread_mutex_lock(&g_replay_mtx);
    int have = g_replay_file[0] != 0;
    if (have) {
        strncpy(out, g_replay_file, out_size - 1);
        out[out_size - 1] = 0;
    }
    pthread_mutex_unlock(&g_replay_mtx);
    return have ? 0 : -1;
}

/* Directory of the module that contains this code: the marvin-core
 * DLL / .so / .dylib, or the executable when the core is linked in
 * statically (tests). */
static int lib_dir(char *out, size_t cap)
{
#if defined(_WIN32)
    HMODULE mod = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(void *)&lib_dir, &mod))
        return -1;
    wchar_t wpath[MAX_PATH];
    DWORD n = GetModuleFileNameW(mod, wpath, MAX_PATH);
    if (n == 0 || n == MAX_PATH)
        return -1;
    char path[MAX_PATH * 4];
    int len = WideCharToMultiByte(CP_UTF8, 0, wpath, (int)n, path, (int)sizeof(path) - 1, NULL, NULL);
    if (len <= 0)
        return -1;
    path[len] = 0;
#else
    Dl_info info;
    if (!dladdr((void *)&lib_dir, &info) || !info.dli_fname)
        return -1;
    char path[4096];
    if (!realpath(info.dli_fname, path))
        return -1;
#endif
    char *slash = strrchr(path, '/');
#if defined(_WIN32)
    char *bs = strrchr(path, '\\');
    if (bs && (!slash || bs > slash))
        slash = bs;
#endif
    if (!slash || (size_t)(slash - path) + 1 > cap)
        return -1;
    memcpy(out, path, (size_t)(slash - path));
    out[slash - path] = 0;
    return 0;
}

/* Size of a file, or -1 if it cannot be opened. */
static long long file_size(const char *path)
{
#if defined(_WIN32)
    wchar_t wpath[PIN_PATH_MAX];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, PIN_PATH_MAX))
        return -1;
    FILE *f = _wfopen(wpath, L"rb");
#else
    FILE *f = fopen(path, "rb");
#endif
    if (!f)
        return -1;
    long long n = -1;
    if (fseek(f, 0, SEEK_END) == 0)
        n = ftell(f);
    fclose(f);
    return n;
}

#if defined(_WIN32)
#define DIR_SEP "\\"
#else
#define DIR_SEP "/"
#endif

/* Every bitstream (fpga-ohci.bin, fpga-capture.bin) is exactly this long. */
#define PIN_BITSTREAM_BYTES 78422

pin_status_t pin_session_firmware_path(const pinnacle_model_t *model, pin_kind_t for_kind,
                                       char *out, size_t out_size, char *why, size_t why_size)
{
    const char *name = NULL;
    if (model)
        name = for_kind == PIN_KIND_ANALOG ? model->analog_bitstream : model->dv_bitstream;
    if (!name) {
        if (why && why_size)
            snprintf(why, why_size, "no FPGA bitstream known for this device model");
        return PIN_ERR_FIRMWARE;
    }
    enum { MAX_DIRS = 4 };
    char dirs[MAX_DIRS][PIN_PATH_MAX];
    int ndirs = 0;

    /* 1. pin_set_firmware_dir() */
    pthread_mutex_lock(&g_fw_mtx);
    if (g_firmware_dir[0])
        snprintf(dirs[ndirs++], PIN_PATH_MAX, "%s", g_firmware_dir);
    pthread_mutex_unlock(&g_fw_mtx);

    /* 2. settings key "firmware_dir" (section "Paths") */
    {
        char settings_path[PIN_PATH_MAX];
        pin_settings_t st;
        pin_settings_init(&st);
        if (pin_settings_default_path(settings_path, sizeof(settings_path)) == 0 &&
            pin_settings_load(&st, settings_path) == 0) {
            const char *o = pin_settings_get_string(&st, "Paths", "firmware_dir", "");
            if (o[0])
                snprintf(dirs[ndirs++], PIN_PATH_MAX, "%s", o);
        }
        pin_settings_free(&st);
    }

    /* 3. firmware/ next to the core library, 4. firmware/ next to the exe */
    char base[PIN_PATH_MAX];
    if (lib_dir(base, sizeof(base)) == 0)
        snprintf(dirs[ndirs++], PIN_PATH_MAX, "%s" DIR_SEP "firmware", base);
    if (exe_dir(base, sizeof(base)) == 0) {
        char d[PIN_PATH_MAX];
        snprintf(d, sizeof(d), "%s" DIR_SEP "firmware", base);
        int dup = 0;
        for (int i = 0; i < ndirs; i++)
            dup |= strcmp(dirs[i], d) == 0;
        if (!dup)
            snprintf(dirs[ndirs++], PIN_PATH_MAX, "%s", d);
    }
    if (ndirs == 0)
        snprintf(dirs[ndirs++], PIN_PATH_MAX, "firmware");

    /* First candidate of the right size wins; a present but wrong-sized
     * file is reported as such rather than as "not found". */
    int bad = -1;
    long long bad_size = 0;
    for (int i = 0; i < ndirs; i++) {
        char path[PIN_PATH_MAX];
        if (snprintf(path, sizeof(path), "%s" DIR_SEP "%s", dirs[i], name) >= (int)sizeof(path))
            continue;
        long long n = file_size(path);
        if (n == PIN_BITSTREAM_BYTES) {
            snprintf(out, out_size, "%s", path);
            return PIN_OK;
        }
        if (n >= 0 && bad < 0) {
            bad = i;
            bad_size = n;
        }
    }

    snprintf(out, out_size, "%s" DIR_SEP "%s", dirs[bad >= 0 ? bad : 0], name);
    if (why && why_size) {
        if (bad >= 0) {
            snprintf(why, why_size,
                     "FPGA bitstream %s is %lld bytes, expected %d (wrong or truncated file)",
                     out, bad_size, PIN_BITSTREAM_BYTES);
        } else {
            int len = snprintf(why, why_size, "FPGA bitstream %s not found. Looked in:", name);
            for (int i = 0; i < ndirs && len > 0 && (size_t)len < why_size; i++)
                len += snprintf(why + len, why_size - (size_t)len, "%s %s", i ? ";" : "", dirs[i]);
        }
    }
    return PIN_ERR_FIRMWARE;
}

/* Error for a failed pinnacle_init_hardware() / pinnacle_analog_open()
 * after the bitstream file itself was found and validated. */
static void set_init_error(pin_session_t *s, pinnacle_status_t pst, const char *fw)
{
    char msg[PIN_TEXT_MAX];
    if (pst == PINNACLE_ERR_BITSTREAM_READ)
        snprintf(msg, sizeof(msg), "Cannot read FPGA bitstream %s", fw);
    else if (pst == PINNACLE_ERR_NOT_READY)
        snprintf(msg, sizeof(msg), "The device did not accept the FPGA bitstream %s: %s", fw,
                 pinnacle_strerror(pst));
    else
        snprintf(msg, sizeof(msg), "Device initialisation failed: %s", pinnacle_strerror(pst));
    set_error(s, pst == PINNACLE_ERR_BITSTREAM_READ || pst == PINNACLE_ERR_NOT_READY
                     ? PIN_ERR_FIRMWARE : PIN_ERR_USB, msg);
}

/* ========================================================================
 * device id resolution (also used by pin_api.c's pin_enumerate)
 * ==================================================================== */

/* "a path to an existing file" (pin_api.h's pin_open()) -- fopen, like
 * every other existing-file check in this codebase (replay_run(),
 * pin_settings.c), rather than stat()/S_ISREG, whose portable form differs
 * enough between MinGW and POSIX to not be worth it here. */
static int path_is_existing_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

/* Reads a free (or already-ours) device's 1394 GUID as the 16 hex char
 * serial pin_device_info_t.serial reports, without a persistent cache
 * (unlike pin_api.c's pin_enumerate(), which is called far more often) --
 * good enough for the one-shot lookup a pin_open() by serial needs. Returns
 * 0 and fills serial[17] on success, -1 if the GUID couldn't be read. */
static int probe_serial_once(const pinnacle_enum_entry_t *e, char *serial)
{
    uint32_t ghi = 0, glo = 0;
    pinnacle_lock_info_t li;
    if (pinnacle_lock_query(e->id, &li) == PINNACLE_OK && li.held && li.guid_known) {
        ghi = li.guid_hi; glo = li.guid_lo;
    } else if (e->state == PINNACLE_ENUM_READY) {
        pinnacle_lock_t *lk = NULL;
        if (pinnacle_lock_acquire(e->id, &lk) != PINNACLE_OK)
            return -1;
        pinnacle_device_t dev;
        memset(&dev, 0, sizeof(dev));
        int got = 0;
        if (pinnacle_open_by_id(&dev, e->id) == PINNACLE_OK) {
            got = pinnacle_read_guid(&dev, &ghi, &glo) == PINNACLE_OK;
            pinnacle_close(&dev);
        }
        pinnacle_lock_release(lk);
        if (!got)
            return -1;
    } else {
        return -1;
    }
    snprintf(serial, 17, "%08X%08X", (unsigned)ghi, (unsigned)glo);
    return 0;
}

static int looks_like_serial(const char *s)
{
    if (strlen(s) != 16)
        return 0;
    for (int i = 0; i < 16; i++)
        if (!isxdigit((unsigned char)s[i]))
            return 0;
    return 1;
}

static int resolve_device_id(const char *want, char *out, size_t out_size)
{
    if (want && strncmp(want, "replay:", 7) == 0) {
        /* pin_enumerate() lists the replay device as "replay:<file name>"
         * because pin_device_info_t.id is short; map that back to the full
         * path it was configured with (PIN_REPLAY, else the process-global
         * pin_set_replay_file() source). A full path is used as is. */
        const char *name = want + 7;
        char cfg[PIN_PATH_MAX] = { 0 };
        const char *full = getenv("PIN_REPLAY");
        if ((!full || !full[0]) && pin_session_get_replay_file(cfg, sizeof(cfg)) == 0)
            full = cfg;
        if (full && full[0]) {
            const char *base = full;
            for (const char *p = full; *p; p++)
                if (*p == '/' || *p == '\\')
                    base = p + 1;
            if (strcmp(base, name) == 0) {
                snprintf(out, out_size, "replay:%s", full);
                return 0;
            }
        }
        strncpy(out, want, out_size - 1);
        out[out_size - 1] = 0;
        return 0;
    }
    if (want && want[0] && strcmp(want, "first") != 0) {
        /* A path to an existing file: adopt it as the replay source (same
         * as calling pin_set_replay_file() first), per pin_api.h's
         * pin_open(). Checked before id/serial matching since a file path
         * would never collide with either ("usb:..." ids and 16-hex-char
         * serials are never valid file names in this form). */
        if (path_is_existing_file(want)) {
            pin_session_set_replay_file(want);
            snprintf(out, out_size, "replay:%s", want);
            return 0;
        }

        pinnacle_enum_entry_t entries[16];
        int n = pinnacle_enumerate(entries, 16);
        if (n > 16) n = 16;
        for (int i = 0; i < n; i++) {
            if (strcmp(entries[i].id, want) == 0) {
                strncpy(out, want, out_size - 1);
                out[out_size - 1] = 0;
                return 0;
            }
        }
        /* No id matched literally: try want as a serial. */
        if (looks_like_serial(want)) {
            for (int i = 0; i < n; i++) {
                char serial[17];
                if (probe_serial_once(&entries[i], serial) == 0 && strcasecmp(serial, want) == 0) {
                    strncpy(out, entries[i].id, out_size - 1);
                    out[out_size - 1] = 0;
                    return 0;
                }
            }
        }
        /* Neither: hand it through unresolved (e.g. an id for a device that
         * just unplugged) so the later hardware open fails with a clear,
         * specific error instead of this function guessing. */
        strncpy(out, want, out_size - 1);
        out[out_size - 1] = 0;
        return 0;
    }
    const char *env_replay = getenv("PIN_REPLAY");
    if (env_replay && env_replay[0]) {
        snprintf(out, out_size, "replay:%s", env_replay);
        return 0;
    }
    char global_replay[PIN_PATH_MAX];
    if (pin_session_get_replay_file(global_replay, sizeof(global_replay)) == 0) {
        snprintf(out, out_size, "replay:%s", global_replay);
        return 0;
    }
    pinnacle_enum_entry_t entries[16];
    int n = pinnacle_enumerate(entries, 16);
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) {
        if (entries[i].state == PINNACLE_ENUM_READY) {
            strncpy(out, entries[i].id, out_size - 1);
            out[out_size - 1] = 0;
            return 0;
        }
    }
    return -1;
}

/* ========================================================================
 * scene-split debounce FIFO
 * ==================================================================== */

static void scene_fifo_clear(pin_session_t *s)
{
    for (unsigned i = 0; i < s->scene_fifo_count; i++) {
        unsigned idx = (s->scene_fifo_head + i) % PIN_SCENE_FIFO_CAP;
        free(s->scene_fifo[idx].data);
        s->scene_fifo[idx].data = NULL;
    }
    s->scene_fifo_head = s->scene_fifo_count = 0;
}

static void writer_commit(void *user, const uint8_t *data, size_t len)
{
    pin_session_t *s = user;
    if (!s->writer)
        return;
    if (!pin_writer_push(s->writer, PIN_UNIT_RAW, s->unit_index++, data, len))
        s->write_dropped++;
}

static void open_sink_for_scene(pin_session_t *s);
static void capture_report_end(pin_session_t *s, pin_stop_reason_t why, const char *detail);

/* Saves the FFV1 rate of the running capture (average of the last 10 minutes)
 * into the core settings as core.ffv1_bytes_per_hour, when it has run long
 * enough (30 s) to mean something. */
static void rate_persist(pin_session_t *s)
{
    double bps = pin_rate_bytes_per_s(&s->rate_win, 30.0);
    if (bps <= 0) return;
    char path[PIN_PATH_MAX];
    if (pin_settings_default_path(path, sizeof(path)) != 0) return;
    if (pin_est_store_ffv1(path, bps * 3600.0) == 0)
        s->ffv1_learned_bph = bps * 3600.0;
}

/* The learned FFV1 rate, read from the settings once per session. */
static double learned_ffv1_bph(pin_session_t *s)
{
    if (!s->ffv1_learned_loaded) {
        s->ffv1_learned_loaded = 1;
        char path[PIN_PATH_MAX];
        if (pin_settings_default_path(path, sizeof(path)) == 0)
            s->ffv1_learned_bph = pin_est_load_ffv1(path);
    }
    return s->ffv1_learned_bph;
}

/* Closes the current sink, first adding what it wrote to the capture's byte
 * total (bytes_written in the status is per file, total_bytes_written sums the
 * closed files plus the open one). The writer must already be stopped. */
static pin_status_t close_sink_counted(pin_session_t *s)
{
    if (!s->sink) return PIN_OK;
    pin_sink_status_t sst;
    memset(&sst, 0, sizeof(sst));
    if (s->sink->get_status) {
        s->sink->get_status(s->sink, &sst);
        s->bytes_closed += sst.bytes_written;
    } else {
        s->bytes_closed += s->bytes_written;
    }
    return s->sink->close(s->sink);
}

/* Feeds the 10-minute rate window (measured bytes/s) and, once a minute, saves
 * an FFV1 capture's rate as the next estimate. Caller holds the lock, capture
 * running. */
static void rate_sample(pin_session_t *s)
{
    if (s->state != PIN_STATE_CAPTURING || s->capture_start_s <= 0)
        return;
    double now = pin_session_now();
    pin_rate_add(&s->rate_win, now, s->bytes_closed + s->bytes_written);
    if (s->stream_kind == PIN_KIND_ANALOG && s->active_format == PIN_FMT_ANALOG_FFV1_MKV &&
        now - s->rate_saved_s > 60.0) {
        s->rate_saved_s = now;
        rate_persist(s);
    }
}

/* The content split commits: close the current file, open the next. Called
 * by the split lookahead (pin_split.h) once the new segment is long and big
 * enough to deserve its own file. */
static void split_do(void *user)
{
    pin_session_t *s = user;
    if (s->writer) { pin_writer_stop(s->writer); s->writer = NULL; }
    if (s->sink) { close_sink_counted(s); s->sink = NULL; }
    s->scene_index++;
    open_sink_for_scene(s);
    pin_session_push_event(s, PIN_EVT_SCENE, (int32_t)s->scene_index, NULL);
}

static void split_ensure(pin_session_t *s)
{
    if (!s->split.commit)
        pin_split_init(&s->split, writer_commit, split_do, s);
}

/* Seconds one new frame_index adds to a pending segment: a DV frame, or an
 * HDV GOP (nominally 15 pictures at 60i, 12 at 50i: ~0.5 s). */
static double unit_seconds(const pin_session_t *s)
{
    if (s->stream_kind == PIN_KIND_HDV)
        return 0.5;
    return s->is_60hz ? 1001.0 / 30000.0 : 0.04;
}

/* Every unit that reaches a file goes through here (so a pending content
 * split can hold it). frame_index < 0: carries no time (PAT/PMT, raw). */
static void commit_unit_fi(pin_session_t *s, const uint8_t *data, size_t len, long frame_index)
{
    split_ensure(s);
    pin_split_push(&s->split, data, len, frame_index, unit_seconds(s));
}

static void commit_unit(pin_session_t *s, const uint8_t *data, size_t len)
{
    commit_unit_fi(s, data, len, -1);
}

/* Pushes one unit into the FIFO, evicting (and committing) the oldest once
 * it exceeds the detector's debounce window. */
static void scene_fifo_push(pin_session_t *s, const uint8_t *data, size_t len, long frame_index)
{
    unsigned window = s->opts_scene_split_window;
    if (s->scene_fifo_count == PIN_SCENE_FIFO_CAP ||
        (window && s->scene_fifo_count > window)) {
        pin_scene_fifo_item_t *oldest = &s->scene_fifo[s->scene_fifo_head];
        commit_unit_fi(s, oldest->data, oldest->len, oldest->frame_index);
        free(oldest->data);
        oldest->data = NULL;
        s->scene_fifo_head = (s->scene_fifo_head + 1) % PIN_SCENE_FIFO_CAP;
        s->scene_fifo_count--;
    }
    unsigned tail = (s->scene_fifo_head + s->scene_fifo_count) % PIN_SCENE_FIFO_CAP;
    pin_scene_fifo_item_t *it = &s->scene_fifo[tail];
    it->data = malloc(len);
    if (it->data) {
        memcpy(it->data, data, len);
        it->len = len;
        it->frame_index = frame_index;
        s->scene_fifo_count++;
    }
}

/* A scene cut was confirmed at cut_frame_index. Units before it belong to
 * the old scene; the cut itself only becomes a new file once the new segment
 * is real (pin_split.h): until then its units are held, and a second cut or
 * the end of the capture merges them back into the current file. */
static void scene_cut(pin_session_t *s, long cut_frame_index)
{
    split_ensure(s);
    /* a still-pending earlier split never made it: its units stay in the old file */
    pin_split_cancel(&s->split);
    unsigned n = s->scene_fifo_count;
    unsigned split_at = n;
    for (unsigned i = 0; i < n; i++) {
        unsigned idx = (s->scene_fifo_head + i) % PIN_SCENE_FIFO_CAP;
        if (s->scene_fifo[idx].frame_index >= cut_frame_index) { split_at = i; break; }
    }
    for (unsigned i = 0; i < split_at; i++) {
        unsigned idx = (s->scene_fifo_head + i) % PIN_SCENE_FIFO_CAP;
        commit_unit_fi(s, s->scene_fifo[idx].data, s->scene_fifo[idx].len,
                       s->scene_fifo[idx].frame_index);
    }
    pin_split_begin(&s->split);
    for (unsigned i = split_at; i < n; i++) {
        unsigned idx = (s->scene_fifo_head + i) % PIN_SCENE_FIFO_CAP;
        commit_unit_fi(s, s->scene_fifo[idx].data, s->scene_fifo[idx].len,
                       s->scene_fifo[idx].frame_index);
        free(s->scene_fifo[idx].data);
        s->scene_fifo[idx].data = NULL;
    }
    /* the pre-cut items were freed by neither loop above */
    for (unsigned i = 0; i < split_at; i++) {
        unsigned idx = (s->scene_fifo_head + i) % PIN_SCENE_FIFO_CAP;
        free(s->scene_fifo[idx].data);
        s->scene_fifo[idx].data = NULL;
    }
    s->scene_fifo_head = s->scene_fifo_count = 0;
}

/* ========================================================================
 * writer consume callback -> sink
 * ==================================================================== */

static int writer_consume(pin_unit_kind_t kind, uint64_t index, const uint8_t *data, size_t len,
                           void *user)
{
    pin_session_t *s = user;
    (void)index;
    if (!s->sink)
        return 0;
    pin_status_t st;
    if (kind == PIN_UNIT_VIDEO)
        st = s->sink->write_video(s->sink, data, len);
    else if (kind == PIN_UNIT_AUDIO)
        st = s->sink->write_audio(s->sink, (const int16_t *)data, len / 4);
    else
        st = s->sink->write_unit(s->sink, data, len);
    return st == PIN_OK ? 0 : -1;
}

/* ========================================================================
 * output naming / sink open
 * ==================================================================== */

/* ---- format table (pin_api.h's pin_formats()/pin_format_info()) --------- */

static const pin_format_info_t g_formats[PIN_FMT_COUNT] = {
    [PIN_FMT_ANALOG_AVI] = { .format = PIN_FMT_ANALOG_AVI, .kind = PIN_KIND_ANALOG,
        .label = "Uncompressed AVI (YUY2)", .extension = "avi",
        .supports_title = 1, .supports_scene_split = 0, .supports_multi_pass = 0, .is_default = 1 },
    [PIN_FMT_ANALOG_FFV1_MKV] = { .format = PIN_FMT_ANALOG_FFV1_MKV, .kind = PIN_KIND_ANALOG,
        .label = "FFV1 lossless (MKV)", .extension = "mkv",
        .supports_title = 1, .supports_scene_split = 0, .supports_multi_pass = 0, .is_default = 0 },
    [PIN_FMT_DV_RAW] = { .format = PIN_FMT_DV_RAW, .kind = PIN_KIND_DV,
        .label = "Raw DV stream", .extension = "dv",
        .supports_title = 0, .supports_scene_split = 1, .supports_multi_pass = 1, .is_default = 1 },
    [PIN_FMT_DV_AVI] = { .format = PIN_FMT_DV_AVI, .kind = PIN_KIND_DV,
        .label = "DV in AVI (type 2)", .extension = "avi",
        .supports_title = 1, .supports_scene_split = 1, .supports_multi_pass = 1, .is_default = 0 },
    [PIN_FMT_DV_MOV] = { .format = PIN_FMT_DV_MOV, .kind = PIN_KIND_DV,
        .label = "DV in QuickTime", .extension = "mov",
        .supports_title = 1, .supports_scene_split = 1, .supports_multi_pass = 1, .is_default = 0 },
    [PIN_FMT_HDV_TS] = { .format = PIN_FMT_HDV_TS, .kind = PIN_KIND_HDV,
        .label = "Raw transport stream", .extension = "ts",
        .supports_title = 0, .supports_scene_split = 1, .supports_multi_pass = 1, .is_default = 1 },
    [PIN_FMT_HDV_MOV] = { .format = PIN_FMT_HDV_MOV, .kind = PIN_KIND_HDV,
        .label = "HDV in QuickTime", .extension = "mov",
        .supports_title = 1, .supports_scene_split = 1, .supports_multi_pass = 1, .is_default = 0 },
    [PIN_FMT_HDV_MKV] = { .format = PIN_FMT_HDV_MKV, .kind = PIN_KIND_HDV,
        .label = "HDV in Matroska", .extension = "mkv",
        .supports_title = 1, .supports_scene_split = 1, .supports_multi_pass = 1, .is_default = 0 },
};

int pin_session_formats(pin_kind_t kind, pin_format_info_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < PIN_FMT_COUNT; i++) {
        if (g_formats[i].kind != kind)
            continue;
        if (out && n < max) {
            out[n] = g_formats[i];
            out[n].size = sizeof(out[n]);
        }
        n++;
    }
    return n;
}

pin_status_t pin_session_format_info(pin_format_t f, pin_format_info_t *out)
{
    if ((unsigned)f >= PIN_FMT_COUNT || !out)
        return PIN_ERR_ARG;
    *out = g_formats[f];
    out->size = sizeof(*out);
    return PIN_OK;
}

static void format_ext(pin_format_t f, char *out, size_t out_size)
{
    pin_format_info_t fi;
    if (pin_session_format_info(f, &fi) == PIN_OK)
        strncpy(out, fi.extension, out_size - 1);
    else
        strncpy(out, "dat", out_size - 1);
    out[out_size - 1] = 0;
}

static unsigned first_scene_number(const pin_session_t *s)
{
    return s->capture_opts.first_number ? s->capture_opts.first_number : 1;
}

/* Zeroes the capture-total counters (a capture or input switch begins). */
static void reset_frame_counters(pin_session_t *s)
{
    s->frames = s->frames_dropped = s->frames_damaged = s->lost_blocks = s->ts_errors = 0;
    s->write_dropped = 0;
    s->frames_error = 0;
    s->clip_frames = s->clip_frames_error = s->clip_frames_dropped = 0;
    s->err_video_blocks = s->err_audio_blocks = s->err_missing_blocks = 0;
    s->hdv_err_reset = 1;
}

/* One video unit (DV frame / HDV picture / analog frame) arrived. Caller holds the lock. */
static void count_frame(pin_session_t *s, int err, int dropped)
{
    s->frames++;
    s->clip_frames++;
    if (err) { s->frames_error++; s->clip_frames_error++; }
    if (dropped) { s->frames_dropped++; s->clip_frames_dropped++; }
}

static void open_sink_for_scene(pin_session_t *s)
{
    pin_naming_opts_t nopts = {
        .scene_split = s->capture_opts.scene_split,
        .always_number = s->capture_opts.first_number != 0,
        .scene_index = s->scene_index,
        .pass = s->pass_index,
    };
    char path[PIN_PATH_MAX];
    if (pin_naming_build(s->naming_base, &nopts, s->naming_ext, path, sizeof(path)) != 0) {
        set_error(s, PIN_ERR_ARG, "output path too long");
        return;
    }

    s->sink = pin_sink_create(s->active_format);
    if (!s->sink) {
        set_error(s, PIN_ERR_ARG, "unsupported output format");
        return;
    }

    pin_sink_params_t params;
    memset(&params, 0, sizeof(params));
    params.kind = s->stream_kind;
    strncpy(params.title, s->capture_opts.title, sizeof(params.title) - 1);
    /* AUTO resolves to the stream's own detected aspect (DV: VAUX 16:9
     * flag via dv_on_unit(); analog: fixed 4:3; HDV: fixed 16:9) -- see
     * the coordinator's "Default AUTO = stream's own". */
    pin_aspect_t effective_aspect = s->capture_opts.aspect;
    if (effective_aspect == PIN_ASPECT_AUTO)
        effective_aspect = s->dar_num == 16 ? PIN_ASPECT_16_9 : PIN_ASPECT_4_3;
    params.aspect = effective_aspect;

    int is_pal = s->detected_std != PIN_STD_NTSC && s->detected_std != PIN_STD_NTSC_443 &&
                 s->detected_std != PIN_STD_NTSC_J;
    /* Analog: "PAL" here means 625/50, which PAL-M and PAL-60 are not. */
    if (s->stream_kind == PIN_KIND_ANALOG)
        is_pal = !pinnacle_std_is_60hz(s->analog.cfg.standard);
    if (s->stream_kind == PIN_KIND_ANALOG) {
        params.width = (int)s->analog.width;
        params.height = (int)s->analog.height;
        params.fps_num = is_pal ? 25 : 30000;
        params.fps_den = is_pal ? 1 : 1001;
        params.interlaced = 1;
        params.top_field_first = 1;
        params.audio_rate = 48000;
        params.audio_channels = 2;
    } else if (s->stream_kind == PIN_KIND_DV) {
        params.width = 720;
        params.height = is_pal ? 576 : 480;
        params.fps_num = is_pal ? 25 : 30000;
        params.fps_den = is_pal ? 1 : 1001;
        params.interlaced = 1;
        params.top_field_first = 0; /* DV: BFF, per the plan */
        params.audio_rate = 48000;
        params.audio_channels = 2;
    } else { /* HDV */
        params.width = 1440;
        params.height = 1080;
        params.fps_num = is_pal ? 25 : 30000;
        params.fps_den = is_pal ? 1 : 1001;
        params.interlaced = 1;
        params.top_field_first = 1;
        params.audio_rate = 48000;
        params.audio_channels = 2;
    }
    pin_sink_sar_for(s->stream_kind, is_pal, params.aspect, params.width,
                      &params.sample_aspect_num, &params.sample_aspect_den);
    params.colour_matrix = s->stream_kind == PIN_KIND_HDV ? PIN_MATRIX_BT709 : PIN_MATRIX_BT601;

    pin_status_t st = s->sink->open(s->sink, path, &params);
    if (st != PIN_OK) {
        /* a scene split or the next pass: the capture ends here */
        if (s->capture_began)
            capture_report_end(s, PIN_STOP_WRITE_ERROR, "the next output file could not be created");
        set_error(s, st, "failed to open output");
        s->sink->close(s->sink); /* never opened: nothing to count */
        s->sink = NULL;
        return;
    }
    s->writer = pin_writer_start(0, writer_consume, s);
    strncpy(s->current_file, path, sizeof(s->current_file) - 1);
    pin_split_new_file(&s->split);
    /* the clip counters restart with every file (frames still held back by a
     * pending split were counted into the previous clip: at most ~1 s) */
    s->clip_frames = s->clip_frames_error = s->clip_frames_dropped = 0;
    pin_session_push_event(s, PIN_EVT_FILE_OPENED, 0, path);
}

/* ========================================================================
 * audio meters + monitor ring: shared by analog line-in (analog_audio_cb,
 * below), DV (dv_on_unit(), via dv_audio_extract()) and HDV (dv_on_unit()
 * via pin_hdv_audio_t's own decode thread and pin_session_feed_monitor_audio()
 * below) -- interleaved s16 stereo at 48 kHz only; callers resample first
 * if their source wasn't already 48 kHz (pin_audio_resample.h).
 * ==================================================================== */

/* Caller must already hold s->mtx (pin_session_lock()) -- true of every
 * call site in this file (analog_audio_cb() and dv_on_unit() both lock
 * before doing anything else). */
static double meter_clock(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Replay pacing: sleep until *next, then advance it by one frame period.
 * Deadline based, so decode/write time doesn't slow playback below real
 * time (live audio monitoring needs the true sample rate); after a pause or
 * a long stall it resynchronises instead of bursting to catch up. */
static void pace_frame(double *next, double period)
{
    double now = meter_clock();
    if (*next == 0 || now - *next > 0.5)
        *next = now;
    *next += period;
    double wait = *next - now;
    if (wait > 0)
        sleep_ms((int)(wait * 1000.0 + 0.5));
}

static void feed_audio_locked(pin_session_t *s, const int16_t *pcm, unsigned frames)
{
    if (!frames)
        return;
    s->audio_meter_t = meter_clock();
    double sum[2] = { 0, 0 };
    int peak[2] = { 0, 0 };
    for (unsigned i = 0; i < frames; i++) {
        for (int ch = 0; ch < 2; ch++) {
            int v = pcm[i * 2 + ch];
            int av = v < 0 ? -v : v; /* int: -32768 has no int16_t magnitude */
            if (av > peak[ch]) peak[ch] = av;
            sum[ch] += (double)v * v;
        }
    }
    for (int ch = 0; ch < 2; ch++) {
        double rms = sqrt(sum[ch] / frames) / 32768.0;
        double pk = peak[ch] / 32768.0;
        if (!s->audio_peak_acc_n || pk > s->audio_peak_acc[ch])
            s->audio_peak_acc[ch] = (float)pk;
        s->audio_peak_db[ch] = pk > 0 ? (float)(20.0 * log10(pk)) : -144.0f;
        s->audio_rms_db[ch] = rms > 0 ? (float)(20.0 * log10(rms)) : -144.0f;
    }
    s->audio_peak_acc_n++;
    if (s->mon_enabled) {
        pthread_mutex_lock(&s->mon_mtx);
        for (unsigned i = 0; i < frames && s->mon_buf; i++) {
            size_t pos = s->mon_head % s->mon_cap_frames;
            s->mon_buf[pos * 2] = pcm[i * 2];
            s->mon_buf[pos * 2 + 1] = pcm[i * 2 + 1];
            s->mon_head++;
            if (s->mon_fill < s->mon_cap_frames) s->mon_fill++;
        }
        pthread_mutex_unlock(&s->mon_mtx);
    }
}

/* For callers that don't already hold s->mtx -- today only pin_hdv_audio_t's
 * own decode thread, via the pin_hdv_audio_feed_cb passed to
 * pin_hdv_audio_create() in pin_session_open(). */
static void pin_session_feed_monitor_audio(void *user, const int16_t *pcm, unsigned frames)
{
    pin_session_t *s = user;
    pin_session_lock(s);
    feed_audio_locked(s, pcm, frames);
    pin_session_unlock(s);
}

/* ========================================================================
 * DV/HDV: EP 0x88 -> dv_reassembler; on_unit -> preview / subcode / scene
 * ==================================================================== */

typedef struct {
    pin_session_t *s;
    double last_tick_transport_poll;
    /* HDV PID map, accumulated across pictures: PAT/PMT ride in only some
     * pictures, so a per-picture scan would leave most of them without a
     * video PID (no preview, no audio, no GOP timecode). */
    hdv_pid_map_t hdv_map;
    int hdv_map_init;
    /* 1394 bus watch (dv_tick): async read of OHCI SelfIDCount, whose
     * generation field (23:16) changes with every bus reset and whose size
     * field (10:2) follows the node count */
    int bus_pending;
    double bus_poll_s;
    uint32_t bus_sig;
    int bus_sig_valid;
    const dv_reassembler_t *reasm; /* live reassembler: latest PAT/PMT for the write gate */
    hdv_err_state_t hdv_err;       /* continuity / reference tracking for the error stats */
} dv_ctx_t;

/* HDV capture gate: a session's sink opens at an arbitrary picture, but a
 * transport stream must start on a GOP (sequence header) or decoders reject
 * the first pictures. Returns 1 if the unit may be written; on the first
 * pass it also emits the latest PAT/PMT via emit(). */
static int hdv_write_gate(pin_session_t *s, const dv_ctx_t *ctx, const uint8_t *data, size_t len,
                          void (*emit)(pin_session_t *, const uint8_t *, size_t))
{
    if (!s->hdv_await_gop || !ctx->reasm) /* replays are already cut on GOPs */
        return 1;
    static const uint8_t seq_hdr[4] = { 0x00, 0x00, 0x01, 0xB3 };
    int found = 0;
    for (size_t i = 0; i + 4 <= len; i++)
        if (!memcmp(data + i, seq_hdr, 4)) { found = 1; break; }
    if (!found)
        return 0;
    s->hdv_await_gop = 0;
    if (ctx->reasm) {
        if (ctx->reasm->ts_pat_valid) emit(s, ctx->reasm->ts_pat, 188);
        if (ctx->reasm->ts_pmt_valid) emit(s, ctx->reasm->ts_pmt, 188);
    }
    return 1;
}

static void emit_commit(pin_session_t *s, const uint8_t *d, size_t n) { commit_unit(s, d, n); }
static void emit_fifo(pin_session_t *s, const uint8_t *d, size_t n)
{
    scene_fifo_push(s, d, n, s->scene_frame_counter);
}

/* Forward decls: defined further down (near handle_inline_commands()), but
 * dv_on_unit()/analog_video_cb() above them need to call in when the
 * stream's kind newly becomes known. */
static void maybe_begin_capture(pin_session_t *s, uint16_t camera_node);

static int dv_write_cb(const uint8_t *data, size_t len, void *user)
{
    dv_ctx_t *ctx = user;
    pin_session_t *s = ctx->s;
    if (!s->sink) /* not capturing: nothing to write, still fully reassembled for preview */
        return 0;
    /* Scene-split buffering (the debounce FIFO) is implemented for both DV
     * (per-frame subcode) and HDV (per-GOP timecode); see dv_on_unit()'s two
     * branches. s->stream_kind is only known once the first unit has gone
     * through dv_on_unit(), which runs right after this on the very first
     * unit, so the one-unit-late kind on that first call only ever affects
     * whether that single first unit is buffered or committed immediately --
     * never a correctness issue, just which file it lands in. */
    int scene_split_active = s->capture_opts.scene_split;
    if (!scene_split_active) {
        if (s->stream_kind == PIN_KIND_HDV && !hdv_write_gate(s, ctx, data, len, emit_commit))
            return 0;
        commit_unit(s, data, len);
    }
    return 0;
}

static void dv_on_unit(dv_format_t fmt, const uint8_t *data, size_t len, void *user)
{
    dv_ctx_t *ctx = user;
    pin_session_t *s = ctx->s;

    pin_session_lock(s);
    if (s->stream_kind == PIN_KIND_ANALOG || s->stream_kind == 0) {
        /* first unit: lock in DV vs HDV and tell the caller (PIN_EVT_INPUT_FORMAT) */
    }
    pin_kind_t kind = fmt == DV_FORMAT_HDV ? PIN_KIND_HDV : PIN_KIND_DV;
    int format_changed = (s->stream_kind != kind);
    int kind_newly_known = !s->stream_kind_known;
    s->stream_kind = kind;
    s->stream_kind_known = 1;
    s->signal = 1;

    if (kind == PIN_KIND_DV) {
        dv_frame_info_t info;
        if (dv_subcode_parse_frame(data, len, &info) == 0) {
            int is_pal = info.dsf_valid ? info.is_pal : (info.sequence_count == DV_SEQ_COUNT_PAL);
            s->detected_std = is_pal ? PIN_STD_PAL : PIN_STD_NTSC;
            s->is_60hz = !is_pal;
            s->width = 720;
            s->height = is_pal ? 576 : 480;
            if (info.aspect.valid)
                s->dar_num = info.aspect.is_16_9 ? 16 : 4, s->dar_den = info.aspect.is_16_9 ? 9 : 3;
            else
                s->dar_num = 4, s->dar_den = 3;
            if (info.timecode.valid)
                snprintf(s->timecode, sizeof(s->timecode), "%02d:%02d:%02d%c%02d",
                         info.timecode.hour, info.timecode.minute, info.timecode.second,
                         info.timecode.drop_frame ? ';' : ':', info.timecode.frame);
            if (info.rec_date.valid && info.rec_time.valid)
                snprintf(s->rec_datetime, sizeof(s->rec_datetime), "%04d-%02d-%02d %02d:%02d:%02d",
                         info.rec_date.year, info.rec_date.month, info.rec_date.day,
                         info.rec_time.hour, info.rec_time.minute, info.rec_time.second);
            dv_frame_errors_t derr;
            if (dv_error_analyze(data, len, &derr) != 0)
                memset(&derr, 0, sizeof(derr));
            count_frame(s, derr.frame_error, derr.frame_dropped);
            s->err_video_blocks += derr.video_err + derr.video_concealed;
            s->err_audio_blocks += derr.audio_err + derr.audio_mute;
            s->err_missing_blocks += derr.missing_blocks;

            pin_previewer_push_dv(s->preview, data, len, is_pal);

            /* Audio (see dv_audio.h for what this is/isn't bit-exact to):
             * cheap enough to run inline, right here on whatever thread
             * dv_on_unit() is called from (the USB read loop, or a replay
             * thread) -- no queue/thread of its own needed, unlike HDV's
             * mp2 decode below. Only pair 1 (CH1/CH2) feeds the monitor. */
            dv_audio_pcm_t apcm;
            if (dv_audio_extract(data, len, &apcm) == 0 && apcm.valid && apcm.pair1_samples > 0) {
                if (apcm.sample_rate == 48000 || apcm.sample_rate <= 0) {
                    feed_audio_locked(s, apcm.pair1, (unsigned)apcm.pair1_samples);
                } else {
                    int16_t rs_out[DV_AUDIO_MAX_PAIR_SAMPLES * 2];
                    size_t n = pin_resample_s16_stereo(&s->dv_audio_rs, apcm.pair1,
                                                        (size_t)apcm.pair1_samples,
                                                        apcm.sample_rate, rs_out,
                                                        sizeof(rs_out) / sizeof(rs_out[0]) / 2,
                                                        48000);
                    if (n)
                        feed_audio_locked(s, rs_out, (unsigned)n);
                }
            }

            if (s->sink && s->capture_opts.scene_split) {
                if (!s->scene_det_ready) {
                    pin_scene_config_t cfg;
                    pin_scene_config_defaults(&cfg, is_pal ? 25.0 : 29.97);
                    pin_scene_init(&s->scene_det, &cfg);
                    s->opts_scene_split_window = pin_scene_debounce_window(&s->scene_det);
                    s->scene_det_ready = 1;
                    s->scene_frame_counter = 0;
                }
                pin_scene_record_t rec;
                memset(&rec, 0, sizeof(rec));
                rec.frame_index = s->scene_frame_counter;
                if (info.timecode.valid) {
                    rec.tc_valid = 1;
                    rec.tc_hour = info.timecode.hour; rec.tc_minute = info.timecode.minute;
                    rec.tc_second = info.timecode.second; rec.tc_frame = info.timecode.frame;
                    rec.tc_drop_frame = info.timecode.drop_frame;
                }
                if (info.rec_date.valid) {
                    rec.date_valid = 1;
                    rec.date_day = info.rec_date.day; rec.date_month = info.rec_date.month;
                    rec.date_year = info.rec_date.year;
                }
                if (info.rec_time.valid) {
                    rec.time_valid = 1;
                    rec.time_hour = info.rec_time.hour; rec.time_minute = info.rec_time.minute;
                    rec.time_second = info.rec_time.second;
                }
                rec.rec_start_valid = info.rec_start_valid;
                rec.rec_start = info.rec_start;

                scene_fifo_push(s, data, len, s->scene_frame_counter);
                long cut_idx = 0;
                if (pin_scene_feed(&s->scene_det, &rec, &cut_idx))
                    scene_cut(s, cut_idx);
                s->scene_frame_counter++;
            } else if (s->sink) {
                /* non-scene-split path already committed in dv_write_cb() */
            }
        } else {
            s->frames_damaged++;
            count_frame(s, 1, 1);
        }
    } else { /* HDV */
        if (!ctx->hdv_map_init) {
            memset(&ctx->hdv_map, 0, sizeof(ctx->hdv_map));
            ctx->hdv_map.pmt_pid = -1; ctx->hdv_map.video_pid = -1;
            ctx->hdv_map.aux_pid = -1; ctx->hdv_map.audio_pid = -1;
            ctx->hdv_map_init = 1;
        }
        hdv_pid_map_t *map_p = &ctx->hdv_map;
        hdv_scan_pat_pmt(data, len / 188, map_p);
        hdv_pid_map_t map = *map_p;
        int vpid = map.video_pid > 0 ? map.video_pid : -1;
        if (vpid > 0)
            pin_previewer_push_hdv(s->preview, data, len / 188, vpid);
        /* Off-thread: pin_hdv_audio_t decodes the mp2 audio PID and calls
         * pin_session_feed_monitor_audio() back (which takes s->mtx itself,
         * so this push -- a bounded-queue copy, never blocking -- is safe
         * to make while already holding it here). */
        if (map.audio_pid > 0)
            pin_hdv_audio_push(s->hdv_audio, data, len / 188, map.audio_pid);
        hdv_gop_time_t gop;
        int disc = 0;
        memset(&gop, 0, sizeof(gop));
        if (vpid > 0 && hdv_parse_picture(data, len / 188, vpid, &gop, &disc) == 0 && gop.found) {
            snprintf(s->timecode, sizeof(s->timecode), "%02d:%02d:%02d%c%02d",
                     gop.hours, gop.minutes, gop.seconds, gop.drop_frame ? ';' : ':', gop.pictures);
        }
        s->ts_errors += hdv_ts_discontinuity_count(data, len / 188);
        hdv_unit_errors_t herr;
        {
            /* a stream break (rewind, pause, no-signal gap) is not a loss: restart the tracking */
            if (s->hdv_err_reset || pin_session_now() - s->last_data_s > 1.0) {
                hdv_error_init(&ctx->hdv_err);
                s->hdv_err_reset = 0;
            }
            hdv_error_analyze(&ctx->hdv_err, data, len / 188, vpid, &herr);
            count_frame(s, herr.frame_error, 0);
            s->err_video_blocks += herr.video_damaged || herr.tainted;
            s->err_missing_blocks += herr.cc_errors + herr.tei + herr.sync_lost;
        }
        if (herr.seq_found && herr.seq_width > 0 && herr.seq_height > 0) {
            s->hdv_seq_w = herr.seq_width;
            s->hdv_seq_h = herr.seq_height;
            pin_vidfmt_rate_from_code(herr.seq_frame_rate_code, &s->hdv_fps_num, &s->hdv_fps_den);
        }
        s->width = s->hdv_seq_w ? s->hdv_seq_w : 1440;
        s->height = s->hdv_seq_h ? s->hdv_seq_h : 1080;
        s->dar_num = 16; s->dar_den = 9;
        s->detected_std = PIN_STD_AUTO; /* HDV: not from the SAA7113, leave detected_std unset */

        /* Scene split for HDV: fed one GOP (picture unit) at a time, using
         * the GOP header timecode as the only signal (HDV has no per-frame
         * recording date/time here -- see hdv_aux.h's file header comment).
         * Same debounce-FIFO scheme as the DV branch above. */
        if (s->sink && s->capture_opts.scene_split) {
            if (!s->scene_det_ready) {
                pin_scene_config_t cfg;
                pin_scene_config_defaults(&cfg, 29.97); /* HDV: 1080i/60 nominal; no field to say 25p/i here */
                pin_scene_init(&s->scene_det, &cfg);
                s->opts_scene_split_window = pin_scene_debounce_window(&s->scene_det);
                s->scene_det_ready = 1;
                s->scene_frame_counter = 0;
            }
            pin_scene_record_t rec;
            memset(&rec, 0, sizeof(rec));
            rec.frame_index = s->scene_frame_counter;
            if (gop.found) {
                rec.tc_valid = 1;
                rec.tc_hour = gop.hours; rec.tc_minute = gop.minutes;
                rec.tc_second = gop.seconds; rec.tc_frame = gop.pictures;
                rec.tc_drop_frame = gop.drop_frame;
            }
            if (hdv_write_gate(s, ctx, data, len, emit_fifo)) {
                scene_fifo_push(s, data, len, s->scene_frame_counter);
                long cut_idx = 0;
                if (pin_scene_feed(&s->scene_det, &rec, &cut_idx))
                    scene_cut(s, cut_idx);
                s->scene_frame_counter++;
            }
        }
        /* else: already committed directly in dv_write_cb() */
    }
    s->last_data_s = pin_session_now();
    s->idle_s = 0;
    if (format_changed)
        pin_session_push_event(s, PIN_EVT_INPUT_FORMAT, 0, NULL);
    if (kind_newly_known)
        maybe_begin_capture(s, s->dev.camera_node);
    pin_session_unlock(s);
}

static void dv_flush_pending(pin_session_t *s)
{
    for (unsigned i = 0; i < s->scene_fifo_count; i++) {
        unsigned idx = (s->scene_fifo_head + i) % PIN_SCENE_FIFO_CAP;
        commit_unit_fi(s, s->scene_fifo[idx].data, s->scene_fifo[idx].len,
                       s->scene_fifo[idx].frame_index);
    }
    scene_fifo_clear(s);
    /* a split that never became a real segment: its units go to the current file */
    pin_split_flush(&s->split);
}

/* Sends one deck command through the async state machine. Always use this
 * instead of calling pin_deck_async_start() directly: the state machine only
 * advances while s->deck_busy is set, and the 1 Hz status poll reuses
 * s->deck_async, so a command started without deck_busy was silently
 * overwritten by the next poll and never sent. If something is in flight, a
 * status query is dropped in favour of the command; another command is
 * queued behind it (completed in dv_tick()). Caller holds the session lock. */
static void deck_send(pin_session_t *s, uint16_t node, pin_deck_cmd_t cmd)
{
    if (!node)
        return;
    if (s->deck_busy) {
        if (s->deck_async.cmd_len && s->deck_async.cmd[0] == 0x01) {
            p1394_avc_cancel(&s->link); /* a status query: not worth waiting for */
        } else {
            s->deck_q_cmd = cmd;
            s->deck_q_valid = 1;
            return;
        }
    }
    s->deck_q_valid = 0;
    pin_deck_async_start(&s->deck_async, &s->link, node, cmd, pin_session_now());
    s->deck_busy = 1;
}

/* ========================================================================
 * ending a capture: why, and the checks that end one on their own
 * ==================================================================== */

static int fat32_and_free(const char *path, uint64_t *free_bytes, int *fat32);

/* The directory part of a file path ("." if it has none). */
static void dir_of(const char *path, char *dir, size_t cap)
{
    snprintf(dir, cap, "%s", path);
    char *slash = strrchr(dir, '/');
    char *slash2 = strrchr(dir, '\\');
    if (slash2 && (!slash || slash2 > slash)) slash = slash2;
    if (slash) *slash = 0; else snprintf(dir, cap, ".");
}

/* Length of what this capture recorded (all passes), from its frame count. */
static double captured_seconds(const pin_session_t *s)
{
    if (!s->capture_began)
        return 0;
    double fps = s->is_60hz ? 30000.0 / 1001.0 : 25.0;
    if (s->stream_kind == PIN_KIND_HDV && s->hdv_fps_num > 0 && s->hdv_fps_den > 0)
        fps = (double)s->hdv_fps_num / s->hdv_fps_den;
    return (double)s->frames / fps;
}

/* Records why the capture ended and tells the GUI (PIN_EVT_CAPTURE_ENDED).
 * Caller holds the lock. */
static void capture_report_end(pin_session_t *s, pin_stop_reason_t why, const char *detail)
{
    s->stop_reason = why;
    s->stop_captured_s = captured_seconds(s);
    pin_stop_message(why, s->stop_captured_s, detail, s->stop_text, sizeof(s->stop_text));
    pin_logf(why == PIN_STOP_USER ? PIN_LOG_INFO : PIN_LOG_WARN, "session: %s\n", s->stop_text);
    pin_session_push_event(s, PIN_EVT_CAPTURE_ENDED, (int32_t)why, s->stop_text);
    s->capture_began = 0;
    s->capture_end_seq++;
}

/* A capture is running, rewinding between passes, or waiting to start. */
static int capture_active(const pin_session_t *s)
{
    return s->sink || s->capture_want_start || s->rewind_before_capture || s->pass_rewinding ||
           s->state == PIN_STATE_CAPTURING || s->state == PIN_STATE_REWINDING;
}

int pin_session_capture_busy(pin_session_t *s)
{
    pin_session_lock(s);
    int busy = capture_active(s) || s->state == PIN_STATE_STOPPING ||
               (s->cmd.pending && (s->cmd.kind == PIN_CMD_CAPTURE_START || s->cmd.kind == PIN_CMD_CAPTURE_STOP));
    pin_session_unlock(s);
    return busy;
}

/* Finishes the open file (pending units first) and reports it closed. */
static void finish_file(pin_session_t *s)
{
    if (!s->sink)
        return;
    dv_flush_pending(s);
    if (s->writer) { pin_writer_stop(s->writer); s->writer = NULL; }
    pin_status_t st = close_sink_counted(s);
    s->sink = NULL;
    pin_session_push_event(s, PIN_EVT_FILE_CLOSED, (int32_t)st, s->current_file);
}

/* Ends the running capture (or the wait for one to start): finishes the
 * file, sends deck Stop to deck_node if non-zero and reports why (only if a
 * capture was active). The state is the caller's (READY, or ERROR). Caller
 * holds the lock. */
static void capture_end(pin_session_t *s, pin_stop_reason_t why, const char *detail, uint16_t deck_node)
{
    int active = capture_active(s);
    s->capture_want_start = 0;
    s->rewind_before_capture = 0;
    s->pass_rewinding = 0;
    finish_file(s);
    if (deck_node)
        deck_send(s, deck_node, PIN_DECK_CMD_STOP);
    if (active)
        capture_report_end(s, why, detail);
}

/* The deck a capture that ends on its own stops: the camera, if the capture
 * drives the deck (start_deck). */
static uint16_t auto_stop_node(const pin_session_t *s, uint16_t node)
{
    return s->capture_opts.start_deck ? node : 0;
}

/* Extra space kept free on the output drive, from PINNACLE_DISK_RESERVE_MB
 * (for testing the disk-full stop on a big drive), else PIN_STOP_DISK_MARGIN. */
static uint64_t disk_margin(void)
{
    static int loaded;
    static uint64_t margin;
    if (!loaded) {
        loaded = 1;
        const char *e = getenv("PINNACLE_DISK_RESERVE_MB");
        if (e && *e)
            margin = (uint64_t)strtoull(e, NULL, 10) << 20;
    }
    return margin;
}

/* While a file is open, once a second: ends the capture while the output
 * drive still has room to finish the file (pin_stop_disk_reserve()), and at
 * once when writing failed. deck_node as for auto_stop_node(). Caller holds
 * the lock. Returns 1 if it ended the capture. */
static int capture_guard(pin_session_t *s, uint16_t deck_node)
{
    if (!s->sink || !s->writer || s->state != PIN_STATE_CAPTURING)
        return 0;
    pin_writer_stats_t wst;
    pin_writer_get_stats(s->writer, &wst);
    double now = pin_session_now();
    if (!wst.failed && now - s->disk_check_s < 1.0)
        return 0;
    s->disk_check_s = now;
    char dir[PIN_PATH_MAX];
    dir_of(s->current_file, dir, sizeof(dir));
    uint64_t free_bytes = 0;
    int fat32 = 0;
    int known = fat32_and_free(dir, &free_bytes, &fat32) == 0;
    /* HDV to MOV / MKV remuxes the whole temp .ts when the file is closed */
    int remux = s->active_format == PIN_FMT_HDV_MOV || s->active_format == PIN_FMT_HDV_MKV;
    uint64_t reserve = pin_stop_disk_reserve(disk_margin(), wst.backlog_bytes,
                                             remux ? s->bytes_written : 0);
    char detail[PIN_TEXT_MAX];
    pin_stop_reason_t why;
    if (known && free_bytes < reserve) {
        why = PIN_STOP_DISK_FULL;
        snprintf(detail, sizeof(detail), "the output drive is almost full (%.0f MB left)",
                 (double)free_bytes / 1048576.0);
    } else if (wst.failed) {
        why = PIN_STOP_WRITE_ERROR;
        detail[0] = 0;
    } else {
        return 0;
    }
    capture_end(s, why, detail, deck_node);
    set_state(s, PIN_STATE_READY);
    return 1;
}

/* "5 minutes" / "1 minute" */
static void minutes_text(int n, char *out, size_t cap)
{
    snprintf(out, cap, "%d minute%s", n, n == 1 ? "" : "s");
}

/* After the USB stream loop ended without being asked to: tells a vanished
 * device from another USB failure, ends the capture and puts the session in
 * ERROR. Returns 1 if the device is gone. Caller does NOT hold the lock. */
static int stream_failed(pin_session_t *s)
{
    int gone = !pinnacle_device_responds(&s->dev);
    const char *name = s->dev.model && s->dev.model->name ? s->dev.model->name : "capture device";
    char detail[PIN_TEXT_MAX];
    if (gone)
        snprintf(detail, sizeof(detail), "the %s was disconnected", name);
    else
        snprintf(detail, sizeof(detail), "the USB connection to the %s failed", name);
    pin_session_lock(s);
    capture_end(s, gone ? PIN_STOP_DEVICE_LOST : PIN_STOP_ERROR, detail, 0);
    set_error(s, gone ? PIN_ERR_NOT_FOUND : PIN_ERR_USB,
              gone ? "The capture device was disconnected. Plug it in again and reopen it."
                   : "The USB connection to the capture device failed.");
    pin_session_unlock(s);
    return gone;
}

/* Actually opens the sink and enters CAPTURING, once the stream's kind
 * (DV/HDV/analog) is known and any requested rewind-to-start has finished.
 * s->capture_opts must already hold the requested options. */
static void start_capture_now(pin_session_t *s, uint16_t camera_node)
{
    s->active_format = s->stream_kind == PIN_KIND_HDV ? s->capture_opts.format_hdv
                      : s->stream_kind == PIN_KIND_ANALOG ? s->capture_opts.format_analog
                      : s->capture_opts.format_dv;
    char ext[16];
    format_ext(s->active_format, ext, sizeof(ext));
    pin_naming_strip_extension(s->capture_opts.path, ext, s->naming_base, sizeof(s->naming_base));
    strncpy(s->naming_ext, ext, sizeof(s->naming_ext) - 1);
    s->scene_index = first_scene_number(s);
    s->pass_index = 1;
    s->unit_index = 0;
    reset_frame_counters(s);
    s->bytes_written = 0;
    s->bytes_closed = 0;
    pin_rate_reset(&s->rate_win);
    s->rate_saved_s = pin_session_now();
    s->capture_start_s = pin_session_now();
    /* the no-signal timer counts from here, not from the last data before a rewind */
    s->last_data_s = pin_session_now();
    s->idle_s = 0;
    s->scene_det_ready = 0;
    s->hdv_await_gop = s->stream_kind == PIN_KIND_HDV;
    pin_split_free(&s->split);
    memset(&s->split, 0, sizeof(s->split));
    open_sink_for_scene(s);
    if (s->state != PIN_STATE_ERROR) {
        s->capture_began = 1;
        s->disk_check_s = 0;
        set_state(s, PIN_STATE_CAPTURING);
        s->pass = 1;
        s->passes = s->capture_opts.passes < 1 ? 1 : s->capture_opts.passes;
        s->scene = 1;
        /* PLAY was already sent by maybe_begin_capture() (before the stream
         * kind was known); this only covers a caller that skipped that. */
        if (s->capture_opts.start_deck && camera_node && !s->start_play_sent)
            deck_send(s, camera_node, PIN_DECK_CMD_PLAY);
    }
}

/* Called right after a CAPTURE_START command, and again from dv_on_unit()/
 * analog_video_cb() whenever stream_kind_known newly becomes true: decides
 * whether the capture can actually begin now, needs to wait for the stream
 * kind to resolve, or (rewind_first) needs a rewind-to-BOT first. */
static void maybe_begin_capture(pin_session_t *s, uint16_t camera_node)
{
    if (!s->capture_want_start)
        return;
    int with_deck = s->capture_opts.start_deck && camera_node;
    if (with_deck && s->capture_opts.rewind_first && !s->rewind_done) {
        if (!s->rewind_before_capture) {
            /* Rewinding needs no stream, so it starts at once; the stream kind
             * is only needed to open the file, after PLAY. */
            s->rewind_before_capture = 1;
            s->rewind_wait_start_s = pin_session_now();
            set_state(s, PIN_STATE_REWINDING);
            deck_send(s, camera_node, PIN_DECK_CMD_REW);
        }
        return; /* dv_tick() calls back here once the tape is at the start */
    }
    /* A stopped deck sends no data, so the stream kind can never resolve
     * until PLAY has been sent. */
    if (with_deck && !s->start_play_sent) {
        s->start_play_sent = 1;
        deck_send(s, camera_node, PIN_DECK_CMD_PLAY);
    }
    if (!s->stream_kind_known)
        return; /* still waiting; dv_on_unit()/analog_video_cb() will call back */
    s->capture_want_start = 0;
    s->rewind_before_capture = 0;
    start_capture_now(s, camera_node);
}

/* True if a command is waiting that must interrupt the streaming loop
 * (input change / close) rather than be serviced inline (capture/deck). */
static int handle_inline_commands(pin_session_t *s, uint16_t camera_node)
{
    pthread_mutex_lock(&s->mtx);
    if (!s->cmd.pending) {
        pthread_mutex_unlock(&s->mtx);
        return 0;
    }
    pin_cmd_kind_t kind = s->cmd.kind;
    if (kind == PIN_CMD_SET_INPUT || kind == PIN_CMD_CLOSE) {
        pthread_mutex_unlock(&s->mtx);
        return 1; /* caller must break out of the streaming loop */
    }

    if (kind == PIN_CMD_CAPTURE_START) {
        s->capture_opts = s->cmd.capture;
        s->stop_reason = PIN_STOP_NONE;
        s->stop_captured_s = 0;
        s->stop_text[0] = 0;
        s->capture_began = 0;
        s->capture_want_start = 1;
        s->rewind_before_capture = 0;
        s->rewind_done = 0;
        s->start_play_sent = 0;
        s->pass_rewinding = 0;
        maybe_begin_capture(s, camera_node);
    } else if (kind == PIN_CMD_CAPTURE_STOP) {
        /* AS_STARTED: only a capture that drives the deck stops it. */
        int stop_deck = s->cmd.stop_deck == PIN_STOP_DECK_YES ||
                        (s->cmd.stop_deck == PIN_STOP_DECK_AS_STARTED && s->capture_opts.start_deck);
        capture_end(s, PIN_STOP_USER, NULL, stop_deck ? camera_node : 0);
        if (s->state != PIN_STATE_ERROR)
            set_state(s, PIN_STATE_READY);
    } else if (kind == PIN_CMD_DECK) {
        uint16_t node = camera_node;
        if (!node) {
            set_error(s, PIN_ERR_NO_CAMERA, "no camera on the 1394 bus");
        } else if (s->state == PIN_STATE_CAPTURING && s->cmd.deck_cmd != PIN_DECK_CMD_STOP) {
            /* "while CAPTURING, only STOP accepted" */
        } else if (s->cmd.deck_cmd == PIN_DECK_CMD_STOP && s->state == PIN_STATE_CAPTURING) {
            /* STOP while capturing: finish the capture first, then the deck */
            capture_end(s, PIN_STOP_USER, NULL, node);
            set_state(s, PIN_STATE_READY);
        } else {
            deck_send(s, node, s->cmd.deck_cmd);
        }
    }
    s->cmd.pending = 0;
    s->cmd.kind = PIN_CMD_NONE;
    pthread_cond_broadcast(&s->cmd_idle);
    pthread_mutex_unlock(&s->mtx);
    return 0;
}

static void dv_tick(void *user)
{
    dv_ctx_t *ctx = user;
    pin_session_t *s = ctx->s;
    uint16_t node = s->dev.camera_node;

    if (handle_inline_commands(s, node)) {
        s->loop_stop = 1;
        return;
    }

    pin_session_lock(s);
    double tnow = pin_session_now();

    /* "signal" for DV/HDV means data arrived recently */
    if (s->signal && tnow - s->last_data_s > 1.0) {
        s->signal = 0;
        pin_session_push_event(s, PIN_EVT_INPUT_FORMAT, 0, NULL);
    }

    /* Bus watch: a camera switched on or off (or a cable moved) resets the
     * bus. During a capture that ends the capture: the reset interrupts the
     * stream (frames are lost while the plug is connected again), and a loose
     * cable must not go unnoticed in a file that looks complete. */
    if ((s->state == PIN_STATE_READY || capture_active(s)) && !s->dv_rescan) {
        int rescan = 0, bus_changed = 0;
        if (ctx->bus_pending) {
            uint32_t v;
            if (p1394_reg_read_poll(&s->link, P1394_OHCI_SELF_ID_COUNT, &v)) {
                ctx->bus_pending = 0;
                if (!ctx->bus_sig_valid) {
                    ctx->bus_sig = v;
                    ctx->bus_sig_valid = 1;
                } else if (v != ctx->bus_sig) {
                    /* Some cameras answer our own link-init bus reset with one
                     * of their own about half a second later, same topology.
                     * Re-scanning for that resets the bus again and loops
                     * forever; take it as the new baseline instead. A change
                     * in the self-ID size (node count) is always a re-scan. */
                    int same_topology = ((v ^ ctx->bus_sig) & 0x7fcu) == 0;
                    if (same_topology && tnow - s->stream_start_s < 3.0) {
                        ctx->bus_sig = v;
                    } else {
                        s->dv_rescan_retries = 0;
                        rescan = 1;
                        bus_changed = 1;
                    }
                }
            } else if (tnow - ctx->bus_poll_s > 1.0) {
                ctx->bus_pending = 0;
            }
        } else if (tnow - ctx->bus_poll_s >= 0.5) {
            ctx->bus_poll_s = tnow;
            ctx->bus_pending = 1;
            p1394_reg_read_begin(&s->link, P1394_OHCI_SELF_ID_COUNT);
        }
        /* another node is on the bus but did not answer: it was probably
         * still starting up when we looked. Look again a few times. */
        if (!rescan && !s->dev.camera_node && s->dev.node_count > 1 &&
            s->dv_rescan_retries < 5 && tnow - s->stream_start_s > 2.0) {
            s->dv_rescan_retries++;
            rescan = 1;
        }
        /* only a capture that is under way (file open, or the deck rewinding
         * for it); one still waiting for its first frame just waits on */
        if (bus_changed && (s->capture_began || s->rewind_before_capture || s->pass_rewinding)) {
            capture_end(s, PIN_STOP_CAMERA_LOST,
                        "the FireWire connection to the camera was interrupted (cable moved or "
                        "camera switched off)", 0);
            set_state(s, PIN_STATE_READY);
        }
        if (rescan) {
            s->dv_rescan = 1;
            s->reconnecting = 1;
            s->loop_stop = 1;
            snprintf(s->step_text, sizeof(s->step_text),
                     "The FireWire bus changed; looking for the camera again");
            s->progress_pct = -1;
        }
    }

    if (s->deck_busy) {
        pin_deck_async_status_t st = pin_deck_async_poll(&s->deck_async, pin_session_now());
        if (st == PIN_DECK_ASYNC_DONE || st == PIN_DECK_ASYNC_FAILED) {
            s->deck_busy = 0;
            if (s->deck_async.cmd[0] == 0x00)   /* a transport command, not a status query */
                s->deck_cmd_done_s = pin_session_now();
            if (st == PIN_DECK_ASYNC_DONE && s->deck_async.cmd[2] == 0x51) {
                /* TIME CODE answer: the tape position, shown while no DV/HDV stream
                 * carries its own timecode (winding, stopped). */
                pin_deck_timecode_t tc;
                if (pin_deck_parse_timecode(s->deck_async.resp, s->deck_async.resp_len, &tc) == 0 &&
                    !s->signal)
                    snprintf(s->timecode, sizeof(s->timecode), "%02d:%02d:%02d%c%02d",
                             tc.hour, tc.minute, tc.second, tc.drop_frame ? ';' : ':', tc.frame);
                if (s->deck != PIN_DECK_REWINDING && s->deck != PIN_DECK_FAST_FORWARD)
                    s->tc_after_wind = 0;
            } else if (st == PIN_DECK_ASYNC_DONE && s->deck_async.resp_len >= 4 &&
                       s->deck_async.resp[0] != 0x08) {
                /* (A NOT_IMPLEMENTED (08) answer echoes the command; it says nothing about the state.) */
                pin_deck_state_t prev = s->deck;
                uint8_t op = s->deck_async.resp[2], mode = s->deck_async.resp[3];
                s->deck = pin_deck_state_from_avc(op, mode);
                if ((prev == PIN_DECK_REWINDING || prev == PIN_DECK_FAST_FORWARD) &&
                    s->deck != PIN_DECK_REWINDING && s->deck != PIN_DECK_FAST_FORWARD)
                    s->tc_after_wind = 1;
                pin_session_push_event(s, PIN_EVT_DECK, (int32_t)s->deck, NULL);
            }
            if (s->deck_q_valid) {
                s->deck_q_valid = 0;
                deck_send(s, node, s->deck_q_cmd);
            }
        }
    } else if (node) {
        /* ~1 Hz transport-state poll (async); drives rewind/BOT detection. While
         * the tape winds (and once more after it stopped) with no DV stream to
         * read the timecode from, every other poll asks the deck's TIME CODE
         * instead, so the timecode display follows the tape (2 Hz polling). */
        double now = pin_session_now();
        int winding = s->deck == PIN_DECK_REWINDING || s->deck == PIN_DECK_FAST_FORWARD;
        if (now - s->last_transport_poll_s > (winding ? 0.5 : 1.0)) {
            s->last_transport_poll_s = now;
            int want_tc = !s->signal && (winding || s->tc_after_wind);
            pin_deck_query_t q = s->poll_tc_next && want_tc ? PIN_DECK_QUERY_TIMECODE
                                                              : PIN_DECK_QUERY_STATE;
            s->poll_tc_next = q == PIN_DECK_QUERY_STATE && want_tc;
            pin_deck_async_start_query(&s->deck_async, &s->link, node, q, now);
            s->deck_busy = 1;
        }
    }

    if (s->capture_start_s > 0)
        s->elapsed_s = pin_session_now() - s->capture_start_s;
    s->idle_s = pin_session_now() - s->last_data_s;
    if (s->writer) {
        pin_writer_stats_t wst;
        pin_writer_get_stats(s->writer, &wst);
        s->writer_backlog = wst.backlog_bytes;
        s->writer_backlog_max = wst.backlog_high_water;
        /* wst.bytes_pushed is raw, pre-encode bytes queued to the writer --
         * for sink_ffv1 that's uncompressed YUYV+PCM, many times the actual
         * FFV1-compressed file size. Prefer the sink's own count of bytes
         * actually written to disk when it tracks one. */
        if (s->sink && s->sink->get_status) {
            pin_sink_status_t sst;
            s->sink->get_status(s->sink, &sst);
            s->bytes_written = sst.bytes_written;
        } else {
            s->bytes_written = wst.bytes_pushed;
        }
        rate_sample(s);
        capture_guard(s, auto_stop_node(s, node));
    }

    /* No-signal stop (idle_stop_minutes) and total-time stop (max_duration_minutes).
     * The no-signal timeout is also how the end of the tape is noticed: with
     * passes left it ends this pass, rewinds and starts the next one. The
     * time limit is per pass: elapsed_s counts only the capture time of the
     * current pass (it restarts when the next pass starts, so the rewind is
     * not counted) and ends the pass exactly like the no-signal timeout; in
     * the last pass either one ends the capture and a capture that drives the
     * deck stops the deck. */
    int duration_stop = s->capture_opts.max_duration_minutes > 0 &&
                         s->elapsed_s > s->capture_opts.max_duration_minutes * 60.0;
    int idle_stop = s->capture_opts.idle_stop_minutes > 0 &&
                    s->idle_s > s->capture_opts.idle_stop_minutes * 60.0;
    if (s->state == PIN_STATE_CAPTURING && (idle_stop || duration_stop)) {
        if (s->pass < s->passes) {
            /* end this pass, rewind, next pass */
            finish_file(s);
            set_state(s, PIN_STATE_REWINDING);
            s->pass_rewinding = 1;
            s->rewind_wait_start_s = pin_session_now();
            deck_send(s, node, PIN_DECK_CMD_REW);
            s->pass_index++;
            s->pass++;
            s->last_data_s = pin_session_now(); /* reset idle timer across the rewind */
        } else {
            /* last pass: stop */
            char mins[32], detail[PIN_TEXT_MAX];
            if (idle_stop) {
                minutes_text(s->capture_opts.idle_stop_minutes, mins, sizeof(mins));
                snprintf(detail, sizeof(detail), "there was no camera or deck signal for %s", mins);
            } else {
                minutes_text(s->capture_opts.max_duration_minutes, mins, sizeof(mins));
                snprintf(detail, sizeof(detail), "the time limit of %s was reached", mins);
            }
            capture_end(s, idle_stop ? PIN_STOP_NO_SIGNAL : PIN_STOP_TIME_LIMIT, detail,
                        auto_stop_node(s, node));
            set_state(s, PIN_STATE_READY);
        }
    } else if (s->state == PIN_STATE_REWINDING && (s->rewind_before_capture || s->pass_rewinding) &&
               s->deck == PIN_DECK_STOPPED && !s->deck_busy && !s->deck_q_valid &&
               s->deck_cmd_done_s >= s->rewind_wait_start_s &&
               pin_session_now() - s->deck_cmd_done_s > 3.0) {
        /* Rewind finished (BOT). A status query answered within a moment of
         * the REW command can still report the old "stopped", so the deck must
         * also have had a few seconds since REW was acknowledged. */
        if (s->rewind_before_capture) {
            /* "Automatic rewind & capture": this was the initial rewind (pass 1).
             * maybe_begin_capture() sends PLAY and opens the file once the
             * stream kind is known; READY meanwhile, as for any capture that
             * is waiting for its first data. */
            s->rewind_before_capture = 0;
            s->rewind_done = 1;
            set_state(s, PIN_STATE_READY);
            maybe_begin_capture(s, node);
        } else {
            /* start the next pass */
            s->pass_rewinding = 0;
            deck_send(s, node, PIN_DECK_CMD_PLAY);
            s->scene_index = first_scene_number(s);
            open_sink_for_scene(s);
            set_state(s, PIN_STATE_CAPTURING);
            pin_session_push_event(s, PIN_EVT_PASS, s->pass, NULL);
            s->capture_start_s = pin_session_now();
            s->elapsed_s = 0;
            s->last_data_s = pin_session_now(); /* no-signal timer restarts with the pass */
            s->idle_s = 0;
            s->scene_det_ready = 0;
            s->hdv_await_gop = s->stream_kind == PIN_KIND_HDV;
        }
    }
    pin_session_unlock(s);
}

/* ========================================================================
 * DV/HDV bring-up and persistent streaming
 * ==================================================================== */

static int replay_run(pin_session_t *s); /* forward decl, see below */

/* dv_reassembler_feed() returns 0/-1 (0 = keep going); pinnacle_data_cb's
 * convention is "0 = keep reading, non-zero = stop" -- the same polarity,
 * so a plain same-signature passthrough works with no cast. */
static int dv_reassembler_feed_cb_shim(const uint8_t *data, size_t len, void *user)
{
    return dv_reassembler_feed((dv_reassembler_t *)user, data, len);
}

static void do_run_dv(pin_session_t *s)
{
    if (s->is_replay) {
        replay_run(s);
        return;
    }

    char fw[PIN_PATH_MAX], why[PIN_TEXT_MAX];
    if (pin_session_firmware_path(s->dev.model, PIN_KIND_DV, fw, sizeof(fw), why, sizeof(why)) != PIN_OK) {
        set_error(s, PIN_ERR_FIRMWARE, why);
        return;
    }

    pinnacle_status_t pst = pinnacle_init_hardware(&s->dev, fw);
    if (pst != PINNACLE_OK) {
        set_init_error(s, pst, fw);
        return;
    }
    pinnacle_lock_update(s->lock, PINNACLE_LOCK_READY, s->dev.guid_hi, s->dev.guid_lo);

    /* switched away while we were bringing the device up: don't start a stream */
    pin_session_lock(s);
    int abandoned = s->cmd.pending || s->worker_stop;
    pin_session_unlock(s);
    if (abandoned)
        return;

    /* One pass per look at the 1394 bus. A change of topology (camera
     * switched on or off, cable moved) sends us round again: the link is
     * re-initialised, the camera looked for and connected afresh. */
    for (int first = 1;; first = 0) {
        pst = pinnacle_stream_start(&s->dev);
        if (pst != PINNACLE_OK) {
            set_error(s, PIN_ERR_USB, s->dev.fail_detail[0] ? s->dev.fail_detail : NULL);
            return;
        }
        pin_session_lock(s);
        s->camera_present = s->dev.camera_node ? 1 : 0;
        if (!s->camera_present) {
            s->deck = PIN_DECK_UNKNOWN;
            s->tape_percent = -1;
            s->deck_busy = 0;
        }
        s->reconnecting = 0;
        s->dv_rescan = 0;
        s->step_text[0] = 0;
        if (!s->camera_present) {
            s->timecode[0] = 0;
            s->rec_datetime[0] = 0;
        }
        pin_session_push_event(s, PIN_EVT_INPUT_FORMAT, 0, NULL);
        pin_session_unlock(s);

        p1394_init(&s->link, &s->dev);
        if (first) {
            pin_session_lock(s);
            set_state(s, PIN_STATE_READY);
            pin_session_unlock(s);
        }

        dv_reassembler_t reasm;
        dv_output_t out = { .write = dv_write_cb, .on_unit = dv_on_unit, .user = NULL };
        dv_ctx_t ctx = { .s = s, .reasm = &reasm };
        out.user = &ctx;
        dv_reassembler_init(&reasm, &out);
        s->reasm = reasm; /* kept for status/inspection only */

        s->loop_stop = 0;
        s->last_data_s = pin_session_now();
        s->stream_start_s = s->last_data_s;
        pinnacle_status_t lst = pinnacle_stream_read_loop_ex(&s->dev, dv_reassembler_feed_cb_shim,
                                                             &reasm, &s->loop_stop, &s->link,
                                                             dv_tick, &ctx);
        /* The loop only returns by itself when the USB stream failed
         * (device unplugged); otherwise loop_stop was set. */
        if (lst != PINNACLE_OK || !s->loop_stop) {
            dv_reassembler_finish(&reasm);
            if (stream_failed(s))
                break; /* nothing left to stop on a device that is gone */
            pinnacle_stream_stop(&s->dev);
            break;
        }

        pin_session_lock(s);
        /* a pending SET_INPUT / CLOSE wins over a re-scan */
        int rescan = s->dv_rescan && !s->worker_stop && !s->cmd.pending;
        if (!rescan)
            capture_end(s, PIN_STOP_USER, NULL, 0); /* input switch / close */
        pin_session_unlock(s);
        dv_reassembler_finish(&reasm);
        pinnacle_stream_stop(&s->dev);
        if (!rescan)
            break;
    }
}

/* ========================================================================
 * analog
 * ==================================================================== */

typedef struct {
    pin_session_t *s;
    uint32_t video_index;
    double last_status_s;   /* analog_tick(): last decoder status poll */
    int auto_mismatch;      /* consecutive polls whose 50/60 Hz differs from the configured standard */
} analog_ctx_t;

static int analog_video_cb(const pinnacle_video_frame_t *f, void *user)
{
    analog_ctx_t *ctx = user;
    pin_session_t *s = ctx->s;

    pin_session_lock(s);
    s->stream_kind = PIN_KIND_ANALOG;
    int kind_newly_known = !s->stream_kind_known;
    s->stream_kind_known = 1;
    s->signal = 1;
    s->width = (int)f->width; s->height = (int)f->height;
    pin_aspect_t asp = s->aspect_override;
    s->dar_num = asp == PIN_ASPECT_16_9 ? 16 : 4;
    s->dar_den = asp == PIN_ASPECT_16_9 ? 9 : 3;

    count_frame(s, f->repeated, f->repeated);              /* a repeated frame is a dropped one */
    if (f->repeated && f->received) s->frames_damaged++;   /* ...because it arrived short */
    s->last_data_s = pin_session_now();
    s->idle_s = 0;
    if (s->capture_start_s > 0)
        s->elapsed_s = pin_session_now() - s->capture_start_s;

    pin_previewer_push_analog(s->preview, f->yuyv, f->width, f->height, !s->is_60hz);

    if (s->writer && !pin_writer_push(s->writer, PIN_UNIT_VIDEO, f->index, f->yuyv,
                                      (size_t)f->width * f->height * 2))
        s->write_dropped++;

    if (kind_newly_known)
        maybe_begin_capture(s, 0); /* analog has no deck/camera_node */
    pin_session_unlock(s);
    return s->loop_stop ? 1 : 0;
}

/* Runs at least every 50 ms whether or not frames arrive: without an input
 * signal the SAA7113 sends nothing at all, so commands, signal / standard
 * polling and the idle timeout must not depend on analog_video_cb(). */
static int analog_tick(void *user)
{
    analog_ctx_t *ctx = user;
    pin_session_t *s = ctx->s;
    double now = pin_session_now();

    pin_session_lock(s);
    if (s->ctl_dirty_picture) {
        s->ctl_dirty_picture = 0;
        pinnacle_analog_set_picture(&s->analog, &s->want_picture);
    }
    if (s->ctl_dirty_gain) {
        s->ctl_dirty_gain = 0;
        pinnacle_analog_set_audio_gain(&s->analog, s->want_gain_db10);
    }
    /* The input's kind and geometry are known from the moment the decoder
     * is up, signal or not, so a capture can start (and wait) without one. */
    if (!s->stream_kind_known) {
        s->stream_kind = PIN_KIND_ANALOG;
        s->stream_kind_known = 1;
        s->width = (int)s->analog.width;
        s->height = (int)s->analog.height;
        maybe_begin_capture(s, 0);
    }

    /* Decoder status is one I2C byte; polling it a few times a second is
     * cheap and is the only way to see a signal come and go. */
    if (now - ctx->last_status_s >= 0.25) {
        ctx->last_status_s = now;
        pinnacle_analog_status_t ast;
        if (pinnacle_analog_get_status(&s->analog, &ast) == PINNACLE_OK) {
            if (s->signal != ast.locked)
                pin_session_push_event(s, PIN_EVT_INPUT_FORMAT, 0, NULL);
            s->signal = ast.locked;
            if (s->requested_std == PIN_STD_AUTO) {
                /* FIDT is meaningless without lock; keep the last answer. */
                if (ast.locked && s->is_60hz != ast.is_60hz) {
                    s->is_60hz = ast.is_60hz;
                    pin_session_push_event(s, PIN_EVT_INPUT_FORMAT, 0, NULL);
                }
                s->detected_std = s->is_60hz ? PIN_STD_NTSC : PIN_STD_PAL;
            }

            /* Put the decoder and the frame geometry on the standard the
             * user chose, or (Auto) the one the source turns out to have;
             * do_run_analog() restarts the video at that standard. */
            pinnacle_std_t want = s->analog.cfg.standard;
            if (s->requested_std != PIN_STD_AUTO) {
                want = (pinnacle_std_t)(s->requested_std - 1);
            } else if (ast.locked && ast.is_60hz != pinnacle_std_is_60hz(want)) {
                if (++ctx->auto_mismatch >= 3)
                    want = ast.is_60hz ? PINNACLE_STD_NTSC : PINNACLE_STD_PAL;
            } else {
                ctx->auto_mismatch = 0;
            }
            if (want != s->analog.cfg.standard && s->state == PIN_STATE_READY) {
                s->analog_restart = 1;
                s->analog_target_std = want;
            }
        }
    }
    if (s->requested_std != PIN_STD_AUTO) {
        s->detected_std = s->requested_std;
        s->is_60hz = s->requested_std == PIN_STD_NTSC || s->requested_std == PIN_STD_NTSC_443 ||
                     s->requested_std == PIN_STD_NTSC_J || s->requested_std == PIN_STD_PAL_60 ||
                     s->requested_std == PIN_STD_PAL_M;
    }

    /* Idle: time since the last real (non-repeated) frame with a signal. */
    if (!s->signal)
        s->idle_s = now - s->last_data_s;
    if (s->state == PIN_STATE_CAPTURING && s->capture_start_s > 0)
        s->elapsed_s = now - s->capture_start_s;
    if (s->writer) {
        pin_writer_stats_t wst;
        pin_writer_get_stats(s->writer, &wst);
        s->writer_backlog = wst.backlog_bytes;
        s->writer_backlog_max = wst.backlog_high_water;
        /* See the analog tick handler's identical comment above sst: prefer
         * the sink's actual on-disk byte count over the writer's raw,
         * pre-encode push count. */
        if (s->sink && s->sink->get_status) {
            pin_sink_status_t sst;
            s->sink->get_status(s->sink, &sst);
            s->bytes_written = sst.bytes_written;
        } else {
            s->bytes_written = wst.bytes_pushed;
        }
        rate_sample(s);
        capture_guard(s, 0);
    }
    int idle_stop = s->capture_opts.idle_stop_minutes > 0 &&
                    now - s->last_data_s > s->capture_opts.idle_stop_minutes * 60.0;
    int should_stop = s->state == PIN_STATE_CAPTURING &&
                      (idle_stop || (s->capture_opts.max_duration_minutes > 0 &&
                                     s->elapsed_s > s->capture_opts.max_duration_minutes * 60.0));
    pin_session_unlock(s);

    if (handle_inline_commands(s, 0)) {
        s->loop_stop = 1;
        return 1;
    }
    if (s->analog_restart)
        return 1;
    if (should_stop) {
        pin_session_lock(s);
        char mins[32], detail[PIN_TEXT_MAX];
        if (idle_stop) {
            minutes_text(s->capture_opts.idle_stop_minutes, mins, sizeof(mins));
            snprintf(detail, sizeof(detail), "there was no video signal for %s", mins);
        } else {
            minutes_text(s->capture_opts.max_duration_minutes, mins, sizeof(mins));
            snprintf(detail, sizeof(detail), "the time limit of %s was reached", mins);
        }
        capture_end(s, idle_stop ? PIN_STOP_NO_SIGNAL : PIN_STOP_TIME_LIMIT, detail, 0);
        set_state(s, PIN_STATE_READY);
        pin_session_unlock(s);
    }
    return s->loop_stop ? 1 : 0;
}

static int analog_audio_cb(const pinnacle_audio_block_t *b, void *user)
{
    analog_ctx_t *ctx = user;
    pin_session_t *s = ctx->s;

    pin_session_lock(s);
    /* b->pcm is documented as "16-bit LE stereo, interleaved" raw bytes;
     * reinterpret as int16_t (valid on every platform this project targets,
     * all little-endian) rather than the byte-at-a-time reads the inline
     * version of this code used to do before being factored out. */
    if (b->samples && !b->silence)
        feed_audio_locked(s, (const int16_t *)b->pcm, b->samples);
    if (s->writer && b->samples &&
        !pin_writer_push(s->writer, PIN_UNIT_AUDIO, b->seq, (const uint8_t *)b->pcm,
                         (size_t)b->samples * 4))
        s->write_dropped++;
    pin_session_unlock(s);
    return 0;
}

static void do_run_analog(pin_session_t *s, pin_input_t input)
{
    char fw[PIN_PATH_MAX], why[PIN_TEXT_MAX];
    if (pin_session_firmware_path(s->dev.model, PIN_KIND_ANALOG, fw, sizeof(fw), why, sizeof(why)) != PIN_OK) {
        set_error(s, PIN_ERR_FIRMWARE, why);
        return;
    }

    pinnacle_analog_config_t cfg;
    pinnacle_analog_config_defaults(&cfg);
    cfg.input = input == PIN_INPUT_SVIDEO ? PINNACLE_INPUT_SVIDEO : PINNACLE_INPUT_COMPOSITE;
    pin_session_lock(s);
    cfg.picture = s->want_picture;
    s->ctl_dirty_picture = 0;
    s->ctl_dirty_gain = 1; /* the gain goes on after pinnacle_analog_start() resets it */
    pin_session_unlock(s);
    if (s->requested_std != PIN_STD_AUTO)
        cfg.standard = (pinnacle_std_t)(s->requested_std - 1); /* enums line up 1:1, see pin_std_t */

    pinnacle_status_t pst = pinnacle_analog_open(&s->analog, &s->dev, fw, &cfg);
    if (pst != PINNACLE_OK) {
        set_init_error(s, pst, fw);
        return;
    }
    pinnacle_lock_update(s->lock, PINNACLE_LOCK_READY, s->dev.guid_hi, s->dev.guid_lo);

    /* Auto: start on the source's own 50/60 Hz rather than the PAL default.
     * analog_tick() only switches while READY, so a capture started before
     * its re-check would keep the wrong geometry. The decoder needs a
     * moment to lock after the input is selected. */
    if (s->requested_std == PIN_STD_AUTO) {
        pinnacle_analog_status_t ast = { 0 };
        for (int i = 0; i < 20; i++) {
            if (pinnacle_analog_get_status(&s->analog, &ast) == PINNACLE_OK && ast.locked)
                break;
            sleep_ms(50);
        }
        if (ast.locked && ast.is_60hz != pinnacle_std_is_60hz(s->analog.cfg.standard))
            pinnacle_analog_set_standard(&s->analog, ast.is_60hz ? PINNACLE_STD_NTSC : PINNACLE_STD_PAL);
        pin_session_lock(s);
        s->is_60hz = pinnacle_std_is_60hz(s->analog.cfg.standard);
        s->detected_std = s->is_60hz ? PIN_STD_NTSC : PIN_STD_PAL;
        pin_session_unlock(s);
    }

    pst = pinnacle_analog_start(&s->analog);
    if (pst != PINNACLE_OK) {
        set_error(s, PIN_ERR_USB, NULL);
        return;
    }
    set_state(s, PIN_STATE_READY);

    analog_ctx_t ctx = { .s = s };
    pinnacle_capture_sink_t sink = { .video = analog_video_cb, .audio = analog_audio_cb,
                                     .tick = analog_tick, .user = &ctx };
    pinnacle_capture_stats_t stats;
    s->loop_stop = 0;
    s->last_data_s = pin_session_now();

    /* auto-standard re-check while READY (the plan's "re-check while READY,
     * emit PIN_EVT_INPUT_FORMAT on change") happens inside analog_video_cb()
     * (every frame -> cheap I2C status byte read, ~25-30 Hz) rather than a
     * separate loop here, since pinnacle_analog_capture_loop() owns the USB
     * event loop for as long as this input stays selected. */
    int device_gone = 0;
    for (;;) {
        pinnacle_status_t lst = pinnacle_analog_capture_loop(&s->analog, &sink, &stats, &s->loop_stop);
        if (lst != PINNACLE_OK && !s->loop_stop && !s->analog_restart) {
            device_gone = stream_failed(s);
            break;
        }
        pin_session_lock(s);
        int restart = s->analog_restart && !s->loop_stop && !s->worker_stop && !s->cmd.pending;
        pinnacle_std_t target = s->analog_target_std;
        s->analog_restart = 0;
        pin_session_unlock(s);
        if (!restart)
            break;
        /* new standard: geometry and audio packet size are fixed at start */
        pinnacle_analog_stop(&s->analog);
        pinnacle_analog_set_standard(&s->analog, target);
        pin_session_lock(s);
        s->width = (int)s->analog.width;
        s->height = (int)s->analog.height;
        s->is_60hz = pinnacle_std_is_60hz(target);
        s->detected_std = (pin_std_t)(target + 1);
        pin_session_push_event(s, PIN_EVT_INPUT_FORMAT, 0, NULL);
        pin_session_unlock(s);
        ctx.auto_mismatch = 0;
        s->ctl_dirty_gain = 1;
        if (pinnacle_analog_start(&s->analog) != PINNACLE_OK) {
            pin_session_lock(s);
            set_error(s, PIN_ERR_USB, NULL);
            pin_session_unlock(s);
            break;
        }
    }

    pin_session_lock(s);
    capture_end(s, PIN_STOP_USER, NULL, 0); /* input switch / close */
    pin_session_unlock(s);
    if (!device_gone)
        pinnacle_analog_stop(&s->analog);
}

/* ========================================================================
 * replay (virtual device)
 * ==================================================================== */

/* EOT: for multi-pass testing (the plan's "multi-pass with replay EOT")
 * this drives the same pass sequencing the real async transport-state poll
 * (dv_tick()) would on hardware -- replay has no such poll (there is no
 * real deck to ask), so that logic is reproduced here instead. Shared by
 * every replay source mode (DV frames, raw EP 0x88 dump, HDV .ts). */
static void replay_handle_eot(pin_session_t *s)
{
    pin_session_lock(s);
    s->deck = PIN_DECK_STOPPED;
    pin_session_push_event(s, PIN_EVT_DECK, (int32_t)s->deck, NULL);
    if (s->sink && s->pass < s->passes) {
        finish_file(s);
        s->pass_index++;
        s->pass++;
        s->scene_index = first_scene_number(s);
        open_sink_for_scene(s);
        s->deck = PIN_DECK_PLAYING;
        s->capture_start_s = pin_session_now();
        pin_session_push_event(s, PIN_EVT_PASS, s->pass, NULL);
    } else if (s->sink) {
        /* last pass: stop */
        capture_end(s, PIN_STOP_END_OF_TAPE, NULL, 0);
        set_state(s, PIN_STATE_READY);
    } else {
        /* no capture attached: just a looping preview source */
        s->deck = PIN_DECK_PLAYING;
    }
    pin_session_unlock(s);
}

/* HDV .ts replay: the file is already a plain, 188-byte-aligned MPEG2-TS
 * (unlike the real EP 0x88 stream, it carries no OHCI/type-9 framing), so
 * dv_reassembler_feed() does not apply. Instead this mirrors what
 * dv_reassembler.c does internally for HDV (see its file header comment
 * and hdv_aux.h): track the PAT/PMT to learn the video PID, then cut a new
 * "picture" unit every time a TS packet on that PID has
 * payload_unit_start_indicator set (a video PES start). Each accumulated
 * picture is handed to dv_write_cb()/dv_on_unit() exactly as
 * dv_reassembler_feed() would during a real capture. */
static int replay_run_ts(pin_session_t *s, FILE *f)
{
    p1394_init(&s->link, &s->dev);
    s->camera_present = 1;
    set_state(s, PIN_STATE_READY);
    s->deck = PIN_DECK_PLAYING;

    dv_ctx_t ctx = { .s = s };
    double next_t = 0; /* pace_frame() deadline */
    hdv_pid_map_t map;
    memset(&map, 0, sizeof(map));
    map.pmt_pid = -1; map.video_pid = -1; map.aux_pid = -1; map.audio_pid = -1;

    uint8_t *pic = NULL;
    size_t pic_len = 0, pic_cap = 0;
    uint8_t pkt[HDV_TS_PACKET_SIZE];

    s->loop_stop = 0;
    while (!s->loop_stop) {
        if (handle_inline_commands(s, 0)) { s->loop_stop = 1; break; }

        pin_session_lock(s);
        capture_guard(s, 0);
        pin_deck_state_t d = s->deck;
        pin_session_unlock(s);
        if (d == PIN_DECK_STOPPED || d == PIN_DECK_PAUSED) { sleep_ms(50); continue; }
        if (d == PIN_DECK_REWINDING) {
            sleep_ms(2000); /* simulated rewind time, per the plan */
            fseek(f, 0, SEEK_SET);
            pic_len = 0;
            memset(&map, 0, sizeof(map)); map.pmt_pid = -1; map.video_pid = -1; map.aux_pid = -1; map.audio_pid = -1;
            pin_session_lock(s);
            s->deck = PIN_DECK_STOPPED;
            pin_session_push_event(s, PIN_EVT_DECK, (int32_t)s->deck, NULL);
            pin_session_unlock(s);
            continue;
        }
        if (d == PIN_DECK_FAST_FORWARD) {
            fseek(f, (long)HDV_TS_PACKET_SIZE * 4000, SEEK_CUR);
            sleep_ms(20);
            continue;
        }

        size_t n = fread(pkt, 1, sizeof(pkt), f);
        if (n != sizeof(pkt)) {
            replay_handle_eot(s);
            fseek(f, 0, SEEK_SET);
            pic_len = 0;
            memset(&map, 0, sizeof(map)); map.pmt_pid = -1; map.video_pid = -1; map.aux_pid = -1; map.audio_pid = -1;
            continue;
        }

        hdv_scan_pat_pmt(pkt, 1, &map);
        int pid = ((pkt[1] & 0x1f) << 8) | pkt[2];
        int pusi = (pkt[1] & 0x40) != 0;
        int is_video_start = pusi && map.video_pid > 0 && pid == map.video_pid;

        if (is_video_start && pic_len > 0) {
            dv_write_cb(pic, pic_len, &ctx);
            dv_on_unit(DV_FORMAT_HDV, pic, pic_len, &ctx);
            pic_len = 0;
            pace_frame(&next_t, 1001.0 / 30000.0); /* one picture emitted */
        }
        if (pic_len + sizeof(pkt) > pic_cap) {
            pic_cap = (pic_len + sizeof(pkt)) * 2 + 4096;
            uint8_t *grown = realloc(pic, pic_cap);
            if (!grown) { free(pic); pic = NULL; pic_cap = pic_len = 0; break; }
            pic = grown;
        }
        memcpy(pic + pic_len, pkt, sizeof(pkt));
        pic_len += sizeof(pkt);
    }
    free(pic);

    pin_session_lock(s);
    capture_end(s, PIN_STOP_USER, NULL, 0);
    pin_session_unlock(s);
    fclose(f);
    return 0;
}

static int replay_run(pin_session_t *s)
{
    const char *path = s->device_id + 7; /* "replay:<file>" */
    FILE *f = fopen(path, "rb");
    if (!f) {
        set_error(s, PIN_ERR_NOT_FOUND, "replay file not found");
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    fseek(f, 0, SEEK_SET);
    double next_t = 0; /* pace_frame() deadline */

    const char *ext = strrchr(path, '.');
    int is_dv_frames = ext && (strcmp(ext, ".dv") == 0);
    int is_ts = ext && (strcmp(ext, ".ts") == 0);
    if (is_ts)
        return replay_run_ts(s, f); /* closes f itself */

    p1394_init(&s->link, &s->dev); /* unused fields only; no real 1394 for replay */
    s->camera_present = 1;
    set_state(s, PIN_STATE_READY);
    s->deck = PIN_DECK_PLAYING;

    dv_reassembler_t reasm;
    dv_ctx_t ctx = { .s = s };
    dv_output_t out = { .write = dv_write_cb, .on_unit = dv_on_unit, .user = &ctx };
    dv_reassembler_init(&reasm, &out);

    long frame_size = 0;
    if (is_dv_frames && total > 0) {
        if (total % 144000 == 0) frame_size = 144000;
        else if (total % 120000 == 0) frame_size = 120000;
    }

    uint8_t *chunk = malloc(frame_size ? (size_t)frame_size : 65536);
    s->loop_stop = 0;
    while (!s->loop_stop) {
        if (handle_inline_commands(s, 0)) { s->loop_stop = 1; break; }

        pin_session_lock(s);
        capture_guard(s, 0);
        pin_deck_state_t d = s->deck;
        pin_session_unlock(s);
        if (d == PIN_DECK_STOPPED || d == PIN_DECK_PAUSED) { sleep_ms(50); continue; }
        if (d == PIN_DECK_REWINDING) {
            sleep_ms(2000); /* simulated rewind time, per the plan */
            fseek(f, 0, SEEK_SET);
            pin_session_lock(s);
            s->deck = PIN_DECK_STOPPED;
            pin_session_push_event(s, PIN_EVT_DECK, (int32_t)s->deck, NULL);
            pin_session_unlock(s);
            continue;
        }
        if (d == PIN_DECK_FAST_FORWARD) {
            fseek(f, frame_size ? frame_size * 25 : 65536 * 25, SEEK_CUR);
            sleep_ms(20);
            continue;
        }

        size_t want = frame_size ? (size_t)frame_size : 65536;
        size_t n = fread(chunk, 1, want, f);
        if (n == 0 || (frame_size && n != (size_t)frame_size)) {
            replay_handle_eot(s);
            /* Loop the source file either way (the plan's "and loops"),
             * whether or not a capture is/was attached, so the preview
             * keeps something to show and a not-yet-multi-pass-finished
             * capture has a tape to rewind to. */
            fseek(f, 0, SEEK_SET);
            continue;
        }
        if (frame_size) {
            dv_write_cb(chunk, n, &ctx);
            dv_on_unit(DV_FORMAT_DV, chunk, n, &ctx);
        } else {
            dv_reassembler_feed(&reasm, chunk, n);
        }
        /* real-time pacing for frame mode (NTSC 29.97 / PAL 25 fps), else a
         * small sleep so a raw-dump replay doesn't spin a core at full tilt. */
        if (frame_size)
            pace_frame(&next_t, frame_size == 144000 ? 1.0 / 25.0 : 1001.0 / 30000.0);
        else
            sleep_ms(5);
    }
    free(chunk);

    pin_session_lock(s);
    capture_end(s, PIN_STOP_USER, NULL, 0);
    pin_session_unlock(s);
    dv_reassembler_finish(&reasm);
    fclose(f);
    return 0;
}

/* ========================================================================
 * worker thread
 * ==================================================================== */

static void *worker_main(void *arg)
{
    pin_session_t *s = arg;

    for (;;) {
        pthread_mutex_lock(&s->mtx);
        while (!s->worker_stop && !s->cmd.pending)
            pthread_cond_wait(&s->cmd_posted, &s->mtx);
        if (s->worker_stop && !s->cmd.pending) {
            pthread_mutex_unlock(&s->mtx);
            break;
        }
        pin_cmd_t cmd = s->cmd;
        pthread_mutex_unlock(&s->mtx);

        if (cmd.kind == PIN_CMD_CLOSE) {
            pthread_mutex_lock(&s->mtx);
            s->cmd.pending = 0; s->cmd.kind = PIN_CMD_NONE;
            pthread_cond_broadcast(&s->cmd_idle);
            pthread_mutex_unlock(&s->mtx);
            break;
        }

        if (cmd.kind == PIN_CMD_SET_INPUT) {
            pthread_mutex_lock(&s->mtx);
            s->cmd.pending = 0; s->cmd.kind = PIN_CMD_NONE;
            pthread_cond_broadcast(&s->cmd_idle);
            s->input = cmd.input;
            s->stream_kind_known = 0;
            s->signal = 0;
            reset_frame_counters(s);
            s->camera_present = -1;
            s->reconnecting = 0;
            s->dv_rescan = 0;
            s->analog_restart = 0;
            strncpy(s->step_text, "Preparing the device", sizeof(s->step_text) - 1);
            s->progress_pct = -1;
            s->capture_want_start = 0;
            s->rewind_before_capture = 0;
            set_state(s, PIN_STATE_PREPARING);
            pthread_mutex_unlock(&s->mtx);

            if (!s->is_replay && !s->dev.handle) {
                pinnacle_status_t pst = pinnacle_open_by_id(&s->dev, s->device_id);
                if (pst != PINNACLE_OK) {
                    pin_session_lock(s);
                    char omsg[PIN_TEXT_MAX];
                    snprintf(omsg, sizeof(omsg), "Cannot open the device: %s", pinnacle_strerror(pst));
                    set_error(s, pst == PINNACLE_ERR_BUSY ? PIN_ERR_BUSY : PIN_ERR_USB, omsg);
                    pin_session_unlock(s);
                    continue;
                }
                /* The same PINNACLE_* debug/tuning env vars the CLIs honour. */
                pinnacle_tuning_from_env(&s->dev.tuning);
                /* Publish the unit's GUID in the lock record straight away,
                 * so other windows can label it; the analog bring-up never
                 * reads it on its own. */
                uint32_t ghi, glo;
                if (pinnacle_read_guid(&s->dev, &ghi, &glo) == PINNACLE_OK)
                    pinnacle_lock_update(s->lock, PINNACLE_LOCK_PREPARING, ghi, glo);
            }

            if (!s->is_replay) {
                s->dev.progress = session_progress;
                s->dev.progress_user = s;
            }

            if (cmd.input == PIN_INPUT_DV) {
                do_run_dv(s);
            } else if (s->is_replay) {
                /* a replay file is DV/HDV; there is no analog hardware to
                 * bring up (that used to crash inside libusb) */
                pin_session_lock(s);
                set_error(s, PIN_ERR_STATE, "The replay device only provides the DV / HDV input");
                pin_session_unlock(s);
            } else {
                do_run_analog(s, cmd.input);
            }
            /* returning here means the loop was told to stop (new
             * SET_INPUT, or CLOSE): go back to the top and process it. */
            continue;
        }

        /* Any other command with nothing streaming (e.g. capture/deck
         * posted before pin_set_input has ever run): nothing to attach to. */
        pthread_mutex_lock(&s->mtx);
        set_error(s, PIN_ERR_STATE, "no input selected yet");
        s->cmd.pending = 0; s->cmd.kind = PIN_CMD_NONE;
        pthread_cond_broadcast(&s->cmd_idle);
        pthread_mutex_unlock(&s->mtx);
    }
    return NULL;
}

/* Posts a command and waits for the worker to at least accept it (not for
 * it to finish -- most commands are documented non-blocking). */
static void post_cmd(pin_session_t *s, const pin_cmd_t *cmd)
{
    pthread_mutex_lock(&s->mtx);
    s->cmd = *cmd;
    s->cmd.pending = 1;
    pthread_cond_signal(&s->cmd_posted);
    pthread_mutex_unlock(&s->mtx);
}

/* ========================================================================
 * public (engine-level) API
 * ==================================================================== */

pin_status_t pin_session_open(const char *device_id, pin_session_t **out)
{
    if (!out)
        return PIN_ERR_ARG;
    char resolved[PIN_PATH_MAX];
    if (resolve_device_id(device_id, resolved, sizeof(resolved)) != 0)
        return PIN_ERR_NOT_FOUND;

    pin_session_t *s = calloc(1, sizeof(*s));
    if (!s)
        return PIN_ERR_NOMEM;
    strncpy(s->device_id, resolved, sizeof(s->device_id) - 1);
    s->is_replay = strncmp(resolved, "replay:", 7) == 0;
    pthread_mutex_init(&s->mtx, NULL);
    pthread_cond_init(&s->cmd_posted, NULL);
    pthread_cond_init(&s->cmd_idle, NULL);
    pthread_mutex_init(&s->evq_mtx, NULL);
    pthread_mutex_init(&s->mon_mtx, NULL);
    s->requested_std = PIN_STD_AUTO;
    s->aspect_override = PIN_ASPECT_AUTO;
    s->tape_percent = -1;
    s->camera_present = -1;
    s->progress_pct = -1;
    s->preview = pin_previewer_create();
    s->hdv_audio = pin_hdv_audio_create(pin_session_feed_monitor_audio, s);
    s->mon_cap_frames = 48000 * 2; /* 2 s at 48 kHz */
    s->mon_buf = calloc(s->mon_cap_frames * 2, sizeof(int16_t));
    pinnacle_analog_config_defaults(&s->analog.cfg);
    pinnacle_picture_defaults(&s->want_picture);

    if (!s->is_replay) {
        pinnacle_status_t pst = pinnacle_lock_acquire(resolved, &s->lock);
        if (pst != PINNACLE_OK) {
            pin_previewer_destroy(s->preview);
            pin_hdv_audio_destroy(s->hdv_audio);
            free(s->mon_buf);
            pthread_mutex_destroy(&s->mtx);
            free(s);
            return pst == PINNACLE_ERR_BUSY ? PIN_ERR_BUSY : PIN_ERR_INTERNAL;
        }
    }

    s->state = PIN_STATE_CLOSED;
    if (pthread_create(&s->worker_thread, NULL, worker_main, s) != 0) {
        pinnacle_lock_release(s->lock);
        pin_previewer_destroy(s->preview);
        pin_hdv_audio_destroy(s->hdv_audio);
        free(s->mon_buf);
        pthread_mutex_destroy(&s->mtx);
        free(s);
        return PIN_ERR_INTERNAL;
    }
    s->worker_started = 1;
    *out = s;
    return PIN_OK;
}

void pin_session_close(pin_session_t *s)
{
    if (!s)
        return;
    pin_session_script_shutdown(s); /* it needs the worker to finalise a capture */
    s->loop_stop = 1;
    pin_cmd_t cmd = { .kind = PIN_CMD_CLOSE };
    pthread_mutex_lock(&s->mtx);
    s->worker_stop = 1;
    s->cmd = cmd;
    s->cmd.pending = 1;
    pthread_cond_signal(&s->cmd_posted);
    pthread_mutex_unlock(&s->mtx);
    if (s->worker_started)
        pthread_join(s->worker_thread, NULL);

    if (s->sink) { if (s->writer) pin_writer_stop(s->writer); close_sink_counted(s); }
    scene_fifo_clear(s);
    pin_split_free(&s->split);
    pinnacle_lock_release(s->lock);
    if (!s->is_replay)
        pinnacle_close(&s->dev);
    pin_previewer_destroy(s->preview);
    pin_hdv_audio_destroy(s->hdv_audio);
    free(s->mon_buf);
    pthread_mutex_destroy(&s->mtx);
    pthread_cond_destroy(&s->cmd_posted);
    pthread_cond_destroy(&s->cmd_idle);
    pthread_mutex_destroy(&s->evq_mtx);
    pthread_mutex_destroy(&s->mon_mtx);
    free(s);
}

pin_status_t pin_session_set_input(pin_session_t *s, pin_input_t input)
{
    if (!s) return PIN_ERR_ARG;
    pin_session_lock(s);
    if (s->state == PIN_STATE_CAPTURING) { pin_session_unlock(s); return PIN_ERR_STATE; }
    /* Say so at once: stopping the running input can take a moment, and until
     * then the status would keep describing the old one. */
    s->input = input;
    if (s->state != PIN_STATE_PREPARING)
        set_state(s, PIN_STATE_PREPARING);
    snprintf(s->step_text, sizeof(s->step_text), "Stopping the current input");
    s->progress_pct = -1;
    s->signal = 0;
    s->camera_present = -1;
    s->deck = PIN_DECK_UNKNOWN;
    s->deck_busy = 0;
    s->tape_percent = -1;
    s->timecode[0] = 0;
    pin_session_unlock(s);
    s->loop_stop = 1;
    pin_cmd_t cmd = { .kind = PIN_CMD_SET_INPUT, .input = input };
    post_cmd(s, &cmd);
    return PIN_OK;
}

pin_status_t pin_session_set_standard(pin_session_t *s, pin_std_t std)
{
    if (!s) return PIN_ERR_ARG;
    if ((unsigned)std >= PIN_STD_COUNT) return PIN_ERR_ARG;
    pin_session_lock(s);
    /* the frame geometry of a running capture cannot change */
    if (s->state == PIN_STATE_CAPTURING || s->state == PIN_STATE_STOPPING) {
        pin_session_unlock(s);
        return PIN_ERR_STATE;
    }
    s->requested_std = std;
    pin_session_unlock(s);
    return PIN_OK;
}

pin_status_t pin_session_get_control(pin_session_t *s, pin_control_t c, pin_control_info_t *out)
{
    if (!s || !out) return PIN_ERR_ARG;
    memset(out, 0, sizeof(*out));
    out->size = sizeof(*out);
    static const struct { const char *label; int32_t min, max, step, def; } tbl[PIN_CTL_COUNT] = {
        [PIN_CTL_BRIGHTNESS] = { "Brightness", 0, 255, 1, 128 },
        [PIN_CTL_CONTRAST]   = { "Contrast", 0, 127, 1, 64 },
        [PIN_CTL_SATURATION] = { "Saturation", 0, 127, 1, 64 },
        [PIN_CTL_HUE]        = { "Hue", -128, 127, 1, 0 },
        [PIN_CTL_SHARPNESS]  = { "Sharpness", 0, 3, 1, 2 },
        [PIN_CTL_AUDIO_GAIN] = { "Audio gain", -345, 120, 15, 0 },
    };
    if ((unsigned)c >= PIN_CTL_COUNT) return PIN_ERR_ARG;
    strncpy(out->label, tbl[c].label, sizeof(out->label) - 1);
    out->min = tbl[c].min; out->max = tbl[c].max; out->step = tbl[c].step; out->def = tbl[c].def;
    pin_session_lock(s);
    switch (c) {
    case PIN_CTL_BRIGHTNESS: out->value = s->want_picture.brightness; break;
    case PIN_CTL_CONTRAST:   out->value = s->want_picture.contrast; break;
    case PIN_CTL_SATURATION: out->value = s->want_picture.saturation; break;
    case PIN_CTL_HUE:        out->value = s->want_picture.hue; break;
    case PIN_CTL_SHARPNESS:  out->value = s->want_picture.sharpness; break;
    case PIN_CTL_AUDIO_GAIN: out->value = s->want_gain_db10; break;
    default: out->value = tbl[c].def; break;
    }
    /* hue only exists for NTSC: the chosen standard, or with Auto the detected one */
    out->enabled = (c != PIN_CTL_HUE) || (s->requested_std == PIN_STD_NTSC ||
                                           s->requested_std == PIN_STD_NTSC_443 ||
                                           s->requested_std == PIN_STD_NTSC_J ||
                                           (s->requested_std == PIN_STD_AUTO && s->is_60hz));
    pin_session_unlock(s);
    return PIN_OK;
}

pin_status_t pin_session_set_control(pin_session_t *s, pin_control_t c, int32_t value)
{
    if (!s) return PIN_ERR_ARG;
    pin_session_lock(s);
    pinnacle_picture_t p = s->want_picture;
    switch (c) {
    case PIN_CTL_BRIGHTNESS: p.brightness = value; break;
    case PIN_CTL_CONTRAST:   p.contrast = value; break;
    case PIN_CTL_SATURATION: p.saturation = value; break;
    case PIN_CTL_HUE:        p.hue = value; break;
    case PIN_CTL_SHARPNESS:  p.sharpness = value; break;
    case PIN_CTL_AUDIO_GAIN:
        s->want_gain_db10 = value;
        s->ctl_dirty_gain = 1;
        pin_session_unlock(s);
        return PIN_OK;
    default: pin_session_unlock(s); return PIN_ERR_ARG;
    }
    s->want_picture = p;
    s->ctl_dirty_picture = 1; /* the worker applies it (analog_tick) */
    pin_session_unlock(s);
    return PIN_OK;
}

pin_status_t pin_session_deck(pin_session_t *s, pin_deck_cmd_t cmd)
{
    if (!s) return PIN_ERR_ARG;
    pin_session_lock(s);
    if (s->state == PIN_STATE_CAPTURING && cmd != PIN_DECK_CMD_STOP) {
        pin_session_unlock(s);
        return PIN_ERR_STATE;
    }
    pin_session_unlock(s);
    if (s->is_replay) {
        pin_session_lock(s);
        switch (cmd) {
        case PIN_DECK_CMD_PLAY: s->deck = PIN_DECK_PLAYING; break;
        case PIN_DECK_CMD_PAUSE: s->deck = PIN_DECK_PAUSED; break;
        case PIN_DECK_CMD_STOP: s->deck = PIN_DECK_STOPPED; break;
        case PIN_DECK_CMD_FF: s->deck = PIN_DECK_FAST_FORWARD; break;
        case PIN_DECK_CMD_REW: s->deck = PIN_DECK_REWINDING; break;
        }
        if (cmd == PIN_DECK_CMD_STOP && s->state == PIN_STATE_CAPTURING) {
            s->cmd.kind = PIN_CMD_CAPTURE_STOP;
            s->cmd.pending = 1;
        }
        pin_session_push_event(s, PIN_EVT_DECK, (int32_t)s->deck, NULL);
        pin_session_unlock(s);
        return PIN_OK;
    }
    pin_cmd_t c = { .kind = PIN_CMD_DECK, .deck_cmd = cmd };
    post_cmd(s, &c);
    return PIN_OK;
}

static int fat32_and_free(const char *path, uint64_t *free_bytes, int *fat32)
{
#if defined(_WIN32)
    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);
    wchar_t root[MAX_PATH];
    wcsncpy(root, wpath, MAX_PATH - 1);
    /* take the drive root: "C:\..." -> "C:\\" */
    if (wcslen(root) >= 2 && root[1] == L':') { root[3] = 0; root[2] = L'\\'; }
    ULARGE_INTEGER avail;
    int ok = GetDiskFreeSpaceExW(root, &avail, NULL, NULL) != 0;
    *free_bytes = ok ? avail.QuadPart : 0;
    wchar_t fsname[64] = { 0 };
    GetVolumeInformationW(root, NULL, 0, NULL, NULL, NULL, fsname, 64);
    *fat32 = wcsstr(fsname, L"FAT32") != NULL || wcscmp(fsname, L"FAT") == 0;
    return ok ? 0 : -1;
#else
    struct statvfs sv;
    if (statvfs(path, &sv) != 0) { *free_bytes = 0; *fat32 = 0; return -1; }
    *free_bytes = (uint64_t)sv.f_bavail * sv.f_frsize;
#if defined(__linux__)
    struct statfs sf;
    *fat32 = (statfs(path, &sf) == 0 && sf.f_type == MSDOS_SUPER_MAGIC);
#else
    *fat32 = 0; /* macOS: f_fstypename "msdos" -- not wired up in this pass */
#endif
    return 0;
#endif
}

/* Nominal data rate for a kind/format, per the coordinator's brief: DV
 * 3.6 MB/s, HDV ~3.3 MB/s, analog uncompressed AVI 720x576x2 @ 25 fps +
 * audio ~ 20.9 MB/s (scales similarly for NTSC at 720x480@29.97), FFV1
 * estimated at ~40% of that. Used both by pin_session_check_output() (a
 * one-off estimate for a hypothetical capture) and by the live
 * disk_free_bytes/est_seconds_left in pin_session_get_status(). */
static double nominal_bytes_per_second(pin_session_t *s, pin_kind_t kind, pin_format_t fmt,
                                       double measured_bps, int *source)
{
    pin_est_kind_t k = kind == PIN_KIND_HDV ? PIN_EST_HDV
                       : kind == PIN_KIND_ANALOG
                             ? (fmt == PIN_FMT_ANALOG_FFV1_MKV ? PIN_EST_ANALOG_FFV1 : PIN_EST_ANALOG_AVI)
                             : PIN_EST_DV;
    return pin_est_rate(k, s->width, s->height, s->is_60hz, learned_ffv1_bph(s), measured_bps, source);
}

pin_status_t pin_session_check_output(pin_session_t *s, const pin_capture_opts_t *o,
                                       pin_output_check_t *out)
{
    if (!s || !o || !out) return PIN_ERR_ARG;
    memset(out, 0, sizeof(*out));
    out->size = sizeof(*out);

    if (!pin_naming_validate(o->path, out->message, sizeof(out->message)))
        return PIN_ERR_ARG;

    pin_kind_t kind = s->stream_kind;
    pin_format_t fmt = kind == PIN_KIND_HDV ? o->format_hdv
                       : kind == PIN_KIND_DV ? o->format_dv : o->format_analog;
    char ext[16];
    format_ext(fmt, ext, sizeof(ext));

    char base[PIN_PATH_MAX];
    pin_naming_strip_extension(o->path, ext, base, sizeof(base));
    pin_naming_opts_t nopts = { .scene_split = 0, .always_number = o->first_number != 0,
                                .scene_index = o->first_number ? o->first_number : 1, .pass = 1 };
    pin_naming_build(base, &nopts, ext, out->first_path, sizeof(out->first_path));

    char first_match[PIN_PATH_MAX];
    /* numbered naming: the caller already picked an unused number */
    int coll = o->first_number ? 0 : pin_naming_collides(base, ext, first_match, sizeof(first_match));
    out->collision = coll > 0;

    /* directory to check free space in: dirname(first_path) */
    char dir[PIN_PATH_MAX];
    strncpy(dir, out->first_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    char *slash = strrchr(dir, '/');
    char *slash2 = strrchr(dir, '\\');
    if (slash2 && (!slash || slash2 > slash)) slash = slash2;
    if (slash) *slash = 0; else strcpy(dir, ".");

    uint64_t free_bytes = 0;
    fat32_and_free(dir, &free_bytes, &out->fat32);
    out->free_bytes = free_bytes;
    out->low_space = pin_output_space_low(free_bytes);

    double bytes_per_s = nominal_bytes_per_second(s, kind, fmt, 0, NULL);
    out->minutes_left = bytes_per_s > 0 ? (uint64_t)(free_bytes / bytes_per_s / 60.0) : 0;

    out->message[0] = 0;
    if (out->collision)
        snprintf(out->message, sizeof(out->message), "%.200s already exists", first_match);
    else if (out->fat32 && out->minutes_left > 0)
        snprintf(out->message, sizeof(out->message),
                 "FAT32: files over 4 GiB will fail (raw DV/HDV and AVI in particular)");
    return PIN_OK;
}

pin_status_t pin_session_set_output_hint(pin_session_t *s, const pin_capture_opts_t *o)
{
    if (!s || !o) return PIN_ERR_ARG;
    pin_session_lock(s);
    s->output_hint = *o;
    s->have_output_hint = 1;
    pin_session_unlock(s);
    return PIN_OK;
}

pin_status_t pin_session_capture_start(pin_session_t *s, const pin_capture_opts_t *o, int overwrite)
{
    if (!s || !o) return PIN_ERR_ARG;
    pin_session_lock(s);
    if (s->state != PIN_STATE_READY) { pin_session_unlock(s); return PIN_ERR_STATE; }
    pin_session_unlock(s);

    pin_output_check_t chk;
    pin_status_t cs = pin_session_check_output(s, o, &chk);
    if (cs == PIN_ERR_ARG)
        return cs; /* unusable file name */
    if (!overwrite && cs == PIN_OK && chk.collision)
        return PIN_ERR_EXISTS;
    pin_cmd_t c = { .kind = PIN_CMD_CAPTURE_START, .capture = *o, .overwrite = overwrite };
    /* A real tape needs a no-signal or time limit to notice the end of a pass;
     * a replay's end of file is the end of the pass. */
    if (!s->is_replay)
        pin_capture_opts_normalize(&c.capture);
    post_cmd(s, &c);
    return PIN_OK;
}

pin_status_t pin_session_capture_stop(pin_session_t *s, pin_stop_deck_t stop_deck)
{
    if (!s) return PIN_ERR_ARG;
    pin_cmd_t c = { .kind = PIN_CMD_CAPTURE_STOP, .stop_deck = stop_deck };
    post_cmd(s, &c);
    return PIN_OK;
}

pin_status_t pin_session_get_status(pin_session_t *s, pin_status_snapshot_t *out)
{
    if (!s || !out) return PIN_ERR_ARG;
    memset(out, 0, sizeof(*out));
    out->size = sizeof(*out);
    pin_session_lock(s);
    out->state = s->state;
    out->last_error = s->last_error;
    strncpy(out->error_text, s->error_text, sizeof(out->error_text) - 1);
    out->input = s->input;
    out->stream_kind = s->stream_kind;
    out->signal = s->signal;
    out->is_60hz = s->is_60hz;
    out->width = s->width; out->height = s->height;
    out->dar_num = s->dar_num ? s->dar_num : 4;
    out->dar_den = s->dar_den ? s->dar_den : 3;
    out->detected_std = s->detected_std;
    out->deck = s->deck;
    out->deck_busy = s->deck_busy;
    strncpy(out->timecode, s->timecode, sizeof(out->timecode) - 1);
    strncpy(out->rec_datetime, s->rec_datetime, sizeof(out->rec_datetime) - 1);
    out->tape_percent = s->tape_percent;
    out->pass = s->pass; out->passes = s->passes; out->scene = (int)s->scene_index;
    strncpy(out->current_file, s->current_file, sizeof(out->current_file) - 1);
    out->elapsed_s = s->elapsed_s;
    out->frames = s->frames; out->frames_dropped = s->frames_dropped;
    out->frames_damaged = s->frames_damaged; out->lost_blocks = s->lost_blocks;
    out->ts_errors = s->ts_errors;
    out->bytes_written = s->bytes_written;
    out->writer_backlog = s->writer_backlog; out->writer_backlog_max = s->writer_backlog_max;
    out->idle_s = s->idle_s;
    out->write_dropped = s->write_dropped;
    out->clip_bytes_written = s->bytes_written;
    out->total_bytes_written = s->bytes_closed + (s->sink ? s->bytes_written : 0);
    out->frames_error = s->frames_error;
    out->clip_frames = s->clip_frames;
    out->clip_frames_error = s->clip_frames_error;
    out->clip_frames_dropped = s->clip_frames_dropped;
    out->err_video_blocks = s->err_video_blocks;
    out->err_audio_blocks = s->err_audio_blocks;
    out->err_missing_blocks = s->err_missing_blocks;
    out->idle_stop_remaining_s = -1;
    out->duration_remaining_s = -1;
    if (s->state == PIN_STATE_CAPTURING) {
        double now = pin_session_now();
        if (s->capture_opts.idle_stop_minutes > 0) {
            double rem = s->capture_opts.idle_stop_minutes * 60.0 - (now - s->last_data_s);
            out->idle_stop_remaining_s = rem > 0 ? rem : 0;
        }
        if (s->capture_opts.max_duration_minutes > 0 && s->capture_start_s > 0) {
            double rem = s->capture_opts.max_duration_minutes * 60.0 -
                         (now - s->capture_start_s);
            out->duration_remaining_s = rem > 0 ? rem : 0;
        }
    }
    out->camera_present = s->camera_present;
    out->stop_reason = s->stop_reason;
    out->stop_captured_s = s->stop_captured_s;
    strncpy(out->stop_text, s->stop_text, sizeof(out->stop_text) - 1);
    if (s->stream_kind == PIN_KIND_HDV) {
        out->video_fps_num = s->hdv_fps_num;
        out->video_fps_den = s->hdv_fps_den;
        out->video_interlaced = pin_vidfmt_hdv_interlaced(s->height);
    } else if (s->height > 0) {
        out->video_fps_num = s->is_60hz ? 30000 : 25;
        out->video_fps_den = s->is_60hz ? 1001 : 1;
        out->video_interlaced = 1;
    }
    if (s->signal)
        pin_vidfmt_label(s->height, out->video_fps_num, out->video_fps_den,
                         out->video_interlaced, out->video_label, sizeof(out->video_label));
    out->progress_percent = -1;
    if (s->state == PIN_STATE_PREPARING) {
        snprintf(out->detail, sizeof(out->detail), "%s", s->step_text[0] ? s->step_text : "Preparing the device");
        double expect = g_prepare_expected_s[s->input == PIN_INPUT_DV ? 0 : 1];
        double frac = s->prepare_start_s > 0 ? (pin_session_now() - s->prepare_start_s) / expect : 0;
        out->progress_percent = frac < 0 ? 0 : frac > 0.99 ? 99 : (int)(frac * 100);
    } else if (s->reconnecting) {
        snprintf(out->detail, sizeof(out->detail), "%s", s->step_text);
    } else if (s->state == PIN_STATE_READY && !s->signal) {
        if (s->input == PIN_INPUT_DV) {
            snprintf(out->detail, sizeof(out->detail), "%s",
                     s->camera_present == 0
                         ? "No camera found. Connect a DV or HDV camera to the FireWire port and switch it on."
                         : "No camera or deck signal. Put the camera in camera mode, or press Play on the deck.");
        } else {
            snprintf(out->detail, sizeof(out->detail),
                     "No video signal on the %s input. Check the cable and that the source is running.",
                     s->input == PIN_INPUT_SVIDEO ? "S-Video" : "composite");
        }
    }
    if (s->audio_meter_t > 0 && meter_clock() - s->audio_meter_t < 0.5) {
        if (s->audio_peak_acc_n) {
            /* loudest sample since the previous read, not just the latest
             * frame's: a fast poller never misses a transient */
            for (int ch = 0; ch < 2; ch++) {
                float pk = s->audio_peak_acc[ch];
                out->audio_peak_db[ch] = pk > 0 ? (float)(20.0 * log10(pk)) : -144.0f;
            }
            s->audio_peak_acc_n = 0;
        } else {
            memcpy(out->audio_peak_db, s->audio_peak_db, sizeof(out->audio_peak_db));
        }
        memcpy(out->audio_rms_db, s->audio_rms_db, sizeof(out->audio_rms_db));
    } else {
        /* no audio yet, or none for half a second (signal lost, stopped):
         * silence, never a stale or zero-initialised (0 dBFS) level */
        for (int ch = 0; ch < 2; ch++)
            out->audio_peak_db[ch] = out->audio_rms_db[ch] = -144.0f;
    }

    /* disk_free_bytes / est_seconds_left / disk_low: from the live capture
     * path while CAPTURING, else from pin_set_output_hint()'s path/format
     * while READY, else left zeroed. */
    const char *hint_path = NULL;
    pin_kind_t hint_kind = s->stream_kind;
    pin_format_t hint_fmt = PIN_FMT_DV_RAW;
    if (s->state == PIN_STATE_CAPTURING && s->current_file[0]) {
        hint_path = s->current_file;
        hint_fmt = s->active_format;
    } else if (s->have_output_hint && s->output_hint.path[0]) {
        hint_path = s->output_hint.path;
        hint_fmt = hint_kind == PIN_KIND_HDV ? s->output_hint.format_hdv
                  : hint_kind == PIN_KIND_ANALOG ? s->output_hint.format_analog
                  : s->output_hint.format_dv;
    }
    if (hint_path) {
        char dir[PIN_PATH_MAX];
        dir_of(hint_path, dir, sizeof(dir));
        uint64_t free_bytes = 0;
        int fat32_unused = 0;
        fat32_and_free(dir, &free_bytes, &fat32_unused);
        out->disk_free_bytes = free_bytes;
        int src = 0;
        double measured = s->state == PIN_STATE_CAPTURING ? pin_rate_bytes_per_s(&s->rate_win, 10.0) : -1;
        double rate = nominal_bytes_per_second(s, hint_kind, hint_fmt, measured, &src);
        out->est_bytes_per_hour = rate * 3600.0;
        out->est_rate_source = src;
        out->est_seconds_left = pin_est_seconds_left(free_bytes, rate);
        out->disk_low = out->est_seconds_left < 3600.0 || free_bytes < (50ull << 30);
    }
    pin_session_unlock(s);
    return PIN_OK;
}

void pin_session_set_aspect(pin_session_t *s, pin_aspect_t aspect)
{
    if (!s) return;
    pin_session_lock(s);
    s->aspect_override = aspect;
    pin_session_unlock(s);
    pin_previewer_set_aspect_override(s->preview, aspect);
}

void pin_session_monitor_enable(pin_session_t *s, int enabled)
{
    if (!s) return;
    pin_session_lock(s);
    s->mon_enabled = enabled != 0;
    pin_session_unlock(s);
}

/* Latency bound (pin_api.h's pin_monitor_read()): a GUI audio output can be
 * a dumb pump because the core itself never lets the ring's backlog grow
 * past ~200 ms -- once it has, the oldest frames are dropped down to
 * ~80 ms before the copy below, rather than handing back 200 ms of stale
 * audio. Applies to every source (analog/DV/HDV all push through the same
 * ring, see pin_session_push_monitor_audio()). */
#define PIN_MON_MAX_MS 200
#define PIN_MON_TARGET_MS 80

int pin_session_monitor_read(pin_session_t *s, int16_t *out, int max_frames)
{
    if (!s || !out || max_frames <= 0) return 0;
    pthread_mutex_lock(&s->mon_mtx);
    size_t max_buffered = (size_t)48000 * PIN_MON_MAX_MS / 1000;
    size_t target_buffered = (size_t)48000 * PIN_MON_TARGET_MS / 1000;
    if (s->mon_fill > max_buffered)
        s->mon_fill = target_buffered;
    int n = (int)(s->mon_fill < (size_t)max_frames ? s->mon_fill : (size_t)max_frames);
    size_t start = (s->mon_head + s->mon_cap_frames - s->mon_fill) % s->mon_cap_frames;
    for (int i = 0; i < n; i++) {
        size_t pos = (start + i) % s->mon_cap_frames;
        out[i * 2] = s->mon_buf[pos * 2];
        out[i * 2 + 1] = s->mon_buf[pos * 2 + 1];
    }
    s->mon_fill -= (size_t)n;
    pthread_mutex_unlock(&s->mon_mtx);
    return n;
}

int pin_session_monitor_available(pin_session_t *s)
{
    if (!s) return 0;
    pthread_mutex_lock(&s->mon_mtx);
    int n = (int)s->mon_fill;
    pthread_mutex_unlock(&s->mon_mtx);
    return n;
}
