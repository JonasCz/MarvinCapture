using System;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.Marshalling;
using System.Text;

namespace PinnacleCapture.Interop;

/// <summary>
/// Source-generated P/Invoke bindings for every function in
/// src/api/pin_api.h ("marvin-core", per the Windows app naming in
/// the plan's "Changes after your review"). Raw [LibraryImport] declarations
/// are private/internal; each has a friendlier public wrapper that deals
/// with pinning, SafeHandle, and the fixed-size embedded UTF-8 strings.
///
/// Size-versioned structs the library *fills* are passed by `ref`, never
/// `out`: the header requires the caller to set `size` before the call,
/// and the source generator zero-initialises `out` arguments, which would
/// wipe the size and get every call rejected with PIN_ERR_ABI.
/// </summary>
public static unsafe partial class Native
{
    private const string Lib = "marvin-core.dll";

    // ---- basics -----------------------------------------------------

    [LibraryImport(Lib)]
    private static partial uint pin_api_version();

    [LibraryImport(Lib)]
    private static partial nint pin_version_string();

    [LibraryImport(Lib)]
    private static partial nint pin_strerror(PinStatus s);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    private static partial PinStatus pin_set_firmware_dir(string utf8Dir);

    public static uint ApiVersion() => pin_api_version();
    public static string VersionString() => PtrToUtf8(pin_version_string());
    public static string StrError(PinStatus s) => PtrToUtf8(pin_strerror(s));
    public static PinStatus SetFirmwareDir(string dir) => pin_set_firmware_dir(dir);

    private static string PtrToUtf8(nint p) => p == 0 ? string.Empty : (Marshal.PtrToStringUTF8(p) ?? string.Empty);

    // ---- devices ------------------------------------------------------

    [LibraryImport(Lib)]
    private static partial int pin_enumerate(PinDeviceInfo[] out_, int max);

    public static PinDeviceInfo[] Enumerate(int max = 16)
    {
        var buf = new PinDeviceInfo[max];
        for (int i = 0; i < max; i++)
        {
            buf[i] = PinDeviceInfo.Create();
        }
        int count = pin_enumerate(buf, max);
        int n = Math.Min(count, max);
        if (n == buf.Length)
        {
            return buf;
        }
        var trimmed = new PinDeviceInfo[n];
        Array.Copy(buf, trimmed, n);
        return trimmed;
    }

    [LibraryImport(Lib)]
    private static partial nint pin_usb_hub_hint();

    /// <summary>The core's advice for a device behind a USB hub (platform tool included).</summary>
    public static string UsbHubHint() => PtrToUtf8(pin_usb_hub_hint());

    [LibraryImport(Lib)]
    private static partial int pin_devices_wait(int timeoutMs);

    [LibraryImport(Lib)]
    private static partial void pin_devices_wake();

    /// <summary>Blocks until devices were plugged/unplugged or another process changed a device's
    /// lock state: 1 = changed, 0 = timeout or woken, &lt;0 = error.</summary>
    public static int DevicesWait(int timeoutMs) => pin_devices_wait(timeoutMs);

    /// <summary>Makes a pending <see cref="DevicesWait"/> return immediately (shutdown).</summary>
    public static void DevicesWake() => pin_devices_wake();

    // ---- sessions -------------------------------------------------------

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    private static partial PinStatus pin_open(string deviceId, out nint outSession);

    [LibraryImport(Lib, EntryPoint = "pin_close")]
    internal static partial void pin_close_raw(nint s);

    public static PinStatus Open(string deviceId, out PinSessionHandle? session)
    {
        var status = pin_open(deviceId, out nint raw);
        session = (status == PinStatus.Ok && raw != 0) ? new PinSessionHandle(raw, true) : null;
        return status;
    }

    [LibraryImport(Lib)]
    private static partial PinStatus pin_set_input(SafeHandle s, PinInput input);
    public static PinStatus SetInput(SafeHandle s, PinInput input) => pin_set_input(s, input);

    // ---- analog controls ------------------------------------------------

    [LibraryImport(Lib)]
    private static partial nint pin_std_name(PinStd std);
    public static string StdName(PinStd std) => PtrToUtf8(pin_std_name(std));

    [LibraryImport(Lib)]
    private static partial PinStatus pin_set_standard(SafeHandle s, PinStd std);
    public static PinStatus SetStandard(SafeHandle s, PinStd std) => pin_set_standard(s, std);

    [LibraryImport(Lib)]
    private static partial PinStatus pin_get_control(SafeHandle s, PinControl c, ref PinControlInfo out_);
    public static PinStatus GetControl(SafeHandle s, PinControl c, out PinControlInfo info)
    {
        info = PinControlInfo.Create();
        return pin_get_control(s, c, ref info);
    }

    [LibraryImport(Lib)]
    private static partial PinStatus pin_set_control(SafeHandle s, PinControl c, int value);
    public static PinStatus SetControl(SafeHandle s, PinControl c, int value) => pin_set_control(s, c, value);

    // ---- deck -------------------------------------------------------------

    [LibraryImport(Lib)]
    private static partial PinStatus pin_deck(SafeHandle s, PinDeckCmd cmd);
    public static PinStatus Deck(SafeHandle s, PinDeckCmd cmd) => pin_deck(s, cmd);

    // ---- formats ------------------------------------------------------------

    [LibraryImport(Lib)]
    private static partial int pin_formats(PinKind kind, PinFormatInfo[] out_, int max);

    public static PinFormatInfo[] Formats(PinKind kind, int max = 16)
    {
        var buf = new PinFormatInfo[max];
        for (int i = 0; i < max; i++)
        {
            buf[i] = PinFormatInfo.Create();
        }
        int count = pin_formats(kind, buf, max);
        int n = Math.Min(count, max);
        if (n == buf.Length)
        {
            return buf;
        }
        var trimmed = new PinFormatInfo[n];
        Array.Copy(buf, trimmed, n);
        return trimmed;
    }

    [LibraryImport(Lib)]
    private static partial PinStatus pin_format_info(PinFormat f, ref PinFormatInfo out_);
    public static PinStatus FormatInfo(PinFormat f, out PinFormatInfo info)
    {
        info = PinFormatInfo.Create();
        return pin_format_info(f, ref info);
    }

    // ---- capture --------------------------------------------------------------

    [LibraryImport(Lib)]
    private static partial void pin_capture_opts_defaults(ref PinCaptureOpts o);

    [LibraryImport(Lib)]
    private static partial int pin_capture_passes_allowed(int idleStopMinutes, int maxDurationMinutes);

    /// <summary>Core rule: more than one pass needs the no-signal timeout or the per-pass time limit.</summary>
    public static bool PassesAllowed(double idleStopMinutes, double maxDurationMinutes)
    {
        static int Min(double v) => double.IsNaN(v) || v < 0 ? 0 : (int)Math.Min(v, int.MaxValue);
        try
        {
            return pin_capture_passes_allowed(Min(idleStopMinutes), Min(maxDurationMinutes)) != 0;
        }
        catch (EntryPointNotFoundException)
        {
            return true; // older core: it enforces nothing either
        }
    }

    public static PinCaptureOpts CaptureOptsDefaults()
    {
        var o = PinCaptureOpts.Create();
        pin_capture_opts_defaults(ref o);
        return o;
    }

    [LibraryImport(Lib)]
    private static partial PinStatus pin_check_output(SafeHandle s, in PinCaptureOpts o, ref PinOutputCheck out_);
    public static PinStatus CheckOutput(SafeHandle s, in PinCaptureOpts o, out PinOutputCheck check)
    {
        check = PinOutputCheck.Create();
        return pin_check_output(s, in o, ref check);
    }

    [LibraryImport(Lib)]
    private static partial PinStatus pin_capture_start(SafeHandle s, in PinCaptureOpts o, int overwrite);
    public static PinStatus CaptureStart(SafeHandle s, in PinCaptureOpts o, bool overwrite) =>
        pin_capture_start(s, in o, overwrite ? 1 : 0);

    [LibraryImport(Lib)]
    private static partial PinStatus pin_set_output_hint(SafeHandle s, in PinCaptureOpts o);

    /// <summary>
    /// Tells the core where the next capture would go (path + formats) so the
    /// status snapshot can report free space / time left before capturing.
    /// Added to the API by the engine workstream; an older core without the
    /// export just reports ErrInternal here instead of throwing.
    /// </summary>
    public static PinStatus SetOutputHint(SafeHandle s, in PinCaptureOpts o)
    {
        try
        {
            return pin_set_output_hint(s, in o);
        }
        catch (EntryPointNotFoundException)
        {
            return PinStatus.ErrInternal;
        }
    }

    [LibraryImport(Lib)]
    private static partial PinStatus pin_capture_stop(SafeHandle s);
    public static PinStatus CaptureStop(SafeHandle s) => pin_capture_stop(s);

    /// <summary>stopDeck: 1 = never send deck Stop, 2 = always (see PIN_STOP_DECK_*).</summary>
    [LibraryImport(Lib)]
    private static partial PinStatus pin_capture_stop_ex(SafeHandle s, int stopDeck);
    public static PinStatus CaptureStop(SafeHandle s, bool stopDeck) => pin_capture_stop_ex(s, stopDeck ? 2 : 1);

    /// <summary>Stops the way the capture was started: deck Stop only if it was started with the deck (PIN_STOP_DECK_AS_STARTED).</summary>
    public static PinStatus CaptureStopAsStarted(SafeHandle s) => pin_capture_stop_ex(s, 0);

    // ---- status -----------------------------------------------------------------

    [LibraryImport(Lib)]
    private static partial PinStatus pin_get_status(SafeHandle s, ref PinStatusSnapshot out_);
    public static PinStatus GetStatus(SafeHandle s, out PinStatusSnapshot status)
    {
        status = PinStatusSnapshot.Create();
        return pin_get_status(s, ref status);
    }

    [LibraryImport(Lib)]
    private static partial void pin_format_status_line(in PinStatusSnapshot st, byte* out_, nuint cap);

    public static string FormatStatusLine(in PinStatusSnapshot st)
    {
        const int cap = 512;
        byte* buf = stackalloc byte[cap];
        pin_format_status_line(in st, buf, (nuint)cap);
        return Utf8Fixed.Get(buf, cap);
    }

    [LibraryImport(Lib)]
    private static partial int pin_stop_reason_abnormal(PinStopReason reason);

    /// <summary>The core's rule: a capture that ended this way failed or lost something (dialog), else it ended normally (status bar).</summary>
    public static bool StopReasonAbnormal(PinStopReason reason) => pin_stop_reason_abnormal(reason) != 0;

    [LibraryImport(Lib)]
    private static partial void pin_format_remaining(double seconds, byte* out_, nuint cap);

    /// <summary>"5m30s" for a stop countdown (formatted by the core so every GUI agrees).</summary>
    public static string FormatRemaining(double seconds)
    {
        const int cap = 32;
        byte* buf = stackalloc byte[cap];
        pin_format_remaining(seconds, buf, (nuint)cap);
        return Utf8Fixed.Get(buf, cap);
    }

    [LibraryImport(Lib)]
    private static partial PinProgressMode pin_status_progress(in PinStatusSnapshot st, double idleTotalS,
                                                               double durationTotalS, ref double fraction);

    /// <summary>Taskbar progress mode and fraction (0..1) for a status snapshot, decided by the core.</summary>
    public static PinProgressMode StatusProgress(in PinStatusSnapshot st, double idleTotalS, double durationTotalS,
                                                 out double fraction)
    {
        fraction = 0;
        return pin_status_progress(in st, idleTotalS, durationTotalS, ref fraction);
    }

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    private static partial void pin_format_window_title(in PinStatusSnapshot st, string deviceName, byte* out_, nuint cap);

    public static string FormatWindowTitle(in PinStatusSnapshot st, string deviceName)
    {
        const int cap = 512;
        byte* buf = stackalloc byte[cap];
        pin_format_window_title(in st, deviceName, buf, (nuint)cap);
        return Utf8Fixed.Get(buf, cap);
    }

    // ---- ready-made texts and enable rules ----------------------------------------

    [LibraryImport(Lib)]
    private static partial void pin_format_bytes(ulong bytes, byte* out_, nuint cap);

    /// <summary>"123 B" / "1.5 GB".</summary>
    public static string FormatBytes(ulong bytes)
    {
        const int cap = 32;
        byte* buf = stackalloc byte[cap];
        pin_format_bytes(bytes, buf, (nuint)cap);
        return Utf8Fixed.Get(buf, cap);
    }

    [LibraryImport(Lib)]
    private static partial void pin_format_time_left(double seconds, byte* out_, nuint cap);

    /// <summary>"2 h 05 min left" / "45 min left".</summary>
    public static string FormatTimeLeft(double seconds)
    {
        const int cap = 48;
        byte* buf = stackalloc byte[cap];
        pin_format_time_left(seconds, buf, (nuint)cap);
        return Utf8Fixed.Get(buf, cap);
    }

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    private static partial uint pin_next_file_number(string path);

    /// <summary>pin_capture_opts_t.first_number for a capture to this path: one past the highest "name-NNNN.ext" in its directory.</summary>
    public static uint NextFileNumber(string path) => pin_next_file_number(path);

    [LibraryImport(Lib)]
    private static partial nint pin_deck_state_name(PinDeckState s);
    public static string DeckStateName(PinDeckState s) => PtrToUtf8(pin_deck_state_name(s));

    [LibraryImport(Lib)]
    private static partial int pin_deck_cmd_allowed(PinState state, int deckAvailable, PinDeckState deck, PinDeckCmd cmd);

    /// <summary>Whether the deck button for cmd is enabled (core rule; deckAvailable = a camera is on the bus).</summary>
    public static bool DeckCmdAllowed(PinState state, bool deckAvailable, PinDeckState deck, PinDeckCmd cmd) =>
        pin_deck_cmd_allowed(state, deckAvailable ? 1 : 0, deck, cmd) != 0;

    [LibraryImport(Lib)]
    private static partial int pin_capture_action_allowed(PinState state, int deckAvailable, PinCaptureAction action);

    /// <summary>Whether the capture button for this action is enabled (core rule).</summary>
    public static bool CaptureActionAllowed(PinState state, bool deckAvailable, PinCaptureAction action) =>
        pin_capture_action_allowed(state, deckAvailable ? 1 : 0, action) != 0;

    [LibraryImport(Lib)]
    private static partial int pin_manual_capture_allowed(PinState state, int deckAvailable, PinDeckState deck);

    /// <summary>DV/HDV "Manual capture" start is enabled (core rule: READY, a camera, deck playing).</summary>
    public static bool ManualCaptureAllowed(PinState state, bool deckAvailable, PinDeckState deck) =>
        pin_manual_capture_allowed(state, deckAvailable ? 1 : 0, deck) != 0;

    [LibraryImport(Lib)]
    private static partial nint pin_manual_capture_block_text(int deckAvailable, PinDeckState deck);

    /// <summary>Why the deck blocks Manual capture ("Start playback on the tape first"), "" when it does not.</summary>
    public static string ManualCaptureBlockText(bool deckAvailable, PinDeckState deck) =>
        PtrToUtf8(pin_manual_capture_block_text(deckAvailable ? 1 : 0, deck));

    [LibraryImport(Lib)]
    private static partial int pin_state_is_capturing(PinState state);

    /// <summary>Capturing, Stopping or Rewinding: a capture is running or ending.</summary>
    public static bool StateIsCapturing(PinState state) => pin_state_is_capturing(state) != 0;

    [LibraryImport(Lib)]
    private static partial void pin_format_state(in PinStatusSnapshot st, byte* out_, nuint cap);

    /// <summary>"Ready", "Rewinding (pass 2/3)", ...</summary>
    public static string FormatState(in PinStatusSnapshot st)
    {
        const int cap = 64;
        byte* buf = stackalloc byte[cap];
        pin_format_state(in st, buf, (nuint)cap);
        return Utf8Fixed.Get(buf, cap);
    }

    [LibraryImport(Lib)]
    private static partial int pin_format_status_short(in PinStatusSnapshot st, int behindHub, byte* out_, nuint cap);

    /// <summary>The status bar's first line. <paramref name="hubReady"/>: it is the "Ready (connection via USB hub ...)" text, whose tooltip is the hub hint.</summary>
    public static string FormatStatusShort(in PinStatusSnapshot st, bool behindHub, out bool hubReady)
    {
        const int cap = PinLimits.PathMax + 128; // a long file name, then "  ·  pass 2/3"
        byte* buf = stackalloc byte[cap];
        hubReady = pin_format_status_short(in st, behindHub ? 1 : 0, buf, (nuint)cap) != 0;
        return Utf8Fixed.Get(buf, cap);
    }

    [LibraryImport(Lib)]
    private static partial void pin_format_signal(in PinStatusSnapshot st, byte* out_, nuint cap);

    /// <summary>"HDV · 1080i25", "S-Video · NTSC", ...</summary>
    public static string FormatSignal(in PinStatusSnapshot st)
    {
        const int cap = 96;
        byte* buf = stackalloc byte[cap];
        pin_format_signal(in st, buf, (nuint)cap);
        return Utf8Fixed.Get(buf, cap);
    }

    [LibraryImport(Lib)]
    private static partial void pin_format_frames(in PinStatusSnapshot st, byte* out_, nuint cap);

    /// <summary>"Frames 1,234 · 2 err · 0 drop".</summary>
    public static string FormatFrames(in PinStatusSnapshot st)
    {
        const int cap = 160;
        byte* buf = stackalloc byte[cap];
        pin_format_frames(in st, buf, (nuint)cap);
        return Utf8Fixed.Get(buf, cap);
    }

    [LibraryImport(Lib)]
    private static partial void pin_format_frames_detail(in PinStatusSnapshot st, byte* out_, nuint cap);

    /// <summary>The frames item's multi-line tooltip.</summary>
    public static string FormatFramesDetail(in PinStatusSnapshot st)
    {
        const int cap = 512;
        byte* buf = stackalloc byte[cap];
        pin_format_frames_detail(in st, buf, (nuint)cap);
        return Utf8Fixed.Get(buf, cap);
    }

    [LibraryImport(Lib)]
    private static partial void pin_format_sizes(in PinStatusSnapshot st, byte* out_, nuint cap);

    /// <summary>"1.5 GB / 300.0 MB": this capture / the current file.</summary>
    public static string FormatSizes(in PinStatusSnapshot st)
    {
        const int cap = 96;
        byte* buf = stackalloc byte[cap];
        pin_format_sizes(in st, buf, (nuint)cap);
        return Utf8Fixed.Get(buf, cap);
    }

    [LibraryImport(Lib)]
    private static partial void pin_format_storage_free(in PinStatusSnapshot st, byte* out_, nuint cap);

    /// <summary>"· 20.0 GB free · 95 min left" (either part only when known), else "".</summary>
    public static string FormatStorageFree(in PinStatusSnapshot st)
    {
        const int cap = 160;
        byte* buf = stackalloc byte[cap];
        pin_format_storage_free(in st, buf, (nuint)cap);
        return Utf8Fixed.Get(buf, cap);
    }

    [LibraryImport(Lib)]
    private static partial void pin_format_storage_detail(in PinStatusSnapshot st, byte* out_, nuint cap);

    /// <summary>The storage item's multi-line tooltip.</summary>
    public static string FormatStorageDetail(in PinStatusSnapshot st)
    {
        const int cap = 512;
        byte* buf = stackalloc byte[cap];
        pin_format_storage_detail(in st, buf, (nuint)cap);
        return Utf8Fixed.Get(buf, cap);
    }

    [LibraryImport(Lib)]
    private static partial void pin_device_display_name(in PinDeviceInfo d, byte* out_, nuint cap);

    /// <summary>The device's name, with " (untested)" for a model nobody verified on hardware.</summary>
    public static string DeviceDisplayName(in PinDeviceInfo d)
    {
        const int cap = 160;
        byte* buf = stackalloc byte[cap];
        pin_device_display_name(in d, buf, (nuint)cap);
        return Utf8Fixed.Get(buf, cap);
    }

    [LibraryImport(Lib)]
    private static partial void pin_device_status_text(in PinDeviceInfo d, int capturingHere, byte* out_, nuint cap);

    /// <summary>The device badge: "Capturing", "Ready", "In use", ...</summary>
    public static string DeviceStatusText(in PinDeviceInfo d, bool capturingHere)
    {
        const int cap = 64;
        byte* buf = stackalloc byte[cap];
        pin_device_status_text(in d, capturingHere ? 1 : 0, buf, (nuint)cap);
        return Utf8Fixed.Get(buf, cap);
    }

    [LibraryImport(Lib)]
    private static partial int pin_device_unavailable_reason(in PinDeviceInfo d, byte* out_, nuint cap);

    /// <summary>Why the device can't be picked (tooltip), or null when it can.</summary>
    public static string? DeviceUnavailableReason(in PinDeviceInfo d)
    {
        const int cap = 256;
        byte* buf = stackalloc byte[cap];
        return pin_device_unavailable_reason(in d, buf, (nuint)cap) != 0 ? Utf8Fixed.Get(buf, cap) : null;
    }

    [LibraryImport(Lib)]
    private static partial int pin_device_open_problem(in PinDeviceInfo d, byte* out_, nuint cap);

    /// <summary>The sentence for a failed attempt to open the device, or null when it can be opened.</summary>
    public static string? DeviceOpenProblem(in PinDeviceInfo d)
    {
        const int cap = 256;
        byte* buf = stackalloc byte[cap];
        return pin_device_open_problem(in d, buf, (nuint)cap) != 0 ? Utf8Fixed.Get(buf, cap) : null;
    }

    [LibraryImport(Lib)]
    private static partial nint pin_no_devices_hint();

    /// <summary>The hint shown while no device is found (platform-specific).</summary>
    public static string NoDevicesHint() => PtrToUtf8(pin_no_devices_hint());

    // ---- events -------------------------------------------------------------------

    [LibraryImport(Lib)]
    private static partial int pin_poll_event(SafeHandle s, ref PinEvent out_);
    public static bool PollEvent(SafeHandle s, out PinEvent evt)
    {
        evt = PinEvent.Create();
        return pin_poll_event(s, ref evt) != 0;
    }

    [LibraryImport(Lib, EntryPoint = "pin_poll_event")]
    private static partial int pin_poll_event_raw(nint s, ref PinEvent out_);

    /// <summary>Process-wide log events (no session open yet): s == NULL is documented as valid.</summary>
    public static bool PollProcessEvent(out PinEvent evt)
    {
        evt = PinEvent.Create();
        return pin_poll_event_raw(0, ref evt) != 0;
    }

    [LibraryImport(Lib)]
    private static partial void pin_set_log_level(int level);
    public static void SetLogLevel(int level) => pin_set_log_level(level);

    // ---- preview --------------------------------------------------------------------

    [LibraryImport(Lib)]
    private static partial int pin_preview_wait(SafeHandle s, ulong afterSeq, int timeoutMs);
    public static int PreviewWait(SafeHandle s, ulong afterSeq, int timeoutMs) => pin_preview_wait(s, afterSeq, timeoutMs);

    [LibraryImport(Lib)]
    private static partial PinStatus pin_preview_lock(SafeHandle s, ref PinFrame out_);
    public static PinStatus PreviewLock(SafeHandle s, out PinFrame frame)
    {
        frame = PinFrame.Create();
        return pin_preview_lock(s, ref frame);
    }

    [LibraryImport(Lib)]
    private static partial PinStatus pin_preview_lock_due(SafeHandle s, double now, ref PinFrame out_);
    public static PinStatus PreviewLockDue(SafeHandle s, double now, out PinFrame frame)
    {
        frame = PinFrame.Create();
        return pin_preview_lock_due(s, now, ref frame);
    }

    [LibraryImport(Lib)]
    private static partial int pin_preview_next_time(SafeHandle s, out double t);
    public static bool PreviewNextTime(SafeHandle s, out double t) => pin_preview_next_time(s, out t) != 0;

    [LibraryImport(Lib)]
    private static partial double pin_clock_now();
    public static double ClockNow() => pin_clock_now();

    [LibraryImport(Lib)]
    private static partial void pin_preview_unlock(SafeHandle s);
    public static void PreviewUnlock(SafeHandle s) => pin_preview_unlock(s);

    [LibraryImport(Lib)]
    private static partial void pin_preview_enable(SafeHandle s, int enabled);
    public static void PreviewEnable(SafeHandle s, bool enabled) => pin_preview_enable(s, enabled ? 1 : 0);

    [LibraryImport(Lib)]
    private static partial void pin_set_aspect(SafeHandle s, PinAspect aspect);
    public static void SetAspect(SafeHandle s, PinAspect aspect) => pin_set_aspect(s, aspect);

    [LibraryImport(Lib)]
    private static partial void pin_fit_rect(int darNum, int darDen, int w, int h, out int x, out int y, out int rw, out int rh);
    public static void FitRect(int darNum, int darDen, int w, int h, out int x, out int y, out int rw, out int rh) =>
        pin_fit_rect(darNum, darDen, w, h, out x, out y, out rw, out rh);

    [LibraryImport(Lib)]
    private static partial void pin_yuv_to_rgb_matrix(PinMatrix m, int fullRange, float* out_);

    public static float[] YuvToRgbMatrix(PinMatrix m, bool fullRange)
    {
        var result = new float[12];
        fixed (float* p = result)
        {
            pin_yuv_to_rgb_matrix(m, fullRange ? 1 : 0, p);
        }
        return result;
    }

    // ---- audio monitoring -------------------------------------------------------------

    [LibraryImport(Lib)]
    private static partial void pin_monitor_enable(SafeHandle s, int enabled);
    public static void MonitorEnable(SafeHandle s, bool enabled) => pin_monitor_enable(s, enabled ? 1 : 0);

    [LibraryImport(Lib)]
    private static partial int pin_monitor_read(SafeHandle s, short* out_, int maxFrames);

    public static int MonitorRead(SafeHandle s, short[] buffer, int maxFrames)
    {
        fixed (short* p = buffer)
        {
            return pin_monitor_read(s, p, maxFrames);
        }
    }

    [LibraryImport(Lib)]
    private static partial int pin_monitor_available(SafeHandle s);
    public static int MonitorAvailable(SafeHandle s) => pin_monitor_available(s);

    // ---- settings ---------------------------------------------------------------------

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    private static partial PinStatus pin_settings_get(string key, byte* out_, nuint cap);

    public static string SettingsGet(string key, string fallback = "")
    {
        const int cap = 1024;
        byte* buf = stackalloc byte[cap];
        var status = pin_settings_get(key, buf, (nuint)cap);
        return status == PinStatus.Ok ? Utf8Fixed.Get(buf, cap) : fallback;
    }

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    private static partial PinStatus pin_settings_set(string key, string value);
    public static PinStatus SettingsSet(string key, string value) => pin_settings_set(key, value);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    private static partial PinStatus pin_device_settings_key(string serial, string id, string? key, byte* out_, nuint cap);

    /// <summary>"dev_&lt;ID&gt;." prefix for this unit's own settings (core: pin_device_settings_key); null if no usable ID.</summary>
    public static string? DeviceSettingsPrefix(string serial, string id)
    {
        const int cap = 256;
        byte* buf = stackalloc byte[cap];
        return pin_device_settings_key(serial, id, null, buf, (nuint)cap) == PinStatus.Ok ? Utf8Fixed.Get(buf, cap) : null;
    }

    // ---- command line: settings + actions (pin_script_*) ------------------------------

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    private static partial PinStatus pin_script_parse(int argc, string[] argv, out nint script, byte* err, nuint errCap);

    [LibraryImport(Lib, EntryPoint = "pin_script_free")]
    internal static partial void pin_script_free_raw(nint script);

    [LibraryImport(Lib)]
    private static partial nint pin_script_help();
    public static string ScriptHelp() => PtrToUtf8(pin_script_help());

    /// <summary>Parses the command line (WITHOUT the program name).</summary>
    public static PinStatus ScriptParse(string[] argv, out PinScriptHandle? script, out string error)
    {
        const int cap = 512;
        byte* buf = stackalloc byte[cap];
        var status = pin_script_parse(argv.Length, argv, out nint raw, buf, (nuint)cap);
        script = (status == PinStatus.Ok && raw != 0) ? new PinScriptHandle(raw) : null;
        error = status == PinStatus.Ok ? string.Empty : Utf8Fixed.Get(buf, cap);
        return status;
    }

    [LibraryImport(Lib)]
    private static partial int pin_script_help_requested(SafeHandle script);
    public static bool ScriptHelpRequested(SafeHandle script) => pin_script_help_requested(script) != 0;

    [LibraryImport(Lib)]
    private static partial nint pin_script_device(SafeHandle script);
    /// <summary>-d / --device value, or "" when none was given.</summary>
    public static string ScriptDevice(SafeHandle script) => PtrToUtf8(pin_script_device(script));

    [LibraryImport(Lib)]
    private static partial int pin_script_debug(SafeHandle script);
    /// <summary>--debug was given.</summary>
    public static bool ScriptDebug(SafeHandle script) => pin_script_debug(script) != 0;

    [LibraryImport(Lib)]
    private static partial int pin_script_exit_when_done(SafeHandle script);
    /// <summary>--exit-when-done was given.</summary>
    public static bool ScriptExitWhenDone(SafeHandle script) => pin_script_exit_when_done(script) != 0;

    [LibraryImport(Lib)]
    private static partial int pin_script_needs_session(SafeHandle script);
    public static bool ScriptNeedsSession(SafeHandle script) => pin_script_needs_session(script) != 0;

    [LibraryImport(Lib)]
    private static partial PinStatus pin_script_settings(SafeHandle script, ref PinScriptSettings out_);
    public static PinScriptSettings ScriptSettings(SafeHandle script)
    {
        var st = PinScriptSettings.Create();
        pin_script_settings(script, ref st);
        return st;
    }

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    private static partial void pin_set_replay_file(string path);
    /// <summary>Makes a recording show up as a "replay:&lt;name&gt;" device (pin_set_replay_file).</summary>
    public static void SetReplayFile(string path) => pin_set_replay_file(path);

    [LibraryImport(Lib)]
    private static partial PinStatus pin_script_run(SafeHandle s, SafeHandle script);
    public static PinStatus ScriptRun(SafeHandle s, SafeHandle script) => pin_script_run(s, script);
}
