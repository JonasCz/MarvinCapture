using System.Runtime.InteropServices;

namespace PinnacleCapture.Interop;

// Blittable, sequential-layout mirrors of every struct in pin_api.h.
//
// Convention from the header: structs the caller fills or receives start
// with a `size` field the caller must set to sizeof() as it compiled the
// struct; the library rejects an unrecognised size (PIN_ERR_ABI) instead of
// reading past the end. Every constructor below sets `size` accordingly.
//
// Fixed-size embedded C strings (char[N], not char*) are declared as
// `fixed byte` buffers and accessed through the Utf8Fixed helper via the
// string-typed properties below, never marshalled directly.

public static class PinLimits
{
    public const int PathMax = 1024;
    public const int NameMax = 64;
    public const int TextMax = 256;
    public const int MaxActions = 16;
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct PinDeviceInfo
{
    public uint Size;
    public fixed byte IdBuf[PinLimits.NameMax];
    public fixed byte NameBuf[PinLimits.NameMax];
    public fixed byte SerialBuf[PinLimits.NameMax];
    public ushort Vid;
    public ushort Pid;
    public PinDevState State;
    public uint OwnerPid;

    public string Id { get { fixed (byte* p = IdBuf) return Utf8Fixed.Get(p, PinLimits.NameMax); } }
    public string Name { get { fixed (byte* p = NameBuf) return Utf8Fixed.Get(p, PinLimits.NameMax); } }
    public string Serial { get { fixed (byte* p = SerialBuf) return Utf8Fixed.Get(p, PinLimits.NameMax); } }

    public static PinDeviceInfo Create()
    {
        var d = new PinDeviceInfo();
        d.Size = (uint)sizeof(PinDeviceInfo);
        return d;
    }
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct PinControlInfo
{
    public uint Size;
    public fixed byte LabelBuf[PinLimits.NameMax];
    public int Min, Max, Step, Def;
    public int Value;
    public int Enabled;

    public string Label { get { fixed (byte* p = LabelBuf) return Utf8Fixed.Get(p, PinLimits.NameMax); } }

    public static PinControlInfo Create()
    {
        var c = new PinControlInfo();
        c.Size = (uint)sizeof(PinControlInfo);
        return c;
    }
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct PinFormatInfo
{
    public uint Size;
    public PinFormat Format;
    public PinKind Kind;
    public fixed byte LabelBuf[PinLimits.NameMax];
    public fixed byte ExtensionBuf[16];
    public int SupportsTitle;
    public int SupportsSceneSplit;
    public int SupportsMultiPass;
    public int IsDefault;

    public string Label { get { fixed (byte* p = LabelBuf) return Utf8Fixed.Get(p, PinLimits.NameMax); } }
    public string Extension { get { fixed (byte* p = ExtensionBuf) return Utf8Fixed.Get(p, 16); } }

    public static PinFormatInfo Create()
    {
        var f = new PinFormatInfo();
        f.Size = (uint)sizeof(PinFormatInfo);
        return f;
    }
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct PinCaptureOpts
{
    public uint Size;
    public fixed byte PathBuf[PinLimits.PathMax];
    public PinFormat FormatAnalog;
    public PinFormat FormatDv;
    public PinFormat FormatHdv;
    public fixed byte TitleBuf[PinLimits.TextMax];
    public PinAspect Aspect;
    public int SceneSplit;
    public int IdleStopMinutes;
    public int Passes;
    public int StartDeck;
    public int KeepRaw;
    // Appended in ABI v2:
    public int RewindFirst;     // "Play and capture": rewind to the start of the tape first
    public uint FirstNumber;    // >= 1: every file is "base-NNNN.ext", counting up per scene from here
    public int MaxDurationMinutes; // stop after this long of capture time, signal/data or not; 0 = never

    public string Path
    {
        get { fixed (byte* p = PathBuf) return Utf8Fixed.Get(p, PinLimits.PathMax); }
        set { fixed (byte* p = PathBuf) Utf8Fixed.Set(p, PinLimits.PathMax, value); }
    }

    public string Title
    {
        get { fixed (byte* p = TitleBuf) return Utf8Fixed.Get(p, PinLimits.TextMax); }
        set { fixed (byte* p = TitleBuf) Utf8Fixed.Set(p, PinLimits.TextMax, value); }
    }

    public static PinCaptureOpts Create()
    {
        var o = new PinCaptureOpts();
        o.Size = (uint)sizeof(PinCaptureOpts);
        return o;
    }
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct PinOutputCheck
{
    public uint Size;
    public ulong FreeBytes;
    public ulong MinutesLeft;
    public int Fat32;
    public int Collision;
    public fixed byte FirstPathBuf[PinLimits.PathMax];
    public fixed byte MessageBuf[PinLimits.TextMax];
    public int LowSpace;           // free space known and below the core's 25 GiB threshold

    public string FirstPath { get { fixed (byte* p = FirstPathBuf) return Utf8Fixed.Get(p, PinLimits.PathMax); } }
    public string Message { get { fixed (byte* p = MessageBuf) return Utf8Fixed.Get(p, PinLimits.TextMax); } }

    public static PinOutputCheck Create()
    {
        var c = new PinOutputCheck();
        c.Size = (uint)sizeof(PinOutputCheck);
        return c;
    }
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct PinStatusSnapshot
{
    public uint Size;
    public PinState State;
    public PinStatus LastError;
    public fixed byte ErrorTextBuf[PinLimits.TextMax];

    public PinInput Input;
    public PinKind StreamKind;
    public int Signal;
    public int Is60Hz;
    public int Width, Height;
    public int DarNum, DarDen;
    public PinStd DetectedStd;

    public PinDeckState Deck;
    public int DeckBusy;
    public fixed byte TimecodeBuf[16];
    public fixed byte RecDatetimeBuf[32];
    public int TapePercent;

    public int Pass, Passes;
    public int Scene;
    public fixed byte CurrentFileBuf[PinLimits.PathMax];
    public double ElapsedS;
    public ulong Frames;
    public ulong FramesDropped;
    public ulong FramesDamaged;
    public ulong LostBlocks;
    public ulong TsErrors;
    public ulong BytesWritten;
    public ulong WriterBacklog;
    public ulong WriterBacklogMax;
    public double IdleS;

    public float AudioPeakDb0, AudioPeakDb1;
    public float AudioRmsDb0, AudioRmsDb1;

    // Appended by the engine workstream (ABI v2):
    public ulong DiskFreeBytes;     // at the capture's (or the hinted) output folder
    public double EstSecondsLeft;   // capture time left at the chosen format's rate; < 0 = unknown
    public int DiskLow;             // < 1 h or < 50 GB left

    // Appended later (still ABI-compatible): what the core is doing / waiting for.
    public fixed byte DetailBuf[PinLimits.TextMax];
    public int ProgressPercent;     // of the current PREPARING step, or -1
    public int CameraPresent;       // DV/HDV: 1 a camera answered, 0 none, -1 n/a or not known yet

    // Appended later: frames/audio blocks the disk writer's queue had to refuse.
    public ulong WriteDropped;

    // Appended later: seconds until the capture stops by itself, -1 = no such limit / not capturing.
    public double IdleStopRemainingS;   // the no-signal timeout (counts down while there is no signal)
    public double DurationRemainingS;   // the total capture time limit

    // Appended later: frame error statistics (see pin_api.h for what counts as an error).
    // Frames / FramesDropped / FramesError are the totals (since capture start, or session start
    // while not capturing); the Clip* ones restart with every output file.
    public ulong FramesError;
    public ulong ClipFrames;
    public ulong ClipFramesError;
    public ulong ClipFramesDropped;
    public ulong ErrVideoBlocks;
    public ulong ErrAudioBlocks;
    public ulong ErrMissingBlocks;

    // Appended later: sizes and the time-left estimate (see pin_api.h for the rate sources).
    public ulong ClipBytesWritten;
    public ulong TotalBytesWritten;
    public double EstBytesPerHour;
    public int EstRateSource;       // 0 nominal, 1 learned FFV1 rate, 2 measured

    public string ErrorText { get { fixed (byte* p = ErrorTextBuf) return Utf8Fixed.Get(p, PinLimits.TextMax); } }
    public string Detail { get { fixed (byte* p = DetailBuf) return Utf8Fixed.Get(p, PinLimits.TextMax); } }
    public string Timecode { get { fixed (byte* p = TimecodeBuf) return Utf8Fixed.Get(p, 16); } }
    public string RecDatetime { get { fixed (byte* p = RecDatetimeBuf) return Utf8Fixed.Get(p, 32); } }
    public string CurrentFile { get { fixed (byte* p = CurrentFileBuf) return Utf8Fixed.Get(p, PinLimits.PathMax); } }

    public static PinStatusSnapshot Create()
    {
        var s = new PinStatusSnapshot();
        s.Size = (uint)sizeof(PinStatusSnapshot);
        return s;
    }
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct PinEvent
{
    public uint Size;
    public PinEventKind Kind;
    public int A;
    public fixed byte TextBuf[PinLimits.PathMax];

    public string Text { get { fixed (byte* p = TextBuf) return Utf8Fixed.Get(p, PinLimits.PathMax); } }

    public static PinEvent Create()
    {
        var e = new PinEvent();
        e.Size = (uint)sizeof(PinEvent);
        return e;
    }
}

/// <summary>
/// Mirrors pin_frame_t. `plane[3]` is `const uint8_t*[3]` in C -- real
/// pointers, not embedded fixed-size data -- so these are plain IntPtr
/// fields (sequential layout gives the same in-memory shape as a C array of
/// 3 pointers) rather than a `fixed` buffer.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public unsafe struct PinFrame
{
    public uint Size;
    public ulong Seq;
    public int Width, Height;
    public int ChromaShiftX;
    public int ChromaShiftY;
    public nint Plane0, Plane1, Plane2;
    public int Stride0, Stride1, Stride2;
    public PinMatrix Matrix;
    public int FullRange;
    public int DarNum, DarDen;
    public int Interlaced;
    public int TopFieldFirst;

    public nint PlaneAt(int i) => i switch { 0 => Plane0, 1 => Plane1, 2 => Plane2, _ => 0 };
    public int StrideAt(int i) => i switch { 0 => Stride0, 1 => Stride1, 2 => Stride2, _ => 0 };

    public static PinFrame Create()
    {
        var f = new PinFrame();
        f.Size = (uint)sizeof(PinFrame);
        return f;
    }
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct PinLaunch
{
    public uint Size;
    public fixed byte DeviceBuf[PinLimits.NameMax];
    public int HasInput;
    public PinInput Input;
    public int HasStd;
    public PinStd Std;
    public int HasCaptureOpts;
    public PinCaptureOpts Capture;
    public uint CaptureFields;
    public int ActionCount;
    public fixed int Actions[PinLimits.MaxActions]; // pin_action_t is int-sized
    public int ExitWhenDone;

    public string Device
    {
        get { fixed (byte* p = DeviceBuf) return Utf8Fixed.Get(p, PinLimits.NameMax); }
        set { fixed (byte* p = DeviceBuf) Utf8Fixed.Set(p, PinLimits.NameMax, value); }
    }

    public PinAction GetAction(int i)
    {
        fixed (int* a = Actions) return (PinAction)a[i];
    }

    public void SetAction(int i, PinAction action)
    {
        fixed (int* a = Actions) a[i] = (int)action;
    }

    public static PinLaunch Create()
    {
        var l = new PinLaunch();
        l.Size = (uint)sizeof(PinLaunch);
        l.Capture = PinCaptureOpts.Create();
        return l;
    }
}
