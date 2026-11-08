using System;
using System.Runtime.InteropServices;
using PinnacleCapture.Interop;

namespace PinnacleCapture.Controls;

/// <summary>
/// Logoff, shutdown and restart. While a capture runs (or its files are being finalised) the
/// window blocks the session end like an editor with unsaved work: a shutdown block reason is
/// registered and WM_QUERYENDSESSION is answered FALSE, so Windows shows "this app is preventing
/// shutdown". If the session ends anyway (WM_ENDSESSION with wParam TRUE: nothing blocked it, or
/// the user chose "Shut down anyway"), the release callback runs on the UI thread before the
/// message returns, because Windows ends the process right after. The window is subclassed
/// (comctl32 SetWindowSubclass; WinUI 3 has no WndProc hook).
/// </summary>
public sealed class SessionEndGuard : IDisposable
{
    private const nuint SubclassId = 0x454E44; // "END"
    private const string BlockReason = "A capture is in progress. Stop it before shutting down.";

    private readonly nint _hwnd;
    private readonly Func<bool> _isBusy;
    private readonly Action _release;
    private readonly Win32.SubclassProc _proc;
    private readonly nint _procPtr;
    private bool _blocking;
    private bool _disposed;

    /// <param name="isBusy">A capture is running or its files are being finalised.</param>
    /// <param name="release">Stops any capture, finalises the files and releases the device (blocks until done).</param>
    public SessionEndGuard(nint hwnd, Func<bool> isBusy, Action release)
    {
        _hwnd = hwnd;
        _isBusy = isBusy;
        _release = release;
        _proc = WndProc;
        _procPtr = Marshal.GetFunctionPointerForDelegate(_proc);
        Win32.SetWindowSubclass(hwnd, _procPtr, SubclassId, 0);
    }

    /// <summary>Registers or withdraws the shutdown block reason; cheap when nothing changes.</summary>
    public void SetBusy(bool busy)
    {
        if (_disposed || busy == _blocking)
        {
            return;
        }
        _blocking = busy;
        if (busy)
        {
            Win32.ShutdownBlockReasonCreate(_hwnd, BlockReason);
        }
        else
        {
            Win32.ShutdownBlockReasonDestroy(_hwnd);
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }
        SetBusy(false);
        _disposed = true;
        Win32.RemoveWindowSubclass(_hwnd, _procPtr, SubclassId);
    }

    private nint WndProc(nint hwnd, uint msg, nuint wParam, nint lParam, nuint id, nuint data)
    {
        try
        {
            if (msg == Win32.WM_QUERYENDSESSION)
            {
                if (_isBusy())
                {
                    SetBusy(true);
                    return 0; // FALSE: do not end the session yet
                }
            }
            else if (msg == Win32.WM_ENDSESSION && wParam != 0)
            {
                _release();
                return 0;
            }
        }
        catch (Exception)
        {
            // never let an exception cross the native window procedure
        }
        return Win32.DefSubclassProc(hwnd, msg, wParam, lParam);
    }
}
