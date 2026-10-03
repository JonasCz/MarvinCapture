using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.UI;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;
using PinnacleCapture.Controls;
using PinnacleCapture.Interop;
using PinnacleCapture.Preview;
using PinnacleCapture.ViewModels;
using PinnacleCapture.Views;
using Windows.Graphics;
using Windows.Storage.Pickers;

namespace PinnacleCapture;

public sealed partial class MainWindow : Window
{
    // Logical (DIP) minimum size; converted to physical pixels for the presenter.
    private const int MinWidthDip = 1000;
    private const int MinHeightDip = 640;

    public MainViewModel VM { get; } = new();

    private readonly nint _hwnd;
    private readonly DispatcherQueueTimer _statusTimer;
    private readonly DispatcherQueueTimer _meterTimer;
    private Thread? _deviceWatch;
    private volatile bool _closing;
    private readonly TaskbarButton _taskbar;
    private readonly SemaphoreSlim _dialogGate = new(1, 1);

    private D3DPreview? _preview;
    private string? _openedDeviceId;
    private bool _startupDone;

    private bool _allowClose;
    private bool _finalizingForClose;

    private bool _keepingAwake;

    // command line
    private PinScriptHandle? _script;
    private string _scriptDevice = "";
    private PinScriptSettings _scriptSettings;
    private bool _pendingScript;
    private bool _scriptRunning;
    private string? _launchError;
    private bool _showHelpOnStart;

    private readonly KindSettingsView _dvView;
    private readonly KindSettingsView _hdvView;

    public MainWindow()
    {
        InitializeComponent();

        _hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        _taskbar = new TaskbarButton(_hwnd);
        // Raised inside the window procedure: handle on the next dispatcher turn.
        _taskbar.StartClicked += () => DispatcherQueue.TryEnqueue(() => _ = TaskbarStartAsync());
        _taskbar.StopClicked += () => DispatcherQueue.TryEnqueue(TaskbarStop);

        Title = "Pinnacle Capture";
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);
        if (Microsoft.UI.Composition.SystemBackdrops.MicaController.IsSupported())
        {
            SystemBackdrop = new MicaBackdrop(); // follows the system light/dark theme by itself
            RootGrid.Background = null;          // let Mica show through
        }
        // else (Windows 10): keep the solid SolidBackgroundFillColorBaseBrush theme background.


        // The two "Settings for" pages share one cell with the same collapse rules as the input panels.
        _dvView = new KindSettingsView(VM.DvSettings);
        _hdvView = new KindSettingsView(VM.HdvSettings);
        KindSwitch.Children.Add(_dvView);
        KindSwitch.Children.Add(_hdvView);

        ConfigureAppWindow();

        _statusTimer = DispatcherQueue.CreateTimer();
        _statusTimer.Interval = TimeSpan.FromMilliseconds(100);
        _statusTimer.Tick += (_, _) => OnStatusTick();
        // Audio meters run faster than the status tick (~30 Hz) with their own decay.
        _meterTimer = DispatcherQueue.CreateTimer();
        _meterTimer.Interval = TimeSpan.FromMilliseconds(33);
        _meterTimer.Tick += (_, _) => VM.UpdateMeters();

        VM.PropertyChanged += VM_PropertyChanged;
        VM.EngineEvent += VM_EngineEvent;

        ParseCommandLine();
        Closed += MainWindow_Closed;
    }

    // ================================================================== x:Bind helpers

    public static Visibility Collapsed(bool value) => value ? Visibility.Collapsed : Visibility.Visible;
    public static bool Both(bool a, bool b) => a && b;
    public static string DevicePlaceholder(bool none) => none ? "No devices found" : "Select a device";
    public static Thickness PlayGlyphNudge(bool capturing) => capturing ? new Thickness(0) : new Thickness(2, 0, 0, 0);
    public static string AnalogCaptureHint(bool capturing) =>
        capturing ? "Finishes the file safely" : "Records the analog input to the file";

    public static double EnabledOpacity(bool enabled) => enabled ? 1.0 : 0.45;
    public static string StartStopName(bool capturing) => capturing ? "Stop capture" : "Start capture";
    public static string Label(string name, string value) => $"{name}: {value}";
    public static string SignalName(string lockText, string type) => type.Length > 0 ? $"Signal: {lockText}, {type}" : $"Signal: {lockText}";
    public static string StorageName(string tip, bool low) => (low ? "Low disk space. " : "Storage: ") + tip;
    public static string SignalText(string lockText, string type) => type.Length > 0 ? lockText + " · " + type : lockText;
    public static Visibility Shown(bool value) => value ? Visibility.Visible : Visibility.Collapsed;

    public static InfoBarSeverity Severity(int s) => s switch
    {
        1 => InfoBarSeverity.Success,
        2 => InfoBarSeverity.Warning,
        3 => InfoBarSeverity.Error,
        _ => InfoBarSeverity.Informational,
    };

    // ================================================================== window setup

    private double Scale => Math.Max(1, Win32.GetDpiForWindow(_hwnd)) / 96.0;

    private void ConfigureAppWindow()
    {
        var aw = AppWindow;
        if (aw.Presenter is OverlappedPresenter op)
        {
            op.PreferredMinimumWidth = (int)(MinWidthDip * Scale);
            op.PreferredMinimumHeight = (int)(MinHeightDip * Scale);
        }

        RestoreWindowGeometry();

        aw.Closing += AppWindow_Closing;
        aw.Changed += AppWindow_Changed;
    }

    private void RestoreWindowGeometry()
    {
        var aw = AppWindow;
        var saved = SafeSettingsGet("gui.window");
        var parts = saved.Split(',');
        var area = default(DisplayArea);
        if (parts.Length == 4 &&
            parts.All(p => int.TryParse(p, NumberStyles.Integer, CultureInfo.InvariantCulture, out _)))
        {
            int x = int.Parse(parts[0], CultureInfo.InvariantCulture);
            int y = int.Parse(parts[1], CultureInfo.InvariantCulture);
            int w = int.Parse(parts[2], CultureInfo.InvariantCulture);
            int h = int.Parse(parts[3], CultureInfo.InvariantCulture);

            // Only restore onto a monitor that still exists: a rectangle that no longer
            // touches any display (monitor unplugged, resolution changed) falls back to the default.
            area = DisplayArea.GetFromRect(new RectInt32(x, y, w, h), DisplayAreaFallback.None);
            if (area is not null)
            {
                var wa = area.WorkArea;
                w = Math.Clamp(w, (int)(MinWidthDip * Scale), wa.Width);
                h = Math.Clamp(h, (int)(MinHeightDip * Scale), wa.Height);
                x = Math.Clamp(x, wa.X, wa.X + wa.Width - w);
                y = Math.Clamp(y, wa.Y, wa.Y + wa.Height - h);
                aw.MoveAndResize(new RectInt32(x, y, w, h));
            }
        }
        if (area is null)
        {
            var wa = DisplayArea.GetFromWindowId(aw.Id, DisplayAreaFallback.Primary).WorkArea;
            int w = Math.Min((int)(1280 * Scale), wa.Width);
            int h = Math.Min((int)(800 * Scale), wa.Height);
            aw.MoveAndResize(new RectInt32(wa.X + (wa.Width - w) / 2, wa.Y + (wa.Height - h) / 2, w, h));
        }
        else if (SafeSettingsGet("gui.window_maximized") == "1" && aw.Presenter is OverlappedPresenter op)
        {
            op.Maximize(); // the saved rectangle above is the restored size
        }
    }

    /// <summary>
    /// Saves the restored (normal) rectangle plus whether the window is maximised. While
    /// maximised the rectangle from the last restored state is kept, so un-maximising later
    /// returns to the user's size. Minimised: nothing to learn, keep the previous values.
    /// </summary>
    private void SaveWindowGeometry()
    {
        if (AppWindow.Presenter is not OverlappedPresenter op)
        {
            return;
        }
        try
        {
            switch (op.State)
            {
                case OverlappedPresenterState.Restored:
                    var p = AppWindow.Position;
                    var s = AppWindow.Size;
                    SafeSettingsSet("gui.window", string.Create(CultureInfo.InvariantCulture, $"{p.X},{p.Y},{s.Width},{s.Height}"));
                    SafeSettingsSet("gui.window_maximized", "0");
                    break;
                case OverlappedPresenterState.Maximized:
                    SafeSettingsSet("gui.window_maximized", "1");
                    break;
            }
        }
        catch (Exception)
        {
            // window already gone; keep what was saved before
        }
    }

    private static string SafeSettingsGet(string key)
    {
        try { return Native.SettingsGet(key); } catch (Exception) { return ""; }
    }

    private static void SafeSettingsSet(string key, string value)
    {
        try { Native.SettingsSet(key, value); } catch (Exception) { }
    }

    private void AppWindow_Changed(AppWindow sender, AppWindowChangedEventArgs args)
    {
        if (args.DidPresenterChange || args.DidSizeChange || args.DidVisibilityChange)
        {
            UpdatePreviewPause();
        }
    }

    /// <summary>No decoding or rendering while minimised or hidden (pin_preview_enable(false) + parked thread).</summary>
    private void UpdatePreviewPause()
    {
        bool minimized = AppWindow.Presenter is OverlappedPresenter { State: OverlappedPresenterState.Minimized };
        bool hidden = !AppWindow.IsVisible || !PreviewPanel.IsLoaded;
        _preview?.SetPaused(minimized || hidden);
    }

    // ================================================================== startup

    private async void RootGrid_Loaded(object sender, RoutedEventArgs e)
    {
        VM.LoadGlobalSettings();

        try
        {
            _preview = new D3DPreview(PreviewPanel, () => VM.Session);
            _preview.Initialize();
        }
        catch (Exception ex)
        {
            _preview = null;
            VM.ShowInfo("Preview unavailable", $"Direct3D 11 could not be initialised: {ex.Message}", 2);
        }

        VM.RefreshDevices();
        var preferred = _script is not null ? _scriptDevice : null;
        // Selecting the device loads its own settings; the command-line settings go on top of them.
        VM.SelectedDevice = VM.PickInitialDevice(string.IsNullOrEmpty(preferred) ? null : preferred);
        if (_script is not null)
        {
            VM.ApplyScriptSettings(in _scriptSettings);
        }
        _startupDone = true;
        OpenSelectedIfNeeded();

        _statusTimer.Start();
        _meterTimer.Start();
        SyncKindSelector();
        StartDeviceWatch();

        if (_showHelpOnStart)
        {
            await ShowHelpAsync(null);
        }
        else if (_launchError is not null)
        {
            await ShowHelpAsync(_launchError);
        }
    }

    private void OpenSelectedIfNeeded()
    {
        if (!_startupDone || VM.IsCapturing)
        {
            return;
        }
        var dev = VM.SelectedDevice;
        if (dev is null || dev.Id == _openedDeviceId)
        {
            return;
        }
        _preview?.ResetFrameState();
        bool ok = VM.OpenSelectedDevice();
        _openedDeviceId = ok ? dev.Id : null;
        if (ok)
        {
            // Re-read the list so this device's badge flips to "Open" (OPEN_HERE).
            VM.RefreshDevices();
            _preview?.Wake();
            UpdatePreviewPause();
        }
    }

    // ================================================================== command line

    private void ParseCommandLine()
    {
        // Whole argv; the core's parser wants it without the program name. No arguments at all
        // is a normal start (the core would answer that with its help text).
        var args = Environment.GetCommandLineArgs().Skip(1).ToArray();
        if (args.Length == 0)
        {
            return;
        }
        if (args.Any(a => a is "/?" or "-?"))
        {
            _showHelpOnStart = true;
            return;
        }

        var st = Native.ScriptParse(args, out var script, out var err);
        if (st != PinStatus.Ok || script is null)
        {
            _launchError = string.IsNullOrEmpty(err) ? Native.StrError(st) : err;
            return;
        }
        if (Native.ScriptHelpRequested(script))
        {
            script.Dispose();
            _showHelpOnStart = true;
            return;
        }
        _script = script;
        if (Native.ScriptDebug(script) && ConsoleOutput.Enabled)
        {
            Native.SetLogLevel(0); // ConsoleOutput mirrors the core's debug lines
        }
        // --device may be a recording to replay: the core lists it as "replay:<name>" once registered
        _scriptDevice = Native.ScriptDevice(script);
        if (_scriptDevice.Length > 0 && System.IO.File.Exists(_scriptDevice))
        {
            Native.SetReplayFile(_scriptDevice);
            _scriptDevice = "replay:" + System.IO.Path.GetFileName(_scriptDevice);
        }
        _scriptSettings = Native.ScriptSettings(script);
        _pendingScript = Native.ScriptNeedsSession(script);
    }

    private void RunPendingScriptIfReady()
    {
        if (_pendingScript && _script is not null && VM.Session is { IsInvalid: false } && VM.SessionState == PinState.Ready)
        {
            _pendingScript = false;
            _scriptRunning = VM.RunScript(_script) == PinStatus.Ok;
        }
    }

    // ================================================================== polling

    private void OnStatusTick()
    {
        VM.Tick();

        bool hasVideo = VM.Session is { IsInvalid: false } && _preview is not null &&
                        _preview.LastFrameTick != 0 && Environment.TickCount64 - _preview.LastFrameTick < 1000;
        if (VM.PreviewHasVideo != hasVideo)
        {
            VM.PreviewHasVideo = hasVideo;
        }

        TrackPreviewAspect();
        RunPendingScriptIfReady();
        UpdateTaskbar();
        UpdateKeepAwake();

        if (_finalizingForClose && VM.SessionState is PinState.Ready or PinState.Error or PinState.Closed)
        {
            FinishClose();
        }
    }

    // ================================================================== preview frame fit

    private int _fitDarNum = 4, _fitDarDen = 3;

    private void PreviewHost_SizeChanged(object sender, SizeChangedEventArgs e)
    {
        UpdatePreviewFrame();
        DispatcherQueue.TryEnqueue(UpdateThumbnailClip); // after the layout pass that applies the new frame size
    }

    /// <summary>
    /// Sizes PreviewFrame (swap chain panel + no-video overlay) to the largest
    /// rectangle of the picture's display aspect that fits the preview area
    /// (pin_fit_rect, in DIPs), centred. The swap chain then fills its panel,
    /// so there are no drawn black bars; the window background shows around it.
    /// </summary>
    private void UpdatePreviewFrame()
    {
        int w = (int)PreviewHost.ActualWidth, h = (int)PreviewHost.ActualHeight;
        if (w <= 0 || h <= 0)
        {
            return;
        }
        Native.FitRect(_fitDarNum, _fitDarDen, w, h, out _, out _, out int rw, out int rh);
        PreviewFrame.Width = Math.Max(1, rw);
        PreviewFrame.Height = Math.Max(1, rh);
    }

    /// <summary>Follow the aspect of what is actually shown (frame DAR, override applied).</summary>
    private void TrackPreviewAspect()
    {
        int num = 4, den = 3;
        if (_preview is not null && _preview.FrameDarNum > 0 && _preview.FrameDarDen > 0 && VM.PreviewHasVideo)
        {
            num = _preview.FrameDarNum;
            den = _preview.FrameDarDen;
        }
        else if (VM.ActiveAspect == PinAspect.Aspect16x9)
        {
            num = 16; den = 9;
        }
        if (num != _fitDarNum || den != _fitDarDen)
        {
            _fitDarNum = num;
            _fitDarDen = den;
            UpdatePreviewFrame();
        }
    }

    /// <summary>
    /// Device list updates without polling: a background thread blocks in
    /// pin_devices_wait (plug/unplug, or another window taking or releasing a
    /// device) and hands each change to the UI thread.
    /// </summary>
    private void StartDeviceWatch()
    {
        _deviceWatch = new Thread(() =>
        {
            while (!_closing)
            {
                int r = Native.DevicesWait(60_000);
                if (_closing)
                {
                    break;
                }
                if (r < 0)
                {
                    Thread.Sleep(2000); // no notifications available: fall back to a slow rescan
                }
                if (r != 0)
                {
                    DispatcherQueue.TryEnqueue(OnDevicesChanged);
                }
            }
        })
        { IsBackground = true, Name = "Device watch" };
        _deviceWatch.Start();
    }

    private void OnDevicesChanged()
    {
        if (_closing)
        {
            return;
        }
        VM.RefreshDevices();

        // The open device was unplugged: drop the session (a running capture
        // ends with an error from the core and is finalised by it).
        if (_openedDeviceId is not null && !VM.IsCapturing && VM.Devices.All(d => d.Id != _openedDeviceId))
        {
            VM.CloseSession();
            _openedDeviceId = null;
            _preview?.ResetFrameState();
        }
        // Nothing open (none connected before, or the previous one went away): take the first usable one.
        if (VM.SelectedDevice is null || _openedDeviceId is null)
        {
            VM.SelectedDevice = VM.PickInitialDevice(null);
        }
        OpenSelectedIfNeeded();
    }

    /// <summary>Selects the DV / HDV options tab that the view model says is current.</summary>
    private void SyncKindSelector()
    {
        int i = Math.Clamp(VM.SelectedKindTabIndex, 0, KindSelector.Items.Count - 1);
        if (!ReferenceEquals(KindSelector.SelectedItem, KindSelector.Items[i]))
        {
            KindSelector.SelectedItem = KindSelector.Items[i];
        }
    }

    /// <summary>
    /// Taskbar progress: the core picks mode and fraction from the status; an error
    /// InfoBar that is still open (device / capture failure) shows red until dismissed.
    /// </summary>
    private void UpdateTaskbar()
    {
        if (VM.InfoOpen && VM.InfoSeverity == 3)
        {
            _taskbar.SetProgress(PinProgressMode.Error, 1);
        }
        else
        {
            _taskbar.SetProgress(VM.TaskbarMode, VM.TaskbarFraction);
        }
        _taskbar.SetCapturing(VM.IsCapturing);
        _taskbar.SetControls(VM.TaskbarStartEnabled, VM.TaskbarStopEnabled);
        UpdateThumbnailClip();

        // A capture that ended or failed while the window is in the background: flash its taskbar button.
        if (_wasCapturing && !VM.IsCapturing && !_finalizingForClose && !_closing)
        {
            _taskbar.FlashIfInBackground();
        }
        _wasCapturing = VM.IsCapturing;
    }

    private bool _wasCapturing;

    private bool _taskbarStartBusy;

    /// <summary>Thumbnail "Start capture": exactly the in-window start (checks, dialogs), for the current mode.</summary>
    private async Task TaskbarStartAsync()
    {
        if (_taskbarStartBusy || !VM.TaskbarStartEnabled)
        {
            return;
        }
        _taskbarStartBusy = true; // a double click must not queue a second start behind a dialog
        try
        {
            await StartCaptureAsync(playFirst: false);
        }
        finally
        {
            _taskbarStartBusy = false;
        }
    }

    /// <summary>Thumbnail "Stop capture": stops the way the capture was started.</summary>
    private void TaskbarStop()
    {
        if (VM.TaskbarStopEnabled)
        {
            VM.StopCaptureAsStarted();
        }
    }

    /// <summary>
    /// The taskbar thumbnail shows only the video area (ITaskbarList3::SetThumbnailClip, client
    /// pixels), or the whole window when no preview area is visible. Cheap: the shell is only
    /// called when the rectangle changes.
    /// </summary>
    private void UpdateThumbnailClip()
    {
        (int, int, int, int)? clip = null;
        var xr = RootGrid.XamlRoot;
        if (xr is not null && PreviewFrame.Visibility == Visibility.Visible
            && PreviewFrame.ActualWidth >= 1 && PreviewFrame.ActualHeight >= 1)
        {
            double s = xr.RasterizationScale;
            var p = PreviewFrame.TransformToVisual(RootGrid).TransformPoint(new Windows.Foundation.Point(0, 0));
            int l = (int)Math.Round(p.X * s), t = (int)Math.Round(p.Y * s);
            int r = (int)Math.Round((p.X + PreviewFrame.ActualWidth) * s), b = (int)Math.Round((p.Y + PreviewFrame.ActualHeight) * s);
            int cw = (int)Math.Round(RootGrid.ActualWidth * s), ch = (int)Math.Round(RootGrid.ActualHeight * s);
            l = Math.Clamp(l, 0, cw); r = Math.Clamp(r, 0, cw);
            t = Math.Clamp(t, 0, ch); b = Math.Clamp(b, 0, ch);
            if (r > l && b > t)
            {
                clip = (l, t, r, b);
            }
        }
        _taskbar.SetThumbnailClip(clip);
    }

    /// <summary>Keeps the machine from sleeping mid-capture; released as soon as capture ends.</summary>
    private void UpdateKeepAwake()
    {
        if (VM.IsCapturing && !_keepingAwake)
        {
            Win32.SetThreadExecutionState(Win32.ES_CONTINUOUS | Win32.ES_SYSTEM_REQUIRED);
            _keepingAwake = true;
        }
        else if (!VM.IsCapturing && _keepingAwake)
        {
            Win32.SetThreadExecutionState(Win32.ES_CONTINUOUS);
            _keepingAwake = false;
        }
    }

    private bool _statusFitQueued;

    private void StatusBar_SizeChanged(object sender, SizeChangedEventArgs e) => FitStatusBar();

    /// <summary>
    /// Keeps the one-line status bar inside the window: shows the items the state calls for
    /// (deck only on DV/HDV, storage only with disk info), then, while they do not fit next to the
    /// file text's minimum width, drops the lowest-priority one: the free-space part of storage,
    /// frames, storage, time, deck. The file text trims on its own.
    /// </summary>
    private void FitStatusBar()
    {
        bool deck = VM.IsDvInput, storage = VM.HasDiskInfo;
        StatusDeckHost.Visibility = deck ? Visibility.Visible : Visibility.Collapsed;
        StatusStorageHost.Visibility = storage ? Visibility.Visible : Visibility.Collapsed;
        StatusFramesHost.Visibility = StatusTimeHost.Visibility = Visibility.Visible;
        StatusFreeHost.Visibility = Visibility.Visible;

        double avail = StatusBar.ActualWidth - StatusBar.Padding.Left - StatusBar.Padding.Right - 80;
        if (avail <= 0) return;
        FrameworkElement[] hosts = { StatusDeckHost, StatusTimeHost, StatusSignalHost, StatusFramesHost,
                                     StatusStorageHost, StatusMeters, StatusMute };
        double Need()
        {
            double sum = 0;
            foreach (var h in hosts)
            {
                if (h.Visibility == Visibility.Collapsed) continue;
                h.Measure(new Windows.Foundation.Size(double.PositiveInfinity, double.PositiveInfinity));
                sum += h.DesiredSize.Width;
            }
            return sum;
        }
        Action[] drops =
        {
            () => StatusFreeHost.Visibility = Visibility.Collapsed,
            () => StatusFramesHost.Visibility = Visibility.Collapsed,
            () => StatusStorageHost.Visibility = Visibility.Collapsed,
            () => StatusTimeHost.Visibility = Visibility.Collapsed,
            () => StatusDeckHost.Visibility = Visibility.Collapsed,
        };
        foreach (var drop in drops)
        {
            if (Need() <= avail) break;
            drop();
        }
    }

    private void QueueStatusFit()
    {
        if (_statusFitQueued) return;
        _statusFitQueued = true;
        DispatcherQueue.TryEnqueue(() => { _statusFitQueued = false; FitStatusBar(); });
    }

    private void VM_PropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        switch (e.PropertyName)
        {
            case nameof(MainViewModel.SizeText):
            case nameof(MainViewModel.StorageFreeText):
            case nameof(MainViewModel.FramesTotalText):
            case nameof(MainViewModel.StatusTimeText):
            case nameof(MainViewModel.SignalLockText):
            case nameof(MainViewModel.SignalTypeText):
            case nameof(MainViewModel.DeckStateText):
            case nameof(MainViewModel.IsDvInput):
            case nameof(MainViewModel.HasDiskInfo):
                QueueStatusFit();
                break;
            case nameof(MainViewModel.WindowTitle):
                Title = VM.WindowTitle;
                break;
            case nameof(MainViewModel.SessionStateText):
                // Announce state changes (not the 10 Hz counter updates) to screen readers.
                Microsoft.UI.Xaml.Automation.Peers.FrameworkElementAutomationPeer.FromElement(StatusBarText)?
                    .RaiseAutomationEvent(Microsoft.UI.Xaml.Automation.Peers.AutomationEvents.LiveRegionChanged);
                break;
            case nameof(MainViewModel.SelectedDevice):
                // null shows up transiently while the list is rebuilt; never close on it.
                if (VM.SelectedDevice is not null)
                {
                    OpenSelectedIfNeeded();
                }
                break;
            case nameof(MainViewModel.IsCapturing):
                _dvView.IsEditable = _hdvView.IsEditable = VM.IsIdle;
                break;
            case nameof(MainViewModel.SelectedKindTabIndex):
                SyncKindSelector(); // the detected stream switched DV <-> HDV
                break;
        }
    }

    private void VM_EngineEvent(object? sender, EngineEventArgs e)
    {
        if (e.Kind == PinEventKind.Done)
        {
            _scriptRunning = false;
        }
        // A capture that ended abnormally (device or camera gone, disk full, write error):
        // a dialog, with how much was captured. A normal end (limit, end of tape) only goes to
        // the status bar (MainViewModel.ApplyStatus). Not while closing or running unattended
        // command-line actions.
        if (e.Kind == PinEventKind.CaptureEnded && Native.StopReasonAbnormal((PinStopReason)e.A) &&
            !_closing && !_finalizingForClose && !_scriptRunning)
        {
            _ = ShowDialogAsync("Capture stopped", e.Text, null, "OK");
        }
    }

    // ================================================================== devices

    private void DeviceCombo_DropDownOpened(object sender, object e) => VM.RefreshDevices();

    // ================================================================== deck

    private void Deck_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not ToggleButton b)
        {
            return;
        }
        var cmd = (string)b.Tag switch
        {
            "Rew" => PinDeckCmd.Rew,
            "Play" => PinDeckCmd.Play,
            "Ff" => PinDeckCmd.Ff,
            _ => PinDeckCmd.Stop,
        };
        VM.UserRequestedDeck(cmd);

        // A click toggles the button locally; the checked state belongs to the
        // deck's reported transport mode, so put it back and let the status poll
        // move it when the deck actually changes mode.
        b.IsChecked = cmd switch
        {
            PinDeckCmd.Rew => VM.IsRewChecked,
            PinDeckCmd.Play => VM.IsPlayChecked,
            PinDeckCmd.Ff => VM.IsFfChecked,
            _ => VM.IsStopChecked,
        };
    }

    private void KindSelector_SelectionChanged(SelectorBar sender, SelectorBarSelectionChangedEventArgs args)
    {
        VM.SelectedKindTabIndex = Math.Max(0, sender.Items.IndexOf(sender.SelectedItem));
    }

    // ================================================================== capture

    private async void Capture_Click(object sender, RoutedEventArgs e)
    {
        if (VM.IsCapturing)
        {
            // DV/HDV "Manual capture" button while recording: stop, leave the tape running.
            // (The analog Capture button shares this handler; there is no deck then.)
            VM.StopCapture(stopDeck: false);
            return;
        }
        await StartCaptureAsync(playFirst: false);
    }

    private async void PlayAndCapture_Click(object sender, RoutedEventArgs e)
    {
        if (VM.IsCapturing)
        {
            // The same button is "Stop capture & stop tape" while recording.
            VM.StopCapture(stopDeck: true);
            return;
        }
        // start_deck = 1: the core sends PLAY first, then starts the writer
        // (and stops the deck again when this capture stops).
        await StartCaptureAsync(playFirst: true);
    }

    private async Task StartCaptureAsync(bool playFirst)
    {
        var path = VM.IsDvInput ? VM.DvOutputPath : VM.AnalogOutputPath;
        if (string.IsNullOrWhiteSpace(path))
        {
            VM.ShowInfo("No output file", "Choose where to save the capture first.", 2);
            return;
        }

        // "Play and capture" = rewind to the start of the tape, play, record.
        var opts = VM.BuildCaptureOpts(startDeck: playFirst, rewindFirst: playFirst);
        var st = VM.CheckOutput(in opts, out var check);
        if (st == PinStatus.ErrArg && !string.IsNullOrEmpty(check.Message))
        {
            VM.ShowInfo("Invalid file name", check.Message, 3);
            return;
        }
        if (st != PinStatus.Ok)
        {
            VM.ShowInfo("Can't capture to this location", $"{Native.StrError(st)} {check.Message}".Trim(), 3);
            return;
        }

        if (!string.IsNullOrEmpty(check.Message) || check.Fat32 != 0)
        {
            var msg = check.Message;
            if (check.Fat32 != 0 && string.IsNullOrEmpty(msg))
            {
                msg = "The destination is a FAT32 drive. Files larger than 4 GB will fail.";
            }
            var free = MainViewModel.HumanSize(check.FreeBytes);
            var r = await ShowDialogAsync("Check the destination",
                $"{msg}\n\nFree space: {free} (about {check.MinutesLeft} minutes at this format).",
                "Capture anyway", "Cancel");
            if (r != ContentDialogResult.Primary)
            {
                return;
            }
        }

        if (check.LowSpace != 0)
        {
            var drive = Path.GetPathRoot(Path.GetFullPath(check.FirstPath)) ?? "the output drive";
            var r = await ShowDialogAsync("Low disk space",
                $"Only {MainViewModel.HumanSize(check.FreeBytes)} free on {drive}. Continue?",
                "OK", "Cancel");
            if (r != ContentDialogResult.Primary)
            {
                return;
            }
        }

        bool overwrite = false;
        if (check.Collision != 0)
        {
            var r = await ShowDialogAsync("Replace existing file?",
                $"{check.FirstPath}\n\nalready exists. Do you want to overwrite it?",
                "Overwrite", "Cancel");
            if (r != ContentDialogResult.Primary)
            {
                return;
            }
            overwrite = true;
        }

        VM.StartCapture(in opts, overwrite);
    }

    private async void Browse_Click(object sender, RoutedEventArgs e)
    {
        bool analog = (string)((FrameworkElement)sender).Tag == "analog";
        var picker = new FolderPicker();
        WinRT.Interop.InitializeWithWindow.Initialize(picker, _hwnd);
        picker.SuggestedStartLocation = PickerLocationId.VideosLibrary;
        picker.FileTypeFilter.Add("*");

        Windows.Storage.StorageFolder? folder;
        try
        {
            folder = await picker.PickSingleFolderAsync();
        }
        catch (Exception ex)
        {
            VM.ShowInfo("Could not open the folder dialog", ex.Message, 3);
            return;
        }
        if (folder is null)
        {
            return;
        }

        if (analog)
        {
            VM.AnalogOutputDir = folder.Path;
        }
        else
        {
            VM.DvOutputDir = folder.Path;
        }
    }

    // ================================================================== menu

    private void NewWindow_Click(object sender, RoutedEventArgs e)
    {
        var exe = Environment.ProcessPath;
        if (exe is null)
        {
            return;
        }
        try
        {
            Process.Start(new ProcessStartInfo(exe) { UseShellExecute = false });
        }
        catch (Exception ex)
        {
            VM.ShowInfo("Could not open a new window", ex.Message, 3);
        }
    }

    private async void About_Click(object sender, RoutedEventArgs e)
    {
        var appVersion = typeof(App).Assembly.GetName().Version?.ToString(3) ?? "0.0.0";
        var text = $"Pinnacle Capture {appVersion}\n" +
                   $"{Native.VersionString()} (API {Native.ApiVersion()})\n\n" +
                   "Open driver for the Pinnacle Studio 500-USB.\n" +
                   "Licensed under the GNU Affero General Public License v3.0.";
        await ShowDialogAsync("About Pinnacle Capture", text, null, "Close");
    }

    private async void Help_Click(object sender, RoutedEventArgs e) => await ShowHelpAsync(null);

    private async Task ShowHelpAsync(string? error)
    {
        var help = Native.ScriptHelp();
        var body = new StackPanel { Spacing = 12 };
        if (error is not null)
        {
            body.Children.Add(new InfoBar
            {
                IsOpen = true,
                IsClosable = false,
                Severity = InfoBarSeverity.Error,
                Title = "Invalid command line",
                Message = error,
            });
        }
        body.Children.Add(new ScrollViewer
        {
            MaxHeight = 420,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
            Content = new TextBlock
            {
                Text = help,
                FontFamily = new FontFamily("Consolas"),
                FontSize = 13,
                IsTextSelectionEnabled = true,
                TextWrapping = TextWrapping.NoWrap,
            },
        });
        await ShowDialogAsync("Command-line options", body, null, "Close", wide: true);
    }

    // ================================================================== dialogs

    private Task<ContentDialogResult> ShowDialogAsync(string title, string text, string? primary, string close) =>
        ShowDialogAsync(title, new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, IsTextSelectionEnabled = true }, primary, close);

    private async Task<ContentDialogResult> ShowDialogAsync(string title, object content, string? primary, string close, bool wide = false)
    {
        // Only one ContentDialog may be open per XamlRoot at a time.
        await _dialogGate.WaitAsync();
        BringToFront(); // a start from the taskbar thumbnail can reach a dialog while the window is behind
        try
        {
            var dlg = new ContentDialog
            {
                XamlRoot = RootGrid.XamlRoot,
                Title = title,
                Content = content,
                CloseButtonText = close,
                DefaultButton = primary is null ? ContentDialogButton.Close : ContentDialogButton.Primary,
                RequestedTheme = RootGrid.ActualTheme,
            };
            if (primary is not null)
            {
                dlg.PrimaryButtonText = primary;
            }
            if (wide)
            {
                // Default ContentDialogMaxWidth (548) clips the fixed-width help text.
                dlg.Resources["ContentDialogMaxWidth"] = 960.0;
            }
            return await dlg.ShowAsync();
        }
        finally
        {
            _dialogGate.Release();
        }
    }

    private void BringToFront()
    {
        if (Win32.GetForegroundWindow() == _hwnd)
        {
            return;
        }
        if (AppWindow.Presenter is OverlappedPresenter { State: OverlappedPresenterState.Minimized } p)
        {
            p.Restore();
        }
        Activate();
    }

    // ================================================================== close

    private async void AppWindow_Closing(AppWindow sender, AppWindowClosingEventArgs args)
    {
        SaveWindowGeometry();
        if (_allowClose)
        {
            return;
        }
        if (_finalizingForClose)
        {
            args.Cancel = true; // already stopping; the window closes itself when done
            return;
        }
        if (!VM.IsCapturing)
        {
            return; // Closed handler does the cleanup
        }

        args.Cancel = true;
        var r = await ShowDialogAsync("Stop capture and quit?",
            "A capture is running. Stopping finishes the current file safely before the window closes.",
            "Stop and quit", "Keep capturing");
        if (r != ContentDialogResult.Primary || !VM.IsCapturing)
        {
            if (!VM.IsCapturing && r == ContentDialogResult.Primary)
            {
                _allowClose = true;
                Close();
            }
            return;
        }

        _finalizingForClose = true;
        FinalizingOverlay.Visibility = Visibility.Visible;
        VM.StopCapture();
        // OnStatusTick calls FinishClose() once the core reports READY (files finalised).
    }

    private void FinishClose()
    {
        _finalizingForClose = false;
        _allowClose = true;
        Close();
    }

    private void MainWindow_Closed(object sender, WindowEventArgs args)
    {
        SaveWindowGeometry();
        _statusTimer.Stop();
        _meterTimer.Stop();
        _closing = true;
        Native.DevicesWake();
        _preview?.Dispose();
        _preview = null;
        VM.Dispose(); // pin_close: blocks until any capture is finalised
        if (_keepingAwake)
        {
            Win32.SetThreadExecutionState(Win32.ES_CONTINUOUS);
            _keepingAwake = false;
        }
        _taskbar.SetProgress(PinProgressMode.None, 0);
        _taskbar.SetCapturing(false);
        _taskbar.Dispose();
    }
}
