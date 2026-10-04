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

// Pure AppKit entry point: gives full control of the NSWindow (close
// interception, frame autosave, min size) and of NSApplicationDelegate.
// SwiftUI lives inside the window (NSHostingController).
// main.swift runs on the main thread, but is nonisolated for the compiler.
MainActor.assumeIsolated {
    let app = NSApplication.shared
    let delegate = AppDelegate()
    app.delegate = delegate          // weak reference: keep `delegate` alive in this scope
    app.setActivationPolicy(.regular)
    app.run()
}
