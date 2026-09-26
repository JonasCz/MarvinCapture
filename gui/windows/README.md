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
| `PinnacleCoreDir` | `..\..\..\tests\stub\build-stub` | Folder with `pinnacle-oss-core.dll` (and `libusb-1.0.dll` if present) |
| `PinnacleRuntimeDllDir` | `C:\msys64\ucrt64\bin` | Where `libwinpthread-1.dll` is taken from. MinGW/UCRT64 builds of the core need it. |

To build against the real core instead of the stub:

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
accepts the same options. Presets are applied over the saved settings. Actions
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

## Testing without hardware (stub DLL)

`tests/stub` builds a fake `pinnacle-oss-core.dll` that implements the whole
`pin_api.h`. It provides two fake devices, colour-bar preview frames, deck
transport, captures that write small files, and command-line parsing. Build it
from an MSYS2 UCRT64 shell (or see `tests/stub/build.sh`):

```bash
tests/stub/build.sh          # -> tests/stub/build-stub/pinnacle-oss-core.dll
```

Then build the app as above. The default `PinnacleCoreDir` already points at
`tests/stub/build-stub`, so the stub is copied next to the exe automatically.

## Transitional ABI fields

The engine is appending these to `pin_api.h`. The app already uses them, and
the stub accepts both the old and the new struct sizes (`tests/stub/pin_stub_abi2.h`):

- `pin_capture_opts_t.rewind_first`
- `pin_status_snapshot_t.disk_free_bytes`, `est_seconds_left` and `disk_low`
- `pin_set_output_hint()`

`pin_launch_t` grows too, because it embeds the capture options. If a core
lacks `pin_set_output_hint`, the app just doesn't show the disk estimate
before a capture starts.

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
- During a capture, every deck control except Stop is disabled. Stop ends the
  capture first, then stops the deck. Outside a capture, the deck buttons are
  enabled under the same rule as the capture buttons (idle and READY).
- "Play and capture" sets `rewind_first = 1` and `start_deck = 1`, so the core
  rewinds to the start of the tape, sends PLAY, then starts the writer. While
  recording, the same button becomes "Stop capture".
- Aspect is set per kind (analog, DV, HDV) next to each format. The one for
  what is currently arriving also drives the preview (`pin_set_aspect`). The
  preview panel itself is sized to `pin_fit_rect`, so the window background
  forms the letterbox instead of drawn black bars.
- Whenever the output path or format changes, the app calls
  `pin_set_output_hint`, so the status bar can show free space and capture
  time left before a capture starts. Both turn red when the core reports
  `disk_low`.
- Closing the window during a capture asks first. Confirming stops the capture,
  shows "Finalizing files…" and exits once the core reports READY.
- There are no keyboard shortcuts, on purpose. Every control is tabbable and
  has an automation name.
