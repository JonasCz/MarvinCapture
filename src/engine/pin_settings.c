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
#include <io.h>
#include <windows.h>
#include <shlobj.h>
#else
#include <fcntl.h>
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

/* Reads one line of any length into *buf (grown as needed). Returns its length, or -1 at EOF.
 * A line past MAX_LINE is consumed and dropped whole (*dropped = 1) so that its tail is never
 * parsed as an entry of its own. */
#define MAX_LINE (1u << 20)

static long read_line(FILE *f, char **buf, size_t *cap, int *dropped)
{
    size_t n = 0;
    int c;
    *dropped = 0;
    while ((c = getc(f)) != EOF && c != '\n') {
        if (n + 1 >= MAX_LINE) {
            *dropped = 1;
            continue;
        }
        if (n + 2 > *cap) {
            size_t nc = *cap ? *cap * 2 : 256;
            char *nb = realloc(*buf, nc);
            if (!nb) {
                *dropped = 1;
                continue;
            }
            *buf = nb;
            *cap = nc;
        }
        (*buf)[n++] = (char)c;
    }
    if (c == EOF && n == 0 && !*dropped)
        return -1;
    if (*buf)
        (*buf)[n] = '\0';
    return (long)n;
}

/* True if p is blank or starts a ; / # comment: what may follow a [Section] header. */
static int rest_is_comment(const char *p)
{
    while (*p && isspace((unsigned char)*p))
        p++;
    return *p == '\0' || *p == ';' || *p == '#';
}

/* Returns the ']' that closes a header line, or NULL if the line is not a header. */
static char *header_end(char *l)
{
    if (l[0] != '[')
        return NULL;
    char *r = strchr(l, ']');
    if (r && rest_is_comment(r + 1))
        return r;
    /* a section name may itself contain ']': fall back to the last character */
    size_t len = strlen(l);
    return l[len - 1] == ']' ? l + len - 1 : NULL;
}

int pin_settings_load(pin_settings_t *s, const char *path)
{
    free_entries(s);

    FILE *f = fopen_utf8(path, "rb");
    if (!f) {
        /* Only a missing file means "empty settings". Any other failure (EACCES, a Windows
         * sharing violation) must not look like an empty file: the caller would save it back
         * and wipe the real one. */
        return errno == ENOENT ? 0 : -1;
    }

    /* a UTF-8 BOM (Notepad) would otherwise become part of the first section name */
    unsigned char bom[3];
    size_t nb = fread(bom, 1, 3, f);
    if (!(nb == 3 && bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF))
        rewind(f);

    char section[256] = "";
    char *line = NULL;
    size_t cap = 0;
    int dropped;
    long len;
    while ((len = read_line(f, &line, &cap, &dropped)) >= 0) {
        if (dropped || !line)
            continue;
        char *l = trim(line);
        if (*l == '\0' || *l == ';' || *l == '#')
            continue;
        char *he = header_end(l);
        if (he) {
            *he = '\0';
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
    free(line);
    int err = ferror(f);
    fclose(f);
    if (err) {
        free_entries(s);
        return -1;
    }
    return 0;
}

/* Flushes the file's data to disk before it replaces the real one: without it a power loss
 * after the rename can leave a zero-length settings file. */
static int sync_file(FILE *f)
{
    if (fflush(f) != 0)
        return -1;
#ifdef _WIN32
    return _commit(_fileno(f)) == 0 ? 0 : -1;
#else
    return fsync(fileno(f)) == 0 ? 0 : -1;
#endif
}

/* Makes the rename itself durable. Best effort: the data is already safe in either file. */
static void sync_parent_dir(const char *path)
{
#ifndef _WIN32
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash == dir)
        dir[1] = '\0';
    else if (slash)
        *slash = '\0';
    else
        snprintf(dir, sizeof(dir), ".");
    int fd = open(dir, O_RDONLY);
    if (fd >= 0) {
        (void)fsync(fd);
        close(fd);
    }
#else
    (void)path;
#endif
}

static unsigned next_tmp_serial(void)
{
    static unsigned serial;
#ifdef _WIN32
    return (unsigned)InterlockedIncrement((volatile LONG *)&serial);
#else
    return __atomic_add_fetch(&serial, 1, __ATOMIC_RELAXED);
#endif
}

int pin_settings_save(const pin_settings_t *s, const char *path)
{
    /* A name of its own per process and call: the CLI and a GUI (or two threads) saving at the
     * same time must not write into one temp file. */
    char tmp_path[4096];
#ifdef _WIN32
    unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
    unsigned long pid = (unsigned long)getpid();
#endif
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%lu.%u", path, pid, next_tmp_serial());
    if (n < 0 || (size_t)n >= sizeof(tmp_path))
        return -1;

    FILE *f = fopen_utf8(tmp_path, "wb");
    if (!f)
        return -1;

    /* Sections in first-seen order, keys in insertion order within each. */
    int ok = 1;

    for (size_t i = 0; i < s->count && ok; i++) {
        const char *section = s->entries[i].section;
        int already = 0;
        for (size_t j = 0; j < i; j++) {
            if (strcmp(s->entries[j].section, section) == 0) {
                already = 1;
                break;
            }
        }
        if (already)
            continue;

        if (fprintf(f, "[%s]\n", section) < 0)
            ok = 0;
        for (size_t j = 0; j < s->count && ok; j++) {
            if (strcmp(s->entries[j].section, section) != 0)
                continue;
            if (fprintf(f, "%s=%s\n", s->entries[j].key, s->entries[j].value) < 0)
                ok = 0;
        }
    }

    if (ok && sync_file(f) != 0)
        ok = 0;
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
    sync_parent_dir(path);
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

int pin_settings_device_key(const char *serial, const char *id, const char *key, char *out,
                            size_t out_size)
{
    const char *src = (serial && *serial) ? serial : id;
    if (!src || !*src || !out || out_size < 8)
        return -1;
    int upper = (serial && *serial) ? 1 : 0;
    size_t n = (size_t)snprintf(out, out_size, "dev_");
    for (; *src; src++) {
        unsigned char ch = (unsigned char)*src;
        char o = (isalnum(ch) || ch == '-' || ch == '_') ? (char)(upper ? toupper(ch) : ch) : '_';
        if (n + 2 >= out_size)
            return -1;
        out[n++] = o;
    }
    out[n++] = '.';
    out[n] = '\0';
    if (key && *key) {
        if (n + strlen(key) + 1 > out_size)
            return -1;
        memcpy(out + n, key, strlen(key) + 1);
    }
    return 0;
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
#endif

/* Creates every missing component of path (on Windows with either separator: a directory
 * from pin_set_app_dirs() may be several levels below one that exists). */
static int ensure_dir_recursive(const char *path)
{
    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
#ifdef _WIN32
        if (*p == '/' || *p == '\\') {
#else
        if (*p == '/') {
#endif
            char sep = *p;
            *p = '\0';
            ensure_dir(buf);
            *p = sep;
        }
    }
    return ensure_dir(buf);
}

/* pin_settings_set_dir(): "" = the platform default. */
static char g_settings_dir[1024];

int pin_settings_set_dir(const char *dir)
{
    if (!dir || !dir[0]) {
        g_settings_dir[0] = '\0';
        return 0;
    }
    if (strlen(dir) >= sizeof(g_settings_dir))
        return -1;
    snprintf(g_settings_dir, sizeof(g_settings_dir), "%s", dir);
    return 0;
}

int pin_settings_default_path(char *out, size_t out_size)
{
    if (g_settings_dir[0]) {
        ensure_dir_recursive(g_settings_dir);
#ifdef _WIN32
        int n = snprintf(out, out_size, "%s\\settings.ini", g_settings_dir);
#else
        int n = snprintf(out, out_size, "%s/settings.ini", g_settings_dir);
#endif
        return (n < 0 || (size_t)n >= out_size) ? -1 : 0;
    }
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
