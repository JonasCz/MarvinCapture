# macOS GUI CPU usage: results

Measured on an M1 Pro, Release build, process CPU as % of one core. Windows GUI: ~0.5 %.

| scenario | before | after |
|---|---|---|
| composite preview | 37 | ~20 |
| S-Video, no signal | 26 | 5.3 |
| window minimised | 36 | 6.7 |

## What was fixed

- `@Observable` properties were written on every tick even when unchanged, so every
  observing view re-evaluated. Now written only on change (`WindowModel.applyStatus`).
- The 10 Hz status tick is a 5 Hz shared tick; the meters update separately at 30 Hz.
- The SwiftUI `Canvas` level meters are a CALayer-backed `NSView` (`LevelMeterView`)
  updated directly at 30 Hz, outside the SwiftUI graph.
- The window title is set only when it changes.
- Display link `preferredFrameRateRange` 24-60 Hz.
- Hidden or minimised window: meters skip, status runs at ~1 Hz.
- Free-space `statvfs` in `pin_session_get_status` is cached for 1 s (core, so it helps
  every GUI and the CLI).
- USB transfers on macOS: 96 x 64 KiB instead of 512 x 8 KiB. USB threads went from ~15 %
  to ~3 % of a core. See [analog.md](../analog.md#macos-and-windows-size-vs-cpu-and-losses).

## What remains (~20 % while previewing)

| part | % of a core |
|---|---|
| main thread | ~4.4 |
| libusb event thread | ~4 |
| Metal texture upload (AGX `replace(region:)`) | ~3 |
| read loop | ~2 |
| deliver and preview threads | ~1 each |

## Next levers

- Linear / buffer-backed Metal textures instead of `replace(region:)` copies.
- libusb's darwin backend calls `GetPipeProperties` on every submit (up to 1.0.30 and
  current master). A patched libusb would cut the event-thread cost.

## Ruled out

Audio monitor (no engine while muted), Dock tile, menu validation, per-frame SwiftUI
drivers, the Metal shader path (GPU, no CPU colour conversion), the device watch thread,
status-bar text measuring (negligible).

## Measuring again

```
bash scripts/build.sh --skip-tests
PID=$(pgrep -x MarvinCapture)
top -pid $PID -stats pid,cpu,threads,power -l 10
ps -M $PID
sample $PID 5 -file /tmp/marvin.txt
```

Scenarios: no device, device Ready with no signal, analog preview (muted), the same with
the window minimised. Check that it is a Release build; a Debug Swift build is many times
slower.
