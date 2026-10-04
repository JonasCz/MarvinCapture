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

import AppKit

/// "New Window": one window = one process (only one process may hold a device), so a new window is a
/// new instance of this app. Port of NewWindow_Click.
@MainActor
enum NewWindow {
    /// Returns an error text (for the info banner) or nil.
    static func open(completion: @escaping @MainActor (String?) -> Void) {
        let bundleURL = Bundle.main.bundleURL
        if bundleURL.pathExtension == "app" {
            let config = NSWorkspace.OpenConfiguration()
            config.createsNewApplicationInstance = true
            config.activates = true
            NSWorkspace.shared.openApplication(at: bundleURL, configuration: config) { _, error in
                let text = error?.localizedDescription
                Task { @MainActor in completion(text) }
            }
            return
        }
        // Not running from a bundle (bare binary during development): start the executable again.
        guard let exe = Bundle.main.executableURL else { completion("The executable path is unknown."); return }
        let p = Process()
        p.executableURL = exe
        do {
            try p.run()
            completion(nil)
        } catch {
            completion(error.localizedDescription)
        }
    }
}
