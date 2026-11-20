using Microsoft.UI.Xaml.Controls;
using PinnacleCapture.ViewModels;

namespace PinnacleCapture.Views;

/// <summary>The Flyout content of the retry check box's gear button: how the retry works.</summary>
public sealed partial class RetryOptionsView : UserControl
{
    public RetryOptionsView(RetryOptionsViewModel options)
    {
        Options = options;
        InitializeComponent();
    }

    public RetryOptionsViewModel Options { get; }
}
