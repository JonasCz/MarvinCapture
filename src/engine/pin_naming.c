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

#include "pin_naming.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* dirent.h is provided by MSYS2/mingw-w64 (our Windows toolchain) as well
 * as every POSIX system, so this one header covers all three target
 * platforms without an #ifdef _WIN32 fork. A pure-MSVC build (not used by
 * this project) would need FindFirstFile/FindNextFile instead. */
#include <dirent.h>

static int is_sep(char c)
{
    return c == '/' || c == '\\';
}

static int str_iequal(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static int ends_with_iext(const char *name, const char *ext)
{
    size_t nlen = strlen(name), elen = strlen(ext);
    if (nlen < elen + 1)
        return 0;
    if (name[nlen - elen - 1] != '.')
        return 0;
    return str_iequal(name + nlen - elen, ext);
}

static int starts_with(const char *name, const char *prefix)
{
    return strncmp(name, prefix, strlen(prefix)) == 0;
}

#define PIN_NAME_MAX_BYTES 200 /* 255 minus "-pass-99-0001.ext" and some slack */

static int fail(char *reason, size_t size, const char *fmt, int arg)
{
    if (reason && size)
        snprintf(reason, size, fmt, arg, arg);
    return 0;
}

static int is_reserved_device(const char *name)
{
    /* the device name is whatever precedes the first '.' (so "con.txt" counts) */
    char stem[8];
    size_t n = 0;
    while (name[n] && name[n] != '.' && n < sizeof(stem) - 1) {
        stem[n] = (char)toupper((unsigned char)name[n]);
        n++;
    }
    if (name[n] && name[n] != '.')
        return 0; /* longer than any reserved name */
    stem[n] = 0;
    if (!strcmp(stem, "CON") || !strcmp(stem, "PRN") || !strcmp(stem, "AUX") || !strcmp(stem, "NUL"))
        return 1;
    return n == 4 && (!strncmp(stem, "COM", 3) || !strncmp(stem, "LPT", 3)) && isdigit((unsigned char)stem[3]);
}

int pin_naming_validate(const char *path, char *reason, size_t reason_size)
{
    if (!path)
        return fail(reason, reason_size, "No file name", 0);
    const char *name = path;
    for (const char *p = path; *p; p++)
        if (is_sep(*p))
            name = p + 1;

    size_t len = strlen(name);
    if (len == 0)
        return fail(reason, reason_size, "The file name is empty", 0);
    if (len > PIN_NAME_MAX_BYTES)
        return fail(reason, reason_size, "The file name is too long (over %d bytes)", PIN_NAME_MAX_BYTES);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7f)
            return fail(reason, reason_size, "The file name contains a control character", 0);
        if (strchr("<>:\"/\\|?*", c)) {
            if (reason && reason_size)
                snprintf(reason, reason_size, "The file name contains the character '%c', which is not allowed", c);
            return 0;
        }
    }
    if (name[0] == ' ')
        return fail(reason, reason_size, "The file name can't start with a space", 0);
    if (name[len - 1] == ' ')
        return fail(reason, reason_size, "The file name can't end with a space", 0);
    if (name[0] == '.')
        return fail(reason, reason_size, "The file name can't start with a dot", 0);
    if (name[len - 1] == '.')
        return fail(reason, reason_size, "The file name can't end with a dot", 0);
    if (is_reserved_device(name)) {
        if (reason && reason_size)
            snprintf(reason, reason_size, "\"%.8s\" is a reserved name and can't be used for a file", name);
        return 0;
    }
    return 1;
}

int pin_naming_strip_extension(const char *base_path, const char *known_ext, char *out,
                                size_t out_size)
{
    if (!base_path || !known_ext || !out || out_size == 0)
        return -1;

    size_t len = strlen(base_path);
    size_t elen = strlen(known_ext);
    size_t copy_len = len;

    if (len > elen + 1 && base_path[len - elen - 1] == '.') {
        /* Only strip it if it's actually the last path component's
         * extension, not e.g. a directory named "foo.dv" earlier in the
         * path with no extension on the filename itself. */
        const char *last_sep = NULL;
        for (size_t i = 0; i < len; i++)
            if (is_sep(base_path[i]))
                last_sep = base_path + i;
        const char *fname = last_sep ? last_sep + 1 : base_path;
        if (ends_with_iext(fname, known_ext))
            copy_len = len - elen - 1;
    }

    if (copy_len + 1 > out_size)
        return -1;
    memcpy(out, base_path, copy_len);
    out[copy_len] = '\0';
    return 0;
}

int pin_naming_build(const char *base_no_ext, const pin_naming_opts_t *opts, const char *ext,
                      char *out, size_t out_size)
{
    if (!base_no_ext || !opts || !ext || !out || out_size == 0)
        return -1;
    if (opts->pass == 0)
        return -1;
    int numbered = opts->scene_split || opts->always_number;
    if (numbered && opts->scene_index == 0)
        return -1;

    int n;
    if (opts->pass == 1 && !numbered) {
        n = snprintf(out, out_size, "%s.%s", base_no_ext, ext);
    } else if (opts->pass == 1 && numbered) {
        n = snprintf(out, out_size, "%s-%04u.%s", base_no_ext, opts->scene_index, ext);
    } else if (opts->pass != 1 && !numbered) {
        n = snprintf(out, out_size, "%s-pass-%u.%s", base_no_ext, opts->pass, ext);
    } else {
        n = snprintf(out, out_size, "%s-pass-%u-%04u.%s", base_no_ext, opts->pass,
                     opts->scene_index, ext);
    }
    if (n < 0 || (size_t)n >= out_size)
        return -1;
    return 0;
}

static void split_dir_and_base(const char *base_no_ext, char *dir, size_t dir_size,
                                const char **base_name)
{
    const char *last_sep = NULL;
    for (const char *p = base_no_ext; *p; p++)
        if (is_sep(*p))
            last_sep = p;

    if (!last_sep) {
        dir[0] = '.';
        dir[1] = '\0';
        *base_name = base_no_ext;
        return;
    }

    size_t dlen = (size_t)(last_sep - base_no_ext);
    if (dlen == 0)
        dlen = 1; /* root "/" */
    if (dlen >= dir_size)
        dlen = dir_size - 1;
    memcpy(dir, base_no_ext, dlen);
    dir[dlen] = '\0';
    *base_name = last_sep + 1;
    (void)dir_size;
}

int pin_naming_collides(const char *base_no_ext, const char *ext, char *first_match,
                         size_t first_match_size)
{
    if (!base_no_ext || !ext)
        return -1;

    char dir[4096];
    const char *base_name;
    split_dir_and_base(base_no_ext, dir, sizeof(dir), &base_name);

    DIR *d = opendir(dir);
    if (!d)
        return -1;

    char prefix_dash[512];
    snprintf(prefix_dash, sizeof(prefix_dash), "%s-", base_name);

    int found = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        int match = 0;

        /* Exact "base.ext"? */
        {
            char candidate[600];
            snprintf(candidate, sizeof(candidate), "%s.%s", base_name, ext);
            if (str_iequal(name, candidate))
                match = 1;
        }
        /* "base-...ext" (covers -NNNN, -pass-N, -pass-N-NNNN)? */
        if (!match && starts_with(name, prefix_dash) && ends_with_iext(name, ext))
            match = 1;

        if (match) {
            found = 1;
            if (first_match && first_match_size > 0) {
                snprintf(first_match, first_match_size, "%s%c%s",
                         dir, (strchr(dir, '\\') ? '\\' : '/'), name);
            }
            break;
        }
    }
    closedir(d);
    return found;
}
