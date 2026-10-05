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
import Foundation
import Observation
import CMarvinCore

// MARK: - small value types

/// Severity of the transient info banner (InfoBar on Windows).
enum InfoSeverity { case info, success, warning, error }

/// A drained engine event, handed to `WindowModel.onEngineEvent` after the model applied it.
struct EngineEvent {
    var kind: pin_event_kind_t
    var a: Int
    var text: String
}

/// Something the view layer should show as a modal alert (an abnormal capture end).
struct ModelAlert: Identifiable {
    let id = UUID()
    var title: String
    var message: String
}

/// A question to ask before a capture may start ("Check the destination", "Low disk space",
/// "Replace existing file?"). Show them in order; any "no" cancels.
struct CaptureConfirmation: Identifiable {
    let id = UUID()
    var title: String
    var message: String
    /// The affirmative button ("Capture anyway", "OK", "Overwrite"); the other one is "Cancel".
    var primaryButton: String
}

/// The result of checking a capture: pass `opts` and `overwrite` to `startCapture` once every
/// confirmation was accepted.
struct CapturePlan {
    var opts: pin_capture_opts_t
    var confirmations: [CaptureConfirmation]
    var overwrite: Bool
}

enum DeckCommand {
    case play, pause, stop, ff, rew

    var native: pin_deck_cmd_t {
        switch self {
        case .play: PIN_DECK_CMD_PLAY
        case .pause: PIN_DECK_CMD_PAUSE
        case .stop: PIN_DECK_CMD_STOP
        case .ff: PIN_DECK_CMD_FF
        case .rew: PIN_DECK_CMD_REW
        }
    }
}

/// Highest value of the last 10 s, for the meters' peak-hold tick.
struct PeakHold {
    private static let window = 10.0
    private let floor: Double
    private var samples: [(t: Double, v: Double)] = []

    init(floor: Double) { self.floor = floor }

    mutating func push(_ value: Double) -> Double {
        let now = ProcessInfo.processInfo.systemUptime
        samples.append((now, value))
        samples.removeAll { now - $0.t > Self.window }
        return samples.reduce(floor) { Swift.max($0, $1.v) }
    }
}

/// Stop flag shared with the device-watch thread.
private final class StopFlag: @unchecked Sendable {
    private let lock = NSLock()
    private var flag = false
    var isSet: Bool { lock.lock(); defer { lock.unlock() }; return flag }
    func set() { lock.lock(); flag = true; lock.unlock() }
}

// MARK: - the model

/// The whole window's state and behaviour: MainViewModel plus the non-view parts of MainWindow.xaml.cs
/// (startup, device watch, status tick, capture start/stop, close). All engine access goes through
/// `Pin`; every text and enable rule comes from the core (`pin_format_*`, `pin_deck_cmd_allowed`, ...).
///
/// Update model: `start()` runs a 100 ms timer calling `tick()` (pin_get_status + draining
/// pin_poll_event) and a ~30 Hz meter timer; the device list is driven by a background thread blocked
/// in pin_devices_wait. There is no other polling.
@MainActor @Observable
final class WindowModel {
    private static let defaultDeviceName = "Pinnacle Studio 500-USB"
    static let meterFloorDb = -60.0
    private static let meterDecayDbPerSecond = 30.0

    // MARK: seams and callbacks (set by the view layer / app delegate)

    @ObservationIgnored var preview: PreviewSink = MetalPreview()
    @ObservationIgnored var audio: AudioMonitor = CoreAudioMonitor()
    /// An abnormal capture end the user must see (not while closing or running command-line steps).
    @ObservationIgnored var onAlert: ((ModelAlert) -> Void)?
    /// Every drained engine event, after the model applied it.
    @ObservationIgnored var onEngineEvent: ((EngineEvent) -> Void)?
    /// `--exit-when-done`: the steps finished; quit with this exit code.
    @ObservationIgnored var onExitRequested: ((Int32) -> Void)?
    /// Runs at the end of every 100 ms tick (the close flow watches for READY here).
    @ObservationIgnored var afterTick: (() -> Void)?
    /// More tick listeners (Dock tile); run after `afterTick`.
    @ObservationIgnored var tickObservers: [() -> Void] = []
    /// A capture ended or failed (the same situations as the background notifications: not while
    /// closing): the Dock bounces if the app is in the background. `critical` = abnormal end.
    @ObservationIgnored var onAttention: ((_ critical: Bool) -> Void)?
    /// Set by the close flow so no dialog / notification fires for the stop the user asked for.
    /// Observable: the view shows the "Finalizing files…" overlay while it is set.
    var isFinalizingForClose = false

    // MARK: devices

    private(set) var devices: [DeviceItem] = []
    /// The picker's selection (a DeviceItem.id). Setting it loads that device's settings and opens it.
    var selectedDeviceID: String? {
        didSet { if selectedDeviceID != oldValue { selectedDeviceChanged() } }
    }
    private(set) var noDevices = true

    var selectedDevice: DeviceItem? { devices.first { $0.id == selectedDeviceID } }
    var hasSelectedDevice: Bool { selectedDevice != nil }
    /// The device picker: fixed while capturing, and disabled outright when there is nothing to pick.
    var deviceSelectEnabled: Bool { !isCapturing && !noDevices }
    /// The core's (platform-specific) hint shown while no device is found.
    let noDevicesHint = Pin.noDevicesHint()

    /// The open device's list entry (the selection, if the list doesn't have it).
    private var openDevice: DeviceItem? { devices.first { $0.id == openedDeviceID } ?? selectedDevice }

    // MARK: session

    @ObservationIgnored private(set) var session: Pin.Session?
    /// True while a session is open.
    var hasSession: Bool { session != nil }
    @ObservationIgnored private(set) var openedDeviceID: String?
    private(set) var sessionState: pin_state_t = PIN_STATE_CLOSED
    private(set) var sessionStateText = "No device open"

    /// True from capture start until the core reports READY again (includes STOPPING / REWINDING).
    private(set) var isCapturing = false {
        didSet { if isCapturing != oldValue { refreshDevices() } }   // the open device's badge reads "Capturing"
    }
    var isIdle: Bool { !isCapturing }

    /// False only when the core knows for sure that no camera is on the FireWire bus.
    private(set) var deckAvailable = true

    // MARK: enable rules (all from the core)

    private func captureAllowed(_ a: pin_capture_action_t) -> Bool {
        Pin.captureActionAllowed(state: sessionState, deckAvailable: deckAvailable, action: a)
    }
    private func deckAllowed(_ c: pin_deck_cmd_t) -> Bool {
        Pin.deckCmdAllowed(state: sessionState, deckAvailable: deckAvailable, deck: deckState, cmd: c)
    }

    /// Stop is possible while writing (or between passes); not while already finalising.
    var canStop: Bool { captureAllowed(PIN_CAPTURE_STOP) }
    /// The Capture / Stop button (analog) and the Manual capture / Stop button (DV/HDV).
    var captureEnabled: Bool { captureAllowed(isCapturing ? PIN_CAPTURE_STOP : PIN_CAPTURE_START_MANUAL) }
    /// DV/HDV Manual capture: only while the deck plays (core rule); while capturing it is the Stop button.
    var playAndCaptureEnabled: Bool {
        isCapturing ? captureAllowed(PIN_CAPTURE_STOP)
                    : Pin.manualCaptureAllowed(state: sessionState, deckAvailable: deckAvailable, deck: deckState)
    }
    /// "Automatic rewind & capture" drives the deck, so it needs a camera. While capturing it stops capture and tape.
    var dvAutoCaptureEnabled: Bool { captureAllowed(isCapturing ? PIN_CAPTURE_STOP_TAPE : PIN_CAPTURE_START_AUTO) }
    var deckRewEnabled: Bool { deckAllowed(PIN_DECK_CMD_REW) }
    var deckPlayEnabled: Bool { deckAllowed(PIN_DECK_CMD_PLAY) }
    var deckStopEnabled: Bool { deckAllowed(PIN_DECK_CMD_STOP) }
    var deckFfEnabled: Bool { deckAllowed(PIN_DECK_CMD_FF) }

    var captureButtonText: String { isCapturing ? "Stop capture" + stopCountdownSuffix : "Capture" }
    var manualCaptureTitle: String { isCapturing ? "Stop capture & continue tape" + stopCountdownSuffix : "Manual capture" }
    var manualCaptureHelp: String {
        if isCapturing { return "Stops the capture and leaves the tape as it is" }
        if !playAndCaptureEnabled {
            let why = Pin.manualCaptureBlockText(deckAvailable: deckAvailable, deck: deckState)
            if !why.isEmpty { return why }
        }
        return "Records whatever the camera or deck is already sending, without controlling it"
    }
    var primaryDvTitle: String { isCapturing ? "Stop capture & stop tape" + stopCountdownSuffix : "Automatic rewind & capture" }
    var primaryDvHelp: String {
        isCapturing ? "Finishes the file, then stops the tape" : "Rewinds to the start of the tape, plays and captures it"
    }
    var analogCaptureHint: String { isCapturing ? "Finishes the file safely" : "Records the analog input to the file" }

    /// Taskbar-style "Start capture" (analog Capture, DV/HDV Manual capture), same enable rule as that button.
    var dockStartEnabled: Bool { !isCapturing && (isDvInput ? playAndCaptureEnabled : captureEnabled) }
    /// "Stop capture" whenever the in-window stop buttons can stop.
    var dockStopEnabled: Bool { isCapturing && canStop }

    // MARK: input

    /// pin_input_t order: DV, S-Video, Composite.
    var inputIndex = 0 {
        didSet {
            guard inputIndex != oldValue else { return }
            if let s = session, !loading {
                report(Pin.setInput(s, inputIndex), "Switch input")
                refreshControls()
            }
            saveSetting("gui.last_input", String(inputIndex))
            applyPreviewAspect()
            pushOutputHint()
        }
    }
    var isDvInput: Bool { inputIndex == Int(PIN_INPUT_DV.rawValue) }
    /// 0 = analog panel, 1 = DV panel.
    var activePanelIndex: Int { isDvInput ? 1 : 0 }

    // MARK: analog

    let standards: [StdItem]
    var selectedStandard: StdItem? {
        didSet {
            guard let v = selectedStandard else { return }
            saveSetting("gui.std", String(v.std.rawValue))
            if let s = session, !loading, v != oldValue {
                report(Pin.setStandard(s, v.std), "Set standard")
                refreshControls()   // hue is only meaningful for NTSC; the core flips `enabled`
            }
        }
    }

    let pictureSliders: [ControlSliderModel]
    let audioGain: ControlSliderModel
    private var allSliders: [ControlSliderModel] { pictureSliders + [audioGain] }

    let analogFormats: [FormatItem]
    var analogFormat: FormatItem? {
        didSet {
            if let f = analogFormat { saveSetting("gui.format_analog", String(f.format.rawValue)) }
            pushOutputHint()
        }
    }

    /// File base name; also embedded as the title when the format supports one.
    var analogName = "analog" { didSet { analogOutputChanged("gui.name_analog", analogName) } }
    var analogOutputDir = WindowModel.defaultOutputDir() { didSet { analogOutputChanged("gui.dir_analog", analogOutputDir) } }
    var analogOutputPath: String { Self.combinePath(analogOutputDir, analogName) }

    private func analogOutputChanged(_ key: String, _ value: String) {
        saveSetting(key, value)
        pushOutputHint()
    }

    /// Stop after this long without signal (minutes); 0 = never. Analog's own setting.
    var analogIdleStopMinutes = 5 { didSet { saveSetting("gui.idle_analog", String(analogIdleStopMinutes)) } }
    /// Stop after this long of capture time (minutes), signal or not; 0 = never.
    var analogMaxDurationMinutes = 0 { didSet { saveSetting("gui.duration_analog", String(analogMaxDurationMinutes)) } }
    /// Aspect override for analog sources (pin_aspect_t order: Auto, 4:3, 16:9).
    var analogAspectIndex = 0 {
        didSet {
            saveSetting("gui.aspect_analog", String(analogAspectIndex))
            applyPreviewAspect()
        }
    }

    // MARK: DV / HDV

    /// File base name for DV and HDV; also embedded as the title when the format supports it.
    var dvName = "tape" { didSet { dvOutputChanged("gui.name_dv", dvName) } }
    var dvOutputDir = WindowModel.defaultOutputDir() { didSet { dvOutputChanged("gui.dir_dv", dvOutputDir) } }
    var dvOutputPath: String { Self.combinePath(dvOutputDir, dvName) }

    private func dvOutputChanged(_ key: String, _ value: String) {
        saveSetting(key, value)
        pushOutputHint()
    }

    let dvSettings = KindSettingsModel(kind: PIN_KIND_DV, tabHeader: "DV")
    let hdvSettings = KindSettingsModel(kind: PIN_KIND_HDV, tabHeader: "HDV")
    var kindTabs: [KindSettingsModel] { [dvSettings, hdvSettings] }

    /// DV / HDV file-options tab. Follows the detected stream; remembered for the next start.
    var selectedKindTabIndex = 0 {
        didSet { saveSetting("gui.last_kind", String(selectedKindTabIndex)) }
    }

    private(set) var deckState: pin_deck_state_t = PIN_DECK_UNKNOWN
    private(set) var isRewChecked = false
    private(set) var isPlayChecked = false
    private(set) var isStopChecked = false
    private(set) var isFfChecked = false
    private(set) var deckBusy = false
    @ObservationIgnored private var lastStreamKind: pin_kind_t = PIN_KIND_DV

    // MARK: preview strip

    /// Muted unless the user unmuted last time (a live monitor next to the source's microphone is a feedback loop).
    var isMuted = true {
        didSet {
            guard isMuted != oldValue else { return }
            saveGlobalSetting("gui.muted", isMuted ? "1" : "0")
            audio.isMuted = isMuted
            if let s = session {
                // The core only keeps the PCM ring filled while monitoring is wanted.
                Pin.monitorEnable(s, !isMuted)
                if isMuted { audio.stop() } else { audio.start() }
            }
        }
    }
    /// A frame was presented within the last second (set from the preview sink every tick).
    var previewHasVideo = false
    /// Display aspect the preview frame is sized to (TrackPreviewAspect): what the preview reports for
    /// the frames on screen, else 16:9 when the active aspect override is 16:9, else 4:3.
    private(set) var previewDar = Dar(num: 4, den: 3)
    struct Dar: Equatable { var num: Int; var den: Int }

    private func trackPreviewAspect() {
        var d = Dar(num: 4, den: 3)
        if previewHasVideo, let f = preview.frameDar, f.num > 0, f.den > 0 {
            d = Dar(num: f.num, den: f.den)
        } else if activeAspectIndex == 2 {   // pin_aspect_t order: Auto, 4:3, 16:9
            d = Dar(num: 16, den: 9)
        }
        if d != previewDar { previewDar = d }
    }

    /// What a click on the mute button does.
    var muteActionName: String { isMuted ? "Unmute" : "Mute" }
    private(set) var noVideoText = "No device open"

    /// No-signal timeout running during a capture: text and a bar that shrinks to 0.
    private(set) var noSignalCountdownVisible = false
    private(set) var noSignalCountdownText = ""
    private(set) var noSignalCountdownFraction = 0.0   // 1 = full timeout left, 0 = stopping
    private(set) var stopCountdownSuffix = ""
    @ObservationIgnored private var activeIdleStopS = 0.0
    @ObservationIgnored private var activeDurationS = 0.0

    /// Dock-progress mode and fraction as decided by the core from the last status.
    private(set) var progressMode: pin_progress_mode_t = PIN_PROGRESS_NONE
    private(set) var progressFraction = 0.0

    /// Progress bar in the preview pane while the device is being prepared.
    private(set) var progressVisible = false
    private(set) var progressIndeterminate = true
    private(set) var progressValue = 0.0

    /// The aspect override (pin_aspect_t index) that applies to what is arriving right now.
    var activeAspectIndex: Int {
        !isDvInput ? analogAspectIndex : (lastStreamKind == PIN_KIND_HDV ? hdvSettings : dvSettings).aspectIndex
    }
    @ObservationIgnored private var appliedAspect: Int?

    private func applyPreviewAspect() {
        guard let s = session else { appliedAspect = nil; return }
        let a = activeAspectIndex
        if appliedAspect != a {
            Pin.setAspect(s, a)
            appliedAspect = a
        }
    }

    /// Tells the core where the next capture would go, so the status can report free space and time
    /// left before capturing starts.
    func pushOutputHint() {
        guard !loading, let s = session, !isCapturing else { return }
        Pin.setOutputHint(s, buildCaptureOpts(startDeck: false))
    }

    // MARK: status bar

    private(set) var statusLine = "Idle"
    /// First status-bar item: the file being written while capturing, else state / error.
    private(set) var statusShortText = "No device open"
    /// "Capturing  ·  pass 2/3" while a capture runs, else "".
    private(set) var statusSubText = ""
    private(set) var timecode = "--:--:--:--"
    /// The tape timecode for DV/HDV, the time since capture start for analog.
    private(set) var statusTimeText = "--:--:--:--"
    private(set) var statusTimeTip = "Tape timecode"
    private(set) var deckStateText = "Deck: —"
    /// Tooltip / accessibility label of the deck status, from the core ("Deck: Playing — waiting for the deck to respond" while busy).
    private(set) var deckTip = "Deck: —"
    private(set) var signalLocked = false
    private(set) var signalLockText = "No signal"
    private(set) var signalTypeText = ""
    private(set) var framesTotalText = "Frames 0 · 0 err · 0 drop"
    private(set) var statusTip = ""
    private(set) var storageTip = ""
    private(set) var storageFreeText = ""
    private(set) var framesTip = ""
    private(set) var sizeText = "0 B / 0 B"
    private(set) var audioPeakLeft = WindowModel.meterFloorDb
    private(set) var audioPeakRight = WindowModel.meterFloorDb
    private(set) var audioPeakText = "L — R —"
    private(set) var audioHoldLeft = WindowModel.meterFloorDb
    private(set) var audioHoldRight = WindowModel.meterFloorDb
    private(set) var windowTitle = "MarvinCapture"
    private(set) var tapePercent = -1
    private(set) var lastLogLine = ""
    private(set) var diskLow = false
    private(set) var hasDiskInfo = false

    // Transient info banner.
    private(set) var infoOpen = false
    private(set) var infoTitle = ""
    private(set) var infoMessage = ""
    private(set) var infoSeverity = InfoSeverity.info

    func showInfo(_ title: String, _ message: String, _ severity: InfoSeverity) {
        infoTitle = title
        infoMessage = message
        infoSeverity = severity
        infoOpen = true
    }

    func dismissInfo() { infoOpen = false }

    private func report(_ st: pin_status_t, _ what: String) {
        if st != PIN_OK { showInfo(what, Pin.strerror(st), st == PIN_ERR_STATE ? .warning : .error) }
    }

    // MARK: launch / script

    @ObservationIgnored private var script: Pin.Script?
    @ObservationIgnored private var scriptDevice = ""
    @ObservationIgnored private var scriptSettings: pin_script_settings_t?
    @ObservationIgnored private var pendingScript = false
    /// True while the command line's steps run (no dialogs then; the script reports by exit code).
    private(set) var scriptRunning = false

    // MARK: lifecycle state

    @ObservationIgnored private var startupDone = false
    @ObservationIgnored private var loading = false
    @ObservationIgnored private var settingsScope: String?
    @ObservationIgnored private(set) var isClosing = false
    @ObservationIgnored private var statusTimer: Timer?
    @ObservationIgnored private var meterTimer: Timer?
    @ObservationIgnored private let watchStop = StopFlag()
    @ObservationIgnored private var deviceWatch: Thread?
    @ObservationIgnored private var captureWithDeck = false
    /// When pin_capture_start was accepted (the core starts asynchronously: the state stays READY for a
    /// moment). Stops a quick second click from planning and starting another capture.
    @ObservationIgnored private var startRequestedAt: Double?
    @ObservationIgnored private let keepAwake = KeepAwake()
    @ObservationIgnored private var holdLeft = PeakHold(floor: WindowModel.meterFloorDb)
    @ObservationIgnored private var holdRight = PeakHold(floor: WindowModel.meterFloorDb)
    @ObservationIgnored private var meterAccL = -Double.infinity
    @ObservationIgnored private var meterAccR = -Double.infinity
    @ObservationIgnored private var meterLastTick = 0.0
    /// A normal capture end whose notification waits one tick in case a no-video event follows.
    @ObservationIgnored private var pendingFinished: (text: String, age: Int)?

    // MARK: init

    init() {
        // Initial values are assigned in init, which does not run the observers: a default must not be
        // saved over a stored setting before it is loaded.
        analogFormats = FormatItem.formats(for: PIN_KIND_ANALOG)
        analogFormat = analogFormats.first(where: \.isDefault) ?? analogFormats.first

        var std = [StdItem(std: PIN_STD_AUTO, name: "Auto")]
        for i in 1..<Int(PIN_STD_COUNT.rawValue) {
            let s = pin_std_t(rawValue: UInt32(i))
            std.append(StdItem(std: s, name: Pin.stdName(s)))
        }
        standards = std
        selectedStandard = std[0]

        // The slider callbacks need `self`, which is not usable before all properties are set.
        var onSlider: (pin_control_t, Int) -> Void = { _, _ in }
        let forward: (pin_control_t, Int) -> Void = { c, v in onSlider(c, v) }
        pictureSliders = [PIN_CTL_BRIGHTNESS, PIN_CTL_CONTRAST, PIN_CTL_SATURATION, PIN_CTL_HUE, PIN_CTL_SHARPNESS]
            .map { ControlSliderModel($0, onChanged: forward) }
        audioGain = ControlSliderModel(PIN_CTL_AUDIO_GAIN, isDb: true, onChanged: forward)
        onSlider = { [unowned self] c, v in MainActor.assumeIsolated { self.sliderChanged(c, v) } }

        for tab in kindTabs {
            tab.onChanged = { [unowned self] k, field in self.kindChanged(k, field) }
        }
    }

    private func sliderChanged(_ c: pin_control_t, _ value: Int) {
        if let s = session { Pin.setControl(s, c, value) }
        saveSetting("gui.control_\(c.rawValue)", String(value))
    }

    private func kindChanged(_ k: KindSettingsModel, _ field: KindSettingsModel.Field) {
        saveKindSettings(k)
        if field == .aspect { applyPreviewAspect() }
        if field == .format || field == .aspect { pushOutputHint() }
    }

    /// Re-applies the last-used proc-amp / gain values to a freshly opened device.
    private func applySavedControls() {
        guard let s = session else { return }
        for slider in allSliders {
            if let v = Int(loadSetting("gui.control_\(slider.control.rawValue)")) {
                Pin.setControl(s, slider.control, v)
            }
        }
    }

    func refreshControls() {
        guard let s = session else { return }
        for slider in allSliders {
            if let info = Pin.control(s, slider.control) { slider.load(from: info) }
        }
    }

    // MARK: start / shutdown

    /// Hands over the parsed command line (AppModel). Call before `start()`.
    func configureLaunch(script: Pin.Script?) {
        self.script = script
        guard let script else { return }
        scriptDevice = script.device
        // --device may be a recording to replay: the core lists it as "replay:<name>" once registered.
        if !scriptDevice.isEmpty, FileManager.default.fileExists(atPath: scriptDevice) {
            Pin.setReplayFile(scriptDevice)
            scriptDevice = "replay:" + (scriptDevice as NSString).lastPathComponent
        }
        scriptSettings = script.settings()
        pendingScript = script.needsSession
    }

    /// Startup (RootGrid_Loaded): global settings, device list, pick + open the device, timers, device watch.
    func start() {
        loadGlobalSettings()
        refreshDevices()
        // Selecting the device loads its own settings; the command-line settings go on top of them.
        selectedDeviceID = pickInitialDevice(preferred: script == nil || scriptDevice.isEmpty ? nil : scriptDevice)
        if script != nil, let s = scriptSettings { applyScriptSettings(s) }
        startupDone = true
        if let m = preview as? MetalPreview, let why = m.unavailableReason {
            showInfo("Preview unavailable", "Metal could not be initialised: \(why)", .warning)
        }
        openSelectedIfNeeded()

        statusTimer = makeTimer(interval: 0.1) { [unowned self] in self.tick() }
        meterTimer = makeTimer(interval: 1.0 / 30.0) { [unowned self] in self.updateMeters() }
        startDeviceWatch()
    }

    private func makeTimer(interval: TimeInterval, _ body: @escaping @MainActor () -> Void) -> Timer {
        // .common: ticks keep running during menu tracking and live resize.
        let t = Timer(timeInterval: interval, repeats: true) { _ in MainActor.assumeIsolated { body() } }
        RunLoop.main.add(t, forMode: .common)
        return t
    }

    /// Stops the timers and the device watch and closes the session (pin_close blocks until files are
    /// finalised: the close flow waits for READY first).
    func shutdown() {
        isClosing = true
        statusTimer?.invalidate(); statusTimer = nil
        meterTimer?.invalidate(); meterTimer = nil
        watchStop.set()
        Pin.devicesWake()
        closeSession()
        keepAwake.set(false)
    }

    // MARK: devices: list, pick, open, close

    /// Rebuilds the list from pin_enumerate (only when something visible changed).
    func refreshDevices() {
        let fresh = Pin.enumerateDevices().map { info -> DeviceItem in
            let id = info.idString
            return DeviceItem(info, capturingHere: isCapturing && id == openedDeviceID)
        }
        let same = fresh.count == devices.count && zip(fresh, devices).allSatisfy { $0.sameRow(as: $1) }
        if !same { devices = fresh }
        noDevices = devices.isEmpty
        if session == nil {
            noVideoText = noDevices ? "No device connected" : "No device open"
            statusShortText = noVideoText
        }
        if let keep = selectedDeviceID, !devices.contains(where: { $0.id == keep }) {
            selectedDeviceID = nil
        }
    }

    /// The device to use at startup: launch preset, else last-used, else the first usable one.
    func pickInitialDevice(preferred: String?) -> String? {
        var pick: DeviceItem?
        if let p = preferred, !p.isEmpty, p != "first" {
            // --device: an id, a serial, or a replay file (the core lists that as "replay:<name>")
            pick = devices.first { $0.isUsable && ($0.id == p || $0.serial.caseInsensitiveCompare(p) == .orderedSame) }
        }
        if pick == nil, preferred != "first" {
            let last = loadGlobalSetting("gui.last_device")
            if !last.isEmpty {
                pick = devices.first { $0.isUsable && ($0.id == last || $0.serial.caseInsensitiveCompare(last) == .orderedSame) }
            }
        }
        if pick == nil { pick = devices.first { $0.isUsable } }   // in-use devices can't be picked at all
        return pick?.id
    }

    private func selectedDeviceChanged() {
        guard let dev = selectedDevice else { return }   // transient nil: never close on it
        if !isCapturing {
            let scope = Pin.deviceSettingsPrefix(serial: dev.serial, id: dev.id)
            if scope != settingsScope {
                settingsScope = scope
                loadSettings()
            }
        }
        openSelectedIfNeeded()
    }

    /// Opens the selected device unless it is already the open one (never while capturing).
    func openSelectedIfNeeded() {
        guard startupDone, !isCapturing, let dev = selectedDevice, dev.id != openedDeviceID else { return }
        if openSelectedDevice() {
            refreshDevices()   // this device's badge flips to "Open" (OPEN_HERE)
        }
    }

    /// Opens `selectedDevice`, closing whatever was open before.
    @discardableResult
    func openSelectedDevice() -> Bool {
        closeSession()
        guard let dev = selectedDevice else {
            noVideoText = noDevices ? "No devices found" : "No device selected"
            return false
        }
        guard dev.isUsable else {
            showInfo(dev.name, dev.openProblem ?? "This device can't be opened right now.", .warning)
            noVideoText = dev.statusText
            return false
        }
        let (status, opened) = Pin.open(dev.id)
        guard status == PIN_OK, let s = opened else {
            showInfo("Could not open the device", "\(dev.name): \(Pin.strerror(status))", .error)
            noVideoText = "Could not open the device"
            return false
        }
        session = s
        openedDeviceID = dev.id
        saveGlobalSetting("gui.last_device", dev.serial.isEmpty ? dev.id : dev.serial)

        appliedAspect = nil
        applyPreviewAspect()
        Pin.monitorEnable(s, !isMuted)
        audio.isMuted = isMuted
        audio.attach(session: s)
        if !isMuted { audio.start() }
        preview.attach(session: s)
        report(Pin.setInput(s, inputIndex), "Prepare device")
        if let std = selectedStandard { Pin.setStandard(s, std.std) }
        applySavedControls()
        refreshControls()
        noVideoText = "Preparing device…"
        pushOutputHint()
        return true
    }

    /// Closes the session. pin_close blocks until any capture is finalised: callers stop the capture
    /// and wait for READY first (see the close flow).
    func closeSession() {
        guard let s = session else { return }
        openedDeviceID = nil
        audio.stop()
        audio.detach()      // never call into a closed session
        preview.detach()
        Pin.close(s)
        session = nil
        sessionState = PIN_STATE_CLOSED
        sessionStateText = "No device open"
        statusShortText = "No device open"
        isCapturing = false
        previewHasVideo = false
    }

    // MARK: device watch

    /// Device list updates without polling: a background thread blocks in pin_devices_wait (plug/unplug,
    /// or another window taking or releasing a device) and hands each change to the main actor.
    private func startDeviceWatch() {
        let stop = watchStop
        let t = Thread { [weak self] in
            while !stop.isSet {
                let r = Pin.devicesWait(timeoutMs: 60_000)
                if stop.isSet { break }
                if r < 0 { Thread.sleep(forTimeInterval: 2) }   // no notifications available: slow rescan
                if r != 0 {
                    DispatchQueue.main.async { MainActor.assumeIsolated { self?.onDevicesChanged() } }
                }
            }
        }
        t.name = "Device watch"
        t.qualityOfService = .utility
        deviceWatch = t
        t.start()
    }

    /// `sessionFailed`: called because the open session went to ERROR (not by a device-list change); it is
    /// only dropped then if its device has gone from the list.
    private func onDevicesChanged(sessionFailed: Bool = false) {
        guard !isClosing else { return }
        refreshDevices()
        // The open device was unplugged: drop the session (a running capture ends with an error from
        // the core and is finalised by it; that ERROR can arrive before or after this list change, so
        // it triggers this too, else a replug of the same device would keep the dead session). After a
        // device-list change a session in ERROR is dropped even if its device is still listed
        // (replugged), so the device is opened afresh.
        let dead = !sessionFailed && sessionState == PIN_STATE_ERROR
        if let open = openedDeviceID, !isCapturing,
           dead || !devices.contains(where: { $0.id == open }) {
            closeSession()
            preview.detach()
        }
        // Nothing open (none connected before, or the previous one went away): take the first usable one.
        if selectedDevice == nil || openedDeviceID == nil {
            selectedDeviceID = pickInitialDevice(preferred: nil)
        }
        openSelectedIfNeeded()
    }

    // MARK: deck

    /// A click on a deck button. Ignored while capturing (the buttons are disabled then).
    func userRequestedDeck(_ cmd: DeckCommand) {
        guard let s = session, !isCapturing else { return }
        report(Pin.deck(s, cmd.native), "Deck command")
    }

    private func applyDeckStatus(_ state: pin_deck_state_t, busy: Bool) {
        deckBusy = busy
        deckState = state
        isRewChecked = state == PIN_DECK_REWINDING
        isPlayChecked = state == PIN_DECK_PLAYING || state == PIN_DECK_RECORDING
        isStopChecked = state == PIN_DECK_STOPPED
        isFfChecked = state == PIN_DECK_FAST_FORWARD
        deckStateText = Pin.deckStateName(state)
        deckTip = Pin.deckTip(state, busy: busy)
    }

    // MARK: capture

    /// Capture options from the current state; `startDeck` = "Automatic rewind & capture".
    func buildCaptureOpts(startDeck: Bool, rewindFirst: Bool = false) -> pin_capture_opts_t {
        var o = Pin.captureOptsDefaults()
        let analog = !isDvInput
        let path = analog ? analogOutputPath : dvOutputPath
        setCString(&o.path, path)
        o.first_number = Pin.nextFileNumber(path: path)   // every file is "name-NNNN.ext": continue after the highest

        if let f = analogFormat { o.format_analog = f.format }
        if let f = dvSettings.selectedFormat { o.format_dv = f.format }
        if let f = hdvSettings.selectedFormat { o.format_hdv = f.format }

        // The DV input auto-detects DV vs HDV; the per-kind scalar options come from the tab matching
        // what is arriving right now.
        let kind = lastStreamKind == PIN_KIND_HDV ? hdvSettings : dvSettings
        let title = analog ? (analogFormat?.supportsTitle == true ? analogName : "") : (kind.titleEnabled ? dvName : "")
        setCString(&o.title, title)
        o.aspect = pin_aspect_t(rawValue: UInt32(clamping: activeAspectIndex))
        o.scene_split = !analog && kind.splitIntoScenes && kind.splitEnabled ? 1 : 0
        o.idle_stop_minutes = Int32(clamping: Swift.max(0, analog ? analogIdleStopMinutes : kind.idleStopMinutes))
        o.max_duration_minutes = Int32(clamping: Swift.max(0, analog ? analogMaxDurationMinutes : kind.maxDurationMinutes))
        o.passes = Int32(analog ? 1 : kind.effectivePasses)
        o.start_deck = startDeck ? 1 : 0
        o.rewind_first = rewindFirst ? 1 : 0   // "Automatic rewind & capture": start of tape, then play
        return o
    }

    /// StartCaptureAsync minus the dialogs: validates, runs pin_check_output and returns the plan with the
    /// confirmations to ask (in order), or nil after showing the reason in the info banner.
    func planCapture(playFirst: Bool) -> CapturePlan? {
        let path = isDvInput ? dvOutputPath : analogOutputPath
        if path.trimmingCharacters(in: .whitespaces).isEmpty {
            showInfo("No output file", "Choose where to save the capture first.", .warning)
            return nil
        }
        // "Automatic rewind & capture" = rewind to the start of the tape, play, record.
        let opts = buildCaptureOpts(startDeck: playFirst, rewindFirst: playFirst)
        guard let s = session else {
            showInfo("Can't capture to this location", Pin.strerror(PIN_ERR_STATE), .error)
            return nil
        }
        let (st, check) = Pin.checkOutput(s, opts)
        let message = cString(check.message)
        if st == PIN_ERR_ARG, !message.isEmpty {
            showInfo("Invalid file name", message, .error)
            return nil
        }
        if st != PIN_OK {
            showInfo("Can't capture to this location", "\(Pin.strerror(st)) \(message)".trimmingCharacters(in: .whitespaces), .error)
            return nil
        }

        var confirmations: [CaptureConfirmation] = []
        let firstPath = cString(check.first_path)
        if !message.isEmpty || check.fat32 != 0 {
            var msg = message
            if check.fat32 != 0, msg.isEmpty {
                msg = "The destination is a FAT32 drive. Files larger than 4 GB will fail."
            }
            confirmations.append(CaptureConfirmation(
                title: "Check the destination",
                message: "\(msg)\n\nFree space: \(Pin.formatBytes(check.free_bytes)) (about \(check.minutes_left) minutes at this format).",
                primaryButton: "Capture anyway"))
        }
        if check.low_space != 0 {
            confirmations.append(CaptureConfirmation(
                title: "Low disk space",
                message: "Only \(Pin.formatBytes(check.free_bytes)) free on \(Self.volumeName(for: firstPath)). Continue?",
                primaryButton: "OK"))
        }
        if check.collision != 0 {
            confirmations.append(CaptureConfirmation(
                title: "Replace existing file?",
                message: "\(firstPath)\n\nalready exists. Do you want to overwrite it?",
                primaryButton: "Overwrite"))
        }
        return CapturePlan(opts: opts, confirmations: confirmations, overwrite: check.collision != 0)
    }

    /// The volume a path is (or would be) on, for "Only 1.2 GB free on ...".
    private static func volumeName(for path: String) -> String {
        var url = URL(fileURLWithPath: path)
        while !FileManager.default.fileExists(atPath: url.path), url.path != "/" { url.deleteLastPathComponent() }
        return (try? url.resourceValues(forKeys: [.volumeNameKey]))?.volumeName ?? "the output drive"
    }

    @discardableResult
    func startCapture(_ o: pin_capture_opts_t, overwrite: Bool) -> pin_status_t {
        guard let s = session else { return PIN_ERR_STATE }
        let st = Pin.captureStart(s, o, overwrite: overwrite)
        if st == PIN_OK {
            captureStarted(o)
        }
        report(st, "Start capture")
        return st
    }

    /// True for a moment after an accepted start, until the status shows the capture (or 2 s pass).
    var captureStartPending: Bool {
        guard let t = startRequestedAt else { return false }
        if isCapturing || ProcessInfo.processInfo.systemUptime - t > 2 {
            startRequestedAt = nil
            return false
        }
        return true
    }

    private func captureStarted(_ o: pin_capture_opts_t) {
        startRequestedAt = ProcessInfo.processInfo.systemUptime
        captureWithDeck = o.start_deck != 0
        activeIdleStopS = Double(Swift.max(0, o.idle_stop_minutes)) * 60
        activeDurationS = Double(Swift.max(0, o.max_duration_minutes)) * 60
        if infoSeverity == .error { infoOpen = false }   // a stale error must not colour the dock of a running capture
        Notifier.shared.requestAuthorizationIfNeeded()
    }

    /// pin_capture_stop: the deck is stopped only if the capture started it.
    func stopCapture() {
        guard let s = session else { return }
        report(Pin.captureStop(s, deck: nil), "Stop capture")
    }

    /// Stops the capture and decides about the tape: true = deck Stop, false = leave it running.
    func stopCapture(stopDeck: Bool) {
        guard let s = session else { return }
        report(Pin.captureStop(s, deck: stopDeck ? PIN_STOP_DECK_YES : PIN_STOP_DECK_NO), "Stop capture")
    }

    /// Stops the way the capture was started (manual: tape keeps running; automatic: tape stops too).
    func stopCaptureAsStarted() {
        guard let s = session else { return }
        report(Pin.captureStop(s, deck: PIN_STOP_DECK_AS_STARTED), "Stop capture")
    }

    // MARK: polling

    // The core reports the loudest peak since the previous status read and resets it, so every read
    // (the 100 ms tick and the meter tick) goes through feedMeter; the meter tick publishes the max
    // with a decay.
    private func feedMeter(_ st: pin_status_snapshot_t) {
        meterAccL = Swift.max(meterAccL, Double(st.audio_peak_db.0))
        meterAccR = Swift.max(meterAccR, Double(st.audio_peak_db.1))
    }

    /// Meter timer (~30 Hz): fresh status read, peak with a short decay.
    func updateMeters() {
        if let s = session, let st = Pin.status(s) { feedMeter(st) }
        let now = ProcessInfo.processInfo.systemUptime
        let dt = meterLastTick == 0 ? 0 : Swift.min(0.25, now - meterLastTick)
        meterLastTick = now
        let fall = Self.meterDecayDbPerSecond * dt
        let floor = Self.meterFloorDb
        let l = Swift.min(Swift.max(meterAccL, floor), 0)
        let r = Swift.min(Swift.max(meterAccR, floor), 0)
        meterAccL = -.infinity
        meterAccR = -.infinity
        // Assign only on change: an @Observable setter notifies even for an equal value, which would
        // re-render the meters 30 times a second while idle (no device, or silence).
        let peakL = Swift.max(l, audioPeakLeft - fall), peakR = Swift.max(r, audioPeakRight - fall)
        if peakL != audioPeakLeft { audioPeakLeft = peakL }
        if peakR != audioPeakRight { audioPeakRight = peakR }
        // The hold tick follows the true (undecayed) peaks.
        let holdL = holdLeft.push(l), holdR = holdRight.push(r)
        if holdL != audioHoldLeft { audioHoldLeft = holdL }
        if holdR != audioHoldRight { audioHoldRight = holdR }
    }

    /// The 100 ms tick: process-wide log lines, status snapshot, session events.
    func tick() {
        // The core's own log lines (process-wide, session or not). Only the console sees these: some
        // warnings are routine retries, not worth a banner. The session's events go to both.
        while let pe = Pin.pollEvent(nil) { ConsoleOutput.write(event: pe) }

        if let s = session {
            if let st = Pin.status(s) { applyStatus(st) }
            while let s = session, let e = Pin.pollEvent(s) { handleEvent(e) }
        }
        flushPendingFinished()
        runPendingScriptIfReady()
        let hasVideo = preview.hasRecentFrame
        if hasVideo != previewHasVideo { previewHasVideo = hasVideo }
        trackPreviewAspect()
        keepAwake.set(isCapturing)
        afterTick?()
        for o in tickObservers { o() }
    }

    private func applyStatus(_ st: pin_status_snapshot_t) {
        sessionState = st.state
        sessionStateText = Pin.formatState(st)

        statusLine = Pin.formatStatusLine(st)
        let behindHub = openDevice?.isBehindHub ?? false
        let (short, hubReady) = Pin.formatStatusShort(st, behindHub: behindHub)
        statusShortText = short
        let capturingState = Pin.stateIsCapturing(st.state)
        statusSubText = capturingState ? sessionStateText : ""
        statusTip = !statusSubText.isEmpty && statusSubText != statusShortText ? "\(statusSubText)\n\(statusShortText)" : statusShortText
        if hubReady, let hint = openDevice?.hubHint { statusTip = hint }

        let tc = cString(st.timecode)
        timecode = tc.isEmpty ? "--:--:--:--" : tc
        if st.input == PIN_INPUT_DV {
            statusTimeText = timecode
            statusTimeTip = "Tape timecode"
        } else {
            let total = Int(Swift.max(0, st.elapsed_s))
            statusTimeText = String(format: "%02d:%02d:%02d", total / 3600, total / 60 % 60, total % 60)
            statusTimeTip = "Time since capture start"
        }

        signalLocked = st.signal != 0
        signalLockText = signalLocked ? "Locked" : "No signal"
        signalTypeText = Pin.formatSignal(st)

        framesTotalText = Pin.formatFrames(st)
        framesTip = Pin.formatFramesDetail(st)
        sizeText = Pin.formatSizes(st)

        feedMeter(st)
        audioPeakText = "Audio peak left \(Self.formatDb(st.audio_peak_db.0)), right \(Self.formatDb(st.audio_peak_db.1))"

        tapePercent = Int(st.tape_percent)
        if lastStreamKind != st.stream_kind {
            lastStreamKind = st.stream_kind
            if st.stream_kind == PIN_KIND_DV || st.stream_kind == PIN_KIND_HDV {
                selectedKindTabIndex = st.stream_kind == PIN_KIND_HDV ? 1 : 0
            }
            applyPreviewAspect()
            pushOutputHint()
        }

        diskLow = st.disk_low != 0
        storageFreeText = Pin.formatStorageFree(st)
        storageTip = Pin.formatStorageDetail(st)
        hasDiskInfo = st.disk_free_bytes > 0

        isCapturing = capturingState

        applyDeckStatus(st.deck, busy: st.deck_busy != 0)

        // One place says why there is no picture: the core's own sentence (bring-up step,
        // "no camera found", "no signal on the composite input", ...).
        let detail = cString(st.detail)
        switch st.state {
        case PIN_STATE_PREPARING:
            noVideoText = detail.isEmpty ? "Preparing device…" : detail
        case PIN_STATE_ERROR:
            let e = cString(st.error_text)
            noVideoText = e.isEmpty ? "Device error" : e
        default:
            noVideoText = detail.isEmpty ? (isDvInput ? "No camera or deck signal." : "No video signal.") : detail
        }
        let counting = st.state == PIN_STATE_CAPTURING && st.signal == 0 && st.idle_stop_remaining_s >= 0
        if counting {
            let left = Pin.formatRemaining(st.idle_stop_remaining_s)
            noSignalCountdownText = "Stopping capture in \(left)"
            noSignalCountdownFraction = activeIdleStopS > 0 ? Swift.min(Swift.max(st.idle_stop_remaining_s / activeIdleStopS, 0), 1) : 0
            stopCountdownSuffix = " (\(left))"
        } else {
            stopCountdownSuffix = ""
        }
        noSignalCountdownVisible = counting
        progressVisible = st.state == PIN_STATE_PREPARING
        progressIndeterminate = st.progress_percent < 0
        progressValue = Double(Swift.max(0, st.progress_percent))
        let p = Pin.statusProgress(st, idleTotalS: activeIdleStopS, durationTotalS: activeDurationS)
        progressMode = p.mode
        progressFraction = p.fraction

        // Deck buttons have nothing to talk to without a camera (analog inputs report -1).
        deckAvailable = !isDvInput || st.camera_present != 0

        windowTitle = Pin.formatWindowTitle(st, deviceName: selectedDevice?.displayName ?? Self.defaultDeviceName)
    }

    private static func formatDb(_ db: Float) -> String { db <= -143 ? "silent" : String(format: "%.1f dBFS", db) }

    private func handleEvent(_ e: Pin.Event) {
        // Log lines also arrive through the process-wide queue (drained in tick), which prints them.
        if e.kind != PIN_EVT_LOG { ConsoleOutput.write(event: e) }
        switch e.kind {
        case PIN_EVT_LOG:
            lastLogLine = e.text
            if e.a >= 3 { showInfo("Error", e.text, .error) }
            else if e.a == 2 { showInfo("Warning", e.text, .warning) }
        case PIN_EVT_ERROR:
            showInfo(Pin.strerror(pin_status_t(rawValue: UInt32(clamping: e.a))), e.text, .error)
        case PIN_EVT_FILE_OPENED:
            lastLogLine = "Writing \(e.text)"
        case PIN_EVT_FILE_CLOSED:
            let st = pin_status_t(rawValue: UInt32(clamping: e.a))
            lastLogLine = st == PIN_OK ? "Saved \(e.text)" : "Problem finalising \(e.text): \(Pin.strerror(st))"
            if st != PIN_OK { showInfo("File not finalised cleanly", lastLogLine, .error) }
        case PIN_EVT_SCENE:
            lastLogLine = "Scene \(e.a)"
        case PIN_EVT_PASS:
            lastLogLine = "Pass \(e.a)"
        case PIN_EVT_DECK:
            applyDeckStatus(pin_deck_state_t(rawValue: UInt32(clamping: e.a)), busy: deckBusy)
        case PIN_EVT_INPUT_FORMAT:
            refreshControls()   // e.g. hue enabled/disabled after a 50/60 Hz change
        case PIN_EVT_DEVICES:
            refreshDevices()
        case PIN_EVT_CAPTURE_ENDED:
            lastLogLine = e.text   // abnormal: the alert below; the status bar shows it either way
        case PIN_EVT_STATE:
            sessionState = pin_state_t(rawValue: UInt32(clamping: e.a))
            refreshDevices()   // badge: Open / Capturing
            if sessionState == PIN_STATE_ERROR {
                // e.g. the device was unplugged during a capture (the core finalised the files and
                // reported the stop): release the dead session if its device is gone. Not inline: this
                // runs while the session's events are being drained.
                DispatchQueue.main.async { [weak self] in MainActor.assumeIsolated { self?.onDevicesChanged(sessionFailed: true) } }
            }
        case PIN_EVT_STEP:
            lastLogLine = e.text
        case PIN_EVT_DONE:
            // a = exit code (docs/cli.md), text = the reason when it is not 0
            if e.a == 0 { showInfo("Done", "The command-line steps finished.", .success) }
            else { showInfo("Command line stopped", e.text.isEmpty ? "Exit code \(e.a)" : e.text, .error) }
        default:
            break
        }
        afterEvent(e)
        onEngineEvent?(EngineEvent(kind: e.kind, a: e.a, text: e.text))
    }

    /// The window-level reactions of MainWindow.VM_EngineEvent, plus the background notifications.
    private func afterEvent(_ e: Pin.Event) {
        let quiet = isClosing || isFinalizingForClose
        switch e.kind {
        case PIN_EVT_DONE:
            scriptRunning = false
            if let script, script.exitWhenDone, !isClosing {
                onExitRequested?(Int32(clamping: e.a))   // --exit-when-done: leave with the script's exit code
            }
        case PIN_EVT_CAPTURE_ENDED:
            let abnormal = Pin.stopReasonAbnormal(pin_stop_reason_t(rawValue: UInt32(clamping: e.a)))
            if abnormal {
                // A dialog with how much was captured. A normal end only goes to the status bar. Not
                // while closing or running unattended command-line steps.
                if !quiet && !scriptRunning { onAlert?(ModelAlert(title: "Capture stopped", message: e.text)) }
                if !quiet {
                    Notifier.shared.postIfInactive(title: "Capture stopped", body: e.text)
                    onAttention?(true)
                }
                pendingFinished = nil
            } else if !quiet {
                pendingFinished = (e.text, 0)   // a no-video event may follow right away
            }
        case PIN_EVT_NO_VIDEO:
            // A capture that received no video at all (an empty tape): no file was written. A warning
            // banner; a command-line script reports it itself through its exit code (Done).
            if !quiet && !scriptRunning { showInfo("No video received", e.text, .warning) }
            if !quiet {
                Notifier.shared.postIfInactive(title: "No video received", body: e.text)
                onAttention?(false)   // a normal end (no-signal timeout on an empty tape): not the critical bounce
            }
            pendingFinished = nil
        default:
            break
        }
    }

    /// A normal end is announced one tick later, unless a no-video event replaced it.
    private func flushPendingFinished() {
        guard var p = pendingFinished else { return }
        if p.age >= 1 {
            pendingFinished = nil
            Notifier.shared.postIfInactive(title: "Capture finished", body: p.text)
            onAttention?(false)
        } else {
            p.age += 1
            pendingFinished = p
        }
    }

    // MARK: command line

    /// Applies the settings a command line gave before its first action (pin_script_settings) over the
    /// loaded settings: input, standard, formats, aspect, split and the analog controls (applied when the
    /// device opens, like the saved ones). The title and --keep-raw have no field in this window; the
    /// script's own captures use them.
    func applyScriptSettings(_ l: pin_script_settings_t) {
        if l.has_input != 0 { inputIndex = Int(l.input.rawValue) }
        if l.has_std != 0, let s = standards.first(where: { $0.std == l.std }) { selectedStandard = s }
        let analog = !isDvInput
        if l.has_format_analog != 0, let f = analogFormats.first(where: { $0.format == l.format_analog }) { analogFormat = f }
        if l.has_format_dv != 0 { dvSettings.applyFormat(l.format_dv) }
        if l.has_format_hdv != 0 { hdvSettings.applyFormat(l.format_hdv) }
        if l.has_split != 0 {
            dvSettings.splitIntoScenes = l.split != 0
            hdvSettings.splitIntoScenes = l.split != 0
        }
        if l.has_aspect != 0 {
            // --aspect applies to the kinds the chosen input can deliver.
            if analog { analogAspectIndex = Int(l.aspect.rawValue) }
            else {
                dvSettings.aspectIndex = Int(l.aspect.rawValue)
                hdvSettings.aspectIndex = Int(l.aspect.rawValue)
            }
        }
        for slider in allSliders {
            if let v = l.value(for: slider.control) { saveSetting("gui.control_\(slider.control.rawValue)", String(v)) }
        }
    }

    private func runPendingScriptIfReady() {
        guard pendingScript, let script, let s = session, sessionState == PIN_STATE_READY else { return }
        pendingScript = false
        let st = script.run(on: s)
        report(st, "Command line")
        scriptRunning = st == PIN_OK
        // The script's captures end with a notification too (an unattended tape run is the
        // typical case); ask for permission now, as a window-started capture does.
        if scriptRunning { Notifier.shared.requestAuthorizationIfNeeded() }
    }

    // MARK: settings

    // Every option except the app-global ones (last device, mute; the window frame is NSWindow's
    // autosave) is stored per device under the core's "dev_<GUID>." prefix. With no device selected
    // there is no scope: loads give the defaults and nothing is written.

    private func loadSetting(_ key: String, fallback: String = "") -> String {
        settingsScope.map { loadGlobalSetting($0 + key, fallback: fallback) } ?? fallback
    }

    private func loadGlobalSetting(_ key: String, fallback: String = "") -> String {
        Pin.settingsGet(key: key, fallback: fallback)
    }

    private func saveSetting(_ key: String, _ value: String) {
        if let scope = settingsScope { saveGlobalSetting(scope + key, value) }
    }

    private func saveGlobalSetting(_ key: String, _ value: String) {
        if loading { return }
        Pin.settingsSet(key: key, value: value)   // best effort
    }

    private func saveKindSettings(_ k: KindSettingsModel) {
        let p = k.settingsPrefix
        if let f = k.selectedFormat { saveSetting("gui.format_\(p)", String(f.format.rawValue)) }
        saveSetting("gui.split_\(p)", k.splitIntoScenes ? "1" : "0")
        saveSetting("gui.passes_\(p)", String(k.passes))
        saveSetting("gui.idle_\(p)", String(k.idleStopMinutes))
        saveSetting("gui.duration_\(p)", String(k.maxDurationMinutes))
        saveSetting("gui.aspect_\(p)", String(k.aspectIndex))
    }

    private func loadInt(_ key: String, _ fallback: Int) -> Int { Int(loadSetting(key)) ?? fallback }

    /// App-global (not per-device) settings, read once at startup.
    private func loadGlobalSettings() {
        loading = true
        defer { loading = false }
        isMuted = loadGlobalSetting("gui.muted", fallback: "1") != "0"
        // Developer aid: start unmuted without touching the saved setting (`loading` blocks the save).
        if ProcessInfo.processInfo.environment["MARVIN_UNMUTE"] == "1" { isMuted = false }
    }

    /// Loads the selected device's settings (defaults for a device never seen before).
    private func loadSettings() {
        loading = true
        defer { loading = false }
        inputIndex = Swift.min(Swift.max(loadInt("gui.last_input", 0), 0), 2)
        analogAspectIndex = Swift.min(Swift.max(loadInt("gui.aspect_analog", 0), 0), 2)
        let a = loadOutput("analog", defaultName: "analog")
        analogOutputDir = a.dir
        analogName = a.name
        analogIdleStopMinutes = Swift.max(0, loadInt("gui.idle_analog", 5))
        analogMaxDurationMinutes = Swift.max(0, loadInt("gui.duration_analog", 0))
        selectedKindTabIndex = Swift.min(Swift.max(loadInt("gui.last_kind", 0), 0), 1)
        lastStreamKind = selectedKindTabIndex == 1 ? PIN_KIND_HDV : PIN_KIND_DV
        let std = loadInt("gui.std", Int(PIN_STD_AUTO.rawValue))
        selectedStandard = standards.first { Int($0.std.rawValue) == std } ?? standards[0]
        let d = loadOutput("dv", defaultName: "tape")
        dvOutputDir = d.dir
        dvName = d.name

        let fa = loadInt("gui.format_analog", -1)
        if let f = analogFormats.first(where: { Int($0.format.rawValue) == fa }) { analogFormat = f }

        for k in kindTabs {
            let p = k.settingsPrefix
            let f = loadInt("gui.format_\(p)", -1)
            if f >= 0 { k.applyFormat(pin_format_t(rawValue: UInt32(f))) }
            k.splitIntoScenes = loadInt("gui.split_\(p)", 0) != 0
            k.passes = Swift.max(1, loadInt("gui.passes_\(p)", 1))
            k.idleStopMinutes = Swift.max(0, loadInt("gui.idle_\(p)", 5))
            k.maxDurationMinutes = Swift.max(0, loadInt("gui.duration_\(p)", 0))
            k.aspectIndex = Swift.min(Swift.max(loadInt("gui.aspect_\(p)", 0), 0), 2)
        }
    }

    /// The directory and name for this device, falling back to the defaults.
    private func loadOutput(_ key: String, defaultName: String) -> (dir: String, name: String) {
        var dir = loadSetting("gui.dir_\(key)")
        var name = loadSetting("gui.name_\(key)")
        if dir.isEmpty { dir = Self.defaultOutputDir() }
        if name.isEmpty { name = defaultName }
        return (dir, name)
    }

    // MARK: paths

    /// ~/Movies, where Windows uses MyVideos.
    static func defaultOutputDir() -> String {
        FileManager.default.urls(for: .moviesDirectory, in: .userDomainMask).first?.path
            ?? (NSHomeDirectory() as NSString).appendingPathComponent("Movies")
    }

    private static func combinePath(_ dir: String, _ name: String) -> String {
        if name.trimmingCharacters(in: .whitespaces).isEmpty { return "" }
        if dir.trimmingCharacters(in: .whitespaces).isEmpty { return name }
        return (dir as NSString).appendingPathComponent(name)
    }
}
