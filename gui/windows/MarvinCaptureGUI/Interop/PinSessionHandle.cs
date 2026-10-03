using System;
using Microsoft.Win32.SafeHandles;

namespace PinnacleCapture.Interop;

/// <summary>
/// Wraps a `pin_session_t*`. pin_open() hands back a raw pointer; this
/// SafeHandle takes ownership of it and guarantees pin_close() runs exactly
/// once, even on an unhandled exception or finalization, matching the
/// "Blocks until done. Safe with NULL." contract documented on pin_close.
/// </summary>
public sealed class PinSessionHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    public PinSessionHandle() : base(true)
    {
    }

    internal PinSessionHandle(nint preexistingHandle, bool ownsHandle) : base(ownsHandle)
    {
        SetHandle(preexistingHandle);
    }

    protected override bool ReleaseHandle()
    {
        Native.pin_close_raw(handle);
        return true;
    }
}
