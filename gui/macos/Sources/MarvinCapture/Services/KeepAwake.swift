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

/// Keeps the machine from idle-sleeping mid-capture (the display may still sleep); released as soon
/// as the capture ends. Port of the SetThreadExecutionState use on Windows.
@MainActor
final class KeepAwake {
    private var activity: NSObjectProtocol?

    var isActive: Bool { activity != nil }

    func set(_ on: Bool) {
        if on, activity == nil {
            activity = ProcessInfo.processInfo.beginActivity(
                // userInitiated also keeps App Nap away; sudden termination is named explicitly so a
                // logout / shutdown has to ask the app (the close flow) while a file is being written.
                options: [.idleSystemSleepDisabled, .userInitiated, .suddenTerminationDisabled],
                reason: "Capturing video")
        } else if !on, let a = activity {
            ProcessInfo.processInfo.endActivity(a)
            activity = nil
        }
    }
}
