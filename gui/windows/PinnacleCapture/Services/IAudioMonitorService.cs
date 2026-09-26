namespace PinnacleCapture.Services;

/// <summary>
/// Live audio monitoring (WASAPI playback of the core's PCM monitor ring,
/// pin_monitor_enable/pin_monitor_read) is explicitly a later phase (Phase
/// 10 in the plan's "Changes after your review" -- Audio monitoring). This
/// interface is the seam for it: today the "Mute" toggle in the UI only
/// calls pin_monitor_enable to tell the core whether to keep the ring
/// filled at all; nothing renders sound yet.
///
/// TODO(phase 10): implement with WASAPI (or XAudio2) pulling PCM frames
/// from Native.MonitorRead on a dedicated audio thread, respecting Mute as
/// a simple gain-to-zero rather than stopping the pull (so re-enabling is
/// glitch-free).
/// </summary>
public interface IAudioMonitorService
{
    bool IsMuted { get; set; }
    void Start();
    void Stop();
}

/// <summary>No-op stand-in used until Phase 10 lands.</summary>
public sealed class NullAudioMonitorService : IAudioMonitorService
{
    public bool IsMuted { get; set; } = true;
    public void Start() { /* TODO(phase 10): WASAPI */ }
    public void Stop() { }
}
