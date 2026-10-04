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
import CMarvinCore

/// Thin Swift layer over the C API (pin_api.h). Stateless helpers only; the
/// session/model objects live elsewhere and use these.
enum Pin {
    /// The API version this app was compiled against.
    static let expectedApiVersion = UInt32(PIN_API_VERSION)

    static var apiVersion: UInt32 { pin_api_version() }
    static var versionString: String { String(cString: pin_version_string()) }
    static var apiVersionMatches: Bool { apiVersion == expectedApiVersion }

    static func strerror(_ s: pin_status_t) -> String { String(cString: pin_strerror(s)) }

    /// Where the FPGA bitstreams are (optional; the core also searches next to itself).
    @discardableResult
    static func setFirmwareDir(_ path: String) -> pin_status_t { pin_set_firmware_dir(path) }

    // MARK: settings (section.key; stored by the core, shared with the other front ends)

    static func settingsGet(key: String, fallback: String = "") -> String {
        var buf = [CChar](repeating: 0, count: 2048)
        guard pin_settings_get(key, &buf, buf.count) == PIN_OK else { return fallback }
        return String(cString: buf)
    }

    @discardableResult
    static func settingsSet(key: String, value: String) -> pin_status_t { pin_settings_set(key, value) }

    // MARK: text formatting

    /// Calls a `(char *out, size_t cap)` formatting function and returns its text, e.g.
    /// `Pin.format { out, cap in pin_format_bytes(n, out, cap) }`. 512 bytes hold every
    /// core text except pin_format_status_short() (pass a larger capacity there).
    static func format(capacity: Int = 512, _ body: (UnsafeMutablePointer<CChar>, Int) -> Void) -> String {
        var buf = [CChar](repeating: 0, count: capacity)
        buf.withUnsafeMutableBufferPointer { body($0.baseAddress!, capacity) }
        return String(cString: buf)
    }

    // MARK: size-versioned structs

    /// A zeroed status snapshot with `size` set, ready for pin_get_status().
    static func makeStatusSnapshot() -> pin_status_snapshot_t {
        var s = pin_status_snapshot_t()
        s.size = UInt32(MemoryLayout<pin_status_snapshot_t>.size)
        return s
    }

    static func makeDeviceInfo() -> pin_device_info_t {
        var d = pin_device_info_t()
        d.size = UInt32(MemoryLayout<pin_device_info_t>.size)
        return d
    }

    static func makeEvent() -> pin_event_t {
        var e = pin_event_t()
        e.size = UInt32(MemoryLayout<pin_event_t>.size)
        return e
    }

    // MARK: devices

    /// All devices pin_enumerate() reports (retries once if there are more than the first buffer).
    static func enumerateDevices() -> [pin_device_info_t] {
        var capacity = 16
        while true {
            var list = [pin_device_info_t](repeating: makeDeviceInfo(), count: capacity)
            let total = Int(pin_enumerate(&list, Int32(capacity)))
            if total <= capacity { return Array(list.prefix(max(total, 0))) }
            capacity = total
        }
    }

    // MARK: sessions

    /// pin_session_t is an opaque C struct, so Swift sees it as an OpaquePointer.
    typealias Session = OpaquePointer

    static func open(_ deviceID: String) -> (status: pin_status_t, session: Session?) {
        var s: Session?
        let st = pin_open(deviceID, &s)
        return (st, st == PIN_OK ? s : nil)
    }

    /// pin_close: blocks until any capture is finalised.
    static func close(_ s: Session) { pin_close(s) }

    @discardableResult
    static func setInput(_ s: Session, _ index: Int) -> pin_status_t {
        pin_set_input(s, pin_input_t(rawValue: UInt32(clamping: index)))
    }

    static func status(_ s: Session) -> pin_status_snapshot_t? {
        var st = makeStatusSnapshot()
        return pin_get_status(s, &st) == PIN_OK ? st : nil
    }

    // MARK: analog controls

    static func stdName(_ std: pin_std_t) -> String { String(cString: pin_std_name(std)) }

    @discardableResult
    static func setStandard(_ s: Session, _ std: pin_std_t) -> pin_status_t { pin_set_standard(s, std) }

    static func control(_ s: Session, _ c: pin_control_t) -> pin_control_info_t? {
        var info = pin_control_info_t()
        info.size = UInt32(MemoryLayout<pin_control_info_t>.size)
        return pin_get_control(s, c, &info) == PIN_OK ? info : nil
    }

    @discardableResult
    static func setControl(_ s: Session, _ c: pin_control_t, _ value: Int) -> pin_status_t {
        pin_set_control(s, c, Int32(clamping: value))
    }

    static func setAspect(_ s: Session, _ index: Int) { pin_set_aspect(s, pin_aspect_t(rawValue: UInt32(clamping: index))) }

    // MARK: deck

    @discardableResult
    static func deck(_ s: Session, _ cmd: pin_deck_cmd_t) -> pin_status_t { pin_deck(s, cmd) }
    static func deckStateName(_ d: pin_deck_state_t) -> String { String(cString: pin_deck_state_name(d)) }
    static func deckCmdAllowed(state: pin_state_t, deckAvailable: Bool, deck: pin_deck_state_t,
                               cmd: pin_deck_cmd_t) -> Bool {
        pin_deck_cmd_allowed(state, deckAvailable ? 1 : 0, deck, cmd) != 0
    }
    static func captureActionAllowed(state: pin_state_t, deckAvailable: Bool,
                                     action: pin_capture_action_t) -> Bool {
        pin_capture_action_allowed(state, deckAvailable ? 1 : 0, action) != 0
    }
    static func stateIsCapturing(_ s: pin_state_t) -> Bool { pin_state_is_capturing(s) != 0 }
    static func stopReasonAbnormal(_ r: pin_stop_reason_t) -> Bool { pin_stop_reason_abnormal(r) != 0 }

    // MARK: formats and capture

    static func formats(_ kind: pin_kind_t) -> [pin_format_info_t] {
        var list = [pin_format_info_t](repeating: {
            var f = pin_format_info_t()
            f.size = UInt32(MemoryLayout<pin_format_info_t>.size)
            return f
        }(), count: 16)
        let n = Int(pin_formats(kind, &list, Int32(list.count)))
        return Array(list.prefix(max(0, min(n, list.count))))
    }

    static func captureOptsDefaults() -> pin_capture_opts_t {
        var o = pin_capture_opts_t()
        pin_capture_opts_defaults(&o)
        return o
    }

    static func passesAllowed(idleStopMinutes: Int, maxDurationMinutes: Int) -> Bool {
        pin_capture_passes_allowed(Int32(clamping: idleStopMinutes), Int32(clamping: maxDurationMinutes)) != 0
    }

    static func nextFileNumber(path: String) -> UInt32 { pin_next_file_number(path) }

    static func checkOutput(_ s: Session, _ o: pin_capture_opts_t) -> (status: pin_status_t, check: pin_output_check_t) {
        var check = pin_output_check_t()
        check.size = UInt32(MemoryLayout<pin_output_check_t>.size)
        var opts = o
        return (pin_check_output(s, &opts, &check), check)
    }

    @discardableResult
    static func setOutputHint(_ s: Session, _ o: pin_capture_opts_t) -> pin_status_t {
        var opts = o
        return pin_set_output_hint(s, &opts)
    }

    static func captureStart(_ s: Session, _ o: pin_capture_opts_t, overwrite: Bool) -> pin_status_t {
        var opts = o
        return pin_capture_start(s, &opts, overwrite ? 1 : 0)
    }

    static func captureStop(_ s: Session, deck: pin_stop_deck_t?) -> pin_status_t {
        guard let deck else { return pin_capture_stop(s) }
        return pin_capture_stop_ex(s, deck)
    }

    // MARK: status text (all from the core)

    private static func withStatus<R>(_ st: pin_status_snapshot_t, _ body: (UnsafePointer<pin_status_snapshot_t>) -> R) -> R {
        withUnsafePointer(to: st, body)
    }

    static func formatState(_ st: pin_status_snapshot_t) -> String {
        withStatus(st) { p in format { pin_format_state(p, $0, $1) } }
    }
    static func formatStatusLine(_ st: pin_status_snapshot_t) -> String {
        withStatus(st) { p in format { pin_format_status_line(p, $0, $1) } }
    }
    /// `behindHub` = the open device's hub_depth > 0. `hubReady` true: READY text for a hub; the tooltip is usbHubHint().
    static func formatStatusShort(_ st: pin_status_snapshot_t, behindHub: Bool) -> (text: String, hubReady: Bool) {
        var ready = false
        let text = withStatus(st) { p in
            format(capacity: Int(PIN_PATH_MAX) + 128) { out, cap in
                ready = pin_format_status_short(p, behindHub ? 1 : 0, out, cap) != 0
            }
        }
        return (text, ready)
    }
    static func formatSignal(_ st: pin_status_snapshot_t) -> String {
        withStatus(st) { p in format { pin_format_signal(p, $0, $1) } }
    }
    static func formatFrames(_ st: pin_status_snapshot_t) -> String {
        withStatus(st) { p in format { pin_format_frames(p, $0, $1) } }
    }
    static func formatFramesDetail(_ st: pin_status_snapshot_t) -> String {
        withStatus(st) { p in format { pin_format_frames_detail(p, $0, $1) } }
    }
    static func formatSizes(_ st: pin_status_snapshot_t) -> String {
        withStatus(st) { p in format { pin_format_sizes(p, $0, $1) } }
    }
    static func formatStorageFree(_ st: pin_status_snapshot_t) -> String {
        withStatus(st) { p in format { pin_format_storage_free(p, $0, $1) } }
    }
    static func formatStorageDetail(_ st: pin_status_snapshot_t) -> String {
        withStatus(st) { p in format { pin_format_storage_detail(p, $0, $1) } }
    }
    static func formatWindowTitle(_ st: pin_status_snapshot_t, deviceName: String) -> String {
        withStatus(st) { p in format { pin_format_window_title(p, deviceName, $0, $1) } }
    }
    static func formatBytes(_ bytes: UInt64) -> String { format { pin_format_bytes(bytes, $0, $1) } }
    static func formatTimeLeft(_ seconds: Double) -> String { format { pin_format_time_left(seconds, $0, $1) } }
    static func formatRemaining(_ seconds: Double) -> String { format { pin_format_remaining(seconds, $0, $1) } }

    /// What a dock / taskbar progress indicator shows (mode and 0...1 fraction).
    static func statusProgress(_ st: pin_status_snapshot_t, idleTotalS: Double,
                               durationTotalS: Double) -> (mode: pin_progress_mode_t, fraction: Double) {
        var fraction = 0.0
        let mode = withStatus(st) { pin_status_progress($0, idleTotalS, durationTotalS, &fraction) }
        return (mode, fraction)
    }

    // MARK: devices

    static func noDevicesHint() -> String { String(cString: pin_no_devices_hint()) }
    static func usbHubHint() -> String { String(cString: pin_usb_hub_hint()) }

    static func deviceDisplayName(_ d: pin_device_info_t) -> String {
        withUnsafePointer(to: d) { p in format { pin_device_display_name(p, $0, $1) } }
    }
    static func deviceStatusText(_ d: pin_device_info_t, capturingHere: Bool) -> String {
        withUnsafePointer(to: d) { p in format { pin_device_status_text(p, capturingHere ? 1 : 0, $0, $1) } }
    }
    /// Tooltip for a device that cannot be picked, nil for one that can.
    static func deviceUnavailableReason(_ d: pin_device_info_t) -> String? {
        var unavailable = false
        let t = withUnsafePointer(to: d) { p in format { unavailable = pin_device_unavailable_reason(p, $0, $1) != 0 } }
        return unavailable ? t : nil
    }
    /// The sentence for a failed attempt to open such a device, nil for one that can be opened.
    static func deviceOpenProblem(_ d: pin_device_info_t) -> String? {
        var problem = false
        let t = withUnsafePointer(to: d) { p in format { problem = pin_device_open_problem(p, $0, $1) != 0 } }
        return problem ? t : nil
    }

    static func setReplayFile(_ path: String?) { pin_set_replay_file(path) }
    static func devicesWait(timeoutMs: Int) -> Int { Int(pin_devices_wait(Int32(timeoutMs))) }
    static func devicesWake() { pin_devices_wake() }

    // MARK: events, log, monitor

    /// One pin_event_t, copied out.
    struct Event {
        var kind: pin_event_kind_t
        var a: Int
        var text: String
    }

    /// Next pending event of the session (nil = process-wide log events), or nil if none.
    static func pollEvent(_ s: Session?) -> Event? {
        var e = makeEvent()
        guard pin_poll_event(s, &e) != 0 else { return nil }
        return Event(kind: e.kind, a: Int(e.a), text: cString(e.text))
    }

    static func setLogLevel(_ level: Int) { pin_set_log_level(Int32(level)) }
    static func monitorEnable(_ s: Session, _ on: Bool) { pin_monitor_enable(s, on ? 1 : 0) }

    // MARK: per-device settings

    /// "dev_<GUID>." (the core's rule), or nil if it cannot be built.
    static func deviceSettingsPrefix(serial: String, id: String) -> String? {
        var buf = [CChar](repeating: 0, count: 256)
        return pin_device_settings_key(serial, id, nil, &buf, buf.count) == PIN_OK ? String(cString: buf) : nil
    }

    // MARK: command line

    /// A parsed command line (pin_script_t); frees itself.
    final class Script {
        fileprivate let raw: OpaquePointer
        fileprivate init(_ raw: OpaquePointer) { self.raw = raw }
        deinit { pin_script_free(raw) }

        var helpRequested: Bool { pin_script_help_requested(raw) != 0 }
        /// -d / --device value, "" when none was given.
        var device: String { pin_script_device(raw).map { String(cString: $0) } ?? "" }
        var debug: Bool { pin_script_debug(raw) != 0 }
        var exitWhenDone: Bool { pin_script_exit_when_done(raw) != 0 }
        var needsSession: Bool { pin_script_needs_session(raw) != 0 }

        func settings() -> pin_script_settings_t {
            var s = pin_script_settings_t()
            s.size = UInt32(MemoryLayout<pin_script_settings_t>.size)
            _ = pin_script_settings(raw, &s)
            return s
        }

        func run(on session: Session) -> pin_status_t { pin_script_run(session, raw) }
    }

    /// Parses argv WITHOUT the program name (at least one argument).
    static func parseScript(_ args: [String]) -> (status: pin_status_t, script: Script?, error: String) {
        guard !args.isEmpty else { return (PIN_ERR_ARG, nil, "") }
        var cstrs: [UnsafePointer<CChar>?] = args.map { UnsafePointer(strdup($0)) }
        defer { for p in cstrs { free(UnsafeMutablePointer(mutating: p)) } }
        var out: OpaquePointer?
        var err = [CChar](repeating: 0, count: 512)
        let st = pin_script_parse(Int32(args.count), &cstrs, &out, &err, err.count)
        guard st == PIN_OK, let out else { return (st, nil, String(cString: err)) }
        return (st, Script(out), "")
    }

    /// The full help text (docs/cli.md).
    static func scriptHelp() -> String { String(cString: pin_script_help()) }
}

extension pin_script_settings_t {
    /// The value the command line gave for control `c` (units of pin_set_control), or nil.
    func value(for c: pin_control_t) -> Int? {
        guard controls_set & (1 << c.rawValue) != 0 else { return nil }
        switch c {
        case PIN_CTL_BRIGHTNESS: return Int(control_brightness)
        case PIN_CTL_CONTRAST: return Int(control_contrast)
        case PIN_CTL_SATURATION: return Int(control_saturation)
        case PIN_CTL_HUE: return Int(control_hue)
        case PIN_CTL_SHARPNESS: return Int(control_sharpness)
        case PIN_CTL_AUDIO_GAIN: return Int(control_audio_gain)
        default: return nil
        }
    }
}

extension pin_device_info_t {
    var idString: String { cString(id) }
    var nameString: String { cString(name) }
    var serialString: String { cString(serial) }
}
