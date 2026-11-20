using CommunityToolkit.Mvvm.ComponentModel;
using PinnacleCapture.Interop;

namespace PinnacleCapture.ViewModels;

/// <summary>
/// How the retry of bad sections works (the gear next to the "Retry bad sections" check box), shared
/// by the DV and HDV tabs: attempts, the memory window, the preroll and what damage asks for a
/// re-read, the mode (each section / whole tape / automatic) and where held tape is kept. Ranges,
/// defaults and help texts come from the core (pin_retry_setting_range / _help); stored per device
/// by MainViewModel (gui.retry_attempts, gui.retry_window, gui.retry_preroll, gui.retry_on,
/// gui.retry_mode, gui.retry_store).
/// </summary>
public sealed partial class RetryOptionsViewModel : ObservableObject
{
    public RetryOptionsViewModel()
    {
        (AttemptsMin, AttemptsMax, _) = Native.RetrySettingRange(Native.RetrySetting.Attempts);
        (WindowMin, WindowMax, _) = Native.RetrySettingRange(Native.RetrySetting.WindowSeconds);
        (PrerollMin, PrerollMax, _) = Native.RetrySettingRange(Native.RetrySetting.PrerollSeconds);
        _attempts = Native.RetrySettingRange(Native.RetrySetting.Attempts).Default;
        _windowSeconds = Native.RetrySettingRange(Native.RetrySetting.WindowSeconds).Default;
        _prerollSeconds = Native.RetrySettingRange(Native.RetrySetting.PrerollSeconds).Default;
    }

    public int AttemptsMin { get; }
    public int AttemptsMax { get; }
    public int WindowMin { get; }
    public int WindowMax { get; }
    public int PrerollMin { get; }
    public int PrerollMax { get; }

    [ObservableProperty] private double _attempts;
    [ObservableProperty] private double _windowSeconds;
    [ObservableProperty] private double _prerollSeconds;
    /// <summary>0 = any damage, 1 = uncorrected only (PinRetryOn order).</summary>
    [ObservableProperty] private int _triggerIndex;
    /// <summary>PinRetryMode order: 0 automatic, 1 each section, 2 whole tape.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(WholeTapeChosen))]
    private int _modeIndex;
    /// <summary>PinRetryStorage order: 0 memory, 1 temporary file.</summary>
    [ObservableProperty] private int _storageIndex;

    /// <summary>The whole-tape note shows (less deck wear, written at the end).</summary>
    public bool WholeTapeChosen => ModeIndex == (int)PinRetryMode.WholeTape;

    /// <summary>False while capturing: the settings apply at capture start.</summary>
    [ObservableProperty] private bool _isEditable = true;

    public string AttemptsHelp => Native.RetrySettingHelp(Native.RetrySetting.Attempts);
    public string WindowHelp => Native.RetrySettingHelp(Native.RetrySetting.WindowSeconds);
    public string PrerollHelp => Native.RetrySettingHelp(Native.RetrySetting.PrerollSeconds);
    public string TriggerHelp => Native.RetrySettingHelp(Native.RetrySetting.Trigger);
    public string ModeHelp => Native.RetrySettingHelp(Native.RetrySetting.Mode);
    public string StorageHelp => Native.RetrySettingHelp(Native.RetrySetting.Storage);
    public string WholeTapeNote => Native.RetrySettingHelp(Native.RetrySetting.WholeTapeNote);

    private static int Whole(double v, int fallback) => double.IsNaN(v) ? fallback : (int)Math.Round(v);

    /// <summary>Into the capture options (the core clamps what is out of range).</summary>
    public void ApplyTo(ref PinCaptureOpts o)
    {
        o.RetryAttempts = Whole(Attempts, 0);
        o.RetryWindowS = Whole(WindowSeconds, 0);
        o.RetryPrerollS = Whole(PrerollSeconds, 0);
        o.RetryTrigger = TriggerIndex == 1 ? (int)PinRetryOn.Uncorrected : (int)PinRetryOn.AnyDamage;
        o.RetryMode = ModeIndex is >= 0 and <= 2 ? ModeIndex : (int)PinRetryMode.Auto;
        o.RetryStorage = StorageIndex == 1 ? (int)PinRetryStorage.File : (int)PinRetryStorage.Memory;
    }
}
