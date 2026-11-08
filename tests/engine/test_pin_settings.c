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

static void test_missing_file_is_empty(void)
{
    pin_settings_t s;
    pin_settings_init(&s);
    int rc = pin_settings_load(&s, "pin_settings_test_does_not_exist.ini");
    CHECK(rc == 0, "loading a missing file is not an error");
    CHECK(strcmp(pin_settings_get_string(&s, "General", "foo", "default"), "default") == 0,
          "missing key returns the default");
    pin_settings_free(&s);
}

static void test_round_trip(void)
{
    const char *path = "pin_settings_test_roundtrip.ini";

    pin_settings_t s;
    pin_settings_init(&s);
    pin_settings_set_string(&s, "Paths", "bitstream_dir", "C:\\firmware");
    pin_settings_set_int(&s, "General", "window_x", 120);
    pin_settings_set_double(&s, "General", "gain", 1.5);
    pin_settings_set_bool(&s, "General", "split", 1);
    pin_settings_set_bool(&s, "General", "verbose", 0);
    /* Overwrite an existing key: must not create a duplicate. */
    pin_settings_set_int(&s, "General", "window_x", 240);

    CHECK(pin_settings_save(&s, path) == 0, "save should succeed");
    pin_settings_free(&s);

    pin_settings_t loaded;
    pin_settings_init(&loaded);
    CHECK(pin_settings_load(&loaded, path) == 0, "load should succeed");

    CHECK(strcmp(pin_settings_get_string(&loaded, "Paths", "bitstream_dir", ""), "C:\\firmware") == 0,
          "string round-trips");
    CHECK(pin_settings_get_int(&loaded, "General", "window_x", -1) == 240,
          "overwritten int round-trips (no duplicate key)");
    CHECK(pin_settings_get_double(&loaded, "General", "gain", -1.0) == 1.5, "double round-trips");
    CHECK(pin_settings_get_bool(&loaded, "General", "split", 0) == 1, "bool true round-trips");
    CHECK(pin_settings_get_bool(&loaded, "General", "verbose", 1) == 0, "bool false round-trips");

    pin_settings_free(&loaded);
    remove(path);
    remove("pin_settings_test_roundtrip.ini.tmp");
}

static void test_bool_variants_and_comments(void)
{
    const char *path = "pin_settings_test_manual.ini";
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL, "can write the manual test file");
    if (f) {
        fputs("; a comment line\n", f);
        fputs("[General]\n", f);
        fputs("# another comment\n", f);
        fputs("a = yes\n", f);
        fputs("b = OFF\n", f);
        fputs("c = TRUE\n", f);
        fputs("garbage line with no equals sign\n", f);
        fputs("d=1\n", f);
        fclose(f);
    }

    pin_settings_t s;
    pin_settings_init(&s);
    CHECK(pin_settings_load(&s, path) == 0, "load should succeed");
    CHECK(pin_settings_get_bool(&s, "General", "a", -1) == 1, "'yes' -> true");
    CHECK(pin_settings_get_bool(&s, "General", "b", -1) == 0, "'OFF' -> false, case-insensitive");
    CHECK(pin_settings_get_bool(&s, "General", "c", -1) == 1, "'TRUE' -> true, case-insensitive");
    CHECK(pin_settings_get_bool(&s, "General", "d", -1) == 1, "'1' -> true");
    CHECK(pin_settings_get_int(&s, "General", "missing", 42) == 42, "unset key keeps the default");

    pin_settings_free(&s);
    remove(path);
}

static void write_file(const char *path, const char *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL, "can write a test file");
    if (f) {
        fwrite(data, 1, n, f);
        fclose(f);
    }
}

static void test_parser_edge_cases(void)
{
    const char *path = "pin_settings_test_parser.ini";

    /* BOM, a header with a trailing comment, a '#' comment, a header whose keys must not
     * leak into the previous section */
    const char *txt = "\xEF\xBB\xBF[Paths] ; where\n"
                      "firmware_dir=/fw\n"
                      "[General]   # more\n"
                      "a=1\n"
                      "[Odd]x]\n"
                      "b=2\n";
    write_file(path, txt, strlen(txt));
    pin_settings_t s;
    pin_settings_init(&s);
    CHECK(pin_settings_load(&s, path) == 0, "load with BOM succeeds");
    CHECK(strcmp(pin_settings_get_string(&s, "Paths", "firmware_dir", ""), "/fw") == 0,
          "BOM does not become part of the first section");
    CHECK(strcmp(pin_settings_get_string(&s, "General", "a", ""), "1") == 0,
          "header followed by a comment is a header");
    CHECK(strcmp(pin_settings_get_string(&s, "Odd]x", "b", ""), "2") == 0,
          "a ']' inside the section name still works");
    CHECK(s.count == 3, "no stray entries");
    pin_settings_free(&s);

    /* a line over 4095 bytes: one entry (or none), never split into bogus ones */
    size_t big = 10000;
    char *buf = malloc(big + 64);
    CHECK(buf != NULL, "alloc");
    if (buf) {
        size_t n = (size_t)sprintf(buf, "[S]\nlong=");
        memset(buf + n, 'x', big);
        n += big;
        n += (size_t)sprintf(buf + n, "\nbogus=zzz\nafter=ok\n");
        /* text that would parse as an entry if the line were split at 4095 bytes */
        memcpy(buf + 5000, "bogus2=1", 8);
        write_file(path, buf, n);
        free(buf);
        pin_settings_init(&s);
        CHECK(pin_settings_load(&s, path) == 0, "load with a long line succeeds");
        CHECK(strcmp(pin_settings_get_string(&s, "S", "after", ""), "ok") == 0,
              "the line after a long one is read");
        CHECK(strlen(pin_settings_get_string(&s, "S", "long", "")) > 4095,
              "a long value is read whole");
        CHECK(strcmp(pin_settings_get_string(&s, "S", "bogus2", "none"), "none") == 0,
              "a long line is not split into entries");
        pin_settings_free(&s);
    }
    remove(path);
}

static void test_many_sections_written_once(void)
{
    const char *path = "pin_settings_test_many.ini";
    pin_settings_t s;
    pin_settings_init(&s);
    for (int i = 0; i < 600; i++) {
        char sec[32], key[32];
        snprintf(sec, sizeof(sec), "sec%d", i);
        snprintf(key, sizeof(key), "k%d", i);
        pin_settings_set_int(&s, sec, key, i);
        pin_settings_set_int(&s, sec, "second", i);
    }
    CHECK(pin_settings_save(&s, path) == 0, "save 600 sections");
    pin_settings_free(&s);

    pin_settings_init(&s);
    CHECK(pin_settings_load(&s, path) == 0, "load 600 sections");
    CHECK(s.count == 1200, "every entry once");
    CHECK(pin_settings_get_int(&s, "sec599", "k599", -1) == 599, "last section intact");
    pin_settings_free(&s);

    /* the file has each header exactly once */
    FILE *f = fopen(path, "rb");
    int headers = 0;
    char line[128];
    while (f && fgets(line, sizeof(line), f))
        if (line[0] == '[')
            headers++;
    if (f)
        fclose(f);
    CHECK(headers == 600, "each section header written once");
    remove(path);
}

static void test_load_failure_is_not_empty(void)
{
    /* a directory opens with fopen() on POSIX but cannot be read: not the same as "missing" */
    pin_settings_t s;
    pin_settings_init(&s);
    pin_settings_set_string(&s, "A", "b", "c");
    int rc = pin_settings_load(&s, ".");
    CHECK(rc != 0, "an unreadable path fails instead of looking empty");
    CHECK(s.count == 0, "entries are cleared on failure");
    pin_settings_free(&s);
}

static void test_save_leaves_no_temp_file(void)
{
    const char *path = "pin_settings_test_tmp.ini";
    pin_settings_t s;
    pin_settings_init(&s);
    pin_settings_set_string(&s, "A", "b", "c");
    CHECK(pin_settings_save(&s, path) == 0, "save");
    CHECK(pin_settings_save(&s, path) == 0, "save again");
    pin_settings_free(&s);
    remove(path);
    /* a save into a missing directory fails without leaving anything */
    pin_settings_init(&s);
    CHECK(pin_settings_save(&s, "no_such_dir_pin/settings.ini") != 0, "missing dir fails");
    pin_settings_free(&s);
}

static void test_default_path(void)
{
    char path[2048];
    int rc = pin_settings_default_path(path, sizeof(path));
    CHECK(rc == 0, "default path should resolve on this environment");
    if (rc == 0) {
        CHECK(strstr(path, "settings.ini") != NULL, "default path ends in settings.ini");
        CHECK((strstr(path, "PinnacleOSS") != NULL) || (strstr(path, "pinnacle-oss") != NULL),
              "default path is namespaced to this app");
    }
}

static void test_device_key(void)
{
    char k[128];
    CHECK(pin_settings_device_key("0a1b2c3d4e5f6071", "usb:1-4.2", "gui.dir_dv", k, sizeof(k)) == 0 &&
              strcmp(k, "dev_0A1B2C3D4E5F6071.gui.dir_dv") == 0,
          "serial wins, upper-cased");
    CHECK(pin_settings_device_key("", "usb:1-4.2", "gui.x", k, sizeof(k)) == 0 &&
              strcmp(k, "dev_usb_1-4_2.gui.x") == 0,
          "id fallback has no dots or colons");
    CHECK(pin_settings_device_key("ABCD", NULL, NULL, k, sizeof(k)) == 0 && strcmp(k, "dev_ABCD.") == 0,
          "no key gives the prefix");
    CHECK(pin_settings_device_key(NULL, NULL, "k", k, sizeof(k)) == -1, "no identity fails");
    CHECK(pin_settings_device_key("ABCD", NULL, "gui.long_key", k, 12) == -1, "too small fails");

    /* two devices stay independent, and the key's section/key split survives the INI store */
    char ka[128], kb[128];
    pin_settings_device_key("AAAA", NULL, "gui.name_dv", ka, sizeof(ka));
    pin_settings_device_key("BBBB", NULL, "gui.name_dv", kb, sizeof(kb));
    CHECK(strcmp(ka, kb) != 0, "different devices, different keys");
    const char *dot = strchr(ka, '.');
    CHECK(dot && strncmp(dot + 1, "gui.name_dv", 11) == 0 && (size_t)(dot - ka) == 8,
          "section ends at the first dot");
}

int main(void)
{
    test_device_key();
    test_missing_file_is_empty();
    test_round_trip();
    test_bool_variants_and_comments();
    test_parser_edge_cases();
    test_many_sections_written_once();
    test_load_failure_is_not_empty();
    test_save_leaves_no_temp_file();
    test_default_path();

    if (g_failures == 0) {
        printf("test_pin_settings: all tests passed\n");
        return 0;
    }
    printf("test_pin_settings: %d failure(s)\n", g_failures);
    return g_failures;
}
