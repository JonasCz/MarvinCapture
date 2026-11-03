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

import Foundation
import Observation
import CMarvinCore

/// The per-kind (DV, HDV) file options: format (its settings behind the gear: `formatOptions`), scene
/// split, no-signal timeout, time limit, pass count, aspect. Port of KindSettingsViewModel.
@MainActor @Observable
final class KindSettingsModel: Identifiable {
    enum Field { case format, split, idle, duration, passes, aspect }

    let kind: pin_kind_t
    let tabHeader: String
    /// Empty until the encoder probe is done (`refreshFormats`): pin_formats() would run it on the main thread.
    private(set) var formats: [FormatItem] = []
    /// The selected format's settings (deinterlace, quality, ...), behind the gear next to the picker.
    let formatOptions: FormatOptionsModel
    /// A saved or command-line format the list may not have yet: the H.264 / HEVC entries only appear
    /// once the encoder probe is done.
    @ObservationIgnored private var wantedFormat: pin_format_t?
    nonisolated var id: UInt32 { kind.rawValue }

    /// Called after any option changed (the window model saves it and updates the preview / hint).
    @ObservationIgnored var onChanged: ((KindSettingsModel, Field) -> Void)?

    var selectedFormat: FormatItem? = nil {
        didSet {
            formatOptions.format = selectedFormat?.format
            onChanged?(self, .format)
        }
    }
    var splitIntoScenes = false { didSet { onChanged?(self, .split) } }
    /// Stop after this long without signal (minutes); 0 = never.
    var idleStopMinutes = 5 { didSet { onChanged?(self, .idle) } }
    /// Stop after this long of capture time (minutes), signal or not; 0 = never.
    var maxDurationMinutes = 0 { didSet { onChanged?(self, .duration) } }
    /// The user's own pass count; kept (and saved) while the entry is greyed out, so it comes back.
    var passes = 1 { didSet { onChanged?(self, .passes) } }
    /// Aspect override (pin_aspect_t order: Auto, 4:3, 16:9).
    var aspectIndex = 0 { didSet { onChanged?(self, .aspect) } }

    init(kind: pin_kind_t, tabHeader: String, choices: FormatChoices) {
        self.kind = kind
        self.tabHeader = tabHeader
        formatOptions = FormatOptionsModel(choices: choices, analog: false)   // DV and HDV share the DV choices
    }

    /// (Re)fills the format list (after the encoder probe), keeping the selection or the wanted format if
    /// the list has it, else the kind's default.
    func refreshFormats() {
        let keep = selectedFormat?.format ?? wantedFormat
        formats = FormatItem.formats(for: kind)
        selectedFormat = formats.first { $0.format == keep } ?? formats.first(where: \.isDefault) ?? formats.first
    }

    var titleEnabled: Bool { selectedFormat?.supportsTitle ?? false }
    var splitEnabled: Bool { selectedFormat?.supportsSceneSplit ?? false }
    var passesEnabled: Bool { selectedFormat?.supportsMultiPass ?? false }

    /// The core's rule: passes need the no-signal timeout or the "stop after" limit.
    var passesAllowed: Bool { Pin.passesAllowed(idleStopMinutes: idleStopMinutes, maxDurationMinutes: maxDurationMinutes) }
    /// Entry enabled (before the editable-while-idle check): the format supports passes and a limit is set.
    var passesUsable: Bool { passesEnabled && passesAllowed }

    /// What the entry shows: 1 while passes are not allowed, else the user's value.
    var displayPasses: Int {
        get { passesAllowed ? passes : 1 }
        set { if passesAllowed { passes = newValue } }
    }
    /// Pass count to capture with (what the core would also enforce).
    var effectivePasses: Int { passesAllowed ? Swift.max(1, passes) : 1 }

    var passesToolTip: String {
        passesAllowed
            ? "1 captures the tape once. More passes rewind to the start of the tape and capture it again. Only for Automatic rewind & capture: each new pass rewinds the tape, so Manual capture always captures one pass."
            : "Needs \"Stop no signal\" or \"Stop after\" to be set: multi-pass needs a way to detect the end of a pass. Set one of them to enable more than one pass. Only for Automatic rewind & capture: each new pass rewinds the tape, so Manual capture always captures one pass."
    }

    /// "dv" / "hdv": the settings key suffix.
    var settingsPrefix: String { kind == PIN_KIND_DV ? "dv" : "hdv" }

    func applyFormat(_ format: pin_format_t) {
        wantedFormat = format
        if let f = formats.first(where: { $0.format == format }) { selectedFormat = f }
    }
}
