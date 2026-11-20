using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using PinnacleCapture.ViewModels;

namespace PinnacleCapture.Views;

public sealed partial class KindSettingsView : UserControl
{
    public static readonly DependencyProperty IsEditableProperty =
        DependencyProperty.Register(nameof(IsEditable), typeof(bool), typeof(KindSettingsView), new PropertyMetadata(true));

    public KindSettingsView(KindSettingsViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();
        FormatFlyout.Content = new FormatOptionsView(viewModel.FormatOptions);
        RetryFlyout.Content = new RetryOptionsView(viewModel.RetryOptions);
    }

    public KindSettingsViewModel ViewModel { get; }

    /// <summary>False while capturing: the options apply at capture start and can't change mid-capture.</summary>
    public bool IsEditable
    {
        get => (bool)GetValue(IsEditableProperty);
        set => SetValue(IsEditableProperty, value);
    }

    public string FormatAutomationName => $"{ViewModel.TabHeader} format";
    public string AspectAutomationName => $"{ViewModel.TabHeader} aspect ratio";
    public string FormatSettingsAutomationName => $"{ViewModel.TabHeader} format settings";
    public string RetryAutomationName => $"{ViewModel.TabHeader}: retry bad sections";
    public string RetrySettingsAutomationName => $"{ViewModel.TabHeader} retry settings";

    public bool Both(bool a, bool b) => a && b;

}
