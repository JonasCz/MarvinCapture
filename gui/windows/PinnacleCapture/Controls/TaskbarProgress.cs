using System;
using System.Runtime.InteropServices;

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
/// Minimal ITaskbarList3 wrapper for the capture progress badge on the
/// taskbar button: indeterminate while capturing without a tape-percent
/// report, a real progress bar once the deck answers a counter inquiry,
/// red on error, yellow while paused (per the plan's "Changes after your
/// review" / Taskbar section).
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

    public void SetState(TaskbarProgressState state)
    {
        try
        {
            _taskbar?.SetProgressState(_hwnd, state);
        }
        catch (COMException)
        {
            // Explorer restarted, or no taskbar button yet; not fatal.
        }
    }

    public void SetValue(int percent0To100)
    {
        try
        {
            _taskbar?.SetProgressValue(_hwnd, (ulong)Math.Clamp(percent0To100, 0, 100), 100);
        }
        catch (COMException)
        {
        }
    }
}
