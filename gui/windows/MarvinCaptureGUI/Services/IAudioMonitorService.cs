namespace PinnacleCapture.Services;

/// <summary>
/// Live audio monitoring (WASAPI playback of the core's PCM monitor ring,
/// pin_monitor_enable/pin_monitor_read) -- see <see cref="WasapiAudioMonitorService"/>
/// for the Phase 10 implementation. This interface is the seam: the owner
/// wires it up by giving the service a reader delegate backed by
/// Native.MonitorRead(session, ...) and an enable delegate backed by
/// Native.MonitorEnable(session, ...), rather than handing over the raw
/// session handle. That keeps this service decoupled from Interop/* and
/// testable (a test can pass a synthetic reader, as in the scratchpad
/// sine-wave check).
/// </summary>
public interface IAudioMonitorService : IDisposable
{
    bool IsMuted { get; set; }

    /// <summary>
    /// Supplies the frame reader and enable callback for the current
    /// session. Call this (or re-call it with a new reader) before/after
    /// <see cref="Start"/> whenever the underlying session changes; pass
    /// null reader to detach (e.g. when the session closes) -- the service
    /// will just output silence until a new one is supplied.
    /// </summary>
    /// <param name="reader">(buffer, maxFrames) -> framesRead, interleaved 48kHz s16 stereo.</param>
    /// <param name="setEnabled">Tells the core whether to keep the monitor ring filled.</param>
    void SetSource(Func<short[], int, int>? reader, Action<bool>? setEnabled);

    void Start();
    void Stop();
}

/// <summary>No-op stand-in used where WASAPI playback isn't wanted (e.g. headless/tests).</summary>
public sealed class NullAudioMonitorService : IAudioMonitorService
{
    public bool IsMuted { get; set; } = true;
    public void SetSource(Func<short[], int, int>? reader, Action<bool>? setEnabled) { }
    public void Start() { }
    public void Stop() { }
    public void Dispose() { }
}
