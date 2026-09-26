using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.Marshalling;

namespace PinnacleCapture.Services;

/// <summary>
/// Phase 10: WASAPI shared-mode, event-driven playback of the core's PCM
/// monitor ring. Pulls interleaved 48 kHz s16 stereo frames from a caller
/// supplied reader delegate (see <see cref="IAudioMonitorService.SetSource"/>)
/// on a dedicated thread and renders them to the default output endpoint.
///
/// All native buffering/latency policy lives in the core (it self-trims to
/// ~80 ms and drops old data above ~200 ms); this class only owns the
/// WASAPI plumbing, mute-to-silence, a small anti-crackle prebuffer after
/// underrun, and device-invalidation / default-device-change recovery.
///
/// Deliberately raw COM interop ([ComImport]) rather than
/// [GeneratedComInterface]: the project has ordinary (non-trimmed,
/// non-AOT, JIT) WindowsAppSDK output (SelfContained/WindowsAppSDKSelfContained
/// are about the app's own deployment, not IL trimming/NativeAOT -- there is
/// no PublishAot/PublishTrimmed here), so classic COM interop (RCW/CCW) is
/// available and is far less boilerplate than hand-rolling a ComWrappers
/// implementation for five WASAPI interfaces plus a notification sink.
/// </summary>
public sealed class WasapiAudioMonitorService : IAudioMonitorService
{
    // ---- public surface --------------------------------------------------

    private volatile bool _muted = true;
    public bool IsMuted
    {
        get => _muted;
        set => _muted = value; // plain gain-to-zero in the render loop; never stops the pull
    }

    private Func<short[], int, int>? _reader;
    private Action<bool>? _setEnabled;

    public void SetSource(Func<short[], int, int>? reader, Action<bool>? setEnabled)
    {
        _reader = reader;
        _setEnabled = setEnabled;
        if (_running)
        {
            setEnabled?.Invoke(true);
        }
    }

    private Thread? _thread;
    private volatile bool _running;
    private ManualResetEventSlim? _stopSignal;

    public void Start()
    {
        if (_running)
        {
            return;
        }

        _running = true;
        _stopSignal = new ManualResetEventSlim(false);
        _setEnabled?.Invoke(true);

        _thread = new Thread(RunAudioThread)
        {
            IsBackground = true,
            Name = "WasapiMonitor",
            Priority = ThreadPriority.Highest,
        };
        _thread.Start();
    }

    public void Stop()
    {
        if (!_running)
        {
            return;
        }

        _running = false;
        _stopSignal?.Set();
        _thread?.Join(2000);
        _thread = null;
        _stopSignal?.Dispose();
        _stopSignal = null;
        _setEnabled?.Invoke(false);
    }

    public void Dispose()
    {
        Stop();
        GC.SuppressFinalize(this);
    }

    // ---- audio thread ------------------------------------------------------

    private const int TargetLatencyMs = 40;
    private const int PrebufferMs = 30;
    private const int SampleRate = 48000;
    private const int Channels = 2;

    private void RunAudioThread()
    {
        nint avrtHandle = 0;
        try
        {
            avrtHandle = TryBeginProAudioThread();

            // Reopen loop: re-acquire the default endpoint on invalidation
            // or on request from the notification sink, until Stop() fires.
            while (_running)
            {
                try
                {
                    RunOnCurrentDevice();
                }
                catch (COMException ex) when (IsDeviceInvalidated(ex))
                {
                    Debug.WriteLine($"[WasapiAudioMonitorService] device invalidated ({ex.HResult:X8}), reopening shortly");
                }
                catch (Exception ex)
                {
                    Debug.WriteLine($"[WasapiAudioMonitorService] audio loop error: {ex}");
                }

                if (_running)
                {
                    // Short delay before trying to reopen the default endpoint
                    // (device may still be settling after a plug/unplug or
                    // default-device switch).
                    _stopSignal?.Wait(300);
                }
            }
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"[WasapiAudioMonitorService] fatal audio thread error: {ex}");
        }
        finally
        {
            if (avrtHandle != 0)
            {
                NativeMethods.AvRevertMmThreadCharacteristics(avrtHandle);
            }
        }
    }

    private static bool IsDeviceInvalidated(COMException ex) =>
        ex.HResult == unchecked((int)0x88890004); // AUDCLNT_E_DEVICE_INVALIDATED

    private void RunOnCurrentDevice()
    {
        _framesSincePrimeCandidate = 0;

        using var enumerator = ComPtr<IMMDeviceEnumerator>.Create(
            (IMMDeviceEnumerator)Activator.CreateInstance(Type.GetTypeFromCLSID(Clsid.MMDeviceEnumerator)!)!);

        // Wire up device-change notifications (best-effort; not fatal if it fails).
        bool requestReopen = false;
        DeviceNotificationSink? sink = null;
        try
        {
            sink = new DeviceNotificationSink(() => requestReopen = true);
            enumerator.Interface.RegisterEndpointNotificationCallback(sink);
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"[WasapiAudioMonitorService] notification registration failed: {ex.Message}");
            sink = null;
        }

        try
        {
            enumerator.Interface.GetDefaultAudioEndpoint(EDataFlow.eRender, ERole.eConsole, out var deviceObj);
            using var device = ComPtr<IMMDevice>.Create(deviceObj);

            var iid = typeof(IAudioClient).GUID;
            device.Interface.Activate(ref iid, ClsCtx.InprocServer, nint.Zero, out var clientObj);
            using var client = ComPtr<IAudioClient>.Create((IAudioClient)clientObj);

            var format = BuildWaveFormat();
            nint formatPtr = Marshal.AllocHGlobal(Marshal.SizeOf<WAVEFORMATEXTENSIBLE>());
            try
            {
                Marshal.StructureToPtr(format, formatPtr, false);

                const uint streamFlags =
                    AudclntStreamflagsEventcallback |
                    AudclntStreamflagsAutoconvertpcm |
                    AudclntStreamflagsSrcDefaultQuality;

                long bufferDuration = TargetLatencyMs * 10_000L; // 100ns units
                int hr = client.Interface.Initialize(AudclntShareMode.Shared, streamFlags, bufferDuration, 0, formatPtr, nint.Zero);
                if (hr < 0)
                {
                    Marshal.ThrowExceptionForHR(hr);
                }
            }
            finally
            {
                Marshal.FreeHGlobal(formatPtr);
            }

            using var eventHandle = new AutoResetEvent(false);
            client.Interface.SetEventHandle(eventHandle.SafeWaitHandle.DangerousGetHandle());

            client.Interface.GetBufferSize(out uint bufferFrameCount);

            var renderClientIid = typeof(IAudioRenderClient).GUID;
            client.Interface.GetService(ref renderClientIid, out var renderObj);
            using var renderClient = ComPtr<IAudioRenderClient>.Create((IAudioRenderClient)renderObj);

            // Pre-fill one full buffer of silence before Start() so the very
            // first callback has something to play against.
            renderClient.Interface.GetBuffer(bufferFrameCount, out nint bufPtr);
            renderClient.Interface.ReleaseBuffer(bufferFrameCount, AudclntBufferflagsSilent);

            client.Interface.Start();
            try
            {
                RenderLoop(client.Interface, renderClient.Interface, eventHandle, bufferFrameCount, () => requestReopen);
            }
            finally
            {
                try { client.Interface.Stop(); } catch { /* best-effort */ }
                try { client.Interface.Reset(); } catch { /* best-effort */ }
            }
        }
        finally
        {
            if (sink != null)
            {
                try { enumerator.Interface.UnregisterEndpointNotificationCallback(sink); } catch { /* best-effort */ }
            }
        }
    }

    private void RenderLoop(IAudioClient client, IAudioRenderClient renderClient, WaitHandle eventHandle,
        uint bufferFrameCount, Func<bool> reopenRequested)
    {
        var scratch = new short[bufferFrameCount * Channels];

        // Anti-crackle prebuffer: after any underrun (reader gave us fewer
        // frames than requested), stay silent until reads consistently
        // return "enough" frames again, rather than alternating between
        // partial audio and silence every callback.
        int prebufferFramesNeeded = SampleRate * PrebufferMs / 1000;
        bool primed = false;

        var waitHandles = new[] { eventHandle, _stopSignal!.WaitHandle };

        while (_running)
        {
            int signaled = WaitHandle.WaitAny(waitHandles, 1000);
            if (!_running || signaled == 1)
            {
                return;
            }
            if (reopenRequested())
            {
                return; // fall back to RunAudioThread's reopen loop
            }
            if (signaled == WaitHandle.WaitTimeout)
            {
                continue; // no event fired within a second; check _running and loop
            }

            client.GetCurrentPadding(out uint padding);
            uint framesAvailable = bufferFrameCount - padding;
            if (framesAvailable == 0)
            {
                continue;
            }

            nint bufPtr;
            try
            {
                renderClient.GetBuffer(framesAvailable, out bufPtr);
            }
            catch (COMException)
            {
                throw; // let RunOnCurrentDevice's caller classify AUDCLNT_E_DEVICE_INVALIDATED
            }

            uint flags = 0;
            var reader = _reader;
            bool muted = _muted;

            if (reader == null || muted)
            {
                flags = AudclntBufferflagsSilent;
                primed = reader != null && !muted; // stay unprimed while muted/no source
            }
            else
            {
                int needed = (int)framesAvailable * Channels;
                if (scratch.Length < needed)
                {
                    scratch = new short[needed];
                }

                int framesRead;
                try
                {
                    framesRead = reader(scratch, (int)framesAvailable);
                }
                catch (Exception ex)
                {
                    Debug.WriteLine($"[WasapiAudioMonitorService] reader threw: {ex.Message}");
                    framesRead = 0;
                }

                if (framesRead < 0)
                {
                    framesRead = 0;
                }
                if (framesRead > framesAvailable)
                {
                    framesRead = (int)framesAvailable;
                }

                bool fullRead = framesRead == framesAvailable;
                if (!primed)
                {
                    // Only start playing once we've seen enough consecutive
                    // full reads to cover the prebuffer window; otherwise
                    // emit silence so we don't stutter in and out.
                    if (fullRead)
                    {
                        _framesSincePrimeCandidate += framesRead;
                        if (_framesSincePrimeCandidate >= prebufferFramesNeeded)
                        {
                            primed = true;
                        }
                    }
                    else
                    {
                        _framesSincePrimeCandidate = 0;
                    }
                }
                else if (framesRead == 0)
                {
                    // True underrun after being primed: drop back to silence
                    // and re-prime rather than crackle.
                    primed = false;
                    _framesSincePrimeCandidate = 0;
                }

                if (!primed)
                {
                    flags = AudclntBufferflagsSilent;
                }
                else
                {
                    unsafe
                    {
                        short* dst = (short*)bufPtr;
                        fixed (short* src = scratch)
                        {
                            int copySamples = framesRead * Channels;
                            Buffer.MemoryCopy(src, dst, (long)framesAvailable * Channels * sizeof(short), copySamples * sizeof(short));
                            int remainingSamples = ((int)framesAvailable - framesRead) * Channels;
                            if (remainingSamples > 0)
                            {
                                new Span<short>(dst + copySamples, remainingSamples).Clear();
                            }
                        }
                    }
                }
            }

            renderClient.ReleaseBuffer(framesAvailable, flags);
        }
    }

    private int _framesSincePrimeCandidate;

    private static WAVEFORMATEXTENSIBLE BuildWaveFormat()
    {
        const int bitsPerSample = 16;
        const int blockAlign = Channels * bitsPerSample / 8;

        return new WAVEFORMATEXTENSIBLE
        {
            Format = new WAVEFORMATEX
            {
                wFormatTag = unchecked((ushort)WaveFormatExtensible),
                nChannels = Channels,
                nSamplesPerSec = SampleRate,
                nAvgBytesPerSec = SampleRate * blockAlign,
                nBlockAlign = (ushort)blockAlign,
                wBitsPerSample = bitsPerSample,
                cbSize = 22,
            },
            wValidBitsPerSample = bitsPerSample,
            dwChannelMask = SpeakerFrontLeft | SpeakerFrontRight,
            SubFormat = KsdatformatSubtypePcm,
        };
    }

    private static nint TryBeginProAudioThread()
    {
        try
        {
            uint taskIndex = 0;
            nint h = NativeMethods.AvSetMmThreadCharacteristicsW("Pro Audio", ref taskIndex);
            if (h == 0)
            {
                Debug.WriteLine($"[WasapiAudioMonitorService] AvSetMmThreadCharacteristics failed: {Marshal.GetLastWin32Error()}");
            }
            return h;
        }
        catch (Exception ex)
        {
            // avrt.dll missing/blocked -- not fatal, just no MMCSS boost.
            Debug.WriteLine($"[WasapiAudioMonitorService] AvSetMmThreadCharacteristics unavailable: {ex.Message}");
            return 0;
        }
    }

    // ---- WASAPI/COM interop ------------------------------------------------

    private const int WaveFormatExtensible = 0xFFFE;
    private const uint SpeakerFrontLeft = 0x1;
    private const uint SpeakerFrontRight = 0x2;
    private static readonly Guid KsdatformatSubtypePcm = new("00000001-0000-0010-8000-00aa00389b71");

    private const uint AudclntStreamflagsEventcallback = 0x00040000;
    private const uint AudclntStreamflagsAutoconvertpcm = 0x80000000;
    private const uint AudclntStreamflagsSrcDefaultQuality = 0x08000000;
    private const uint AudclntBufferflagsSilent = 0x2;

    private static class Clsid
    {
        public static readonly Guid MMDeviceEnumerator = new("BCDE0395-E52F-467C-8E3D-C4579291692E");
    }

    private enum EDataFlow { eRender = 0, eCapture = 1, eAll = 2 }
    private enum ERole { eConsole = 0, eMultimedia = 1, eCommunications = 2 }
    private enum AudclntShareMode { Shared = 0, Exclusive = 1 }
    private static class ClsCtx { public const uint InprocServer = 0x1; }

    // Windows' real mmreg.h wraps WAVEFORMATEX/WAVEFORMATEXTENSIBLE in
    // #include <pshpack1.h> (byte packing), so the native size is exactly
    // 18 / 40 bytes with no alignment padding. Pack = 1 replicates that;
    // without it .NET's default alignment pads the struct to 20 / 44 bytes,
    // shifting every field after the embedded WAVEFORMATEX and making
    // IAudioClient::Initialize fail with E_INVALIDARG.
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    private struct WAVEFORMATEX
    {
        public ushort wFormatTag;
        public ushort nChannels;
        public uint nSamplesPerSec;
        public uint nAvgBytesPerSec;
        public ushort nBlockAlign;
        public ushort wBitsPerSample;
        public ushort cbSize;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    private struct WAVEFORMATEXTENSIBLE
    {
        public WAVEFORMATEX Format;
        public ushort wValidBitsPerSample;
        public uint dwChannelMask;
        public Guid SubFormat;
    }

    [ComImport]
    [Guid("A95664D2-9614-4F35-A746-DE8DB63617E6")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMMDeviceEnumerator
    {
        void EnumAudioEndpoints(EDataFlow dataFlow, uint dwStateMask, out nint devices);
        void GetDefaultAudioEndpoint(EDataFlow dataFlow, ERole role, [MarshalAs(UnmanagedType.Interface)] out object endpoint);
        void GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id, [MarshalAs(UnmanagedType.Interface)] out object device);
        void RegisterEndpointNotificationCallback([MarshalAs(UnmanagedType.Interface)] IMMNotificationClient client);
        void UnregisterEndpointNotificationCallback([MarshalAs(UnmanagedType.Interface)] IMMNotificationClient client);
    }

    [ComImport]
    [Guid("D666063F-1587-4E43-81F1-B948E807363F")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMMDevice
    {
        void Activate(ref Guid iid, uint dwClsCtx, nint pActivationParams, [MarshalAs(UnmanagedType.Interface)] out object interfacePointer);
        void OpenPropertyStore(uint stgmAccess, out nint properties);
        void GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
        void GetState(out uint state);
    }

    [ComImport]
    [Guid("1CB9AD4C-DBFA-4C32-B178-C2F568A703B2")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IAudioClient
    {
        [PreserveSig]
        int Initialize(AudclntShareMode shareMode, uint streamFlags, long bufferDuration, long periodicity, nint format, nint audioSessionGuid);
        void GetBufferSize(out uint numBufferFrames);
        void GetStreamLatency(out long latency);
        void GetCurrentPadding(out uint numPaddingFrames);
        void IsFormatSupported(AudclntShareMode shareMode, nint format, out nint closestMatch);
        void GetMixFormat(out nint deviceFormat);
        void GetDevicePeriod(out long defaultDevicePeriod, out long minimumDevicePeriod);
        void Start();
        void Stop();
        void Reset();
        void SetEventHandle(nint eventHandle);
        void GetService(ref Guid riid, [MarshalAs(UnmanagedType.Interface)] out object service);
    }

    [ComImport]
    [Guid("F294ACFC-3146-4483-A7BF-ADDCA7C260E2")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IAudioRenderClient
    {
        void GetBuffer(uint numFramesRequested, out nint data);
        void ReleaseBuffer(uint numFramesWritten, uint flags);
    }

    [ComImport]
    [Guid("7991EEC9-7E89-4D85-8390-6C703CEC60C0")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMMNotificationClient
    {
        void OnDeviceStateChanged([MarshalAs(UnmanagedType.LPWStr)] string deviceId, uint newState);
        void OnDeviceAdded([MarshalAs(UnmanagedType.LPWStr)] string deviceId);
        void OnDeviceRemoved([MarshalAs(UnmanagedType.LPWStr)] string deviceId);
        void OnDefaultDeviceChanged(EDataFlow flow, ERole role, [MarshalAs(UnmanagedType.LPWStr)] string? defaultDeviceId);
        void OnPropertyValueChanged([MarshalAs(UnmanagedType.LPWStr)] string deviceId, PropertyKeyPlaceholder key);
    }

    // Real signature takes a PROPERTYKEY (a GUID + a uint); we never read it,
    // this just has to match its blittable size for the vtable slot.
    [StructLayout(LayoutKind.Sequential)]
    private struct PropertyKeyPlaceholder
    {
        public Guid fmtid;
        public uint pid;
    }

    /// <summary>Minimal IMMNotificationClient sink: flags "please reopen" on default-device change or device removal.</summary>
    private sealed class DeviceNotificationSink : IMMNotificationClient
    {
        private readonly Action _requestReopen;
        public DeviceNotificationSink(Action requestReopen) => _requestReopen = requestReopen;

        public void OnDeviceStateChanged(string deviceId, uint newState) { }
        public void OnDeviceAdded(string deviceId) { }
        public void OnDeviceRemoved(string deviceId) { }

        public void OnDefaultDeviceChanged(EDataFlow flow, ERole role, string? defaultDeviceId)
        {
            if (flow == EDataFlow.eRender && role == ERole.eConsole)
            {
                _requestReopen();
            }
        }

        public void OnPropertyValueChanged(string deviceId, PropertyKeyPlaceholder key) { }
    }

    /// <summary>Tiny RAII wrapper releasing a COM RCW's reference on Dispose.</summary>
    private readonly struct ComPtr<T> : IDisposable where T : class
    {
        public T Interface { get; }
        private ComPtr(T iface) => Interface = iface;
        public static ComPtr<T> Create(object obj) => new((T)obj);

        public void Dispose()
        {
            if (Interface != null && Marshal.IsComObject(Interface))
            {
                Marshal.ReleaseComObject(Interface);
            }
        }
    }

    private static class NativeMethods
    {
        [DllImport("avrt.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern nint AvSetMmThreadCharacteristicsW(string taskName, ref uint taskIndex);

        [DllImport("avrt.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool AvRevertMmThreadCharacteristics(nint avrtHandle);
    }
}
