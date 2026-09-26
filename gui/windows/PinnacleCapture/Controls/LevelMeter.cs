using System;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;

namespace PinnacleCapture.Controls;

/// <summary>
/// Thin horizontal audio level meter: a bar for the current peak (dBFS) and a
/// tick for the held peak. Plain shapes, no template, so it costs nothing to
/// update at the status-bar rate.
/// </summary>
public sealed partial class LevelMeter : Grid
{
    public const double FloorDb = -60;

    private readonly Rectangle _bar = new() { HorizontalAlignment = HorizontalAlignment.Left, RadiusX = 1.5, RadiusY = 1.5 };
    private readonly Rectangle _tick = new() { HorizontalAlignment = HorizontalAlignment.Left, Width = 2 };

    public static readonly DependencyProperty LevelProperty =
        DependencyProperty.Register(nameof(Level), typeof(double), typeof(LevelMeter), new PropertyMetadata(FloorDb, OnChanged));

    public static readonly DependencyProperty HoldProperty =
        DependencyProperty.Register(nameof(Hold), typeof(double), typeof(LevelMeter), new PropertyMetadata(FloorDb, OnChanged));

    public double Level
    {
        get => (double)GetValue(LevelProperty);
        set => SetValue(LevelProperty, value);
    }

    public double Hold
    {
        get => (double)GetValue(HoldProperty);
        set => SetValue(HoldProperty, value);
    }

    public LevelMeter()
    {
        Height = 4;
        VerticalAlignment = VerticalAlignment.Center;
        CornerRadius = new CornerRadius(2);
        Background = Brush("ControlAltFillColorQuarternaryBrush");
        _bar.Fill = Brush("AccentFillColorDefaultBrush");
        Children.Add(_bar);
        Children.Add(_tick);
        SizeChanged += (_, _) => Update();
        ActualThemeChanged += (_, _) =>
        {
            Background = Brush("ControlAltFillColorQuarternaryBrush");
            _bar.Fill = Brush("AccentFillColorDefaultBrush");
            Update();
        };
    }

    private static Brush? Brush(string key) =>
        Application.Current.Resources.TryGetValue(key, out var b) ? b as Brush : null;

    private static void OnChanged(DependencyObject d, DependencyPropertyChangedEventArgs e) => ((LevelMeter)d).Update();

    private static double Fraction(double db) => Math.Clamp((db - FloorDb) / -FloorDb, 0, 1);

    private void Update()
    {
        double w = ActualWidth;
        if (w <= 0)
        {
            return;
        }
        _bar.Width = Fraction(Level) * w;
        double hold = Fraction(Hold);
        _tick.Visibility = Hold <= FloorDb ? Visibility.Collapsed : Visibility.Visible;
        _tick.Margin = new Thickness(Math.Max(0, hold * w - _tick.Width), 0, 0, 0);
        // The tick turns red when the last 10 s came within 1 dB of clipping.
        _tick.Fill = Brush(Hold > -1 ? "SystemFillColorCriticalBrush" : "TextFillColorPrimaryBrush");
    }
}
