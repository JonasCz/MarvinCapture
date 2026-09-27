using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;
using PinnacleCapture.Interop;

namespace PinnacleCapture;

/// <summary>
/// Mirrors every engine event and core log line to the console when the app
/// was started from one. A GUI-subsystem exe gets no console of its own, so:
/// if stdout was redirected (<c>PinnacleCapture.exe &gt; log.txt</c>, or a
/// pipe) that is used as is; otherwise the parent's console, if there is
/// one, is attached. Started from Explorer there is neither and this does
/// nothing. PINNACLE_LOG_LEVEL=0 adds the core's debug lines.
/// </summary>
internal static partial class ConsoleOutput
{
    private static TextWriter? _out;
    private static readonly object _lock = new();

    public static void Init()
    {
        try
        {
            var h = GetStdHandle(StdOutputHandle);
            if (h != 0 && h != -1 && GetFileType(h) != FileTypeUnknown)
            {
                _out = Open(new SafeFileHandle(h, ownsHandle: false));
            }
            else if (AttachConsole(AttachParentProcess))
            {
                var con = CreateFileW("CONOUT$", GenericWrite, FileShareWrite, 0, OpenExisting, 0, 0);
                if (con != -1)
                {
                    _out = Open(new SafeFileHandle(con, ownsHandle: true));
                    _out.WriteLine(); // start below the shell prompt it returned to
                }
            }
        }
        catch
        {
            _out = null;
        }

        try
        {
            if (_out is not null &&
                int.TryParse(Environment.GetEnvironmentVariable("PINNACLE_LOG_LEVEL"), out var level))
            {
                Native.SetLogLevel(level);
            }
        }
        catch
        {
            // core not loadable: App.OnLaunched reports that
        }
    }

    private static TextWriter Open(SafeFileHandle h) =>
        new StreamWriter(new FileStream(h, FileAccess.Write), new UTF8Encoding(false)) { AutoFlush = true };

    public static void WriteEvent(in PinEvent evt)
    {
        if (_out is null)
        {
            return;
        }
        string line = evt.Kind == PinEventKind.Log
            ? $"{LevelName(evt.A)}: {evt.Text.Trim()}"
            : $"event {evt.Kind} a={evt.A}" + (evt.Text.Length > 0 ? $" {evt.Text.Trim()}" : "");
        Write(line);
    }

    public static void Write(string line)
    {
        if (_out is null)
        {
            return;
        }
        lock (_lock)
        {
            try
            {
                _out.WriteLine($"[{DateTime.Now:HH:mm:ss.fff}] {line}");
            }
            catch
            {
                _out = null; // console or pipe went away
            }
        }
    }

    private static string LevelName(int level) => level switch
    {
        0 => "debug",
        1 => "info",
        2 => "warning",
        _ => "error",
    };

    private const int StdOutputHandle = -11;
    private const int AttachParentProcess = -1;
    private const uint FileTypeUnknown = 0;
    private const uint GenericWrite = 0x40000000;
    private const uint FileShareWrite = 0x2;
    private const uint OpenExisting = 3;

    [LibraryImport("kernel32.dll")]
    private static partial nint GetStdHandle(int nStdHandle);

    [LibraryImport("kernel32.dll")]
    private static partial uint GetFileType(nint hFile);

    [LibraryImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool AttachConsole(int dwProcessId);

    [LibraryImport("kernel32.dll", StringMarshalling = StringMarshalling.Utf16)]
    private static partial nint CreateFileW(string name, uint access, uint share, nint security,
                                            uint creation, uint flags, nint template);
}
