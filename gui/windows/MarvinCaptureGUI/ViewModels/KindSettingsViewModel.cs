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
    public bool SupportsRetry => Info.SupportsRetry != 0;
    public bool IsDefault => Info.IsDefault != 0;
    /// <summary>The file stores a display aspect (raw .dv / .m2t do not: players take it from the video).</summary>
    public bool SupportsAspect => Info.SupportsAspect != 0;
    /// <summary>The format always splits into scenes (HDV MOV / MKV): the option is checked and disabled.</summary>
    public bool SceneSplitForced => Info.SceneSplitForced != 0;

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
/// split, the two stops and the retry of bad sections (its settings behind the gear, shared by both
/// tabs: <see cref="RetryOptions"/>).
/// </summary>
public sealed partial class KindSettingsViewModel : ObservableObject
{
    public PinKind Kind { get; }
    public string TabHeader { get; }

    public ObservableCollection<FormatItem> Formats { get; } = new();

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(SplitEnabled), nameof(RetryEnabled),
                              nameof(AspectEnabled), nameof(EffectiveAspect), nameof(AspectHelp),
                              nameof(SplitForced), nameof(SplitChecked), nameof(SplitHelp))]
    private FormatItem? _selectedFormat;

    partial void OnSelectedFormatChanged(FormatItem? value) => FormatOptions.Format = value?.Format;

    /// <summary>The selected format's settings (deinterlace, quality, ...), behind the gear next to the drop-down.</summary>
    public FormatOptionsViewModel FormatOptions { get; }

    /// <summary>A saved or command-line format that the list may not have yet: the H.264 / HEVC
    /// entries only appear once the encoder probe is done (<see cref="RefreshFormats"/>).</summary>
    private PinFormat? _wantedFormat;

    /// <summary>The user's choice; kept (and saved) while a format that always splits shows the option checked.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(SplitChecked))]
    private bool _splitIntoScenes;
    [ObservableProperty]
    private double _idleStopMinutes = 5;

    [ObservableProperty]
    private double _maxDurationMinutes;

    /// <summary>"Retry bad sections": re-read damaged stretches of tape and keep the best of every read.</summary>
    [ObservableProperty]
    private bool _retry;

    /// <summary>How the retry works (the gear next to the check box); one object shared by the DV and HDV tabs.</summary>
    public RetryOptionsViewModel RetryOptions { get; }

    /// <summary>The check box is enabled for a DV / HDV format (all of them retry).</summary>
    public bool RetryEnabled => SelectedFormat?.SupportsRetry ?? false;

    /// <summary>The check box's tooltip, from the core.</summary>
    public string RetryHelp => Native.RetrySettingHelp(Native.RetrySetting.Checkbox);

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

    /// <summary>The selected format always splits into scenes (the core's pin_format_info_t.scene_split_forced).</summary>
    public bool SplitForced => SelectedFormat?.SceneSplitForced ?? false;

    /// <summary>What the check box shows: checked while the format forces it, else the user's choice.</summary>
    public bool SplitChecked
    {
        get => SplitForced || SplitIntoScenes;
        set
        {
            if (!SplitForced)
            {
                SplitIntoScenes = value;
            }
        }
    }

    /// <summary>The check box is enabled for a format with scene split that does not force it.</summary>
    public bool SplitEnabled => (SelectedFormat?.SupportsSceneSplit ?? false) && !SplitForced;

    /// <summary>The format drop-down's tooltip, from the core (AVI / MOV need a clean finish; HDV: MOV / MKV are not truly lossless);
    /// null for none, so no empty tooltip shows.</summary>
    public object? FormatHelp
    {
        get
        {
            var t = Native.FormatKindHelp(Kind);
            return string.IsNullOrEmpty(t) ? null : t;
        }
    }

    /// <summary>The check box's tooltip, from the core: what it does, or why it is always on.</summary>
    public string SplitHelp => SelectedFormat is null ? "" : Native.FormatSceneSplitHelp(SelectedFormat.Format);

    public string SettingsPrefix => Kind == PinKind.Dv ? "dv" : "hdv";

    /// <summary>The list stays empty until <see cref="RefreshFormats"/>: pin_formats() would run the
    /// encoder probe on the UI thread.</summary>
    public KindSettingsViewModel(PinKind kind, string tabHeader, FormatChoices choices, RetryOptionsViewModel retry)
    {
        Kind = kind;
        TabHeader = tabHeader;
        RetryOptions = retry;
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
