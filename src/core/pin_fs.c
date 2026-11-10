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

/* UTF-8 file access (pin_fs.h). Built as its own small library (pinnacle_fs) because both the
 * hardware layer and the hardware-free engine modules use it. */

#include "pin_fs.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

/* ---- portable conversion ------------------------------------------------ */

long pin_fs_utf8_to_utf16(const char *s, uint16_t *out, size_t cap)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t n = 0;
    if (!s || !out || cap == 0)
        return -1;
    while (*p) {
        uint32_t cp;
        unsigned extra;
        unsigned char c = *p;
        if (c < 0x80) {
            cp = c;
            extra = 0;
        } else if (c >= 0xC2 && c <= 0xDF) {
            cp = c & 0x1F;
            extra = 1;
        } else if (c >= 0xE0 && c <= 0xEF) {
            cp = c & 0x0F;
            extra = 2;
        } else if (c >= 0xF0 && c <= 0xF4) {
            cp = c & 0x07;
            extra = 3;
        } else {
            return -1;   /* a continuation byte, an overlong lead (C0, C1) or past U+10FFFF */
        }
        p++;
        for (unsigned i = 0; i < extra; i++, p++) {
            if ((*p & 0xC0) != 0x80)
                return -1;
            cp = (cp << 6) | (*p & 0x3F);
        }
        if ((extra == 2 && cp < 0x800) || (extra == 3 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            return -1;
        if (cp >= 0x10000) {
            if (n + 2 >= cap)
                return -1;
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xD800 | (cp >> 10));
            out[n++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
        } else {
            if (n + 1 >= cap)
                return -1;
            out[n++] = (uint16_t)cp;
        }
    }
    out[n] = 0;
    return (long)n;
}

static int wsep(uint16_t c)
{
    return c == '\\' || c == '/';
}

static size_t wlen(const uint16_t *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

typedef struct {
    uint16_t *out;
    size_t cap, len;
    int full;
} wbuf_t;

static void put(wbuf_t *b, const uint16_t *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        b->full = 1;
        return;
    }
    memcpy(b->out + b->len, s, n * sizeof(*s));
    b->len += n;
}

static void put_ascii(wbuf_t *b, const char *s)
{
    while (*s) {
        uint16_t c = (uint16_t)(unsigned char)*s++;
        put(b, &c, 1);
    }
}

/* Appends in[pos..len) to b as "\comp\comp...", dropping empty and "." components and letting
 * ".." remove the one before it, never above what b already holds (the root). */
static void put_components(wbuf_t *b, const uint16_t *in, size_t pos, size_t len)
{
    size_t root = b->len;
    while (pos < len) {
        while (pos < len && wsep(in[pos]))
            pos++;
        size_t end = pos;
        while (end < len && !wsep(in[end]))
            end++;
        size_t n = end - pos;
        if (n == 2 && in[pos] == '.' && in[pos + 1] == '.') {
            while (b->len > root && b->out[b->len - 1] != '\\')
                b->len--;
            if (b->len > root)
                b->len--;
        } else if (n > 0 && !(n == 1 && in[pos] == '.')) {
            put_ascii(b, "\\");
            put(b, in + pos, n);
        }
        pos = end;
    }
    if (b->len == root)
        put_ascii(b, "\\");   /* the root itself: "\\?\C:\" */
}

long pin_fs_extend_wpath(const uint16_t *in, uint16_t *out, size_t cap)
{
    if (!in || !out || cap == 0)
        return -1;
    size_t len = wlen(in);
    wbuf_t b = { out, cap, 0, 0 };

    int prefixed = len >= 4 && wsep(in[0]) && (wsep(in[1]) || in[1] == '?') &&
                   (in[2] == '?' || in[2] == '.') && wsep(in[3]);
    int drive = len >= 3 && ((in[0] >= 'A' && in[0] <= 'Z') || (in[0] >= 'a' && in[0] <= 'z')) &&
                in[1] == ':' && wsep(in[2]);
    int unc = !prefixed && len >= 2 && wsep(in[0]) && wsep(in[1]);

    if (len < PIN_FS_LONG_PATH || prefixed || !(drive || unc)) {
        put(&b, in, len);
    } else if (drive) {
        put_ascii(&b, "\\\\?\\");
        put(&b, in, 2);
        put_components(&b, in, 3, len);
    } else {
        /* \\server\share: both names are needed, else Windows could not use it either */
        size_t s0 = 2;
        while (s0 < len && wsep(in[s0]))
            s0++;
        size_t s1 = s0;
        while (s1 < len && !wsep(in[s1]))
            s1++;
        size_t h0 = s1;
        while (h0 < len && wsep(in[h0]))
            h0++;
        size_t h1 = h0;
        while (h1 < len && !wsep(in[h1]))
            h1++;
        if (s1 == s0 || h1 == h0) {
            put(&b, in, len);
        } else {
            put_ascii(&b, "\\\\?\\UNC\\");
            put(&b, in + s0, s1 - s0);
            put_ascii(&b, "\\");
            put(&b, in + h0, h1 - h0);
            put_components(&b, in, h1, len);
        }
    }
    if (b.full)
        return -1;
    out[b.len] = 0;
    return (long)b.len;
}

long pin_fs_win_path(const char *utf8, uint16_t *out, size_t cap)
{
    if (!utf8 || !out || cap == 0)
        return -1;
    size_t n = strlen(utf8) + 1;
    uint16_t *w = malloc(n * sizeof(*w));
    if (!w)
        return -1;
    long r = pin_fs_utf8_to_utf16(utf8, w, n) < 0 ? -1 : pin_fs_extend_wpath(w, out, cap);
    free(w);
    return r;
}

#if defined(_WIN32)

/* ---- Windows ------------------------------------------------------------ */

_Static_assert(sizeof(wchar_t) == sizeof(uint16_t), "wchar_t is UTF-16 on Windows");

static void set_errno_from_win32(void)
{
    switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
        errno = ENOENT;
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_WRITE_PROTECT:
        errno = EACCES;
        break;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        errno = EEXIST;
        break;
    case ERROR_DIRECTORY:
        errno = ENOTDIR;
        break;
    case ERROR_INVALID_NAME:
    case ERROR_FILENAME_EXCED_RANGE:
        errno = EINVAL;
        break;
    default:
        errno = EIO;
        break;
    }
}

static int w_absolute(const wchar_t *w)
{
    return (w[0] && w[1] == L':' && (w[2] == L'\\' || w[2] == L'/')) ||
           ((w[0] == L'\\' || w[0] == L'/') && (w[1] == L'\\' || w[1] == L'/' || w[1] == L'?'));
}

wchar_t *pin_fs_wpath(const char *utf8)
{
    if (!utf8 || !*utf8) {
        errno = ENOENT;
        return NULL;
    }
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    if (n <= 0) {
        errno = EINVAL;
        return NULL;
    }
    wchar_t *w = malloc((size_t)n * sizeof(wchar_t));
    if (!w) {
        errno = ENOMEM;
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, w, n);
    if (!w_absolute(w)) {
        /* relative ("clip.dv", "..\x", "\x", "C:x"): the prefix needs a full path. A string
         * operation on the current directory, not bound by MAX_PATH in the wide version. */
        DWORD need = GetFullPathNameW(w, 0, NULL, NULL);
        wchar_t *full = need ? malloc((size_t)need * sizeof(wchar_t)) : NULL;
        DWORD got = full ? GetFullPathNameW(w, need, full, NULL) : 0;
        free(w);
        if (got == 0 || got >= need) {
            free(full);
            errno = EINVAL;
            return NULL;
        }
        w = full;
    }
    size_t cap = wcslen(w) + 9;   /* "\\?\UNC\" in place of "\\", and the terminator */
    wchar_t *out = malloc(cap * sizeof(wchar_t));
    if (!out || pin_fs_extend_wpath((const uint16_t *)w, (uint16_t *)out, cap) < 0) {
        free(w);
        free(out);
        errno = out ? ENAMETOOLONG : ENOMEM;
        return NULL;
    }
    free(w);
    return out;
}

FILE *pin_fs_fopen(const char *path, const char *mode)
{
    wchar_t wmode[16];
    size_t i = 0;
    for (; mode[i] && i + 1 < sizeof(wmode) / sizeof(wmode[0]); i++)
        wmode[i] = (wchar_t)(unsigned char)mode[i];
    wmode[i] = 0;
    wchar_t *w = pin_fs_wpath(path);
    if (!w)
        return NULL;
    FILE *f = _wfopen(w, wmode);
    int e = errno;
    free(w);
    errno = e;
    return f;
}

int pin_fs_remove(const char *path)
{
    wchar_t *w = pin_fs_wpath(path);
    if (!w)
        return -1;
    int ok = DeleteFileW(w) != 0;
    if (!ok)
        set_errno_from_win32();
    free(w);
    return ok ? 0 : -1;
}

int pin_fs_rename(const char *from, const char *to)
{
    wchar_t *wf = pin_fs_wpath(from);
    if (!wf)
        return -1;
    wchar_t *wt = pin_fs_wpath(to);
    if (!wt) {
        free(wf);
        return -1;
    }
    int ok = MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok)
        set_errno_from_win32();
    free(wf);
    free(wt);
    return ok ? 0 : -1;
}

int pin_fs_mkdir(const char *path, int mode)
{
    (void)mode;
    wchar_t *w = pin_fs_wpath(path);
    if (!w)
        return -1;
    int ok = CreateDirectoryW(w, NULL) != 0;
    if (!ok)
        set_errno_from_win32();
    free(w);
    return ok ? 0 : -1;
}

static int attributes(const char *path, WIN32_FILE_ATTRIBUTE_DATA *a)
{
    wchar_t *w = pin_fs_wpath(path);
    if (!w)
        return -1;
    int ok = GetFileAttributesExW(w, GetFileExInfoStandard, a) != 0;
    if (!ok)
        set_errno_from_win32();
    free(w);
    return ok ? 0 : -1;
}

int pin_fs_is_dir(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    return attributes(path, &a) == 0 && (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}

int pin_fs_exists(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    return attributes(path, &a) == 0;
}

long long pin_fs_size(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (attributes(path, &a) != 0 || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return -1;
    return (long long)(((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow);
}

/* FindFirstFileExW rather than mingw's _wopendir, which builds its search path in a MAX_PATH
 * buffer. */
struct pin_fs_dir {
    HANDLE h;
    WIN32_FIND_DATAW fd;
    int pending;            /* fd holds an entry readdir has not returned yet */
    char name[MAX_PATH * 4];
};

pin_fs_dir_t *pin_fs_opendir(const char *path)
{
    wchar_t *w = pin_fs_wpath(path);
    if (!w)
        return NULL;
    size_t n = wcslen(w);
    wchar_t *pat = malloc((n + 3) * sizeof(wchar_t));
    pin_fs_dir_t *d = calloc(1, sizeof(*d));
    if (!pat || !d) {
        free(w);
        free(pat);
        free(d);
        errno = ENOMEM;
        return NULL;
    }
    memcpy(pat, w, n * sizeof(wchar_t));
    if (n && pat[n - 1] != L'\\' && pat[n - 1] != L'/')
        pat[n++] = L'\\';
    pat[n++] = L'*';
    pat[n] = 0;
    free(w);
    d->h = FindFirstFileExW(pat, FindExInfoBasic, &d->fd, FindExSearchNameMatch, NULL, 0);
    free(pat);
    if (d->h == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND)
            return d;   /* an empty drive root: no entries at all */
        set_errno_from_win32();
        free(d);
        return NULL;
    }
    d->pending = 1;
    return d;
}

const char *pin_fs_readdir(pin_fs_dir_t *d)
{
    if (!d || d->h == INVALID_HANDLE_VALUE)
        return NULL;
    for (;;) {
        if (!d->pending && !FindNextFileW(d->h, &d->fd))
            return NULL;
        d->pending = 0;
        if (WideCharToMultiByte(CP_UTF8, 0, d->fd.cFileName, -1, d->name, (int)sizeof(d->name),
                                NULL, NULL) > 0)
            return d->name;
    }
}

void pin_fs_closedir(pin_fs_dir_t *d)
{
    if (!d)
        return;
    if (d->h != INVALID_HANDLE_VALUE)
        FindClose(d->h);
    free(d);
}

#else

/* ---- POSIX: the plain calls ---------------------------------------------- */

FILE *pin_fs_fopen(const char *path, const char *mode)
{
    return fopen(path, mode);
}

int pin_fs_remove(const char *path)
{
    return remove(path) == 0 ? 0 : -1;
}

int pin_fs_rename(const char *from, const char *to)
{
    return rename(from, to) == 0 ? 0 : -1;
}

int pin_fs_mkdir(const char *path, int mode)
{
    return mkdir(path, (mode_t)mode) == 0 ? 0 : -1;
}

int pin_fs_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int pin_fs_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

long long pin_fs_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode))
        return -1;
    return (long long)st.st_size;
}

struct pin_fs_dir {
    DIR *d;
};

pin_fs_dir_t *pin_fs_opendir(const char *path)
{
    DIR *dd = opendir(path);
    if (!dd)
        return NULL;
    pin_fs_dir_t *d = malloc(sizeof(*d));
    if (!d) {
        closedir(dd);
        errno = ENOMEM;
        return NULL;
    }
    d->d = dd;
    return d;
}

const char *pin_fs_readdir(pin_fs_dir_t *d)
{
    struct dirent *e = d ? readdir(d->d) : NULL;
    return e ? e->d_name : NULL;
}

void pin_fs_closedir(pin_fs_dir_t *d)
{
    if (!d)
        return;
    closedir(d->d);
    free(d);
}

#endif
