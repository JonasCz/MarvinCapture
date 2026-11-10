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
#include "../core/pin_fs.h"
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int is_sep(char c)
{
    return c == '/' || c == '\\';
}

/* ---- Name comparison ---------------------------------------------------------------------
 * A name is compared as a key: its code points, simple-case-folded, with the precomposed Latin
 * letters (U+00C0..U+017F) decomposed into base letter + combining mark. Case-insensitive
 * file systems (NTFS, APFS, HFS+, exFAT) treat "ÉCRAN" and "écran" as one name, and APFS and
 * HFS+ also treat the NFC and NFD spellings of "Café" as one; NTFS keeps those two apart, so
 * there a match only costs an "overwrite?" question that was not needed. Bytes that are not
 * valid UTF-8 compare as themselves. */

#define KEY_MAX 1100   /* code points of a key: a name of up to 1024 bytes, decomposed */

/* Decompositions of U+00C0..U+017F: base letter and mark, "  " for none. Marks: ` grave,
 * ' acute, ^ circumflex, ~ tilde, - macron, u breve, . dot above, : diaeresis, o ring above,
 * " double acute, v caron, , cedilla, ; ogonek. */
static const char decomp_tab[] =
    /* C0 */ "A`A'A^A~A:Ao  C,E`E'E^E:I`I'I^I:"
    /* D0 */ "  N~O`O'O^O~O:    U`U'U^U:Y'    "
    /* E0 */ "a`a'a^a~a:ao  c,e`e'e^e:i`i'i^i:"
    /* F0 */ "  n~o`o'o^o~o:    u`u'u^u:y'  y:"
    /* 100 */ "A-a-AuauA;a;C'c'C^c^C.c.CvcvDvdv"
    /* 110 */ "    E-e-EueuE.e.E;e;EvevG^g^Gugu"
    /* 120 */ "G.g.G,g,H^h^    I~i~I-i-IuiuI;i;"
    /* 130 */ "I.      J^j^K,k,  L'l'L,l,Lvlv  "
    /* 140 */ "      N'n'N,n,Nvnv      O-o-Ouou"
    /* 150 */ "O\"o\"    R'r'R,r,RvrvS's'S^s^S,s,"
    /* 160 */ "SvsvT,t,Tvtv    U~u~U-u-UuuuUouo"
    /* 170 */ "U\"u\"U;u;W^w^Y^y^Y:Z'z'Z.z.Zvzv  ";

static uint32_t mark_cp(char m)
{
    switch (m) {
    case '`': return 0x300;
    case '\'': return 0x301;
    case '^': return 0x302;
    case '~': return 0x303;
    case '-': return 0x304;
    case 'u': return 0x306;
    case '.': return 0x307;
    case ':': return 0x308;
    case 'o': return 0x30A;
    case '"': return 0x30B;
    case 'v': return 0x30C;
    case ',': return 0x327;
    case ';': return 0x328;
    }
    return 0;
}

/* Simple case folding of the scripts a file name is likely to use. */
static uint32_t fold_cp(uint32_t c)
{
    if (c >= 'A' && c <= 'Z')
        return c + 32;
    if (c < 0x80)
        return c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7)
        return c + 32;
    if (c == 0x178)
        return 0xFF;
    if (((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) && !(c & 1))
        return c + 1;
    if (((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) && (c & 1))
        return c + 1;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2)
        return c + 32;                 /* Greek capitals */
    if (c == 0x386)
        return 0x3AC;                  /* Greek capitals with tonos */
    if (c >= 0x388 && c <= 0x38A)
        return c + 37;
    if (c == 0x38C)
        return 0x3CC;
    if (c == 0x38E || c == 0x38F)
        return c + 63;
    if (c == 0x3C2)
        return 0x3C3;                  /* final sigma */
    if (c >= 0x410 && c <= 0x42F)
        return c + 32;                 /* Cyrillic capitals */
    if (c >= 0x400 && c <= 0x40F)
        return c + 80;                 /* Cyrillic capitals with marks */
    return c;
}

/* Decodes one UTF-8 sequence at *s (advancing it); an invalid byte comes back as 0xDC00 + the
 * byte, which no valid sequence decodes to. */
static uint32_t next_cp(const unsigned char **s)
{
    const unsigned char *p = *s;
    unsigned char c = *p;
    uint32_t cp;
    unsigned extra;
    if (c < 0x80) {
        *s = p + 1;
        return c;
    }
    if (c >= 0xC2 && c <= 0xDF) { cp = c & 0x1F; extra = 1; }
    else if (c >= 0xE0 && c <= 0xEF) { cp = c & 0x0F; extra = 2; }
    else if (c >= 0xF0 && c <= 0xF4) { cp = c & 0x07; extra = 3; }
    else { *s = p + 1; return 0xDC00 + c; }
    for (unsigned i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *s = p + 1;
            return 0xDC00 + c;
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if ((extra == 2 && cp < 0x800) || (extra == 3 && (cp < 0x10000 || cp > 0x10FFFF)) ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
        *s = p + 1;
        return 0xDC00 + c;
    }
    *s = p + 1 + extra;
    return cp;
}

/* The comparison key of s (see above). Returns its length; a longer name is cut at cap, which
 * no name we write comes near. */
static size_t name_key(const char *s, uint32_t *out, size_t cap)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t n = 0;
    while (*p && n + 2 <= cap) {
        uint32_t c = next_cp(&p);
        if (c >= 0xC0 && c <= 0x17F && decomp_tab[2 * (c - 0xC0)] != ' ') {
            out[n++] = fold_cp((unsigned char)decomp_tab[2 * (c - 0xC0)]);
            out[n++] = mark_cp(decomp_tab[2 * (c - 0xC0) + 1]);
        } else {
            out[n++] = fold_cp(c);
        }
    }
    return n;
}

int pin_naming_same_name(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    uint32_t ka[KEY_MAX], kb[KEY_MAX];
    size_t na = name_key(a, ka, KEY_MAX), nb = name_key(b, kb, KEY_MAX);
    return na == nb && memcmp(ka, kb, na * sizeof(*ka)) == 0;
}

/* ASCII-only folding, for the extension test of pin_naming_strip_extension() (the extensions
 * are ASCII). */
static int fold(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int str_iequal(const char *a, const char *b)
{
    while (*a && *b) {
        if (fold((unsigned char)*a) != fold((unsigned char)*b))
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

int pin_naming_validate_name(const char *name, char *reason, size_t reason_size)
{
    if (!name)
        return fail(reason, reason_size, "No file name", 0);
    /* Checked before the shared rules: those would accept "sub\\clip" (they look at the last
     * component only) and report a drive prefix as a plain ':' problem. */
    if (((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z')) && name[1] == ':')
        return fail(reason, reason_size,
                    "The file name can't start with a drive; choose the folder separately", 0);
    for (const char *p = name; *p; p++)
        if (is_sep(*p))
            return fail(reason, reason_size,
                        "The file name can't contain a folder (the characters '/' and '\\'); choose the folder separately", 0);
    return pin_naming_validate(name, reason, reason_size);
}

int pin_naming_folder_exists(const char *path, char *reason, size_t reason_size)
{
    if (!path)
        return 0;
    char dir[4096];
    const char *sep = NULL;
    for (const char *p = path; *p; p++)
        if (is_sep(*p))
            sep = p;
    /* no folder part, the root, or a bare drive (C:\x): nothing to look up */
    if (!sep || sep == path || sep[-1] == ':')
        return 1;
    size_t n = (size_t)(sep - path);
    if (n >= sizeof(dir))
        return 1; /* too long to look at; opening the file reports it */
    memcpy(dir, path, n);
    dir[n] = 0;
    if (pin_fs_is_dir(dir))
        return 1;
    if (reason && reason_size)
        snprintf(reason, reason_size, "The folder %.300s does not exist", dir);
    return 0;
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
    if (dlen == 0 || (dlen == 2 && base_no_ext[1] == ':'))
        dlen++; /* the root: "/" or "C:\" (not the drive's current directory) */
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

    pin_fs_dir_t *d = pin_fs_opendir(dir);
    if (!d) {
        /* A folder that is not there holds no file to collide with; any other failure
         * (permissions, I/O) means we cannot tell. */
        return (errno == ENOENT || errno == ENOTDIR) ? 0 : -1;
    }

    /* The keys of "base.ext", "base-" and ".ext" (see name_key()) */
    uint32_t kname[KEY_MAX], kexact[KEY_MAX], kprefix[KEY_MAX], kext[KEY_MAX];
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.%s", base_name, ext);
    size_t nexact = name_key(tmp, kexact, KEY_MAX);
    snprintf(tmp, sizeof(tmp), "%s-", base_name);
    size_t nprefix = name_key(tmp, kprefix, KEY_MAX);
    snprintf(tmp, sizeof(tmp), ".%s", ext);
    size_t next = name_key(tmp, kext, KEY_MAX);

    int found = 0;
    const char *name;
    while ((name = pin_fs_readdir(d)) != NULL) {
        size_t n = name_key(name, kname, KEY_MAX);
        /* "base.ext", or "base-...ext" (covers -NNNN, -pass-N, -pass-N-NNNN) */
        int match = (n == nexact && memcmp(kname, kexact, n * sizeof(*kname)) == 0) ||
                    (n >= nprefix && n >= next &&
                     memcmp(kname, kprefix, nprefix * sizeof(*kname)) == 0 &&
                     memcmp(kname + n - next, kext, next * sizeof(*kname)) == 0);
        if (match) {
            found = 1;
            if (first_match && first_match_size > 0) {
                size_t dl = strlen(dir);
                snprintf(first_match, first_match_size, "%s%s%s", dir,
                         is_sep(dir[dl - 1]) ? "" : (strchr(dir, '\\') ? "\\" : "/"), name);
            }
            break;
        }
    }
    pin_fs_closedir(d);
    return found;
}

int pin_output_space_low(unsigned long long free_bytes)
{
    return free_bytes > 0 && free_bytes < PIN_LOW_SPACE_BYTES;
}
