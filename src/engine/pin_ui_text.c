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

#include "pin_ui_text.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

/* The core never calls setlocale(), so printf's "%.1f" always uses '.' here. */

void pin_ui_format_bytes(uint64_t bytes, char *out, size_t cap)
{
    static const char *const units[] = { "B", "KB", "MB", "GB", "TB" };
    if (!out || !cap)
        return;
    double size = (double)bytes;
    int unit = 0;
    while (size >= 1024 && unit < 4) {
        size /= 1024;
        unit++;
    }
    if (unit == 0)
        snprintf(out, cap, "%llu B", (unsigned long long)bytes);
    else
        snprintf(out, cap, "%.1f %s", size, units[unit]);
}

void pin_ui_format_time_left(double seconds, char *out, size_t cap)
{
    if (!out || !cap)
        return;
    long s = seconds >= 1 ? (long)seconds : 0; /* also NaN -> 0 */
    if (s >= 3600)
        snprintf(out, cap, "%ld h %02ld min left", s / 3600, (s / 60) % 60);
    else
        snprintf(out, cap, "%ld min left", s / 60);
}

void pin_ui_format_count(uint64_t n, char *out, size_t cap)
{
    if (!out || !cap)
        return;
    char digits[24];
    int len = snprintf(digits, sizeof(digits), "%llu", (unsigned long long)n);
    char grouped[32];
    int o = 0;
    for (int i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0)
            grouped[o++] = ',';
        grouped[o++] = digits[i];
    }
    grouped[o] = 0;
    snprintf(out, cap, "%s", grouped);
}

/* ---- next file number ------------------------------------------------------ */

static int is_sep(char c)
{
    return c == '/' || c == '\\';
}

static int iequal_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    return 1;
}

/* Does entry look like "<name>-<digits>.<ext>" (name compared case-insensitively,
 * ext non-empty without a dot)? Sets *number (ignored on a number that does not
 * fit 32 bits). */
static int match_numbered(const char *entry, const char *name, size_t name_len, uint32_t *number)
{
    if (strlen(entry) <= name_len + 1 || !iequal_n(entry, name, name_len) || entry[name_len] != '-')
        return 0;
    const char *p = entry + name_len + 1;
    uint64_t v = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        if (v <= UINT32_MAX)
            v = v * 10 + (uint64_t)(*p - '0');
        digits++;
        p++;
    }
    if (digits == 0 || *p != '.' || p[1] == 0 || strchr(p + 1, '.'))
        return 0;
    if (v > UINT32_MAX)
        return 0;
    *number = (uint32_t)v;
    return 1;
}

#ifdef _WIN32
static wchar_t *utf8_to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    wchar_t *w = malloc((size_t)n * sizeof(wchar_t));
    if (w)
        MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
#endif

/* Calls visit(file name as UTF-8, ctx) for every entry of dir; returns 0 if the
 * directory can't be read. */
static int scan_dir(const char *dir, void (*visit)(const char *, void *), void *ctx)
{
#ifdef _WIN32
    size_t dlen = strlen(dir);
    char *pat = malloc(dlen + 3);
    if (!pat)
        return 0;
    snprintf(pat, dlen + 3, "%s%s*", dir, (dlen && is_sep(dir[dlen - 1])) ? "" : "\\");
    wchar_t *wpat = utf8_to_wide(pat);
    free(pat);
    if (!wpat)
        return 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    free(wpat);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND; /* an empty directory */
    do {
        char name[1024];
        if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof(name), NULL, NULL) > 0)
            visit(name, ctx);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 1;
#else
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        visit(e->d_name, ctx);
    closedir(d);
    return 1;
#endif
}

typedef struct {
    const char *name;
    size_t name_len;
    uint32_t max;
} scan_ctx_t;

static void scan_visit(const char *entry, void *p)
{
    scan_ctx_t *c = p;
    uint32_t n;
    if (match_numbered(entry, c->name, c->name_len, &n) && n > c->max)
        c->max = n;
}

uint32_t pin_ui_next_file_number(const char *path, const char *const *exts, int n_exts)
{
    if (!path)
        return 1;
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (is_sep(*p))
            base = p + 1;

    /* the directory part: "" -> ".", "/x" -> "/", "C:\\x" -> "C:\\", else up to the last separator */
    char dir[PIN_PATH_MAX];
    size_t dlen = (size_t)(base - path);
    if (dlen == 0)
        snprintf(dir, sizeof(dir), ".");
    else {
        if (dlen > 1 && !(dlen == 3 && path[1] == ':'))
            dlen--;
        if (dlen >= sizeof(dir))
            return 1;
        memcpy(dir, path, dlen);
        dir[dlen] = 0;
    }

    size_t name_len = strlen(base);
    if (name_len == 0)
        return 1;
    const char *dot = strrchr(base, '.');
    if (dot && dot[1]) {
        for (int i = 0; i < n_exts; i++) {
            if (exts[i] && strlen(exts[i]) == strlen(dot + 1) && iequal_n(dot + 1, exts[i], strlen(dot + 1))) {
                name_len = (size_t)(dot - base);
                break;
            }
        }
    }
    if (name_len == 0)
        return 1;

    scan_ctx_t ctx = { base, name_len, 0 };
    if (!scan_dir(dir, scan_visit, &ctx))
        return 1;
    return ctx.max == UINT32_MAX ? ctx.max : ctx.max + 1;
}

/* ---- enable rules ------------------------------------------------------------ */

/* The deck buttons work only while idle and READY, with a camera to talk to and a tape in it. */
static int deck_usable(pin_state_t state, int deck_available, pin_deck_state_t deck)
{
    return state == PIN_STATE_READY && deck_available && deck != PIN_DECK_NO_TAPE;
}

int pin_ui_deck_cmd_allowed(pin_state_t state, int deck_available, pin_deck_state_t deck,
                            pin_deck_cmd_t cmd)
{
    if (!deck_usable(state, deck_available, deck))
        return 0;
    switch (cmd) {
    case PIN_DECK_CMD_PLAY: return deck != PIN_DECK_PLAYING && deck != PIN_DECK_RECORDING;
    case PIN_DECK_CMD_PAUSE: return deck != PIN_DECK_PAUSED;
    case PIN_DECK_CMD_STOP: return deck != PIN_DECK_STOPPED;
    case PIN_DECK_CMD_FF: return deck != PIN_DECK_FAST_FORWARD;
    case PIN_DECK_CMD_REW: return deck != PIN_DECK_REWINDING;
    }
    return 0;
}

int pin_ui_capture_action_allowed(pin_state_t state, int deck_available, pin_capture_action_t action)
{
    int can_stop = state == PIN_STATE_CAPTURING || state == PIN_STATE_REWINDING;
    switch (action) {
    case PIN_CAPTURE_START_MANUAL: return state == PIN_STATE_READY;
    case PIN_CAPTURE_START_AUTO: return state == PIN_STATE_READY && deck_available;
    case PIN_CAPTURE_STOP: return can_stop;
    case PIN_CAPTURE_STOP_TAPE: return can_stop && deck_available;
    }
    return 0;
}
