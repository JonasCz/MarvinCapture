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

/// Developer aid for verifying the UI without screen-recording permission: with
/// `MARVIN_SNAPSHOT=/path/file.png` the main window's content view is rendered into that PNG every
/// 2 seconds (cacheDisplay, so it works while the window is hidden or behind others).
/// `MARVIN_SNAPSHOT_QUIT=seconds` quits the app after that long (with a last snapshot first).
/// Harmless in every build: without the variables nothing happens.
@MainActor
final class SnapshotHook {
    private let path: String
    private weak var window: NSWindow?
    private var timer: Timer?

    static func installIfRequested(window: NSWindow) -> SnapshotHook? {
        let env = ProcessInfo.processInfo.environment
        guard let path = env["MARVIN_SNAPSHOT"], !path.isEmpty else { return nil }
        let hook = SnapshotHook(path: path, window: window)
        hook.start(quitAfter: env["MARVIN_SNAPSHOT_QUIT"].flatMap(Double.init))
        return hook
    }

    private init(path: String, window: NSWindow) {
        self.path = path
        self.window = window
    }

    private func start(quitAfter: Double?) {
        let t = Timer(timeInterval: 2, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.snapshot() }
        }
        RunLoop.main.add(t, forMode: .common)
        timer = t
        // The first picture after layout has settled a little.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in self?.snapshot() }
        if let seconds = quitAfter {
            DispatchQueue.main.asyncAfter(deadline: .now() + seconds) { [weak self] in
                MainActor.assumeIsolated {
                    self?.snapshot()
                    NSApp.terminate(nil)
                }
            }
        }
    }

    private func snapshot() {
        guard let view = window?.contentView, view.bounds.width > 0 else { return }
        view.layoutSubtreeIfNeeded()
        guard let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { return }
        view.cacheDisplay(in: view.bounds, to: rep)
        let out = rep
        guard let png = out.representation(using: .png, properties: [:]) else { return }
        // Write to a temp name first: a reader never sees half a file.
        let tmp = path + ".tmp"
        do {
            try png.write(to: URL(fileURLWithPath: tmp))
            if rename(tmp, path) != 0 { throw POSIXError(.init(rawValue: errno) ?? .EIO) }
        } catch {
            FileHandle.standardError.write(Data("MARVIN_SNAPSHOT: \(error)\n".utf8))
        }
    }
}
