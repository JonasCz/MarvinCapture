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

/// The format settings the user chose: deinterlace and quality per input kind (analog, DV; HDV shares the
/// DV ones), FFV1 compression once. -1 = the format's own default (pin_format_options). Stored per device by
/// WindowModel under the Android app's keys: gui.deinterlace_analog / _dv, gui.quality_analog / _dv,
/// gui.ffv1_compression. Port of FormatChoices (Windows).
@MainActor @Observable
final class FormatChoices {
    enum Field { case deinterlaceAnalog, deinterlaceDv, qualityAnalog, qualityDv, ffv1Compression }

    /// Called after a value changed (the window model saves it and updates the output hint).
    @ObservationIgnored var onChanged: ((Field) -> Void)?

    var deinterlaceAnalog = -1 { didSet { if deinterlaceAnalog != oldValue { onChanged?(.deinterlaceAnalog) } } }
    var deinterlaceDv = -1 { didSet { if deinterlaceDv != oldValue { onChanged?(.deinterlaceDv) } } }
    var qualityAnalog = -1 { didSet { if qualityAnalog != oldValue { onChanged?(.qualityAnalog) } } }
    var qualityDv = -1 { didSet { if qualityDv != oldValue { onChanged?(.qualityDv) } } }
    var ffv1Compression = -1 { didSet { if ffv1Compression != oldValue { onChanged?(.ffv1Compression) } } }

    func deinterlace(analog: Bool) -> Int { analog ? deinterlaceAnalog : deinterlaceDv }
    func quality(analog: Bool) -> Int { analog ? qualityAnalog : qualityDv }

    func setDeinterlace(analog: Bool, _ value: Int) {
        if analog { deinterlaceAnalog = value } else { deinterlaceDv = value }
    }

    func setQuality(analog: Bool, _ value: Int) {
        if analog { qualityAnalog = value } else { qualityDv = value }
    }
}

/// pin_format_options_t with Swift strings: which settings apply to a format, their labels, range,
/// defaults and help texts.
struct FormatOptions {
    var hasQuality = false
    var hasDeinterlace = false
    var hasFfv1 = false

    var qualityMin = 0
    var qualityMax = 0
    var qualityDefault = 0
    var qualityLabel = ""
    var qualityLowLabel = ""
    var qualityHighLabel = ""
    var qualityHelp = ""

    var deinterlaceDefault = 0
    var deinterlaceLabel = ""
    var keepFieldsLabel = ""
    var doubleRateLabel = ""
    var keepFieldsHelp = ""
    var doubleRateHelp = ""

    var ffv1Default = 0
    var ffv1Label = ""
    /// In pin_ffv1_compression_t order: fast, smaller, smallest.
    var ffv1LevelLabels: [String] = []
    var ffv1Help = ""

    var note = ""

    var hasAny: Bool { hasQuality || hasDeinterlace || hasFfv1 }

    init() {}

    init(_ o: pin_format_options_t) {
        hasQuality = o.has_quality != 0
        hasDeinterlace = o.has_deinterlace != 0
        hasFfv1 = o.has_ffv1_compression != 0
        qualityMin = Int(o.quality_min)
        qualityMax = Swift.max(Int(o.quality_min), Int(o.quality_max))
        qualityDefault = Int(o.quality_default)
        qualityLabel = cString(o.quality_label)
        qualityLowLabel = cString(o.quality_low_label)
        qualityHighLabel = cString(o.quality_high_label)
        qualityHelp = cString(o.quality_help)
        deinterlaceDefault = Int(o.deinterlace_default)
        deinterlaceLabel = cString(o.deinterlace_label)
        keepFieldsLabel = cString(o.deinterlace_keep_label)
        doubleRateLabel = cString(o.deinterlace_double_label)
        keepFieldsHelp = cString(o.deinterlace_keep_help)
        doubleRateHelp = cString(o.deinterlace_double_help)
        ffv1Default = Int(o.ffv1_default)
        ffv1Label = cString(o.ffv1_label)
        ffv1LevelLabels = [cString(o.ffv1_fast_label), cString(o.ffv1_smaller_label), cString(o.ffv1_smallest_label)]
        ffv1Help = cString(o.ffv1_help)
        note = cString(o.note)
    }
}

/// The settings of one output format, behind the gear next to its picker (FormatOptionsView in a popover):
/// everything comes from pin_format_options() for the selected format, the warning next to the picker from
/// pin_format_warning(), the data-rate line from pin_format_quality_hint(). The values live in
/// `FormatChoices`; this presents them for one picker. Port of FormatOptionsViewModel (Windows).
@MainActor @Observable
final class FormatOptionsModel {
    let choices: FormatChoices
    /// Analog's choices, or DV's (DV and HDV share them).
    let analog: Bool

    /// The format whose settings are shown (the picker's selection), or nil.
    var format: pin_format_t? = nil {
        didSet { if format != oldValue { load() } }
    }
    /// The signal's video standard (status is_60hz), for the data-rate line.
    var is60Hz = false
    /// False while capturing: the settings apply at capture start.
    var isEditable = true

    private(set) var options = FormatOptions()
    /// pin_format_warning(): shown next to the picker while the format is selected.
    private(set) var warning = ""

    init(choices: FormatChoices, analog: Bool) {
        self.choices = choices
        self.analog = analog
    }

    private func load() {
        if let f = format, let o = Pin.formatOptions(f) {
            options = FormatOptions(o)
            warning = Pin.formatWarning(f)
        } else {
            options = FormatOptions()
            warning = format.map { Pin.formatWarning($0) } ?? ""
        }
    }

    private static let keepFields = Int(PIN_DEINTERLACE_KEEP_FIELDS.rawValue)
    private static let doubleRate = Int(PIN_DEINTERLACE_DOUBLE_RATE.rawValue)

    /// The deinterlace choice shown (pin_deinterlace_t): the stored one, else the format's default.
    var deinterlace: Int {
        get {
            let d = choices.deinterlace(analog: analog)
            return d == Self.keepFields || d == Self.doubleRate ? d : options.deinterlaceDefault
        }
        set { choices.setDeinterlace(analog: analog, newValue) }
    }

    /// "Keep fields" is chosen: its help is shown as a warning (interlaced video loses quality).
    var keepFieldsChosen: Bool { deinterlace == Self.keepFields }

    /// The quality shown: the stored one (clamped to the format's range), else the format's default.
    var quality: Int {
        get {
            let q = choices.quality(analog: analog)
            return q < 0 ? options.qualityDefault : Swift.min(Swift.max(q, options.qualityMin), options.qualityMax)
        }
        set { choices.setQuality(analog: analog, newValue) }
    }

    /// The FFV1 compression shown (pin_ffv1_compression_t): the stored one, else the format's default.
    var ffv1: Int {
        get {
            let c = choices.ffv1Compression
            return c >= 0 && c < Int(PIN_FFV1_LEVEL_COUNT.rawValue) ? c : options.ffv1Default
        }
        set { choices.ffv1Compression = newValue }
    }

    /// The data rate the quality comes to ("About 2.9 GB per hour (6.4 Mbit/s)"), "" for a format without it.
    var qualityHint: String {
        guard options.hasQuality, let f = format else { return "" }
        return Pin.formatQualityHint(f, quality: quality, deinterlace: deinterlace, is60Hz: is60Hz)
    }
}
