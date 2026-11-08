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
#include "pin_format_opts.h"
#include "pin_stop.h"
#include "pin_vidfmt.h"
#include "pin_naming.h"
#include "pin_settings.h"
#include "pin_exe_dir.h"
#include "../core/pinnacle_enum.h"
#include "../core/pinnacle_cfg.h"
#include "../core/pin_log.h"
#include "../sinks/sinks_internal.h"
#include "../sinks/pin_stdout.h"
#include "../sinks/pin_hwenc.h"

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
#elif defined(__APPLE__)
#include <mach-o/dyld.h> /* _NSGetExecutablePath */
#include <sys/mount.h>   /* statfs, f_fstypename */
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
    /* a caller built against an older, shorter pin_event_t gets those fields only (0: no size set) */
    size_t n = out->size;
    if (n == 0 || n > sizeof(*out))
        n = sizeof(*out);
    if (n < sizeof(uint32_t))
        return 0;
    pthread_mutex_lock(mtx);
    if (*count == 0) {
        pthread_mutex_unlock(mtx);
        return 0;
    }
    memcpy(out, &q[*head], n);
    out->size = (uint32_t)n;
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

/* The state the API reports. A requested capture that has not begun yet (after
 * the rewind: PLAY sent, waiting for the first frame / the stream kind) is
 * internally READY, but it is a running capture to the caller: it must not
 * look idle, or a UI would offer "start" for three seconds and a script would
 * think the capture was over. */
static pin_state_t visible_state(const pin_session_t *s)
{
    return s->state == PIN_STATE_READY && s->capture_want_start ? PIN_STATE_CAPTURING : s->state;
}

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
    if (st != s->state) {
        static const char *const names[] = { "closed", "preparing", "ready", "capturing",
                                              "stopping", "rewinding", "error" };
        pin_logf(PIN_LOG_DEBUG, "session: state %s\n",
                 (unsigned)st < sizeof(names) / sizeof(names[0]) ? names[st] : "?");
    }
    s->state = st;
    if (s->lock) {
        pinnacle_lock_state_t ls = PINNACLE_LOCK_READY;
        if (st == PIN_STATE_PREPARING || st == PIN_STATE_REWINDING)
            ls = PINNACLE_LOCK_PREPARING;
        else if (st == PIN_STATE_CAPTURING || st == PIN_STATE_STOPPING)
            ls = PINNACLE_LOCK_CAPTURING;
        pinnacle_lock_update(s->lock, ls, s->dev.guid_hi, s->dev.guid_lo);
    }
    pin_session_push_event(s, PIN_EVT_STATE, (int32_t)visible_state(s), NULL);
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

pin_status_t pin_session_set_app_dirs(const char *config_dir, const char *runtime_dir,
                                      const char *temp_dir)
{
    /* Each module keeps its own directory (settings, lock files, sinks). All or nothing:
     * 511 bytes fit every one of the three. */
    const char *dirs[3] = { config_dir, runtime_dir, temp_dir };
    for (int i = 0; i < 3; i++)
        if (dirs[i] && strlen(dirs[i]) > 511)
            return PIN_ERR_ARG;
    pin_settings_set_dir(config_dir);
    pinnacle_lock_set_dir(runtime_dir);
    pin_sink_set_temp_dir(temp_dir);
    return PIN_OK;
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
    if (pin_exe_dir(base, sizeof(base)) == 0) {
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
    else if (pst == PINNACLE_ERR_FD_REENUMERATE)
        snprintf(msg, sizeof(msg), "%s: %s", s->dev.model ? s->dev.model->name : "This model",
                 pinnacle_strerror(pst));
    else
        snprintf(msg, sizeof(msg), "Device initialisation failed: %s", pinnacle_strerror(pst));
    /* FD_REENUMERATE: not a USB fault; the model cannot be driven from "fd:N" (yet) */
    set_error(s, pst == PINNACLE_ERR_BITSTREAM_READ || pst == PINNACLE_ERR_NOT_READY ? PIN_ERR_FIRMWARE
                 : pst == PINNACLE_ERR_FD_REENUMERATE ? PIN_ERR_ARG : PIN_ERR_USB, msg);
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
    if (pinnacle_parse_fd_id(want, NULL) > 0) {
        /* "fd:N": a descriptor the caller opened (pin_session_open() checked it); never a
         * file name, an enumerated id or a serial */
        snprintf(out, out_size, "%s", want);
        return 0;
    }
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
 * scene-split hold-back FIFO (pin_holdback.h) and split lookahead (pin_split.h)
 * ==================================================================== */

static double unit_seconds(const pin_session_t *s);

static void writer_commit(void *user, const uint8_t *data, size_t len)
{
    pin_session_t *s = user;
    if (!s->writer)
        return;
    if (!pin_writer_push(s->writer, PIN_UNIT_RAW, s->unit_index++, data, len)) {
        s->write_dropped++;
        s->clip_write_dropped++;
        s->captured_s -= unit_seconds(s);   /* counted on arrival, but never reached the file */
    }
}

static void open_sink_for_scene(pin_session_t *s);
static void deck_send(pin_session_t *s, uint16_t node, pin_deck_cmd_t cmd);
static void capture_report_end(pin_session_t *s, pin_stop_reason_t why, const char *detail);
static void status_fill_locked(pin_session_t *s, pin_status_snapshot_t *out);

/* Saves the FFV1 rate of the running capture (average of the last 10 minutes)
 * into the core settings as core.ffv1_bytes_per_hour, when it has run long
 * enough (30 s) to mean something. */
static void rate_persist(pin_session_t *s)
{
    double bps = pin_rate_bytes_per_s(&s->rate_win, 30.0);
    if (bps <= 0) return;
    char path[PIN_PATH_MAX];
    if (pin_settings_default_path(path, sizeof(path)) != 0) return;
    /* kept as the "fast" level's rate, so a later capture at another level scales it */
    double bph = bps * 3600.0 / pin_est_ffv1_factor(s->capture_opts.ffv1_compression);
    if (pin_est_store_ffv1(path, bph) == 0)
        s->ffv1_learned_bph = bph;
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

/* Folds what the closed files had beyond their hand-over counts into the
 * capture's totals. Caller holds the lock. */
static void closer_collect(pin_session_t *s)
{
    int64_t bytes, units;
    pin_closer_frames_t fr;
    pin_closer_take(s->closer, &bytes, &units, &fr);
    s->bytes_closed = (uint64_t)((int64_t)s->bytes_closed + bytes);
    s->units_total = (uint64_t)((int64_t)s->units_total + units);
    s->sinkf_error = (uint64_t)((int64_t)s->sinkf_error + fr.error);
    s->sinkf_repeated = (uint64_t)((int64_t)s->sinkf_repeated + fr.repeated);
    pin_status_t fst = PIN_OK;
    char fpath[PIN_PATH_MAX];
    fpath[0] = 0;
    unsigned nfail = pin_closer_take_failure(s->closer, &fst, fpath, sizeof(fpath));
    if (nfail) {
        if (s->close_failed == 0) {
            s->close_fail_status = fst;
            snprintf(s->close_fail_path, sizeof(s->close_fail_path), "%s", fpath);
        }
        s->close_failed += nfail;
    }
}

/* Every file handed over is closed and counted. Caller holds the lock (the
 * closer never takes it). */
static void closer_drain(pin_session_t *s)
{
    pin_closer_wait(s->closer);
    closer_collect(s);
}

/* Waits for the closer with the lock released, in PIN_STATE_STOPPING
 * ("Finalizing..."): a long writer queue can take a while, and status calls (the
 * GUI) must not wait that long. Only where nothing is half done under the
 * lock (capture_end(), from a tick, a command or a loop exit); a command
 * posted meanwhile stays pending for the next tick. The caller sets the next
 * state. */
static void closer_drain_unlocked(pin_session_t *s)
{
    if (!pin_closer_busy(s->closer))
        return;
    set_state(s, PIN_STATE_STOPPING);
    pin_session_unlock(s);
    pin_closer_wait(s->closer);
    pin_session_lock(s);
}

/* The closer thread, after each file: it is announced once it has a unit
 * (a file shorter than one status tick was not yet), and reported closed
 * unless it was never created. */
static void file_closed(void *user, const pin_closer_done_t *d)
{
    pin_session_t *s = user;
    if (d->units > 0 && !d->announced)
        pin_session_push_event(s, PIN_EVT_FILE_OPENED, 0, d->path);
    if (d->units > 0 || d->status != PIN_OK)
        pin_session_push_event(s, PIN_EVT_FILE_CLOSED, (int32_t)d->status, d->path);
}

static void adopt_renumbered_file(pin_session_t *s);

/* Hands the open file (writer and sink) to the closer, which drains and
 * finalises it on its own thread, so the capture can go on into the next
 * file at once. What it has written so far goes into the capture's totals
 * now (bytes_written in the status is per file, total_bytes_written sums the
 * closed files plus the open one); the rest when it is closed. Caller holds
 * the lock (or the worker has ended). */
static void hand_over_file(pin_session_t *s)
{
    if (!s->sink)
        return;
    adopt_renumbered_file(s);
    pin_sink_status_t sst;
    memset(&sst, 0, sizeof(sst));
    if (s->sink->get_status)
        s->sink->get_status(s->sink, &sst);
    else
        sst.bytes_written = s->bytes_written;
    s->bytes_closed += sst.bytes_written;
    s->units_total += sst.units_written;
    /* The file now counts in bytes_closed; until the next tick the new file would be added
     * on top of the old one's bytes_written in the total. */
    s->bytes_written = 0;
    pin_writer_stats_t wst;
    memset(&wst, 0, sizeof(wst));
    if (s->writer)
        pin_writer_get_stats(s->writer, &wst);
    /* A queue that overflowed ends its file where it overflowed, which is the normal DISK_SLOW
     * end. A cut (scene, size, format) that hands the file over before capture_guard() saw the
     * overflow would go on in a new file with the refused units missing between the two: the
     * capture ends instead. A pipe cannot cut; its overflow is PIPE_SLOW. */
    if (wst.overflowed && !wst.failed && !pin_path_is_stdout(s->capture_opts.path))
        s->disk_slow_pending = 1;
    uint64_t reserve = wst.backlog_bytes;
    /* a file that counts its own frames (HDV MOV / MKV): what it has counted so far is the
     * session's from now on; the closer adds what the close still brings */
    s->sinkf_error += sst.frames_error;
    s->sinkf_repeated += sst.frames_repeated;
    pin_closer_frames_t fr = { (int64_t)sst.frames_out, (int64_t)sst.frames_error,
                               (int64_t)sst.frames_repeated };
    pin_closer_submit(s->closer, s->writer, s->sink, s->current_file, s->file_announced,
                      sst.bytes_written, sst.units_written, reserve, &fr);
    s->writer = NULL;
    s->sink = NULL;
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
    hand_over_file(s);
    s->scene_index++;
    open_sink_for_scene(s);
    pin_session_push_event(s, PIN_EVT_SCENE, (int32_t)s->scene_index, NULL);
}

static void split_ensure(pin_session_t *s)
{
    if (!s->split.commit)
        pin_split_init(&s->split, writer_commit, split_do, s);
}

/* HDV: the frame rate the stream's sequence header (and extension) gives,
 * 29.97 until one has been seen. Caller holds the lock. */
static void hdv_frame_rate(const pin_session_t *s, int *num, int *den)
{
    if (s->hdv_fps_num > 0 && s->hdv_fps_den > 0) {
        *num = s->hdv_fps_num;
        *den = s->hdv_fps_den;
    } else {
        *num = 30000;
        *den = 1001;
    }
}

/* HDV: interlaced or not, from the sequence extension (by the height until
 * one has been seen). Caller holds the lock. */
static int hdv_interlaced(const pin_session_t *s)
{
    return pin_vidfmt_hdv_interlaced(s->hdv_seq_h ? s->hdv_seq_h : 1080,
                                     s->hdv_seqext_seen ? s->hdv_progressive : -1);
}

/* Seconds one new frame_index adds to a pending segment: a DV frame, or an
 * HDV picture (frame_index counts pictures) at the stream's own rate. */
static double unit_seconds(const pin_session_t *s)
{
    if (s->stream_kind == PIN_KIND_HDV) {
        int num, den;
        hdv_frame_rate(s, &num, &den);
        return (double)den / num;
    }
    return s->is_60hz ? 1001.0 / 30000.0 : 0.04;
}

/* The kind of a DV / HDV unit by its bytes: an HDV unit is whole TS packets that start with
 * the sync byte; a DV frame (120,000 / 144,000 bytes, a header block 0x1F first) never is. */
static pin_kind_t unit_kind(const uint8_t *data, size_t len)
{
    return len && len % HDV_TS_PACKET_SIZE == 0 && data[0] == HDV_TS_SYNC_BYTE ? PIN_KIND_HDV
                                                                                 : PIN_KIND_DV;
}

/* HDV: the PID of the unit's first TS packet if that starts a video PES (a picture, as the
 * reassembler and the TS replay cut them), else -1 (PAT/PMT and what precedes a stream's
 * first picture). */
static int hdv_picture_pid(const uint8_t *d, size_t len)
{
    if (len < HDV_TS_PACKET_SIZE || d[0] != HDV_TS_SYNC_BYTE || !(d[1] & 0x40) || !(d[3] & 0x10))
        return -1;
    size_t po = (d[3] & 0x20) ? 5u + d[4] : 4u;
    if (po + 4 > HDV_TS_PACKET_SIZE || d[po] || d[po + 1] || d[po + 2] != 1 || (d[po + 3] & 0xF0) != 0xE0)
        return -1;
    return ((d[1] & 0x1F) << 8) | d[2];
}

static void commit_unit(pin_session_t *s, const uint8_t *data, size_t len);

static void commit_push(pin_session_t *s, const uint8_t *data, size_t len, long frame_index);

/* A raw HDV transport stream (.m2t / .ts, or HDV on standard output) with a title: it gets the
 * title as its own DVB SDT (hdv_emit_psi()), and the SDT packets the stream may send itself are
 * left out (commit_unit_fi()), so the title is the file's only service name. Without a title the
 * stream is written as it came. HDV MOV / MKV store the title themselves. */
static int hdv_ts_titled(const pin_session_t *s)
{
    return s->file_kind == PIN_KIND_HDV && s->active_format == PIN_FMT_HDV_TS &&
           s->capture_opts.title[0];
}

/* HDV: the latest PAT and PMT, so what follows decodes from its first packet. A titled raw
 * transport stream (hdv_ts_titled()) then gets its title as a DVB SDT packet (hdv_sdt_packet()),
 * the TS's own place for one: ffprobe shows it as the program's service_name, MediaInfo as
 * "Service name". Behind the PAT, where MediaInfo looks for it; once per file is enough for both. */
static void hdv_emit_psi(pin_session_t *s)
{
    if (s->hdv_psi.pat_valid) commit_unit(s, s->hdv_psi.pat, HDV_TS_PACKET_SIZE);
    if (s->hdv_psi.pmt_valid) commit_unit(s, s->hdv_psi.pmt, HDV_TS_PACKET_SIZE);
    uint8_t sdt[HDV_TS_PACKET_SIZE];
    if (s->sink && hdv_ts_titled(s) &&
        hdv_sdt_packet(&s->hdv_psi, "MarvinCapture", s->capture_opts.title, s->hdv_sdt_cc, sdt)) {
        s->hdv_sdt_cc = (s->hdv_sdt_cc + 1) & 0x0F;
        commit_push(s, sdt, sizeof(sdt), -1); /* not commit_unit(), which leaves SDTs out */
    }
}

/* HDV file gate: a file opens at an arbitrary picture (a capture start, a pass, a format
 * change), but a transport stream must start on a GOP (sequence header) or decoders reject
 * the first pictures. Returns 1 if the unit may be written; when it opens, it first writes
 * the latest PAT/PMT, so the file decodes from its first packet. Live and replay alike. */
static int hdv_write_gate(pin_session_t *s, const uint8_t *data, size_t len)
{
    if (!s->hdv_await_gop)
        return 1;
    static const uint8_t seq_hdr[4] = { 0x00, 0x00, 0x01, 0xB3 };
    int found = 0;
    for (size_t i = 0; i + 4 <= len; i++)
        if (!memcmp(data + i, seq_hdr, 4)) { found = 1; break; }
    if (!found)
        return 0;
    s->hdv_await_gop = 0;
    hdv_emit_psi(s);
    return 1;
}

/* Every unit that reaches a file goes through here (so a pending content
 * split can hold it). frame_index < 0: carries no time (PAT/PMT, raw). Units of
 * another kind than the open file's (DV <-> HDV) are dropped: a stray one, or
 * one of a change that is cut elsewhere (a new file, or the end of a pipe). */
static void commit_unit_fi(pin_session_t *s, const uint8_t *data, size_t len, long frame_index)
{
    if (!s->sink || unit_kind(data, len) != s->file_kind)
        return;
    if (s->file_kind == PIN_KIND_HDV && !hdv_write_gate(s, data, len))
        return;
    size_t n = len / HDV_TS_PACKET_SIZE;
    if (hdv_ts_titled(s) && hdv_ts_count_pid(data, n, HDV_SDT_PID)) {
        /* the stream's own SDT: the title's replaces it (hdv_emit_psi()); out of memory, the
         * unit goes as it came */
        if (s->hdv_unit_cap < len) {
            uint8_t *b = realloc(s->hdv_unit_buf, len);
            if (b) {
                s->hdv_unit_buf = b;
                s->hdv_unit_cap = len;
            }
        }
        if (s->hdv_unit_cap >= len) {
            len = hdv_ts_drop_pid(data, n, HDV_SDT_PID, s->hdv_unit_buf) * HDV_TS_PACKET_SIZE;
            data = s->hdv_unit_buf;
            if (!len)
                return;
        }
    }
    commit_push(s, data, len, frame_index);
}

/* commit_unit_fi() past its checks: to the split lookahead (pin_split_push() writes or copies
 * it before it returns), then the file. */
static void commit_push(pin_session_t *s, const uint8_t *data, size_t len, long frame_index)
{
    split_ensure(s);
    pin_split_push(&s->split, data, len, frame_index, unit_seconds(s));
}

static void commit_unit(pin_session_t *s, const uint8_t *data, size_t len)
{
    commit_unit_fi(s, data, len, -1);
}

static void hold_commit(void *user, const uint8_t *data, size_t len, long frame_index)
{
    commit_unit_fi(user, data, len, frame_index);
}

/* Holds one unit back (pin_holdback.h); what the caller releases goes to
 * commit_unit_fi(). */
static void hold_push(pin_session_t *s, const uint8_t *data, size_t len, long frame_index,
                      unsigned flags)
{
    if (!s->hold.commit)
        pin_holdback_init(&s->hold, hold_commit, s);
    pin_holdback_push(&s->hold, data, len, frame_index, flags);
}

/* The new segment of a scene cut begins here (pin_holdback_cut()): the units
 * before it went to the current file. HDV: a new file starts with the PAT and
 * PMT, then the cut picture (which opens a GOP). */
static void scene_cut_begin(void *user, long index)
{
    pin_session_t *s = user;
    (void)index;
    if (pin_split_begin(&s->split) && s->file_kind == PIN_KIND_HDV)
        hdv_emit_psi(s);
}

/* A scene cut was confirmed at cut_frame_index. Units before it belong to
 * the old scene; the cut itself only becomes a new file once the new segment
 * is real (pin_split.h): until then its units are held, and a second cut or
 * the end of the capture merges them back into the current file (the PAT/PMT
 * in front of the cut picture then just sit in the middle of it, as a stream's
 * periodic PAT/PMT do). */
static void scene_cut(pin_session_t *s, long cut_frame_index)
{
    split_ensure(s);
    /* a still-pending earlier split never made it: its units stay in the old file */
    pin_split_cancel(&s->split);
    pin_holdback_cut(&s->hold, cut_frame_index, scene_cut_begin, s);
}

static void file_format_for_stream(pin_session_t *s);

/* The aspect the user chose for files of this kind: aspect_hdv for HDV, aspect for DV and
 * analog. Every file open (start, scene split, format change, FAT32 cut, pass) takes its own
 * kind's, whichever kind the capture started on. */
static pin_aspect_t file_aspect(const pin_capture_opts_t *o, pin_kind_t kind)
{
    return kind == PIN_KIND_HDV ? o->aspect_hdv : o->aspect;
}

/* A format change's new file begins here (pin_holdback_cut()): the units before it went
 * to the old one. It is a hard cut, never merged back like a scene split's: the old file
 * is finished and the next one opened with the new format (kind, format and extension,
 * geometry, aspect, audio), numbered like a scene: "name-0002" after "name" without scene
 * split. A file that got nothing yet (a pass that began on the old format's last
 * frames) is replaced under its own name. HDV: the file starts on a GOP with PAT/PMT
 * (hdv_write_gate()). Caller holds the lock. */
static int scene_split_on(const pin_session_t *s);

static void format_cut_begin(void *user, long index)
{
    pin_session_t *s = user;
    (void)index;
    int empty = s->split.file_units == 0;
    hand_over_file(s);
    if (!empty) {
        s->scene_index++;
        if (!scene_split_on(s))
            s->fmt_numbered = 1;
    }
    file_format_for_stream(s);
    s->scene_det_ready = 0; /* the scene detector starts over at the new frame rate */
    open_sink_for_scene(s);
}

/* A change of the stream's format was confirmed (pin_fmtwatch.h) during a capture, its
 * first unit at index start (held, with what followed it). What it does to the open file
 * is pin_format_change_action()'s. Caller holds the lock. */
static void capture_format_change(pin_session_t *s, const pin_stream_fmt_t *old, long start)
{
    const pin_stream_fmt_t *nf = &s->fmtw.cur;
    int to_stdout = pin_path_is_stdout(s->capture_opts.path);
    pin_fmtchg_t act = pin_format_change_action(s->active_format, to_stdout,
                                                file_aspect(&s->capture_opts, s->file_kind) != PIN_ASPECT_AUTO,
                                                old, nf);
    if (act == PIN_FMTCHG_KEEP)
        return; /* the open file carries it (a scene cut waiting for this goes ahead) */
    s->scene_cut_deferred = 0; /* this is the cut */
    if (act == PIN_FMTCHG_STOP) {
        /* a pipe carries one kind: what came before the change still goes out, the new
         * kind never does; capture_guard() ends the capture */
        pin_holdback_flush(&s->hold);
        if (!s->fmt_stop) {
            s->fmt_stop = 1;
            pin_format_change_stop_text(old->kind, nf->kind, s->fmt_stop_text, sizeof(s->fmt_stop_text));
        }
        return;
    }
    /* a pending scene split is settled first, by its own rule: still too short to be a file
     * of its own, so its units stay in the current one */
    split_ensure(s);
    pin_split_cancel(&s->split);
    pin_holdback_cut(&s->hold, start, format_cut_begin, s);
}

/* ========================================================================
 * writer consume callback -> sink
 * ==================================================================== */

/* Test seam PIN_TEST_SINK_DELAY_MS (test_seams_read()): a slow drive. */
static int g_test_sink_delay_ms;

static int writer_consume(pin_unit_kind_t kind, uint64_t index, const uint8_t *data, size_t len,
                           void *user)
{
    /* its own sink, not s->sink: a file handed to the closer still drains
     * while the next one is open */
    pin_sink_t *sink = user;
    (void)index;
    if (!sink)
        return 0;
    if (g_test_sink_delay_ms > 0)
        sleep_ms(g_test_sink_delay_ms);
    pin_status_t st;
    if (kind == PIN_UNIT_VIDEO)
        st = sink->write_video(sink, data, len);
    else if (kind == PIN_UNIT_AUDIO)
        st = sink->write_audio(sink, (const int16_t *)data, len / 4);
    else
        st = sink->write_unit(sink, data, len);
    return st == PIN_OK ? 0 : -1;
}

/* ========================================================================
 * output naming / sink open
 * ==================================================================== */

/* ---- format table (pin_api.h's pin_formats()/pin_format_info()) --------- */

/* The analog default: uncompressed AVI everywhere but Android, where 75 GB per hour is more
 * than phone storage keeps up with or holds for long (pin_session_format_warning() says so if
 * the user picks it anyway), so lossless FFV1 is the default there. pin_capture_opts_defaults()
 * (pin_api.c) follows the same rule. */
#if defined(__ANDROID__)
#define ANALOG_DEFAULT_IS_AVI 0
#else
#define ANALOG_DEFAULT_IS_AVI 1
#endif

/* supports_title / supports_aspect: what each sink writes. sink_raw (raw .dv, raw .m2t / .ts, and
 * DV / HDV on standard output) writes the stream as it came: no aspect flag of its own (the DV
 * frames carry the camera's, the MPEG-2 sequence headers the stream's) and no title in a raw .dv
 * (DV has no field for one that players or MediaInfo read). A raw .m2t / .ts has its title in a
 * DVB SDT packet the session puts behind the PAT/PMT at each file start (hdv_emit_psi(),
 * hdv_ts_titled()). sink_avi (OpenDML vprp), sink_ffv1, sink_rewrap (DV AVI / MOV, HDV MOV / MKV)
 * and sink_hwenc store both the title and the aspect (the sample aspect ratio); so does sink_nut
 * (analog on standard output). */
static const pin_format_info_t g_formats[PIN_FMT_COUNT] = {
    [PIN_FMT_ANALOG_AVI] = { .format = PIN_FMT_ANALOG_AVI, .kind = PIN_KIND_ANALOG,
        .label = "Uncompressed AVI (YUY2)", .extension = "avi",
        .supports_title = 1, .supports_scene_split = 0, .supports_multi_pass = 0, .is_default = ANALOG_DEFAULT_IS_AVI,
        .supports_aspect = 1 },
    [PIN_FMT_ANALOG_FFV1_MKV] = { .format = PIN_FMT_ANALOG_FFV1_MKV, .kind = PIN_KIND_ANALOG,
        .label = "FFV1 + FLAC lossless (MKV)", .extension = "mkv",
        .supports_title = 1, .supports_scene_split = 0, .supports_multi_pass = 0, .is_default = !ANALOG_DEFAULT_IS_AVI,
        .supports_aspect = 1 },
    [PIN_FMT_DV_RAW] = { .format = PIN_FMT_DV_RAW, .kind = PIN_KIND_DV,
        .label = "Raw DV stream", .extension = "dv",
        .supports_title = 0, .supports_scene_split = 1, .supports_multi_pass = 1, .is_default = 1,
        .supports_aspect = 0 },
    [PIN_FMT_DV_AVI] = { .format = PIN_FMT_DV_AVI, .kind = PIN_KIND_DV,
        .label = "DV in AVI (type 2)", .extension = "avi",
        .supports_title = 1, .supports_scene_split = 1, .supports_multi_pass = 1, .is_default = 0,
        .supports_aspect = 1 },
    [PIN_FMT_DV_MOV] = { .format = PIN_FMT_DV_MOV, .kind = PIN_KIND_DV,
        .label = "DV in QuickTime", .extension = "mov",
        .supports_title = 1, .supports_scene_split = 1, .supports_multi_pass = 1, .is_default = 0,
        .supports_aspect = 1 },
    [PIN_FMT_HDV_TS] = { .format = PIN_FMT_HDV_TS, .kind = PIN_KIND_HDV,
        .label = "Raw transport stream", .extension = "m2t",
        .supports_title = 1, .supports_scene_split = 1, .supports_multi_pass = 1, .is_default = 1,
        .supports_aspect = 0 },
    [PIN_FMT_HDV_MOV] = { .format = PIN_FMT_HDV_MOV, .kind = PIN_KIND_HDV,
        .label = "HDV in QuickTime", .extension = "mov",
        .supports_title = 1, .supports_scene_split = 1, .supports_multi_pass = 1, .is_default = 0,
        .supports_aspect = 1, .scene_split_forced = 1 },
    [PIN_FMT_HDV_MKV] = { .format = PIN_FMT_HDV_MKV, .kind = PIN_KIND_HDV,
        .label = "HDV in Matroska", .extension = "mkv",
        .supports_title = 1, .supports_scene_split = 1, .supports_multi_pass = 1, .is_default = 0,
        .supports_aspect = 1, .scene_split_forced = 1 },
    [PIN_FMT_ANALOG_H264_MKV] = { .format = PIN_FMT_ANALOG_H264_MKV, .kind = PIN_KIND_ANALOG,
        .label = "H.264 in MKV (hardware encoder)", .extension = "mkv",
        .supports_title = 1, .supports_scene_split = 0, .supports_multi_pass = 0, .is_default = 0,
        .supports_aspect = 1 },
    [PIN_FMT_ANALOG_HEVC_MKV] = { .format = PIN_FMT_ANALOG_HEVC_MKV, .kind = PIN_KIND_ANALOG,
        .label = "HEVC in MKV (hardware encoder)", .extension = "mkv",
        .supports_title = 1, .supports_scene_split = 0, .supports_multi_pass = 0, .is_default = 0,
        .supports_aspect = 1 },
    [PIN_FMT_DV_H264_MKV] = { .format = PIN_FMT_DV_H264_MKV, .kind = PIN_KIND_DV,
        .label = "H.264 in MKV (hardware encoder)", .extension = "mkv",
        .supports_title = 1, .supports_scene_split = 1, .supports_multi_pass = 1, .is_default = 0,
        .supports_aspect = 1 },
    [PIN_FMT_DV_HEVC_MKV] = { .format = PIN_FMT_DV_HEVC_MKV, .kind = PIN_KIND_DV,
        .label = "HEVC in MKV (hardware encoder)", .extension = "mkv",
        .supports_title = 1, .supports_scene_split = 1, .supports_multi_pass = 1, .is_default = 0,
        .supports_aspect = 1 },
};

/* Scene split for the open file: the option, or always for a format that needs it
 * (scene_split_forced: HDV to MOV / MKV, whose files must not run across a jump of the camera
 * clock). Never to standard output (the option is cleared there). */
static int scene_split_on(const pin_session_t *s)
{
    if (s->capture_opts.scene_split)
        return 1;
    if (pin_path_is_stdout(s->capture_opts.path) || (unsigned)s->active_format >= PIN_FMT_COUNT)
        return 0;
    return pin_fmt_always_splits(s->active_format) && g_formats[s->active_format].kind == s->file_kind;
}

int pin_session_format_available(pin_format_t f)
{
    if (!pin_fmt_is_hwenc(f))
        return (unsigned)f < PIN_FMT_COUNT;
    return pin_hwenc_available(pin_fmt_is_hevc(f) ? PIN_HWENC_HEVC : PIN_HWENC_H264);
}

/* Formats this platform does not offer: pin_formats() leaves them out, pin_format_info() and
 * a capture still take them. The H.264 / HEVC formats only where their encoder opened (a
 * capture refuses them elsewhere). HDV to MOV / MKV everywhere, Android too: a phone that
 * cannot keep up with decoding every GOP (or with the repairs) ends the capture with an error
 * (sink_hdvmux.c, reencode_behind), as a computer would. */
static int format_offered(pin_format_t f)
{
    if (pin_fmt_is_hwenc(f))
        return pin_session_format_available(f);
    return 1;
}

void pin_session_format_warning(pin_format_t f, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = 0;
    if (pin_fmt_is_hwenc(f)) {
        pin_fmt_hwenc_warning(f, out, cap);
        return;
    }
#if defined(__ANDROID__)
    /* the rates of pin_estimate.c: YUY2 720x576 at 25 fps + PCM is 20.9 MB/s, 75 GB/h */
    if (f == PIN_FMT_ANALOG_AVI)
        snprintf(out, cap, "Uncompressed video takes about 75 GB per hour (21 MB/s). Phone storage "
                 "may be too slow to keep up with that, or fill up quickly; FFV1 (lossless, about "
                 "30 GB per hour) is the safer choice.");
#else
    (void)f;
#endif
}

/* ---- help texts that name formats (pin_format_title_help(), pin_format_aspect_help()) ---- */

/* How those texts call each format: short, with the kind where the container alone does not say. */
static const char *const g_format_short[PIN_FMT_COUNT] = {
    [PIN_FMT_ANALOG_AVI] = "uncompressed AVI",
    [PIN_FMT_ANALOG_FFV1_MKV] = "FFV1 MKV",
    [PIN_FMT_DV_RAW] = "raw .dv",
    [PIN_FMT_DV_AVI] = "DV AVI",
    [PIN_FMT_DV_MOV] = "DV MOV",
    [PIN_FMT_HDV_TS] = "raw .m2t (HDV)",
    [PIN_FMT_HDV_MOV] = "HDV MOV",
    [PIN_FMT_HDV_MKV] = "HDV MKV",
    [PIN_FMT_ANALOG_H264_MKV] = "H.264 MKV",
    [PIN_FMT_ANALOG_HEVC_MKV] = "HEVC MKV",
    [PIN_FMT_DV_H264_MKV] = "DV H.264 MKV",
    [PIN_FMT_DV_HEVC_MKV] = "DV HEVC MKV",
};

static int format_is_kind(pin_format_t f, pin_kind_t kind)
{
    return (unsigned)f < PIN_FMT_COUNT && g_formats[f].kind == kind;
}

/* format_offered() without running the encoder probe (a GUI asks for these texts on its UI
 * thread): an H.264 / HEVC format only once a probe found its encoder. */
static int format_offered_known(pin_format_t f)
{
    if (pin_fmt_is_hwenc(f))
        return pin_hwenc_known_available(pin_fmt_is_hevc(f) ? PIN_HWENC_HEVC : PIN_HWENC_H264);
    return format_offered(f);
}

/* Appends the short names of fs[0..n-1] as "A", "A and B", "A, B and C"; the first letter in
 * upper case if cap_first. */
static void append_names(char *out, size_t cap, const pin_format_t *fs, int n, int cap_first)
{
    for (int i = 0; i < n; i++) {
        size_t len = strlen(out);
        if (len + 1 >= cap)
            return;
        const char *sep = i == 0 ? "" : i == n - 1 ? " and " : ", ";
        snprintf(out + len, cap - len, "%s%s", sep, g_format_short[fs[i]]);
        if (i == 0 && cap_first)
            out[len] = (char)toupper((unsigned char)out[len]);
    }
}

static void append_text(char *out, size_t cap, const char *text)
{
    size_t len = strlen(out);
    if (len + 1 < cap)
        snprintf(out + len, cap - len, "%s", text);
}

void pin_session_format_title_help(pin_input_t input, pin_format_t format, pin_format_t format_hdv,
                                   char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = 0;
    pin_format_t fs[2];
    int n = 0;
    if (input == PIN_INPUT_DV) {
        snprintf(out, cap, "File name for DV and HDV (scenes and passes get numbered suffixes).");
        if (format_is_kind(format, PIN_KIND_DV))
            fs[n++] = format;
        if (format_is_kind(format_hdv, PIN_KIND_HDV))
            fs[n++] = format_hdv;
    } else {
        snprintf(out, cap, "File name (the extension follows the format).");
        if (format_is_kind(format, PIN_KIND_ANALOG))
            fs[n++] = format;
    }
    pin_format_t with[2], without[2];
    int nw = 0, nwo = 0;
    for (int i = 0; i < n; i++) {
        if (g_formats[fs[i]].supports_title)
            with[nw++] = fs[i];
        else
            without[nwo++] = fs[i];
    }
    if (nw) {
        append_text(out, cap, " Also stored as the title in ");
        append_names(out, cap, with, nw, 0);
        append_text(out, cap, " files");
    }
    if (nwo) {
        append_text(out, cap, nw ? "; " : " ");
        append_names(out, cap, without, nwo, !nw);
        append_text(out, cap, " files have no title");
    }
    if (nw || nwo)
        append_text(out, cap, ".");
}

void pin_session_format_kind_help(pin_kind_t kind, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = 0;
    /* AVI and MOV get their index, sizes and moov only when the file is closed; an unclean end
     * (kill, power loss, pulled drive, full disk) leaves one that many players cannot open. */
    if (kind == PIN_KIND_ANALOG)
        snprintf(out, cap, "AVI needs a clean finish: its index is written when the capture ends, "
                 "so if the capture is cut off (power loss, drive pulled, disk full) many players "
                 "cannot open the file. MKV stays readable.");
    else if (kind == PIN_KIND_DV)
        snprintf(out, cap, "AVI and MOV need a clean finish: the index (AVI) or the movie header "
                 "(MOV) is written when the capture ends, so if the capture is cut off (power loss, "
                 "drive pulled, disk full) many players cannot open the file; a MOV without it "
                 "cannot be opened at all. MKV and the raw .dv stay readable.");
    else if (kind == PIN_KIND_HDV)
        snprintf(out, cap, "MOV needs a clean finish: its movie header is written when the capture "
                 "ends, so if the capture is cut off (power loss, drive pulled, disk full) the file "
                 "cannot be opened. MKV and the raw .m2t stay readable.\n\n"
                 "Note the MOV and MKV for capturing HDV are not truly lossless: undamaged "
                 "frames are passed through as is, and MarvinCapture re-encodes broken frames with "
                 "the damage concealed (by ffmpeg's decoder). This improves compatibility with "
                 "applications such as DaVinci Resolve, preventing clip import failure / rendering "
                 "failure. However, applications will no longer be able to know where the damage / "
                 "concealment happened. If you need truly lossless, use the .m2t raw HDV stream "
                 "instead.");
}

void pin_session_format_scene_split_help(pin_format_t f, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = 0;
    if ((unsigned)f >= PIN_FMT_COUNT)
        return;
    const pin_format_info_t *fi = &g_formats[f];
    pin_format_t one[1] = { f };
    if (fi->scene_split_forced) {
        snprintf(out, cap, "Always on for ");
        append_names(out, cap, one, 1, 0);
        append_text(out, cap, " files: each recording on the tape goes to a file of its own "
                              "(numbered), as their timing starts at 0 and runs without gaps. A new "
                              "file starts where the timecode, the recording date or the camera's "
                              "clock jumps.");
    } else if (fi->supports_scene_split) {
        snprintf(out, cap, "A new numbered file for each recording on the tape: where the timecode%s "
                           "jumps.",
                 fi->kind == PIN_KIND_HDV ? ", the recording date or the camera's clock"
                                          : " or the recording date");
    } else {
        append_names(out, cap, one, 1, 1);
        append_text(out, cap, " files are not split into scenes.");
    }
}

void pin_session_format_aspect_help(pin_format_t f, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = 0;
    if ((unsigned)f >= PIN_FMT_COUNT)
        return;
    const pin_format_info_t *fi = &g_formats[f];
    pin_format_t one[1] = { f };
    if (fi->supports_aspect) {
        if (fi->kind == PIN_KIND_ANALOG)
            snprintf(out, cap, "4:3, or 16:9 for anamorphic widescreen tapes (Auto is 4:3). Affects the "
                     "preview and the ");
        else
            snprintf(out, cap, "Overrides the aspect ratio for the preview and the ");
        append_names(out, cap, one, 1, 0);
        append_text(out, cap, fi->kind == PIN_KIND_ANALOG ? " files."
                    : fi->kind == PIN_KIND_DV ? " files. Auto uses what the tape reports."
                    : " files. Auto uses what the stream reports (16:9 for HDV).");
        return;
    }
    append_names(out, cap, one, 1, 1);
    append_text(out, cap, fi->kind == PIN_KIND_HDV
        ? " files have no aspect flag of their own: players take the aspect from the video itself "
          "(the MPEG-2 stream says 16:9 or 4:3). So this is disabled: Auto applies, and the preview "
          "shows what the stream reports."
        : " files have no aspect flag of their own: players take the aspect from the video itself "
          "(the 4:3 or 16:9 flag the camera recorded with every frame). So this is disabled: Auto "
          "applies, and the preview shows what the tape reports.");
    pin_format_t with[PIN_FMT_COUNT];
    int nw = 0;
    for (int i = 0; i < PIN_FMT_COUNT; i++)
        if (g_formats[i].kind == fi->kind && g_formats[i].supports_aspect &&
            format_offered_known((pin_format_t)i))
            with[nw++] = (pin_format_t)i;
    if (nw) {
        append_text(out, cap, " ");
        append_names(out, cap, with, nw, 0);
        append_text(out, cap, " files store one.");
    } else {
        append_text(out, cap, fi->kind == PIN_KIND_HDV ? " None of the HDV formats here stores one."
                                                       : " None of the formats here stores one.");
    }
}

int pin_session_formats(pin_kind_t kind, pin_format_info_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < PIN_FMT_COUNT; i++) {
        /* the kind first: listing HDV formats needs no encoder probe */
        if (g_formats[i].kind != kind || !format_offered((pin_format_t)i))
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

/* The file name extension of format f for these options: the format's own, except
 * that a raw HDV transport stream keeps the user's .ts when they asked for it (default .m2t). */
static void format_ext(const pin_capture_opts_t *o, pin_format_t f, char *out, size_t out_size)
{
    pin_format_info_t fi;
    if (f == PIN_FMT_HDV_TS && o && o->ts_extension[0])
        snprintf(out, out_size, "%.7s", o->ts_extension);
    else if (pin_session_format_info(f, &fi) == PIN_OK)
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
    s->captured_s = 0;
    s->write_dropped = s->audio_write_dropped = 0;
    s->frames_error = 0;
    s->clip_frames = s->clip_frames_error = s->clip_frames_dropped = s->clip_write_dropped = 0;
    s->sinkf_error = s->sinkf_repeated = 0;
    s->err_video_blocks = s->err_audio_blocks = s->err_missing_blocks = 0;
    s->hdv_err_reset = 1;
}

/* One video unit (DV frame / HDV picture / analog frame) arrived. Caller holds the lock. */
static void count_frame(pin_session_t *s, int err, int dropped)
{
    s->frames++;
    s->clip_frames++;
    /* the length is summed per unit, at the rate it came in: a tape can change from PAL to NTSC,
     * and a unit that arrives with no file open (after a pass ended, before the rewind took
     * effect) is not in any file */
    if (s->sink)
        s->captured_s += unit_seconds(s);
    if (err) { s->frames_error++; s->clip_frames_error++; }
    if (dropped) { s->frames_dropped++; s->clip_frames_dropped++; }
}

/* What one DV frame / HDV picture adds to the error counters; applied once the unit has a file
 * (apply_unit_counts()). */
typedef struct {
    int err, dropped;
    int hdv;                     /* an HDV picture: into HDV MOV / MKV the file counts instead */
    uint64_t video, audio, missing;
} unit_counts_t;

static void apply_unit_counts(pin_session_t *s, const unit_counts_t *uc)
{
    int file_counts = uc->hdv && s->sink && s->file_kind == PIN_KIND_HDV &&
                      (s->active_format == PIN_FMT_HDV_MOV || s->active_format == PIN_FMT_HDV_MKV);
    count_frame(s, file_counts ? 0 : uc->err, uc->dropped);
    s->err_video_blocks += uc->video;
    s->err_audio_blocks += uc->audio;
    s->err_missing_blocks += uc->missing;
}

/* Numbered output: the names after a taken one, for the lazy sink (sink_lazy.c, "Taken names":
 * Android shared storage can hold files of this name that the app cannot see). Owned by the
 * sink, which may use it on the writer thread after the session moved on, hence the copy. */
typedef struct {
    char base[PIN_PATH_MAX];
    char ext[16];
    pin_naming_opts_t nopts;
} renumber_ctx_t;

static int renumber_next(void *ctx, unsigned k, char *out, size_t cap)
{
    const renumber_ctx_t *c = ctx;
    pin_naming_opts_t o = c->nopts;
    o.scene_index += k;
    return pin_naming_build(c->base, &o, c->ext, out, cap) == 0 ? 0 : -1;
}

/* Takes over the name the lazy sink moved the open file to, if it had to skip a taken number:
 * current_file (status, events, the closer) and scene_index (the next scene counts on from
 * it). Caller holds the lock. */
static void adopt_renumbered_file(pin_session_t *s)
{
    char path[PIN_PATH_MAX];
    unsigned k = s->sink ? pin_sink_lazy_renumbered(s->sink, path, sizeof(path)) : 0;
    if (k == 0 || strcmp(path, s->current_file) == 0)
        return;     /* none, or already taken over */
    snprintf(s->current_file, sizeof(s->current_file), "%s", path);
    s->scene_index += k;
}

/* A file could not be opened: for a capture (its first file, a scene split, the next pass)
 * that ends the capture and stops the deck, which has been sent PLAY by then; then the
 * session is in ERROR. s->sink is NULL here. Caller holds the lock. */
static void open_failed(pin_session_t *s, pin_status_t st, const char *text)
{
    if (s->capture_began) {
        /* The earlier files may still be draining to a slow disk: waiting for them with the
         * lock held would freeze every status call meanwhile. */
        closer_drain_unlocked(s);
        capture_report_end(s, PIN_STOP_WRITE_ERROR,
                           s->opening_first_file ? "the output file could not be created"
                                                 : "the next output file could not be created");
        if (s->capture_opts.start_deck && s->dev.camera_node)
            deck_send(s, s->dev.camera_node, PIN_DECK_CMD_STOP);
    }
    set_error(s, st, text);
}

static void open_sink_for_scene(pin_session_t *s)
{
    /* numbered: scene split, a first number, or the files after a format change */
    pin_naming_opts_t nopts = {
        .scene_split = scene_split_on(s),
        .always_number = s->capture_opts.first_number != 0 || s->fmt_numbered,
        .scene_index = s->scene_index,
        .pass = s->pass_index,
    };
    char path[PIN_PATH_MAX];
    const int to_stdout = pin_path_is_stdout(s->capture_opts.path);
    if (to_stdout) {
        snprintf(path, sizeof(path), "-");
    } else if (pin_naming_build(s->naming_base, &nopts, s->naming_ext, path, sizeof(path)) != 0) {
        open_failed(s, PIN_ERR_ARG, "output path too long");
        return;
    }

    /* a file is created by its first video unit (pin_sink_lazy): no video, no file */
    s->sink = to_stdout ? pin_sink_create_stdout(s->file_kind)
                        : pin_sink_lazy(pin_sink_create(s->active_format));
    if (!s->sink) {
        open_failed(s, PIN_ERR_ARG, "unsupported output format");
        return;
    }
    if (!to_stdout && s->capture_opts.first_number != 0) {
        renumber_ctx_t *rc = calloc(1, sizeof(*rc));
        if (rc) {
            snprintf(rc->base, sizeof(rc->base), "%s", s->naming_base);
            snprintf(rc->ext, sizeof(rc->ext), "%s", s->naming_ext);
            rc->nopts = nopts;
            pin_sink_lazy_set_renumber(s->sink, renumber_next, rc, free);
        }
    }

    pin_sink_params_t params;
    memset(&params, 0, sizeof(params));
    /* the open file's kind; its format is the stream's confirmed one (dv_on_unit()) */
    const pin_kind_t kind = s->file_kind;
    params.kind = kind;
    strncpy(params.title, s->capture_opts.title, sizeof(params.title) - 1);
    /* The file's kind's aspect (aspect_hdv for HDV, aspect for DV and analog: a tape that
     * switches kind gives each file its own). AUTO resolves to the stream's own detected aspect (DV: VAUX 16:9
     * flag via dv_on_unit(); analog: fixed 4:3; HDV: the sequence header's
     * aspect_ratio_information, 16:9 until one was seen) -- see the
     * coordinator's "Default AUTO = stream's own". Wider than 4:3 is 16:9. */
    pin_aspect_t effective_aspect = file_aspect(&s->capture_opts, kind);
    if (effective_aspect == PIN_ASPECT_AUTO)
        effective_aspect = s->dar_num * 3 > s->dar_den * 4 ? PIN_ASPECT_16_9 : PIN_ASPECT_4_3;
    params.aspect = effective_aspect;

    int is_pal = s->detected_std != PIN_STD_NTSC && s->detected_std != PIN_STD_NTSC_443 &&
                 s->detected_std != PIN_STD_NTSC_J;
    /* Analog: "PAL" here means 625/50, which PAL-M and PAL-60 are not. */
    if (kind == PIN_KIND_ANALOG)
        is_pal = !pinnacle_std_is_60hz(s->analog.cfg.standard);
    if (kind == PIN_KIND_ANALOG) {
        params.width = (int)s->analog.width;
        params.height = (int)s->analog.height;
        params.fps_num = is_pal ? 25 : 30000;
        params.fps_den = is_pal ? 1 : 1001;
        params.interlaced = 1;
        params.top_field_first = 1;
        params.audio_rate = 48000;
        params.audio_channels = 2;
    } else if (kind == PIN_KIND_DV) {
        params.width = 720;
        params.height = is_pal ? 576 : 480;
        params.fps_num = is_pal ? 25 : 30000;
        params.fps_den = is_pal ? 1 : 1001;
        params.interlaced = 1;
        params.top_field_first = 0; /* DV: BFF, per the plan */
        /* the audio the stream's AAUX says (the DV sinks also read it from the first frame) */
        params.audio_rate = s->fmtw.cur.audio_rate > 0 ? s->fmtw.cur.audio_rate : 48000;
        params.audio_channels = s->fmtw.cur.audio_channels > 0 ? s->fmtw.cur.audio_channels : 2;
    } else { /* HDV: what the stream says (dv_on_unit()); 1080i29.97 TFF until it is known */
        params.width = s->hdv_seq_w ? s->hdv_seq_w : 1440;
        params.height = s->hdv_seq_h ? s->hdv_seq_h : 1080;
        hdv_frame_rate(s, &params.fps_num, &params.fps_den);
        params.interlaced = hdv_interlaced(s);
        params.top_field_first = params.interlaced && !s->hdv_bff;
        params.audio_rate = 48000;
        params.audio_channels = 2;
    }
    pin_sink_sar_for(kind, is_pal, params.aspect, params.width, params.height,
                      &params.sample_aspect_num, &params.sample_aspect_den);
    params.colour_matrix = kind == PIN_KIND_HDV ? PIN_MATRIX_BT709 : PIN_MATRIX_BT601;
    params.deinterlace = pin_fmt_clamp_deinterlace(s->capture_opts.deinterlace);
    params.video_quality = pin_fmt_clamp_quality(s->capture_opts.video_quality);
    params.ffv1_compression = pin_fmt_clamp_ffv1(s->capture_opts.ffv1_compression);

    pin_status_t st = s->sink->open(s->sink, path, &params);
    if (st != PIN_OK) {
        s->sink->close(s->sink); /* never opened: nothing to count */
        s->sink = NULL;
        open_failed(s, st, "failed to open output");
        return;
    }
    /* A full queue ends the capture (PIPE_SLOW / DISK_SLOW, capture_guard()):
     * the file then stops before the first lost unit instead of going on
     * with a hole that the sinks, counting frames, would not show. */
    size_t cap = to_stdout ? PIN_WRITER_PIPE_CAPACITY
               : kind == PIN_KIND_ANALOG ? PIN_WRITER_ANALOG_CAPACITY : 0;
    if (s->test_writer_queue)
        cap = (size_t)s->test_writer_queue;
    s->writer = pin_writer_start_ex(cap, writer_consume, s->sink, PIN_WRITER_OVERFLOW_FATAL);
    if (!s->writer) {
        /* no memory for the queue (or no thread): without a writer every unit would be
         * skipped and the capture would "run" without a file */
        s->sink->close(s->sink);
        s->sink = NULL;
        open_failed(s, PIN_ERR_NOMEM, "not enough memory to write the output file");
        return;
    }
    strncpy(s->current_file, path, sizeof(s->current_file) - 1);
    s->encoder_behind_logged = 0;
    pin_split_new_file(&s->split);
    /* the clip counters restart with every file (frames still held back by a
     * pending split were counted into the previous clip: at most ~1 s) */
    s->clip_frames = s->clip_frames_error = s->clip_frames_dropped = s->clip_write_dropped = 0;
    /* a file is announced when its first unit has been written; stdout has no file to wait for */
    s->file_announced = to_stdout;
    if (to_stdout)
        pin_session_push_event(s, PIN_EVT_FILE_OPENED, 0, path);
}

/* Sends PIN_EVT_FILE_OPENED once the open file has received its first video
 * unit (it does not exist before). Caller holds the lock. */
static void file_announce(pin_session_t *s)
{
    if (!s->sink || s->file_announced || !s->sink->get_status)
        return;
    pin_sink_status_t sst;
    memset(&sst, 0, sizeof(sst));
    s->sink->get_status(s->sink, &sst);
    if (sst.units_written > 0) {
        adopt_renumbered_file(s);
        s->file_announced = 1;
        pin_session_push_event(s, PIN_EVT_FILE_OPENED, 0, s->current_file);
    }
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
    hdv_err_state_t hdv_err;       /* continuity / reference tracking for the error stats */
    unsigned units_out;            /* frames / pictures handed to dv_on_unit() (replay pacing) */
    pin_kind_t unit_kind;          /* the kind of the last unit (DV <-> HDV resets the above) */
    dv_reassembler_t *reasm;       /* the live stream's reassembler (NULL for a replay) */
} dv_ctx_t;

/* A pass or capture that ends on its own: the frame (or HDV picture) the reassembler still
 * holds belongs to the file that is closing, and the data after the rewind starts a new
 * stream (it must not push that frame out into the next file, nor be glued to a half
 * frame). The frame is emitted into the open file, then the reassembler starts afresh.
 * Without the session lock: the reassembler's callbacks take it. */
static void dv_reasm_restart(dv_ctx_t *ctx)
{
    if (ctx->reasm)
        dv_reassembler_restart(ctx->reasm);
}

/* The same after a capture that ended some other way (a user's Stop, an error): the held
 * frame is dropped, not written, and the next capture starts on clean ground. Calls no
 * callback, so it is safe under the lock. */
static void dv_reasm_discard(dv_ctx_t *ctx)
{
    if (ctx->reasm)
        dv_reassembler_discard(ctx->reasm);
}

/* Forward decls: defined further down (near handle_inline_commands()), but
 * dv_on_unit()/analog_video_cb() above them need to call in when the
 * stream's kind newly becomes known. */
static void maybe_begin_capture(pin_session_t *s, uint16_t camera_node);

static int dv_write_cb(const uint8_t *data, size_t len, void *user)
{
    dv_ctx_t *ctx = user;
    pin_session_t *s = ctx->s;
    if (unit_kind(data, len) != PIN_KIND_HDV)
        return 0; /* a DV frame: dv_on_unit() follows and routes it */
    pin_session_lock(s);
    /* HDV: the latest PAT/PMT, to start a file with (hdv_write_gate(), scene_cut()); every unit,
     * captured or not, including the reassembler's own PAT/PMT unit at its gate. */
    hdv_psi_scan(&s->hdv_psi, data, len / HDV_TS_PACKET_SIZE);
    /* A picture gets dv_on_unit() next, which routes it. A unit that is no picture (that
     * PAT/PMT unit, what precedes the TS replay's first picture) gets none: it joins the
     * held units here with the index of the picture that follows it, so it goes where that
     * picture goes (after a new video PID or a DV -> HDV change: the new file, where the
     * gate writes the latest PAT/PMT in front of the first GOP anyway). */
    if (s->sink && hdv_picture_pid(data, len) < 0)
        hold_push(s, data, len, s->unit_seq, 0);
    pin_session_unlock(s);
    return 0;
}

/* The status fields of a DV / HDV stream from its confirmed format (s->fmtw.cur), so a
 * single frame or GOP with another value neither shows nor reaches a file's parameters.
 * Caller holds the lock. */
static void stream_status_from_format(pin_session_t *s)
{
    const pin_stream_fmt_t *f = &s->fmtw.cur;
    if (f->kind == 0)
        return;
    s->stream_kind = f->kind;
    if (f->kind == PIN_KIND_DV) {
        if (f->pal >= 0) {
            s->detected_std = f->pal ? PIN_STD_PAL : PIN_STD_NTSC;
            s->is_60hz = !f->pal;
            s->width = 720;
            s->height = f->pal ? 576 : 480;
        }
        s->dar_num = f->wide > 0 ? 16 : 4;
        s->dar_den = f->wide > 0 ? 9 : 3;
        return;
    }
    /* HDV: the format, as the stream says it (open_sink_for_scene() hands it to the sinks) */
    if (f->width > 0 && f->height > 0) {
        s->hdv_seq_w = f->width;
        s->hdv_seq_h = f->height;
    }
    if (f->fps_num > 0 && f->fps_den > 0) {
        /* a capture that starts mid-GOP has set the scene detector up with the default rate before
         * the first sequence header said which one it is */
        if (s->hdv_fps_num != f->fps_num || s->hdv_fps_den != f->fps_den)
            s->scene_det_ready = 0;
        s->hdv_fps_num = f->fps_num;
        s->hdv_fps_den = f->fps_den;
    }
    s->hdv_seqext_seen = f->progressive >= 0;
    s->hdv_progressive = f->progressive > 0;
    s->width = s->hdv_seq_w ? s->hdv_seq_w : 1440;
    s->height = s->hdv_seq_h ? s->hdv_seq_h : 1080;
    s->dar_num = s->hdv_dar_num ? s->hdv_dar_num : 16;
    s->dar_den = s->hdv_dar_num ? s->hdv_dar_den : 9;
    s->detected_std = PIN_STD_AUTO; /* HDV: not from the SAA7113, leave detected_std unset */
}

/* What the open file holds once the units on their way to it are in: what its sink wrote, the
 * writer's queue, and the units held back (pin_holdback.h) or by a pending scene split
 * (pin_split.h), which go to it first when a hard cut comes. An estimate on the high side (an
 * encoding sink writes less than it is given). Caller holds the lock. */
static uint64_t file_bytes_due(pin_session_t *s)
{
    uint64_t n = 0;
    if (s->sink && s->sink->get_status) {
        pin_sink_status_t sst;
        memset(&sst, 0, sizeof(sst));
        s->sink->get_status(s->sink, &sst);
        n = sst.bytes_written;
    }
    if (s->writer) {
        pin_writer_stats_t wst;
        pin_writer_get_stats(s->writer, &wst);
        n += wst.backlog_bytes;
    }
    return n + s->hold.bytes + s->split.pend_bytes;
}

/* The new file of a size cut begins here (pin_holdback_cut()): the open file is full for its
 * drive (FAT32), so it is finished and the capture goes on in the next one, numbered as after a
 * format change ("name-0002" after "name"). The format stays; an HDV file starts with the PAT and
 * PMT, then the cut picture, which opens a GOP. Caller holds the lock. */
static void size_cut_begin(void *user, long index)
{
    pin_session_t *s = user;
    (void)index;
    hand_over_file(s);
    s->scene_index++;
    if (!scene_split_on(s))
        s->fmt_numbered = 1;
    open_sink_for_scene(s);
    if (s->sink && s->file_kind == PIN_KIND_HDV)
        hdv_emit_psi(s);
}

/* A DV / HDV unit during a capture: held (pin_holdback.h) while a format change or a scene
 * cut may still be back-dated to it, then on to the file (commit_unit_fi()). fmt_res and
 * old/fmt_start: what the format watcher said about this unit; rec: its scene record (NULL:
 * none, e.g. an unreadable DV frame). Caller holds the lock. */
static void capture_unit(pin_session_t *s, const uint8_t *data, size_t len, pin_kind_t kind, long idx,
                         unsigned flags, pin_fmtwatch_result_t fmt_res, const pin_stream_fmt_t *old,
                         long fmt_start, pin_scene_record_t *rec)
{
    /* A drive whose files have a size limit (FAT32: 4 GiB): the file goes on in a new one, at
     * this unit if a file may start with it and the open file would otherwise grow past
     * pin_stop_file_cap() before the next such unit, one DV frame or HDV GOP on (as long as the
     * last one was). Not while a format change is pending: its cut comes first (the margin
     * under the limit covers the wait). Never a pipe (file_limit 0). */
    if (flags & PIN_HOLD_CUT_OK) {
        s->cut_last_run = s->cut_run_bytes;
        s->cut_run_bytes = 0;
    }
    s->cut_run_bytes += len;
    uint64_t ahead = s->cut_last_run > len ? s->cut_last_run : len;
    int size_cut = s->file_limit && (flags & PIN_HOLD_CUT_OK) && kind == s->file_kind &&
                   (s->split.file_units > 0 || s->hold.count > 0) && fmt_res == PIN_FMTWATCH_SAME &&
                   !pin_fmtwatch_pending(&s->fmtw, NULL) &&
                   file_bytes_due(s) + ahead > pin_stop_file_cap(s->file_limit);

    hold_push(s, data, len, idx, flags);

    if (size_cut) {
        pin_logf(PIN_LOG_INFO, "session: %s is %.0f MB, near the drive's file size limit: "
                               "going on in a new file\n", s->current_file, file_bytes_due(s) / 1e6);
        split_ensure(s);
        pin_split_cancel(&s->split); /* a pending scene split's units stay in the open file */
        pin_holdback_cut(&s->hold, idx, size_cut_begin, s);
    }

    /* a confirmed format change: a new file from its first unit on, or the end of a pipe */
    if (fmt_res == PIN_FMTWATCH_CHANGED)
        capture_format_change(s, old, fmt_start);

    /* scene split, on the units of the open file's kind */
    int scene = scene_split_on(s) && rec && kind == s->file_kind && s->sink;
    if (scene) {
        if (!s->scene_det_ready) {
            pin_scene_config_t cfg;
            if (kind == PIN_KIND_HDV) {
                int fn, fd;
                hdv_frame_rate(s, &fn, &fd); /* the GOP timecode counts pictures at this rate */
                pin_scene_config_hdv(&cfg, (double)fn / fd);
            } else {
                pin_scene_config_defaults(&cfg, s->is_60hz ? 29.97 : 25.0);
            }
            pin_scene_init(&s->scene_det, &cfg);
            s->opts_scene_split_window = pin_scene_debounce_window(&s->scene_det);
            s->scene_det_ready = 1;
        }
        rec->frame_index = idx;
        long cut_idx = 0;
        if (pin_scene_feed(&s->scene_det, rec, &cut_idx)) {
            if (pin_fmtwatch_pending(&s->fmtw, NULL)) {
                /* a format change may be confirmed at about the same unit (a new recording
                 * often brings both): it is the harder cut, so this one waits for it */
                s->scene_cut_deferred = 1;
                s->scene_cut_at = cut_idx;
            } else {
                scene_cut(s, cut_idx);
            }
        }
    }
    if (s->scene_cut_deferred && !pin_fmtwatch_pending(&s->fmtw, NULL)) {
        /* the format change it waited for was not confirmed (or changes no file) */
        s->scene_cut_deferred = 0;
        scene_cut(s, s->scene_cut_at);
    }

    /* release what no pending cut can land on any more */
    long keep_from = idx + 1, st;
    if (pin_fmtwatch_hold_from(&s->fmtw, &st) && st < keep_from)
        keep_from = st;
    if (s->scene_cut_deferred && s->scene_cut_at < keep_from)
        keep_from = s->scene_cut_at;
    if (scene && s->scene_det_ready) {
        /* hold from a pending break's first unit (HDV: it can span a few hundred pictures,
         * the detector debounces over GOPs) */
        if (pin_scene_pending(&s->scene_det, &st) && st < keep_from)
            keep_from = st;
        /* DV: and the last debounce_frames + 1 frames, as the detector confirms within
         * debounce_frames records, each a frame */
        if (kind == PIN_KIND_DV && idx - (long)s->opts_scene_split_window < keep_from)
            keep_from = idx - (long)s->opts_scene_split_window;
    }
    pin_holdback_release_before(&s->hold, keep_from);
}

/* Data lost in transit (dv_output_t.on_loss): the blocks, and the whole frames that never
 * reached dv_on_unit(), which are dropped ones. */
static void dv_on_loss(unsigned long blocks, unsigned long frames, void *user)
{
    dv_ctx_t *ctx = user;
    pin_session_t *s = ctx->s;
    pin_session_lock(s);
    s->lost_blocks += blocks;
    s->frames_dropped += frames;
    s->clip_frames_dropped += frames;
    pin_session_unlock(s);
}

static void dv_on_unit(dv_format_t fmt, const uint8_t *data, size_t len, void *user)
{
    dv_ctx_t *ctx = user;
    pin_session_t *s = ctx->s;
    ctx->units_out++;

    pin_session_lock(s);
    pin_kind_t kind = fmt == DV_FORMAT_HDV ? PIN_KIND_HDV : PIN_KIND_DV;
    int kind_newly_known = !s->stream_kind_known;
    int signal_back = !s->signal;
    s->stream_kind_known = 1;
    s->signal = 1;
    if (kind != ctx->unit_kind) {
        /* the first unit, or DV <-> HDV: the per-stream tracking starts afresh */
        ctx->unit_kind = kind;
        ctx->hdv_map_init = 0;
        s->hdv_err_reset = 1;
        s->hdv_tc_live = 0;
    }
    long idx = s->unit_seq++;
    pin_stream_fmt_t obs;          /* what this unit says about the format */
    pin_stream_fmt_init(&obs, kind);
    unsigned flags = 0;            /* PIN_HOLD_CUT_OK: a file may start with it */
    pin_scene_record_t rec;
    memset(&rec, 0, sizeof(rec));
    int have_rec = 0;
    unit_counts_t uc;
    memset(&uc, 0, sizeof(uc));
    int hdv_dar_num = 0, hdv_dar_den = 0, hdv_bff = -1; /* this picture's, if it says */

    if (kind == PIN_KIND_DV) {
        dv_frame_info_t info;
        if (dv_subcode_parse_frame(data, len, &info) == 0) {
            int is_pal = info.dsf_valid ? info.is_pal : (info.sequence_count == DV_SEQ_COUNT_PAL);
            obs.pal = is_pal;
            if (info.aspect.valid)
                obs.wide = info.aspect.is_16_9 ? 1 : 0;
            if (info.audio.valid && info.audio.sample_rate > 0) {
                obs.audio_rate = info.audio.sample_rate;
                obs.audio_channels = info.audio.channels;
                obs.audio_bits = info.audio.bits;
            }
            flags = PIN_HOLD_CUT_OK;
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
            uc.err = derr.frame_error;
            uc.dropped = derr.frame_dropped;
            uc.video = derr.video_err + derr.video_concealed;
            uc.audio = derr.audio_err + derr.audio_mute;
            uc.missing = derr.missing_blocks;

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

            /* the scene record (scene split, capture_unit()) */
            have_rec = 1;
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
        } else {
            /* not 10 or 12 whole sequences (a bogus resync): says nothing about the format;
             * the sink counts it as damaged and drops it */
            s->frames_damaged++;
            uc.err = uc.dropped = 1;
        }
    } else { /* HDV */
        if (!ctx->hdv_map_init) {
            memset(&ctx->hdv_map, 0, sizeof(ctx->hdv_map));
            ctx->hdv_map.pmt_pid = -1; ctx->hdv_map.video_pid = -1;
            ctx->hdv_map.aux_pid = -1; ctx->hdv_map.audio_pid = -1;
            ctx->hdv_map_init = 1;
        }
        hdv_pid_map_t *map_p = &ctx->hdv_map;
        int pic_pid = hdv_picture_pid(data, len);
        if (pic_pid > 0 && map_p->video_pid > 0 && pic_pid != map_p->video_pid) {
            /* the reassembler follows a new video PID (its gate opened again on it): so does
             * the map, from the latest PAT/PMT */
            memset(map_p, 0, sizeof(*map_p));
            map_p->pmt_pid = -1; map_p->video_pid = -1; map_p->aux_pid = -1; map_p->audio_pid = -1;
        }
        if (map_p->video_pid < 0) {
            /* The reassembler hands the PAT/PMT it has seen to the file only (dv_write_cb(),
             * which keeps them in hdv_psi), not in a picture: start from them, or the first
             * pictures (before the next PAT/PMT rides in one) go without a video PID: no
             * format, preview or error stats. */
            if (s->hdv_psi.pat_valid) hdv_scan_pat_pmt(s->hdv_psi.pat, 1, map_p);
            if (s->hdv_psi.pmt_valid) hdv_scan_pat_pmt(s->hdv_psi.pmt, 1, map_p);
        }
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
            /* A stream that says 00:00:00:00 all the time carries no timecode: leave
             * the display to the deck's TIME CODE poll (dv_tick) until it shows a real one. */
            if (hdv_gop_time_nonzero(&gop))
                s->hdv_tc_live = 1;
            if (s->hdv_tc_live)
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
            /* into HDV MOV / MKV: whether it is "with error" is the file's to say (what it
             * re-encoded or played with silence, sink_hdvmux.c), not the packets' */
            uc.hdv = 1;
            uc.err = herr.frame_error;
            uc.video = herr.video_damaged || herr.tainted;
            uc.missing = herr.cc_errors + herr.tei + herr.sync_lost;
        }
        /* the video format, from a sequence header (and its extension) */
        if (herr.seq_found && herr.seq_width > 0 && herr.seq_height > 0) {
            obs.width = herr.seq_width;
            obs.height = herr.seq_height;
            int fn = 0, fd = 0;
            if (pin_vidfmt_rate_from_code(herr.seq_frame_rate_code, &fn, &fd)) {
                if (herr.seqext_found) {
                    fn *= herr.seq_fr_ext_n + 1;
                    fd *= herr.seq_fr_ext_d + 1;
                }
                obs.fps_num = fn;
                obs.fps_den = fd;
            }
            if (herr.seqext_found)
                obs.progressive = herr.seq_progressive ? 1 : 0;
            pin_vidfmt_mpeg2_dar(herr.seq_aspect, herr.seq_width, herr.seq_height,
                                 &hdv_dar_num, &hdv_dar_den);
        }
        /* field order: from interlaced pictures only (a progressive frame's top_field_first
         * belongs to its repeat_first_field); a field picture's first field is its structure */
        if (herr.picext_found && !herr.pic_progressive)
            hdv_bff = herr.pic_structure == 3 ? !herr.pic_tff : herr.pic_structure == 2;

        /* A file may start on a picture that opens a GOP (HDV puts a sequence header in
         * front of every GOP; a GOP without one is cut at its first picture all the same,
         * and the file then decodes from the next sequence header). Scene split: the GOP
         * header's timecode on the picture that carries it is the only signal (HDV has no
         * per-frame recording date/time here -- see hdv_aux.h's file header comment); the
         * other pictures are neutral, and the detector debounces over GOPs
         * (pin_scene_config_hdv()). */
        flags = gop.found || herr.seq_found ? PIN_HOLD_CUT_OK : 0;
        have_rec = 1;
        if (gop.found) {
            rec.tc_valid = 1;
            rec.tc_hour = gop.hours; rec.tc_minute = gop.minutes;
            rec.tc_second = gop.seconds; rec.tc_frame = gop.pictures;
            rec.tc_drop_frame = gop.drop_frame;
            /* the camera clock too: a jump of it is another recording (pin_scene.h) */
            rec.ts_valid = herr.dts_valid;
            rec.ts = herr.dts;
        }
    }

    /* The format, with its cool-down: a different value counts only once it persists
     * (pin_fmtwatch.h); the status (and the next file) shows the confirmed one. */
    long fmt_start = idx;
    pin_stream_fmt_t old_fmt;
    pin_fmtwatch_result_t fr = pin_fmtwatch_feed(&s->fmtw, &obs, idx, &fmt_start, &old_fmt);
    if (kind == PIN_KIND_HDV && pin_stream_fmt_agrees(&s->fmtw.cur, &obs)) {
        /* aspect and field order of the confirmed format's pictures */
        if (hdv_dar_num > 0) {
            s->hdv_dar_num = hdv_dar_num;
            s->hdv_dar_den = hdv_dar_den;
        }
        if (hdv_bff >= 0)
            s->hdv_bff = hdv_bff;
    }
    stream_status_from_format(s);
    if (fr == PIN_FMTWATCH_CHANGED) {
        char a[96], b[96];
        pin_stream_fmt_describe(&old_fmt, a, sizeof(a));
        pin_stream_fmt_describe(&s->fmtw.cur, b, sizeof(b));
        pin_logf(PIN_LOG_INFO, "session: the input changed from %s to %s\n", a, b);
    }

    /* The unit that makes the stream kind known opens the file, and is its first unit:
     * the capture's counters restart there, so this unit is counted after them. */
    if (kind_newly_known)
        maybe_begin_capture(s, s->dev.camera_node);
    apply_unit_counts(s, &uc);

    if (s->sink)
        capture_unit(s, data, len, kind, idx, flags, fr, &old_fmt, fmt_start, have_rec ? &rec : NULL);

    s->last_data_s = pin_session_now();
    s->idle_s = 0;
    if (fr != PIN_FMTWATCH_SAME || signal_back)
        pin_session_push_event(s, PIN_EVT_INPUT_FORMAT, 0, NULL);
    pin_session_unlock(s);
}

static void dv_flush_pending(pin_session_t *s)
{
    pin_holdback_flush(&s->hold);
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

/* Length of what this capture recorded (all passes), summed over the units that went into a
 * file (count_frame()). */
static double captured_seconds(const pin_session_t *s)
{
    if (!s->capture_began)
        return 0;
    return s->captured_s > 0 ? s->captured_s : 0;
}

/* Records why the capture ended and tells the GUI (PIN_EVT_CAPTURE_ENDED).
 * Caller holds the lock. */
static void capture_report_end(pin_session_t *s, pin_stop_reason_t why, const char *detail)
{
    /* The end is reported once every file is closed (PIN_EVT_FILE_CLOSED
     * first, the counts below complete, capture_end_seq means "on disk"). */
    closer_drain(s);
    s->stop_reason = why;
    s->stop_captured_s = captured_seconds(s);
    /* no unit reached any file (they are closed by now): there is nothing to keep */
    s->stop_no_video = s->units_total == 0;
    int say_no_video = s->stop_no_video && !pin_stop_abnormal(why);
    if (say_no_video)
        pin_stop_message_no_video(why, s->capture_opts.path, detail, s->stop_text, sizeof(s->stop_text));
    else
        pin_stop_message(why, s->stop_captured_s, detail, s->stop_text, sizeof(s->stop_text));
    if (why == PIN_STOP_DEVICE_LOST && !pin_path_is_stdout(s->capture_opts.path)) {
        /* the files were closed (finalised) before this; say where they are */
        size_t n = strlen(s->stop_text);
        if (s->units_total > 0 && s->current_file[0])
            snprintf(s->stop_text + n, sizeof(s->stop_text) - n,
                     " The file was saved up to that point: %s", s->current_file);
        else
            snprintf(s->stop_text + n, sizeof(s->stop_text) - n, " No file was written.");
    }
    if (s->close_failed && !pin_path_is_stdout(s->capture_opts.path)) {
        /* a file that was not closed properly is a failed capture, whatever else ended it */
        const char *name = s->close_fail_path;
        for (const char *p = name; *p; p++)
            if (*p == '/' || *p == '\\')
                name = p + 1;
        size_t n = strlen(s->stop_text);
        snprintf(s->stop_text + n, sizeof(s->stop_text) - n, " But %s%s could not be written completely: %s.",
                 name, s->close_failed > 1 ? " (and other files)" : "", pin_strerror(s->close_fail_status));
        if (!pin_stop_abnormal(why))
            s->stop_reason = why = PIN_STOP_WRITE_ERROR;
    }
    /* the totals as they stand now: a script may start the next capture (and reset them) before
     * the caller has read these */
    status_fill_locked(s, &s->end_status);
    s->have_end_status = 1;
    if (s->audio_write_dropped)
        pin_logf(PIN_LOG_WARN, "session: %llu audio block(s) were refused by the full writer queue\n",
                 (unsigned long long)s->audio_write_dropped);
    pin_logf(why == PIN_STOP_USER ? PIN_LOG_INFO : PIN_LOG_WARN, "session: %s\n", s->stop_text);
    pin_session_push_event(s, PIN_EVT_CAPTURE_ENDED, (int32_t)why, s->stop_text);
    if (say_no_video)
        pin_session_push_event(s, PIN_EVT_NO_VIDEO, (int32_t)why, s->stop_text);
    s->capture_began = 0;
    s->disk_slow_pending = 0;
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

/* Finishes the open file (pending units first): hands it to the closer,
 * which reports it closed (PIN_EVT_FILE_CLOSED). */
static void finish_file(pin_session_t *s)
{
    if (!s->sink) {
        /* the file was lost (a failed open): what was held for it has no file to go to, and
         * must not open the next capture's */
        pin_holdback_free(&s->hold);
        pin_split_free(&s->split);
        memset(&s->split, 0, sizeof(s->split));
        return;
    }
    dv_flush_pending(s);
    hand_over_file(s);
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
    if (active) {
        closer_drain_unlocked(s);
        capture_report_end(s, why, detail);
    }
}

/* The deck a capture that ends on its own stops: the camera, if the capture
 * drives the deck (start_deck). */
static uint16_t auto_stop_node(const pin_session_t *s, uint16_t node)
{
    return s->capture_opts.start_deck ? node : 0;
}

/* While a file is open, once a second: ends the capture while the output
 * drive still has room to finish the file (pin_stop_disk_reserve()), and at
 * once when writing failed. deck_node as for auto_stop_node(). Caller holds
 * the lock. Returns 1 if it ended the capture. */
static int capture_guard(pin_session_t *s, uint16_t deck_node)
{
    if (s->disk_slow_pending && s->state == PIN_STATE_CAPTURING) {
        capture_end(s, PIN_STOP_DISK_SLOW, NULL, deck_node);
        set_state(s, PIN_STATE_READY);
        return 1;
    }
    if (s->sink && s->state == PIN_STATE_CAPTURING && pin_closer_failures(s->closer) > 0) {
        /* a finished file could not be written completely (a scene split's old file): the drive
         * is failing, so the capture ends; its report says which file */
        capture_end(s, PIN_STOP_WRITE_ERROR, NULL, deck_node);
        set_state(s, PIN_STATE_READY);
        return 1;
    }
    if (!s->sink || !s->writer || s->state != PIN_STATE_CAPTURING)
        return 0;
    if (s->fmt_stop) {
        /* standard output saw DV <-> HDV (capture_format_change()): the reading program
         * expects the one format it got, and a pipe cannot start a new file */
        s->fmt_stop = 0;
        char detail[PIN_TEXT_MAX];
        snprintf(detail, sizeof(detail), "%s", s->fmt_stop_text);
        capture_end(s, PIN_STOP_ERROR, detail, deck_node);
        set_state(s, PIN_STATE_READY);
        return 1;
    }
    pin_writer_stats_t wst;
    pin_writer_get_stats(s->writer, &wst);
    if (pin_path_is_stdout(s->capture_opts.path)) {
        /* a pipe: no disk to run out of, and the reader cannot be waited for */
        pin_stop_reason_t pwhy;
        if (wst.overflowed) {
            pwhy = PIN_STOP_PIPE_SLOW;
            pin_writer_abort(s->writer); /* do not wait for a stalled reader to drain the queue */
        } else if (wst.failed) {
            pin_sink_status_t sst;
            memset(&sst, 0, sizeof(sst));
            if (s->sink->get_status)
                s->sink->get_status(s->sink, &sst);
            pwhy = sst.pipe_closed ? PIN_STOP_PIPE_CLOSED : PIN_STOP_WRITE_ERROR;
        } else {
            return 0;
        }
        capture_end(s, pwhy, NULL, deck_node);
        set_state(s, PIN_STATE_READY);
        return 1;
    }
    if (wst.overflowed && !wst.failed) {
        /* The queue is drained into the file as it closes. */
        capture_end(s, PIN_STOP_DISK_SLOW, NULL, deck_node);
        set_state(s, PIN_STATE_READY);
        return 1;
    }
    if (s->file_limit && !wst.failed) {
        /* A drive whose files cannot pass a size (FAT32): DV and HDV go on in a new file before
         * it (capture_unit()), so for them this is only the backstop, half way into the margin
         * (a stream that offers no place to cut); analog has no cut and stops at the cap. The
         * file is finished in the margin left. */
        uint64_t cap = pin_stop_file_cap(s->file_limit);
        if (s->file_kind != PIN_KIND_ANALOG)
            cap += (s->file_limit - cap) / 2;
        if (file_bytes_due(s) > cap) {
            char detail[PIN_TEXT_MAX];
            pin_stop_file_limit_text(s->file_limit, detail, sizeof(detail));
            capture_end(s, PIN_STOP_DISK_FULL, detail, deck_node);
            set_state(s, PIN_STATE_READY);
            return 1;
        }
    }
    double now = pin_session_now();
    if (!wst.failed && now - s->disk_check_s < 1.0)
        return 0;
    s->disk_check_s = now;
    char dir[PIN_PATH_MAX];
    dir_of(s->current_file, dir, sizeof(dir));
    uint64_t free_bytes = 0;
    int fat32 = 0;
    int known = fat32_and_free(dir, &free_bytes, &fat32) == 0;
    if (s->test_free_space) {
        /* test seam PIN_TEST_FREE_SPACE: a drive with that much room when the capture began */
        pin_sink_status_t sst;
        memset(&sst, 0, sizeof(sst));
        if (s->sink->get_status)
            s->sink->get_status(s->sink, &sst);
        uint64_t used = s->bytes_closed + sst.bytes_written;
        free_bytes = s->test_free_space > used ? s->test_free_space - used : 0;
        known = 1;
    }
    /* plus the files the closer is still finishing */
    uint64_t reserve = pin_stop_disk_reserve(0, wst.backlog_bytes) +
                       pin_closer_pending_bytes(s->closer);
    char detail[PIN_TEXT_MAX];
    pin_stop_reason_t why;
    if (known && free_bytes < reserve) {
        why = PIN_STOP_DISK_FULL;
        snprintf(detail, sizeof(detail), "the output drive is almost full (%.0f MB left)",
                 (double)free_bytes / 1048576.0);
    } else if (wst.failed) {
        /* the H.264 / HEVC sink reports a failed encoder as a codec error, not a disk one */
        pin_sink_status_t sst;
        memset(&sst, 0, sizeof(sst));
        if (s->sink->get_status)
            s->sink->get_status(s->sink, &sst);
        why = pin_stop_writer_failure(s->active_format, sst.last_error, sst.reencode_behind,
                                      detail, sizeof(detail));
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
              gone ? "The capture device was disconnected. Reconnect it to continue."
                   : "The USB connection to the capture device failed.");
    pin_session_unlock(s);
    return gone;
}

/* The next file's kind and format: the stream's (confirmed) kind, capture_opts' format for
 * it (raw DIF / TS on standard output), its extension (the base name stays the one the
 * capture started with) and, for HDV, the gate. A capture start, a pass and a DV <-> HDV
 * change. Caller holds the lock. */
static void file_format_for_stream(pin_session_t *s)
{
    s->file_kind = s->stream_kind;
    s->active_format = s->file_kind == PIN_KIND_HDV ? s->capture_opts.format_hdv
                      : s->file_kind == PIN_KIND_ANALOG ? s->capture_opts.format_analog
                      : s->capture_opts.format_dv;
    if (pin_path_is_stdout(s->capture_opts.path))     /* stdout: raw DIF / TS / NUT, see open_sink_for_scene() */
        s->active_format = s->file_kind == PIN_KIND_HDV ? PIN_FMT_HDV_TS
                           : s->file_kind == PIN_KIND_ANALOG ? PIN_FMT_ANALOG_AVI : PIN_FMT_DV_RAW;
    format_ext(&s->capture_opts, s->active_format, s->naming_ext, sizeof(s->naming_ext));
    s->hdv_await_gop = s->file_kind == PIN_KIND_HDV;
}

/* Test seams, like PIN_REPLAY_UNPLUG_AFTER: environment variables the ctest suite sets, read as a
 * capture starts; unset (or 0) they change nothing.
 *   PIN_TEST_FREE_SPACE=<bytes>    capture_guard() sees that much free space on the output drive,
 *                                  less what the capture has written: a small drive
 *   PIN_TEST_WRITER_QUEUE=<bytes>  the size of the writer queue in front of every file or pipe
 *   PIN_TEST_SINK_DELAY_MS=<ms>    the writer thread waits that long before each unit: a slow drive
 *   PIN_TEST_FAT32_LIMIT=<bytes>   the output drive is FAT32 with that file size limit (not 4 GiB)
 * The sinks have PIN_TEST_WRITE_FAIL_AFTER (pin_sink_fault.c): writes that fail. */
static uint64_t test_seam(const char *name)
{
    const char *v = getenv(name);
    return v && v[0] ? (uint64_t)strtoull(v, NULL, 10) : 0;
}

/* Caller holds the lock. */
static void test_seams_read(pin_session_t *s)
{
    s->test_free_space = test_seam("PIN_TEST_FREE_SPACE");
    s->test_writer_queue = test_seam("PIN_TEST_WRITER_QUEUE");
    g_test_sink_delay_ms = (int)test_seam("PIN_TEST_SINK_DELAY_MS");
}

/* The largest file the capture's output drive takes (FAT32: 4 GiB less a byte), 0 for none or
 * standard output. Caller holds the lock. */
static uint64_t output_file_limit(pin_session_t *s)
{
    if (pin_path_is_stdout(s->capture_opts.path))
        return 0;
    uint64_t seam = test_seam("PIN_TEST_FAT32_LIMIT");
    if (seam)
        return seam;
    char dir[PIN_PATH_MAX];
    dir_of(s->naming_base, dir, sizeof(dir));
    uint64_t free_bytes = 0;
    int fat32 = 0;
    return fat32_and_free(dir, &free_bytes, &fat32) == 0 && fat32 ? PIN_FAT32_FILE_MAX : 0;
}

/* Actually opens the sink and enters CAPTURING, once the stream's kind
 * (DV/HDV/analog) is known and any requested rewind-to-start has finished.
 * s->capture_opts must already hold the requested options. */
static void start_capture_now(pin_session_t *s, uint16_t camera_node)
{
    file_format_for_stream(s);
    pin_naming_strip_extension(s->capture_opts.path, s->naming_ext, s->naming_base, sizeof(s->naming_base));
    test_seams_read(s);
    s->file_limit = output_file_limit(s);
    s->cut_run_bytes = s->cut_last_run = 0;
    s->fmt_numbered = 0;
    s->fmt_stop = 0;
    s->scene_cut_deferred = 0;
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
    pin_split_free(&s->split);
    memset(&s->split, 0, sizeof(s->split));
    pin_holdback_free(&s->hold);   /* the last capture's units are not this one's */
    /* the capture exists before its first file does: a failed open ends it and stops the deck
     * (open_failed()) */
    s->capture_began = 1;
    s->disk_slow_pending = 0;
    s->opening_first_file = 1;
    open_sink_for_scene(s);
    s->opening_first_file = 0;
    if (s->state != PIN_STATE_ERROR) {
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
    /* Taken before it runs: ending a capture releases the lock while the
     * files finish (closer_drain_unlocked()), and a command posted then
     * must stay pending, not be cleared with this one. */
    pin_cmd_t cmd = s->cmd;
    s->cmd.pending = 0;
    s->cmd.kind = PIN_CMD_NONE;
    pthread_cond_broadcast(&s->cmd_idle);

    if (kind == PIN_CMD_CAPTURE_START) {
        s->capture_opts = cmd.capture;
        if (pin_path_is_stdout(s->capture_opts.path)) {
            /* a stream has no files to split, keep or number, and one pass */
            s->capture_opts.scene_split = 0;
            s->capture_opts.keep_raw = 0;
            s->capture_opts.passes = 1;
            s->capture_opts.first_number = 0;
        }
        s->stop_reason = PIN_STOP_NONE;
        s->stop_no_video = 0;
        s->units_total = 0;
        s->stop_captured_s = 0;
        s->stop_text[0] = 0;
        s->close_failed = 0;
        pin_closer_take_failure(s->closer, NULL, NULL, 0);
        s->capture_began = 0;
        s->capture_want_start = 1;
        s->rewind_before_capture = 0;
        s->rewind_done = 0;
        s->start_play_sent = 0;
        s->pass_rewinding = 0;
        maybe_begin_capture(s, camera_node);
    } else if (kind == PIN_CMD_CAPTURE_STOP) {
        /* AS_STARTED: only a capture that drives the deck stops it. */
        int stop_deck = cmd.stop_deck == PIN_STOP_DECK_YES ||
                        (cmd.stop_deck == PIN_STOP_DECK_AS_STARTED && s->capture_opts.start_deck);
        /* The frame the reassembler is still assembling when the user stops is not written
         * (dv_tick() drops it afterwards): a frame is only complete once the next one starts, so
         * the last frame before the stop may be missing from the file. At most one frame at the
         * very end of a capture the user ended by hand is accepted; it cannot be written without
         * running the reassembler from here, with the lock held. */
        capture_end(s, PIN_STOP_USER, NULL, stop_deck ? camera_node : 0);
        if (s->state != PIN_STATE_ERROR)
            set_state(s, PIN_STATE_READY);
    } else if (kind == PIN_CMD_DECK) {
        uint16_t node = camera_node;
        if (!node) {
            set_error(s, PIN_ERR_NO_CAMERA, "no camera on the 1394 bus");
        } else if (s->state == PIN_STATE_CAPTURING && cmd.deck_cmd != PIN_DECK_CMD_STOP) {
            /* "while CAPTURING, only STOP accepted" */
        } else if (cmd.deck_cmd == PIN_DECK_CMD_STOP && s->state == PIN_STATE_CAPTURING) {
            /* STOP while capturing: finish the capture first, then the deck */
            capture_end(s, PIN_STOP_USER, NULL, node);
            set_state(s, PIN_STATE_READY);
        } else {
            deck_send(s, node, cmd.deck_cmd);
        }
    }
    pthread_mutex_unlock(&s->mtx);
    return 0;
}

/* Whether the deck's TIME CODE answer may set the displayed timecode: when no stream
 * is arriving (winding, stopped), or the HDV stream has none of its own (see hdv_tc_live).
 * Caller holds the session lock. */
static int deck_timecode_wanted(const pin_session_t *s)
{
    return !s->signal || (s->stream_kind == PIN_KIND_HDV && !s->hdv_tc_live);
}

static void dv_tick(void *user)
{
    dv_ctx_t *ctx = user;
    pin_session_t *s = ctx->s;
    uint16_t node = s->dev.camera_node;
    const unsigned end_seq = s->capture_end_seq;

    if (handle_inline_commands(s, node)) {
        s->loop_stop = 1;
        return;
    }

    pin_session_lock(s);
    double tnow = pin_session_now();

    /* "signal" for DV/HDV means data arrived recently */
    if (s->signal && tnow - s->last_data_s > 1.0) {
        s->signal = 0;
        s->hdv_tc_live = 0;
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
                    deck_timecode_wanted(s))
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
                /* the poll answers once or twice a second: an event per answer would fill the
                 * queue with the same state and push the rare ones (a file closed, the capture
                 * ended) out of it */
                if (s->deck != prev)
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
        /* HDV that carries no timecode of its own: ask the deck while it plays, too */
        int hdv_from_deck = s->signal && s->stream_kind == PIN_KIND_HDV && !s->hdv_tc_live;
        if (now - s->last_transport_poll_s > (winding || hdv_from_deck ? 0.5 : 1.0)) {
            s->last_transport_poll_s = now;
            int want_tc = (!s->signal && (winding || s->tc_after_wind)) || hdv_from_deck;
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
            if (sst.encoder_behind && !s->encoder_behind_logged) {
                /* the queue in front of the disk writer absorbs it; if it stays behind that
                 * fills and the capture ends (PIN_STOP_DISK_SLOW) */
                s->encoder_behind_logged = 1;
                pin_logf(PIN_LOG_WARN, "session: the encoder of %s is falling behind the capture\n",
                         s->current_file);
            }
        } else {
            s->bytes_written = wst.bytes_pushed;
        }
        file_announce(s);
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
        /* the last frame goes into the file that is ending (the lock is released for it; only
         * this thread changes the state, but the frame's own units can end the capture) */
        pin_session_unlock(s);
        dv_reasm_restart(ctx);
        pin_session_lock(s);
    }
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
            /* start the next pass, in the kind the stream has now (a change at the tape's
             * start replaces the file, see format_cut_begin()) */
            s->pass_rewinding = 0;
            s->scene_index = first_scene_number(s);
            s->fmt_numbered = 0;
            s->scene_cut_deferred = 0;
            file_format_for_stream(s);
            open_sink_for_scene(s);
            /* a failed open has ended the capture and stopped the deck (open_sink_for_scene()) */
            if (s->state != PIN_STATE_ERROR) {
                deck_send(s, node, PIN_DECK_CMD_PLAY);
                set_state(s, PIN_STATE_CAPTURING);
                pin_session_push_event(s, PIN_EVT_PASS, s->pass, NULL);
                s->capture_start_s = pin_session_now();
                s->elapsed_s = 0;
                s->last_data_s = pin_session_now(); /* no-signal timer restarts with the pass */
                s->idle_s = 0;
                s->scene_det_ready = 0;
            }
        }
    }
    pin_session_unlock(s);
    /* a capture that ended without the flush above (Stop, an error): see dv_reasm_discard() */
    if (s->capture_end_seq != end_seq)
        dv_reasm_discard(ctx);
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

    s->analog_valid = 0; /* the OHCI design replaces Capture */
    char fw[PIN_PATH_MAX], why[PIN_TEXT_MAX];
    if (pin_session_firmware_path(s->dev.model, PIN_KIND_DV, fw, sizeof(fw), why, sizeof(why)) != PIN_OK) {
        set_error(s, PIN_ERR_FIRMWARE, why);
        return;
    }

    pin_logf(PIN_LOG_DEBUG, "session: DV/HDV bring-up, FPGA bitstream %s\n", fw);
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
        dv_output_t out = { .write = dv_write_cb, .on_unit = dv_on_unit, .on_loss = dv_on_loss,
                            .user = NULL };
        dv_ctx_t ctx = { .s = s };
        out.user = &ctx;
        memset(&s->hdv_psi, 0, sizeof(s->hdv_psi)); /* a new stream: its own PAT/PMT */
        dv_reassembler_init(&reasm, &out);
        ctx.reasm = &reasm;

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
        pin_session_unlock(s);
        /* the last complete frame still goes to the open file */
        dv_reassembler_finish(&reasm);
        if (!rescan) {
            pin_session_lock(s);
            capture_end(s, PIN_STOP_USER, NULL, 0); /* input switch / close */
            pin_session_unlock(s);
        }
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
                                      (size_t)f->width * f->height * 2)) {
        s->write_dropped++;
        s->clip_write_dropped++;
        s->captured_s -= unit_seconds(s);
    }

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
        file_announce(s);
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
    if (b->silence) {
        /* A lost audio packet is not a frame: the video side already counts the frames it had to
         * stand in for as errors, and a second error per outage frame would push the share of
         * frames with errors past 100 %. It is counted as the audio block it is. */
        s->err_audio_blocks++;
    }
    if (s->writer && b->samples &&
        !pin_writer_push(s->writer, PIN_UNIT_AUDIO, b->seq, (const uint8_t *)b->pcm,
                         (size_t)b->samples * 4))
        s->audio_write_dropped++;   /* not a frame: kept apart from the "Dropped" count */
    pin_session_unlock(s);
    return 0;
}

static void do_run_analog(pin_session_t *s, pin_input_t input, int reuse)
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

    pinnacle_status_t pst = PINNACLE_OK;
    s->analog_valid = 0;
    if (reuse) {
        /* Input switch between S-Video and composite: the Capture design, the codec and the
         * capture block are as the last run left them (stopped). Only the decoder's input
         * (and the standard / picture, which a user may have changed meanwhile) are set. */
        pin_logf(PIN_LOG_DEBUG, "session: switching the analog input without a new bring-up\n");
        pinnacle_progress(&s->dev, "Switching the input", -1);
        pinnacle_analog_t *a = &s->analog;
        a->cfg.picture = s->want_picture;
        pst = pinnacle_analog_set_input(a, cfg.input);
        if (pst == PINNACLE_OK && s->requested_std != PIN_STD_AUTO)
            pst = pinnacle_analog_set_standard(a, cfg.standard);
        else if (pst == PINNACLE_OK)
            pst = pinnacle_analog_set_picture(a, &s->want_picture);
        if (pst != PINNACLE_OK) {
            pin_logf(PIN_LOG_WARN, "session: input switch failed (%s), opening the device again\n",
                     pinnacle_strerror(pst));
            reuse = 0;
        }
    }
    if (!reuse) {
        pin_logf(PIN_LOG_DEBUG, "session: analog bring-up, FPGA bitstream %s\n", fw);
        pst = pinnacle_analog_open(&s->analog, &s->dev, fw, &cfg);
        if (pst != PINNACLE_OK) {
            set_init_error(s, pst, fw);
            return;
        }
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
        pin_logf(PIN_LOG_DEBUG, "session: auto standard: %s, field rate %s -> %s\n",
                 ast.locked ? "decoder locked" : "no signal", ast.locked ? (ast.is_60hz ? "60 Hz" : "50 Hz") : "unknown",
                 pinnacle_std_name(s->analog.cfg.standard));
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
    s->analog_valid = 1;
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
            s->analog_valid = 0;
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
            s->analog_valid = 0;
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
        s->fmt_numbered = 0;
        s->scene_cut_deferred = 0;
        file_format_for_stream(s);
        open_sink_for_scene(s);
        s->deck = PIN_DECK_PLAYING;
        s->capture_start_s = pin_session_now();
        /* as a live pass change (dv_tick()): the tape starts over, so does the scene
         * detector, and an HDV file starts on a GOP */
        s->scene_det_ready = 0;
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

/* Test seam: PIN_REPLAY_UNPLUG_AFTER=<frames> makes the replay device behave as if it
 * were unplugged once a capture has written that many frames (stream_failed() then
 * ends the capture with PIN_STOP_DEVICE_LOST, as for a real USB disconnect). */
static uint64_t replay_unplug_after(void)
{
    const char *v = getenv("PIN_REPLAY_UNPLUG_AFTER");
    return v && v[0] ? (uint64_t)strtoull(v, NULL, 10) : 0;
}

/* Test seam: PIN_REPLAY_HOLD_MS=<ms> keeps the DV replay from sending its first byte for that
 * long, so a capture can be asked for before the first frame arrives. */
static double replay_hold_s(void)
{
    const char *v = getenv("PIN_REPLAY_HOLD_MS");
    return v && v[0] ? (double)strtoul(v, NULL, 10) / 1000.0 : 0;
}

/* The replay loops' share of what a hardware loop keeps up to date in the status (the DV and
 * analog ticks): the capture time, the writer backlog, the bytes on disk, the file-opened event
 * and the rate window behind the time-left estimate. At most every 100 ms (*last: the loop's
 * own timestamp; the HDV loop runs per 188-byte packet). Status only: the replay device ends a
 * capture at the end of its file, not by the time limits. Caller holds the lock. */
static void replay_stats(pin_session_t *s, double *last)
{
    double now = pin_session_now();
    if (now - *last < 0.1)
        return;
    *last = now;
    if (s->state == PIN_STATE_CAPTURING && s->capture_start_s > 0)
        s->elapsed_s = now - s->capture_start_s;
    if (!s->writer)
        return;
    pin_writer_stats_t wst;
    pin_writer_get_stats(s->writer, &wst);
    s->writer_backlog = wst.backlog_bytes;
    s->writer_backlog_max = wst.backlog_high_water;
    if (s->sink && s->sink->get_status) {
        pin_sink_status_t sst;
        memset(&sst, 0, sizeof(sst));
        s->sink->get_status(s->sink, &sst);
        s->bytes_written = sst.bytes_written;
    } else {
        s->bytes_written = wst.bytes_pushed;
    }
    file_announce(s);
    rate_sample(s);
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
    memset(&s->hdv_psi, 0, sizeof(s->hdv_psi));
    double next_t = 0; /* pace_frame() deadline */
    hdv_pid_map_t map;
    memset(&map, 0, sizeof(map));
    map.pmt_pid = -1; map.video_pid = -1; map.aux_pid = -1; map.audio_pid = -1;

    uint8_t *pic = NULL;
    size_t pic_len = 0, pic_cap = 0;
    int pic_is_video = 0; /* pic starts with a video PES start; else it is what precedes the
                             first picture (SDT, PAT, PMT, audio) */
    uint8_t pkt[HDV_TS_PACKET_SIZE];
    uint64_t unplug_after = replay_unplug_after();
    int unplugged = 0;
    double stats_t = 0;

    s->loop_stop = 0;
    while (!s->loop_stop) {
        if (handle_inline_commands(s, 0)) { s->loop_stop = 1; break; }

        pin_session_lock(s);
        capture_guard(s, 0);
        replay_stats(s, &stats_t);
        pin_deck_state_t d = s->deck;
        if (unplug_after && s->sink && s->frames >= unplug_after)
            unplugged = 1;
        pin_session_unlock(s);
        if (unplugged) { s->loop_stop = 1; break; }
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
            /* What precedes the first picture is no picture: like the reassembler's PAT/PMT
             * unit, it goes to the write callback only (which keeps its PAT/PMT for the
             * file starts), not counted as a picture. */
            if (pic_is_video) {
                dv_on_unit(DV_FORMAT_HDV, pic, pic_len, &ctx);
                /* one picture emitted: real time at the stream's own rate (its sequence header) */
                int fn, fd;
                pin_session_lock(s);
                hdv_frame_rate(s, &fn, &fd);
                pin_session_unlock(s);
                pace_frame(&next_t, (double)fd / fn);
            }
            pic_len = 0;
        }
        if (pic_len == 0)
            pic_is_video = is_video_start;
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

    if (unplugged) {
        stream_failed(s); /* the same path as a real unplug */
    } else {
        pin_session_lock(s);
        capture_end(s, PIN_STOP_USER, NULL, 0);
        pin_session_unlock(s);
    }
    fclose(f);
    return 0;
}

/* A plain MPEG-2 TS: the sync byte 0x47 at the start of each of its first (up to) four
 * 188-byte packets, whatever the file is called (a .dv file starts with a DIF header
 * block, 0x1F; an EP 0x88 dump has no such 188-byte pattern). */
static int replay_looks_like_ts(const uint8_t *head, size_t n)
{
    size_t packets = 0;
    for (size_t off = 0; off < n && packets < 4; off += HDV_TS_PACKET_SIZE, packets++)
        if (head[off] != HDV_TS_SYNC_BYTE)
            return 0;
    return packets > 0;
}

static int replay_run(pin_session_t *s)
{
    const char *path = s->device_id + 7; /* "replay:<file>" */
    FILE *f = fopen(path, "rb");
    if (!f) {
        set_error(s, PIN_ERR_NOT_FOUND, "replay file not found");
        return -1;
    }
    uint8_t head[4 * HDV_TS_PACKET_SIZE];
    size_t head_len = fread(head, 1, sizeof(head), f);
    fseek(f, 0, SEEK_SET);
    double next_t = 0; /* pace_frame() deadline */

    /* What the file is: an HDV transport stream (by its content, or a .ts / .m2t name),
     * whole DV frames (.dv), or else a raw EP 0x88 dump for the reassembler. */
    const char *ext = strrchr(path, '.');
    int is_dv_frames = ext && strcasecmp(ext, ".dv") == 0;
    int is_ts = replay_looks_like_ts(head, head_len) ||
                (ext && (strcasecmp(ext, ".ts") == 0 || strcasecmp(ext, ".m2t") == 0));
    if (is_ts)
        return replay_run_ts(s, f); /* closes f itself */

    p1394_init(&s->link, &s->dev); /* unused fields only; no real 1394 for replay */
    s->camera_present = 1;
    set_state(s, PIN_STATE_READY);
    s->deck = PIN_DECK_PLAYING;

    dv_reassembler_t reasm;
    dv_ctx_t ctx = { .s = s };
    dv_output_t out = { .write = dv_write_cb, .on_unit = dv_on_unit, .on_loss = dv_on_loss,
                        .user = &ctx };
    dv_reassembler_init(&reasm, &out);
    memset(&s->hdv_psi, 0, sizeof(s->hdv_psi));

    /* DV frames: PAL (144,000 bytes) or NTSC (120,000) by the DSF bit of the first frame's
     * header block (byte 3, bit 7), not by the file size: a whole number of NTSC frames can
     * be a whole number of PAL frames too (any multiple of 6). */
    long frame_size = 0;
    if (is_dv_frames && head_len >= 4 && (head[0] & 0xE0) == 0 && head[2] == 0) /* SCT 0, DBN 0 */
        frame_size = (head[3] & 0x80) ? DV_SEQ_COUNT_PAL * DV_SEQ_SIZE : DV_SEQ_COUNT_NTSC * DV_SEQ_SIZE;

    uint8_t *chunk = malloc(frame_size ? (size_t)frame_size : 65536);
    unsigned units_paced = 0;
    int chunks_idle = 0; /* raw dump: chunks in a row that ended no frame */
    uint64_t unplug_after = replay_unplug_after();
    int unplugged = 0;
    double stats_t = 0;
    double hold_until = pin_session_now() + replay_hold_s();
    s->loop_stop = 0;
    while (!s->loop_stop) {
        if (handle_inline_commands(s, 0)) { s->loop_stop = 1; break; }
        if (pin_session_now() < hold_until) { sleep_ms(10); continue; }

        pin_session_lock(s);
        capture_guard(s, 0);
        replay_stats(s, &stats_t);
        pin_deck_state_t d = s->deck;
        if (unplug_after && s->sink && s->frames >= unplug_after)
            unplugged = 1;
        pin_session_unlock(s);
        if (unplugged) { s->loop_stop = 1; break; }
        if (d == PIN_DECK_STOPPED || d == PIN_DECK_PAUSED) { sleep_ms(50); continue; }
        if (d == PIN_DECK_REWINDING) {
            sleep_ms(2000); /* simulated rewind time, per the plan */
            fseek(f, 0, SEEK_SET);
            /* the tape starts over: so does the reassembly (no half frame from before the
             * rewind glued to the first one; a real deck sends nothing while winding) */
            if (!frame_size) {
                dv_reassembler_finish(&reasm);
                dv_reassembler_init(&reasm, &out);
            }
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
        /* real-time pacing (NTSC 29.97 / PAL 25 fps): per frame in frame mode;
         * a raw dump per frame the reassembler emitted, so it runs at the true
         * rate too (the preview's jitter buffer and audio monitoring count on
         * it). A frame spans a few chunks; only a dump that yields no frames
         * at all sleeps, so it doesn't spin a core at full tilt. */
        if (frame_size) {
            pace_frame(&next_t, frame_size == 144000 ? 1.0 / 25.0 : 1001.0 / 30000.0);
        } else if (units_paced == ctx.units_out) {
            if (++chunks_idle > 16)
                sleep_ms(5);
        } else {
            chunks_idle = 0;
            pin_session_lock(s);
            double period = s->is_60hz ? 1001.0 / 30000.0 : 1.0 / 25.0;
            pin_session_unlock(s);
            while (units_paced != ctx.units_out) {
                pace_frame(&next_t, period);
                units_paced++;
            }
        }
    }
    free(chunk);

    dv_reassembler_finish(&reasm);
    if (unplugged) {
        stream_failed(s); /* the same path as a real unplug */
    } else {
        pin_session_lock(s);
        capture_end(s, PIN_STOP_USER, NULL, 0);
        pin_session_unlock(s);
    }
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
            pin_fmtwatch_init(&s->fmtw); /* a new input: its format is learnt afresh */
            s->signal = 0;
            s->hdv_tc_live = 0;
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

            pin_logf(PIN_LOG_DEBUG, "session: selecting the %s input\n",
                     cmd.input == PIN_INPUT_DV ? "DV/HDV" : cmd.input == PIN_INPUT_SVIDEO ? "S-Video" : "composite");
            if (!s->is_replay && !s->dev.handle) {
                pinnacle_status_t pst = pinnacle_open_by_id(&s->dev, s->device_id);
                if (pst != PINNACLE_OK) {
                    pin_session_lock(s);
                    char omsg[PIN_TEXT_MAX];
                    snprintf(omsg, sizeof(omsg), "Cannot open the device: %s", pinnacle_strerror(pst));
                    set_error(s, pst == PINNACLE_ERR_BUSY ? PIN_ERR_BUSY
                                 : pst == PINNACLE_ERR_BAD_FD ? PIN_ERR_NOT_FOUND
                                 : pst == PINNACLE_ERR_FD_UNSUPPORTED ? PIN_ERR_ARG : PIN_ERR_USB, omsg);
                    pin_session_unlock(s);
                    continue;
                }
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
                /* S-Video <-> composite needs no new bring-up when the analog design is
                 * still up from the last run on this device. */
                do_run_analog(s, cmd.input, s->analog_valid && s->dev.handle != NULL);
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
    /* "fd:N" is checked now (malformed, closed, or a platform that cannot use it), not at
     * the first pin_set_input(), where the device is actually opened. */
    pinnacle_status_t fdst = pinnacle_check_fd_id(device_id);
    if (fdst != PINNACLE_OK) {
        pin_logf(PIN_LOG_ERROR, "pin_open: \"%s\": %s\n", device_id,
                 fdst == PINNACLE_ERR_BAD_FD
                     ? "expected fd:N, N being an open file descriptor of the USB device"
                     : pinnacle_strerror(fdst));
        return PIN_ERR_ARG;
    }
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
    pin_fmtwatch_init(&s->fmtw);
    s->preview = pin_previewer_create();
    s->hdv_audio = pin_hdv_audio_create(pin_session_feed_monitor_audio, s);
    s->closer = pin_closer_create(file_closed, s);
    s->mon_cap_frames = 48000 * 2; /* 2 s at 48 kHz */
    s->mon_buf = calloc(s->mon_cap_frames * 2, sizeof(int16_t));
    pinnacle_analog_config_defaults(&s->analog.cfg);
    pinnacle_picture_defaults(&s->want_picture);

    if (!s->closer) {
        pin_previewer_destroy(s->preview);
        pin_hdv_audio_destroy(s->hdv_audio);
        free(s->mon_buf);
        pthread_mutex_destroy(&s->mtx);
        free(s);
        return PIN_ERR_NOMEM;
    }

    if (!s->is_replay) {
        pinnacle_status_t pst = pinnacle_lock_acquire(resolved, &s->lock);
        if (pst != PINNACLE_OK) {
            pin_closer_destroy(s->closer);
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
        pin_closer_destroy(s->closer);
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

    hand_over_file(s);
    pin_closer_destroy(s->closer); /* before the event queue it reports to goes */
    pin_holdback_free(&s->hold);
    pin_split_free(&s->split);
    pinnacle_lock_release(s->lock);
    if (!s->is_replay)
        pinnacle_close(&s->dev);
    pin_previewer_destroy(s->preview);
    pin_hdv_audio_destroy(s->hdv_audio);
    free(s->mon_buf);
    free(s->hdv_unit_buf);
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
#elif defined(__APPLE__)
    struct statfs sf; /* "msdos" is FAT12/16/32; exFAT is "exfat" (no 4 GiB limit) */
    *fat32 = (statfs(path, &sf) == 0 && strcmp(sf.f_fstypename, "msdos") == 0);
#else
    *fat32 = 0;
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
                                       const pin_capture_opts_t *o, double measured_bps, int *source)
{
    if (pin_fmt_is_hwenc(fmt)) {
        /* the bit rate the quality setting maps to (pin_estimate.h), until measured */
        if (measured_bps > 0) {
            if (source) *source = PIN_EST_SRC_MEASURED;
            return measured_bps;
        }
        if (source) *source = PIN_EST_SRC_DEFAULT;
        return pin_est_hwenc_rate(pin_fmt_is_hevc(fmt), s->width, s->height, s->is_60hz,
                                  o && pin_fmt_clamp_deinterlace(o->deinterlace) == PIN_DEINTERLACE_DOUBLE_RATE,
                                  o ? pin_fmt_clamp_quality(o->video_quality) : PIN_QUALITY_DEFAULT);
    }
    pin_est_kind_t k = kind == PIN_KIND_HDV ? PIN_EST_HDV
                       : kind == PIN_KIND_ANALOG
                             ? (fmt == PIN_FMT_ANALOG_FFV1_MKV ? PIN_EST_ANALOG_FFV1 : PIN_EST_ANALOG_AVI)
                             : PIN_EST_DV;
    double rate = pin_est_rate(k, s->width, s->height, s->is_60hz, learned_ffv1_bph(s), measured_bps, source);
    /* the default and the learned FFV1 rate are the "fast" level's */
    if (k == PIN_EST_ANALOG_FFV1 && measured_bps <= 0)
        rate *= pin_est_ffv1_factor(o ? pin_fmt_clamp_ffv1(o->ffv1_compression) : PIN_FFV1_DEFAULT);
    return rate;
}

/* The kind whose data rate the free-space estimate uses: what is arriving, and before the first
 * frame the input's (a DV / HDV input with the camera off is not analog, its rate is a fifth). */
static pin_kind_t estimate_kind(const pin_session_t *s)
{
    if (s->input == PIN_INPUT_DV && s->stream_kind != PIN_KIND_HDV)
        return PIN_KIND_DV;
    return s->stream_kind;
}

pin_format_t pin_session_format_for(const pin_session_t *s, const pin_capture_opts_t *o)
{
    if (s->input != PIN_INPUT_DV)
        return o->format_analog;
    return s->stream_kind == PIN_KIND_HDV ? o->format_hdv : o->format_dv;
}

pin_status_t pin_session_check_output(pin_session_t *s, const pin_capture_opts_t *o,
                                       pin_output_check_t *out)
{
    if (!s || !o || !out) return PIN_ERR_ARG;
    memset(out, 0, sizeof(*out));
    out->size = sizeof(*out);

    if (pin_path_is_stdout(o->path)) {
        /* standard output: no name, collision or disk to check */
        snprintf(out->first_path, sizeof(out->first_path), "-");
        return PIN_OK;
    }
    if (!pin_naming_validate(o->path, out->message, sizeof(out->message)))
        return PIN_ERR_ARG;
    /* a typo or an unplugged drive: say so now, not after the deck has started to play */
    if (!pin_naming_folder_exists(o->path, out->message, sizeof(out->message)))
        return PIN_ERR_ARG;

    pin_kind_t kind = estimate_kind(s);
    pin_format_t fmt = kind == PIN_KIND_HDV ? o->format_hdv
                       : kind == PIN_KIND_DV ? o->format_dv : o->format_analog;
    char ext[16];
    format_ext(o, fmt, ext, sizeof(ext));

    char base[PIN_PATH_MAX];
    pin_naming_strip_extension(o->path, ext, base, sizeof(base));
    pin_naming_opts_t nopts = { .scene_split = 0, .always_number = o->first_number != 0,
                                .scene_index = o->first_number ? o->first_number : 1, .pass = 1 };
    pin_naming_build(base, &nopts, ext, out->first_path, sizeof(out->first_path));

    char first_match[PIN_PATH_MAX];
    /* numbered naming: the caller already picked an unused number */
    int coll = o->first_number ? 0 : pin_naming_collides(base, ext, first_match, sizeof(first_match));
    if (coll <= 0 && !o->first_number && s->input == PIN_INPUT_DV) {
        /* a tape can change between DV and HDV: the capture then goes on in the other kind's
         * format and extension, under the same base name (format_cut_begin()) */
        pin_format_t other = kind == PIN_KIND_HDV ? o->format_dv : o->format_hdv;
        char other_ext[16];
        format_ext(o, other, other_ext, sizeof(other_ext));
        if (strcmp(other_ext, ext) != 0)
            coll = pin_naming_collides(base, other_ext, first_match, sizeof(first_match));
    }
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
    int free_known = fat32_and_free(dir, &free_bytes, &out->fat32) == 0;
    uint64_t file_limit = test_seam("PIN_TEST_FAT32_LIMIT");
    if (file_limit)
        out->fat32 = 1;
    else if (out->fat32)
        file_limit = PIN_FAT32_FILE_MAX;
    out->free_bytes = free_bytes;
    out->low_space = free_known && pin_output_space_low(free_bytes);

    double bytes_per_s = nominal_bytes_per_second(s, kind, fmt, o, 0, NULL);
    out->minutes_left = bytes_per_s > 0 ? (uint64_t)(free_bytes / bytes_per_s / 60.0) : 0;

    out->message[0] = 0;
    pin_format_t want = pin_session_format_for(s, o);
    if (!pin_session_format_available(want)) {
        pin_fmt_unavailable_text(want, out->message, sizeof(out->message));
        return PIN_ERR_CODEC;
    }
    if (out->collision)
        snprintf(out->message, sizeof(out->message), "%.200s already exists", first_match);
    else if (out->fat32)   /* DV / HDV go on in a new file at the limit, analog stops there */
        pin_stop_file_limit_note(file_limit, kind == PIN_KIND_ANALOG,
                                 bytes_per_s > 0 ? pin_stop_file_cap(file_limit) / bytes_per_s / 60.0 : 0,
                                 out->message, sizeof(out->message));
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
    if (!pin_path_is_stdout(o->path) && !pin_session_format_available(pin_session_format_for(s, o)))
        return PIN_ERR_CODEC; /* no encoder for it here (chk.message says so) */
    if (pin_path_is_stdout(o->path) && pin_stdout_check(NULL, 0) != 0)
        return PIN_ERR_ARG; /* a terminal, or no standard output */
    if (!overwrite && cs == PIN_OK && chk.collision)
        return PIN_ERR_EXISTS;
    pin_cmd_t c = { .kind = PIN_CMD_CAPTURE_START, .capture = *o, .overwrite = overwrite };
    /* A real tape needs a no-signal or time limit to notice the end of a pass;
     * a replay's end of file is the end of the pass. */
    if (!s->is_replay) {
        pin_capture_opts_normalize(&c.capture);
        /* A pass change rewinds the deck: without start_deck (manual capture) there is
         * one pass, and the no-signal / time limit just ends the capture. */
        if (!c.capture.start_deck)
            c.capture.passes = 1;
    }
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

/* The whole snapshot, into a zeroed *out. Caller holds the lock. */
static void status_fill_locked(pin_session_t *s, pin_status_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    out->size = sizeof(*out);
    out->state = visible_state(s);
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
    out->clip_frames_dropped = s->clip_frames_dropped + s->clip_write_dropped;
    {
        /* HDV MOV / MKV count their frames with error themselves: the closed files', the open one's */
        pin_sink_status_t fst;
        memset(&fst, 0, sizeof(fst));
        if (s->sink && s->sink->get_status && s->file_kind == PIN_KIND_HDV &&
            (s->active_format == PIN_FMT_HDV_MOV || s->active_format == PIN_FMT_HDV_MKV))
            s->sink->get_status(s->sink, &fst);
        out->frames_error += s->sinkf_error + fst.frames_error;
        out->frames_dropped += s->sinkf_repeated + fst.frames_repeated;
        out->clip_frames_error += fst.frames_error;
        out->clip_frames_dropped += fst.frames_repeated;
    }
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
    out->stop_no_video = s->stop_no_video;
    out->stop_captured_s = s->stop_captured_s;
    strncpy(out->stop_text, s->stop_text, sizeof(out->stop_text) - 1);
    if (s->stream_kind == PIN_KIND_HDV) {
        out->video_fps_num = s->hdv_fps_num;
        out->video_fps_den = s->hdv_fps_den;
        out->video_interlaced = hdv_interlaced(s);
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
    pin_kind_t hint_kind = estimate_kind(s);
    pin_format_t hint_fmt = PIN_FMT_DV_RAW;
    out->est_seconds_left = -1;   /* unknown until there is a volume and a rate */
    if (s->state == PIN_STATE_CAPTURING && pin_path_is_stdout(s->capture_opts.path)) {
        /* a pipe: no disk to report on */
    } else if (s->state == PIN_STATE_CAPTURING && s->current_file[0]) {
        hint_path = s->current_file;
        hint_fmt = s->active_format;
    } else if (s->have_output_hint && s->output_hint.path[0] && !pin_path_is_stdout(s->output_hint.path)) {
        hint_path = s->output_hint.path;
        hint_fmt = hint_kind == PIN_KIND_HDV ? s->output_hint.format_hdv
                  : hint_kind == PIN_KIND_ANALOG ? s->output_hint.format_analog
                  : s->output_hint.format_dv;
    }
    if (hint_path) {
        char dir[PIN_PATH_MAX];
        dir_of(hint_path, dir, sizeof(dir));
        uint64_t free_bytes = 0;
        int free_known;
        /* Status is polled 40 times a second by the GUIs and statvfs/statfs are real syscalls (visible in
         * the macOS GUI's profile): ask again after a second, or when the directory changed. */
        double now_s = pin_previewer_clock();
        if (strcmp(s->free_cache_dir, dir) == 0 && s->free_cache_t > 0 && now_s - s->free_cache_t < 1.0) {
            free_bytes = s->free_cache_bytes;
            free_known = s->free_cache_known;
        } else {
            int fat32_unused = 0;
            free_known = fat32_and_free(dir, &free_bytes, &fat32_unused) == 0;
            snprintf(s->free_cache_dir, sizeof(s->free_cache_dir), "%s", dir);
            s->free_cache_bytes = free_bytes;
            s->free_cache_known = free_known;
            s->free_cache_t = now_s;
        }
        out->disk_free_bytes = free_bytes;
        int src = 0;
        double measured = s->state == PIN_STATE_CAPTURING ? pin_rate_bytes_per_s(&s->rate_win, 10.0) : -1;
        const pin_capture_opts_t *rate_opts = s->state == PIN_STATE_CAPTURING ? &s->capture_opts
                                              : s->have_output_hint ? &s->output_hint : NULL;
        double rate = nominal_bytes_per_second(s, hint_kind, hint_fmt, rate_opts, measured, &src);
        out->est_bytes_per_hour = rate * 3600.0;
        out->est_rate_source = src;
        /* a volume that cannot be asked (a folder not made yet, a network drive gone) has no
         * free space to judge: no estimate and no low-space warning */
        if (free_known) {
            out->est_seconds_left = pin_est_seconds_left(free_bytes, rate);
            out->disk_low = (out->est_seconds_left >= 0 && out->est_seconds_left < 3600.0) ||
                            free_bytes < (50ull << 30);
        }
    }
}

/* Hands a snapshot to a caller that built against an older, shorter struct: only the fields it
 * knows are copied, the rest of its memory is not touched. A size of 0 is a caller that never
 * set one. */
static pin_status_t status_copy_out(pin_status_snapshot_t *out, const pin_status_snapshot_t *full)
{
    uint32_t n = out->size;
    if (n == 0 || n > sizeof(*full))
        n = sizeof(*full);
    if (n < sizeof(uint32_t))
        return PIN_ERR_ABI;
    memcpy(out, full, n);
    out->size = n;
    return PIN_OK;
}

pin_status_t pin_session_get_status(pin_session_t *s, pin_status_snapshot_t *out)
{
    if (!s || !out) return PIN_ERR_ARG;
    pin_status_snapshot_t full;
    pin_session_lock(s);
    status_fill_locked(s, &full);
    pin_session_unlock(s);
    return status_copy_out(out, &full);
}

pin_status_t pin_session_get_capture_end_status(pin_session_t *s, pin_status_snapshot_t *out)
{
    if (!s || !out) return PIN_ERR_ARG;
    pin_session_lock(s);
    int have = s->have_end_status;
    pin_status_snapshot_t full;
    if (have)
        full = s->end_status;
    pin_session_unlock(s);
    return have ? status_copy_out(out, &full) : PIN_ERR_STATE;
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

/* The newest `hold` frames stay in the ring: the preview's jitter buffer
 * shows each picture that much later than it arrived (pin_previewer_delay()),
 * so the sound is held back by the same amount to stay in sync. Both limits
 * above count on top of it. */
int pin_session_monitor_read(pin_session_t *s, int16_t *out, int max_frames)
{
    if (!s || !out || max_frames <= 0) return 0;
    size_t hold = (size_t)(48000 * pin_previewer_delay(s->preview));
    pthread_mutex_lock(&s->mon_mtx);
    size_t max_buffered = (size_t)48000 * PIN_MON_MAX_MS / 1000 + hold;
    size_t target_buffered = (size_t)48000 * PIN_MON_TARGET_MS / 1000 + hold;
    if (s->mon_fill > max_buffered)
        s->mon_fill = target_buffered;
    size_t ready = s->mon_fill > hold ? s->mon_fill - hold : 0;
    int n = (int)(ready < (size_t)max_frames ? ready : (size_t)max_frames);
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
    size_t hold = (size_t)(48000 * pin_previewer_delay(s->preview));
    pthread_mutex_lock(&s->mon_mtx);
    int n = s->mon_fill > hold ? (int)(s->mon_fill - hold) : 0;
    pthread_mutex_unlock(&s->mon_mtx);
    return n;
}
