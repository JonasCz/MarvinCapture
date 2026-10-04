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
import SwiftUI

/// The device picker as a real pop-up button: SwiftUI's Picker cannot give a menu item a second line,
/// NSMenuItem.subtitle can (name, then id · status and the USB-hub warning). Unusable devices (in use
/// by another window, no driver, ...) are disabled with the core's reason as their tooltip.
struct DevicePopUp: NSViewRepresentable {
    let devices: [DeviceItem]
    let selection: String?
    let enabled: Bool
    let placeholder: String
    let onSelect: (String) -> Void

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> NSPopUpButton {
        let b = NSPopUpButton(frame: .zero, pullsDown: false)
        b.menu?.autoenablesItems = false
        b.setContentHuggingPriority(.defaultLow, for: .horizontal)
        b.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        (b.cell as? NSPopUpButtonCell)?.lineBreakMode = .byTruncatingTail
        b.setAccessibilityLabel("Capture device")
        b.setAccessibilityHelp("Disabled while capturing. Devices used by another window can't be picked.")
        b.toolTip = "Capture device"
        return b
    }

    func sizeThatFits(_ proposal: ProposedViewSize, nsView: NSPopUpButton, context: Context) -> CGSize? {
        CGSize(width: proposal.width ?? 240, height: nsView.intrinsicContentSize.height)
    }

    func updateNSView(_ b: NSPopUpButton, context: Context) {
        context.coordinator.onSelect = onSelect
        b.isEnabled = enabled
        // Rebuild only when something visible changed: replacing the menu while it is open would close it.
        let signature = [placeholder, selection ?? "-"]
            + devices.map { "\($0.id)|\($0.name)|\($0.statusText)|\($0.isUsable)|\($0.hubDepth)" }
        guard signature != context.coordinator.signature else { return }
        context.coordinator.signature = signature

        let menu = NSMenu()
        menu.autoenablesItems = false
        let hasSelection = selection.map { s in devices.contains { $0.id == s } } ?? false
        if !hasSelection || devices.isEmpty {
            let p = NSMenuItem(title: placeholder, action: nil, keyEquivalent: "")
            p.isEnabled = false
            menu.addItem(p)
            if !devices.isEmpty { menu.addItem(.separator()) }
        }
        for d in devices {
            let item = NSMenuItem(title: d.name, action: #selector(Coordinator.changed(_:)), keyEquivalent: "")
            item.target = context.coordinator
            item.representedObject = d.id
            var sub = "\(d.shortId) · \(d.statusText)"
            if d.isBehindHub { sub += "\n\(d.hubLine)" }
            item.subtitle = sub
            item.isEnabled = d.isUsable
            item.toolTip = d.unavailableReason ?? d.hubHint
            menu.addItem(item)
        }
        b.menu = menu
        if hasSelection, let s = selection, let i = menu.items.firstIndex(where: { ($0.representedObject as? String) == s }) {
            b.selectItem(at: i)
        } else {
            b.selectItem(at: 0)
        }
    }

    @MainActor
    final class Coordinator: NSObject {
        var signature: [String] = []
        var onSelect: (String) -> Void = { _ in }

        @objc func changed(_ sender: NSMenuItem) {
            // Items carry their own action; the pop-up forwards the chosen item.
            if let id = sender.representedObject as? String { onSelect(id) }
        }
    }
}
