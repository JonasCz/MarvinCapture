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

/// A section of the sidebar (the Windows CardStyle): grouped controls straight on the sidebar surface,
/// like Finder's sidebar. No box: the sections are told apart by spacing and a hairline `Divider`.
struct Card<Content: View>: View {
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 12) { content }
            .frame(maxWidth: .infinity, alignment: .leading)
    }
}

/// Section heading ("Output", "Signal", "Tape"): small, semibold, secondary, like a Finder sidebar header.
struct CardHeader: View {
    let title: String
    init(_ title: String) { self.title = title }
    var body: some View {
        Text(title).font(.subheadline.weight(.semibold)).foregroundStyle(.secondary)
            .accessibilityAddTraits(.isHeader)
    }
}

/// The sidebar's background: the system sidebar material (what Finder uses), which follows light / dark
/// mode and the window's active state on its own.
struct SidebarMaterial: NSViewRepresentable {
    func makeNSView(context: Context) -> NSVisualEffectView {
        let v = NSVisualEffectView()
        v.material = .sidebar
        v.blendingMode = .behindWindow
        v.state = .followsWindowActiveState
        return v
    }

    func updateNSView(_ nsView: NSVisualEffectView, context: Context) {}
}

/// A control with a small caption above it (the Windows "Header").
struct Field<Content: View>: View {
    let title: String
    @ViewBuilder var content: Content

    init(_ title: String, @ViewBuilder content: () -> Content) {
        self.title = title
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(title).font(.caption).foregroundStyle(.secondary)
            content
        }
    }
}

/// Integer entry with a stepper (the Windows NumberBox): clamped to the range, tooltip = the help.
struct NumberField: View {
    let title: String
    @Binding var value: Int
    var range: ClosedRange<Int> = 0...600
    let help: String
    var width: CGFloat = 60
    var accessibilityName: String?   // when `title` is a shortened label

    var body: some View {
        let clamped = Binding<Int>(
            get: { value },
            set: { value = Swift.min(Swift.max($0, range.lowerBound), range.upperBound) })
        Field(title) {
            HStack(spacing: 6) {
                TextField("", value: clamped, format: .number)
                    .textFieldStyle(.roundedBorder)
                    .multilineTextAlignment(.trailing)
                    .frame(width: width)
                    .accessibilityLabel(accessibilityName ?? title)
                Stepper("", value: clamped, in: range)
                    .labelsHidden()
                    .accessibilityLabel(accessibilityName ?? title)
            }
        }
        .help(help)
        .accessibilityHint(help)
    }
}

/// Name entry plus the folder button (analog and DV share the layout).
struct OutputNameRow: View {
    @Binding var name: String
    let directory: String
    let nameLabel: String
    let nameHelp: String
    let folderLabel: String
    let onChooseFolder: () -> Void

    var body: some View {
        HStack(spacing: 8) {
            TextField("Name and title", text: $name)
                .textFieldStyle(.roundedBorder)
                .accessibilityLabel(nameLabel)
                .accessibilityHint(nameHelp)
                .help(nameHelp)
            Button(action: onChooseFolder) {
                Image(systemName: "folder").frame(width: 18)
            }
            .help(directory.isEmpty ? "Choose the output folder" : directory)   // tooltip = the current folder
            .accessibilityLabel(folderLabel)
            .accessibilityValue(directory)
        }
    }
}

/// The big action button: icon, title and an optional caption. Prominent = the primary action.
struct CaptureButton: View {
    let title: String
    var caption: String?
    let symbol: String
    var prominent = false
    var destructive = false   // a Stop: red
    var help: String?
    let action: () -> Void

    var body: some View {
        let label = Button(action: action) {
            HStack(spacing: 12) {
                Image(systemName: symbol).font(.system(size: 20)).frame(width: 28)
                VStack(alignment: .leading, spacing: 1) {
                    Text(title).fontWeight(.semibold)
                    if let caption { Text(caption).font(.caption).opacity(0.85) }
                }
                Spacer(minLength: 0)
            }
            .padding(.vertical, 4)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .controlSize(.large)
        .tint(destructive ? .red : nil)
        .accessibilityLabel(title)
        if prominent { label.buttonStyle(.borderedProminent).help(help ?? "") }
        else { label.buttonStyle(.bordered).help(help ?? "") }
    }
}
