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
  Controls/     SameHeightSwitchPanel, DeviceComboBox, TaskbarProgress (ITaskbarList3), DbThumbConverter
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
- The status bar has two rows and one font (no bold, no monospace). Left to
  right: deck status (DV/HDV), the file being written (or "Ready" / the error)
  with the capture state under it, time (tape timecode; for analog the time
  since capture start), signal (Locked / No signal over the source type HDV / DV /
  S-Video / Composite and PAL / NTSC), frames (total on row 1, current clip on row
  2: frames, frames with error, dropped), storage (bytes total / current clip over
  free space and hours left), then the audio meters and mute. All numbers come
  from the core status: totals count since capture start (since app start while
  idle), the clip ones restart with every file. Tooltips and automation names
  spell the items out.
- Closing the window during a capture asks first. Confirming stops the capture,
  shows "Finalizing files…" and exits once the core reports READY.
- There are no keyboard shortcuts, on purpose. Every control is tabbable and
  has an automation name.
