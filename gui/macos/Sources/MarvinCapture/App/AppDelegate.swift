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

/// Application delegate. Later steps add applicationDockMenu, the capture
/// confirmation in applicationShouldTerminate, URL/file opening, etc.
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private(set) var windowController: MainWindowController?

    func applicationDidFinishLaunching(_ notification: Notification) {
        MainMenu.install()
        let wc = MainWindowController(startup: Startup.run())
        windowController = wc
        wc.showWindow(nil)
        NSApp.activate()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        // Later: ask for confirmation while capturing.
        .terminateNow
    }
}
