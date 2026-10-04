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

/// Window-modal alerts (NSAlert sheets), one at a time. Port of ShowDialogAsync: Windows allows one
/// ContentDialog per XamlRoot and brings the window to the front first; a sheet needs the same two
/// rules (a second sheet on a window that already has one would be refused or stacked wrongly).
@MainActor
final class Alerts {
    static let shared = Alerts()

    struct Button {
        var title: String
        /// Red "destructive" look (Overwrite, Delete, ...).
        var destructive = false
    }

    private var busy = false
    private var waiters: [CheckedContinuation<Void, Never>] = []

    /// The window sheets attach to. Set by the window controller; falls back to any visible window.
    weak var window: NSWindow?

    /// Shows the alert as a sheet and returns the index of the clicked button. Buttons are in the
    /// usual order (first = right-most / default unless `defaultIndex` says otherwise). Alerts asked
    /// while one is open wait their turn.
    func ask(title: String, message: String, buttons: [Button], defaultIndex: Int = 0,
             bringForward: Bool = true) async -> Int {
        await acquire()
        defer { release() }
        if bringForward { bringToFront() }

        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = title
        alert.informativeText = message
        for b in buttons {
            let nb = alert.addButton(withTitle: b.title)
            nb.hasDestructiveAction = b.destructive
        }
        // Return = the default button, Escape = the last one ("Cancel", "Keep capturing", ...).
        for (i, nb) in alert.buttons.enumerated() {
            nb.keyEquivalent = i == defaultIndex ? "\r" : (i == buttons.count - 1 && buttons.count > 1 ? "\u{1b}" : "")
        }
        guard let host = targetWindow else {
            let r = alert.runModal()
            return max(0, r.rawValue - NSApplication.ModalResponse.alertFirstButtonReturn.rawValue)
        }
        let r = await alert.beginSheetModal(for: host)
        return max(0, r.rawValue - NSApplication.ModalResponse.alertFirstButtonReturn.rawValue)
    }

    /// An information alert with a single OK. `bringForward: false` leaves the app in the background
    /// (the Dock bounce calls the user back) instead of taking the focus.
    func notify(title: String, message: String, bringForward: Bool = true) async {
        _ = await ask(title: title, message: message, buttons: [Button(title: "OK")], bringForward: bringForward)
    }

    // MARK: serialisation

    private func acquire() async {
        if !busy { busy = true; return }
        await withCheckedContinuation { waiters.append($0) }   // busy stays true: ownership is handed over
    }

    private func release() {
        if waiters.isEmpty { busy = false } else { waiters.removeFirst().resume() }
    }

    // MARK: window

    private var targetWindow: NSWindow? {
        if let w = window, w.isVisible || w.isMiniaturized { return w }
        return NSApp.windows.first { $0.isVisible && $0.canBecomeMain }
    }

    /// A capture started from the Dock menu can reach a dialog while the window is behind others or
    /// minimised: show the window before the sheet.
    private func bringToFront() {
        guard let w = targetWindow else { return }
        if w.isMiniaturized { w.deminiaturize(nil) }
        if !NSApp.isActive { NSApp.activate() }
        if !w.isKeyWindow { w.makeKeyAndOrderFront(nil) }
    }
}
