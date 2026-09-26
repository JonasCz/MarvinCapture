using System.Runtime.InteropServices;

namespace PinnacleCapture.Interop;

/// <summary>The few plain Win32 calls the window needs.</summary>
internal static partial class Win32
{
    public const uint ES_CONTINUOUS = 0x80000000;
    public const uint ES_SYSTEM_REQUIRED = 0x00000001;

    [LibraryImport("kernel32.dll")]
    public static partial uint SetThreadExecutionState(uint esFlags);

    [LibraryImport("user32.dll")]
    public static partial uint GetDpiForWindow(nint hwnd);
}
