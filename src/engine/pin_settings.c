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

#include "pin_settings.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static int ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static char *pin_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

void pin_settings_init(pin_settings_t *s)
{
    memset(s, 0, sizeof(*s));
}

static void free_entries(pin_settings_t *s)
{
    for (size_t i = 0; i < s->count; i++) {
        free(s->entries[i].section);
        free(s->entries[i].key);
        free(s->entries[i].value);
    }
    free(s->entries);
    s->entries = NULL;
    s->count = 0;
    s->capacity = 0;
}

void pin_settings_free(pin_settings_t *s)
{
    free_entries(s);
}

static pin_settings_entry_t *find_entry(const pin_settings_t *s, const char *section,
                                         const char *key)
{
    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->entries[i].section, section) == 0 && strcmp(s->entries[i].key, key) == 0)
            return &s->entries[i];
    }
    return NULL;
}

static void set_entry(pin_settings_t *s, const char *section, const char *key, const char *value)
{
    pin_settings_entry_t *e = find_entry(s, section, key);
    if (e) {
        free(e->value);
        e->value = pin_strdup(value);
        return;
    }
    if (s->count == s->capacity) {
        size_t new_cap = s->capacity ? s->capacity * 2 : 16;
        pin_settings_entry_t *ne = realloc(s->entries, new_cap * sizeof(*ne));
        if (!ne)
            return;
        s->entries = ne;
        s->capacity = new_cap;
    }
    s->entries[s->count].section = pin_strdup(section);
    s->entries[s->count].key = pin_strdup(key);
    s->entries[s->count].value = pin_strdup(value);
    s->count++;
}

/* Trims leading/trailing ASCII whitespace in place, returns the trimmed
 * start (the buffer itself is mutated: a NUL is written after the last
 * non-space character). */
static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        len--;
    }
    return s;
}

int pin_settings_load(pin_settings_t *s, const char *path)
{
    free_entries(s);

    FILE *f = fopen(path, "rb");
    if (!f)
        return 0; /* missing file: empty settings, not an error */

    char section[256] = "";
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char *l = trim(line);
        if (*l == '\0' || *l == ';' || *l == '#')
            continue;
        size_t len = strlen(l);
        if (l[0] == '[' && l[len - 1] == ']') {
            l[len - 1] = '\0';
            snprintf(section, sizeof(section), "%s", l + 1);
            continue;
        }
        char *eq = strchr(l, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = trim(l);
        char *value = trim(eq + 1);
        set_entry(s, section, key, value);
    }
    fclose(f);
    return 0;
}

#ifdef _WIN32
static wchar_t *utf8_to_wide(const char *utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    wchar_t *w = malloc((size_t)n * sizeof(wchar_t));
    if (!w)
        return NULL;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n);
    return w;
}
#endif

static FILE *fopen_utf8(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t *wpath = utf8_to_wide(path);
    wchar_t *wmode = utf8_to_wide(mode);
    FILE *f = NULL;
    if (wpath && wmode)
        f = _wfopen(wpath, wmode);
    free(wpath);
    free(wmode);
    return f;
#else
    return fopen(path, mode);
#endif
}

static int rename_replace_utf8(const char *from, const char *to)
{
#ifdef _WIN32
    wchar_t *wfrom = utf8_to_wide(from);
    wchar_t *wto = utf8_to_wide(to);
    int ok = 0;
    if (wfrom && wto)
        ok = MoveFileExW(wfrom, wto, MOVEFILE_REPLACE_EXISTING) != 0;
    free(wfrom);
    free(wto);
    return ok ? 0 : -1;
#else
    return rename(from, to) == 0 ? 0 : -1;
#endif
}

int pin_settings_save(const pin_settings_t *s, const char *path)
{
    char tmp_path[4096];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp_path))
        return -1;

    FILE *f = fopen_utf8(tmp_path, "wb");
    if (!f)
        return -1;

    /* Sections in first-seen order, keys in insertion order within each. */
    char written_sections[256][256];
    size_t n_written = 0;
    int ok = 1;

    for (size_t i = 0; i < s->count && ok; i++) {
        const char *section = s->entries[i].section;
        int already = 0;
        for (size_t j = 0; j < n_written; j++) {
            if (strcmp(written_sections[j], section) == 0) {
                already = 1;
                break;
            }
        }
        if (already)
            continue;
        if (n_written < 256)
            snprintf(written_sections[n_written++], 256, "%s", section);

        if (fprintf(f, "[%s]\n", section) < 0)
            ok = 0;
        for (size_t j = 0; j < s->count && ok; j++) {
            if (strcmp(s->entries[j].section, section) != 0)
                continue;
            if (fprintf(f, "%s=%s\n", s->entries[j].key, s->entries[j].value) < 0)
                ok = 0;
        }
    }

    if (fclose(f) != 0)
        ok = 0;
    if (!ok) {
        remove(tmp_path);
        return -1;
    }
    if (rename_replace_utf8(tmp_path, path) != 0) {
        remove(tmp_path);
        return -1;
    }
    return 0;
}

const char *pin_settings_get_string(const pin_settings_t *s, const char *section, const char *key,
                                     const char *default_value)
{
    pin_settings_entry_t *e = find_entry(s, section, key);
    return e ? e->value : default_value;
}

int pin_settings_get_int(const pin_settings_t *s, const char *section, const char *key,
                          int default_value)
{
    pin_settings_entry_t *e = find_entry(s, section, key);
    if (!e)
        return default_value;
    char *end;
    long v = strtol(e->value, &end, 10);
    if (end == e->value)
        return default_value;
    return (int)v;
}

double pin_settings_get_double(const pin_settings_t *s, const char *section, const char *key,
                                double default_value)
{
    pin_settings_entry_t *e = find_entry(s, section, key);
    if (!e)
        return default_value;
    char *end;
    double v = strtod(e->value, &end);
    if (end == e->value)
        return default_value;
    return v;
}

int pin_settings_get_bool(const pin_settings_t *s, const char *section, const char *key,
                           int default_value)
{
    pin_settings_entry_t *e = find_entry(s, section, key);
    if (!e)
        return default_value;
    if (strcmp(e->value, "1") == 0 || ieq(e->value, "true") || ieq(e->value, "yes") ||
        ieq(e->value, "on"))
        return 1;
    if (strcmp(e->value, "0") == 0 || ieq(e->value, "false") || ieq(e->value, "no") ||
        ieq(e->value, "off"))
        return 0;
    return default_value;
}

void pin_settings_set_string(pin_settings_t *s, const char *section, const char *key,
                              const char *value)
{
    set_entry(s, section, key, value);
}

void pin_settings_set_int(pin_settings_t *s, const char *section, const char *key, int value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    set_entry(s, section, key, buf);
}

void pin_settings_set_double(pin_settings_t *s, const char *section, const char *key, double value)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%.17g", value);
    set_entry(s, section, key, buf);
}

void pin_settings_set_bool(pin_settings_t *s, const char *section, const char *key, int value)
{
    set_entry(s, section, key, value ? "true" : "false");
}

#ifdef _WIN32
static int ensure_dir(const char *utf8_path)
{
    wchar_t *w = utf8_to_wide(utf8_path);
    if (!w)
        return -1;
    int ok = CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    free(w);
    return ok ? 0 : -1;
}
#else
static int ensure_dir(const char *path)
{
    if (mkdir(path, 0700) == 0)
        return 0;
    return (errno == EEXIST) ? 0 : -1;
}

/* Creates every missing component of path (POSIX/macOS only; the Windows
 * config directory is always exactly one level below %APPDATA%, which
 * already exists, so it only ever needs ensure_dir()). */
static int ensure_dir_recursive(const char *path)
{
    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            ensure_dir(buf);
            *p = '/';
        }
    }
    return ensure_dir(buf);
}
#endif

int pin_settings_default_path(char *out, size_t out_size)
{
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    char buf[2048];
    if (appdata) {
        snprintf(buf, sizeof(buf), "%s\\PinnacleOSS", appdata);
    } else {
        PWSTR wpath = NULL;
        if (SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &wpath) != S_OK)
            return -1;
        char narrow[2048];
        int n = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, narrow, sizeof(narrow), NULL, NULL);
        CoTaskMemFree(wpath);
        if (n <= 0)
            return -1;
        snprintf(buf, sizeof(buf), "%s\\PinnacleOSS", narrow);
    }
    ensure_dir(buf);
    int n = snprintf(out, out_size, "%s\\settings.ini", buf);
    return (n < 0 || (size_t)n >= out_size) ? -1 : 0;
#elif defined(__APPLE__)
    const char *home = getenv("HOME");
    if (!home)
        return -1;
    char buf[2048];
    snprintf(buf, sizeof(buf), "%s/Library/Application Support/PinnacleOSS", home);
    ensure_dir_recursive(buf);
    int n = snprintf(out, out_size, "%s/settings.ini", buf);
    return (n < 0 || (size_t)n >= out_size) ? -1 : 0;
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char buf[2048];
    if (xdg && *xdg) {
        snprintf(buf, sizeof(buf), "%s/pinnacle-oss", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home)
            return -1;
        snprintf(buf, sizeof(buf), "%s/.config/pinnacle-oss", home);
    }
    ensure_dir_recursive(buf);
    int n = snprintf(out, out_size, "%s/settings.ini", buf);
    return (n < 0 || (size_t)n >= out_size) ? -1 : 0;
#endif
}
