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

    /// Return false to veto closing (the close flow asks about a running capture).
    var shouldClose: (() -> Bool)?
    private let model: AppModel?

    init(startup: Startup.Result, model: AppModel?) {
        self.model = model
        let content: AnyView
        switch startup {
        case .ok: content = AnyView(MainView(app: model!))
        case .failed(let message): content = AnyView(StartupErrorView(message: message))
        }
        let window = NSWindow(
            contentRect: NSRect(origin: .zero, size: NSSize(width: 1100, height: 700)),
            styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
            backing: .buffered, defer: false)
        window.title = "MarvinCapture"
        // Finder-like: a transparent title bar, the content (the sidebar material) runs up under it. The
        // SwiftUI safe area keeps the controls clear of the traffic lights.
        window.titlebarAppearsTransparent = true
        window.titlebarSeparatorStyle = .none
        window.contentViewController = NSHostingController(rootView: content)
        window.contentMinSize = Self.minContentSize
        window.isReleasedWhenClosed = false
        super.init(window: window)
        window.delegate = self
        // After the content is set, so the saved frame wins; centre on first launch.
        window.setContentSize(NSSize(width: 1100, height: 700))
        window.center()
        if Instance.isPrimary {
            window.setFrameAutosaveName(Self.frameAutosaveName)
        } else {
            // Another instance is running: start from the saved frame but never write it (setting the
            // autosave name would), offset 30 pt per older instance, kept on screen.
            window.setFrameUsingName(Self.frameAutosaveName)
            let step = CGFloat(30 * (Instance.ordinal % 8))
            var frame = window.frame.offsetBy(dx: step, dy: -step)
            if let screen = window.screen ?? NSScreen.main { frame = window.constrainFrameRect(frame, to: screen) }
            window.setFrame(frame, display: false)
        }
        // Developer aid: MARVIN_WINDOW_SIZE=WxH forces the content size (and lifts the minimum), to
        // look at narrow layouts and take snapshots of a known size.
        if let s = ProcessInfo.processInfo.environment["MARVIN_WINDOW_SIZE"] {
            let p = s.lowercased().split(separator: "x").compactMap { Double($0) }
            if p.count == 2 {
                window.contentMinSize = NSSize(width: 400, height: 300)
                window.setContentSize(NSSize(width: p[0], height: p[1]))
            }
        }
        // Developer aid: MARVIN_APPEARANCE=dark|light (the -AppleInterfaceStyle argument is not honoured
        // by every AppKit path, and is dropped from the command line anyway).
        if let a = ProcessInfo.processInfo.environment["MARVIN_APPEARANCE"] {
            window.appearance = NSAppearance(named: a.lowercased() == "dark" ? .darkAqua : .aqua)
        }
        trackWindowTitle()
        trackVisibility()
    }

    private var visibilityObservers: [NSObjectProtocol] = []

    /// Tells the model when nothing of the window can be seen (minimised, covered, app hidden).
    private func trackVisibility() {
        guard let model, let w = window else { return }
        let update: @Sendable (Notification) -> Void = { [weak self, weak w] _ in
            MainActor.assumeIsolated {
                guard let w else { return }
                _ = self
                model.window.setUIVisible(w.occlusionState.contains(.visible) && !w.isMiniaturized && !NSApp.isHidden)
            }
        }
        let nc = NotificationCenter.default
        for n in [NSWindow.didChangeOcclusionStateNotification, NSWindow.didMiniaturizeNotification, NSWindow.didDeminiaturizeNotification] {
            visibilityObservers.append(nc.addObserver(forName: n, object: w, queue: .main, using: update))
        }
        for n in [NSApplication.didHideNotification, NSApplication.didUnhideNotification] {
            visibilityObservers.append(nc.addObserver(forName: n, object: nil, queue: .main, using: update))
        }
    }

    /// The title bar shows the model's window title (device, state, file); re-arms after every change.
    private func trackWindowTitle() {
        guard let model else { return }
        withObservationTracking {
            let t = model.window.windowTitle
            if window?.title != t { window?.title = t }
        } onChange: { [weak self] in
            DispatchQueue.main.async { MainActor.assumeIsolated { self?.trackWindowTitle() } }
        }
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    func windowShouldClose(_ sender: NSWindow) -> Bool { shouldClose?() ?? true }
}
