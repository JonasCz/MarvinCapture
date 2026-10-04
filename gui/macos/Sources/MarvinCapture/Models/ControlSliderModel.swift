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

/// One analog proc-amp slider (brightness ... audio gain), sourced from pin_get_control.
/// Port of ControlSliderViewModel.
@MainActor @Observable
final class ControlSliderModel: Identifiable {
    let control: pin_control_t
    nonisolated var id: UInt32 { control.rawValue }

    var label = ""
    var min = 0
    var max = 0
    var step = 1
    var def = 0
    var isEnabled = true
    /// Audio gain is dB * 10 in the API and shown with one decimal.
    var isDb = false

    var value = 0 {
        didSet {
            if !suppressChange { onChanged(control, value) }
        }
    }

    @ObservationIgnored private var suppressChange = false
    @ObservationIgnored private let onChanged: (pin_control_t, Int) -> Void

    init(_ control: pin_control_t, isDb: Bool = false, onChanged: @escaping (pin_control_t, Int) -> Void) {
        self.control = control
        self.isDb = isDb
        self.onChanged = onChanged
    }

    var valueText: String { isDb ? String(format: "%.1f dB", Double(value) / 10.0) : String(value) }
    var resetName: String { "Reset \(label) to default" }

    /// Slider.value is a Double; this adapts it to the integer API value.
    var sliderValue: Double {
        get { Double(value) }
        set { value = Int(newValue.rounded()) }
    }

    /// Takes what the core reports (range, current value, enabled) without echoing it back.
    func load(from info: pin_control_info_t) {
        suppressChange = true
        label = cString(info.label)
        min = Int(info.min)
        max = Int(info.max)
        step = info.step <= 0 ? 1 : Int(info.step)
        def = Int(info.def)
        value = Int(info.value)
        isEnabled = info.enabled != 0
        suppressChange = false
    }

    func resetToDefault() { value = def }
}
