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
    [NotifyPropertyChangedFor(nameof(TitleEnabled), nameof(SplitEnabled), nameof(PassesEnabled))]
    private FormatItem? _selectedFormat;

    [ObservableProperty] private string _title = "";
    [ObservableProperty] private bool _splitIntoScenes;
    [ObservableProperty] private double _idleStopMinutes = 5;
    [ObservableProperty] private double _passes = 1;

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
