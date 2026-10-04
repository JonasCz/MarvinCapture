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

/// Seams for the parts that are separate steps: the Metal preview and the audio monitor. The
/// window model calls these on open / close / mute; the stubs do nothing.

/// The video preview. Attached right after a session opens, detached BEFORE it closes (never use
/// the session after detach).
@MainActor
protocol PreviewSink: AnyObject {
    func attach(session: Pin.Session)
    func detach()
    /// Display aspect of the frames being shown (the core's value with the aspect override applied),
    /// nil until a frame arrived. The window sizes the preview frame from it.
    var frameDar: (num: Int, den: Int)? { get }
    /// A frame was presented within the last second (the window's "no video" overlay follows this).
    var hasRecentFrame: Bool { get }
}

/// The audio monitor (pin_monitor_read pump into the system output). The model owns the mute state
/// and pin_monitor_enable; this only plays what the core's ring holds.
@MainActor
protocol AudioMonitor: AnyObject {
    var isMuted: Bool { get set }
    /// A session opened; `start()` follows when not muted.
    func attach(session: Pin.Session)
    func start()
    func stop()
    /// The session is about to close.
    func detach()
}

@MainActor
final class NullPreviewSink: PreviewSink {
    var frameDar: (num: Int, den: Int)? { nil }
    var hasRecentFrame: Bool { false }
    func attach(session: Pin.Session) {}
    func detach() {}
}

@MainActor
final class NullAudioMonitor: AudioMonitor {
    var isMuted = true
    func attach(session: Pin.Session) {}
    func start() {}
    func stop() {}
    func detach() {}
}
