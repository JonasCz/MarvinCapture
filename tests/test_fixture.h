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

/*
 * The recordings the ctest suite makes at test time with ffmpeg (tests/data/README.md,
 * tests/make_fixtures.cmake). A test that reads one skips when it is missing (no ffmpeg),
 * except when the environment variable CI is set (GitHub Actions sets CI=true): then the
 * test fails, so a CI run cannot pass by skipping.
 */

#ifndef PIN_TEST_FIXTURE_H
#define PIN_TEST_FIXTURE_H

#include <stdio.h>
#include <stdlib.h>

/* 1 if `path` is there. If not: prints a SKIP line and returns 0 -- or, when CI is set,
 * prints FAIL and exits with 1. */
static inline int pin_test_have_fixture(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        return 1;
    }
    const char *ci = getenv("CI");
    if (ci && ci[0]) {
        fprintf(stderr, "FAIL: %s not present, and CI is set (made with ffmpeg at test time, "
                        "see tests/data/README.md)\n", path);
        exit(1);
    }
    printf("SKIP: %s not present (made with ffmpeg at test time, see tests/data/README.md)\n", path);
    return 0;
}

#endif /* PIN_TEST_FIXTURE_H */
