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
 * The process's standard output as a capture target (`--capture -`): binary
 * mode, large writes, and a closed reader reported as such (the reading
 * program exited) instead of killing the process or looking like a disk error.
 * pin_stdout_set_fd() redirects it to another descriptor (tests).
 */

#ifndef PIN_STDOUT_H
#define PIN_STDOUT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PIN_STDOUT_OK 0
#define PIN_STDOUT_ERROR (-1)
#define PIN_STDOUT_CLOSED (-2)   /* the reader is gone: EPIPE / ERROR_BROKEN_PIPE / ERROR_NO_DATA */

/* The capture path that means standard output. */
int pin_path_is_stdout(const char *path);

/* Binary mode, SIGPIPE ignored (POSIX). Idempotent. */
void pin_stdout_prepare(void);

/* fd >= 0: write there instead of fd 1 (the caller keeps ownership); -1: back to stdout. */
void pin_stdout_set_fd(int fd);

/* 0 if the target can take a video stream; else -1 with a one-line reason in why:
 * a terminal ("refusing to write video to the terminal; pipe it into a program"),
 * or no standard output at all. */
int pin_stdout_check(char *why, size_t why_size);

/* Writes all n bytes (blocks until the reader took them). PIN_STDOUT_OK,
 * PIN_STDOUT_CLOSED or PIN_STDOUT_ERROR. */
int pin_stdout_write(const void *data, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* PIN_STDOUT_H */
