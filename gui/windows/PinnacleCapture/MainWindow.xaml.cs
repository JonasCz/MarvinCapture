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
    private const int MinWidthDip = 1100;
    private const int MinHeightDip = 640;

    public MainViewModel VM { get; } = new();

    private readonly nint _hwnd;
    private readonly DispatcherQueueTimer _statusTimer;
    private readonly DispatcherQueueTimer _meterTimer;
    private Thread? _deviceWatch;
    private volatile bool _closing;
    private readonly TaskbarProgress _taskbar;
    private readonly SemaphoreSlim _dialogGate = new(1, 1);

    private D3DPreview? _preview;
    private string? _openedDeviceId;
    private bool _startupDone;

    private bool _allowClose;
    private bool _finalizingForClose;

    private TaskbarProgressState _lastTaskbarState = TaskbarProgressState.NoProgress;
    private int _lastTaskbarValue = -1;
    private bool _keepingAwake;

    // command line
    private PinLaunch _launch;
    private bool _hasLaunch;
    private bool _pendingActions;
    private bool _exitWhenDone;
    private string? _launchError;
    private bool _showHelpOnStart;

    private readonly KindSettingsView _dvView;
    private readonly KindSettingsView _hdvView;

    public MainWindow()
    {
        InitializeComponent();

        _hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        _taskbar = new TaskbarProgress(_hwnd);

        Title = "Pinnacle Capture";
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);
        if (Microsoft.UI.Composition.SystemBackdrops.MicaController.IsSupported())
        {
            SystemBackdrop = new MicaBackdrop(); // follows the system light/dark theme by itself
            RootGrid.Background = null;          // let Mica show through
        }
        // else (Windows 10): keep the solid SolidBackgroundFillColorBaseBrush theme background.

        ApplyDebugThemeOverride();

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

    public static Visibility DiskVisible(bool hasInfo, bool low, bool wantLow) =>
        hasInfo && low == wantLow ? Visibility.Visible : Visibility.Collapsed;

    public static string DiskName(string free, string left, bool low) =>
        (low ? "Low disk space: " : "Disk: ") + free + ", " + left;

    public static double EnabledOpacity(bool enabled) => enabled ? 1.0 : 0.45;
    public static string StartStopName(bool capturing) => capturing ? "Stop capture" : "Start capture";
    public static string Label(string name, string value) => $"{name}: {value}";
    public static string SignalName(string lockText, string type) => type.Length > 0 ? $"Signal: {lockText}, {type}" : $"Signal: {lockText}";
    public static string StorageName(string size, string free, string left, bool low) =>
        (low ? "Low disk space. " : "Storage: ") + size + ", " + free + ", " + left;

    public static InfoBarSeverity Severity(int s) => s switch
    {
        1 => InfoBarSeverity.Success,
        2 => InfoBarSeverity.Warning,
        3 => InfoBarSeverity.Error,
        _ => InfoBarSeverity.Informational,
    };

    /// <summary>
    /// Testing aid only: PIN_THEME=dark|light forces the app theme so both
    /// themes can be checked without changing the Windows setting. Normal
    /// runs follow the system theme.
    /// </summary>
    private void ApplyDebugThemeOverride()
    {
        var theme = Environment.GetEnvironmentVariable("PIN_THEME")?.Trim().ToLowerInvariant();
        if (theme is not ("dark" or "light"))
        {
            return;
        }
        RootGrid.RequestedTheme = theme == "dark" ? ElementTheme.Dark : ElementTheme.Light;
        AppWindow.TitleBar.PreferredTheme = theme == "dark" ? TitleBarTheme.Dark : TitleBarTheme.Light;
    }

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
        var preferred = _hasLaunch ? _launch.Device : null;
        // Selecting the device loads its own settings; the command-line presets go on top of them.
        VM.SelectedDevice = VM.PickInitialDevice(string.IsNullOrEmpty(preferred) ? null : preferred);
        if (_hasLaunch)
        {
            VM.ApplyLaunch(in _launch);
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
        // Whole argv: pin_launch_parse skips argv[0] itself.
        var args = Environment.GetCommandLineArgs();
        if (args.Length == 0)
        {
            return;
        }
        if (args.Any(a => a is "--help" or "-h" or "/?" or "-?"))
        {
            _showHelpOnStart = true;
            return;
        }

        var st = Native.LaunchParse(args, out var launch, out var err);
        if (st != PinStatus.Ok)
        {
            _launchError = string.IsNullOrEmpty(err) ? Native.StrError(st) : err;
            return;
        }
        _launch = launch;
        _hasLaunch = true;
        _pendingActions = launch.ActionCount > 0;
        _exitWhenDone = launch.ExitWhenDone != 0;
    }

    private void RunPendingActionsIfReady()
    {
        if (_pendingActions && VM.Session is { IsInvalid: false } && VM.SessionState == PinState.Ready)
        {
            _pendingActions = false;
            var st = VM.RunActions(in _launch);
            if (st != PinStatus.Ok && _exitWhenDone)
            {
                _allowClose = true;
                Close();
            }
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
        RunPendingActionsIfReady();
        UpdateTaskbar();
        UpdateKeepAwake();

        if (_finalizingForClose && VM.SessionState is PinState.Ready or PinState.Error or PinState.Closed)
        {
            FinishClose();
        }
    }

    // ================================================================== preview frame fit

    private int _fitDarNum = 4, _fitDarDen = 3;

    private void PreviewHost_SizeChanged(object sender, SizeChangedEventArgs e) => UpdatePreviewFrame();

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

    private void UpdateTaskbar()
    {
        TaskbarProgressState state;
        int value = -1;
        if (VM.SessionState == PinState.Error)
        {
            state = TaskbarProgressState.Error;
        }
        else if (!VM.IsCapturing)
        {
            state = TaskbarProgressState.NoProgress;
        }
        else if (VM.DeckState == PinDeckState.Paused)
        {
            state = TaskbarProgressState.Paused;
            value = VM.TapePercent >= 0 ? VM.TapePercent : 100;
        }
        else if (VM.TapePercent >= 0)
        {
            state = TaskbarProgressState.Normal;
            value = VM.TapePercent;
        }
        else
        {
            state = TaskbarProgressState.Indeterminate;
        }

        if (state != _lastTaskbarState)
        {
            _taskbar.SetState(state);
            _lastTaskbarState = state;
            _lastTaskbarValue = -1;
        }
        if (value >= 0 && value != _lastTaskbarValue)
        {
            _taskbar.SetValue(value);
            _lastTaskbarValue = value;
        }
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

    private void VM_PropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        switch (e.PropertyName)
        {
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
        if (e.Kind == PinEventKind.Done && _exitWhenDone)
        {
            _allowClose = true;
            Close();
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
            VM.StopCapture();
            return;
        }
        await StartCaptureAsync(playFirst: false);
    }

    private async void PlayAndCapture_Click(object sender, RoutedEventArgs e)
    {
        if (VM.IsCapturing)
        {
            // The same button is "Stop capture" while recording.
            VM.StopCapture();
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
        var help = Native.LaunchHelp();
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
        _taskbar.SetState(TaskbarProgressState.NoProgress);
    }
}
