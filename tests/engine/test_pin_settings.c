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
#include <math.h>
#include <pthread.h>
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

static void test_unrepresentable_entries(void)
{
    pin_settings_t s;
    pin_settings_init(&s);
    CHECK(pin_settings_set_string(&s, "A", "k", "line1\nline2") != 0, "newline in a value refused");
    CHECK(pin_settings_set_string(&s, "A", "k", "line1\rline2") != 0, "CR in a value refused");
    CHECK(pin_settings_set_string(&s, "A\n[B]", "k", "v") != 0, "newline in a section refused");
    CHECK(pin_settings_set_string(&s, "A", "k\nx", "v") != 0, "newline in a key refused");
    CHECK(pin_settings_set_string(&s, "A", ";k", "v") != 0, "key starting with ; refused");
    CHECK(pin_settings_set_string(&s, "A", "#k", "v") != 0, "key starting with # refused");
    CHECK(pin_settings_set_string(&s, "A", "[k", "v") != 0, "key starting with [ refused");
    CHECK(pin_settings_set_string(&s, "A", "", "v") != 0, "empty key refused");
    CHECK(pin_settings_set_string(&s, "A", "a=b", "v") != 0, "= in a key refused");
    CHECK(pin_settings_set_double(&s, "A", "d", NAN) != 0, "nan refused");
    CHECK(pin_settings_set_double(&s, "A", "d", INFINITY) != 0, "inf refused");
    CHECK(s.count == 0, "nothing was stored");
    CHECK(pin_settings_set_string(&s, "A", "k", "a = b ; c # d") == 0, "= ; # inside a value are fine");

    const char *path = "pin_settings_test_unrep.ini";
    CHECK(pin_settings_save(&s, path) == 0, "save");
    pin_settings_free(&s);
    pin_settings_init(&s);
    CHECK(pin_settings_load(&s, path) == 0, "load");
    CHECK(s.count == 1 && strcmp(pin_settings_get_string(&s, "A", "k", ""), "a = b ; c # d") == 0,
          "one entry, round-tripped");
    pin_settings_free(&s);
    remove(path);
}

static void test_strict_numbers(void)
{
    const char *path = "pin_settings_test_num.ini";
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL, "create");
    if (!f)
        return;
    fputs("[N]\na=12abc\nb=99999999999\nc=-5\nd= 7 \ne=1e400\nf=nan\ng=inf\nh=2.5x\ni=2.5\n"
          "j=\nk=-2147483648\nl=2147483648\n", f);
    fclose(f);
    pin_settings_t s;
    pin_settings_init(&s);
    CHECK(pin_settings_load(&s, path) == 0, "load");
    CHECK(pin_settings_get_int(&s, "N", "a", 42) == 42, "12abc falls back");
    CHECK(pin_settings_get_int(&s, "N", "b", 42) == 42, "overflow falls back");
    CHECK(pin_settings_get_int(&s, "N", "c", 42) == -5, "negative ok");
    CHECK(pin_settings_get_int(&s, "N", "d", 42) == 7, "edge whitespace trimmed on load");
    CHECK(pin_settings_get_int(&s, "N", "j", 42) == 42, "empty falls back");
    CHECK(pin_settings_get_int(&s, "N", "k", 42) == -2147483647 - 1, "INT_MIN ok");
    CHECK(pin_settings_get_int(&s, "N", "l", 42) == 42, "INT_MAX + 1 falls back");
    CHECK(pin_settings_get_double(&s, "N", "e", 3.0) == 3.0, "overflowing double falls back");
    CHECK(pin_settings_get_double(&s, "N", "f", 3.0) == 3.0, "nan falls back");
    CHECK(pin_settings_get_double(&s, "N", "g", 3.0) == 3.0, "inf falls back");
    CHECK(pin_settings_get_double(&s, "N", "h", 3.0) == 3.0, "2.5x falls back");
    CHECK(pin_settings_get_double(&s, "N", "i", 3.0) == 2.5, "2.5 ok");
    pin_settings_free(&s);
    remove(path);
}

#define UPD_THREADS 4
#define UPD_KEYS 25
static const char *g_upd_path = "pin_settings_test_upd.ini";

static void *upd_thread(void *arg)
{
    long t = (long)arg;
    for (int i = 0; i < UPD_KEYS; i++) {
        char key[32], val[16];
        snprintf(key, sizeof(key), "T.k%ld_%d", t, i);
        snprintf(val, sizeof(val), "%d", i);
        pin_settings_kv_t kv = { key, val };
        if (pin_settings_update(g_upd_path, &kv, 1) != 0)
            g_failures++;
    }
    return NULL;
}

static void test_concurrent_updates_keep_every_key(void)
{
    remove(g_upd_path);
    pthread_t th[UPD_THREADS];
    for (long t = 0; t < UPD_THREADS; t++)
        pthread_create(&th[t], NULL, upd_thread, (void *)t);
    for (int t = 0; t < UPD_THREADS; t++)
        pthread_join(th[t], NULL);
    pin_settings_t s;
    pin_settings_init(&s);
    CHECK(pin_settings_load(&s, g_upd_path) == 0, "load");
    CHECK(s.count == UPD_THREADS * UPD_KEYS, "no update lost to a concurrent save");
    pin_settings_free(&s);
    remove(g_upd_path);
}

static void test_update_and_read(void)
{
    const char *path = "pin_settings_test_rw.ini";
    remove(path);
    const pin_settings_kv_t ok[] = { { "a.x", "1" }, { "a.y", "2" }, { "top", "t" } };
    CHECK(pin_settings_update(path, ok, 3) == 0, "update three keys");
    char out[16];
    CHECK(pin_settings_read(path, "a.y", out, sizeof(out)) == 0 && strcmp(out, "2") == 0, "read");
    CHECK(pin_settings_read(path, "top", out, sizeof(out)) == 0 && strcmp(out, "t") == 0,
          "no dot: section General");
    CHECK(pin_settings_read(path, "a.none", out, sizeof(out)) == 0 && out[0] == 0, "missing key is empty");

    /* one bad entry: nothing is written */
    const pin_settings_kv_t bad[] = { { "a.x", "changed" }, { "a.z", "bad\nvalue" } };
    CHECK(pin_settings_update(path, bad, 2) == PIN_SETTINGS_BAD_KEY, "bad entry refused");
    CHECK(pin_settings_read(path, "a.x", out, sizeof(out)) == 0 && strcmp(out, "1") == 0,
          "the good entry was not written either");

    /* a section of 64 characters is an error, 63 is fine */
    char key[128];
    memset(key, 's', 64);
    strcpy(key + 64, ".k");
    CHECK(pin_settings_read(path, key, out, sizeof(out)) == PIN_SETTINGS_BAD_KEY, "64-char section on read");
    const pin_settings_kv_t longsec = { key, "v" };
    CHECK(pin_settings_update(path, &longsec, 1) == PIN_SETTINGS_BAD_KEY, "64-char section on update");
    memset(key, 's', 63);
    strcpy(key + 63, ".k");
    CHECK(pin_settings_read(path, key, out, sizeof(out)) == 0, "63-char section ok");

    /* truncation ends on a character boundary and says so */
    const pin_settings_kv_t u = { "a.u", "ab\xc3\xa9\xc3\xa9" };
    CHECK(pin_settings_update(path, &u, 1) == 0, "utf-8 value");
    char small[5];
    CHECK(pin_settings_read(path, "a.u", small, sizeof(small)) == PIN_SETTINGS_TRUNCATED &&
              strcmp(small, "ab\xc3\xa9") == 0,
          "cut on a character boundary");
    char one[1];
    CHECK(pin_settings_read(path, "a.u", one, sizeof(one)) == PIN_SETTINGS_TRUNCATED && one[0] == 0,
          "cap 1");
    CHECK(pin_settings_read(path, "a.u", out, sizeof(out)) == 0 && strlen(out) == 6, "fits");

    /* a path that cannot be read is an IO error, and update does not overwrite it */
    CHECK(pin_settings_read(".", "a.x", out, sizeof(out)) == PIN_SETTINGS_IO && out[0] == 0,
          "unreadable: IO error");
    CHECK(pin_settings_update(".", ok, 1) == PIN_SETTINGS_IO, "unreadable: update refused");
    remove(path);
}

static void test_migrate_section(void)
{
    const char *path = "pin_settings_test_mig.ini";
    remove(path);
    const pin_settings_kv_t kv[] = { { "dev_old.a", "1" }, { "dev_old.b", "2" }, { "other.c", "3" } };
    CHECK(pin_settings_update(path, kv, 3) == 0, "setup");
    CHECK(pin_settings_migrate_section(path, "dev_old", "dev_new") == 0, "migrate");
    char out[8];
    CHECK(pin_settings_read(path, "dev_new.b", out, sizeof(out)) == 0 && strcmp(out, "2") == 0, "moved");
    CHECK(pin_settings_read(path, "dev_old.a", out, sizeof(out)) == 0 && out[0] == 0, "old is gone");
    CHECK(pin_settings_read(path, "other.c", out, sizeof(out)) == 0 && strcmp(out, "3") == 0, "others stay");
    /* a target that has entries is never overwritten */
    const pin_settings_kv_t again[] = { { "dev_old.a", "9" }, { "dev_new.b", "keep" } };
    CHECK(pin_settings_update(path, again, 2) == 0, "setup again");
    CHECK(pin_settings_migrate_section(path, "dev_old", "dev_new") == 0, "no-op");
    CHECK(pin_settings_read(path, "dev_new.b", out, sizeof(out)) == 0 && strcmp(out, "keep") == 0 &&
              pin_settings_read(path, "dev_old.a", out, sizeof(out)) == 0 && strcmp(out, "9") == 0,
          "both sections untouched");
    remove(path);
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
    test_unrepresentable_entries();
    test_strict_numbers();
    test_concurrent_updates_keep_every_key();
    test_update_and_read();
    test_migrate_section();

    if (g_failures == 0) {
        printf("test_pin_settings: all tests passed\n");
        return 0;
    }
    printf("test_pin_settings: %d failure(s)\n", g_failures);
    return g_failures;
}
