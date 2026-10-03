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

/* USB topology advice: whether a device sits behind a hub (where it shares
 * the hub's bandwidth with everything else plugged into it), and the one
 * sentence of advice every front end shows for it. Pure: the caller hands in
 * the port chain it got from libusb_get_port_numbers(). */

#ifndef PIN_USB_TOPOLOGY_H
#define PIN_USB_TOPOLOGY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Number of hubs between the root hub and the device, from its port chain
 * (ports[0] = root hub port, one more entry per hub below it): 0 = directly
 * on a root hub port, >0 = behind that many hubs, -1 = unknown (the backend
 * gave no chain, an error, or a nonsense port number 0). */
int pin_usb_hub_depth(const uint8_t *ports, int count);

/* Advice for a device with hub depth > 0: why it matters, what to do and the
 * platform's tool to look at the topology. Static text, never NULL. */
const char *pin_usb_hub_hint_text(void);

#ifdef __cplusplus
}
#endif

#endif /* PIN_USB_TOPOLOGY_H */
