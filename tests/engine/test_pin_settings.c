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
    test_default_path();

    if (g_failures == 0) {
        printf("test_pin_settings: all tests passed\n");
        return 0;
    }
    printf("test_pin_settings: %d failure(s)\n", g_failures);
    return g_failures;
}
