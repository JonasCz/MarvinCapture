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

/// Helpers for the fixed-size `char[N]` fields of the C structs, which Swift
/// imports as N-tuples of CChar. Generic over the tuple type, so they work for
/// every size (PIN_NAME_MAX, PIN_TEXT_MAX, PIN_PATH_MAX, ...).

/// Reads a NUL-terminated UTF-8 string out of a char-array tuple.
/// Invalid UTF-8 is replaced, never crashes. A missing terminator reads the whole array.
func cString<T>(_ chars: T) -> String {
    withUnsafeBytes(of: chars) { raw in
        String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self)
    }
}

/// Writes `value` into a char-array tuple as UTF-8, truncated on a character
/// boundary so that the terminator always fits; the rest is zero-filled.
func setCString<T>(_ chars: inout T, _ value: String) {
    withUnsafeMutableBytes(of: &chars) { raw in
        guard raw.count > 0 else { return }
        raw.initializeMemory(as: UInt8.self, repeating: 0)
        var utf8 = Array(value.utf8)
        if utf8.count > raw.count - 1 {
            var n = raw.count - 1
            while n > 0 && (utf8[n] & 0xC0) == 0x80 { n -= 1 }   // don't cut a multi-byte sequence
            utf8.removeSubrange(n...)
        }
        for (i, b) in utf8.enumerated() { raw[i] = b }
    }
}
