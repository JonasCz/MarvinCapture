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
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

static int g_failures = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)
#define CHECK_STR(a, b, msg)                                                 \
    do {                                                                     \
        if (strcmp((a), (b)) != 0) {                                         \
            printf("FAIL %s:%d: %s (got \"%s\", want \"%s\")\n", __FILE__,   \
                   __LINE__, msg, (a), (b));                                 \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static void test_strip_extension(void)
{
    char out[256];

    CHECK(pin_naming_strip_extension("foo.dv", "dv", out, sizeof(out)) == 0, "strip ok");
    CHECK_STR(out, "foo", "strip matching extension");

    CHECK(pin_naming_strip_extension("foo", "dv", out, sizeof(out)) == 0, "no ext ok");
    CHECK_STR(out, "foo", "no extension present, unchanged");

    CHECK(pin_naming_strip_extension("foo.mov", "dv", out, sizeof(out)) == 0, "mismatched ok");
    CHECK_STR(out, "foo.mov", "mismatched extension left alone");

    CHECK(pin_naming_strip_extension("C:\\videos\\foo.DV", "dv", out, sizeof(out)) == 0,
          "windows path ok");
    CHECK_STR(out, "C:\\videos\\foo", "windows path + case-insensitive extension");

    CHECK(pin_naming_strip_extension("/home/user/foo.dv", "dv", out, sizeof(out)) == 0,
          "posix path ok");
    CHECK_STR(out, "/home/user/foo", "posix path");

    char tiny[3];
    CHECK(pin_naming_strip_extension("foo.dv", "dv", tiny, sizeof(tiny)) == -1,
          "output buffer too small must fail");
}

static void test_build(void)
{
    char out[256];
    pin_naming_opts_t opts;

    memset(&opts, 0, sizeof(opts));
    opts.pass = 1;
    opts.scene_split = 0;
    CHECK(pin_naming_build("base", &opts, "dv", out, sizeof(out)) == 0, "plain build ok");
    CHECK_STR(out, "base.dv", "plain name");

    opts.scene_split = 1;
    opts.scene_index = 3;
    CHECK(pin_naming_build("base", &opts, "dv", out, sizeof(out)) == 0, "split build ok");
    CHECK_STR(out, "base-0003.dv", "split name");

    opts.scene_split = 0;
    opts.pass = 2;
    CHECK(pin_naming_build("base", &opts, "dv", out, sizeof(out)) == 0, "pass build ok");
    CHECK_STR(out, "base-pass-2.dv", "pass name");

    opts.scene_split = 1;
    opts.scene_index = 7;
    opts.pass = 2;
    CHECK(pin_naming_build("base", &opts, "dv", out, sizeof(out)) == 0, "pass+split build ok");
    CHECK_STR(out, "base-pass-2-0007.dv", "pass+split name");

    opts.pass = 0;
    CHECK(pin_naming_build("base", &opts, "dv", out, sizeof(out)) == -1, "pass 0 rejected");

    opts.pass = 1;
    opts.scene_split = 1;
    opts.scene_index = 0;
    CHECK(pin_naming_build("base", &opts, "dv", out, sizeof(out)) == -1,
          "scene_index 0 with split rejected");

    char tiny[4];
    opts.scene_split = 0;
    opts.pass = 1;
    CHECK(pin_naming_build("base", &opts, "dv", tiny, sizeof(tiny)) == -1,
          "output buffer too small must fail");
}

static void test_collides(void)
{
    const char *dir = "pin_naming_test_dir";
    MKDIR(dir); /* ignore failure: may already exist from a previous run */

    char base[256];
    snprintf(base, sizeof(base), "%s/capture", dir);

    char first_match[512];
    CHECK(pin_naming_collides(base, "dv", first_match, sizeof(first_match)) == 0,
          "no collision before any file exists");

    char path[300];
    snprintf(path, sizeof(path), "%s/capture-0001.dv", dir);
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL, "can create the marker file");
    if (f)
        fclose(f);

    int rc = pin_naming_collides(base, "dv", first_match, sizeof(first_match));
    CHECK(rc == 1, "collision detected after the file exists");
    CHECK(strstr(first_match, "capture-0001.dv") != NULL, "first_match names the colliding file");

    char other_base[256];
    snprintf(other_base, sizeof(other_base), "%s/other", dir);
    CHECK(pin_naming_collides(other_base, "dv", NULL, 0) == 0,
          "a different base in the same directory has no collision");

    remove(path);
}

static void test_validate(void)
{
    char why[128];
    CHECK(pin_naming_validate("C:\\videos\\tape 1", why, sizeof(why)) == 1, "plain name ok");
    CHECK(pin_naming_validate("/home/u/my.tape-2", NULL, 0) == 1, "dots inside and NULL reason ok");
    CHECK(pin_naming_validate("/home/u/CONSOLE", NULL, 0) == 1, "CON prefix only is fine");
    CHECK(pin_naming_validate("/home/u/COM10", NULL, 0) == 1, "COM10 is not reserved");
    CHECK(pin_naming_validate("", why, sizeof(why)) == 0, "empty");
    CHECK(pin_naming_validate("C:\\videos\\", why, sizeof(why)) == 0, "empty after separator");
    CHECK(pin_naming_validate("a<b", why, sizeof(why)) == 0, "<");
    CHECK(pin_naming_validate("a>b", why, sizeof(why)) == 0, ">");
    CHECK(pin_naming_validate("a:b", why, sizeof(why)) == 0, ":");
    CHECK(pin_naming_validate("a\"b", why, sizeof(why)) == 0, "quote");
    CHECK(pin_naming_validate("a|b", why, sizeof(why)) == 0, "|");
    CHECK(pin_naming_validate("a?b", why, sizeof(why)) == 0, "?");
    CHECK(pin_naming_validate("a*b", why, sizeof(why)) == 0, "*");
    CHECK(pin_naming_validate("a	b", why, sizeof(why)) == 0, "control char");
    CHECK(pin_naming_validate("/x/ name", why, sizeof(why)) == 0, "leading space");
    CHECK(pin_naming_validate("/x/name ", why, sizeof(why)) == 0, "trailing space");
    CHECK(pin_naming_validate("/x/name.", why, sizeof(why)) == 0, "trailing dot");
    CHECK(pin_naming_validate("/x/.name", why, sizeof(why)) == 0, "leading dot");
    CHECK(pin_naming_validate("/x/con", why, sizeof(why)) == 0, "con");
    CHECK(pin_naming_validate("/x/NUL.txt", why, sizeof(why)) == 0, "NUL.txt");
    CHECK(pin_naming_validate("/x/Aux", why, sizeof(why)) == 0, "Aux");
    CHECK(pin_naming_validate("/x/lpt1", why, sizeof(why)) == 0, "lpt1");
    CHECK(pin_naming_validate("/x/com0.dv", why, sizeof(why)) == 0, "com0.dv");
    CHECK(strstr(why, "reserved") != NULL, "reason mentions reserved");
    char longname[300];
    memset(longname, 'a', sizeof(longname));
    longname[299] = 0;
    CHECK(pin_naming_validate(longname, why, sizeof(why)) == 0, "too long");
}

int main(void)
{
    test_strip_extension();
    test_build();
    test_collides();
    test_validate();

    if (g_failures == 0) {
        printf("test_pin_naming: all tests passed\n");
        return 0;
    }
    printf("test_pin_naming: %d failure(s)\n", g_failures);
    return g_failures;
}
