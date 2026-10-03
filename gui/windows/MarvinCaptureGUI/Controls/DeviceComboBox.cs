using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using PinnacleCapture.ViewModels;

namespace PinnacleCapture.Controls;

/// <summary>
/// Device ComboBox whose entries for devices this window can't open (in use
/// by another window, being prepared, no driver, unsupported) are disabled
/// containers: shown greyed out, not selectable by mouse or keyboard, with
/// the reason as tooltip and automation help text. WinUI has no bindable
/// "item enabled" hook, so it is done where the container is prepared.
/// Uses the stock ComboBox template (DefaultStyleKey stays ComboBox).
/// </summary>
public sealed partial class DeviceComboBox : ComboBox
{
    protected override void PrepareContainerForItemOverride(DependencyObject element, object item)
    {
        base.PrepareContainerForItemOverride(element, item);
        if (element is ComboBoxItem container && item is DeviceItemViewModel device)
        {
            var reason = device.UnavailableReason;
            container.IsEnabled = reason is null;
            // An enabled entry shows the USB hub advice from its template; a disabled one can't,
            // so its tooltip carries both.
            ToolTipService.SetToolTip(container, reason is not null && device.HubHint is not null
                ? $"{reason}\n\n{device.HubHint}" : reason);
            AutomationProperties.SetHelpText(container, reason ?? string.Empty);
        }
    }
}
