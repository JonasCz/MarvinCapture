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

/// "New Window": one window = one process (only one process may hold a device), so a new window is a
/// new instance of this app. Port of NewWindow_Click.
@MainActor
enum NewWindow {
    /// The menu / "…" menu action: opens a window, a failure goes to the info banner.
    static func open(reportingTo model: WindowModel?) {
        open { error in
            if let error { model?.showInfo("Could not open a new window", error, .error) }
        }
    }

    /// Returns an error text (for the info banner) or nil.
    static func open(completion: @escaping @MainActor (String?) -> Void) {
        let bundleURL = Bundle.main.bundleURL
        if bundleURL.pathExtension == "app" {
            let config = NSWorkspace.OpenConfiguration()
            config.createsNewApplicationInstance = true
            config.activates = true
            config.environment = [Instance.secondaryVariable: "1"]
            NSWorkspace.shared.openApplication(at: bundleURL, configuration: config) { _, error in
                let text = error?.localizedDescription
                Task { @MainActor in completion(text) }
            }
            return
        }
        // Not running from a bundle (bare binary during development): start the executable again.
        guard let exe = Bundle.main.executableURL else { completion("The executable path is unknown."); return }
        let p = Process()
        p.executableURL = exe
        p.environment = ProcessInfo.processInfo.environment.merging([Instance.secondaryVariable: "1"]) { _, new in new }
        do {
            try p.run()
            completion(nil)
        } catch {
            completion(error.localizedDescription)
        }
    }
}

/// Which MarvinCapture process this is. Every process autosaves its window under the same frame name,
/// so a second one would open exactly on top of the first; only the first instance may save the
/// frame, later ones read it and cascade from it by their ordinal.
@MainActor
enum Instance {
    /// Set by "New Window" for the process it starts: the only way to know for a bare binary (no bundle
    /// id), and a safeguard for a bundle whose first instance is not yet listed as running.
    static let secondaryVariable = "MARVIN_SECONDARY"

    /// 0 = the first running instance; n = n older instances of this app are running. Fixed at launch:
    /// if the first instance quits later, the others stay secondary (and keep not saving the frame).
    static let ordinal: Int = {
        var older = 0
        if let id = Bundle.main.bundleIdentifier {
            let me = NSRunningApplication.current
            for other in NSRunningApplication.runningApplications(withBundleIdentifier: id)
            where other.processIdentifier != me.processIdentifier {
                if let o = other.launchDate, let m = me.launchDate, o >= m { continue }
                older += 1
            }
        }
        if older == 0, ProcessInfo.processInfo.environment[secondaryVariable] == "1" { older = 1 }
        return older
    }()

    static var isPrimary: Bool { ordinal == 0 }
}
