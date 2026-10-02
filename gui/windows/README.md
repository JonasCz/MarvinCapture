# Pinnacle Capture (Windows)

WinUI 3 front-end for the Pinnacle Studio 500-USB open driver. It is a thin
GUI over the `pinnacle-oss-core` C library: device list, option rules, file
naming, status text and settings all come from the core through
`src/api/pin_api.h`. The app holds widgets, the Direct3D 11 preview, the file
dialogs and the window lifecycle.

- .NET 10, Windows App SDK (unpackaged, self-contained), x64 only
- CommunityToolkit.Mvvm for view models
- Vortice.Direct3D11 / DXGI / D3DCompiler for the preview renderer
- No Visual Studio needed: everything builds with `dotnet build`

## Build

Requires the .NET 10 SDK. NuGet packages come from nuget.org through
`gui/windows/nuget.config`.

```powershell
cd gui\windows\PinnacleCapture
dotnet build -c Debug -p:Platform=x64
```

The output lands in `bin\x64\Debug\net10.0-windows10.0.19041.0\win-x64\`.
For a release build, use `-c Release`.

### Where the core DLL comes from (`PinnacleCoreDir`)

After every build, `pinnacle-oss-core.dll` is copied next to
`PinnacleCapture.exe` from the directory in the `PinnacleCoreDir` MSBuild
property. If the DLL isn't there, the build still succeeds and simply skips the
copy.

| Property | Default | Purpose |
|---|---|---|
| `PinnacleCoreDir` | `..\..\..\build\core` | Folder with `pinnacle-oss-core.dll` (and `libusb-1.0.dll` if present) |
| `PinnacleRuntimeDllDir` | `C:\msys64\ucrt64\bin` | Where `libwinpthread-1.dll` is taken from. MinGW/UCRT64 builds of the core need it. |

To build against a core built elsewhere:

```powershell
dotnet build -c Debug -p:Platform=x64 -p:PinnacleCoreDir=C:\path\to\core\build
```

## Run

```powershell
.\bin\x64\Debug\net10.0-windows10.0.19041.0\win-x64\PinnacleCapture.exe
```

If `pinnacle-oss-core.dll` can't be loaded, or it reports an API version other
than 1, the app shows an error window instead of the UI and logs the details.

Crashes and unhandled exceptions go to `%LOCALAPPDATA%\PinnacleOSS\crash.log`.
If the app runs cleanly, that file doesn't exist.

## Command line

The core parses the command line (`pin_launch_parse`), so every front-end
accepts the same options. Presets are applied over the selected device's saved settings. Actions
run once the device is ready.

```powershell
# open a specific device on the composite input, PAL, 16:9 anamorphic
PinnacleCapture.exe --device usb:1-4 --input composite --std PAL --aspect 16:9

# DV tape: split scenes, capture twice, stop after 3 min without data
PinnacleCapture.exe --input dv --output D:\tapes\holiday --format dv-avi --split --passes 2 --idle-min 3

# unattended: rewind, capture the whole tape, close the window when finished
PinnacleCapture.exe --device first --input dv --output D:\tapes\tape07 --actions rewind,capture --exit-when-done

# show the option list (also under "..." > "Command-line help")
PinnacleCapture.exe --help
```

Actions are `rewind`, `play`, `stop`, `capture` and `wait-eot`. The `--help`
text comes from `pin_launch_help()`, so it always matches the core in use.

Each window runs as its own process. "..." > "New window" starts a second one,
for example to use a second device.

## Taskbar progress

The taskbar button shows what the app is doing (mode and value come from the
core, `pin_status_progress`; `Controls/TaskbarProgress` only talks to the shell):

| State | Taskbar |
|---|---|
| Device initialisation | green bar, same value as the in-window progress bar (indeterminate if unmeasurable) |
| Capturing, no signal, "no signal" timeout set | green bar counting down from full to empty |
| Capturing with "Stop after (min)" | green bar, elapsed / limit of the current pass |
| Capturing, no limit | indeterminate |
| Capturing, waiting for signal (no timeout), or rewinding between passes | yellow (paused), full |
| Capturing and the core reports low disk space (under 1 hour or 50 GB left) | yellow (paused); keeps the value of a countdown / time limit bar, else full. Beaten only by an error |
| Stopping / finalising | indeterminate |
| Device error, or an error InfoBar open | red; clears when the InfoBar is dismissed, or when the next capture starts |
| Idle / ready | none |

While a capture runs (including stopping and rewinding between passes) the button also carries a small red
dot at its lower right corner (`ITaskbarList3::SetOverlayIcon`, accessible name "Capturing"). The icon is
drawn at run time at the small-icon size of the window's DPI (`Controls/TaskbarIcons`), so there are no
asset files. It is cleared when the capture ends.

When a capture finishes or fails while the window is not in the foreground, the taskbar button flashes
(`FlashWindowEx`, `FLASHW_TRAY | FLASHW_TIMERNOFG`) until the window is brought to the front.

### Thumbnail toolbar and thumbnail clip

Hovering the taskbar button shows two buttons under the thumbnail (`ThumbBarAddButtons`; Segoe Fluent /
MDL2 glyphs E896 and E71A drawn at run time):

| Button | Does | Enabled |
|---|---|---|
| Start capture | the main start action of the current mode, through the same code as the in-window button (output checks, free-space / overwrite dialogs; the window is brought to the front if a dialog is needed): analog = Capture, DV/HDV = Manual capture (it does not move the tape; "Automatic rewind & capture" stays in the window) | idle and the in-window button is enabled |
| Stop capture | `pin_capture_stop_ex(PIN_STOP_DECK_AS_STARTED)`: a manual capture leaves the tape running, an automatic rewind & capture stops it | while a capture can be stopped (not while finalising) |

The buttons can only be added after the shell has created the taskbar button, so the window listens for
the registered `TaskbarButtonCreated` message and re-adds everything when it arrives again (Explorer
restart). The glyphs are redrawn when the taskbar theme or the DPI changes.

The thumbnail shows only the video picture instead of the whole window: `ITaskbarList3::SetThumbnailClip`
with the preview frame's rectangle in client pixels (XAML position x rasterization scale). It is updated on
layout changes (checked on every status tick, the shell is only called when the rectangle changes) and
cleared (whole window) when there is no preview area.

## Theme testing

The app follows the Windows light/dark setting. To check the other theme
without changing Windows, start it with `PIN_THEME=dark` (or `light`):

```powershell
$env:PIN_THEME = "dark"; .ind\Debug
et10.0-windows10.0.19041.0\win-x64\PinnacleCapture.exe
```

## Layout

```
PinnacleCapture/
  Interop/      Native.cs ([LibraryImport] bindings for all of pin_api.h),
                NativeStructs.cs / NativeEnums.cs (blittable mirrors),
                PinSessionHandle.cs (SafeHandle -> pin_close), Win32.cs
  ViewModels/   MainViewModel (all state + the 100 ms status/event tick),
                KindSettingsViewModel (DV / HDV tab), ControlSliderViewModel,
                DeviceItemViewModel
  Views/        KindSettingsView (format, title, split, idle, passes)
  Controls/     SameHeightSwitchPanel, DeviceComboBox, TaskbarButton + TaskbarIcons (ITaskbarList3), DbThumbConverter
  Preview/      D3DPreview (swap chain + render thread), Shaders (HLSL)
  Services/     IAudioMonitorService (stub; WASAPI playback is a later phase)
  MainWindow.xaml(.cs), App.xaml(.cs)
```

## Notes

- The preview has a dedicated render thread that blocks in
  `pin_preview_wait()` and presents only when a new frame arrives. Without a
  session, or while the window is minimised, the thread sleeps on an event and
  the core stops decoding (`pin_preview_enable(0)`). Nothing is polled or
  redrawn on a timer.
- Status and events use one 100 ms `DispatcherQueueTimer`
  (`pin_get_status` plus draining `pin_poll_event`). The device list is
  re-enumerated every 2 s only while its dropdown is open or no device is open.
- During a capture, all deck controls (including Stop) are disabled; only the
  capture buttons stay active. Outside a capture the deck buttons are enabled
  when idle and READY, and each one is disabled when the deck already reports
  that state (Stop when stopped, Play when playing, ...; none with no tape).
- "Automatic rewind & capture" sets `rewind_first = 1` and `start_deck = 1`, so
  the core rewinds to the start of the tape, sends PLAY, then starts the
  writer. While recording it becomes "Stop capture & stop tape" (the core stops
  the deck when the capture ends). "Manual capture" records without touching
  the deck and becomes "Stop capture & continue tape" while recording. Both stop
  buttons are active during any DV/HDV capture: the GUI calls
  `pin_capture_stop_ex` with `PIN_STOP_DECK_NO` ("continue tape") or
  `PIN_STOP_DECK_YES` ("stop tape"), so the choice does not depend on how the
  capture was started.
- "Capture passes" is disabled and shown as 1 (tooltip: multi-pass needs a way
  to detect the end of a pass) while both "Stop no signal" and "Stop after" are
  0. The rule is the core's (`pin_capture_passes_allowed`, called through
  `Native.PassesAllowed`); `KindSettingsViewModel` keeps the user's pass count
  and restores it when a limit is set again, and `EffectivePasses` is what goes
  into the capture options (the core also enforces it at capture start).
- A capture that ends abnormally (`Native.StopReasonAbnormal`: device or
  camera disconnected, disk almost full, write error) shows a "Capture stopped"
  dialog with the core's sentence ("Capture stopped after capturing 12m30s,
  because ...") and an OK button (`PIN_EVT_CAPTURE_ENDED`, handled in
  `MainWindow.VM_EngineEvent`); not while closing or when running command-line
  actions with exit-when-done. A normal end by a limit (no signal, time limit,
  end of tape) only replaces "Ready" in the status bar with that sentence
  until the next capture (`MainViewModel.ApplyStatus`, `stop_text`).
- Before a capture starts, `pin_check_output` reports `low_space` (free space
  known and under 25 GiB, `PIN_LOW_SPACE_BYTES`); the window then asks "Only X
  free on D:\. Continue?" with OK / Cancel, for analog, DV and HDV alike.
- Settings are per device. Everything except the items below is stored under
  `dev_<GUID>.gui.*` in the settings file, where the prefix comes from the core
  (`pin_device_settings_key`: the unit's GUID, or the sanitised USB port id
  while no GUID is known). Selecting another device in the list loads that
  device's settings (defaults if it has none); with no device selected nothing
  is saved. Global: window geometry (`gui.window*`), the last used device
  (`gui.last_device`) and mute (`gui.muted`). Older flat `gui.*` options are not
  migrated and are ignored.
- Window position, size and maximised state are saved on close
  (`gui.window`, `gui.window_maximized`) and restored at start. A saved
  rectangle that no longer touches any monitor falls back to the centred
  default.
- Aspect is set per kind (analog, DV, HDV) next to each format. The one for
  what is currently arriving also drives the preview (`pin_set_aspect`). The
  preview panel itself is sized to `pin_fit_rect`, so the window background
  forms the letterbox instead of drawn black bars.
- Whenever the output path or format changes, the app calls
  `pin_set_output_hint`, so the status bar can show free space and capture
  time left before a capture starts. Both turn red when the core reports
  `disk_low`.
- The status bar is one line with one font (no bold, no monospace), items
  separated by uniform spacing. Left to right: the file being written (or
  "Ready" / the error; the capture state is in its tooltip), deck status
  (DV/HDV),
  time (tape timecode; for analog the time since capture start), signal
  (check mark Locked / No signal, source type HDV / DV / S-Video / Composite and
  the format label from the core's `video_label`: PAL, NTSC, 1080i25, 720p59.94),
  frames ("Frames 1,234 · 2 err · 0 drop", totals; the current clip's numbers are
  in the tooltip), storage (one icon: bytes total / current file, free space,
  time left), then the small audio meters and mute. When the window is too narrow
  the bar drops items in this order: free space / time left, frames, storage,
  time, deck (the file text just trims); minimum window width is 1000 DIP. All
  numbers come from the core status: totals count since capture start (since
  app start while idle), the clip ones restart with every file. Tooltips and
  automation names spell the items out, one fact per line (frames: total and
  current clip; storage: written, free, time left).
- Closing the window during a capture asks first. Confirming stops the capture,
  shows "Finalizing files…" and exits once the core reports READY.
- There are no keyboard shortcuts, on purpose. Every control is tabbable and
  has an automation name.
