using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using PinnacleCapture.Interop;

namespace PinnacleCapture.ViewModels;

/// <summary>
/// Bindable wrapper around pin_format_info_t. XAML bindings need a
/// reference type with plain properties; the raw struct (with fixed
/// buffers) stays inside.
/// </summary>
public sealed partial class FormatItem
{
    public PinFormatInfo Info { get; }
    public PinFormat Format => Info.Format;
    public string Label { get; }
    public string Extension { get; }
    public bool SupportsTitle => Info.SupportsTitle != 0;
    public bool SupportsSceneSplit => Info.SupportsSceneSplit != 0;
    public bool SupportsMultiPass => Info.SupportsMultiPass != 0;
    public bool IsDefault => Info.IsDefault != 0;

    public FormatItem(PinFormatInfo info)
    {
        Info = info;
        Label = info.Label;
        Extension = info.Extension;
    }

    public override string ToString() => Label;
}

/// <summary>
/// One tab of the "Settings for: DV | HDV" TabView: format, title (disabled
/// per format capability), scene split, idle-stop minutes and pass count.
/// </summary>
public sealed partial class KindSettingsViewModel : ObservableObject
{
    public PinKind Kind { get; }
    public string TabHeader { get; }

    public ObservableCollection<FormatItem> Formats { get; } = new();

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(TitleEnabled), nameof(SplitEnabled), nameof(PassesEnabled), nameof(PassesUsable))]
    private FormatItem? _selectedFormat;

    [ObservableProperty] private bool _splitIntoScenes;
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(PassesAllowed), nameof(PassesUsable), nameof(DisplayPasses), nameof(PassesToolTip))]
    private double _idleStopMinutes = 5;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(PassesAllowed), nameof(PassesUsable), nameof(DisplayPasses), nameof(PassesToolTip))]
    private double _maxDurationMinutes;

    /// <summary>The user's own pass count; kept (and saved) while the entry is greyed out, so it comes back.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(DisplayPasses), nameof(EffectivePasses))]
    private double _passes = 1;

    /// <summary>The core's rule: passes need the no-signal timeout or the "stop after" limit.</summary>
    public bool PassesAllowed => Native.PassesAllowed(IdleStopMinutes, MaxDurationMinutes);

    /// <summary>Entry enabled (before the editable-while-idle check): format supports passes and a limit is set.</summary>
    public bool PassesUsable => PassesEnabled && PassesAllowed;

    /// <summary>What the entry shows: 1 while passes are not allowed, else the user's value.</summary>
    public double DisplayPasses
    {
        get => PassesAllowed ? Passes : 1;
        set
        {
            if (PassesAllowed)
            {
                Passes = value;
            }
        }
    }

    /// <summary>Pass count to capture with (what the core would also enforce).</summary>
    public int EffectivePasses => PassesAllowed ? (int)Math.Max(1, double.IsNaN(Passes) ? 1 : Passes) : 1;

    public string PassesToolTip => PassesAllowed
        ? "1 captures the tape once. More passes rewind to the start of the tape and capture it again. Only for Automatic rewind & capture: each new pass rewinds the tape, so Manual capture always captures one pass."
        : "Needs \"Stop no signal\" or \"Stop after\" to be set: multi-pass needs a way to detect the end of a pass. Set one of them to enable more than one pass. Only for Automatic rewind & capture: each new pass rewinds the tape, so Manual capture always captures one pass.";

    /// <summary>Aspect override for this kind (PinAspect order: Auto, 4:3, 16:9).</summary>
    [ObservableProperty] private int _aspectIndex;

    public bool TitleEnabled => SelectedFormat?.SupportsTitle ?? false;
    public bool SplitEnabled => SelectedFormat?.SupportsSceneSplit ?? false;
    public bool PassesEnabled => SelectedFormat?.SupportsMultiPass ?? false;

    public string SettingsPrefix => Kind == PinKind.Dv ? "dv" : "hdv";

    public KindSettingsViewModel(PinKind kind, string tabHeader)
    {
        Kind = kind;
        TabHeader = tabHeader;
        foreach (var f in Native.Formats(kind))
        {
            Formats.Add(new FormatItem(f));
        }
        _selectedFormat = PickDefault();
    }

    private FormatItem? PickDefault()
    {
        foreach (var f in Formats)
        {
            if (f.IsDefault)
            {
                return f;
            }
        }
        return Formats.Count > 0 ? Formats[0] : null;
    }

    public void ApplyFormat(PinFormat format)
    {
        foreach (var f in Formats)
        {
            if (f.Format == format)
            {
                SelectedFormat = f;
                return;
            }
        }
    }
}
