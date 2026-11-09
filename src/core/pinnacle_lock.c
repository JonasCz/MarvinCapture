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
/* pinnacle_lock_set_dir(): "" = the platform default. Set once before any lock is taken. */
static char g_lock_dir[512];

int pinnacle_lock_set_dir(const char *dir)
{
    if (!dir || !dir[0]) {
        g_lock_dir[0] = '\0';
        return 0;
    }
    if (strlen(dir) >= sizeof(g_lock_dir))
        return -1;
    snprintf(g_lock_dir, sizeof(g_lock_dir), "%s", dir);
    return 0;
}

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

/* Ownership is the existence of a named event: the owner creates it, and whoever finds it
 * already there is told BUSY. That holds whichever thread opens or closes the session (a
 * mutex belongs to the thread that took it: released from another thread it stays held, and
 * the same thread could take it twice), and it cannot be left behind: the kernel closes the
 * owner's handle when the process dies, and the event goes with its last handle. There is no
 * "abandoned" state to accept.
 *
 * The event is in the Global\ namespace, so a second user logged on at the same time (fast
 * user switching, Remote Desktop) is excluded too. Creating an event there needs no
 * privilege; if it is refused anyway, the per-session Local\ name is used and only processes
 * of the same logon session are excluded (logged). The status record is a pagefile-backed
 * mapping in Local\: creating mappings in Global\ needs SeCreateGlobalPrivilege, which a
 * normal user does not have, so another session's query sees "held" without the details. */
struct pinnacle_lock {
    HANDLE owner;            /* the named event */
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

/* scope: "Global" or "Local" */
static void build_owner_name(const char *device_id, const char *scope, wchar_t *out, int cap)
{
    char sanitized[PINNACLE_ENUM_ID_MAX_LOCAL];
    sanitize_id(device_id, sanitized, sizeof(sanitized));
    char utf8[160];
    snprintf(utf8, sizeof(utf8), "%s\\PinnacleOSS-own-%s", scope, sanitized);
    widen(utf8, out, cap);
}

static void build_map_name(const char *device_id, wchar_t *out, int cap)
{
    char sanitized[PINNACLE_ENUM_ID_MAX_LOCAL];
    sanitize_id(device_id, sanitized, sizeof(sanitized));
    char utf8[160];
    snprintf(utf8, sizeof(utf8), "Local\\PinnacleOSS-rec-%s", sanitized);
    widen(utf8, out, cap);
}

/* 1: we created the event (*out set); 0: it exists already (held); -1: error. */
static int create_owner(const wchar_t *name, HANDLE *out, DWORD *err)
{
    SetLastError(0);
    HANDLE h = CreateEventW(NULL, TRUE, FALSE, name);
    *err = GetLastError();
    if (h && *err != ERROR_ALREADY_EXISTS) {
        *out = h;
        return 1;
    }
    if (h) {
        CloseHandle(h);
        return 0;
    }
    if (*err != ERROR_ACCESS_DENIED)
        return -1;
    /* Denied: either another user's event exists (held), or this token may not create in
     * the namespace at all (the event then cannot be found either). */
    HANDLE o = OpenEventW(SYNCHRONIZE, FALSE, name);
    DWORD oerr = GetLastError();
    if (o) {
        CloseHandle(o);
        return 0;
    }
    return oerr == ERROR_ACCESS_DENIED ? 0 : -1;
}

pinnacle_status_t pinnacle_lock_acquire(const char *device_id, pinnacle_lock_t **out)
{
    wchar_t global_name[160], local_name[160], map_name[160];
    build_owner_name(device_id, "Global", global_name, 160);
    build_owner_name(device_id, "Local", local_name, 160);
    build_map_name(device_id, map_name, 160);

    HANDLE owner = NULL;
    for (int attempt = 0;; attempt++) {
        DWORD err = 0;
        int r = create_owner(global_name, &owner, &err);
        if (r < 0) {
            /* Global\ refused (a restricted token): exclude this logon session at least */
            DWORD gerr = err;
            r = create_owner(local_name, &owner, &err);
            if (r >= 0 && attempt == 0)
                pin_logf(PIN_LOG_WARN, "pinnacle_lock: the global device lock is not available "
                         "(%lu); only this logon session is excluded\n", gerr);
        }
        if (r > 0)
            break;
        if (r < 0) {
            pin_logf(PIN_LOG_ERROR, "pinnacle_lock: CreateEventW failed (%lu)\n", err);
            return PINNACLE_ERR_LOCK;
        }
        /* pinnacle_lock_query() holds the event open for an instant while it looks; that
         * must not read as a device in use */
        if (attempt >= 3)
            return PINNACLE_ERR_BUSY;
        Sleep(2);
    }

    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                        0, (DWORD)sizeof(pinnacle_lock_record_t), map_name);
    if (!mapping) {
        pin_logf(PIN_LOG_ERROR, "pinnacle_lock: CreateFileMappingW failed (%lu)\n", GetLastError());
        CloseHandle(owner);
        return PINNACLE_ERR_LOCK;
    }

    pinnacle_lock_record_t *view =
        (pinnacle_lock_record_t *)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                                 sizeof(pinnacle_lock_record_t));
    if (!view) {
        pin_logf(PIN_LOG_ERROR, "pinnacle_lock: MapViewOfFile failed (%lu)\n", GetLastError());
        CloseHandle(mapping);
        CloseHandle(owner);
        return PINNACLE_ERR_LOCK;
    }

    /* We own the device, so we're the only writer; a mapping that already existed is held
     * open only by a query reading it, and its contents are ours to overwrite. */
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
        CloseHandle(owner);
        return PINNACLE_ERR_LOCK;
    }
    lock->owner = owner;
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
    /* We own the device for the lifetime of *lock, so we're the only
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
    if (lock->view) {
        /* a query that has the mapping open right now must not report us */
        lock->view->magic = 0;
        if (!UnmapViewOfFile(lock->view))
            pin_logf(PIN_LOG_WARN, "pinnacle_lock: UnmapViewOfFile failed (%lu)\n", GetLastError());
    }
    if (lock->mapping && !CloseHandle(lock->mapping))
        pin_logf(PIN_LOG_WARN, "pinnacle_lock: closing the status record failed (%lu)\n",
                 GetLastError());
    /* Any thread may do this: the event is not owned by a thread. */
    if (lock->owner && !CloseHandle(lock->owner))
        pin_logf(PIN_LOG_ERROR, "pinnacle_lock: releasing the device lock failed (%lu)\n",
                 GetLastError());
    free(lock);
}

/* 1 if the process is still running. */
static int process_alive(uint32_t pid)
{
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!h)
        return GetLastError() == ERROR_ACCESS_DENIED;   /* exists, another user's */
    int alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

/* 1 if the owner event exists under name (opened briefly), 0 if not. */
static int owner_exists(const wchar_t *name)
{
    HANDLE h = OpenEventW(SYNCHRONIZE, FALSE, name);
    if (h) {
        CloseHandle(h);
        return 1;
    }
    return GetLastError() == ERROR_ACCESS_DENIED;   /* another user's: it exists */
}

pinnacle_status_t pinnacle_lock_query(const char *device_id, pinnacle_lock_info_t *out)
{
    memset(out, 0, sizeof(*out));

    wchar_t global_name[160], local_name[160], map_name[160];
    build_owner_name(device_id, "Global", global_name, 160);
    build_owner_name(device_id, "Local", local_name, 160);
    build_map_name(device_id, map_name, 160);

    if (!owner_exists(global_name) && !owner_exists(local_name))
        return PINNACLE_OK;   /* never acquired, or the owner is gone: not held */

    /* The event can exist for an instant without an owner (another query has it open), so
     * the record decides when it can be read: a valid one names a live owner. */
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, map_name);
    if (!mapping) {
        out->held = 1;        /* held from another logon session: no details */
        return PINNACLE_OK;
    }
    pinnacle_lock_record_t *view =
        (pinnacle_lock_record_t *)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0,
                                                 sizeof(pinnacle_lock_record_t));
    if (view) {
        pinnacle_lock_record_t rec = *view;
        UnmapViewOfFile(view);
        if (rec.magic == PINNACLE_LOCK_MAGIC && rec.version == PINNACLE_LOCK_VERSION) {
            if (process_alive(rec.owner_pid)) {
                out->held = 1;
                out->owner_pid = rec.owner_pid;
                out->state = (pinnacle_lock_state_t)rec.state;
                out->guid_hi = rec.guid_hi;
                out->guid_lo = rec.guid_lo;
                out->guid_known = rec.guid_known;
            }
        } else if (rec.magic != 0) {
            /* Held by something whose record does not parse (another version): still
             * held, just without state/guid detail. */
            out->held = 1;
        }
    } else {
        out->held = 1;
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

/* pinnacle_lock_set_dir()'s directory if set, else
 * $XDG_RUNTIME_DIR/pinnacle-oss (Linux: a per-user tmpfs cleared at logout
 * or reboot), else $TMPDIR/pinnacle-oss (macOS: cleared at reboot), else
 * /tmp/pinnacle-oss-<uid> as a last resort. Created mode 0700 and verified
 * (not just created) to be a real directory we own, so a symlink or a
 * world-writable stand-in left by another user in a shared /tmp can't
 * redirect our lock file -- see ensure_private_dir(). */
static int lock_dir(char *out, size_t out_cap)
{
    if (g_lock_dir[0]) {
        snprintf(out, out_cap, "%s", g_lock_dir);
        return 0;
    }
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

    int fd = -1;
    for (int attempt = 0;; attempt++) {
        fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) {
            pin_logf(PIN_LOG_ERROR, "pinnacle_lock: open '%s' failed: %s\n", path, strerror(errno));
            return PINNACLE_ERR_LOCK;
        }

        if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
            int e = errno;
            close(fd);
            if (e == EWOULDBLOCK) {
                /* pinnacle_lock_query() holds a shared lock for an instant while it checks
                 * for a stale file; that must not read as a device in use. */
                if (attempt < 3) {
                    usleep(2000);
                    continue;
                }
                return PINNACLE_ERR_BUSY;
            }
            pin_logf(PIN_LOG_ERROR, "pinnacle_lock: flock '%s' failed: %s\n", path, strerror(e));
            return PINNACLE_ERR_LOCK;
        }

        /* The file we locked may have been unlinked meanwhile (a release, or lock_query()
         * removing a stale file) and replaced by another process's: two processes would then
         * each hold a lock on a different inode. Only a lock on the file now at the path counts. */
        struct stat by_fd, by_path;
        if (fstat(fd, &by_fd) == 0 && stat(path, &by_path) == 0 &&
            by_fd.st_dev == by_path.st_dev && by_fd.st_ino == by_path.st_ino)
            break;
        flock(fd, LOCK_UN);
        close(fd);
        if (attempt >= 20) {
            pin_logf(PIN_LOG_ERROR, "pinnacle_lock: '%s' keeps being replaced\n", path);
            return PINNACLE_ERR_LOCK;
        }
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
