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

#include "pinnacle_fx2.h"

#include <time.h>

static void settle(unsigned ms)
{
    if (!ms)
        return;
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

int pinnacle_fx2_validate(const uint8_t *img, size_t len)
{
    if (!img || len == 0 || len > PINNACLE_FX2_MAX_IMAGE)
        return -1;
    return img[0] == 0x02 ? 0 : -1;
}

int pinnacle_fx2_block(size_t len, unsigned i, uint16_t *addr, uint16_t *n)
{
    size_t off = (size_t)i * PINNACLE_FX2_BLOCK;
    if (off >= len)
        return -1;
    size_t left = len - off;
    *addr = (uint16_t)off;
    *n = (uint16_t)(left < PINNACLE_FX2_BLOCK ? left : PINNACLE_FX2_BLOCK);
    return 0;
}

int pinnacle_fx2_download(const uint8_t *img, size_t len, pinnacle_fx2_write_fn write,
                          void *user, unsigned settle_ms)
{
    uint8_t cpucs = 1;
    int rc = write(user, PINNACLE_FX2_CPUCS, &cpucs, 1);
    if (rc)
        return rc;
    settle(settle_ms);

    uint16_t addr, n;
    for (unsigned i = 0; pinnacle_fx2_block(len, i, &addr, &n) == 0; i++) {
        rc = write(user, addr, img + addr, n);
        if (rc)
            return rc;
    }

    cpucs = 0;
    rc = write(user, PINNACLE_FX2_CPUCS, &cpucs, 1);
    settle(settle_ms);
    return rc;
}
