using CommunityToolkit.Mvvm.ComponentModel;
using PinnacleCapture.Interop;

namespace PinnacleCapture.ViewModels;

/// <summary>
/// The format settings the user chose: deinterlace and quality per input kind (analog, DV; HDV
/// shares the DV ones), FFV1 compression once. -1 = the format's own default (pin_format_options).
/// Stored per device by MainViewModel under the Android app's keys: gui.deinterlace_analog / _dv,
/// gui.quality_analog / _dv, gui.ffv1_compression.
/// </summary>
public sealed partial class FormatChoices : ObservableObject
{
    [ObservableProperty] private int _deinterlaceAnalog = -1;
    [ObservableProperty] private int _deinterlaceDv = -1;
    [ObservableProperty] private int _qualityAnalog = -1;
    [ObservableProperty] private int _qualityDv = -1;
    [ObservableProperty] private int _ffv1Compression = -1;

    public int DeinterlaceFor(bool analog) => analog ? DeinterlaceAnalog : DeinterlaceDv;
    public int QualityFor(bool analog) => analog ? QualityAnalog : QualityDv;

    public void SetDeinterlace(bool analog, int value)
    {
        if (analog)
        {
            DeinterlaceAnalog = value;
        }
        else
        {
            DeinterlaceDv = value;
        }
    }

    public void SetQuality(bool analog, int value)
    {
        if (analog)
        {
            QualityAnalog = value;
        }
        else
        {
            QualityDv = value;
        }
    }
}

/// <summary>
/// The settings of one output format, behind the gear next to its drop-down (FormatOptionsView in
/// a Flyout): everything comes from pin_format_options() for the selected format (which parts
/// apply, labels, range, defaults, help texts), the warning next to the drop-down from
/// pin_format_warning(), the data rate line from pin_format_quality_hint(). The values live in
/// <see cref="FormatChoices"/>; this presents them for one drop-down.
/// </summary>
public sealed partial class FormatOptionsViewModel : ObservableObject
{
    private readonly FormatChoices _choices;
    private readonly bool _analog;  // analog's choices, or DV's (DV and HDV share them)
    private PinFormat? _format;
    private bool _is60Hz;
    private bool _applying;         // values set from the choices, not by the user

    public FormatOptionsViewModel(FormatChoices choices, bool analog)
    {
        _choices = choices;
        _analog = analog;
        _choices.PropertyChanged += (_, _) => Load();
    }

    /// <summary>The format whose settings are shown (the drop-down's selection), or null.</summary>
    public PinFormat? Format
    {
        get => _format;
        set
        {
            if (_format != value)
            {
                _format = value;
                Load();
            }
        }
    }

    /// <summary>The signal's video standard (status is_60hz), for the data rate line.</summary>
    public bool Is60Hz
    {
        get => _is60Hz;
        set
        {
            if (_is60Hz != value)
            {
                _is60Hz = value;
                UpdateHint();
            }
        }
    }

    /// <summary>False while capturing: the settings apply at capture start.</summary>
    [ObservableProperty] private bool _isEditable = true;

    /// <summary>The format has at least one setting (else the gear is disabled).</summary>
    [ObservableProperty] private bool _hasAny;
    [ObservableProperty] private bool _hasDeinterlace;
    [ObservableProperty] private bool _hasQuality;
    [ObservableProperty] private bool _hasFfv1;

    /// <summary>pin_format_warning(): shown next to the drop-down while the format is selected.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasWarning))]
    private string _warning = "";
    public bool HasWarning => Warning.Length > 0;

    /// <summary>One line for the whole settings panel of this format, or "".</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasNote))]
    private string _note = "";
    public bool HasNote => Note.Length > 0;

    // ---- deinterlace (two choices in PinDeinterlace order)

    [ObservableProperty] private string _deinterlaceLabel = "";
    [ObservableProperty] private string _keepFieldsLabel = "";
    [ObservableProperty] private string _doubleRateLabel = "";
    [ObservableProperty] private string _keepFieldsHelp = "";
    [ObservableProperty] private string _doubleRateHelp = "";

    /// <summary>The RadioButtons selection: a PinDeinterlace.</summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(KeepFieldsChosen), nameof(DoubleRateChosen))]
    private int _deinterlaceIndex = -1;

    /// <summary>"Keep fields" is chosen: its help is shown as a warning (interlaced video loses quality).</summary>
    public bool KeepFieldsChosen => DeinterlaceIndex == (int)PinDeinterlace.KeepFields;
    public bool DoubleRateChosen => DeinterlaceIndex == (int)PinDeinterlace.DoubleRate;

    partial void OnDeinterlaceIndexChanged(int value)
    {
        UpdateHint();
        if (_applying || !HasDeinterlace || value < 0)
        {
            return;
        }
        _choices.SetDeinterlace(_analog, value);
    }

    // ---- quality (a slider from QualityMin to QualityMax, lower is better)

    [ObservableProperty] private string _qualityLabel = "";
    [ObservableProperty] private string _qualityHeader = "";
    [ObservableProperty] private string _qualityLowLabel = "";
    [ObservableProperty] private string _qualityHighLabel = "";
    [ObservableProperty] private string _qualityHelp = "";
    [ObservableProperty] private string _qualityHint = "";
    [ObservableProperty] private double _qualityMin;
    [ObservableProperty] private double _qualityMax = 100;   // the Slider's own default until a format says
    [ObservableProperty] private double _quality;

    partial void OnQualityChanged(double value)
    {
        QualityHeader = QualityLabel.Length > 0 ? $"{QualityLabel}: {QualityValue}" : "";
        UpdateHint();
        if (_applying || !HasQuality)
        {
            return;
        }
        _choices.SetQuality(_analog, QualityValue);
    }

    private int QualityValue => (int)Math.Round(Quality);

    // ---- FFV1 compression (three levels in PinFfv1Compression order)

    [ObservableProperty] private string _ffv1Label = "";
    [ObservableProperty] private string _ffv1FastLabel = "";
    [ObservableProperty] private string _ffv1SmallerLabel = "";
    [ObservableProperty] private string _ffv1SmallestLabel = "";
    [ObservableProperty] private string _ffv1Help = "";

    /// <summary>The RadioButtons selection: a PinFfv1Compression.</summary>
    [ObservableProperty] private int _ffv1Index = -1;

    partial void OnFfv1IndexChanged(int value)
    {
        if (_applying || !HasFfv1 || value < 0)
        {
            return;
        }
        _choices.Ffv1Compression = value;
    }

    // ----

    /// <summary>Re-reads the format's settings from the core and shows the stored choices (or the defaults).</summary>
    private void Load()
    {
        _applying = true;
        try
        {
            PinFormatOptions o = PinFormatOptions.Create();
            bool ok = _format is PinFormat f && Native.FormatOptions(f, out o) == PinStatus.Ok;
            Warning = _format is PinFormat w ? Native.FormatWarning(w) : "";
            HasDeinterlace = ok && o.HasDeinterlace != 0;
            HasQuality = ok && o.HasQuality != 0;
            HasFfv1 = ok && o.HasFfv1Compression != 0;
            HasAny = HasDeinterlace || HasQuality || HasFfv1;
            Note = ok ? o.Note : "";
            if (ok)
            {
                DeinterlaceLabel = o.DeinterlaceLabel;
                KeepFieldsLabel = o.DeinterlaceKeepLabel;
                DoubleRateLabel = o.DeinterlaceDoubleLabel;
                KeepFieldsHelp = o.DeinterlaceKeepHelp;
                DoubleRateHelp = o.DeinterlaceDoubleHelp;
                int d = _choices.DeinterlaceFor(_analog);
                DeinterlaceIndex = d == (int)PinDeinterlace.KeepFields || d == (int)PinDeinterlace.DoubleRate
                    ? d : o.DeinterlaceDefault;

                QualityLabel = o.QualityLabel;
                QualityLowLabel = o.QualityLowLabel;
                QualityHighLabel = o.QualityHighLabel;
                QualityHelp = o.QualityHelp;
                QualityMin = o.QualityMin;
                QualityMax = Math.Max(o.QualityMin, o.QualityMax);
                int q = _choices.QualityFor(_analog);
                Quality = q < 0 ? o.QualityDefault : Math.Max(o.QualityMin, Math.Min(q, o.QualityMax));
                QualityHeader = $"{QualityLabel}: {QualityValue}";

                Ffv1Label = o.Ffv1Label;
                Ffv1FastLabel = o.Ffv1FastLabel;
                Ffv1SmallerLabel = o.Ffv1SmallerLabel;
                Ffv1SmallestLabel = o.Ffv1SmallestLabel;
                Ffv1Help = o.Ffv1Help;
                int c = _choices.Ffv1Compression;
                Ffv1Index = c >= 0 && c < (int)PinFfv1Compression.LevelCount ? c : o.Ffv1Default;
            }
        }
        finally
        {
            _applying = false;
        }
        UpdateHint();
    }

    private void UpdateHint()
    {
        QualityHint = HasQuality && _format is PinFormat f
            ? Native.FormatQualityHint(f, QualityValue, DeinterlaceIndex, _is60Hz)
            : "";
    }
}
