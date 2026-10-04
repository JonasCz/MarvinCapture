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

/// Folder chooser sheet (port of Browse_Click). Starts in the current folder.
@MainActor
enum FolderPicker {
    static func choose(startingAt dir: String, in window: NSWindow?, completion: @escaping (String) -> Void) {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.allowsMultipleSelection = false
        panel.prompt = "Choose"
        if !dir.isEmpty { panel.directoryURL = URL(fileURLWithPath: dir, isDirectory: true) }
        let handle: (NSApplication.ModalResponse) -> Void = { r in
            if r == .OK, let url = panel.url { completion(url.path) }
        }
        if let w = window ?? NSApp.keyWindow { panel.beginSheetModal(for: w, completionHandler: handle) }
        else { panel.begin(completionHandler: handle) }
    }
}
