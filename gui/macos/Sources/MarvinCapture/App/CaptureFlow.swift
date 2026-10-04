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
import CMarvinCore

/// The capture start/stop clicks (port of Capture_Click / PlayAndCapture_Click / StartCaptureAsync):
/// the model plans the capture, this asks each confirmation as a sheet, in order, then starts.
@MainActor
enum CaptureFlow {
    /// True while the plan's sheets are open: a second click (or Dock menu item) must not plan again.
    private(set) static var starting = false

    /// Manual capture / analog Capture: stop (tape keeps running) or start.
    static func captureClicked(_ model: WindowModel) {
        if model.isCapturing { model.stopCapture(stopDeck: false); return }
        start(model, playFirst: false)
    }

    /// "Automatic rewind & capture": stop capture and tape, or start with the deck.
    static func playAndCaptureClicked(_ model: WindowModel) {
        if model.isCapturing { model.stopCapture(stopDeck: true); return }
        start(model, playFirst: true)
    }

    static func start(_ model: WindowModel, playFirst: Bool) {
        guard !starting else { return }
        starting = true
        Task { @MainActor in
            defer { starting = false }
            await run(model, playFirst: playFirst)
        }
    }

    private static func run(_ model: WindowModel, playFirst: Bool) async {
        guard let plan = model.planCapture(playFirst: playFirst) else { return }
        for c in plan.confirmations {
            let overwrite = c.primaryButton == "Overwrite"
            // A destructive confirmation defaults to Cancel (Return must not destroy a file).
            let choice = await Alerts.shared.ask(
                title: c.title, message: c.message,
                buttons: [.init(title: c.primaryButton, destructive: overwrite), .init(title: "Cancel")],
                defaultIndex: overwrite ? 1 : 0)
            if choice != 0 { return }
        }
        // The user may have stopped / changed things while a sheet was open; the core re-checks.
        model.startCapture(plan.opts, overwrite: plan.overwrite)
    }
}
