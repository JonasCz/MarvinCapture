// Pinnacle Studio 500-USB open driver
// Copyright (C) 2026 Jonas Cz.
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
// for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

import AVFoundation
import Foundation
import os
import CMarvinCore

// Live audio monitor: pumps the core's PCM ring (pin_monitor_read, 48 kHz s16 interleaved stereo)
// into the system output with AVAudioEngine. Port of WasapiAudioMonitorService.cs: the core already
// bounds the latency (it trims the ring to ~80 ms above ~200 ms), so this is a dumb pump plus the
// anti-crackle prebuffer, mute and device-change recovery.
//
// Threading and session lifetime
// ------------------------------
// The render block runs on CoreAudio's real-time thread. It must not allocate, wait on anything the
// main thread can hold for long, send Objective-C messages or hop actors, so everything it touches
// lives in `RenderCore`, a manually allocated struct addressed by a raw pointer (no ARC traffic, no
// Swift runtime exclusivity checks on class properties). The session pointer in it is guarded by an
// os_unfair_lock with this protocol:
//   * the render thread only TRY-locks; if the main thread holds the lock it outputs silence for that
//     one buffer (never blocks, never inverts priority);
//   * it keeps the lock for the whole buffer, including the pin_monitor_read call;
//   * `detach()` takes the lock (blocking, at most one buffer's worth of work) and sets the session
//     to nil. When it returns, the render thread is out of the core and will not enter it again with
//     the old session, so the model may pin_close right after. (The model also stops the engine
//     first, which makes this a belt-and-braces handshake, like SetSource(null) on Windows.)

/// Mutable state of the render callback. Only the render thread touches it while it holds `lock`;
/// the main thread takes `lock` (blocking) for every access.
private struct RenderCore {
    var lock = os_unfair_lock()
    var session: OpaquePointer?
    /// Anti-crackle prebuffer: false = output silence until reads are consistently full again.
    var primed = false
    var primeFrames = 0
    /// pin_monitor_read target, `scratchFrames` stereo frames (preallocated).
    var scratch: UnsafeMutablePointer<Int16>
    // Statistics for --debug (frames, counted over the whole life of the monitor).
    var callbacks = 0
    var requested = 0
    var fromCore = 0
    var played = 0
    var shortCalls = 0       // primed callbacks where the core had fewer frames than asked
    var underruns = 0        // primed -> unprimed transitions (a read returning nothing)
}

/// The render block, built outside any actor so the closure is not main-actor isolated (no executor
/// checks on the real-time thread). It captures only the raw pointer.
private func makeRenderBlock(_ core: UnsafeMutablePointer<RenderCore>, scratchFrames: Int,
                             prebufferFrames: Int) -> AVAudioSourceNodeRenderBlock {
    return { isSilence, _, frameCount, abl in
        let list = UnsafeMutableAudioBufferListPointer(abl)
        let n = Int(frameCount)
        guard list.count >= 2,
              let left = list[0].mData?.assumingMemoryBound(to: Float.self),
              let right = list[1].mData?.assumingMemoryBound(to: Float.self) else {
            isSilence.pointee = true
            return noErr
        }
        var played = 0
        if os_unfair_lock_trylock(&core.pointee.lock) {
            if let s = core.pointee.session {
                let want = Swift.min(n, scratchFrames)
                let got = Swift.max(0, Swift.min(Int(pin_monitor_read(s, core.pointee.scratch, Int32(want))), want))
                core.pointee.callbacks += 1
                core.pointee.requested += want
                core.pointee.fromCore += got

                // Port of Windows RenderLoop: after an underrun stay silent until enough consecutive
                // full reads cover the prebuffer window, rather than alternating partial audio and silence.
                let full = got == want
                if !core.pointee.primed {
                    if full {
                        core.pointee.primeFrames += got
                        if core.pointee.primeFrames >= prebufferFrames { core.pointee.primed = true }
                    } else {
                        core.pointee.primeFrames = 0
                    }
                } else if got == 0 {
                    core.pointee.primed = false
                    core.pointee.primeFrames = 0
                    core.pointee.underruns += 1
                }
                if core.pointee.primed {
                    if !full { core.pointee.shortCalls += 1 }
                    let src = core.pointee.scratch
                    let k: Float = 1.0 / 32768.0
                    for i in 0..<got {
                        left[i] = Float(src[2 * i]) * k
                        right[i] = Float(src[2 * i + 1]) * k
                    }
                    // A primed read that came up a few frames short (source and device clocks drift
                    // apart): hold the last sample for the gap instead of a jump to zero, which clicks
                    // less. Differs from the Windows port, which zero-fills.
                    if got > 0 && got < want {
                        let lastL = left[got - 1], lastR = right[got - 1]
                        for i in got..<want { left[i] = lastL; right[i] = lastR }
                    }
                    played = got < want ? want : got
                    core.pointee.played += got
                }
            }
            os_unfair_lock_unlock(&core.pointee.lock)
        }
        if played < n {
            (left + played).update(repeating: 0, count: n - played)
            (right + played).update(repeating: 0, count: n - played)
        }
        isSilence.pointee = ObjCBool(played == 0)
        return noErr
    }
}

@MainActor
final class CoreAudioMonitor: AudioMonitor {
    /// `--debug`: one statistics line per second while the engine runs (set by AppModel).
    nonisolated(unsafe) static var debugStats = false

    private static let sampleRate = 48_000.0
    /// Same 30 ms prebuffer as the Windows service.
    private static let prebufferFrames = 48_000 * 30 / 1000
    /// Largest buffer the render block serves per call (the engine's maximum is 4096 frames; more
    /// would be output as silence rather than reallocating).
    private static let scratchFrames = 8192
    private static let retryDelay: Duration = .seconds(2)

    /// Deliberately never freed: the monitor lives as long as the window, and freeing would need a proof
    /// that the render thread is gone (the engine is released first, but it is not worth the risk).
    private let core: UnsafeMutablePointer<RenderCore>

    private var engine: AVAudioEngine?
    private var configObserver: NSObjectProtocol?
    private var retryTask: Task<Void, Never>?
    private var statsTimer: Timer?
    private var lastStats = (callbacks: 0, requested: 0, fromCore: 0, played: 0, shortCalls: 0, underruns: 0)
    /// The user (the model) wants audio: not muted, session attached and start() called.
    private var wantRunning = false

    var isMuted = true {
        didSet { if isMuted { stop() } }
    }

    init() {
        let scratch = UnsafeMutablePointer<Int16>.allocate(capacity: Self.scratchFrames * 2)
        scratch.initialize(repeating: 0, count: Self.scratchFrames * 2)
        core = UnsafeMutablePointer<RenderCore>.allocate(capacity: 1)
        core.initialize(to: RenderCore(scratch: scratch))
    }

    // MARK: AudioMonitor

    func attach(session: Pin.Session) {
        withCore { $0.session = session }
    }

    func start() {
        guard !isMuted else { return }
        wantRunning = true
        if engine == nil { startEngine() }
    }

    /// No audio device is held while stopped (an engine is only created between start and stop), as on
    /// Windows where Stop() ends the WASAPI client.
    func stop() {
        wantRunning = false
        retryTask?.cancel()
        retryTask = nil
        tearDownEngine()
    }

    /// Synchronous: when this returns the render thread has left the core (see the file comment).
    func detach() {
        stop()
        withCore { $0.session = nil }
    }

    // MARK: engine

    private func startEngine() {
        guard wantRunning, engine == nil else { return }
        withCore { $0.primed = false; $0.primeFrames = 0 }

        let format = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: Self.sampleRate,
                                   channels: 2, interleaved: false)!
        let node = AVAudioSourceNode(format: format, renderBlock:
            makeRenderBlock(core, scratchFrames: Self.scratchFrames, prebufferFrames: Self.prebufferFrames))
        let e = AVAudioEngine()
        e.attach(node)
        e.connect(node, to: e.mainMixerNode, format: format)   // the mixer resamples to the device rate
        e.prepare()
        do {
            try e.start()
        } catch {
            log("engine start failed: \(error.localizedDescription); retrying in 2 s")
            scheduleRetry()
            return
        }
        engine = e
        // The default output device changed (or its format): the engine stops itself, rebuild it.
        configObserver = NotificationCenter.default.addObserver(
            forName: .AVAudioEngineConfigurationChange, object: e, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.configurationChanged() }
        }
        let out = e.outputNode.outputFormat(forBus: 0)
        log("engine started: source 48000 Hz 2 ch Float32, output \(Int(out.sampleRate)) Hz \(out.channelCount) ch")
        startStatsTimer()
    }

    private func tearDownEngine() {
        statsTimer?.invalidate()
        statsTimer = nil
        if let o = configObserver { NotificationCenter.default.removeObserver(o) }
        configObserver = nil
        if let e = engine {
            e.stop()
            engine = nil     // releases the output unit and with it the audio device
            log("engine stopped")
        }
    }

    private func configurationChanged() {
        guard wantRunning else { return }
        log("audio configuration changed, restarting")
        tearDownEngine()
        startEngine()
    }

    /// Starting can fail while a device is still settling (unplug, default device switch, no output at
    /// all): try again later instead of giving up.
    private func scheduleRetry() {
        guard wantRunning else { return }
        retryTask?.cancel()
        retryTask = Task { [weak self] in
            try? await Task.sleep(for: Self.retryDelay)
            guard !Task.isCancelled else { return }
            self?.startEngine()
        }
    }

    // MARK: helpers

    private func withCore<T>(_ body: (inout RenderCore) -> T) -> T {
        os_unfair_lock_lock(&core.pointee.lock)
        defer { os_unfair_lock_unlock(&core.pointee.lock) }
        return body(&core.pointee)
    }

    private func startStatsTimer() {
        guard Self.debugStats else { return }
        lastStats = withCore { (callbacks: $0.callbacks, requested: $0.requested, fromCore: $0.fromCore,
                                played: $0.played, shortCalls: $0.shortCalls, underruns: $0.underruns) }
        statsTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.logStats() }
        }
    }

    private func logStats() {
        let c = withCore { (callbacks: $0.callbacks, requested: $0.requested, fromCore: $0.fromCore,
                            played: $0.played, shortCalls: $0.shortCalls, underruns: $0.underruns,
                            primed: $0.primed) }
        let l = lastStats
        log("audio: \(c.callbacks - l.callbacks) callbacks, asked \(c.requested - l.requested) frames, "
            + "core gave \(c.fromCore - l.fromCore), played \(c.played - l.played), "
            + "short \(c.shortCalls - l.shortCalls), underruns \(c.underruns - l.underruns), primed \(c.primed)")
        lastStats = (c.callbacks, c.requested, c.fromCore, c.played, c.shortCalls, c.underruns)
    }

    /// Engine start/stop and failures always go to the console (rare); the per-second statistics only
    /// with --debug.
    private func log(_ text: String) {
        ConsoleOutput.write("audio monitor: \(text)")
    }
}
