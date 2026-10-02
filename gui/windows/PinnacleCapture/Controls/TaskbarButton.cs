using System;
using System.Runtime.InteropServices;
using PinnacleCapture.Interop;

namespace PinnacleCapture.Controls;

public enum TaskbarProgressState
{
    NoProgress = 0,
    Indeterminate = 0x1,
    Normal = 0x2,
    Error = 0x4,
    Paused = 0x8,
}

/// <summary>
/// Everything the window shows on its taskbar button, through ITaskbarList3: the progress bar,
/// the "capturing" overlay dot, the flash, the thumbnail toolbar (Start / Stop) and the thumbnail
/// clip. What to show (progress mode and fraction) is decided by the
/// core (pin_status_progress); this class only talks to the shell and skips calls when nothing
/// visible changed.
///
/// The window is subclassed (comctl32 SetWindowSubclass; WinUI 3 has no WndProc hook) to see
/// the "TaskbarButtonCreated" message. The shell forgets everything set on a button when
/// Explorer restarts and then sends that message again, so all cached state is dropped and the
/// next update re-applies it.
/// </summary>
public sealed class TaskbarButton : IDisposable
{
    [ComImport]
    [Guid("56FDF344-FD6D-11D0-958A-006097C9A090")]
    private class TaskbarInstance
    {
    }

    [ComImport]
    [Guid("EA1AFB91-9E28-4B86-90E9-9E9F8A5EEFAF")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface ITaskbarList3
    {
        // ITaskbarList
        void HrInit();
        void AddTab(nint hwnd);
        void DeleteTab(nint hwnd);
        void ActivateTab(nint hwnd);
        void SetActiveAlt(nint hwnd);

        // ITaskbarList2
        void MarkFullscreenWindow(nint hwnd, [MarshalAs(UnmanagedType.Bool)] bool fullscreen);

        // ITaskbarList3: the vtable order matters, so every member up to the last one we call is declared
        void SetProgressValue(nint hwnd, ulong completed, ulong total);
        void SetProgressState(nint hwnd, TaskbarProgressState state);
        void RegisterTab(nint hwndTab, nint hwndMdi);
        void UnregisterTab(nint hwndTab);
        void SetTabOrder(nint hwndTab, nint hwndInsertBefore);
        void SetTabActive(nint hwndTab, nint hwndMdi, uint reserved);
        void ThumbBarAddButtons(nint hwnd, uint count, nint buttons);
        void ThumbBarUpdateButtons(nint hwnd, uint count, nint buttons);
        void ThumbBarSetImageList(nint hwnd, nint imageList);
        void SetOverlayIcon(nint hwnd, nint icon, [MarshalAs(UnmanagedType.LPWStr)] string? description);
        void SetThumbnailTooltip(nint hwnd, [MarshalAs(UnmanagedType.LPWStr)] string? tip);
        void SetThumbnailClip(nint hwnd, nint clip);
    }

    private const string ButtonCreatedMessage = "TaskbarButtonCreated";
    private const nuint SubclassId = 0x50494E; // "PIN"

    private readonly ITaskbarList3? _taskbar;
    private readonly nint _hwnd;
    private readonly uint _buttonCreatedMsg;
    private readonly Win32.SubclassProc _proc;
    private readonly nint _procPtr;
    private bool _disposed;
    private readonly nint _scratch = Marshal.AllocHGlobal(ButtonSize * 2 + 16); // two THUMBBUTTONs, then a RECT

    /// <summary>The user clicked "Start capture" / "Stop capture" in the taskbar thumbnail (UI thread, inside the window procedure).</summary>
    public event Action? StartClicked;
    public event Action? StopClicked;

    public TaskbarButton(nint hwnd)
    {
        _hwnd = hwnd;
        try
        {
            _taskbar = (ITaskbarList3)new TaskbarInstance();
            _taskbar.HrInit();
        }
        catch (COMException)
        {
            _taskbar = null;
        }
        catch (InvalidCastException)
        {
            _taskbar = null;
        }

        _buttonCreatedMsg = Win32.RegisterWindowMessageW(ButtonCreatedMessage);
        // An elevated app would otherwise never see the message from the (non-elevated) shell.
        Win32.ChangeWindowMessageFilterEx(hwnd, _buttonCreatedMsg, Win32.MSGFLT_ALLOW, 0);
        _proc = WndProc;
        _procPtr = Marshal.GetFunctionPointerForDelegate(_proc);
        Win32.SetWindowSubclass(hwnd, _procPtr, SubclassId, 0);
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }
        _disposed = true;
        Win32.RemoveWindowSubclass(_hwnd, _procPtr, SubclassId);
        if (_dotIcon != 0)
        {
            Win32.DestroyIcon(_dotIcon);
            _dotIcon = 0;
        }
        DestroyButtonIcons(_startIcon, _stopIcon);
        _startIcon = _stopIcon = 0;
        Marshal.FreeHGlobal(_scratch);
    }

    private nint WndProc(nint hwnd, uint msg, nuint wParam, nint lParam, nuint id, nuint data)
    {
        try
        {
            if (msg == _buttonCreatedMsg)
            {
                ForgetShellState();
            }
            else if (msg == Win32.WM_COMMAND && (uint)((ulong)wParam >> 16 & 0xFFFF) == Win32.THBN_CLICKED)
            {
                uint button = (uint)((ulong)wParam & 0xFFFF);
                if (button == StartButtonId)
                {
                    StartClicked?.Invoke();
                }
                else if (button == StopButtonId)
                {
                    StopClicked?.Invoke();
                }
                return 0;
            }
            else if (msg == Win32.WM_DPICHANGED
                     || (msg == Win32.WM_SETTINGCHANGE && lParam != 0
                         && Marshal.PtrToStringUni(lParam) == "ImmersiveColorSet"))
            {
                _iconsStale = true; // glyph colour follows the taskbar theme, size follows the DPI
            }
        }
        catch (Exception)
        {
            // never let an exception cross the native window procedure
        }
        return Win32.DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    /// <summary>The shell has a fresh button (first time, or Explorer restarted): everything must be applied again.</summary>
    private void ForgetShellState()
    {
        _mode = (PinProgressMode)(-1);
        _percent = -2;
        _overlayOn = null;
        _buttonsAdded = false;
        _clip = null;
    }

    // ================================================================== progress

    private PinProgressMode _mode = PinProgressMode.None;
    private int _percent = -1;

    /// <summary>Shows mode / fraction (0..1); cheap to call on every status tick.</summary>
    public void SetProgress(PinProgressMode mode, double fraction)
    {
        if (_taskbar is null)
        {
            return;
        }
        int percent = mode is PinProgressMode.None or PinProgressMode.Indeterminate
            ? -1 : (int)Math.Round(Math.Clamp(fraction, 0, 1) * 100);
        // Value first for a mode that shows one, so a bar never flashes at its old value.
        try
        {
            if (mode != _mode)
            {
                if (percent >= 0)
                {
                    _taskbar.SetProgressValue(_hwnd, (ulong)percent, 100);
                }
                _taskbar.SetProgressState(_hwnd, mode switch
                {
                    PinProgressMode.Indeterminate => TaskbarProgressState.Indeterminate,
                    PinProgressMode.Normal => TaskbarProgressState.Normal,
                    PinProgressMode.Paused => TaskbarProgressState.Paused,
                    PinProgressMode.Error => TaskbarProgressState.Error,
                    _ => TaskbarProgressState.NoProgress,
                });
                _mode = mode;
                _percent = percent;
            }
            else if (percent != _percent)
            {
                _taskbar.SetProgressValue(_hwnd, (ulong)percent, 100);
                _percent = percent;
            }
        }
        catch (COMException)
        {
            // Explorer restarted or no taskbar button yet: not cached, so the next tick retries.
        }
    }

    // ================================================================== thumbnail toolbar

    private const uint StartButtonId = 1, StopButtonId = 2;
    private const int ButtonSize = 552;     // sizeof(THUMBBUTTON) on x64: 4+4+4 (+4) +8 +520 +4 (+4)
    private const uint THB_ICON = 0x2, THB_TOOLTIP = 0x4, THB_FLAGS = 0x8;
    private const uint THBF_ENABLED = 0, THBF_DISABLED = 1;

    private nint _startIcon, _stopIcon;
    private bool _buttonsAdded, _iconsStale;
    private bool? _appliedStart, _appliedStop;
    private bool _wantStart, _wantStop;

    /// <summary>
    /// "Start capture" and "Stop capture" buttons under the taskbar thumbnail. They can only be added
    /// once the shell has created the taskbar button (the first call may fail; the next tick retries).
    /// The icons are Segoe Fluent / MDL2 glyphs drawn at run time. Cheap to call on every tick.
    /// </summary>
    public void SetControls(bool startEnabled, bool stopEnabled)
    {
        _wantStart = startEnabled;
        _wantStop = stopEnabled;
        if (_taskbar is null || _disposed || IntPtr.Size != 8)
        {
            return;
        }
        if (_buttonsAdded && !_iconsStale && _appliedStart == startEnabled && _appliedStop == stopEnabled)
        {
            return;
        }
        nint oldStart = 0, oldStop = 0;
        try
        {
            if (_startIcon == 0 || _stopIcon == 0 || _iconsStale)
            {
                uint dpi = Win32.GetDpiForWindow(_hwnd);
                int size = Math.Max(16, Win32.GetSystemMetricsForDpi(Win32.SM_CXSMICON, dpi == 0 ? 96 : dpi));
                oldStart = _startIcon;
                oldStop = _stopIcon;
                _startIcon = TaskbarIcons.CreateGlyph("", size); // Download
                _stopIcon = TaskbarIcons.CreateGlyph("", size);  // Stop
                _iconsStale = false;
            }
            WriteButton(_scratch, StartButtonId, _startIcon, "Start capture", startEnabled);
            WriteButton(_scratch + ButtonSize, StopButtonId, _stopIcon, "Stop capture", stopEnabled);
            if (_buttonsAdded)
            {
                _taskbar.ThumbBarUpdateButtons(_hwnd, 2, _scratch);
            }
            else
            {
                _taskbar.ThumbBarAddButtons(_hwnd, 2, _scratch);
                _buttonsAdded = true;
            }
            _appliedStart = startEnabled;
            _appliedStop = stopEnabled;
        }
        catch (COMException)
        {
            // no taskbar button yet: retried by the next tick
        }
        finally
        {
            DestroyButtonIcons(oldStart, oldStop);
        }
    }

    private static void WriteButton(nint p, uint id, nint icon, string tip, bool enabled)
    {
        for (int i = 0; i < ButtonSize; i++)
        {
            Marshal.WriteByte(p, i, 0);
        }
        Marshal.WriteInt32(p, 0, (int)(THB_ICON | THB_TOOLTIP | THB_FLAGS)); // dwMask
        Marshal.WriteInt32(p, 4, (int)id);                                   // iId
        Marshal.WriteIntPtr(p, 16, icon);                                    // hIcon
        for (int i = 0; i < tip.Length && i < 259; i++)
        {
            Marshal.WriteInt16(p, 24 + i * 2, tip[i]);                       // szTip[260]
        }
        Marshal.WriteInt32(p, 544, (int)(enabled ? THBF_ENABLED : THBF_DISABLED)); // dwFlags
    }

    private static void DestroyButtonIcons(nint a, nint b)
    {
        if (a != 0) Win32.DestroyIcon(a);
        if (b != 0) Win32.DestroyIcon(b);
    }

    // ================================================================== thumbnail clip

    private (int L, int T, int R, int B)? _clip;

    /// <summary>
    /// Shows only this rectangle of the window (client pixels) in the taskbar thumbnail, or the
    /// whole window for null. Only calls the shell when the rectangle changed.
    /// </summary>
    public void SetThumbnailClip((int L, int T, int R, int B)? clip)
    {
        if (_taskbar is null || _disposed || clip == _clip)
        {
            return;
        }
        try
        {
            if (clip is { } c)
            {
                nint r = _scratch + ButtonSize * 2;
                Marshal.WriteInt32(r, 0, c.L);
                Marshal.WriteInt32(r, 4, c.T);
                Marshal.WriteInt32(r, 8, c.R);
                Marshal.WriteInt32(r, 12, c.B);
                _taskbar.SetThumbnailClip(_hwnd, r);
            }
            else
            {
                _taskbar.SetThumbnailClip(_hwnd, 0);
            }
            _clip = clip;
        }
        catch (COMException)
        {
            // no button yet: retried by the next update
        }
    }

    // ================================================================== attention

    /// <summary>
    /// Flashes the taskbar button (not the caption) until the window is brought to the front, but
    /// only if it is not the foreground window right now.
    /// </summary>
    public void FlashIfInBackground()
    {
        if (_disposed || Win32.GetForegroundWindow() == _hwnd)
        {
            return;
        }
        var info = new Win32.FLASHWINFO
        {
            CbSize = (uint)Marshal.SizeOf<Win32.FLASHWINFO>(),
            Hwnd = _hwnd,
            Flags = Win32.FLASHW_TRAY | Win32.FLASHW_TIMERNOFG,
            Count = uint.MaxValue, // with TIMERNOFG: until the window comes to the foreground
        };
        Win32.FlashWindowEx(ref info);
    }

    // ================================================================== overlay icon

    private bool? _overlayOn;
    private nint _dotIcon;
    private int _dotSize;

    /// <summary>
    /// Red dot at the lower right of the taskbar button while a capture runs (accessible name
    /// "Capturing"); cleared when it ends. The icon is drawn at the small-icon size of the
    /// window's current DPI.
    /// </summary>
    public void SetCapturing(bool capturing)
    {
        if (_taskbar is null || _overlayOn == capturing)
        {
            return;
        }
        try
        {
            if (capturing)
            {
                uint dpi = Win32.GetDpiForWindow(_hwnd);
                int size = Math.Max(16, Win32.GetSystemMetricsForDpi(Win32.SM_CXSMICON, dpi == 0 ? 96 : dpi));
                if (_dotIcon == 0 || _dotSize != size)
                {
                    if (_dotIcon != 0)
                    {
                        Win32.DestroyIcon(_dotIcon);
                    }
                    _dotIcon = TaskbarIcons.CreateCaptureDot(size);
                    _dotSize = size;
                }
                if (_dotIcon == 0)
                {
                    return;
                }
                _taskbar.SetOverlayIcon(_hwnd, _dotIcon, "Capturing");
            }
            else
            {
                _taskbar.SetOverlayIcon(_hwnd, 0, null);
            }
            _overlayOn = capturing;
        }
        catch (COMException)
        {
            // no button yet: not cached, the next tick retries
        }
    }
}
