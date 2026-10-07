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

/* Parsing of the published VERSION file and version comparison; see pin_update.h. */

#include "pin_update.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- a minimal JSON reader: one object; the string values we know are kept ---- */

typedef struct {
    const char *p, *end;
} jcur_t;

static void skip_ws(jcur_t *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r'))
        c->p++;
}

static int hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char ch = p[i];
        v <<= 4;
        if (ch >= '0' && ch <= '9') v |= (unsigned)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') v |= (unsigned)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') v |= (unsigned)(ch - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

/* Appends code point cp as UTF-8 if it fits (keeps room for the terminator). */
static void put_utf8(char *out, size_t cap, size_t *n, unsigned cp)
{
    char b[4];
    size_t k;
    if (cp < 0x80) {
        b[0] = (char)cp;
        k = 1;
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 0x3F));
        k = 2;
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        k = 3;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
        k = 4;
    }
    if (out && *n + k < cap) {
        memcpy(out + *n, b, k);
        *n += k;
    }
}

/* Reads the string at c->p (the opening quote) into out (NULL: skip it). A value
 * too long for out is cut at a character boundary. 0 / -1. */
static int read_string(jcur_t *c, char *out, size_t cap)
{
    size_t n = 0;
    if (c->p >= c->end || *c->p != '"')
        return -1;
    c->p++;
    while (c->p < c->end) {
        unsigned char ch = (unsigned char)*c->p++;
        if (ch == '"') {
            if (out && cap)
                out[n] = '\0';
            return 0;
        }
        if (ch < 0x20)
            return -1;
        if (ch != '\\') {
            /* raw UTF-8: copy whole sequences only */
            size_t k = ch < 0x80 ? 1 : ch >= 0xF0 ? 4 : ch >= 0xE0 ? 3 : 2;
            if ((size_t)(c->end - (c->p - 1)) < k)
                return -1;
            if (out && n + k < cap) {
                memcpy(out + n, c->p - 1, k);
                n += k;
            }
            c->p += k - 1;
            continue;
        }
        if (c->p >= c->end)
            return -1;
        unsigned cp;
        switch (*c->p++) {
        case '"': cp = '"'; break;
        case '\\': cp = '\\'; break;
        case '/': cp = '/'; break;
        case 'b': cp = '\b'; break;
        case 'f': cp = '\f'; break;
        case 'n': cp = '\n'; break;
        case 'r': cp = '\r'; break;
        case 't': cp = '\t'; break;
        case 'u':
            if (c->end - c->p < 4 || hex4(c->p, &cp) != 0)
                return -1;
            c->p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                unsigned lo;
                if (c->end - c->p >= 6 && c->p[0] == '\\' && c->p[1] == 'u' &&
                    hex4(c->p + 2, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    c->p += 6;
                } else {
                    cp = 0xFFFD;
                }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                cp = 0xFFFD;
            }
            break;
        default:
            return -1;
        }
        put_utf8(out, cap, &n, cp);
    }
    return -1;
}

/* Skips any value (string, number, literal, nested object / array). 0 / -1. */
static int skip_value(jcur_t *c, int depth)
{
    skip_ws(c);
    if (c->p >= c->end || depth > 32)
        return -1;
    char open = *c->p;
    if (open == '"')
        return read_string(c, NULL, 0);
    if (open == '{' || open == '[') {
        char close = open == '{' ? '}' : ']';
        c->p++;
        skip_ws(c);
        if (c->p < c->end && *c->p == close) {
            c->p++;
            return 0;
        }
        for (;;) {
            if (open == '{') {
                skip_ws(c);
                if (read_string(c, NULL, 0) != 0)
                    return -1;
                skip_ws(c);
                if (c->p >= c->end || *c->p++ != ':')
                    return -1;
            }
            if (skip_value(c, depth + 1) != 0)
                return -1;
            skip_ws(c);
            if (c->p >= c->end)
                return -1;
            if (*c->p == ',') {
                c->p++;
                continue;
            }
            if (*c->p == close) {
                c->p++;
                return 0;
            }
            return -1;
        }
    }
    /* number / true / false / null */
    const char *start = c->p;
    while (c->p < c->end && *c->p != ',' && *c->p != '}' && *c->p != ']' && *c->p != ' ' &&
           *c->p != '\t' && *c->p != '\n' && *c->p != '\r')
        c->p++;
    return c->p > start ? 0 : -1;
}

static int is_dotted_number(const char *s)
{
    int digits = 0;
    if (!*s)
        return 0;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9')
            digits++;
        else if (*s == '.' && digits)
            digits = 0;
        else
            return 0;
    }
    return digits > 0;
}

int pin_update_parse(const char *json, size_t len, pin_update_info_t *info)
{
    if (!json || !info)
        return -1;
    jcur_t c = { json, json + len };
    /* a UTF-8 byte order mark is tolerated */
    if (len >= 3 && (unsigned char)json[0] == 0xEF && (unsigned char)json[1] == 0xBB &&
        (unsigned char)json[2] == 0xBF)
        c.p += 3;
    skip_ws(&c);
    if (c.p >= c.end || *c.p++ != '{')
        return -1;
    char version[PIN_NAME_MAX] = "";
    char url[PIN_PATH_MAX] = "";
    char notes[PIN_UPDATE_NOTES_MAX] = "";
    skip_ws(&c);
    if (c.p < c.end && *c.p == '}')
        return -1;
    for (;;) {
        char key[64];
        skip_ws(&c);
        if (read_string(&c, key, sizeof(key)) != 0)
            return -1;
        skip_ws(&c);
        if (c.p >= c.end || *c.p++ != ':')
            return -1;
        skip_ws(&c);
        char *dst = NULL;
        size_t cap = 0;
        if (strcmp(key, "version") == 0) {
            dst = version;
            cap = sizeof(version);
        } else if (strcmp(key, "release_notes") == 0) {
            dst = notes;
            cap = sizeof(notes);
        } else if (strcmp(key, "download_url") == 0) {
            dst = url;
            cap = sizeof(url);
        }
        if (dst && c.p < c.end && *c.p == '"') {
            if (read_string(&c, dst, cap) != 0)
                return -1;
        } else if (skip_value(&c, 0) != 0) {
            return -1;
        }
        skip_ws(&c);
        if (c.p >= c.end)
            return -1;
        if (*c.p == ',') {
            c.p++;
            continue;
        }
        if (*c.p == '}')
            break;
        return -1;
    }
    if (!is_dotted_number(version))
        return -1;
    snprintf(info->latest, sizeof(info->latest), "%s", version);
    snprintf(info->notes, sizeof(info->notes), "%s", notes);
    if (strncmp(url, "https://", 8) == 0 || strncmp(url, "http://", 7) == 0)
        snprintf(info->download_url, sizeof(info->download_url), "%s", url);
    else
        info->download_url[0] = '\0';
    return 0;
}

int pin_update_compare(const char *a, const char *b)
{
    if (!a)
        a = "";
    if (!b)
        b = "";
    while (*a || *b) {
        uint64_t x = 0, y = 0;
        for (; *a && *a != '.'; a++)
            if (*a >= '0' && *a <= '9' && x < UINT64_MAX / 10)
                x = x * 10 + (uint64_t)(*a - '0');
        for (; *b && *b != '.'; b++)
            if (*b >= '0' && *b <= '9' && y < UINT64_MAX / 10)
                y = y * 10 + (uint64_t)(*b - '0');
        if (x != y)
            return x < y ? -1 : 1;
        if (*a == '.')
            a++;
        if (*b == '.')
            b++;
    }
    return 0;
}

void pin_update_format(const pin_update_info_t *info, char *out, size_t cap)
{
    if (!out || !cap)
        return;
    out[0] = '\0';
    if (!info || !info->available)
        return;
    snprintf(out, cap, "MarvinCapture %s is available (you have %s).", info->latest,
             info->current);
}
