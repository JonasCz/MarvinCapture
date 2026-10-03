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

#include "pin_usb_topology.h"
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

int main(void)
{
    const uint8_t root[] = { 8 };
    const uint8_t one_hub[] = { 3, 2 };
    const uint8_t two_hubs[] = { 1, 4, 2 };
    const uint8_t bogus[] = { 3, 0 };

    CHECK(pin_usb_hub_depth(root, 1) == 0, "root hub port: depth 0");
    CHECK(pin_usb_hub_depth(one_hub, 2) == 1, "behind one hub");
    CHECK(pin_usb_hub_depth(two_hubs, 3) == 2, "behind two hubs");
    CHECK(pin_usb_hub_depth(root, 0) == -1, "empty chain: unknown");
    CHECK(pin_usb_hub_depth(root, -5) == -1, "libusb error: unknown");
    CHECK(pin_usb_hub_depth(NULL, 2) == -1, "no buffer: unknown");
    CHECK(pin_usb_hub_depth(bogus, 2) == -1, "port 0: unknown");

    const char *h = pin_usb_hub_hint_text();
    CHECK(h && strstr(h, "hub") != NULL, "hint mentions the hub");
#if defined(_WIN32)
    CHECK(strstr(h, "USBTreeView") != NULL, "Windows tool");
#elif defined(__APPLE__)
    CHECK(strstr(h, "System Information") != NULL, "macOS tool");
#else
    CHECK(strstr(h, "lsusb -t") != NULL, "Linux tool");
#endif

    if (g_failures == 0)
        printf("test_pin_usb_topology: all passed\n");
    return g_failures ? 1 : 0;
}
