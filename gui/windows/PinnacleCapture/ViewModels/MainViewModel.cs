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
    [NotifyPropertyChangedFor(nameof(HasSelectedDevice))]
    private DeviceItemViewModel? _selectedDevice;

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
                              nameof(DeckTransportEnabled), nameof(DeckStopEnabled), nameof(DvAutoCaptureEnabled))]
    private bool _isCapturing;

    public bool IsIdle => !IsCapturing;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(PlayAndCaptureEnabled), nameof(CaptureEnabled), nameof(CanStop),
                              nameof(DeckTransportEnabled), nameof(DeckStopEnabled), nameof(DvAutoCaptureEnabled))]
    private PinState _sessionState = PinState.Closed;

    /// <summary>Stop is possible while writing (or between passes); not while already finalising.</summary>
    public bool CanStop => SessionState is PinState.Capturing or PinState.Rewinding;

    /// <summary>The Capture/Stop button (analog) and the Play-and-capture/Stop button (DV).</summary>
    public bool CaptureEnabled => IsCapturing ? CanStop : SessionState == PinState.Ready;

    /// <summary>Only for "capture without starting the tape", which is hidden while capturing.</summary>
    public bool PlayAndCaptureEnabled => !IsCapturing && SessionState == PinState.Ready;

    /// <summary>False only when the core knows for sure that no camera is on the FireWire bus.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(DeckTransportEnabled), nameof(DeckStopEnabled), nameof(DvAutoCaptureEnabled))]
    private bool _deckAvailable = true;

    /// <summary>REW / PLAY / FF: only while idle and READY (same rule as the capture buttons), and with a camera to talk to.</summary>
    public bool DeckTransportEnabled => PlayAndCaptureEnabled && DeckAvailable;

    /// <summary>STOP: while capturing (it ends the capture) or whenever the transport is usable.</summary>
    public bool DeckStopEnabled => IsCapturing ? CanStop : PlayAndCaptureEnabled && DeckAvailable;

    /// <summary>"Automatic rewind &amp; capture" drives the deck, so it needs a camera; Stop capture always works.</summary>
    public bool DvAutoCaptureEnabled => IsCapturing ? CanStop : SessionState == PinState.Ready && DeckAvailable;

    public string PrimaryDvTitle => IsCapturing ? "Stop capture" : "Automatic rewind & capture";
    public string PrimaryDvHelp => IsCapturing ? "Finishes the file, then stops the tape" : "Rewinds to the start of the tape, plays and records it";
    public string PrimaryDvGlyph => IsCapturing ? "\uE71A" : "\uE896"; // Stop / Download

    public string CaptureButtonText => IsCapturing ? "Stop capture" : "Capture";

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
        if (Session is { IsInvalid: false })
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
    [NotifyPropertyChangedFor(nameof(AnalogTitleEnabled))]
    private FormatItem? _analogFormat;

    [ObservableProperty] private string _analogTitle = "";

    partial void OnAnalogTitleChanged(string value) => SaveSetting("gui.title_analog", value);
    [ObservableProperty] private string _analogOutputPath = "";

    /// <summary>Stop after this long without signal; 0 = never. Analog's own setting
    /// (a lost RCA/S-Video signal isn't detected the same way as a DV/HDV dropout).</summary>
    [ObservableProperty] private double _analogIdleStopMinutes = 5;

    partial void OnAnalogIdleStopMinutesChanged(double value) =>
        SaveSetting("gui.idle_analog", ((int)value).ToString(CultureInfo.InvariantCulture));

    /// <summary>Stop after this long of capture time, signal or not; 0 = never.</summary>
    [ObservableProperty] private double _analogMaxDurationMinutes;

    partial void OnAnalogMaxDurationMinutesChanged(double value) =>
        SaveSetting("gui.duration_analog", ((int)value).ToString(CultureInfo.InvariantCulture));

    public bool AnalogTitleEnabled => AnalogFormat?.SupportsTitle ?? false;

    partial void OnAnalogFormatChanged(FormatItem? value)
    {
        if (value is not null)
        {
            SaveSetting("gui.format_analog", ((int)value.Format).ToString(CultureInfo.InvariantCulture));
        }
        PushOutputHint();
    }

    partial void OnAnalogOutputPathChanged(string value)
    {
        SaveSetting("gui.output_analog", value);
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

    [ObservableProperty] private string _dvOutputPath = "";

    partial void OnDvOutputPathChanged(string value)
    {
        SaveSetting("gui.output_dv", value);
        PushOutputHint();
    }

    public ObservableCollection<KindSettingsViewModel> KindTabs { get; } = new();
    public KindSettingsViewModel DvSettings { get; }
    public KindSettingsViewModel HdvSettings { get; }

    /// <summary>DV / HDV file-options tab. Follows the detected stream; remembered for the next start.</summary>
    [ObservableProperty] private int _selectedKindTabIndex;

    partial void OnSelectedKindTabIndexChanged(int value) =>
        SaveSetting("gui.last_kind", value.ToString(CultureInfo.InvariantCulture));

    [ObservableProperty] private PinDeckState _deckState = PinDeckState.Unknown;
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
        SaveSetting("gui.muted", value ? "1" : "0");
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

    /// <summary>Visible left side of the status bar: state word + current file, or the error.
    /// The full pin_format_status_line text is only the automation name (live region).</summary>
    [ObservableProperty] private string _statusShortText = "No device open";

    private static bool IsCapturingState(PinState s) => s is PinState.Capturing or PinState.Stopping or PinState.Rewinding;
    [ObservableProperty] private string _timecode = "--:--:--:--";
    [ObservableProperty] private string _deckStateText = "Deck: —";
    [ObservableProperty] private bool _signalLocked;
    [ObservableProperty] private string _signalLockText = "No signal";
    [ObservableProperty] private string _framesText = "0 frames";
    [ObservableProperty] private string _droppedText = "0 dropped";
    [ObservableProperty] private string _fileSizeText = "0 B";
    [ObservableProperty] private string _elapsedText = "00:00:00";
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
            var last = LoadSetting("gui.last_device");
            pick = Devices.FirstOrDefault(d => d.Id == last && d.IsUsable);
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
        SaveSetting("gui.last_device", dev.Id);

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
        if (IsCapturing && cmd != PinDeckCmd.Stop)
        {
            return; // also disabled in the view; core would refuse anyway
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
        o.Title = analog ? (AnalogTitleEnabled ? AnalogTitle : "") : (kind.TitleEnabled ? kind.Title : "");
        o.Aspect = ActiveAspect;
        o.SceneSplit = !analog && kind.SplitIntoScenes && kind.SplitEnabled ? 1 : 0;
        o.IdleStopMinutes = (int)Math.Max(0, analog ? AnalogIdleStopMinutes : kind.IdleStopMinutes);
        o.MaxDurationMinutes = (int)Math.Max(0, analog ? AnalogMaxDurationMinutes : kind.MaxDurationMinutes);
        o.Passes = analog ? 1 : (int)Math.Max(1, kind.Passes);
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

    // ================================================================== polling

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
                : string.IsNullOrEmpty(file) || !IsCapturingState(st.State)
                ? SessionStateText
                : SessionStateText +"  \u00B7  " + file;
        }
        Timecode = string.IsNullOrEmpty(st.Timecode) ? "--:--:--:--" : st.Timecode;
        SignalLocked = st.Signal != 0;
        SignalLockText = SignalLocked ? "Signal locked" : "No signal";
        FramesText = $"{st.Frames:N0} frames";
        DroppedText = $"{st.FramesDropped + st.WriteDropped:N0} dropped";
        FileSizeText = HumanSize(st.BytesWritten);
        ElapsedText = TimeSpan.FromSeconds(Math.Max(0, st.ElapsedS)).ToString(@"hh\:mm\:ss", CultureInfo.InvariantCulture);

        AudioPeakLeft = Math.Clamp(st.AudioPeakDb0, MeterFloorDb, 0);
        AudioPeakRight = Math.Clamp(st.AudioPeakDb1, MeterFloorDb, 0);
        AudioHoldLeft = _holdLeft.Push(AudioPeakLeft);
        AudioHoldRight = _holdRight.Push(AudioPeakRight);
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
        HasDiskInfo = st.DiskFreeBytes > 0;

        IsCapturing = st.State is PinState.Capturing or PinState.Stopping or PinState.Rewinding;

        ApplyDeckStatus(st.Deck, st.DeckBusy != 0);

        // One place says why there is no picture: the core's own sentence (bring-up step,
        // "no camera found", "no signal on the composite input", ...).
        NoVideoText = st.State switch
        {
            PinState.Preparing => st.Detail.Length > 0 ? st.Detail : "Preparing device…",
            PinState.Error => string.IsNullOrEmpty(st.ErrorText) ? "Device error" : st.ErrorText,
            _ => st.Detail.Length > 0 ? st.Detail : IsDvInput ? "No camera or deck signal" : "No video signal",
        };
        ProgressVisible = st.State == PinState.Preparing;
        ProgressIndeterminate = st.ProgressPercent < 0;
        ProgressValue = Math.Max(0, st.ProgressPercent);

        // Deck buttons have nothing to talk to without a camera (analog inputs report -1).
        DeckAvailable = !IsDvInput || st.CameraPresent != 0;

        WindowTitle = Native.FormatWindowTitle(in st, SelectedDevice?.DisplayName ?? DefaultDeviceName);
    }

    private static string FormatDb(float db) => db <= -143 ? "silent" : $"{db:0.0} dBFS";

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
            case PinEventKind.State:
                SessionState = (PinState)evt.A;
                RefreshDevices(); // badge: Open / Capturing
                break;
            case PinEventKind.Done:
                var ds = (PinStatus)evt.A;
                if (ds == PinStatus.Ok)
                {
                    ShowInfo("Done", "All command-line actions finished.", 1);
                }
                else
                {
                    ShowInfo("Actions stopped", Native.StrError(ds), 3);
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

    private static string LoadSetting(string key, string fallback = "")
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
        SaveSetting($"gui.title_{p}", k.Title);
    }

    private static int LoadInt(string key, int fallback) =>
        int.TryParse(LoadSetting(key), NumberStyles.Integer, CultureInfo.InvariantCulture, out var v) ? v : fallback;

    public void LoadSettings()
    {
        _loading = true;
        try
        {
            InputIndex = Math.Clamp(LoadInt("gui.last_input", 0), 0, 2);
            // Muted unless the user unmuted last time: a live monitor into
            // speakers next to the source's microphone is a feedback loop.
            IsMuted = LoadInt("gui.muted", 1) != 0;
            AnalogAspectIndex = Math.Clamp(LoadInt("gui.aspect_analog", 0), 0, 2);
            AnalogOutputPath = LoadSetting("gui.output_analog", DefaultOutput("analog"));
            AnalogTitle = LoadSetting("gui.title_analog");
            AnalogIdleStopMinutes = Math.Max(0, LoadInt("gui.idle_analog", 5));
            AnalogMaxDurationMinutes = Math.Max(0, LoadInt("gui.duration_analog", 0));
            SelectedKindTabIndex = Math.Clamp(LoadInt("gui.last_kind", 0), 0, 1);
            _lastStreamKind = SelectedKindTabIndex == 1 ? PinKind.Hdv : PinKind.Dv;
            int std = LoadInt("gui.std", (int)PinStd.Auto);
            SelectedStandard = Standards.FirstOrDefault(x => (int)x.Std == std) ?? Standards[0];
            DvOutputPath = LoadSetting("gui.output_dv", DefaultOutput("tape"));

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
                k.Title = LoadSetting($"gui.title_{p}");
            }
        }
        finally
        {
            _loading = false;
        }
    }

    private static string DefaultOutput(string baseName) =>
        System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyVideos), baseName);

    // ================================================================== launch options

    /// <summary>
    /// Applies the presets from pin_launch_parse over the loaded settings.
    /// Only the capture fields flagged in capture_fields are applied, as the
    /// header specifies.
    /// </summary>
    public void ApplyLaunch(in PinLaunch l)
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
        if (l.HasCaptureOpts == 0)
        {
            return;
        }

        var c = l.Capture;
        var fields = (PinOptFields)l.CaptureFields;
        bool analog = !IsDvInput;
        if (fields.HasFlag(PinOptFields.Path))
        {
            if (analog) AnalogOutputPath = c.Path; else DvOutputPath = c.Path;
        }
        if (fields.HasFlag(PinOptFields.Format))
        {
            var af = AnalogFormats.FirstOrDefault(f => f.Format == c.FormatAnalog);
            if (af is not null) AnalogFormat = af;
            DvSettings.ApplyFormat(c.FormatDv);
            HdvSettings.ApplyFormat(c.FormatHdv);
        }
        if (fields.HasFlag(PinOptFields.Title))
        {
            AnalogTitle = c.Title;
            DvSettings.Title = c.Title;
            HdvSettings.Title = c.Title;
        }
        if (fields.HasFlag(PinOptFields.Split))
        {
            DvSettings.SplitIntoScenes = HdvSettings.SplitIntoScenes = c.SceneSplit != 0;
        }
        if (fields.HasFlag(PinOptFields.Passes))
        {
            DvSettings.Passes = HdvSettings.Passes = Math.Max(1, c.Passes);
        }
        if (fields.HasFlag(PinOptFields.Idle))
        {
            if (analog) AnalogIdleStopMinutes = Math.Max(0, c.IdleStopMinutes);
            else DvSettings.IdleStopMinutes = HdvSettings.IdleStopMinutes = Math.Max(0, c.IdleStopMinutes);
        }
        if (fields.HasFlag(PinOptFields.Aspect))
        {
            // --aspect applies to the kinds the chosen input can deliver.
            if (analog) AnalogAspectIndex = (int)c.Aspect;
            else DvSettings.AspectIndex = HdvSettings.AspectIndex = (int)c.Aspect;
        }
    }

    public PinStatus RunActions(in PinLaunch l)
    {
        if (Session is not { IsInvalid: false })
        {
            return PinStatus.ErrState;
        }
        var st = Native.RunActions(Session, in l);
        Report(st, "Command-line actions");
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
