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
    /// <summary>The file stores a display aspect (raw .dv / .m2t do not: players take it from the video).</summary>
    public bool SupportsAspect => Info.SupportsAspect != 0;

    public FormatItem(PinFormatInfo info)
    {
        Info = info;
        Label = info.Label;
        Extension = info.Extension;
    }

    public override string ToString() => Label;
}

/// <summary>
/// One tab of the "Settings for: DV | HDV" TabView: format (with its settings behind the
/// gear, <see cref="FormatOptions"/>), aspect (disabled for a format that stores none), scene
/// split, idle-stop minutes and pass count.
/// </summary>
public sealed partial class KindSettingsViewModel : ObservableObject
{
    public PinKind Kind { get; }
    public string TabHeader { get; }

    public ObservableCollection<FormatItem> Formats { get; } = new();

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(SplitEnabled), nameof(PassesEnabled), nameof(PassesUsable),
                              nameof(AspectEnabled), nameof(EffectiveAspect), nameof(AspectHelp))]
    private FormatItem? _selectedFormat;

    partial void OnSelectedFormatChanged(FormatItem? value) => FormatOptions.Format = value?.Format;

    /// <summary>The selected format's settings (deinterlace, quality, ...), behind the gear next to the drop-down.</summary>
    public FormatOptionsViewModel FormatOptions { get; }

    /// <summary>A saved or command-line format that the list may not have yet: the H.264 / HEVC
    /// entries only appear once the encoder probe is done (<see cref="RefreshFormats"/>).</summary>
    private PinFormat? _wantedFormat;

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

    /// <summary>Aspect override for this kind (PinAspect order: Auto, 4:3, 16:9): the user's choice, kept
    /// (and saved) while the drop-down is disabled, so it comes back with a format that stores an aspect.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(EffectiveAspect))]
    private int _aspectIndex;

    /// <summary>The drop-down is enabled only for a format that stores an aspect (not raw .dv / .m2t).</summary>
    public bool AspectEnabled => SelectedFormat?.SupportsAspect ?? false;

    /// <summary>The aspect this kind's files and its preview use: the choice, or Auto while the
    /// drop-down is disabled (a disabled control never has an effect).</summary>
    public PinAspect EffectiveAspect => AspectEnabled ? (PinAspect)AspectIndex : PinAspect.Auto;

    /// <summary>The drop-down's tooltip, from the core: what it does for the selected format, or why it is disabled.</summary>
    public string AspectHelp => SelectedFormat is null ? "" : Native.FormatAspectHelp(SelectedFormat.Format);

    public bool SplitEnabled => SelectedFormat?.SupportsSceneSplit ?? false;
    public bool PassesEnabled => SelectedFormat?.SupportsMultiPass ?? false;

    public string SettingsPrefix => Kind == PinKind.Dv ? "dv" : "hdv";

    /// <summary>The list stays empty until <see cref="RefreshFormats"/>: pin_formats() would run the
    /// encoder probe on the UI thread.</summary>
    public KindSettingsViewModel(PinKind kind, string tabHeader, FormatChoices choices)
    {
        Kind = kind;
        TabHeader = tabHeader;
        FormatOptions = new FormatOptionsViewModel(choices, analog: false); // DV and HDV share the DV choices
    }

    /// <summary>(Re)fills the format list (after the encoder probe), keeping the selection or the
    /// wanted format if the list has it, else the kind's default.</summary>
    public void RefreshFormats()
    {
        var keep = SelectedFormat?.Format ?? _wantedFormat;
        Formats.Clear(); // the ComboBox may push a null selection meanwhile
        foreach (var f in Native.Formats(Kind))
        {
            Formats.Add(new FormatItem(f));
        }
        SelectedFormat = Formats.FirstOrDefault(f => f.Format == keep) ?? PickDefault();
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
        _wantedFormat = format;
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
