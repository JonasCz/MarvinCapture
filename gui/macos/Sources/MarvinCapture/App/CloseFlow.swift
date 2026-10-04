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

/// Closing or quitting while a capture runs (port of AppWindow_Closing / FinishClose): ask, stop the
/// capture, show "Finalizing files…" and leave once the core is READY again (files are finalised).
/// One window = one process, so closing the window and quitting are the same thing.
@MainActor
final class CloseFlow {
    private let model: WindowModel
    private var asking = false
    /// Quit was asked through ⌘Q: answer AppKit's pending terminate request when done.
    private var replyPending = false
    /// The capture is finalised and the user agreed: the next terminate request goes through without asking
    /// again (the model's `isCapturing` can lag a tick behind the state the close flow saw).
    private var approved = false

    init(model: WindowModel) {
        self.model = model
        model.afterTick = { [weak self] in self?.afterTick() }
    }

    /// NSWindowDelegate.windowShouldClose.
    func windowShouldClose() -> Bool {
        if model.isFinalizingForClose { return false }   // already stopping; the app quits itself
        if !model.isCapturing { return true }
        confirmAndFinalize(viaTerminate: false)
        return false
    }

    /// NSApplicationDelegate.applicationShouldTerminate.
    func shouldTerminate() -> NSApplication.TerminateReply {
        if approved { return .terminateNow }
        if model.isFinalizingForClose {
            replyPending = true
            return .terminateLater
        }
        if !model.isCapturing { return .terminateNow }
        confirmAndFinalize(viaTerminate: true)
        return .terminateLater
    }

    private func confirmAndFinalize(viaTerminate: Bool) {
        if viaTerminate { replyPending = true }
        guard !asking else { return }
        asking = true
        Task { @MainActor in
            defer { asking = false }
            let r = await Alerts.shared.ask(
                title: "Stop capture and quit?",
                message: "A capture is running. Stopping finishes the current file safely before the window closes.",
                buttons: [.init(title: "Stop and quit"), .init(title: "Keep capturing")])
            guard r == 0 else { cancelled(); return }
            if !model.isCapturing {   // it ended while the sheet was open
                finish()
                return
            }
            model.isFinalizingForClose = true
            model.stopCapture()
            // afterTick calls finish() once the core reports READY.
        }
    }

    private func cancelled() {
        if replyPending {
            replyPending = false
            NSApp.reply(toApplicationShouldTerminate: false)
        }
    }

    private func afterTick() {
        guard model.isFinalizingForClose else { return }
        switch model.sessionState {
        case PIN_STATE_READY, PIN_STATE_ERROR, PIN_STATE_CLOSED: finish()
        default: break
        }
    }

    private func finish() {
        model.isFinalizingForClose = false
        if replyPending {
            replyPending = false
            NSApp.reply(toApplicationShouldTerminate: true)
        } else {
            // The window route: terminating runs shouldTerminate again, which now says yes.
            approved = true
            NSApp.terminate(nil)
        }
    }
}
