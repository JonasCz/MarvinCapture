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

/// The one-line footer (port of the Windows status bar). One font for every text, secondary colour.
/// Left to right: file/status | deck | time | signal | frames | storage | meters | mute, 24 pt apart.
/// Too narrow: items drop in the order of `StatusBarMetrics` and leave no gap.
struct StatusBar: View {
    let model: WindowModel

    private typealias M = StatusBarMetrics
    private static let barPadding = EdgeInsets(top: 4, leading: 16, bottom: 4, trailing: 8)

    var body: some View {
        GeometryReader { geo in
            let shown = M.visibleItems(model: model, availableWidth: geo.size.width - 24)
            HStack(spacing: 0) {
                FileItem(model: model)
                    .frame(minWidth: M.fileMinWidth, maxWidth: .infinity, alignment: .leading)
                if shown.contains(.deck) { DeckItem(model: model).padding(.leading, M.spacing) }
                if shown.contains(.time) { TimeItem(model: model).padding(.leading, M.spacing) }
                SignalItem(model: model).padding(.leading, M.spacing)
                if shown.contains(.frames) { FramesItem(model: model).padding(.leading, M.spacing) }
                if shown.contains(.storage) { StorageItem(model: model).padding(.leading, M.spacing) }
                if shown.contains(.free) { FreeItem(model: model).padding(.leading, M.innerSpacing) }
                LevelMeters(model: model).padding(.leading, M.spacing)
                MuteButton(model: model).padding(.leading, 8)
            }
            .padding(Self.barPadding)
            .frame(width: geo.size.width, height: geo.size.height)
        }
        .frame(height: 34)
        .font(.system(size: NSFont.smallSystemFontSize))
        .monospacedDigit()
        .foregroundStyle(.secondary)
        .background(.bar)
    }
}

private struct StatusIcon: View {
    let name: String
    var color: Color?
    var body: some View {
        Image(systemName: name)
            .font(.system(size: 11))
            .foregroundStyle(color ?? .secondary)
            .frame(width: StatusBarMetrics.iconWidth)
            .accessibilityHidden(true)
    }
}

private struct FileItem: View {
    let model: WindowModel
    var body: some View {
        HStack(spacing: 6) {
            if model.isCapturing {
                Circle().fill(.red).frame(width: 8, height: 8).accessibilityHidden(true)
            }
            Text(model.statusShortText)
                .lineLimit(1).truncationMode(.tail)
                .accessibilityLabel(model.statusLine)
        }
        .help(model.statusTip)
    }
}

private struct DeckItem: View {
    let model: WindowModel
    var body: some View {
        HStack(spacing: StatusBarMetrics.iconGap) {
            StatusIcon(name: "recordingtape")
            // The invisible busy-width text reserves room for the trailing " …".
            ZStack(alignment: .leading) {
                Text(model.deckStateReserveText).hidden()
                Text(model.deckStateText)
            }
        }
        .fixedSize()
        .help("Deck status")
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("Deck: \(model.deckStateText)")
    }
}

private struct TimeItem: View {
    let model: WindowModel
    var body: some View {
        HStack(spacing: StatusBarMetrics.iconGap) {
            StatusIcon(name: "clock")
            Text(model.statusTimeText)
        }
        .fixedSize()
        .help(model.statusTimeTip)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("\(model.statusTimeTip): \(model.statusTimeText)")
    }
}

private struct SignalItem: View {
    let model: WindowModel
    var body: some View {
        HStack(spacing: StatusBarMetrics.iconGap) {
            if model.signalLocked { StatusIcon(name: "checkmark.circle.fill", color: .green) }
            else { StatusIcon(name: "exclamationmark.triangle.fill", color: .yellow) }
            Text(StatusBarMetrics.signalText(model))
        }
        .fixedSize()
        .help("Signal: lock state, source type and video format")
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(model.signalTypeText.isEmpty
                            ? "Signal: \(model.signalLockText)"
                            : "Signal: \(model.signalLockText), \(model.signalTypeText)")
    }
}

private struct FramesItem: View {
    let model: WindowModel
    var body: some View {
        HStack(spacing: StatusBarMetrics.iconGap) {
            StatusIcon(name: "film")
            Text(model.framesTotalText)
        }
        .fixedSize()
        .help(model.framesTip)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(model.framesTip)
    }
}

/// Storage: icon and the size written; the free space follows as `FreeItem` (it drops first).
private struct StorageItem: View {
    let model: WindowModel
    var body: some View {
        HStack(spacing: StatusBarMetrics.iconGap) {
            StatusIcon(name: "internaldrive", color: model.diskLow ? .red : nil)
            Text(model.sizeText).foregroundStyle(model.diskLow ? Color.red : Color.secondary)
        }
        .fixedSize()
        .help(model.storageTip)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel((model.diskLow ? "Low disk space. " : "Storage: ") + model.storageTip)
    }
}

private struct FreeItem: View {
    let model: WindowModel
    var body: some View {
        Text(model.storageFreeText)
            .foregroundStyle(model.diskLow ? Color.red : Color.secondary)
            .fixedSize()
            .help(model.storageTip)
            .accessibilityHidden(true)   // StorageItem's label already carries the whole tip
    }
}

private struct MuteButton: View {
    let model: WindowModel
    var body: some View {
        Button { model.isMuted.toggle() } label: {
            Image(systemName: model.isMuted ? "speaker.slash.fill" : "speaker.wave.2.fill")
                .font(.system(size: 13))
                .foregroundStyle(.primary)
                .frame(width: StatusBarMetrics.muteWidth, height: 22)
                .contentShape(Rectangle())
        }
        .buttonStyle(.borderless)
        .help(model.muteActionName)
        .accessibilityLabel(model.muteActionName)
        .accessibilityHint("Audio monitoring of the source")
    }
}
