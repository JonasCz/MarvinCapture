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
 * The directory of the running executable (UTF-8, no trailing separator). Shared by the firmware
 * lookup in pin_session.c and MarvinPlayout, which both look for firmware/ next to the program.
 */

#ifndef PIN_EXE_DIR_H
#define PIN_EXE_DIR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 0 and out filled, or -1 (cannot tell, or it does not fit in cap). */
int pin_exe_dir(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PIN_EXE_DIR_H */
