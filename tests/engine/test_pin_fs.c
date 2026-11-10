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

/* pin_fs: the Windows path conversion (UTF-8 -> UTF-16, the \\?\ prefix) on every platform,
 * and the file calls on non-ASCII names and paths longer than MAX_PATH. */

#include "pin_fs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

/* 1 if the UTF-16 string w equals the ASCII string a */
static int weq(const uint16_t *w, const char *a)
{
    size_t i = 0;
    for (; a[i]; i++)
        if (w[i] != (uint16_t)(unsigned char)a[i])
            return 0;
    return w[i] == 0;
}

static void test_utf8(void)
{
    uint16_t w[64];
    CHECK(pin_fs_utf8_to_utf16("abc", w, 64) == 3 && weq(w, "abc"), "ASCII");
    CHECK(pin_fs_utf8_to_utf16("Caf\xc3\xa9", w, 64) == 4 && w[3] == 0xE9 && w[4] == 0, "2-byte é");
    CHECK(pin_fs_utf8_to_utf16("\xe2\x82\xac", w, 64) == 1 && w[0] == 0x20AC, "3-byte €");
    CHECK(pin_fs_utf8_to_utf16("\xf0\x9f\x98\x80", w, 64) == 2 && w[0] == 0xD83D && w[1] == 0xDE00,
          "4-byte emoji as a surrogate pair");
    CHECK(pin_fs_utf8_to_utf16("", w, 64) == 0 && w[0] == 0, "empty");
    CHECK(pin_fs_utf8_to_utf16("\xc3", w, 64) == -1, "truncated sequence");
    CHECK(pin_fs_utf8_to_utf16("\x80", w, 64) == -1, "lone continuation byte");
    CHECK(pin_fs_utf8_to_utf16("\xc0\xaf", w, 64) == -1, "overlong '/'");
    CHECK(pin_fs_utf8_to_utf16("\xe0\x80\xaf", w, 64) == -1, "overlong 3-byte '/'");
    CHECK(pin_fs_utf8_to_utf16("\xed\xa0\x80", w, 64) == -1, "encoded surrogate");
    CHECK(pin_fs_utf8_to_utf16("\xf4\x90\x80\x80", w, 64) == -1, "past U+10FFFF");
    CHECK(pin_fs_utf8_to_utf16("\xe9t\xe9", w, 64) == -1, "cp1252 bytes are not UTF-8");
    CHECK(pin_fs_utf8_to_utf16("abcd", w, 4) == -1, "no room for the terminator");
    CHECK(pin_fs_utf8_to_utf16("abc", w, 4) == 3, "exactly fits");
    CHECK(pin_fs_utf8_to_utf16("\xf0\x9f\x98\x80", w, 2) == -1, "no room for the pair");
}

/* a path of `len` characters: prefix, then "\\aaaa...\\" components, then the end */
static void make_long(char *out, size_t cap, const char *head, char sep, size_t len)
{
    size_t n = (size_t)snprintf(out, cap, "%s", head);
    int k = 0;
    while (n + 1 < len && n + 1 < cap) {
        out[n++] = (k++ % 20 == 0) ? sep : 'a';
    }
    out[n] = 0;
}

static void test_prefix(void)
{
    static uint16_t w[2048];
    static char p[1024];
    char want[1200];

    /* short: unchanged, even with '/' (Windows normalises those itself) */
    CHECK(pin_fs_win_path("C:/Videos/Caf\xc3\xa9.dv", w, 2048) > 0 && w[2] == '/' && w[13] == 0xE9,
          "a short path is left alone");
    CHECK(pin_fs_win_path("relative\\clip.dv", w, 2048) > 0 && weq(w, "relative\\clip.dv"),
          "a relative path is left alone");

    /* long drive path with '/': prefixed, separators turned */
    make_long(p, sizeof(p), "C:/Videos", '/', 300);
    long n = pin_fs_win_path(p, w, 2048);
    snprintf(want, sizeof(want), "\\\\?\\%s", p);
    for (char *c = want; *c; c++)
        if (*c == '/')
            *c = '\\';
    CHECK(n > 0 && weq(w, want), "a long drive path gets \\\\?\\ and backslashes");

    /* long path with ".", ".." and doubled separators: resolved, since \\?\ turns off that */
    make_long(p, sizeof(p), "D:\\a\\.\\b\\..\\\\c", '\\', 280);
    n = pin_fs_win_path(p, w, 2048);
    snprintf(want, sizeof(want), "\\\\?\\D:\\a\\c%s", p + strlen("D:\\a\\.\\b\\..\\\\c"));
    CHECK(n > 0 && weq(w, want), "\".\", \"..\" and \"\\\\\\\\\" resolved before prefixing");

    /* ".." never climbs above the drive */
    make_long(p, sizeof(p), "E:\\..\\..\\x", '\\', 260);
    n = pin_fs_win_path(p, w, 2048);
    snprintf(want, sizeof(want), "\\\\?\\E:\\x%s", p + strlen("E:\\..\\..\\x"));
    CHECK(n > 0 && weq(w, want), "\"..\" stops at the drive root");

    /* long UNC path: \\server\share -> \\?\UNC\server\share */
    make_long(p, sizeof(p), "\\\\server\\share\\dir", '\\', 301);
    n = pin_fs_win_path(p, w, 2048);
    snprintf(want, sizeof(want), "\\\\?\\UNC\\server\\share\\dir%s", p + strlen("\\\\server\\share\\dir"));
    CHECK(n > 0 && weq(w, want), "a long UNC path gets \\\\?\\UNC\\");
    make_long(p, sizeof(p), "//server/share/dir", '/', 300);
    n = pin_fs_win_path(p, w, 2048);
    for (char *c = want; *c; c++)
        if (*c == '/')
            *c = '\\';
    CHECK(n > 0 && w[0] == '\\' && w[4] == 'U' && w[7] == '\\' && w[8] == 's', "forward-slash UNC");

    /* already prefixed: left exactly as it is */
    make_long(p, sizeof(p), "\\\\?\\C:\\x/y", '\\', 300);
    n = pin_fs_win_path(p, w, 2048);
    CHECK(n > 0 && weq(w, p), "an extended path is not touched");
    make_long(p, sizeof(p), "\\\\.\\pipe\\x", '\\', 300);
    CHECK(pin_fs_win_path(p, w, 2048) > 0 && weq(w, p), "a device path is not touched");

    /* a long relative path: Windows makes it absolute first; this step leaves it */
    make_long(p, sizeof(p), "rel", '\\', 300);
    CHECK(pin_fs_win_path(p, w, 2048) > 0 && weq(w, p), "a long relative path is left for GetFullPathNameW");

    /* non-ASCII in a long path */
    make_long(p, sizeof(p), "C:\\Caf\xc3\xa9", '\\', 300);
    n = pin_fs_win_path(p, w, 2048);
    CHECK(n > 0 && w[0] == '\\' && w[3] == '\\' && w[4] == 'C' && w[10] == 0xE9, "non-ASCII after the prefix");

    /* threshold: just under stays, at it gets the prefix */
    make_long(p, sizeof(p), "C:\\", '\\', PIN_FS_LONG_PATH);       /* length PIN_FS_LONG_PATH - 1 */
    CHECK(pin_fs_win_path(p, w, 2048) == PIN_FS_LONG_PATH - 1 && w[0] == 'C', "just under the limit");
    make_long(p, sizeof(p), "C:\\", '\\', PIN_FS_LONG_PATH + 1);   /* length PIN_FS_LONG_PATH */
    CHECK(pin_fs_win_path(p, w, 2048) > 0 && w[0] == '\\' && w[2] == '?', "at the limit");

    /* the drive root alone keeps its backslash */
    {
        uint16_t in[400], out[420];
        in[0] = 'C'; in[1] = ':';
        size_t k = 2;
        for (int i = 0; i < 130; i++) { in[k++] = '\\'; in[k++] = '.'; }
        in[k] = 0;
        CHECK(pin_fs_extend_wpath(in, out, 420) == 7 && weq(out, "\\\\?\\C:\\"), "the root itself");
        CHECK(pin_fs_extend_wpath(in, out, 5) == -1, "too small an output");
    }
    CHECK(pin_fs_win_path("C:\\\xff", w, 2048) == -1, "invalid UTF-8 is refused");
}

/* The calls on non-ASCII names and a path longer than 260 bytes (on POSIX the plain calls; on
 * Windows the same test runs the wide ones with the \\?\ prefix). */
static void test_calls(void)
{
    char dir[1024] = "pin_fs_test_dir_\xc3\xa9t\xc3\xa9";
    pin_fs_mkdir(dir, 0755);
    CHECK(pin_fs_is_dir(dir), "non-ASCII folder made");
    CHECK(pin_fs_mkdir(dir, 0755) == -1 && errno == EEXIST, "mkdir of an existing folder: EEXIST");
    for (int level = 0; level < 3; level++) {
        size_t n = strlen(dir);
        n += (size_t)snprintf(dir + n, sizeof(dir) - n, "/\xd0\x9f\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb0-%d-", level);
        for (int i = 0; i < 90; i++)
            dir[n++] = 'y';
        dir[n] = 0;
        pin_fs_mkdir(dir, 0755);
        CHECK(pin_fs_is_dir(dir), "long folder level made");
    }
    CHECK(strlen(dir) > 260, "the folder is longer than MAX_PATH");

    char a[1200], b[1200];
    snprintf(a, sizeof(a), "%s/Caf\xc3\xa9 \xe2\x82\xac.dv", dir);
    snprintf(b, sizeof(b), "%s/\xf0\x9f\x98\x80.dv", dir);
    pin_fs_remove(a);
    pin_fs_remove(b);
    CHECK(!pin_fs_exists(a) && pin_fs_size(a) == -1, "not there yet");
    FILE *f = pin_fs_fopen(a, "wb");
    CHECK(f != NULL, "fopen in the long non-ASCII folder");
    if (f) {
        fwrite("12345", 1, 5, f);
        fclose(f);
    }
    CHECK(pin_fs_exists(a) && !pin_fs_is_dir(a) && pin_fs_size(a) == 5, "exists, is a file, 5 bytes");
    CHECK(pin_fs_size(dir) == -1, "a folder has no file size");

    /* rename replaces an existing target */
    f = pin_fs_fopen(b, "wb");
    if (f)
        fclose(f);
    CHECK(pin_fs_rename(a, b) == 0, "rename over an existing file");
    CHECK(!pin_fs_exists(a) && pin_fs_size(b) == 5, "the target has the content");

    /* the listing returns the names as UTF-8 */
    pin_fs_dir_t *d = pin_fs_opendir(dir);
    CHECK(d != NULL, "opendir of the long folder");
    int seen = 0;
    const char *name;
    while (d && (name = pin_fs_readdir(d)) != NULL)
        if (strcmp(name, "\xf0\x9f\x98\x80.dv") == 0)
            seen = 1;
    pin_fs_closedir(d);
    CHECK(seen, "the emoji file name read back as UTF-8");

    CHECK(pin_fs_remove(b) == 0 && !pin_fs_exists(b), "remove");
    CHECK(pin_fs_remove(b) == -1 && errno == ENOENT, "remove of a missing file: ENOENT");
    snprintf(a, sizeof(a), "%s/no_such_folder", dir);
    CHECK(pin_fs_opendir(a) == NULL && errno == ENOENT, "opendir of a missing folder: ENOENT");
    snprintf(a, sizeof(a), "%s/no_such_folder/x.dv", dir);
    CHECK(pin_fs_fopen(a, "wb") == NULL, "fopen in a missing folder fails");
}

int main(void)
{
    test_utf8();
    test_prefix();
    test_calls();
    if (g_failures) {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_pin_fs: all passed\n");
    return 0;
}
