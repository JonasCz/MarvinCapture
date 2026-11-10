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
 * File access on UTF-8 paths. Every path in the core is UTF-8 on every platform. On Windows
 * the narrow CRT calls (fopen, stat, remove, ...) read a path in the ANSI code page, which is
 * cp1252 or similar unless the user turned on the "UTF-8 for worldwide language support" beta
 * switch, so "Café" would become "CafÃ©". These wrappers convert the path to UTF-16 and use the
 * wide CRT / Win32 calls instead, so the result does not depend on that switch. A full path of
 * PIN_FS_LONG_PATH or more UTF-16 units gets the \\?\ (or \\?\UNC\) prefix, so long paths work
 * whether or not the OS "LongPathsEnabled" switch is on (CreateDirectoryW stops at 248 even
 * with it). On POSIX they are the plain calls.
 *
 * Errors: the functions that return -1 / NULL set errno (ENOENT, EACCES, EEXIST, ENOTDIR, EINVAL
 * for a path that is not valid UTF-8, EIO for anything else).
 *
 * FFmpeg (avio_open) is given the plain UTF-8 path: its file protocol converts it to UTF-16 and
 * adds the long-path prefix itself (libavutil/wchar_filename.h).
 */

#ifndef PIN_FS_H
#define PIN_FS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Full paths this long (UTF-16 units, without the terminator) or longer get the \\?\ prefix. */
#define PIN_FS_LONG_PATH 240

FILE *pin_fs_fopen(const char *path, const char *mode);
int pin_fs_remove(const char *path);                    /* a file: 0 or -1 */
int pin_fs_rename(const char *from, const char *to);    /* replaces `to` if it exists: 0 or -1 */
int pin_fs_mkdir(const char *path, int mode);           /* 0, or -1 (errno EEXIST if it exists);
                                                           mode: POSIX permissions, ignored on Windows */
int pin_fs_is_dir(const char *path);                    /* 1 if path is a directory */
int pin_fs_exists(const char *path);                    /* 1 if anything is at path */
long long pin_fs_size(const char *path);                /* a file's size, or -1 */

/* Directory listing: names as UTF-8, "." and ".." included. opendir returns NULL (errno set)
 * if the directory cannot be read; ENOENT / ENOTDIR if it is not there. */
typedef struct pin_fs_dir pin_fs_dir_t;
pin_fs_dir_t *pin_fs_opendir(const char *path);
const char *pin_fs_readdir(pin_fs_dir_t *d);            /* NULL at the end */
void pin_fs_closedir(pin_fs_dir_t *d);

/* ---- The Windows path conversion, as plain functions so it can be tested anywhere. ---- */

/* Strict UTF-8 -> UTF-16 (what MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS) accepts:
 * no overlong forms, no surrogates, nothing past U+10FFFF). Writes the terminator. Returns the
 * number of units without it, or -1 if s is not valid UTF-8 or does not fit in cap units. */
long pin_fs_utf8_to_utf16(const char *s, uint16_t *out, size_t cap);

/* The path Windows should be given for the full (absolute) UTF-16 path `in`: unchanged if it is
 * shorter than PIN_FS_LONG_PATH, already prefixed (\\?\, \??\, \\.\) or not absolute; otherwise
 * '/' turned into '\', repeated separators and "." / ".." components resolved (the prefix turns
 * off that normalisation in Windows) and "\\?\" put in front of "C:\..." or "\\?\UNC\" in place
 * of the "\\" of "\\server\share\...". Returns the length written (without the terminator), or
 * -1 if it does not fit in cap units. */
long pin_fs_extend_wpath(const uint16_t *in, uint16_t *out, size_t cap);

/* Both steps, for a path that is already absolute (the tests; Windows makes a relative path
 * absolute in between with GetFullPathNameW). Returns as pin_fs_extend_wpath(). */
long pin_fs_win_path(const char *utf8, uint16_t *out, size_t cap);

#if defined(_WIN32)
#include <wchar.h>
/* The path to give a wide Win32 / CRT call, malloc'd (free() it), or NULL (errno set). */
wchar_t *pin_fs_wpath(const char *utf8);
#endif

#ifdef __cplusplus
}
#endif

#endif /* PIN_FS_H */
