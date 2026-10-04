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
import SwiftUI

/// Owns the single main window (one window per process). The SwiftUI content
/// is hosted in an NSHostingController.
@MainActor
final class MainWindowController: NSWindowController, NSWindowDelegate {
    static let frameAutosaveName = "MarvinCaptureMain"
    static let minContentSize = NSSize(width: 1000, height: 640)

    /// Return false to veto closing (later: "stop the capture first?").
    var shouldClose: (() -> Bool)?

    init(startup: Startup.Result) {
        let content: AnyView
        switch startup {
        case .ok: content = AnyView(ContentView())
        case .failed(let message): content = AnyView(StartupErrorView(message: message))
        }
        let window = NSWindow(
            contentRect: NSRect(origin: .zero, size: NSSize(width: 1100, height: 700)),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered, defer: false)
        window.title = "MarvinCapture"
        window.contentViewController = NSHostingController(rootView: content)
        window.contentMinSize = Self.minContentSize
        window.isReleasedWhenClosed = false
        super.init(window: window)
        window.delegate = self
        // After the content is set, so the saved frame wins; centre on first launch.
        window.setContentSize(NSSize(width: 1100, height: 700))
        window.center()
        window.setFrameAutosaveName(Self.frameAutosaveName)
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    func windowShouldClose(_ sender: NSWindow) -> Bool { shouldClose?() ?? true }
}
