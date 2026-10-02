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
/// Minimal ITaskbarList3 wrapper for the progress badge on the taskbar button.
/// What to show (mode and fraction) is decided by the core
/// (pin_status_progress); this class only talks to the shell and skips calls
/// when nothing visible changed (state, or the value by less than 1 %).
/// </summary>
public sealed class TaskbarProgress
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

        // ITaskbarList3 (only the members this app needs, in interface order)
        void SetProgressValue(nint hwnd, ulong completed, ulong total);
        void SetProgressState(nint hwnd, TaskbarProgressState state);

        // Remaining ITaskbarList3 members omitted deliberately: COM vtable
        // slots after the last one we call are never invoked, so leaving
        // them off the interface is safe as long as we never call past
        // SetProgressState's neighbours. We stop right here.
    }

    private readonly ITaskbarList3? _taskbar;
    private readonly nint _hwnd;

    public TaskbarProgress(nint hwnd)
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
    }

    private PinProgressMode _mode = PinProgressMode.None;
    private int _percent = -1;

    /// <summary>Shows mode / fraction (0..1); cheap to call on every status tick.</summary>
    public void Apply(PinProgressMode mode, double fraction)
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
}
