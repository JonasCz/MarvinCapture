using System;
using Microsoft.UI.Xaml.Data;

namespace PinnacleCapture.Controls;

/// <summary>Slider thumb tooltip for the audio gain: the API value is dB * 10.</summary>
public sealed partial class DbThumbConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, string language) =>
        value is double d ? $"{d / 10.0:0.0} dB" : value?.ToString() ?? "";

    public object ConvertBack(object value, Type targetType, object parameter, string language) =>
        throw new NotSupportedException();
}
