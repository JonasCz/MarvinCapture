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

import SwiftUI

/// A "card": grouped controls on the window surface (the Windows CardStyle). A rounded
/// quaternary fill with a hairline, which follows light / dark mode on its own.
struct Card<Content: View>: View {
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 12) { content }
            .padding(.horizontal, 14).padding(.top, 12).padding(.bottom, 14)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 10).fill(.quaternary.opacity(0.6)))
            .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(.separator))
    }
}

/// Card heading ("Output", "Signal", "Tape").
struct CardHeader: View {
    let title: String
    init(_ title: String) { self.title = title }
    var body: some View { Text(title).font(.headline) }
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
                    .accessibilityLabel(title)
                Stepper("", value: clamped, in: range)
                    .labelsHidden()
                    .accessibilityLabel(title)
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
            .help(directory)
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
