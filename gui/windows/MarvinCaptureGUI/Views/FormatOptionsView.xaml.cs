using Microsoft.UI.Xaml.Controls;
using PinnacleCapture.ViewModels;

namespace PinnacleCapture.Views;

/// <summary>The Flyout content of a format drop-down's gear button: the format's settings.</summary>
public sealed partial class FormatOptionsView : UserControl
{
    public FormatOptionsView(FormatOptionsViewModel options)
    {
        Options = options;
        InitializeComponent();
    }

    public FormatOptionsViewModel Options { get; }
}
