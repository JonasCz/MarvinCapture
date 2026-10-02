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
 * pinlist — prints every Marvin-family device pinnacle_enumerate() finds,
 * with lock state merged in, one line each. No arguments, no hardware
 * required to run cleanly: on a box with nothing plugged in (this Windows
 * dev box, today) it prints "No devices found." and exits 0.
 *
 * This is the smallest possible integration test for pinnacle_enum.[ch] and
 * pinnacle_lock.[ch] together, and doubles as a quick "is the 500-USB free
 * right now" check ahead of a pincli/pindeck/pinanalog run.
 */

#include "pinnacle_enum.h"
#include "pinnacle_lock.h"

#include <stdio.h>

static const char *lock_state_name(pinnacle_lock_state_t s)
{
    switch (s) {
    case PINNACLE_LOCK_PREPARING: return "PREPARING";
    case PINNACLE_LOCK_READY:     return "READY";
    case PINNACLE_LOCK_CAPTURING: return "CAPTURING";
    }
    return "?";
}

int main(void)
{
    pinnacle_enum_entry_t entries[16];
    int total = pinnacle_enumerate(entries, 16);
    int shown = total < 16 ? total : 16;

    if (shown == 0) {
        printf("No devices found.\n");
        return 0;
    }

    for (int i = 0; i < shown; i++) {
        const pinnacle_enum_entry_t *e = &entries[i];
        char status[64];

        switch (e->state) {
        case PINNACLE_ENUM_UNSUPPORTED:
            snprintf(status, sizeof(status), "UNSUPPORTED");
            break;
        case PINNACLE_ENUM_NO_DRIVER:
            snprintf(status, sizeof(status), "NO DRIVER (bind WinUSB, e.g. with Zadig)");
            break;
        case PINNACLE_ENUM_NO_PERMISSION:
            snprintf(status, sizeof(status), "NO PERMISSION (check the udev rule)");
            break;
        case PINNACLE_ENUM_IN_USE: {
            /* pinnacle_enumerate() already resolved IN_USE from the lock,
             * but only gave us the owner pid -- query again for the state
             * the owner last reported (Preparing/Ready/Capturing), which
             * is worth showing separately (e.g. "IN USE (pid 1234,
             * PREPARING)" tells you it's mid firmware-load, not stuck). */
            pinnacle_lock_info_t linfo;
            if (pinnacle_lock_query(e->id, &linfo) == PINNACLE_OK && linfo.held)
                snprintf(status, sizeof(status), "IN USE (pid %u, %s)",
                         (unsigned)linfo.owner_pid, lock_state_name(linfo.state));
            else
                snprintf(status, sizeof(status), "IN USE (pid %u)", (unsigned)e->owner_pid);
            break;
        }
        case PINNACLE_ENUM_READY:
        default:
            snprintf(status, sizeof(status), "READY%s", e->tested ? "" : " (untested)");
            break;
        }

        printf("%-20s %-28s %04x:%04x  %s\n", e->id, e->name, e->vid, e->pid, status);
    }

    if (total > shown)
        printf("(%d more device(s) not shown)\n", total - shown);

    return 0;
}
