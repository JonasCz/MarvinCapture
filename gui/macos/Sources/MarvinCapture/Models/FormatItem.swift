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

/// One output format (pin_format_info_t) with plain properties for pickers.
struct FormatItem: Identifiable, Hashable {
    var format: pin_format_t
    var kind: pin_kind_t
    var label: String
    var fileExtension: String
    var supportsTitle: Bool
    var supportsSceneSplit: Bool
    var supportsMultiPass: Bool
    var isDefault: Bool

    var id: UInt32 { format.rawValue }

    init(_ info: pin_format_info_t) {
        format = info.format
        kind = info.kind
        label = cString(info.label)
        fileExtension = cString(info.extension)
        supportsTitle = info.supports_title != 0
        supportsSceneSplit = info.supports_scene_split != 0
        supportsMultiPass = info.supports_multi_pass != 0
        isDefault = info.is_default != 0
    }

    static func == (a: FormatItem, b: FormatItem) -> Bool { a.format == b.format }
    func hash(into h: inout Hasher) { h.combine(format.rawValue) }

    /// The formats of a kind in the core's display order.
    static func formats(for kind: pin_kind_t) -> [FormatItem] { Pin.formats(kind).map(FormatItem.init) }
}

/// An analog standard entry ("Auto", "PAL", ...).
struct StdItem: Identifiable, Hashable {
    var std: pin_std_t
    var name: String
    var id: UInt32 { std.rawValue }

    static func == (a: StdItem, b: StdItem) -> Bool { a.std == b.std }
    func hash(into h: inout Hasher) { h.combine(std.rawValue) }
}
