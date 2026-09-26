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

#include "pinnacle_lock.h"
#include "pin_log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Wire-format status record, private to this file. Shared between
 * processes either via a Windows pagefile-backed mapping or a POSIX file
 * (see below); "version" lets a future field addition refuse to
 * misinterpret an older/newer record instead of reading garbage. */
#define PINNACLE_LOCK_MAGIC   0x504c4b31u /* "PLK1" */
#define PINNACLE_LOCK_VERSION 1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t owner_pid;
    uint32_t state; /* pinnacle_lock_state_t */
    uint32_t guid_hi, guid_lo;
    uint32_t guid_known;
} pinnacle_lock_record_t;

/* Turns a device id ("usb:1-4.2", "replay:/path/to/file", ...) into
 * something safe to embed in a Windows kernel-object name or a POSIX file
 * name: alnum/'.'/'-' pass through, everything else (including '/', '\',
 * ':', which all show up in real ids) becomes '_'. Collisions between two
 * different ids that sanitise to the same string are not a real-world
 * concern here -- ids are short USB port paths or a single replay path. */
static void sanitize_id(const char *id, char *out, size_t out_cap)
{
    size_t j = 0;
    for (size_t i = 0; id[i] != '\0' && j + 1 < out_cap; i++) {
        unsigned char c = (unsigned char)id[i];
        out[j++] = (isalnum(c) || c == '.' || c == '-') ? (char)c : '_';
    }
    out[j] = '\0';
}

#if defined(_WIN32)

#include <windows.h>

struct pinnacle_lock {
    HANDLE mutex;
    HANDLE mapping;
    pinnacle_lock_record_t *view;
};

/* Local buffer size so this file doesn't need to include pinnacle_enum.h
 * just for one constant; kept well above what sanitize_id() will ever need
 * (device ids are capped at PINNACLE_ENUM_ID_MAX = 64 bytes anyway). */
#define PINNACLE_ENUM_ID_MAX_LOCAL 96

static int widen(const char *utf8, wchar_t *out, int out_cap)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, out_cap);
    return n > 0;
}

static void build_names(const char *device_id, wchar_t *mutex_name, int mutex_cap,
                         wchar_t *map_name, int map_cap)
{
    char sanitized[PINNACLE_ENUM_ID_MAX_LOCAL];
    sanitize_id(device_id, sanitized, sizeof(sanitized));

    char mutex_utf8[128], map_utf8[128];
    snprintf(mutex_utf8, sizeof(mutex_utf8), "Local\\PinnacleOSS-dev-%s", sanitized);
    snprintf(map_utf8, sizeof(map_utf8), "Local\\PinnacleOSS-rec-%s", sanitized);

    widen(mutex_utf8, mutex_name, mutex_cap);
    widen(map_utf8, map_name, map_cap);
}

pinnacle_status_t pinnacle_lock_acquire(const char *device_id, pinnacle_lock_t **out)
{
    wchar_t mutex_name[160], map_name[160];
    build_names(device_id, mutex_name, 160, map_name, 160);

    HANDLE mutex = CreateMutexW(NULL, FALSE, mutex_name);
    if (!mutex) {
        pin_logf(PIN_LOG_ERROR, "pinnacle_lock: CreateMutexW failed (%lu)\n", GetLastError());
        return PINNACLE_ERR_LOCK;
    }

    DWORD wait = WaitForSingleObject(mutex, 0);
    if (wait == WAIT_TIMEOUT) {
        CloseHandle(mutex);
        return PINNACLE_ERR_BUSY;
    }
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        pin_logf(PIN_LOG_ERROR, "pinnacle_lock: WaitForSingleObject failed (%lu)\n", GetLastError());
        CloseHandle(mutex);
        return PINNACLE_ERR_LOCK;
    }
    /* WAIT_ABANDONED: the previous owner crashed while holding the mutex.
     * We still now own it -- nothing further to do here, which is exactly
     * the "no stale state survives a crash" requirement: falling through
     * to (re)initialise the record below overwrites whatever the dead
     * owner last wrote. */

    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                        0, (DWORD)sizeof(pinnacle_lock_record_t), map_name);
    if (!mapping) {
        pin_logf(PIN_LOG_ERROR, "pinnacle_lock: CreateFileMappingW failed (%lu)\n", GetLastError());
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return PINNACLE_ERR_LOCK;
    }

    pinnacle_lock_record_t *view =
        (pinnacle_lock_record_t *)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                                 sizeof(pinnacle_lock_record_t));
    if (!view) {
        pin_logf(PIN_LOG_ERROR, "pinnacle_lock: MapViewOfFile failed (%lu)\n", GetLastError());
        CloseHandle(mapping);
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return PINNACLE_ERR_LOCK;
    }

    /* We hold the mutex, so we're the only writer right now, whether or not
     * the mapping already existed (if it did, either we abandoned-acquired
     * -- meaning that data is from a dead process -- or it's leftover from
     * our own previous instance; either way it's ours to overwrite). */
    view->magic = PINNACLE_LOCK_MAGIC;
    view->version = PINNACLE_LOCK_VERSION;
    view->owner_pid = (uint32_t)GetCurrentProcessId();
    view->state = (uint32_t)PINNACLE_LOCK_PREPARING;
    view->guid_hi = 0;
    view->guid_lo = 0;
    view->guid_known = 0;

    pinnacle_lock_t *lock = (pinnacle_lock_t *)calloc(1, sizeof(*lock));
    if (!lock) {
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return PINNACLE_ERR_LOCK;
    }
    lock->mutex = mutex;
    lock->mapping = mapping;
    lock->view = view;
    *out = lock;
    return PINNACLE_OK;
}

pinnacle_status_t pinnacle_lock_update(pinnacle_lock_t *lock, pinnacle_lock_state_t state,
                                       uint32_t guid_hi, uint32_t guid_lo)
{
    if (!lock || !lock->view)
        return PINNACLE_ERR_LOCK;
    /* We hold the mutex for the lifetime of *lock, so we're the only
     * writer; a reader on another process may observe a torn update for at
     * most a few stores, which is fine for a status display (see
     * pinnacle_lock_query()'s doc comment on the POSIX side for the same
     * trade-off spelled out in more detail). */
    lock->view->state = (uint32_t)state;
    if (guid_hi != 0 || guid_lo != 0) {
        lock->view->guid_hi = guid_hi;
        lock->view->guid_lo = guid_lo;
        lock->view->guid_known = 1;
    }
    return PINNACLE_OK;
}

void pinnacle_lock_release(pinnacle_lock_t *lock)
{
    if (!lock)
        return;
    if (lock->view)
        UnmapViewOfFile(lock->view);
    if (lock->mapping)
        CloseHandle(lock->mapping);
    if (lock->mutex) {
        ReleaseMutex(lock->mutex);
        CloseHandle(lock->mutex);
    }
    /* Both kernel objects are now closed on our end; since we're always the
     * only handle holder outside of a query's brief peek (which closes its
     * own handles immediately, see below), they're destroyed right here --
     * nothing left for a crash to leave behind, because there was nothing
     * left even for a clean exit. */
    free(lock);
}

typedef struct {
    HANDLE mutex;
    int held;
} query_probe_ctx_t;

static DWORD WINAPI query_probe_thread(LPVOID arg)
{
    query_probe_ctx_t *ctx = (query_probe_ctx_t *)arg;
    DWORD wait = WaitForSingleObject(ctx->mutex, 0);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        ReleaseMutex(ctx->mutex);
        ctx->held = 0;
    } else {
        ctx->held = 1; /* WAIT_TIMEOUT: some thread genuinely holds it */
    }
    return 0;
}

/* See the big comment at its call site in pinnacle_lock_query(): this has
 * to run on a thread that has never touched mutex_name, so it's a thread of
 * its own rather than the caller's. Returns 1 if mutex_name is currently
 * held by some thread (in any process, including our own), else 0. */
static int query_probe_mutex_held(const wchar_t *mutex_name)
{
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, mutex_name);
    if (!mutex)
        return 0; /* the mutex itself is gone: definitely not held */

    query_probe_ctx_t ctx = { mutex, 0 };
    HANDLE thread = CreateThread(NULL, 0, query_probe_thread, &ctx, 0, NULL);
    if (thread) {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    } else {
        /* Couldn't even spin up a thread (should be exceedingly rare) --
         * fail safe by reporting held, so a caller doesn't wrongly offer to
         * open a device we couldn't actually check. */
        ctx.held = 1;
    }

    CloseHandle(mutex);
    return ctx.held;
}

pinnacle_status_t pinnacle_lock_query(const char *device_id, pinnacle_lock_info_t *out)
{
    memset(out, 0, sizeof(*out));

    wchar_t mutex_name[160], map_name[160];
    build_names(device_id, mutex_name, 160, map_name, 160);

    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, map_name);
    if (!mapping)
        return PINNACLE_OK; /* never acquired, or the owner is fully gone: not held */

    /* The mapping's existence alone doesn't prove anyone holds it (the
     * kernel keeps it alive as long as our own handle is open, even after
     * we're done reading it below) -- the mutex is the real ownership
     * signal. Try to take it non-blocking: if we can (or it's abandoned),
     * nobody owns it, so release it again immediately and report free.
     *
     * This probe deliberately runs on a throwaway worker thread rather than
     * this one: Windows mutex ownership is per-THREAD, not per-process, so
     * if *this* thread is the one that holds the lock (a process querying
     * its own device -- pin_api.h's future PIN_DEV_OPEN_HERE case), calling
     * WaitForSingleObject on it directly here would "succeed" via the same
     * recursive-ownership rule that lets one thread call
     * WaitForSingleObject on a mutex it already owns any number of times,
     * and then our ReleaseMutex would silently eat one level of our own
     * outer acquire's ownership -- reporting a self-held device as free
     * while quietly weakening the real lock. A fresh thread has never
     * acquired anything, so it blocks (WAIT_TIMEOUT) exactly when some
     * thread -- ours included -- genuinely holds it. */
    int held = query_probe_mutex_held(mutex_name);

    if (held) {
        pinnacle_lock_record_t *view =
            (pinnacle_lock_record_t *)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0,
                                                     sizeof(pinnacle_lock_record_t));
        if (view) {
            if (view->magic == PINNACLE_LOCK_MAGIC && view->version == PINNACLE_LOCK_VERSION) {
                out->held = 1;
                out->owner_pid = view->owner_pid;
                out->state = (pinnacle_lock_state_t)view->state;
                out->guid_hi = view->guid_hi;
                out->guid_lo = view->guid_lo;
                out->guid_known = view->guid_known;
            } else {
                /* Held by something, but the record doesn't parse (a
                 * mismatched version, most likely). Still report held so a
                 * caller doesn't wrongly offer to open the device -- just
                 * without state/guid detail. */
                out->held = 1;
            }
            UnmapViewOfFile(view);
        } else {
            out->held = 1;
        }
    }

    CloseHandle(mapping);
    return PINNACLE_OK;
}

#else /* POSIX */

#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct pinnacle_lock {
    int fd;
    char path[768];
};

/* $XDG_RUNTIME_DIR/pinnacle-oss (Linux: a per-user tmpfs cleared at logout
 * or reboot), else $TMPDIR/pinnacle-oss (macOS: cleared at reboot), else
 * /tmp/pinnacle-oss-<uid> as a last resort. Created mode 0700 and verified
 * (not just created) to be a real directory we own, so a symlink or a
 * world-writable stand-in left by another user in a shared /tmp can't
 * redirect our lock file -- see ensure_private_dir(). */
static int lock_dir(char *out, size_t out_cap)
{
    const char *base = getenv("XDG_RUNTIME_DIR");
    if (!base || !base[0])
        base = getenv("TMPDIR");

    if (base && base[0])
        snprintf(out, out_cap, "%s/pinnacle-oss", base);
    else
        snprintf(out, out_cap, "/tmp/pinnacle-oss-%ld", (long)getuid());
    return 0;
}

static int ensure_private_dir(const char *path)
{
    struct stat st;
    if (mkdir(path, 0700) != 0 && errno != EEXIST) {
        pin_logf(PIN_LOG_ERROR, "pinnacle_lock: mkdir '%s' failed: %s\n", path, strerror(errno));
        return -1;
    }
    /* lstat, not stat: refuse a symlink here outright rather than follow it
     * (the classic /tmp symlink attack -- someone else pre-creates a link
     * pointing at a file we'd then happily open/lock/write). */
    if (lstat(path, &st) != 0) {
        pin_logf(PIN_LOG_ERROR, "pinnacle_lock: lstat '%s' failed: %s\n", path, strerror(errno));
        return -1;
    }
    if (!S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
        pin_logf(PIN_LOG_ERROR,
                "pinnacle_lock: '%s' exists but isn't a directory we own -- refusing to use it "
                "(possible symlink/tmp-race attack; remove it manually if that's wrong)\n", path);
        return -1;
    }
    chmod(path, 0700); /* best-effort; ignore a failure on e.g. a read-only mount */
    return 0;
}

static int lock_path(const char *device_id, char *out, size_t out_cap)
{
    char dir[600];
    lock_dir(dir, sizeof(dir));
    if (ensure_private_dir(dir) != 0)
        return -1;
    char sanitized[96];
    sanitize_id(device_id, sanitized, sizeof(sanitized));
    snprintf(out, out_cap, "%s/%s.lock", dir, sanitized);
    return 0;
}

pinnacle_status_t pinnacle_lock_acquire(const char *device_id, pinnacle_lock_t **out)
{
    char path[768];
    if (lock_path(device_id, path, sizeof(path)) != 0)
        return PINNACLE_ERR_LOCK;

    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        pin_logf(PIN_LOG_ERROR, "pinnacle_lock: open '%s' failed: %s\n", path, strerror(errno));
        return PINNACLE_ERR_LOCK;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int e = errno;
        close(fd);
        if (e == EWOULDBLOCK)
            return PINNACLE_ERR_BUSY;
        pin_logf(PIN_LOG_ERROR, "pinnacle_lock: flock '%s' failed: %s\n", path, strerror(e));
        return PINNACLE_ERR_LOCK;
    }

    pinnacle_lock_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic = PINNACLE_LOCK_MAGIC;
    rec.version = PINNACLE_LOCK_VERSION;
    rec.owner_pid = (uint32_t)getpid();
    rec.state = (uint32_t)PINNACLE_LOCK_PREPARING;

    if (ftruncate(fd, (off_t)sizeof(rec)) != 0 ||
        pwrite(fd, &rec, sizeof(rec), 0) != (ssize_t)sizeof(rec)) {
        pin_logf(PIN_LOG_ERROR, "pinnacle_lock: writing '%s' failed: %s\n", path, strerror(errno));
        flock(fd, LOCK_UN);
        close(fd);
        return PINNACLE_ERR_LOCK;
    }

    pinnacle_lock_t *lock = (pinnacle_lock_t *)calloc(1, sizeof(*lock));
    if (!lock) {
        flock(fd, LOCK_UN);
        close(fd);
        return PINNACLE_ERR_LOCK;
    }
    lock->fd = fd;
    snprintf(lock->path, sizeof(lock->path), "%s", path);
    *out = lock;
    return PINNACLE_OK;
}

pinnacle_status_t pinnacle_lock_update(pinnacle_lock_t *lock, pinnacle_lock_state_t state,
                                       uint32_t guid_hi, uint32_t guid_lo)
{
    if (!lock)
        return PINNACLE_ERR_LOCK;

    pinnacle_lock_record_t rec;
    if (pread(lock->fd, &rec, sizeof(rec), 0) != (ssize_t)sizeof(rec) ||
        rec.magic != PINNACLE_LOCK_MAGIC || rec.version != PINNACLE_LOCK_VERSION) {
        /* Shouldn't happen (we wrote it in acquire()) -- reconstruct rather
         * than fail the update outright. */
        memset(&rec, 0, sizeof(rec));
        rec.magic = PINNACLE_LOCK_MAGIC;
        rec.version = PINNACLE_LOCK_VERSION;
        rec.owner_pid = (uint32_t)getpid();
    }
    rec.state = (uint32_t)state;
    if (guid_hi != 0 || guid_lo != 0) {
        rec.guid_hi = guid_hi;
        rec.guid_lo = guid_lo;
        rec.guid_known = 1;
    }
    if (pwrite(lock->fd, &rec, sizeof(rec), 0) != (ssize_t)sizeof(rec)) {
        pin_logf(PIN_LOG_ERROR, "pinnacle_lock: updating '%s' failed: %s\n", lock->path, strerror(errno));
        return PINNACLE_ERR_LOCK;
    }
    return PINNACLE_OK;
}

void pinnacle_lock_release(pinnacle_lock_t *lock)
{
    if (!lock)
        return;
    /* Clean release: unlink first (so a concurrent query racing us sees
     * either the old file, still validly locked by us, or no file at all --
     * never a half-written one), then drop the lock and close. */
    unlink(lock->path);
    flock(lock->fd, LOCK_UN);
    close(lock->fd);
    free(lock);
}

pinnacle_status_t pinnacle_lock_query(const char *device_id, pinnacle_lock_info_t *out)
{
    memset(out, 0, sizeof(*out));

    char dir[600];
    lock_dir(dir, sizeof(dir));
    /* Don't call ensure_private_dir() here: a query shouldn't fail (or
     * create anything) just because nothing has ever locked this id yet --
     * it should simply report "not held". Only try to open the file; a
     * missing directory means ENOENT below, handled the same way. */
    char sanitized[96];
    sanitize_id(device_id, sanitized, sizeof(sanitized));
    char path[768];
    snprintf(path, sizeof(path), "%s/%s.lock", dir, sanitized);

    int fd = open(path, O_RDWR);
    if (fd < 0)
        return PINNACLE_OK; /* ENOENT (or any other open failure): never locked, or fully cleaned up */

    if (flock(fd, LOCK_SH | LOCK_NB) == 0) {
        /* We got a shared lock, so nobody holds the exclusive one: the
         * owner exited without releasing (crash, kill -9) or the file is
         * simply unused. Stale -- remove it, but only if it's still the
         * very file we opened (compare device+inode) so we don't delete a
         * fresh lock that a new acquire() created at this path in the
         * meantime, between our open() and here. */
        struct stat by_fd, by_path;
        if (fstat(fd, &by_fd) == 0 && lstat(path, &by_path) == 0 &&
            by_fd.st_dev == by_path.st_dev && by_fd.st_ino == by_path.st_ino) {
            unlink(path);
        }
        flock(fd, LOCK_UN);
        close(fd);
        return PINNACLE_OK; /* out->held already 0 */
    }
    if (errno != EWOULDBLOCK) {
        pin_logf(PIN_LOG_WARN, "pinnacle_lock: flock query on '%s' failed: %s\n", path, strerror(errno));
        close(fd);
        return PINNACLE_OK;
    }

    /* Held. Read the record without taking any lock ourselves (we can't --
     * that's the point) -- the owner might be mid-pwrite, so validate magic
     * and version before trusting it rather than risk a torn read
     * corrupting the state a caller displays. Worst case for one poll is a
     * stale-looking read, which self-corrects on the next call. */
    pinnacle_lock_record_t rec;
    if (pread(fd, &rec, sizeof(rec), 0) == (ssize_t)sizeof(rec) &&
        rec.magic == PINNACLE_LOCK_MAGIC && rec.version == PINNACLE_LOCK_VERSION) {
        out->held = 1;
        out->owner_pid = rec.owner_pid;
        out->state = (pinnacle_lock_state_t)rec.state;
        out->guid_hi = rec.guid_hi;
        out->guid_lo = rec.guid_lo;
        out->guid_known = rec.guid_known;
    } else {
        out->held = 1; /* known locked even if the record didn't parse this time */
    }
    close(fd);
    return PINNACLE_OK;
}

#endif /* POSIX */
