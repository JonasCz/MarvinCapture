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
    Count,
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
}

public enum PinMatrix : int
{
    Bt601 = 0,
    Bt709,
}

public enum PinAction : int
{
    None = 0,
    Rewind,
    Play,
    Stop,
    Capture,
    WaitEot,
}

[System.Flags]
public enum PinOptFields : uint
{
    Path = 1u << 0,
    Format = 1u << 1,
    Title = 1u << 2,
    Split = 1u << 3,
    Passes = 1u << 4,
    Idle = 1u << 5,
    Aspect = 1u << 6,
}
