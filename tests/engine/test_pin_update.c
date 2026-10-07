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

/* The hardware-free half of the update check: VERSION parsing, version order, text. */

#include "pin_update.h"
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

static int parse(const char *json, pin_update_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    return pin_update_parse(json, strlen(json), info);
}

static void test_parse(void)
{
    pin_update_info_t i;
    CHECK(parse("{\n  \"version\": \"1.2\",\n  \"release_notes\": \"Line one.\\nTab\\there \\\"q\\\" \\u00e9\\ud83d\\ude00\",\n"
                "  \"download_url\": \"https://example.com/dl\"\n}\n", &i) == 0, "parses");
    CHECK(strcmp(i.latest, "1.2") == 0, "version");
    CHECK(strcmp(i.notes, "Line one.\nTab\there \"q\" \xc3\xa9\xf0\x9f\x98\x80") == 0, "escapes");
    CHECK(strcmp(i.download_url, "https://example.com/dl") == 0, "url");

    CHECK(parse("\xef\xbb\xbf{\"x\": [1, {\"y\": null}, true], \"version\": \"2.0.1\", \"n\": -1.5e3}", &i) == 0,
          "bom, unknown keys of every type");
    CHECK(strcmp(i.latest, "2.0.1") == 0 && i.notes[0] == '\0' && i.download_url[0] == '\0', "defaults");

    CHECK(parse("{\"version\": \"1.0\", \"download_url\": \"javascript:alert(1)\"}", &i) == 0 &&
          i.download_url[0] == '\0', "non-http url dropped");

    CHECK(parse("1.0\n", &i) != 0, "plain old VERSION file rejected");
    CHECK(parse("{}", &i) != 0, "empty object");
    CHECK(parse("{\"version\": \"1.x\"}", &i) != 0, "non-numeric version");
    CHECK(parse("{\"version\": \"1..0\"}", &i) != 0, "empty part");
    CHECK(parse("{\"version\": 1.0}", &i) != 0, "number, not string");
    CHECK(parse("{\"version\": \"1.0\"", &i) != 0, "truncated");
    CHECK(parse("<html>404</html>", &i) != 0, "html");

    /* long notes are cut, still terminated */
    char big[5000];
    int n = snprintf(big, sizeof(big), "{\"version\":\"3\",\"release_notes\":\"");
    memset(big + n, 'a', 3000);
    snprintf(big + n + 3000, sizeof(big) - (size_t)n - 3000, "\"}");
    CHECK(parse(big, &i) == 0 && strlen(i.notes) == PIN_UPDATE_NOTES_MAX - 1, "long notes truncated");
}

static void test_compare(void)
{
    CHECK(pin_update_compare("1.0", "1.0.0") == 0, "1.0 == 1.0.0");
    CHECK(pin_update_compare("1.0", "1.0.1") < 0, "1.0 < 1.0.1");
    CHECK(pin_update_compare("1.10", "1.9") > 0, "1.10 > 1.9");
    CHECK(pin_update_compare("2", "1.99.99") > 0, "2 > 1.99.99");
    CHECK(pin_update_compare("0.1.0", "1.0") < 0, "0.1.0 < 1.0");
}

static void test_format(void)
{
    pin_update_info_t i;
    memset(&i, 0, sizeof(i));
    snprintf(i.current, sizeof(i.current), "1.0");
    snprintf(i.latest, sizeof(i.latest), "1.1");
    char out[128] = "x";
    pin_update_format(&i, out, sizeof(out));
    CHECK(out[0] == '\0', "nothing when not available");
    i.available = 1;
    pin_update_format(&i, out, sizeof(out));
    CHECK(strcmp(out, "MarvinCapture 1.1 is available (you have 1.0).") == 0, "notice text");
}

int main(void)
{
    test_parse();
    test_compare();
    test_format();
    if (g_failures) {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
