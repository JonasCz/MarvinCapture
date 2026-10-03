using System;
using System.Collections.ObjectModel;
using System.Globalization;
using System.Linq;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using PinnacleCapture.Interop;
using PinnacleCapture.Services;

namespace PinnacleCapture.ViewModels;

public sealed partial record StdItem(PinStd Std, string Name)
{
    public override string ToString() => Name;
}

/// <summary>
/// The whole window's state. All engine access goes through Interop.Native;
/// nothing here touches XAML types, so it could be driven headless.
///
/// Update model: MainWindow runs one DispatcherQueueTimer at 100 ms that
/// calls <see cref="Tick"/>, which snapshots pin_get_status and drains
/// pin_poll_event. There is no other polling.
/// </summary>
public sealed partial class MainViewModel : ObservableObject, IDisposable
{
    private const string DefaultDeviceName = "Pinnacle Studio 500-USB";

    // ================================================================== devices

    public ObservableCollection<DeviceItemViewModel> Devices { get; } = new();

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasSelectedDevice), nameof(DeviceComboHeight))]
    private DeviceItemViewModel? _selectedDevice;

    /// <summary>The picker's fixed height: one caption line more for a device behind a USB hub.</summary>
    public double DeviceComboHeight => SelectedDevice is { IsBehindHub: true } ? 72 : 56;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(DeviceSelectEnabled))]
    private bool _noDevices = true;

    /// <summary>The device picker: fixed while capturing, and disabled outright when there is nothing to pick.</summary>
    public bool DeviceSelectEnabled => !IsCapturing && !NoDevices;

    public bool HasSelectedDevice => SelectedDevice is not null;

    private string? _openedDeviceId;

    /// <summary>True from capture start until the core reports READY again (includes STOPPING / REWINDING).</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsIdle), nameof(CaptureButtonText), nameof(CaptureButtonGlyph),
                              nameof(PlayAndCaptureEnabled), nameof(CaptureEnabled),
                              nameof(PrimaryDvTitle), nameof(PrimaryDvHelp), nameof(PrimaryDvGlyph), nameof(DeviceSelectEnabled),
                              nameof(ManualCaptureTitle), nameof(ManualCaptureHelp), nameof(ManualCaptureGlyph),
                              nameof(DeckRewEnabled), nameof(DeckPlayEnabled), nameof(DeckStopEnabled), nameof(DeckFfEnabled), nameof(DvAutoCaptureEnabled))]
    private bool _isCapturing;

    public bool IsIdle => !IsCapturing;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(PlayAndCaptureEnabled), nameof(CaptureEnabled), nameof(CanStop),
                              nameof(DeckRewEnabled), nameof(DeckPlayEnabled), nameof(DeckStopEnabled), nameof(DeckFfEnabled), nameof(DvAutoCaptureEnabled))]
    private PinState _sessionState = PinState.Closed;

    /// <summary>Stop is possible while writing (or between passes); not while already finalising.</summary>
    public bool CanStop => SessionState is PinState.Capturing or PinState.Rewinding;

    /// <summary>The Capture/Stop button (analog) and the Play-and-capture/Stop button (DV).</summary>
    public bool CaptureEnabled => IsCapturing ? CanStop : SessionState == PinState.Ready;

    /// <summary>Idle: start a capture without touching the tape. Capturing: stop it and leave the tape alone (always available, however the capture was started).</summary>
    public bool PlayAndCaptureEnabled => IsCapturing ? CanStop : SessionState == PinState.Ready;

    /// <summary>True when the running capture was started by "Automatic rewind &amp; capture" (the core stops the deck when it ends).</summary>
    private bool _captureWithDeck;

    public string ManualCaptureTitle => IsCapturing ? "Stop capture & continue tape" + StopCountdownSuffix : "Manual capture";
    public string ManualCaptureHelp => IsCapturing
        ? "Stops the capture and leaves the tape as it is"
        : "Records whatever the camera or deck is already sending, without controlling it";
    public string ManualCaptureGlyph => IsCapturing ? "" : ""; // Stop / Download

    /// <summary>False only when the core knows for sure that no camera is on the FireWire bus.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(DeckRewEnabled), nameof(DeckPlayEnabled), nameof(DeckStopEnabled), nameof(DeckFfEnabled), nameof(DvAutoCaptureEnabled))]
    private bool _deckAvailable = true;

    /// <summary>The deck buttons are usable only while idle and READY with a camera to talk to; never while capturing.</summary>
    private bool DeckControlsUsable => !IsCapturing && SessionState == PinState.Ready && DeckAvailable && DeckState != PinDeckState.NoTape;

    /// <summary>Each button is disabled when the deck is already doing what it would ask for.</summary>
    public bool DeckRewEnabled => DeckControlsUsable && DeckState != PinDeckState.Rewinding;
    public bool DeckPlayEnabled => DeckControlsUsable && DeckState is not (PinDeckState.Playing or PinDeckState.Recording);
    public bool DeckStopEnabled => DeckControlsUsable && DeckState != PinDeckState.Stopped;
    public bool DeckFfEnabled => DeckControlsUsable && DeckState != PinDeckState.FastForward;

    /// <summary>"Automatic rewind &amp; capture" drives the deck, so it needs a camera. While capturing (started that way) it stops the capture and the tape.</summary>
    public bool DvAutoCaptureEnabled => IsCapturing ? CanStop && DeckAvailable : SessionState == PinState.Ready && DeckAvailable;

    public string PrimaryDvTitle => IsCapturing ? "Stop capture & stop tape" + StopCountdownSuffix : "Automatic rewind & capture";
    public string PrimaryDvHelp => IsCapturing ? "Finishes the file, then stops the tape" : "Rewinds to the start of the tape, plays and records it";
    public string PrimaryDvGlyph => IsCapturing ? "\uE71A" : "\uE896"; // Stop / Download

    public string CaptureButtonText => IsCapturing ? "Stop capture" + StopCountdownSuffix : "Capture";

    partial void OnIsCapturingChanged(bool value)
    {
        // The open device's badge reads "Capturing" while this window records.
        RefreshDevices();
    }
    public string CaptureButtonGlyph => IsCapturing ? "" : ""; // Stop / Download

    [ObservableProperty] private string _sessionStateText = "No device open";

    // ================================================================== input

    public PinSessionHandle? Session { get; private set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ActivePanelIndex), nameof(IsDvInput))]
    private int _inputIndex; // PinInput order: DV, S-Video, Composite

    public PinInput SelectedInput => (PinInput)InputIndex;
    public bool IsDvInput => SelectedInput == PinInput.Dv;

    /// <summary>0 = analog panel, 1 = DV panel (fed to SameHeightSwitchPanel.ActiveIndex).</summary>
    public int ActivePanelIndex => IsDvInput ? 1 : 0;

    partial void OnInputIndexChanged(int value)
    {
        if (Session is { IsInvalid: false } && !_loading)
        {
            Report(Native.SetInput(Session, (PinInput)value), "Switch input");
            RefreshControls();
        }
        SaveSetting("gui.last_input", value.ToString(CultureInfo.InvariantCulture));
        ApplyPreviewAspect();
        PushOutputHint();
    }

    // ================================================================== analog

    public ObservableCollection<StdItem> Standards { get; } = new();

    [ObservableProperty] private StdItem? _selectedStandard;

    partial void OnSelectedStandardChanged(StdItem? value)
    {
        if (value is not null)
        {
            SaveSetting("gui.std", ((int)value.Std).ToString(CultureInfo.InvariantCulture));
        }
        if (value is not null && Session is { IsInvalid: false } && !_loading)
        {
            Report(Native.SetStandard(Session, value.Std), "Set standard");
            // Hue is only meaningful for NTSC; the core flips pin_control_info_t.enabled.
            RefreshControls();
        }
    }

    public ObservableCollection<ControlSliderViewModel> PictureSliders { get; } = new();

    public ControlSliderViewModel AudioGain { get; }

    public ObservableCollection<FormatItem> AnalogFormats { get; } = new();

    [ObservableProperty]
    private FormatItem? _analogFormat;

    /// <summary>File base name; also embedded as the title when the format supports one.</summary>
    [ObservableProperty] private string _analogName = "";
    [ObservableProperty] private string _analogOutputDir = "";

    public string AnalogOutputPath => CombinePath(AnalogOutputDir, AnalogName);

    partial void OnAnalogNameChanged(string value) => OnAnalogOutputChanged("gui.name_analog", value);
    partial void OnAnalogOutputDirChanged(string value) => OnAnalogOutputChanged("gui.dir_analog", value);

    private void OnAnalogOutputChanged(string key, string value)
    {
        SaveSetting(key, value);
        OnPropertyChanged(nameof(AnalogOutputPath));
        PushOutputHint();
    }

    private static string CombinePath(string dir, string name) =>
        string.IsNullOrWhiteSpace(name) ? "" : string.IsNullOrWhiteSpace(dir) ? name : System.IO.Path.Combine(dir, name);

    private static void SplitPath(string path, out string dir, out string name)
    {
        dir = System.IO.Path.GetDirectoryName(path) ?? "";
        name = System.IO.Path.GetFileName(path);
    }

    /// <summary>Stop after this long without signal; 0 = never. Analog's own setting
    /// (a lost RCA/S-Video signal isn't detected the same way as a DV/HDV dropout).</summary>
    [ObservableProperty] private double _analogIdleStopMinutes = 5;

    partial void OnAnalogIdleStopMinutesChanged(double value) =>
        SaveSetting("gui.idle_analog", ((int)value).ToString(CultureInfo.InvariantCulture));

    /// <summary>Stop after this long of capture time, signal or not; 0 = never.</summary>
    [ObservableProperty] private double _analogMaxDurationMinutes;

    partial void OnAnalogMaxDurationMinutesChanged(double value) =>
        SaveSetting("gui.duration_analog", ((int)value).ToString(CultureInfo.InvariantCulture));

    partial void OnAnalogFormatChanged(FormatItem? value)
    {
        if (value is not null)
        {
            SaveSetting("gui.format_analog", ((int)value.Format).ToString(CultureInfo.InvariantCulture));
        }
        PushOutputHint();
    }

    /// <summary>Aspect override for analog sources (PinAspect order: Auto, 4:3, 16:9).</summary>
    [ObservableProperty] private int _analogAspectIndex;

    partial void OnAnalogAspectIndexChanged(int value)
    {
        SaveSetting("gui.aspect_analog", value.ToString(CultureInfo.InvariantCulture));
        ApplyPreviewAspect();
    }

    // ================================================================== DV / HDV

    /// <summary>File base name for DV and HDV; also embedded as the title when the format supports one.</summary>
    [ObservableProperty] private string _dvName = "";
    [ObservableProperty] private string _dvOutputDir = "";

    public string DvOutputPath => CombinePath(DvOutputDir, DvName);

    partial void OnDvNameChanged(string value) => OnDvOutputChanged("gui.name_dv", value);
    partial void OnDvOutputDirChanged(string value) => OnDvOutputChanged("gui.dir_dv", value);

    private void OnDvOutputChanged(string key, string value)
    {
        SaveSetting(key, value);
        OnPropertyChanged(nameof(DvOutputPath));
        PushOutputHint();
    }

    public ObservableCollection<KindSettingsViewModel> KindTabs { get; } = new();
    public KindSettingsViewModel DvSettings { get; }
    public KindSettingsViewModel HdvSettings { get; }

    /// <summary>DV / HDV file-options tab. Follows the detected stream; remembered for the next start.</summary>
    [ObservableProperty] private int _selectedKindTabIndex;

    partial void OnSelectedKindTabIndexChanged(int value) =>
        SaveSetting("gui.last_kind", value.ToString(CultureInfo.InvariantCulture));

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(DeckRewEnabled), nameof(DeckPlayEnabled), nameof(DeckStopEnabled), nameof(DeckFfEnabled))]
    private PinDeckState _deckState = PinDeckState.Unknown;
    [ObservableProperty] private bool _isRewChecked;
    [ObservableProperty] private bool _isPlayChecked;
    [ObservableProperty] private bool _isStopChecked;
    [ObservableProperty] private bool _isFfChecked;
    [ObservableProperty] private bool _deckBusy;

    /// <summary>
    /// Re-entrancy guard: true while a status-driven deck update is being
    /// applied. The view's ToggleButton handlers check it (via
    /// <see cref="UserRequestedDeck"/>) so a checked-state change coming
    /// *from* the device never gets echoed back *to* the device as a command.
    /// </summary>
    private bool _applyingDeckStatus;

    private PinKind _lastStreamKind = PinKind.Dv;

    // ================================================================== preview strip

    [ObservableProperty] private bool _isMuted = true;
    [ObservableProperty] private bool _previewHasVideo;

    [ObservableProperty] private string _noVideoText = "No device open";

    /// <summary>
    /// No-signal timeout running during a capture: shown under "No camera or deck signal" as text and a
    /// bar that shrinks to 0, and appended to the stop button's text. The seconds come from the core.
    /// </summary>
    [ObservableProperty] private bool _noSignalCountdownVisible;
    [ObservableProperty] private string _noSignalCountdownText = "";
    [ObservableProperty] private double _noSignalCountdownFraction;   // 1 = full timeout left, 0 = stopping
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(CaptureButtonText), nameof(ManualCaptureTitle), nameof(PrimaryDvTitle))]
    private string _stopCountdownSuffix = "";
    private double _activeIdleStopS;   // the timeout of the running capture, for the bar
    private double _activeDurationS;   // its per-pass time limit, for the taskbar

    /// <summary>Taskbar progress as decided by the core from the last status (see Native.StatusProgress).</summary>
    public PinProgressMode TaskbarMode { get; private set; }
    public double TaskbarFraction { get; private set; }

    /// <summary>Progress bar in the preview pane while the device is being prepared.</summary>
    [ObservableProperty] private bool _progressVisible;
    [ObservableProperty] private bool _progressIndeterminate = true;
    [ObservableProperty] private double _progressValue;

    private readonly IAudioMonitorService _audio = new WasapiAudioMonitorService();

    public string MuteGlyph => IsMuted ? "\uE74F" : "\uE767";     // Mute / Volume
    public string MuteActionName => IsMuted ? "Unmute" : "Mute";  // what a click does

    partial void OnIsMutedChanged(bool value)
    {
        OnPropertyChanged(nameof(MuteGlyph));
        OnPropertyChanged(nameof(MuteActionName));
        SaveGlobalSetting("gui.muted", value ? "1" : "0");
        _audio.IsMuted = value;
        if (Session is { IsInvalid: false })
        {
            // The core only keeps the PCM monitor ring filled while monitoring
            // is wanted; no audio stream is held open while muted.
            Native.MonitorEnable(Session, !value);
            if (value) _audio.Stop(); else _audio.Start();
        }
    }

    /// <summary>The aspect override that applies to what is arriving right now: the analog one, or
    /// the DV / HDV tab's, depending on the input and the detected stream. Drives the preview.</summary>
    public PinAspect ActiveAspect => !IsDvInput
        ? (PinAspect)AnalogAspectIndex
        : (PinAspect)(_lastStreamKind == PinKind.Hdv ? HdvSettings : DvSettings).AspectIndex;

    private PinAspect? _appliedAspect;

    private void ApplyPreviewAspect()
    {
        if (Session is not { IsInvalid: false })
        {
            _appliedAspect = null;
            return;
        }
        var a = ActiveAspect;
        if (_appliedAspect != a)
        {
            Native.SetAspect(Session, a);
            _appliedAspect = a;
        }
    }

    /// <summary>
    /// Tell the core where the next capture would go, so the status snapshot
    /// can report free space and time left before capturing starts.
    /// </summary>
    public void PushOutputHint()
    {
        if (_loading || Session is not { IsInvalid: false } || IsCapturing)
        {
            return;
        }
        var o = BuildCaptureOpts(startDeck: false);
        Native.SetOutputHint(Session, in o);
    }

    // ================================================================== status bar

    [ObservableProperty] private string _statusLine = "Idle";

    /// <summary>Status bar, row 1 of the file item: the file being written while capturing, else the
    /// state ("Ready", "Preparing: ...") or the error. The full pin_format_status_line text is only the
    /// automation name (live region).</summary>
    [ObservableProperty] private string _statusShortText = "No device open";

    /// <summary>Status bar, row 2 of the file item: "Capturing  ·  pass 2/3" while a capture runs, else "".</summary>
    [ObservableProperty] private string _statusSubText = "";

    private static bool IsCapturingState(PinState s) => s is PinState.Capturing or PinState.Stopping or PinState.Rewinding;
    [ObservableProperty] private string _timecode = "--:--:--:--";
    /// <summary>Status bar time: the tape timecode for DV/HDV, the time since capture start for analog.</summary>
    [ObservableProperty] private string _statusTimeText = "--:--:--:--";
    [ObservableProperty] private string _statusTimeTip = "Tape timecode";
    [ObservableProperty] private string _deckStateText = "Deck: —";
    [ObservableProperty] private bool _signalLocked;
    [ObservableProperty] private string _signalLockText = "No signal";
    [ObservableProperty] private string _signalTypeText = "";
    // Frame counters, all from the core: total = since capture start (since app start while idle), clip = current file.
    [ObservableProperty] private string _framesTotalText = "Frames 0 · 0 err · 0 drop";
    [ObservableProperty] private string _statusTip = "";
    [ObservableProperty] private string _storageTip = "";
    [ObservableProperty] private string _storageFreeText = "";
    [ObservableProperty] private string _framesTip = "";
    [ObservableProperty] private string _sizeText = "0 B / 0 B";
    [ObservableProperty] private double _audioPeakLeft = MeterFloorDb;
    [ObservableProperty] private double _audioPeakRight = MeterFloorDb;
    [ObservableProperty] private string _audioPeakText = "L — R —";
    [ObservableProperty] private double _audioHoldLeft = MeterFloorDb;
    [ObservableProperty] private double _audioHoldRight = MeterFloorDb;
    private readonly PeakHold _holdLeft = new(MeterFloorDb), _holdRight = new(MeterFloorDb);
    [ObservableProperty] private string _windowTitle = "Pinnacle Capture";
    [ObservableProperty] private int _tapePercent = -1;
    [ObservableProperty] private string _lastLogLine = "";

    public const double MeterFloorDb = -60;

    [ObservableProperty] private bool _diskLow;
    [ObservableProperty] private bool _hasDiskInfo;
    [ObservableProperty] private string _diskFreeText = "";
    [ObservableProperty] private string _timeLeftText = "";

    private static string FormatTimeLeft(double seconds)
    {
        var t = TimeSpan.FromSeconds(seconds);
        if (t.TotalHours >= 1)
        {
            return $"{(int)t.TotalHours} h {t.Minutes:00} min left";
        }
        return $"{Math.Max(0, (int)t.TotalMinutes)} min left";
    }

    // Transient InfoBar
    [ObservableProperty] private bool _infoOpen;
    [ObservableProperty] private string _infoTitle = "";
    [ObservableProperty] private string _infoMessage = "";
    [ObservableProperty] private int _infoSeverity; // InfoBarSeverity: 0 info, 1 success, 2 warning, 3 error

    /// <summary>Raised for every drained engine event, after the VM has applied it.</summary>
    public event EventHandler<EngineEventArgs>? EngineEvent;

    private bool _loading;

    public MainViewModel()
    {
        DvSettings = new KindSettingsViewModel(PinKind.Dv, "DV");
        HdvSettings = new KindSettingsViewModel(PinKind.Hdv, "HDV");
        KindTabs.Add(DvSettings);
        KindTabs.Add(HdvSettings);

        // Set the backing field, not the property: OnAnalogFormatChanged would save the
        // default over gui.format_analog before LoadSettings gets to read it.
        foreach (var f in Native.Formats(PinKind.Analog))
        {
            var item = new FormatItem(f);
            AnalogFormats.Add(item);
            if (item.IsDefault || _analogFormat is null)
            {
                _analogFormat = item;
            }
        }

        foreach (var c in new[] { PinControl.Brightness, PinControl.Contrast, PinControl.Saturation,
                                  PinControl.Hue, PinControl.Sharpness })
        {
            PictureSliders.Add(new ControlSliderViewModel(c, OnSliderChanged));
        }
        AudioGain = new ControlSliderViewModel(PinControl.AudioGain, OnSliderChanged) { IsDb = true };

        Standards.Add(new StdItem(PinStd.Auto, "Auto"));
        for (int i = 1; i < (int)PinStd.Count; i++)
        {
            var std = (PinStd)i;
            Standards.Add(new StdItem(std, Native.StdName(std)));
        }
        _selectedStandard = Standards[0];

        foreach (var tab in KindTabs)
        {
            tab.PropertyChanged += (s, e) =>
            {
                SaveKindSettings((KindSettingsViewModel)s!);
                if (e.PropertyName == nameof(KindSettingsViewModel.AspectIndex))
                {
                    ApplyPreviewAspect();
                }
                if (e.PropertyName is nameof(KindSettingsViewModel.SelectedFormat) or nameof(KindSettingsViewModel.AspectIndex))
                {
                    PushOutputHint();
                }
            };
        }
    }

    private void OnSliderChanged(PinControl c, int value)
    {
        if (Session is { IsInvalid: false })
        {
            Native.SetControl(Session, c, value);
        }
        SaveSetting($"gui.control_{(int)c}", value.ToString(CultureInfo.InvariantCulture));
    }

    /// <summary>Re-applies the last-used proc-amp / gain values to a freshly opened device.</summary>
    private void ApplySavedControls()
    {
        if (Session is not { IsInvalid: false })
        {
            return;
        }
        foreach (var slider in PictureSliders.Append(AudioGain))
        {
            var saved = LoadSetting($"gui.control_{(int)slider.Control}");
            if (int.TryParse(saved, NumberStyles.Integer, CultureInfo.InvariantCulture, out var v))
            {
                Native.SetControl(Session, slider.Control, v);
            }
        }
    }

    // ================================================================== enumeration / open

    public void RefreshDevices()
    {
        var infos = Native.Enumerate();
        var keepId = SelectedDevice?.Id;

        // Rebuild in place (keeps the ComboBox's selection stable when nothing changed).
        var fresh = infos.Select(i => new DeviceItemViewModel(i, IsCapturing && i.Id == _openedDeviceId)).ToList();
        bool same = fresh.Count == Devices.Count &&
                    fresh.Zip(Devices).All(p => p.First.Id == p.Second.Id && p.First.State == p.Second.State &&
                                                p.First.IsCapturingHere == p.Second.IsCapturingHere);
        if (!same)
        {
            Devices.Clear();
            foreach (var d in fresh)
            {
                Devices.Add(d);
            }
        }
        NoDevices = Devices.Count == 0;
        if (Session is null)
        {
            NoVideoText = StatusShortText = NoDevices ? "No device connected" : "No device open";
        }

        if (keepId is not null)
        {
            var match = Devices.FirstOrDefault(d => d.Id == keepId);
            if (!ReferenceEquals(match, SelectedDevice))
            {
                SelectedDevice = match;
            }
        }
    }

    /// <summary>Picks the device to use at startup: launch preset, else last-used, else first usable.</summary>
    public DeviceItemViewModel? PickInitialDevice(string? preferredId)
    {
        DeviceItemViewModel? pick = null;
        if (!string.IsNullOrEmpty(preferredId) && preferredId != "first")
        {
            // --device: an id, a serial, or a replay file (the core lists that as "replay:<name>")
            pick = Devices.FirstOrDefault(d => d.IsUsable && (d.Id == preferredId ||
                       string.Equals(d.Serial, preferredId, StringComparison.OrdinalIgnoreCase)));
        }
        if (pick is null && preferredId != "first")
        {
            var last = LoadGlobalSetting("gui.last_device");
            pick = Devices.FirstOrDefault(d => d.IsUsable && last.Length > 0 &&
                       (d.Id == last || string.Equals(d.Serial, last, StringComparison.OrdinalIgnoreCase)));
        }
        pick ??= Devices.FirstOrDefault(d => d.IsUsable); // in-use devices can't be picked at all
        return pick;
    }

    /// <summary>Opens <see cref="SelectedDevice"/>, closing whatever was open before.</summary>
    public bool OpenSelectedDevice()
    {
        CloseSession();
        var dev = SelectedDevice;
        if (dev is null)
        {
            NoVideoText = NoDevices ? "No devices found" : "No device selected";
            return false;
        }
        if (!dev.IsUsable)
        {
            ShowInfo(dev.Name, dev.State switch
            {
                PinDevState.InUse => $"This device is in use by another program (process {dev.OwnerPid}).",
                PinDevState.Preparing => "Another program is preparing this device.",
                PinDevState.NoDriver => "This device needs the WinUSB driver. Install it with Zadig, then reconnect.",
                PinDevState.Unsupported => "This model is recognised but not supported yet.",
                _ => "This device can't be opened right now.",
            }, 2);
            NoVideoText = dev.StatusText;
            return false;
        }

        var status = Native.Open(dev.Id, out var session);
        if (status != PinStatus.Ok || session is null)
        {
            ShowInfo("Could not open the device", $"{dev.Name}: {Native.StrError(status)}", 3);
            NoVideoText = "Could not open the device";
            return false;
        }

        Session = session;
        _openedDeviceId = dev.Id;
        SaveGlobalSetting("gui.last_device", dev.Serial.Length > 0 ? dev.Serial : dev.Id);

        _appliedAspect = null;
        ApplyPreviewAspect();
        Native.MonitorEnable(Session, !IsMuted);
        var s = Session;
        _audio.SetSource((buf, max) => Native.MonitorRead(s, buf, max), on => Native.MonitorEnable(s, on));
        _audio.IsMuted = IsMuted;
        if (!IsMuted)
        {
            _audio.Start();
        }
        Report(Native.SetInput(Session, SelectedInput), "Prepare device");
        if (SelectedStandard is not null)
        {
            Native.SetStandard(Session, SelectedStandard.Std);
        }
        ApplySavedControls();
        RefreshControls();
        NoVideoText = "Preparing device…";
        PushOutputHint();
        return true;
    }

    public void RefreshControls()
    {
        if (Session is not { IsInvalid: false })
        {
            return;
        }
        foreach (var slider in PictureSliders.Append(AudioGain))
        {
            if (Native.GetControl(Session, slider.Control, out var info) == PinStatus.Ok)
            {
                slider.LoadFrom(in info);
            }
        }
    }

    public void CloseSession()
    {
        if (Session is null)
        {
            return;
        }
        _openedDeviceId = null;
        _audio.Stop();
        _audio.SetSource(null, null); // never call into a closed session
        Session.Dispose(); // pin_close: blocks until any capture is finalised
        Session = null;
        SessionState = PinState.Closed;
        SessionStateText = "No device open";
        StatusShortText = "No device open";
        IsCapturing = false;
        PreviewHasVideo = false;
    }

    // ================================================================== deck

    /// <summary>Called by the view when the user clicks a deck ToggleButton.</summary>
    public void UserRequestedDeck(PinDeckCmd cmd)
    {
        if (_applyingDeckStatus || Session is not { IsInvalid: false })
        {
            return;
        }
        if (IsCapturing)
        {
            return; // the deck buttons are disabled during capture
        }
        Report(Native.Deck(Session, cmd), "Deck command");
    }

    private void ApplyDeckStatus(PinDeckState state, bool busy)
    {
        DeckBusy = busy;
        _applyingDeckStatus = true;
        try
        {
            DeckState = state;
            IsRewChecked = state == PinDeckState.Rewinding;
            IsPlayChecked = state is PinDeckState.Playing or PinDeckState.Recording;
            IsStopChecked = state == PinDeckState.Stopped;
            IsFfChecked = state == PinDeckState.FastForward;
            DeckStateText = state switch
            {
                PinDeckState.Stopped => "Stopped",
                PinDeckState.Playing => "Playing",
                PinDeckState.Paused => "Paused",
                PinDeckState.FastForward => "Fast forward",
                PinDeckState.Rewinding => "Rewinding",
                PinDeckState.Recording => "Camera recording",
                PinDeckState.NoTape => "No tape",
                _ => "—",
            } + (busy ? " …" : "");
        }
        finally
        {
            _applyingDeckStatus = false;
        }
    }

    public bool IsApplyingDeckStatus => _applyingDeckStatus;

    // ================================================================== capture

    /// <summary>Capture options from the current UI state; <paramref name="startDeck"/> = "Play and capture".</summary>
    public PinCaptureOpts BuildCaptureOpts(bool startDeck, bool rewindFirst = false)
    {
        var o = Native.CaptureOptsDefaults();
        bool analog = !IsDvInput;
        o.Path = analog ? AnalogOutputPath : DvOutputPath;
        o.FirstNumber = NextFileNumber(o.Path);

        if (AnalogFormat is not null) o.FormatAnalog = AnalogFormat.Format;
        if (DvSettings.SelectedFormat is not null) o.FormatDv = DvSettings.SelectedFormat.Format;
        if (HdvSettings.SelectedFormat is not null) o.FormatHdv = HdvSettings.SelectedFormat.Format;

        // The DV input auto-detects DV vs HDV; the per-kind options that
        // are scalar in pin_capture_opts_t come from the tab matching what is
        // actually arriving right now.
        var kind = _lastStreamKind == PinKind.Hdv ? HdvSettings : DvSettings;
        o.Title = analog ? (AnalogFormat?.SupportsTitle == true ? AnalogName : "") : (kind.TitleEnabled ? DvName : "");
        o.Aspect = ActiveAspect;
        o.SceneSplit = !analog && kind.SplitIntoScenes && kind.SplitEnabled ? 1 : 0;
        o.IdleStopMinutes = (int)Math.Max(0, analog ? AnalogIdleStopMinutes : kind.IdleStopMinutes);
        o.MaxDurationMinutes = (int)Math.Max(0, analog ? AnalogMaxDurationMinutes : kind.MaxDurationMinutes);
        o.Passes = analog ? 1 : kind.EffectivePasses;
        o.StartDeck = startDeck ? 1 : 0;
        o.RewindFirst = rewindFirst ? 1 : 0; // "Play and capture": start of tape, then play
        return o;
    }

    /// <summary>
    /// Every capture file is "name-NNNN.ext" (scene splitting or not). Returns the
    /// number after the highest one already used for this name, in any format.
    /// </summary>
    private uint NextFileNumber(string path)
    {
        try
        {
            var dir = System.IO.Path.GetDirectoryName(path);
            var name = System.IO.Path.GetFileName(path);
            if (string.IsNullOrEmpty(name))
            {
                return 1;
            }
            var ext = System.IO.Path.GetExtension(name).TrimStart('.');
            var known = AnalogFormats.Concat(DvSettings.Formats).Concat(HdvSettings.Formats);
            if (ext.Length > 0 && known.Any(f => string.Equals(f.Extension, ext, StringComparison.OrdinalIgnoreCase)))
            {
                name = System.IO.Path.GetFileNameWithoutExtension(name);
            }
            if (string.IsNullOrEmpty(dir))
            {
                dir = ".";
            }
            if (!System.IO.Directory.Exists(dir))
            {
                return 1;
            }
            var re = new System.Text.RegularExpressions.Regex(
                "^" + System.Text.RegularExpressions.Regex.Escape(name) + @"-(\d+)\.[^.]+$",
                System.Text.RegularExpressions.RegexOptions.IgnoreCase);
            uint max = 0;
            foreach (var f in System.IO.Directory.EnumerateFiles(dir))
            {
                var m = re.Match(System.IO.Path.GetFileName(f));
                if (m.Success && uint.TryParse(m.Groups[1].Value, out var n) && n > max)
                {
                    max = n;
                }
            }
            return max + 1;
        }
        catch (Exception)
        {
            return 1;
        }
    }

    public PinStatus CheckOutput(in PinCaptureOpts o, out PinOutputCheck check)
    {
        check = PinOutputCheck.Create();
        return Session is { IsInvalid: false } ? Native.CheckOutput(Session, in o, out check) : PinStatus.ErrState;
    }

    public PinStatus StartCapture(in PinCaptureOpts o, bool overwrite)
    {
        if (Session is not { IsInvalid: false })
        {
            return PinStatus.ErrState;
        }
        var st = Native.CaptureStart(Session, in o, overwrite);
        if (st == PinStatus.Ok)
        {
            _captureWithDeck = o.StartDeck != 0;
            _activeIdleStopS = Math.Max(0, o.IdleStopMinutes) * 60.0;
            _activeDurationS = Math.Max(0, o.MaxDurationMinutes) * 60.0;
            if (InfoSeverity == 3)
            {
                InfoOpen = false; // a stale error must not colour the taskbar of a running capture
            }
            OnPropertyChanged(nameof(PlayAndCaptureEnabled));
            OnPropertyChanged(nameof(DvAutoCaptureEnabled));
        }
        Report(st, "Start capture");
        return st;
    }

    public void StopCapture()
    {
        if (Session is { IsInvalid: false })
        {
            Report(Native.CaptureStop(Session), "Stop capture");
        }
    }

    /// <summary>Taskbar "Start capture": the main start action of the current mode (analog Capture, DV/HDV Manual capture), same enable rule as that button.</summary>
    public bool TaskbarStartEnabled => !IsCapturing && (IsDvInput ? PlayAndCaptureEnabled : CaptureEnabled);

    /// <summary>Taskbar "Stop capture": whenever the in-window stop buttons can stop.</summary>
    public bool TaskbarStopEnabled => IsCapturing && CanStop;

    /// <summary>Stops the way the capture was started (manual: tape keeps running; automatic: tape stops too).</summary>
    public void StopCaptureAsStarted()
    {
        if (Session is { IsInvalid: false })
        {
            Report(Native.CaptureStopAsStarted(Session), "Stop capture");
        }
    }

    /// <summary>Stops the capture and decides about the tape: true = deck Stop, false = leave it running.</summary>
    public void StopCapture(bool stopDeck)
    {
        if (Session is { IsInvalid: false })
        {
            Report(Native.CaptureStop(Session, stopDeck), "Stop capture");
        }
    }

    // ================================================================== polling

    // The core reports the loudest peak since the previous status read and
    // resets it, so every read (the 100 ms tick and the meter tick) must pass
    // through FeedMeter; the meter tick then publishes the max with a decay.
    private double _meterAccL = double.NegativeInfinity, _meterAccR = double.NegativeInfinity;
    private long _meterLastTick;
    private const double MeterDecayDbPerSecond = 30;

    private void FeedMeter(in PinStatusSnapshot st)
    {
        _meterAccL = Math.Max(_meterAccL, st.AudioPeakDb0);
        _meterAccR = Math.Max(_meterAccR, st.AudioPeakDb1);
    }

    /// <summary>Meter timer (~30 Hz): fresh status read, peak with a short decay.</summary>
    public void UpdateMeters()
    {
        if (Session is { IsInvalid: false } && Native.GetStatus(Session, out var st) == PinStatus.Ok)
        {
            FeedMeter(in st);
        }
        long now = Environment.TickCount64;
        double dt = _meterLastTick == 0 ? 0 : Math.Min(0.25, (now - _meterLastTick) / 1000.0);
        _meterLastTick = now;
        double fall = MeterDecayDbPerSecond * dt;
        double l = Math.Clamp(_meterAccL, MeterFloorDb, 0);
        double r = Math.Clamp(_meterAccR, MeterFloorDb, 0);
        _meterAccL = _meterAccR = double.NegativeInfinity;
        AudioPeakLeft = Math.Max(l, AudioPeakLeft - fall);
        AudioPeakRight = Math.Max(r, AudioPeakRight - fall);
        // Hold tick follows the true (undecayed) peaks.
        AudioHoldLeft = _holdLeft.Push(l);
        AudioHoldRight = _holdRight.Push(r);
    }

    public void Tick()
    {
        // The core's own log lines (process-wide, session or not). Only the
        // console sees these: some warnings are routine retries, not worth a
        // banner. The session's events below go to both.
        while (Native.PollProcessEvent(out var pe))
        {
            ConsoleOutput.WriteEvent(in pe);
        }

        if (Session is not { IsInvalid: false })
        {
            return;
        }

        if (Native.GetStatus(Session, out var st) == PinStatus.Ok)
        {
            ApplyStatus(in st);
        }

        while (Native.PollEvent(Session, out var evt))
        {
            HandleEvent(in evt);
        }
    }

    private const string ReadyBehindHubText = "Ready (connection via USB hub detected, see readme)";

    /// <summary>The open device's list entry (the selection, if the list doesn't have it).</summary>
    private DeviceItemViewModel? OpenDevice =>
        Devices.FirstOrDefault(d => d.Id == _openedDeviceId) ?? SelectedDevice;

    /// <summary>The open device is plugged in through a USB hub (core hub_depth &gt; 0; advisory).</summary>
    private bool OpenDeviceBehindHub => OpenDevice is { IsBehindHub: true };

    private void ApplyStatus(in PinStatusSnapshot st)
    {
        SessionState = st.State;
        SessionStateText = st.State switch
        {
            PinState.Closed => "Closed",
            PinState.Preparing => "Preparing…",
            PinState.Ready => "Ready",
            PinState.Capturing => "Capturing",
            PinState.Stopping => "Finalizing…",
            PinState.Rewinding => $"Rewinding (pass {st.Pass}/{st.Passes})",
            PinState.Error => "Error",
            _ => st.State.ToString(),
        };

        StatusLine = Native.FormatStatusLine(in st);
        var file = string.IsNullOrEmpty(st.CurrentFile) ? "" : System.IO.Path.GetFileName(st.CurrentFile);
        if (st.State == PinState.Error)
        {
            StatusShortText = "Error: " + (string.IsNullOrEmpty(st.ErrorText) ? Native.StrError(st.LastError) : st.ErrorText);
        }
        else
        {
            StatusShortText = st.State == PinState.Preparing && st.Detail.Length > 0
                ? "Preparing: " + st.Detail
                // why the last capture stopped (not after the user's own stop), until the next one
                : st.State == PinState.Ready && st.StopReason is not (PinStopReason.None or PinStopReason.User) &&
                  st.StopText.Length > 0
                ? st.StopText
                : string.IsNullOrEmpty(file) || st.State != PinState.Capturing
                ? (st.State == PinState.Ready && OpenDeviceBehindHub ? ReadyBehindHubText : SessionStateText)
                : st.Passes > 1 ? $"{file}  \u00B7  pass {st.Pass}/{st.Passes}" : file;
        }
        StatusSubText = IsCapturingState(st.State) ? SessionStateText : "";
        StatusTip = StatusSubText.Length > 0 && StatusSubText != StatusShortText
            ? $"{StatusSubText}\n{StatusShortText}" : StatusShortText;
        if (StatusShortText == ReadyBehindHubText && OpenDevice?.HubHint is { } hint)
        {
            StatusTip = hint;
        }

        Timecode = string.IsNullOrEmpty(st.Timecode) ? "--:--:--:--" : st.Timecode;
        if (st.Input == PinInput.Dv)
        {
            StatusTimeText = Timecode;
            StatusTimeTip = "Tape timecode";
        }
        else
        {
            StatusTimeText = TimeSpan.FromSeconds(Math.Max(0, st.ElapsedS)).ToString(@"hh\:mm\:ss", CultureInfo.InvariantCulture);
            StatusTimeTip = "Time since capture start";
        }

        SignalLocked = st.Signal != 0;
        SignalLockText = SignalLocked ? "Locked" : "No signal";
        SignalTypeText = SignalTypeFor(in st);

        var dropped = st.FramesDropped + st.WriteDropped;
        FramesTotalText = $"Frames {st.Frames:N0} · {st.FramesError:N0} err · {dropped:N0} drop";
        FramesTip = $"Total (since {(IsCapturingState(st.State) ? "capture start" : "the app started")})\n"
            + $"Frames: {st.Frames:N0}\nWith errors: {st.FramesError:N0}\nDropped: {dropped:N0}\n\n"
            + $"Current clip\nFrames: {st.ClipFrames:N0}\nWith errors: {st.ClipFramesError:N0}\nDropped: {st.ClipFramesDropped:N0}\n\n"
            + "A frame has an error if the camera damaged or concealed it,\ndata was missing, or (HDV) it depends on a damaged picture.";
        SizeText = $"{HumanSize(st.TotalBytesWritten)} / {HumanSize(st.ClipBytesWritten)}";

        FeedMeter(in st);
        AudioPeakText = $"Audio peak left {FormatDb(st.AudioPeakDb0)}, right {FormatDb(st.AudioPeakDb1)}";

        TapePercent = st.TapePercent;
        if (_lastStreamKind != st.StreamKind)
        {
            _lastStreamKind = st.StreamKind;
            if (st.StreamKind is PinKind.Dv or PinKind.Hdv)
            {
                SelectedKindTabIndex = st.StreamKind == PinKind.Hdv ? 1 : 0;
            }
            ApplyPreviewAspect();
            PushOutputHint();
        }

        DiskLow = st.DiskLow != 0;
        DiskFreeText = st.DiskFreeBytes > 0 ? $"{HumanSize(st.DiskFreeBytes)} free" : "";
        TimeLeftText = st.EstSecondsLeft >= 0 ? FormatTimeLeft(st.EstSecondsLeft) : "";
        StorageFreeText = (DiskFreeText.Length > 0 ? "· " + DiskFreeText : "")
            + (TimeLeftText.Length > 0 ? " · " + TimeLeftText : "");
        StorageTip = $"Written in this capture: {HumanSize(st.TotalBytesWritten)}\nWritten in the current file: {HumanSize(st.ClipBytesWritten)}"
            + (DiskFreeText.Length > 0 ? $"\nFree on the output volume: {HumanSize(st.DiskFreeBytes)}" : "")
            + (TimeLeftText.Length > 0 ? $"\n{TimeLeftText}" : "")
            + (st.DiskLow != 0 ? "\n\nLow disk space: less than 1 hour or 50 GB left" : "");
        HasDiskInfo = st.DiskFreeBytes > 0;

        IsCapturing = st.State is PinState.Capturing or PinState.Stopping or PinState.Rewinding;

        ApplyDeckStatus(st.Deck, st.DeckBusy != 0);

        // One place says why there is no picture: the core's own sentence (bring-up step,
        // "no camera found", "no signal on the composite input", ...).
        NoVideoText = st.State switch
        {
            PinState.Preparing => st.Detail.Length > 0 ? st.Detail : "Preparing device…",
            PinState.Error => string.IsNullOrEmpty(st.ErrorText) ? "Device error" : st.ErrorText,
            _ => st.Detail.Length > 0 ? st.Detail : IsDvInput ? "No camera or deck signal." : "No video signal.",
        };
        bool counting = st.State == PinState.Capturing && st.Signal == 0 && st.IdleStopRemainingS >= 0;
        if (counting)
        {
            var left = Native.FormatRemaining(st.IdleStopRemainingS);
            NoSignalCountdownText = $"Stopping capture in {left}";
            NoSignalCountdownFraction = _activeIdleStopS > 0 ? Math.Clamp(st.IdleStopRemainingS / _activeIdleStopS, 0, 1) : 0;
            StopCountdownSuffix = $" ({left})";
        }
        else
        {
            StopCountdownSuffix = "";
        }
        NoSignalCountdownVisible = counting;
        ProgressVisible = st.State == PinState.Preparing;
        ProgressIndeterminate = st.ProgressPercent < 0;
        ProgressValue = Math.Max(0, st.ProgressPercent);
        TaskbarMode = Native.StatusProgress(in st, _activeIdleStopS, _activeDurationS, out var taskbarFraction);
        TaskbarFraction = taskbarFraction;

        // Deck buttons have nothing to talk to without a camera (analog inputs report -1).
        DeckAvailable = !IsDvInput || st.CameraPresent != 0;

        WindowTitle = Native.FormatWindowTitle(in st, SelectedDevice?.DisplayName ?? DefaultDeviceName);
    }

    private static string FormatDb(float db) => db <= -143 ? "silent" : $"{db:0.0} dBFS";

    /// <summary>"HDV · 1080i25", "DV · PAL", "S-Video · NTSC", ...: what is arriving, from the core's status.</summary>
    private static string SignalTypeFor(in PinStatusSnapshot st)
    {
        string type = st.Input switch
        {
            PinInput.SVideo => "S-Video",
            PinInput.Composite => "Composite",
            _ => st.StreamKind == PinKind.Hdv ? "HDV" : st.StreamKind == PinKind.Dv ? "DV" : "DV/HDV",
        };
        if (st.Signal == 0)
        {
            return type;
        }
        // DV/HDV: the core's own label (PAL, NTSC, 1080i25, 720p59.94); analog adds the
        // variants the decoder detects.
        string std = st.Input == PinInput.Dv
            ? st.VideoLabel
            : st.DetectedStd switch
            {
                PinStd.Pal => "PAL",
                PinStd.Ntsc => "NTSC",
                PinStd.PalM => "PAL-M",
                PinStd.PalN => "PAL-N",
                PinStd.Pal60 => "PAL-60",
                PinStd.Ntsc443 => "NTSC-4.43",
                PinStd.NtscJ => "NTSC-J",
                PinStd.Secam => "SECAM",
                _ => st.VideoLabel,
            };
        return std.Length > 0 ? type + " · " + std : type;
    }

    public static string HumanSize(ulong bytes)
    {
        string[] units = { "B", "KB", "MB", "GB", "TB" };
        double size = bytes;
        int unit = 0;
        while (size >= 1024 && unit < units.Length - 1)
        {
            size /= 1024;
            unit++;
        }
        return unit == 0 ? $"{bytes} B" : $"{size:0.0} {units[unit]}";
    }

    private void HandleEvent(in PinEvent evt)
    {
        ConsoleOutput.WriteEvent(in evt);
        switch (evt.Kind)
        {
            case PinEventKind.Log:
                LastLogLine = evt.Text;
                if (evt.A >= 3)
                {
                    ShowInfo("Error", evt.Text, 3);
                }
                else if (evt.A == 2)
                {
                    ShowInfo("Warning", evt.Text, 2);
                }
                break;
            case PinEventKind.Error:
                ShowInfo(Native.StrError((PinStatus)evt.A), evt.Text, 3);
                break;
            case PinEventKind.FileOpened:
                LastLogLine = $"Writing {evt.Text}";
                break;
            case PinEventKind.FileClosed:
                LastLogLine = (PinStatus)evt.A == PinStatus.Ok
                    ? $"Saved {evt.Text}"
                    : $"Problem finalising {evt.Text}: {Native.StrError((PinStatus)evt.A)}";
                if ((PinStatus)evt.A != PinStatus.Ok)
                {
                    ShowInfo("File not finalised cleanly", LastLogLine, 3);
                }
                break;
            case PinEventKind.Scene:
                LastLogLine = $"Scene {evt.A}";
                break;
            case PinEventKind.Pass:
                LastLogLine = $"Pass {evt.A}";
                break;
            case PinEventKind.Deck:
                ApplyDeckStatus((PinDeckState)evt.A, DeckBusy);
                break;
            case PinEventKind.InputFormat:
                RefreshControls(); // e.g. hue enabled/disabled after a 50/60 Hz change
                break;
            case PinEventKind.Devices:
                RefreshDevices();
                break;
            case PinEventKind.CaptureEnded:
                LastLogLine = evt.Text; // abnormal: MainWindow's dialog; the status bar shows it either way
                break;
            case PinEventKind.State:
                SessionState = (PinState)evt.A;
                RefreshDevices(); // badge: Open / Capturing
                break;
            case PinEventKind.Step:
                LastLogLine = evt.Text;
                break;
            case PinEventKind.Done:
                // A = exit code (docs/cli.md), Text = the reason when it is not 0
                if (evt.A == 0)
                {
                    ShowInfo("Done", "The command-line steps finished.", 1);
                }
                else
                {
                    ShowInfo("Command line stopped", evt.Text.Length > 0 ? evt.Text : $"Exit code {evt.A}", 3);
                }
                break;
        }
        EngineEvent?.Invoke(this, new EngineEventArgs(evt.Kind, evt.A, evt.Text));
    }

    // ================================================================== info bar

    public void ShowInfo(string title, string message, int severity)
    {
        InfoTitle = title;
        InfoMessage = message;
        InfoSeverity = severity;
        InfoOpen = true;
    }

    private void Report(PinStatus st, string what)
    {
        if (st != PinStatus.Ok)
        {
            ShowInfo(what, Native.StrError(st), st == PinStatus.ErrState ? 2 : 3);
        }
    }

    // ================================================================== settings

    // Every option except the app-global ones (window geometry, last device, mute) is stored per
    // device: the core builds the "dev_<GUID>." key prefix (pin_device_settings_key). With no device
    // selected there is no scope: loads give the defaults and nothing is written.
    private string? _settingsScope;

    /// <summary>The selected device's settings prefix, or null when no device is selected.</summary>
    private static string? ScopeFor(DeviceItemViewModel d)
    {
        try
        {
            return Native.DeviceSettingsPrefix(d.Serial, d.Id);
        }
        catch (Exception)
        {
            return null;
        }
    }

    /// <summary>Per-device settings are reloaded whenever another device is selected (not while capturing).</summary>
    partial void OnSelectedDeviceChanged(DeviceItemViewModel? value)
    {
        if (value is null || IsCapturing)
        {
            return;
        }
        var scope = ScopeFor(value);
        if (scope != _settingsScope)
        {
            _settingsScope = scope;
            LoadSettings();
        }
    }

    private string LoadSetting(string key, string fallback = "") =>
        _settingsScope is null ? fallback : LoadGlobalSetting(_settingsScope + key, fallback);

    private static string LoadGlobalSetting(string key, string fallback = "")
    {
        try
        {
            return Native.SettingsGet(key, fallback);
        }
        catch (Exception)
        {
            return fallback;
        }
    }

    private void SaveSetting(string key, string value)
    {
        if (_settingsScope is not null)
        {
            SaveGlobalSetting(_settingsScope + key, value);
        }
    }

    private void SaveGlobalSetting(string key, string value)
    {
        if (_loading)
        {
            return;
        }
        try
        {
            Native.SettingsSet(key, value);
        }
        catch (Exception)
        {
            // settings are best effort
        }
    }

    private void SaveKindSettings(KindSettingsViewModel k)
    {
        var p = k.SettingsPrefix;
        if (k.SelectedFormat is not null)
        {
            SaveSetting($"gui.format_{p}", ((int)k.SelectedFormat.Format).ToString(CultureInfo.InvariantCulture));
        }
        SaveSetting($"gui.split_{p}", k.SplitIntoScenes ? "1" : "0");
        SaveSetting($"gui.passes_{p}", ((int)k.Passes).ToString(CultureInfo.InvariantCulture));
        SaveSetting($"gui.idle_{p}", ((int)k.IdleStopMinutes).ToString(CultureInfo.InvariantCulture));
        SaveSetting($"gui.duration_{p}", ((int)k.MaxDurationMinutes).ToString(CultureInfo.InvariantCulture));
        SaveSetting($"gui.aspect_{p}", k.AspectIndex.ToString(CultureInfo.InvariantCulture));
    }

    private int LoadInt(string key, int fallback) =>
        int.TryParse(LoadSetting(key), NumberStyles.Integer, CultureInfo.InvariantCulture, out var v) ? v : fallback;

    /// <summary>App-global (not per-device) settings, read once at startup.</summary>
    public void LoadGlobalSettings()
    {
        _loading = true;
        try
        {
            // Muted unless the user unmuted last time: a live monitor into
            // speakers next to the source's microphone is a feedback loop.
            IsMuted = LoadGlobalSetting("gui.muted", "1") != "0";
        }
        finally
        {
            _loading = false;
        }
    }

    /// <summary>Loads the selected device's settings (defaults for a device never seen before).</summary>
    private void LoadSettings()
    {
        _loading = true;
        try
        {
            InputIndex = Math.Clamp(LoadInt("gui.last_input", 0), 0, 2);
            AnalogAspectIndex = Math.Clamp(LoadInt("gui.aspect_analog", 0), 0, 2);
            LoadOutput("analog", "analog", out var analogDir, out var analogName);
            AnalogOutputDir = analogDir;
            AnalogName = analogName;
            AnalogIdleStopMinutes = Math.Max(0, LoadInt("gui.idle_analog", 5));
            AnalogMaxDurationMinutes = Math.Max(0, LoadInt("gui.duration_analog", 0));
            SelectedKindTabIndex = Math.Clamp(LoadInt("gui.last_kind", 0), 0, 1);
            _lastStreamKind = SelectedKindTabIndex == 1 ? PinKind.Hdv : PinKind.Dv;
            int std = LoadInt("gui.std", (int)PinStd.Auto);
            SelectedStandard = Standards.FirstOrDefault(x => (int)x.Std == std) ?? Standards[0];
            LoadOutput("dv", "tape", out var dvDir, out var dvName);
            DvOutputDir = dvDir;
            DvName = dvName;

            int fa = LoadInt("gui.format_analog", -1);
            var af = AnalogFormats.FirstOrDefault(f => (int)f.Format == fa);
            if (af is not null)
            {
                AnalogFormat = af;
            }

            foreach (var k in KindTabs)
            {
                var p = k.SettingsPrefix;
                int f = LoadInt($"gui.format_{p}", -1);
                if (f >= 0)
                {
                    k.ApplyFormat((PinFormat)f);
                }
                k.SplitIntoScenes = LoadInt($"gui.split_{p}", 0) != 0;
                k.Passes = Math.Max(1, LoadInt($"gui.passes_{p}", 1));
                k.IdleStopMinutes = Math.Max(0, LoadInt($"gui.idle_{p}", 5));
                k.MaxDurationMinutes = Math.Max(0, LoadInt($"gui.duration_{p}", 0));
                k.AspectIndex = Math.Clamp(LoadInt($"gui.aspect_{p}", 0), 0, 2);
            }
        }
        finally
        {
            _loading = false;
        }
    }

    /// <summary>Loads the directory and name for this device, falling back to the defaults.</summary>
    private void LoadOutput(string key, string defaultName, out string dir, out string name)
    {
        dir = LoadSetting($"gui.dir_{key}");
        name = LoadSetting($"gui.name_{key}");
        if (dir.Length == 0)
        {
            dir = Environment.GetFolderPath(Environment.SpecialFolder.MyVideos);
        }
        if (name.Length == 0)
        {
            name = defaultName;
        }
    }

    // ================================================================== command line

    /// <summary>
    /// Applies the settings a command line gave before its first action (pin_script_settings) over
    /// the loaded settings: input, standard, formats, aspect, split and the analog controls (which
    /// are applied when the device opens, like the saved ones). The title and --keep-raw have no
    /// field in this window; the script's own captures use them.
    /// </summary>
    public void ApplyScriptSettings(in PinScriptSettings l)
    {
        if (l.HasInput != 0)
        {
            InputIndex = (int)l.Input;
        }
        if (l.HasStd != 0)
        {
            var std = l.Std;
            SelectedStandard = Standards.FirstOrDefault(s => s.Std == std) ?? SelectedStandard;
        }
        bool analog = !IsDvInput;
        if (l.HasFormatAnalog != 0)
        {
            var wanted = l.FormatAnalog;
            var af = AnalogFormats.FirstOrDefault(f => f.Format == wanted);
            if (af is not null) AnalogFormat = af;
        }
        if (l.HasFormatDv != 0)
        {
            DvSettings.ApplyFormat(l.FormatDv);
        }
        if (l.HasFormatHdv != 0)
        {
            HdvSettings.ApplyFormat(l.FormatHdv);
        }
        if (l.HasSplit != 0)
        {
            DvSettings.SplitIntoScenes = HdvSettings.SplitIntoScenes = l.Split != 0;
        }
        if (l.HasAspect != 0)
        {
            // --aspect applies to the kinds the chosen input can deliver.
            if (analog) AnalogAspectIndex = (int)l.Aspect;
            else DvSettings.AspectIndex = HdvSettings.AspectIndex = (int)l.Aspect;
        }
        foreach (var slider in PictureSliders.Append(AudioGain))
        {
            if (l.Control(slider.Control) is int v)
            {
                SaveSetting($"gui.control_{(int)slider.Control}", v.ToString(CultureInfo.InvariantCulture));
            }
        }
    }

    public PinStatus RunScript(PinScriptHandle script)
    {
        if (Session is not { IsInvalid: false })
        {
            return PinStatus.ErrState;
        }
        var st = Native.ScriptRun(Session, script);
        Report(st, "Command line");
        return st;
    }

    public void Dispose()
    {
        CloseSession();
        _audio.Dispose();
    }
}

public sealed partial class EngineEventArgs : EventArgs
{
    public PinEventKind Kind { get; }
    public int A { get; }
    public string Text { get; }

    public EngineEventArgs(PinEventKind kind, int a, string text)
    {
        Kind = kind;
        A = a;
        Text = text;
    }
}

/// <summary>Highest value of the last 10 s, for the meters' peak-hold tick.</summary>
public sealed class PeakHold
{
    private static readonly long WindowMs = 10_000;
    private readonly double _floor;
    private readonly System.Collections.Generic.Queue<(long T, double V)> _samples = new();

    public PeakHold(double floor) => _floor = floor;

    public double Push(double value)
    {
        long now = Environment.TickCount64;
        _samples.Enqueue((now, value));
        while (_samples.Count > 0 && now - _samples.Peek().T > WindowMs)
        {
            _samples.Dequeue();
        }
        double max = _floor;
        foreach (var (_, v) in _samples)
        {
            max = Math.Max(max, v);
        }
        return max;
    }
}
