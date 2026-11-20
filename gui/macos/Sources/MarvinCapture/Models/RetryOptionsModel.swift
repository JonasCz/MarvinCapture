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

/// How the retry of bad sections works (the gear next to the "Retry bad sections" toggle), shared by the DV
/// and HDV tabs: attempts, the memory window, the preroll and what damage asks for a re-read. Ranges,
/// defaults and help texts come from the core (pin_retry_setting_range / _help); stored per device by
/// WindowModel (gui.retry_attempts, gui.retry_window, gui.retry_preroll, gui.retry_on). Port of
/// RetryOptionsViewModel (Windows).
@MainActor @Observable
final class RetryOptionsModel {
    enum Field { case attempts, window, preroll, trigger, mode, storage }

    /// Called after a value changed (the window model saves it).
    @ObservationIgnored var onChanged: ((Field) -> Void)?

    let attemptsRange: ClosedRange<Int>
    let windowRange: ClosedRange<Int>
    let prerollRange: ClosedRange<Int>
    let attemptsDefault: Int
    let windowDefault: Int
    let prerollDefault: Int

    var attempts: Int { didSet { if attempts != oldValue { onChanged?(.attempts) } } }
    var windowSeconds: Int { didSet { if windowSeconds != oldValue { onChanged?(.window) } } }
    var prerollSeconds: Int { didSet { if prerollSeconds != oldValue { onChanged?(.preroll) } } }
    /// 0 = any damage, 1 = uncorrected only (pin_retry_on_t order).
    var triggerIndex = 0 { didSet { if triggerIndex != oldValue { onChanged?(.trigger) } } }
    /// pin_retry_mode_api_t order: 0 automatic, 1 each section, 2 whole tape.
    var modeIndex = 0 { didSet { if modeIndex != oldValue { onChanged?(.mode) } } }
    /// pin_retry_storage_t order: 0 memory, 1 temporary file.
    var storageIndex = 0 { didSet { if storageIndex != oldValue { onChanged?(.storage) } } }
    /// False while capturing: the settings apply at capture start.
    var isEditable = true

    init() {
        let a = Pin.retrySettingRange(0), w = Pin.retrySettingRange(1), p = Pin.retrySettingRange(2)
        attemptsRange = a.min...a.max
        windowRange = w.min...w.max
        prerollRange = p.min...p.max
        attemptsDefault = a.def
        windowDefault = w.def
        prerollDefault = p.def
        attempts = a.def
        windowSeconds = w.def
        prerollSeconds = p.def
    }

    var attemptsHelp: String { Pin.retrySettingHelp(0) }
    var windowHelp: String { Pin.retrySettingHelp(1) }
    var prerollHelp: String { Pin.retrySettingHelp(2) }
    var triggerHelp: String { Pin.retrySettingHelp(3) }
    var modeHelp: String { Pin.retrySettingHelp(5) }
    var storageHelp: String { Pin.retrySettingHelp(6) }
    var wholeTapeNote: String { Pin.retrySettingHelp(7) }

    /// Into the capture options (the core clamps what is out of range).
    func apply(to o: inout pin_capture_opts_t) {
        o.retry_attempts = Int32(clamping: attempts)
        o.retry_window_s = Int32(clamping: windowSeconds)
        o.retry_preroll_s = Int32(clamping: prerollSeconds)
        o.retry_trigger = Int32(triggerIndex == 1 ? PIN_RETRY_ON_UNCORRECTED.rawValue : PIN_RETRY_ON_ANY_DAMAGE.rawValue)
        o.retry_mode = Int32((0...2).contains(modeIndex) ? modeIndex : 0)
        o.retry_storage = Int32(storageIndex == 1 ? PIN_RETRY_IN_FILE.rawValue : PIN_RETRY_IN_MEMORY.rawValue)
    }
}
