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

/// Which status-bar items fit (port of MainWindow.FitStatusBar). SwiftUI's Layout protocol cannot
/// remove a subview from the accessibility tree or the drawing, and "a hidden item takes no space and
/// does not exist" is what the Windows bar does, so the fit is decided here from the text widths and
/// the bar then simply contains the items that fit.
///
/// Drop order when the window is narrow: the free-space part of storage, frames, storage, time, deck.
/// The file text (always first) just truncates, down to `fileMinWidth`; signal, meters and mute stay.
enum StatusItem: Int, CaseIterable {
    case free = 1, frames, storage, time, deck   // raw value = drop order (1 goes first)
    case signal, meters, mute                    // never dropped
}

struct StatusBarMetrics {
    static let fileMinWidth: CGFloat = 80
    static let spacing: CGFloat = 24
    /// Between the storage size and its free-space part (they read as one item). The core's free text
    /// starts with "· ", so this is the whole gap before the dot: about one space, not two.
    static let innerSpacing: CGFloat = 4
    static let iconWidth: CGFloat = 14
    static let iconGap: CGFloat = 6
    static let meterBarWidth: CGFloat = 100
    static let muteWidth: CGFloat = 24
    /// Safety margin: the measured widths are of NSFont, SwiftUI lays out with its own rounding.
    static let slack: CGFloat = 8

    /// The one status font: small system size, tabular digits so numbers don't jitter.
    static let nsFont = NSFont.monospacedDigitSystemFont(ofSize: NSFont.smallSystemFontSize, weight: .regular)

    /// Measuring text is the costly part of the fit and the texts rarely change: remember the widths.
    @MainActor private static var widthCache: [String: CGFloat] = [:]

    @MainActor
    static func textWidth(_ s: String) -> CGFloat {
        if let w = widthCache[s] { return w }
        let w = ceil((s as NSString).size(withAttributes: [.font: nsFont]).width)
        if widthCache.count > 256 { widthCache.removeAll() }
        widthCache[s] = w
        return w
    }

    @MainActor static func iconTextWidth(_ s: String) -> CGFloat { iconWidth + iconGap + textWidth(s) }

    /// The items to show for the texts now (deck on DV/HDV input only, storage with disk info only),
    /// dropping the lowest-priority ones until they fit next to the file text's minimum width.
    @MainActor
    static func visibleItems(model: WindowModel, availableWidth: CGFloat) -> Set<StatusItem> {
        var widths: [StatusItem: CGFloat] = [
            .time: spacing + iconTextWidth(model.statusTimeText),
            .signal: spacing + iconTextWidth(signalText(model)),
            .frames: spacing + iconTextWidth(model.framesTotalText),
            .meters: spacing + 6 + 10 + meterBarWidth,
            .mute: 8 + muteWidth,
        ]
        if model.isDvInput {
            widths[.deck] = spacing + iconGap + iconWidth + textWidth(model.deckStateText)
        }
        if model.hasDiskInfo {
            widths[.storage] = spacing + iconTextWidth(model.sizeText)
            if !model.storageFreeText.isEmpty { widths[.free] = innerSpacing + textWidth(model.storageFreeText) }
        }
        var need = fileMinWidth + slack + widths.values.reduce(0, +)
        var visible = Set(widths.keys)
        for item in StatusItem.allCases.filter({ $0.rawValue <= StatusItem.deck.rawValue }) where need > availableWidth {
            if let w = widths[item] {
                need -= w
                visible.remove(item)
            }
        }
        return visible
    }

    @MainActor
    static func signalText(_ m: WindowModel) -> String {
        m.signalTypeText.isEmpty ? m.signalLockText : m.signalLockText + " · " + m.signalTypeText
    }
}
