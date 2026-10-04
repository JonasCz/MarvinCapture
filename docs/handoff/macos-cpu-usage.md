# macOS GUI CPU usage: handoff

Written on Windows by reading code only. Nothing here has been measured on a Mac. Line numbers are
from the tree at commit `cdd0e0c` and may drift by a few lines.

## Problem

`MarvinCapture.app` (gui/macos) uses roughly:

| State | macOS | Windows GUI |
|---|---|---|
| Idle (no device / no video displayed) | ~20 % of one core | ~0.5 % |
| SD analog preview | ~40 % of one core | ~3 % |

Both GUIs share the same C core and have the same update structure (100 ms status tick, 33 ms
meter tick, device watch thread, render thread). So the gap is in (a) how Swift/SwiftUI reacts to
those ticks, (b) the Metal/display-link path, and (c) libusb's macOS backend under the analog
stream. The plan is: measure first (section "How to measure"), then work down the ranked list.

First thing to establish, because it decides which causes apply: **was a device attached during the
"idle" number?** With no device, `session == nil`, so none of the core/libusb streaming costs exist
and the 20 % must be GUI-side (causes 1, 2, 4, 6). With a device open and READY, the analog USB
stream (cause 3) and the per-status core calls (cause 7) are also live.

## How the update loop is structured (shared with Windows)

- `WindowModel.start()` (Models/WindowModel.swift ~558) creates two main-run-loop `Timer`s in
  `.common` mode: `tick()` every 100 ms and `updateMeters()` every 1/30 s.
- `tick()` (~919): drains `pin_poll_event(nil)`, `pin_get_status`, `applyStatus` (assigns ~40
  observed properties, calls ~12 `pin_format_*` C functions that return Swift `String`s), drains
  session events, `preview.hasRecentFrame`, `trackPreviewAspect`, `keepAwake.set`, then
  `afterTick` and `tickObservers` (Dock tile).
- `updateMeters()` (~900): `pin_get_status` again, then assigns `audioPeakLeft/Right` and
  `audioHoldLeft/Right` (observed properties read by `LevelMeters`).
- Windows has the same 100 ms / 33 ms DispatcherQueueTimers and the same core calls
  (MainWindow.xaml.cs 93-98, MainViewModel.cs `UpdateMeters`, `Tick`). The differences are in what
  an assignment does: on Windows `[ObservableProperty]` only raises `PropertyChanged` when the value
  actually changed (CommunityToolkit compares with `EqualityComparer<T>.Default`), so an idle tick
  that writes the same strings and numbers is almost free. See cause 1.

---

## Ranked causes

### 1. `@Observable` properties are re-assigned every tick even when unchanged (likely, verify)

**Where**
- `Models/WindowModel.swift` `applyStatus` (~938-1025): unconditional assignments of
  `sessionState`, `sessionStateText`, `statusLine`, `statusShortText`, `statusSubText`, `statusTip`,
  `timecode`, `statusTimeText`, `statusTimeTip`, `signalLocked`, `signalLockText`, `signalTypeText`,
  `framesTotalText`, `framesTip`, `sizeText`, `audioPeakText`, `tapePercent`, `diskLow`,
  `storageFreeText`, `storageTip`, `hasDiskInfo`, `isCapturing`, `noVideoText`,
  `noSignalCountdown*`, `stopCountdownSuffix`, `progress*`, `deckAvailable`, `windowTitle`, and
  `applyDeckStatus` (deck fields), every 100 ms while a session is open.
- `updateMeters` (~909-915): `audioPeakLeft/Right` and `audioHoldLeft/Right` assigned every 33 ms,
  also with no session and at the -60 dB floor (the values do not change, the writes still happen).
- Observers that wake on those writes: `Views/StatusBar/StatusBar.swift` (every item reads a
  model string), `Views/StatusBar/LevelMeter.swift` `LevelMeters` (the 30 Hz ones),
  `Views/PreviewArea.swift`, `Views/Sidebar/*` (read `isIdle`, `captureEnabled`,
  `captureButtonText`, `deckAvailable`, `sessionState` through the enable rules), and
  `App/MainWindowController.swift:82` `trackWindowTitle` (a `withObservationTracking` on
  `windowTitle`).

**Why it costs CPU.** The Swift `@Observable` macro's setter notifies observers on every write.
Whether it skips the notification when the new value is `==` the old one depends on the toolchain
(I believe plain Swift 6.0/6.1 does not; I am not certain, and Swift 6.2 may). Where it does not
skip, every tick re-evaluates the bodies of every view that read a touched property: the whole
sidebar (`ScrollView` with AppKit-backed `Picker`, `Slider`, `TextField`, `NSPopUpButton` via
`DevicePopUp`), `StatusBar`, `PreviewArea`. At 30 Hz the meter writes invalidate `LevelMeters` and
re-run both Canvas closures plus the `.help` / `.accessibilityLabel` updates. `trackWindowTitle`
re-arms and sets `NSWindow.title` every 100 ms.

**Confidence.** Medium-high that it explains a large part of the idle gap (it is the one thing that
differs structurally from Windows and does not need a device). The exact dedupe behaviour of the
toolchain in use is the uncertain part, hence "verify".

**Confirm on the Mac**
1. Add temporarily `let _ = Self._printChanges()` as the first line of `StatusBar.body`,
   `LevelMeters.body`, `AnalogPanel.body`, `PreviewArea.body`; run with no device. If bodies print
   continuously with the same values, the writes are notifying.
2. Instruments > SwiftUI template (or "SwiftUI View Body" instrument): body evaluation counts per
   second for those views while idle. Healthy idle is ~0.
3. Quick A/B: in `updateMeters()` add `guard session != nil || audioPeakLeft != floor else { return }`
   style early-outs, or comment out the meter timer, and see whether idle CPU drops by a lot.
4. Time Profiler, main thread, invert call tree: look for `AG::Graph::update`, `SwiftUI.ViewGraph`,
   `DisplayList`, `CA::Transaction::commit`, `NSHostingView` layout.

**Fix**
- Make every assignment in `applyStatus` / `updateMeters` conditional. Smallest change: a tiny helper
  `@inline(__always) func set<T: Equatable>(_ kp: ReferenceWritableKeyPath<WindowModel, T>, _ v: T)`
  or just `if x != new { x = new }` per property. Do the same for the deck fields in
  `applyDeckStatus`.
- `updateMeters`: return early when `session == nil` and the published values are already at the
  floor; only publish when a value moved by at least ~0.25 dB (cuts the redraws when audio is
  silent or steady) and publish hold values only when they change.
- `trackWindowTitle`: set `window.title` only if different.
- Split the meter values into their own small `@Observable` (`MeterModel`) so only `LevelMeters`
  depends on it. Today `LevelMeters` is a separate struct, but it reads the big `WindowModel`; any
  other write to the model does not invalidate it, but the inverse (sidebar/status bar invalidated by
  unrelated writes) is the main concern, and a separate model makes the meter path independent.

### 2. 30 Hz SwiftUI `Canvas` meters redrawing in the window that also hosts a `.bar` material and Metal view (likely, partly depends on 1)

**Where**: `Views/StatusBar/LevelMeter.swift:39` (two Canvases, ~4 fills each, built with a new
`zones` array literal per draw), `Views/StatusBar/StatusBar.swift:50` (`.background(.bar)`),
`WindowModel.updateMeters`.

**Why**: Each meter change goes SwiftUI graph update -> display list -> Core Animation commit on the
main thread, 30 times a second, even if only a 100x4 pt region changed. `Canvas` redraws through
Core Graphics on the main thread. The footer sits on a `NSVisualEffectView`-style material, which
makes each commit more expensive for WindowServer, and the preview window has a live Metal layer, so
the window is already compositing every frame. Windows does the same 30 Hz update but WinUI's
composition handles it off the UI thread.

**Confidence**: Medium. Real, but 30 Hz of two tiny Canvases should be single-digit percent on its
own. It becomes significant combined with cause 1 (everything re-evaluating) and when the meters
genuinely move (preview with audio).

**Confirm**: Disable the meter timer entirely (comment out line ~559) and compare idle and preview
CPU. Also Instruments > Core Animation / Time Profiler: `CA::Transaction::commit` frequency.

**Fix**: Replace the SwiftUI Canvas with a small `NSViewRepresentable` backed by a `CALayer`/`NSView`
that the timer updates directly (set `layer.contents` or two `CALayer` frames with
`CATransaction.setDisableActions(true)`), keeping the SwiftUI tree out of the 30 Hz path. Or drop the
timer to ~15-20 Hz and stop it while the window is occluded/minimised (see cause 6). Hoist the
`zones` array out of `body`.

### 3. libusb darwin backend under the analog stream: ~2500 small transfers/s (likely main cause of the 40 % in preview)

**Where**: `src/core/pinnacle_analog.c` ~539-542 (`VIDEO_QUEUE 512`, `VIDEO_XFER 8 KiB`) and the read
loop `pinnacle_analog_read_loop` ~700-745
(`libusb_handle_events_timeout_completed(dev->usb_ctx, &tv=50 ms)` then `queue_poll` per pass,
`cb(0, NULL, 0, user)` per pass). `thread_boost()` is Windows-only (MMCSS); `set_raw_io` is a no-op
on macOS (no `RAW_IO` support).

**Why it costs CPU**: SD analog is ~20 MB/s, so 8 KiB transfers means ~2500 completions per second.
Per the code comment, 8 KiB is deliberate (larger transfers lose data on Linux xHCI). On Windows
RAW_IO arms the whole queue at the controller and completions are cheap. On macOS the libusb darwin
backend delivers each completion on its own IOKit/CFRunLoop thread and then wakes the thread that is
blocked in `libusb_handle_events_*` through a pipe (two context switches per transfer), and each
resubmit is an IOKit `ReadPipeAsync` call into the kernel. That is a plausible 20-30 % of a core by
itself, all in threads that are not in the GUI code, so it would show as process CPU in Activity
Monitor but not in main-thread profiles.

**Confidence**: Medium-high for the preview number, because the Windows RAW_IO path is documented as
the thing that makes this cheap and macOS has no equivalent. Unknown how much of the idle number it
covers: it matters only if a device is open with the stream running (it streams while READY for the
preview).

**Confirm**
1. `sample MarvinCapture 5 -file /tmp/s.txt` (or Time Profiler with "All threads") during preview
   and look at which thread is hot: the thread running `pinnacle_analog_read_loop`
   (`libusb_handle_events_timeout_completed`, `darwin_*`, `mach_msg`, `read`/`write` on the event
   pipe), the libusb darwin event thread (`darwin_event_thread_main`, `CFRunLoopRun`), the "Preview
   render" thread, or the main thread.
2. With `--debug`, the read loop logs completions/second and queue state (see `docs/analog.md`).
3. Experiment: build with `VIDEO_XFER` 16/32 KiB and `VIDEO_QUEUE` 128-256 on macOS only
   (`#if defined(__APPLE__)`), check CPU **and** frame integrity (short frames / dropped frames counters
   in the status bar; macOS xHCI may not show the Linux problem). Do not ship a larger size without
   a multi-minute capture check.

**Fix options** (in order of invasiveness): larger transfers on macOS if capture integrity holds;
raise the thread's QoS/priority (`pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED)` or a
time-constraint policy) so it is not preempted into more wakeups; drop the extra
`cb(0, NULL, 0, user)` call and `queue_poll` of the audio queue per pass if only the video queue
completed; if libusb is the bottleneck, check the bundled libusb version (a newer darwin backend
reduces per-transfer overhead) and consider a patched build.

### 4. Window title and tooltip/accessibility text churn (moderate, tied to cause 1)

**Where**: `App/MainWindowController.swift:80-87` `trackWindowTitle`; `Views/StatusBar/StatusBar.swift`
`.help(model.statusTip)`, `.accessibilityLabel(model.statusLine)`; `LevelMeters` `.help` /
`.accessibilityLabel(model.audioPeakText)` (the string changes every 100 ms while audio flows, e.g.
"Audio peak left -23.4 dBFS ..."); `Views/PreviewArea.swift` `.textSelection(.enabled)` on the
no-video text.

**Why**: Setting `NSWindow.title` re-lays out the title bar, updates the Window menu entry and posts
accessibility notifications; changing `.help` text on a macOS SwiftUI view updates an `NSView`
tooltip/tracking area; accessibility labels that change at 10 Hz cost a lot more if any
accessibility client is attached (VoiceOver, Accessibility Inspector, some screen-recording or
automation tools are enough). `.textSelection(.enabled)` makes the Text selectable (heavier backing
view) and it is re-evaluated each time the state text writes.

**Confidence**: Low-medium as a primary cause, but cheap to fix.

**Confirm**: Time Profiler main thread, search for `NSWindow setTitle`, `NSToolTip`,
`NSAccessibilityPostNotification`. Quick A/B: comment out the `.help`/`.accessibilityLabel` on
`LevelMeters` and the title tracking.

**Fix**: Set the title only when different. Only change tooltip/label text when the formatted
string changes (cause 1's guards fix most of it); quantise `audioPeakText` (it is only needed for
accessibility; update it at ~2 Hz or only when it differs after rounding to whole dB); drop
`.textSelection` if not needed.

### 5. `StatusBar` recomputes text widths with `NSString.size(withAttributes:)` in `body` (low-moderate)

**Where**: `Views/StatusBar/StatusBar.swift:30` calling
`Views/StatusBar/StatusBarLayout.swift:56-81` (`visibleItems`, `textWidth` at :47, which measures
~7 strings per evaluation using `NSString.size`).

**Why**: Text measurement goes through Core Text and costs tens of microseconds per string; it runs
inside the `GeometryReader` closure each time the status bar body re-evaluates (up to 10 Hz, more
when cause 1 forces it), and the model strings (time, frames, size, free) really do change
every second or so during preview/capture.

**Confidence**: Low as a main cause (maybe 1-2 %), but trivial to cache.

**Fix**: cache widths in a small dictionary keyed by string (the strings repeat or change slowly),
or compute `visibleItems` only when one of the width-relevant strings or the available width
changes.

### 6. Preview display link runs at the screen's full refresh rate and nothing pauses the timers when the window is hidden (moderate for preview, low for idle)

**Where**: `Preview/MetalPreview.swift:60` (`displayLink(target:selector:)`, no
`preferredFrameRateRange`), `Preview/PreviewRenderer.swift` `presentLoop` (~251-281; wakes the
render thread on each tick, then `pin_preview_lock_due` and `pin_preview_next_time`, both taking
core mutexes, even when no frame is due), `displayLinkTick` (:190). The two model timers are never
paused or slowed when the window is occluded/minimised/the app hidden (the renderer parks itself
via `setVisible`, but `statusTimer` and `meterTimer` keep running).

**Why**: SD sources are 25/29.97 fps. On a 60 Hz display that is 2.4 display ticks per frame, on a
120 Hz ProMotion display ~5: every tick wakes the main run loop, the render thread and takes locks,
mostly to find nothing due. The drawing itself (3 `replace(region:)` uploads of a ~0.6 MB frame and a
fullscreen triangle into a Retina-size drawable) is cheap on the CPU. The unpaused model timers keep
the 100 ms and 33 ms work running for an invisible window.

**Confidence**: Medium for the preview overhead being a few percent; low that it is a big part.

**Confirm**: Instruments > Time Profiler, "Preview render" thread, and the main thread's
`PreviewLayerView.tick`. Check the display refresh rate. Quick experiment: set
`l.preferredFrameRateRange = CAFrameRateRange(minimum: 24, maximum: 30, preferred: 30)` on the link.

**Fix**: cap the link with `preferredFrameRateRange` (30 or 60), or drive presentation from the
core's frame-ready signal instead of a free-running link (the render thread already blocks in
`pin_preview_wait` for new frames; present directly when due and only use the link for pacing when
the next frame time is near). Observe `NSWindow.didChangeOcclusionStateNotification` (already used
by `MetalPreview.windowChanged`) in `WindowModel` too and slow the 100 ms timer to ~1 Hz and stop
the meter timer while occluded, resuming (and doing one catch-up `tick()`) when visible. Note the
close flow's `afterTick` needs the tick while a capture is stopping, so only slow it when
`!isCapturing`.

### 7. `pin_get_status` does `statvfs` + `statfs` on every call, called ~40 times/s (low-moderate, core, device open only)

**Where**: `src/engine/pin_session.c` `pin_session_get_status` (~3187 onward; the `hint_path` block
near the end calls `fat32_and_free(dir, ...)` at ~3321) and `fat32_and_free` (~3038). The GUI calls
status from both timers: 10 Hz (`tick`) + 30 Hz (`updateMeters`) = ~40 calls/s.

**Why**: Every call makes two filesystem syscalls on the output directory (APFS `statfs`/`statvfs`
can take tens to hundreds of microseconds, much more on an external, network or sleeping volume),
and takes the session mutex shared with the analog deliver thread and audio feed. On Windows the
equivalent is `GetDiskFreeSpaceExW` + `GetVolumeInformationW`, which is cheap and cached by the OS.

**Confidence**: Low as a main cause on an internal SSD; could be significant on external storage.

**Confirm**: Time Profiler, search for `statvfs` / `statfs` under `pin_get_status`; `sudo fs_usage -w
-f filesys MarvinCapture` shows the call rate.

**Fix**: cache the free-space result for ~1-2 s in the session (keyed by directory), recompute on
capture start/stop and on `pin_set_output_hint`. This is core code, so it helps every GUI and the CLI.
Also let `updateMeters` use a lighter call (a peak-only accessor) instead of a full status snapshot,
or read status once per 33 ms tick and let `tick()` reuse it.

### 8. Timers have zero tolerance and wake the process 40+ times a second even when nothing can change (low, easy)

**Where**: `WindowModel.makeTimer` (~563): `Timer(timeInterval:repeats:)` with default tolerance 0.

**Why**: Zero tolerance defeats timer coalescing, so each timer fires at an exact instant and wakes
the CPU separately. Windows' DispatcherQueueTimer behaves similarly, so this does not explain the
gap, but it adds to it and to energy use.

**Fix**: `t.tolerance = interval * 0.2` on both timers (more for the status timer).

### 9. Device watch / libusb hotplug thread and enumerate (low)

**Where**: `src/engine/pin_devices_wait.c` `watcher_main` POSIX branch (~236-250):
`libusb_handle_events_timeout(NULL, 500 ms)` every iteration, then `device_fingerprint()`
(`pinnacle_lock_query` = open/`flock`/read/close per known device) every iteration, i.e. 2 wakeups/s;
a re-enumerate (`libusb_get_device_list`, expensive on darwin) only on hotplug. The Swift side
(`WindowModel.startDeviceWatch` ~699) blocks in `pin_devices_wait(60 s)`. Re-enumeration from Swift
(`refreshDevices`, ~586) happens only on state changes and hotplug.

**Why / confidence**: Not expected to be significant. One macOS-specific risk worth ruling out:
if `libusb_handle_events_timeout(NULL, ...)` on the default context returned immediately (for
example because of a leftover pending event on the default context) the loop would spin. That would
show as a thread pegged at 100 % with no device, not 20 %.

**Confirm**: `sample` / Time Profiler thread list at idle: any thread other than main with
noticeable samples? `top -pid <pid> -H` equivalent: Activity Monitor > Inspect > Threads, or
`ps -M <pid>`.

**Fix**: only if it shows up: block on a condition variable and a hotplug-driven wake instead of the
500 ms poll when libusb hotplug is available.

---

## Checked and ruled out (do not spend time here)

- Audio monitor: `CoreAudioMonitor` creates the `AVAudioEngine` only between `start()` and `stop()`;
  muted is the default, so at idle no engine and no render thread exist
  (`Services/AudioMonitor.swift` 170-233). If unmuted, the render block is real-time safe and the
  stats timer only runs with `--debug`.
- Dock tile: `DockTile.update` (Services/DockTile.swift:63) returns before `display()` unless the
  mode, badge or fraction (>= 1 %) changed; the Dock tile has no animation.
- Menu bar: `validateMenuItem` is only called by AppKit when menus open or key equivalents are
  resolved; no per-tick menu updates. The capture menu is rebuilt in `menuNeedsUpdate` only.
- No `TimelineView`, `withAnimation`, `.animation`, `repeatForever`, `symbolEffect`, `.onReceive`,
  `CVDisplayLink` or other per-frame SwiftUI drivers anywhere in gui/macos.
  The only indeterminate `ProgressView`s are shown while PREPARING and the "Finalizing" overlay.
- Metal path is GPU: planar Y/U/V `r8Unorm` textures, YUV to RGB in the fragment shader
  (`Preview/PreviewShaders.swift`), no CGImage/CIImage, no CPU colour conversion, the ring of three
  texture sets avoids per-frame texture allocation. The only per-frame CPU work is the three
  `replace(region:)` copies and a 12-float matrix array. The CPU YUV conversion in
  `dumpFrame` runs once and only with `MARVIN_PREVIEW_DUMP`.
- The render thread parks (`state.wait()`) when there is no session or the window is hidden and
  blocks in `pin_preview_wait(100 ms)` when no frame arrives, so an idle preview costs ~10
  wakeups/s; the core's `pv_thread` also waits 100 ms slices.
- Decoding for the analog preview in the core (`decode_analog`, `src/engine/pin_preview.c`) is a
  plain YUYV de-interleave, identical on Windows.
- Build configuration: `scripts/build.sh` defaults to Release for both CMake and `swift build -c
  release`. Double-check the app being measured is not a `--config Debug` build (a debug Swift build
  is many times slower in exactly these view paths, and `cString<T>` over big tuples is notably slow
  unoptimised).

---

## How to measure

Baseline numbers from the report: idle ~20 %, SD analog preview ~40 % (one core = 100 %); Windows
~0.5 % / ~3 %. Record the same three scenarios on the Mac, each for 30 s after the app has settled:

1. **No device attached**, window visible and in front.
2. **Device attached, READY, no signal** (preview black, "No video signal").
3. **Device attached, SD analog source playing**, preview visible, muted.
Optionally 4: same as 3 with the window minimised (the renderer pauses; if CPU stays high the
timers/stream are to blame).

Tools, in this order:

```sh
# Release build, run from a shell so --debug output is visible
bash scripts/build.sh --skip-tests
build/macos-arm64/dist/MarvinCapture.app/Contents/MacOS/MarvinCapture --debug

PID=$(pgrep -x MarvinCapture)
top -pid $PID -stats pid,cpu,threads,power -l 10          # steady CPU%
ps -M $PID                                                # per-thread CPU (which thread is busy)
sample $PID 5 -file /tmp/marvin-idle.txt                  # call tree per thread, 5 s
sudo fs_usage -w -f filesys $PID | head -200              # statvfs/statfs call rate (cause 7)
sudo fs_usage -w -f network,filesys $PID                  # optional
```

Instruments (Xcode > Open Developer Tool > Instruments, attach to the running process):

- **Time Profiler**, "All threads", Call Tree with "Invert Call Tree" and "Hide System Libraries"
  off first. Compare the main thread against "Preview render", the libusb darwin event thread, the
  analog read-loop thread (a thread in `pinnacle_analog_read_loop`) and "Device watch". This alone
  separates cause 1/2/4/5 (main thread, SwiftUI/CoreAnimation frames) from cause 3 (libusb/IOKit
  threads) and cause 6 ("Preview render", `PreviewLayerView.tick`).
- **SwiftUI** template (View Body / View Properties): bodies evaluated per second at idle.
- **Core Animation** / **System Trace**: commit rate and wakeups per second for causes 2 and 8.
- **Energy Log** / Activity Monitor "Wakeups" column: idle wakeups per second (target: well under
  50, the timers alone are ~40).

Quick A/B experiments (each one line, revert after):

| Experiment | Tells you |
|---|---|
| Comment out `meterTimer = makeTimer(...)` (WindowModel ~559) | how much the 30 Hz meter path costs, idle and preview |
| Make `applyStatus` and `updateMeters` assign only on change | cause 1 |
| Do not start the preview link (`link?.isPaused = true` always) | cost of the display link vs. decoding vs. USB |
| `export MARVIN_PREVIEW_IGNORE_OCCLUSION=1` off / minimise the window | whether the pauses work and what remains |
| Mute (default) vs unmute (`MARVIN_UNMUTE=1`) | audio monitor cost (expected small) |
| Temporarily raise `VIDEO_XFER` to 32 KiB on macOS | cause 3 (check frame integrity afterwards) |

Report back for each fix: before/after CPU% for scenarios 1-3, which thread dropped, and for any
core/USB change a multi-minute capture with the dropped/short-frame counters unchanged.

## Suggested order of work on the Mac

1. Measure scenarios 1-3 with Time Profiler + `ps -M` and write down the busiest threads.
2. Cause 1 (guard assignments, early-out meters when idle) plus cause 8 (timer tolerance): small
   diff, then re-measure scenario 1.
3. Causes 2 and 4 (cheaper meter drawing, title/tooltips) if the main thread is still the top.
4. Cause 3 and 6 if scenario 3 is still far above scenario 2: USB transfer size on macOS and the
   display-link rate cap.
5. Cause 7 (core status cache) as a cross-platform cleanup.
Keep the core-first rule from the gui-changes skill: anything done in the core (status caching,
USB transfer sizes, thread QoS) must keep the Windows build and `ctest` green.
