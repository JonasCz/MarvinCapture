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

/// The standard About panel with the core's version in the credits (port of About_Click).
@MainActor
enum About {
    static func show() {
        let text = "\(Pin.versionString) (API \(Pin.apiVersion))\n\n"
            + "Open driver for the Pinnacle Studio 500-USB.\n"
            + "Licensed under the GNU Affero General Public License v3.0."
        let para = NSMutableParagraphStyle()
        para.alignment = .center
        let credits = NSAttributedString(string: text, attributes: [
            .font: NSFont.systemFont(ofSize: NSFont.smallSystemFontSize),
            .foregroundColor: NSColor.labelColor,
            .paragraphStyle: para,
        ])
        var options: [NSApplication.AboutPanelOptionKey: Any] = [.credits: credits]
        // Unbundled (development) runs have no version: the panel would show "(null)".
        if let v = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String {
            options[.applicationVersion] = v
        } else {
            options[.applicationVersion] = "development"
        }
        NSApp.activate()
        NSApp.orderFrontStandardAboutPanel(options: options)
    }
}
