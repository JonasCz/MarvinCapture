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

/// Thin Swift layer over the C API (pin_api.h). Stateless helpers only; the
/// session/model objects live elsewhere and use these.
enum Pin {
    /// The API version this app was compiled against.
    static let expectedApiVersion = UInt32(PIN_API_VERSION)

    static var apiVersion: UInt32 { pin_api_version() }
    static var versionString: String { String(cString: pin_version_string()) }
    static var apiVersionMatches: Bool { apiVersion == expectedApiVersion }

    static func strerror(_ s: pin_status_t) -> String { String(cString: pin_strerror(s)) }

    /// Where the FPGA bitstreams are (optional; the core also searches next to itself).
    @discardableResult
    static func setFirmwareDir(_ path: String) -> pin_status_t { pin_set_firmware_dir(path) }

    // MARK: settings (section.key; stored by the core, shared with the other front ends)

    static func settingsGet(key: String, fallback: String = "") -> String {
        var buf = [CChar](repeating: 0, count: 2048)
        guard pin_settings_get(key, &buf, buf.count) == PIN_OK else { return fallback }
        return String(cString: buf)
    }

    @discardableResult
    static func settingsSet(key: String, value: String) -> pin_status_t { pin_settings_set(key, value) }

    // MARK: text formatting

    /// Calls a `(char *out, size_t cap)` formatting function and returns its text, e.g.
    /// `Pin.format { out, cap in pin_format_bytes(n, out, cap) }`. 512 bytes hold every
    /// core text except pin_format_status_short() (pass a larger capacity there).
    static func format(capacity: Int = 512, _ body: (UnsafeMutablePointer<CChar>, Int) -> Void) -> String {
        var buf = [CChar](repeating: 0, count: capacity)
        buf.withUnsafeMutableBufferPointer { body($0.baseAddress!, capacity) }
        return String(cString: buf)
    }

    // MARK: size-versioned structs

    /// A zeroed status snapshot with `size` set, ready for pin_get_status().
    static func makeStatusSnapshot() -> pin_status_snapshot_t {
        var s = pin_status_snapshot_t()
        s.size = UInt32(MemoryLayout<pin_status_snapshot_t>.size)
        return s
    }

    static func makeDeviceInfo() -> pin_device_info_t {
        var d = pin_device_info_t()
        d.size = UInt32(MemoryLayout<pin_device_info_t>.size)
        return d
    }

    static func makeEvent() -> pin_event_t {
        var e = pin_event_t()
        e.size = UInt32(MemoryLayout<pin_event_t>.size)
        return e
    }

    // MARK: devices

    /// All devices pin_enumerate() reports (retries once if there are more than the first buffer).
    static func enumerateDevices() -> [pin_device_info_t] {
        var capacity = 16
        while true {
            var list = [pin_device_info_t](repeating: makeDeviceInfo(), count: capacity)
            let total = Int(pin_enumerate(&list, Int32(capacity)))
            if total <= capacity { return Array(list.prefix(max(total, 0))) }
            capacity = total
        }
    }
}

extension pin_device_info_t {
    var idString: String { cString(id) }
    var nameString: String { cString(name) }
    var serialString: String { cString(serial) }
}
