namespace PinnacleCapture.Interop;

// Every enum in pin_api.h is a plain C enum, which is `int`-sized under every
// ABI this project targets (MSVC/UCRT64 gcc on Windows). Backing them with
// `int` here keeps struct layout and by-value parameter passing identical to
// the C side.

public enum PinStatus : int
{
    Ok = 0,
    ErrAbi,
    ErrArg,
    ErrState,
    ErrNotFound,
    ErrBusy,
    ErrNoDriver,
    ErrUsb,
    ErrFirmware,
    ErrNotReady,
    ErrNoCamera,
    ErrDeck,
    ErrIo,
    ErrExists,
    ErrDiskFull,
    ErrCodec,
    ErrNoMem,
    ErrInternal,
}

public enum PinDevState : int
{
    Ready = 0,
    Preparing,
    InUse,
    OpenHere,
    NoDriver,
    Unsupported,
}

/// <summary>pin_progress_mode_t: what the taskbar progress should show.</summary>
public enum PinProgressMode : int
{
    None = 0,
    Indeterminate,
    Normal,
    Paused,
    Error,
}

public enum PinState : int
{
    Closed = 0,
    Preparing,
    Ready,
    Capturing,
    Stopping,
    Rewinding,
    Error,
}

public enum PinInput : int
{
    Dv = 0,
    SVideo,
    Composite,
}

public enum PinStd : int
{
    Auto = 0,
    Pal,
    Ntsc,
    PalM,
    PalN,
    Pal60,
    Ntsc443,
    NtscJ,
    Secam,
    Count,
}

public enum PinControl : int
{
    Brightness = 0,
    Contrast,
    Saturation,
    Hue,
    Sharpness,
    AudioGain,
    Count,
}

public enum PinDeckState : int
{
    Unknown = 0,
    Stopped,
    Playing,
    Paused,
    FastForward,
    Rewinding,
    Recording,
    NoTape,
}

public enum PinDeckCmd : int
{
    Play = 0,
    Pause,
    Stop,
    Ff,
    Rew,
}

/// <summary>pin_capture_action_t: which capture button a pin_capture_action_allowed() question is about.</summary>
public enum PinCaptureAction : int
{
    StartManual = 0,
    StartAuto,
    Stop,
    StopTape,
}

public enum PinFormat : int
{
    AnalogAvi = 0,
    AnalogFfv1Mkv,
    DvRaw,
    DvAvi,
    DvMov,
    HdvTs,
    HdvMov,
    HdvMkv,
    // Appended: lossy, with the system's (normally hardware) encoder; listed by pin_formats()
    // only where the encoder opened (pin_encoders_probe).
    AnalogH264Mkv,
    AnalogHevcMkv,
    DvH264Mkv,
    DvHevcMkv,
    Count,
}

/// <summary>pin_deinterlace_t: the H.264 / HEVC formats' handling of interlaced video.</summary>
public enum PinDeinterlace : int
{
    KeepFields = 0,     // each interlaced frame encoded as one progressive picture
    DoubleRate = 1,     // bwdif: every field becomes a frame (25i -> 50p, 29.97i -> 59.94p)
}

/// <summary>pin_ffv1_compression_t: FFV1's coder settings, all lossless.</summary>
public enum PinFfv1Compression : int
{
    Fast = 0,
    Smaller,
    Smallest,
    LevelCount,
}

public enum PinKind : int
{
    Analog = 0,
    Dv,
    Hdv,
}

public enum PinAspect : int
{
    Auto = 0,
    Aspect4x3,
    Aspect16x9,
}

public enum PinEventKind : int
{
    State = 0,
    FileOpened,
    FileClosed,
    Scene,
    Pass,
    Deck,
    InputFormat,
    Log,
    Error,
    Done,
    Devices,
    CaptureEnded,   // A = PinStopReason, Text = ready-made sentence
    Step,           // command-line script: A = step index, Text = its description
    NoVideo,        // a capture that did not end abnormally got no video: A = PinStopReason, Text = sentence
}

/// <summary>Why a capture ended (pin_stop_reason_t).</summary>
public enum PinStopReason : int
{
    None = 0,
    User,
    NoSignal,
    TimeLimit,
    EndOfTape,
    DeviceLost,
    CameraLost,
    DiskFull,
    WriteError,
    Error,
    PipeSlow,       // output to stdout: the reading program could not keep up
    PipeClosed,     // output to stdout: the reading program exited
    DiskSlow,       // the output drive could not keep up; the file ends before the first lost unit
}

public enum PinMatrix : int
{
    Bt601 = 0,
    Bt709,
}
