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

#include <stddef.h>

int pin_usb_hub_depth(const uint8_t *ports, int count)
{
    if (!ports || count <= 0)
        return -1;
    for (int i = 0; i < count; i++)
        if (ports[i] == 0)
            return -1;
    return count - 1;
}

/* Kept short: the GUI shows it in an InfoBar, the CLI on one or two lines.
 * Advisory wording on purpose: some mainboards route their own ports through
 * a built-in hub, which looks the same from here. */
#if defined(_WIN32)
#define PIN_USB_TOPOLOGY_TOOL "USBTreeView (Uwe Sieber)"
#elif defined(__APPLE__)
#define PIN_USB_TOPOLOGY_TOOL "System Information > USB (or \"system_profiler SPUSBDataType\")"
#else
#define PIN_USB_TOPOLOGY_TOOL "\"lsusb -t\""
#endif

const char *pin_usb_hub_hint_text(void)
{
    return "The device is connected through a USB hub and shares its bandwidth with the other "
           "devices on it, which can drop frames. Plug it directly into a USB port of the computer "
           "(not a hub, front-panel hub, dock or monitor). Check the connection with "
           PIN_USB_TOPOLOGY_TOOL ". Some computers have built-in hubs; then this warning is harmless.";
}
