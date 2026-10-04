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
import UserNotifications

/// User notifications for a capture that ended while the app is in the background (a user waiting
/// for a tape wants to know). Authorization is requested lazily, at the first capture start.
/// UNUserNotificationCenter needs a real bundle: run as a bare binary from .build it does nothing.
@MainActor
final class Notifier {
    static let shared = Notifier()

    private var requested = false
    private var available: Bool { Bundle.main.bundleIdentifier != nil }

    /// Asks for .alert and .sound once; call when a capture starts.
    func requestAuthorizationIfNeeded() {
        guard available, !requested else { return }
        requested = true
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { _, _ in }
    }

    /// Posts only while the app is not the active application (otherwise the window shows it).
    func postIfInactive(title: String, body: String) {
        guard available, !NSApp.isActive else { return }
        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
        content.sound = .default
        let request = UNNotificationRequest(identifier: UUID().uuidString, content: content, trigger: nil)
        UNUserNotificationCenter.current().add(request) { _ in }
    }
}
