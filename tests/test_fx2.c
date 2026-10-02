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

/* ctest for pinnacle_fx2.[ch]: image validation, block split, download order. */

#include "pinnacle_fx2.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

typedef struct {
    int calls, fail_at;
    uint16_t addr[16], len[16];
    uint8_t first[16];
    uint8_t mem[0x4000];
} rec_t;

static int rec_write(void *user, uint16_t addr, const uint8_t *data, uint16_t len)
{
    rec_t *r = user;
    if (r->calls == r->fail_at)
        return 5;
    if (r->calls < 16) {
        r->addr[r->calls] = addr;
        r->len[r->calls] = len;
        r->first[r->calls] = data[0];
    }
    if (addr != PINNACLE_FX2_CPUCS)
        memcpy(r->mem + addr, data, len);
    r->calls++;
    return 0;
}

int main(void)
{
    uint8_t img[1300];
    for (size_t i = 0; i < sizeof(img); i++)
        img[i] = (uint8_t)(i * 7 + 1);
    img[0] = 0x02;

    CHECK(pinnacle_fx2_validate(img, sizeof(img)) == 0);
    CHECK(pinnacle_fx2_validate(img, 0) != 0);
    CHECK(pinnacle_fx2_validate(NULL, 10) != 0);
    CHECK(pinnacle_fx2_validate(img, PINNACLE_FX2_MAX_IMAGE + 1) != 0);
    img[0] = 0x00;
    CHECK(pinnacle_fx2_validate(img, sizeof(img)) != 0);
    img[0] = 0x02;

    uint16_t a, n;
    CHECK(pinnacle_fx2_block(sizeof(img), 0, &a, &n) == 0 && a == 0 && n == 512);
    CHECK(pinnacle_fx2_block(sizeof(img), 1, &a, &n) == 0 && a == 512 && n == 512);
    CHECK(pinnacle_fx2_block(sizeof(img), 2, &a, &n) == 0 && a == 1024 && n == 276);
    CHECK(pinnacle_fx2_block(sizeof(img), 3, &a, &n) != 0);
    CHECK(pinnacle_fx2_block(512, 0, &a, &n) == 0 && n == 512);
    CHECK(pinnacle_fx2_block(512, 1, &a, &n) != 0);

    static rec_t r;
    memset(&r, 0, sizeof(r));
    r.fail_at = -1;
    CHECK(pinnacle_fx2_download(img, sizeof(img), rec_write, &r, 0) == 0);
    CHECK(r.calls == 5);
    CHECK(r.addr[0] == PINNACLE_FX2_CPUCS && r.len[0] == 1 && r.first[0] == 1);
    CHECK(r.addr[1] == 0 && r.len[1] == 512);
    CHECK(r.addr[2] == 512 && r.len[2] == 512);
    CHECK(r.addr[3] == 1024 && r.len[3] == 276);
    CHECK(r.addr[4] == PINNACLE_FX2_CPUCS && r.len[4] == 1 && r.first[4] == 0);
    CHECK(memcmp(r.mem, img, sizeof(img)) == 0);

    /* a failing block aborts, and CPUCS is not released */
    memset(&r, 0, sizeof(r));
    r.fail_at = 2;
    CHECK(pinnacle_fx2_download(img, sizeof(img), rec_write, &r, 0) == 5);
    CHECK(r.calls == 2);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("test_fx2: ok\n");
    return 0;
}
