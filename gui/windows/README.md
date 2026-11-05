# MarvinCapture (Windows)

WinUI 3 front-end for the Pinnacle Studio 500-USB open driver. It is a thin
GUI over the `marvin-core` C library: device list, option rules, file
naming, status text and settings all come from the core through
`src/api/pin_api.h`. The app holds widgets, the Direct3D 11 preview, the file
dialogs and the window lifecycle.

- .NET 10, Windows App SDK (unpackaged, self-contained; WinUI components only), x64 or ARM64
  (`-p:Platform=ARM64`, output under `build\windows-arm64\`; built by CI, not tested on hardware)
- CommunityToolkit.Mvvm for view models
- Vortice.Direct3D11 / DXGI / D3DCompiler for the preview renderer
- No Visual Studio needed: everything builds with `dotnet build`

## Build

Requires the .NET 10 SDK. NuGet packages come from nuget.org through
`gui/windows/nuget.config`.

```powershell
cd gui\windows\MarvinCaptureGUI
dotnet build -c Debug -p:Platform=x64
```

`gui/windows/Directory.Build.props` moves `obj\` and `bin\` out of the project
into the repo's `build\windows-x86_64\gui\`, so the output lands in
`build\windows-x86_64\gui\bin\x64\Debug\net10.0-windows10.0.19041.0\win-x64\`.
(`scripts\build.ps1 -Config Debug` instead sets `OutDir` to `build\windows-x86_64\dist\`.)

The release (`scripts\build.ps1`, Release) is a `dotnet publish` into `dist\`, which
trims the framework (`PublishTrimmed`, `TrimMode=partial`), precompiles it
(`PublishReadyToRun`) and drops the .NET debugger DLLs and WinUI's non-English `.mui`
resources. By hand:

```powershell
dotnet publish -c Release -p:Platform=x64 -o C:\path\to\out
```

Plain `dotnet build` (and the IDE) is not trimmed. See
[../../docs/windows-size.md](../../docs/windows-size.md).

### Where the core DLL comes from (`PinnacleCoreDir`)

After every build, `marvin-core.dll` is copied next to
`MarvinCaptureGUI.exe` from the directory in the `PinnacleCoreDir` MSBuild
property. If the DLL isn't there, the build still succeeds and simply skips the
copy.

| Property | Default | Purpose |
|---|---|---|
| `PinnacleCoreDir` | `..\..\..\build\windows-x86_64\core` | Folder with `marvin-core.dll` (and `libusb-1.0.dll` if present) |
| `PinnacleRuntimeDllDir` | `C:\msys64\ucrt64\bin` | Where `libwinpthread-1.dll` is taken from. MinGW/UCRT64 builds of the core need it. |

To build against a core built elsewhere:

```powershell
dotnet build -c Debug -p:Platform=x64 -p:PinnacleCoreDir=C:\path\to\core\build
```

## Run

```powershell
..\..\..\build\windows-x86_64\gui\bin\x64\Debug\net10.0-windows10.0.19041.0\win-x64\MarvinCaptureGUI.exe
```

If `marvin-core.dll` can't be loaded, or it reports an API version other
than 1, the app shows an error window instead of the UI and logs the details.

Crashes and unhandled exceptions go to `%LOCALAPPDATA%\PinnacleOSS\crash.log`.
If the app runs cleanly, that file doesn't exist.

### Connection via USB hub

"(⚠ connected via USB hub)" under a device in the device list, and "Ready
(connection via USB hub detected, see readme)" in the status bar, mean the
device is plugged in through a USB hub and shares the hub's bandwidth with the
other devices on it, which can drop frames. Plug it directly into a USB port of
the computer (not a hub, front-panel hub, dock or monitor). Check where it is
connected with [USBTreeView](https://www.uwe-sieber.de/usbtreeview_e.html)
(Uwe Sieber). Some computers route their own ports through a built-in hub;
then the warning can be ignored. Hover over the entry for the same advice.
(The core reports it: `pin_device_info_t.hub_depth > 0`, text from
`pin_usb_hub_hint()`; see also the [main README](../../README.md#connection-via-usb-hub).)

## Format settings

The gear button right of each format drop-down (analog, and DV / HDV in the
per-kind options) opens a Flyout with the selected format's settings: for the
H.264 / HEVC formats the interlaced-video choice (deinterlace to 50 / 60 fps, or
keep the fields, whose help shows as a warning) and the quality slider (0 best
... 51 smallest, like x264's CRF, with the data rate it comes to under it), for
FFV1 the compression level (Fast, Smaller, Smallest, all lossless). Everything
in it comes from the core: which parts apply (`pin_format_options()`, one
control per `has_*` flag), labels, range, defaults and help texts, and the data
rate line (`pin_format_quality_hint()`, for the signal's 50 / 60 Hz). The gear
is disabled for a format without settings (raw DV, AVI, HDV, ...). A format
with a warning (`pin_format_warning()`: H.264 / HEVC are lossy) shows it in a
warning InfoBar under the drop-down while it is selected.

The choices are stored per device like the other options, under the Android
app's keys: `gui.deinterlace_analog`, `gui.deinterlace_dv`, `gui.quality_analog`,
`gui.quality_dv` (DV and HDV share the DV ones), `gui.ffv1_compression`; missing
or -1 means the format's default. They go into `pin_capture_opts_t`'s
`deinterlace`, `video_quality` and `ffv1_compression` for the input being
captured, and are locked while capturing. Code: `FormatChoices` and
`FormatOptionsViewModel` (`ViewModels/FormatOptionsViewModel.cs`),
`Views/FormatOptionsView.xaml` (the Flyout content).

The H.264 / HEVC formats are listed only where their encoder opens on this
machine. `pin_encoders_probe()` finds that out once (it opens each encoder,
50-300 ms) on a worker thread started with the window
(`MainViewModel.ProbeEncoders`); the format lists stay empty until it returns
and are then filled (`RefreshFormats`), so the UI thread never waits for it. A
saved or command-line H.264 / HEVC choice is selected then; on a machine
without that encoder the drop-down shows the kind's default and the stored
choice is left alone.

## Command line

The core parses the command line (`pin_script_parse`), so every front-end
accepts the same language, documented in [docs/cli.md](../../docs/cli.md):
settings and actions processed left to right. The settings the window has
fields for (`--device`, `--input`, `--std`, `--format`, `--aspect`, `--split`,
`--deinterlace`, `--quality`, `--ffv1-compression`, the analog controls) are
applied over the selected device's saved settings (`--aspect`, `--deinterlace`
and `--quality` to the kinds the chosen input delivers);
`--title` and `--keep-raw` have no field here and only affect the script's own
captures. If there are actions (`--rew`, `--play`, `--capture PATH`, `--wait`,
...), they run (`pin_script_run`) once the device is ready, and the window shows
them like any other capture. The old `--preset`, `--actions`, `--output`,
`--passes` and `--idle-min` are gone. `--exit-when-done` closes the window when the
steps finish, with the script's exit code.

```powershell
# open a specific device on the composite input, PAL, 16:9 anamorphic
MarvinCaptureGUI.exe --device usb:1-4 --input composite --std pal --aspect 16:9

# DV tape: rewind, play, capture until the tape ends
MarvinCaptureGUI.exe --rew --wait --play --capture D:\tapes\tape07.dv --wait

# show the option list (also under "..." > "Command-line help")
MarvinCaptureGUI.exe --help
```

The help text comes from `pin_script_help()`, so it always matches the core in use.

`--debug` sets the core's log level to debug. The GUI prints the core's log lines
(AV/C traffic, bring-up steps) on the console it was started from, or into the
file when stdout is redirected (`MarvinCaptureGUI.exe --debug > log.txt`);
without a console it has no effect. This replaces the old `PINNACLE_LOG_LEVEL`
environment variable.

## Update check

Right after start the GUI asks the core (`pin_update_refresh`, 5 s timeout, on a
background thread) whether a newer release is known. The core downloads the version file at
most once a week (`update.last_check` in the settings; a failed download is retried at the next
start) and remembers the answer, so the notice comes back at every start until the app is
updated, without a request. If a newer one is known, a closable
informational banner shows `pin_format_update`'s text, the release notes (remembered
with the answer) and a
"Download" button that opens the release page in the default browser. Any
failure (offline, bad answer, no newer version) shows nothing. The banner has
its own InfoBar, so an error banner does not replace it. `MARVIN_UPDATE_URL`
overrides where the core fetches the release info from (a URL or a local JSON
file path; for testing:
`{"version":"9.9","release_notes":"...","download_url":"..."}`; to check again before the week is
up, delete the `[update]` section of settings.ini).

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

## Layout

```
MarvinCaptureGUI/
  Interop/      Native.cs ([LibraryImport] bindings for all of pin_api.h),
                NativeStructs.cs / NativeEnums.cs (blittable mirrors),
                PinSessionHandle.cs (SafeHandle -> pin_close), Win32.cs
  ViewModels/   MainViewModel (all state + the 100 ms status/event tick),
                KindSettingsViewModel (DV / HDV tab), FormatOptionsViewModel
                (+ FormatChoices: the format settings), ControlSliderViewModel,
                DeviceItemViewModel
  Views/        KindSettingsView (format, title, split, idle, passes),
                FormatOptionsView (the format gear's Flyout)
  Controls/     SameHeightSwitchPanel, DeviceComboBox, TaskbarButton + TaskbarIcons (ITaskbarList3), DbThumbConverter,
                MarvinIdle (the animated logo on the no-signal screen, XAML shapes)
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
  writer. While recording it becomes the red "Stop capture & stop tape" (the
  core stops the deck when the capture ends), enabled for any DV/HDV capture
  with a camera (a manual one too). "Manual capture" is enabled when READY with an incoming
  video signal (`pin_manual_capture_allowed`, tooltip "No incoming video signal"
  otherwise), records without touching the deck (never a deck command, also not
  at the no-signal timeout or time limit, and a single pass) and becomes
  "Stop capture" while recording, enabled for any DV/HDV capture
  (`pin_capture_stop_ex` with `PIN_STOP_DECK_NO`; the Auto stop button uses
  `PIN_STOP_DECK_YES`). While recording, the button the capture was started
  with shows a second line: "Stopping in 4m12s" (", no signal" while the
  no-signal timeout counts), for an automatic capture "Stopping capture and tape
  in ..." or "Next pass in ... (pass 1 of 2)"; the analog "Stop capture" shows
  "Stopping in ...". Idle, the analog button says "Capture S-Video input to file".
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
  `MainWindow.VM_EngineEvent`); not while closing or while a command-line script
  runs (its `PIN_EVT_DONE` reports the failure instead). A normal end by a limit (no signal, time limit,
  end of tape) only replaces "Ready" in the status bar with that sentence
  until the next capture (`MainViewModel.ApplyStatus`, `stop_text`).
- A capture that received no video at all (an empty tape; the core's
  `stop_no_video`, event `PIN_EVT_NO_VIDEO`) leaves no file. Unless it ended
  abnormally, `MainWindow.VM_EngineEvent` shows the warning bar "No video
  received" with the core's sentence ("No video received; nothing was captured to
  PATH.") and the status bar shows it instead of "Ready" (also after the user's
  own stop). A command-line script reports it through its exit code 5 (`Done`).
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
