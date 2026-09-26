using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using PinnacleCapture.Interop;

namespace PinnacleCapture.ViewModels;

/// <summary>One analog proc-amp slider (brightness/contrast/.../audio gain), sourced from pin_get_control.</summary>
public sealed partial class ControlSliderViewModel : ObservableObject
{
    public PinControl Control { get; }
    private readonly System.Action<PinControl, int> _onChanged;

    [ObservableProperty] private string _label = "";
    [ObservableProperty] private int _min;
    [ObservableProperty] private int _max;
    [ObservableProperty] private int _step = 1;
    [ObservableProperty] private int _def;
    [ObservableProperty] private int _value;
    [ObservableProperty] private bool _isEnabled = true;
    [ObservableProperty] private bool _isDb;

    /// <summary>Display text: audio gain is dB*10 in the API, shown as one decimal of dB.</summary>
    public string ValueText => IsDb ? $"{Value / 10.0:0.0} dB" : Value.ToString();

    private bool _suppressChange;

    public ControlSliderViewModel(PinControl control, System.Action<PinControl, int> onChanged)
    {
        Control = control;
        _onChanged = onChanged;
    }

    public void LoadFrom(in PinControlInfo info)
    {
        _suppressChange = true;
        Label = info.Label;
        Min = info.Min;
        Max = info.Max;
        Step = info.Step <= 0 ? 1 : info.Step;
        Def = info.Def;
        Value = info.Value;
        IsEnabled = info.Enabled != 0;
        _suppressChange = false;
        OnPropertyChanged(nameof(ValueText));
    }

    /// <summary>Slider.Value is a double; this adapts it to the integer API value.</summary>
    public double SliderValue
    {
        get => Value;
        set => Value = (int)System.Math.Round(value);
    }

    public string ResetName => $"Reset {Label} to default";

    partial void OnLabelChanged(string value) => OnPropertyChanged(nameof(ResetName));

    partial void OnValueChanged(int value)
    {
        OnPropertyChanged(nameof(ValueText));
        OnPropertyChanged(nameof(SliderValue));
        if (!_suppressChange)
        {
            _onChanged(Control, value);
        }
    }

    [RelayCommand]
    private void ResetToDefault() => Value = Def;
}
