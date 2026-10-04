// Pinnacle Studio 500-USB open driver
// Copyright (C) 2026 Jonas Cz.
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
// for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

import Foundation
import CMarvinCore

/// One row of the device picker: name, id and a badge, all texts from the core.
/// Port of DeviceItemViewModel. Immutable: the list is rebuilt on every refresh.
struct DeviceItem: Identifiable, Hashable {
    /// Badge colour class for the view (no colours here).
    enum Badge { case neutral, caution, critical, capturing }

    let id: String
    let name: String
    let state: pin_dev_state_t
    let ownerPid: UInt32
    /// Hubs between the computer and the device: 0 direct, > 0 behind a hub, -1 unknown.
    let hubDepth: Int
    let serial: String
    let isCapturingHere: Bool
    /// Badge text ("Capturing", "Ready", "In use", ...).
    let statusText: String
    /// Why this entry can't be picked, nil when it can (the entry's tooltip).
    let unavailableReason: String?
    /// The core's sentence for a failed attempt to open this entry, nil when it can be opened.
    let openProblem: String?

    init(_ info: pin_device_info_t, capturingHere: Bool = false) {
        id = info.idString
        // The core flags models nobody verified on real hardware as "(untested)".
        name = Pin.deviceDisplayName(info)
        statusText = Pin.deviceStatusText(info, capturingHere: capturingHere)
        unavailableReason = Pin.deviceUnavailableReason(info)
        openProblem = Pin.deviceOpenProblem(info)
        state = info.state
        ownerPid = info.owner_pid
        serial = info.serialString
        hubDepth = Int(info.hub_depth)
        isCapturingHere = capturingHere
    }

    /// Plugged in through a USB hub, where it shares bandwidth (advisory; unknown = false).
    var isBehindHub: Bool { hubDepth > 0 }
    var hubLine: String { "⚠ connected via USB hub" }
    /// The core's advice for a device behind a hub, else nil: the entry's tooltip.
    var hubHint: String? { isBehindHub ? Self.hubHintText : nil }
    private static let hubHintText: String? = { let t = Pin.usbHubHint(); return t.isEmpty ? nil : t }()

    /// Whether this process can open it (free, or already open here; the core's rule).
    var isUsable: Bool { unavailableReason == nil }

    /// Stable id shown in the list: the full 1394 GUID once known, else the USB port id.
    var shortId: String { serial.isEmpty ? id : serial.uppercased() }
    var displayName: String { "\(name) (\(shortId))" }

    var accessibilityName: String {
        (unavailableReason == nil ? "\(name), \(shortId), \(statusText)" : "\(name), \(shortId), \(unavailableReason!)")
            + (isBehindHub ? ", connected via USB hub" : "")
    }

    var badge: Badge {
        if isCapturingHere { return .capturing }
        switch state {
        case PIN_DEV_PREPARING: return .caution
        case PIN_DEV_NO_DRIVER, PIN_DEV_UNSUPPORTED: return .critical
        default: return .neutral
        }
    }

    /// What the list diff compares (the Windows rule: id, state and the capturing badge).
    func sameRow(as o: DeviceItem) -> Bool { id == o.id && state == o.state && isCapturingHere == o.isCapturingHere }
}
