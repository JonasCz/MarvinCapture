using PinnacleCapture.Interop;

namespace PinnacleCapture.ViewModels;

/// <summary>One row of the device ComboBox: name, id, and a status badge derived from pin_dev_state_t.</summary>
public sealed partial class DeviceItemViewModel
{
    public string Id { get; }
    public string Name { get; }
    public PinDevState State { get; }
    public uint OwnerPid { get; }

    /// <summary>Hubs between the computer and the device (core's pin_device_info_t.hub_depth): 0 direct, &gt;0 behind a hub, -1 unknown.</summary>
    public int HubDepth { get; }

    /// <summary>Plugged in through a USB hub, where it shares bandwidth (advisory; unknown = false).</summary>
    public bool IsBehindHub => HubDepth > 0;

    /// <summary>Line under the id in the picker for a device behind a hub.</summary>
    public string HubLine => "⚠ connected via USB hub";

    /// <summary>The core's advice for a device behind a hub (pin_usb_hub_hint), else null: the entry's tooltip.</summary>
    public string? HubHint => IsBehindHub ? HubHintText.Value : null;

    private static readonly Lazy<string?> HubHintText = new(() =>
    {
        try
        {
            var t = Native.UsbHubHint();
            return t.Length > 0 ? t : null;
        }
        catch (Exception)
        {
            return null;
        }
    });

    /// <summary>Whether this process can open it (free, or already open here; the core's rule).</summary>
    public bool IsUsable => UnavailableReason is null;

    /// <summary>Badge background, resolved from theme resources (so high-contrast themes apply).</summary>
    public Microsoft.UI.Xaml.Media.Brush? StatusBrush =>
        Microsoft.UI.Xaml.Application.Current.Resources.TryGetValue(StatusBrushKey, out var b)
            ? b as Microsoft.UI.Xaml.Media.Brush
            : null;

    public string AutomationName => (UnavailableReason is null
        ? $"{Name}, {ShortId}, {StatusText}"
        : $"{Name}, {ShortId}, {UnavailableReason}") + (IsBehindHub ? ", connected via USB hub" : "");

    public string Serial { get; }

    /// <summary>Stable id shown in the list: the full 1394 GUID once known, else the USB port id.</summary>
    public string ShortId => Serial.Length > 0 ? Serial.ToUpperInvariant() : Id;

    public string DisplayName => $"{Name} ({ShortId})";

    /// <summary>Why this entry can't be picked, or null when it can (shown as the item's tooltip; from the core).</summary>
    public string? UnavailableReason { get; }

    /// <summary>The core's sentence for a failed attempt to open this entry, or null when it can be opened.</summary>
    public string? OpenProblem { get; }

    /// <summary>This window has the device open and is recording from it.</summary>
    public bool IsCapturingHere { get; }

    /// <summary>Badge text ("Capturing", "Ready", "In use", ...; from the core).</summary>
    public string StatusText { get; }

    /// <summary>Semantic colour key consumed by a XAML converter/resource lookup (Success/Caution/Critical/Neutral).</summary>
    public string StatusBrushKey => IsCapturingHere ? "SystemFillColorCriticalBrush" : State switch
    {
        PinDevState.Ready or PinDevState.OpenHere or PinDevState.InUse => "ControlAltFillColorQuarternaryBrush",
        PinDevState.Preparing => "SystemFillColorCautionBackgroundBrush",
        PinDevState.NoDriver or PinDevState.Unsupported => "SystemFillColorCriticalBrush",
        _ => "SystemFillColorNeutralBrush",
    };

    /// <summary>Badge text: grey on the neutral pills, white on the red ones.</summary>
    public Microsoft.UI.Xaml.Media.Brush? StatusForeground =>
        Microsoft.UI.Xaml.Application.Current.Resources.TryGetValue(
            StatusBrushKey == "SystemFillColorCriticalBrush" ? "TextOnAccentFillColorPrimaryBrush" : "TextFillColorSecondaryBrush",
            out var b) ? b as Microsoft.UI.Xaml.Media.Brush : null;

    public DeviceItemViewModel(PinDeviceInfo info, bool capturingHere = false)
    {
        IsCapturingHere = capturingHere;
        Id = info.Id;
        // The core flags models it drives but nobody verified on real hardware as "(untested)".
        Name = Native.DeviceDisplayName(in info);
        StatusText = Native.DeviceStatusText(in info, capturingHere);
        UnavailableReason = Native.DeviceUnavailableReason(in info);
        OpenProblem = Native.DeviceOpenProblem(in info);
        State = info.State;
        OwnerPid = info.OwnerPid;
        Serial = info.Serial;
        HubDepth = info.HubDepth;
    }
}
