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

/// Startup probe (port of the WinUI App.OnLaunched check). If the core dylib
/// is missing the process dies in dyld before this runs; a version mismatch
/// is reported in the window instead of the UI.
enum Startup {
    enum Result {
        case ok
        case failed(String)
    }

    static func run() -> Result {
        guard Pin.apiVersionMatches else {
            return .failed("""
                The core library (libmarvin-core.dylib) has API version \(Pin.apiVersion), \
                but this app was built for version \(Pin.expectedApiVersion).

                The app and the core library do not belong together. Rebuild with \
                scripts/build.sh, or reinstall the app.

                Core: \(Pin.versionString)
                """)
        }
        // Firmware lives in Contents/Resources/firmware (non-code files in
        // Contents/Frameworks would break codesign).
        if let res = Bundle.main.resourceURL {
            let fw = res.appendingPathComponent("firmware", isDirectory: true)
            var isDir: ObjCBool = false
            if FileManager.default.fileExists(atPath: fw.path, isDirectory: &isDir), isDir.boolValue {
                Pin.setFirmwareDir(fw.path)
            }
        }
        return .ok
    }
}
