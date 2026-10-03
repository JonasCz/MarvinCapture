using Microsoft.Win32.SafeHandles;

namespace PinnacleCapture.Interop;

/// <summary>Wraps a `pin_script_t*` (a parsed command line); pin_script_free runs once on dispose.</summary>
public sealed class PinScriptHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    public PinScriptHandle() : base(true)
    {
    }

    internal PinScriptHandle(nint preexistingHandle) : base(true)
    {
        SetHandle(preexistingHandle);
    }

    protected override bool ReleaseHandle()
    {
        Native.pin_script_free_raw(handle);
        return true;
    }
}
