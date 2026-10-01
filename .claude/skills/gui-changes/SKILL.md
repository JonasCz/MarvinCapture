---
name: gui-changes
description: Make changes to the Pinnacle Capture GUI (WinUI 3 / C#, gui/windows/PinnacleCapture): where things live, how settings and capture options flow, the core-first rule, build/lock gotchas. Use before editing GUI XAML, view models or the native interop.
---

# GUI changes

## Core-first rule

Each platform gets its own GUI codebase (today only `gui/windows/`, a C# WinUI 3
app; macOS/Linux GUIs will be separate code). So **put every piece of logic that
isn't UI in the core** (`src/engine`, `src/api`) and keep the per-platform code
to widgets, dialogs, bindings and P/Invoke. Before writing C# logic, ask
"would the macOS GUI need this too?". If yes, it goes in the core and the GUI
calls it through `pin_api.h`. Examples already done that way: output naming
and collision/free-space checks (`pin_check_output`), file name validation
(`pin_naming_validate`), settings, command-line parsing, scene splitting.

Core change checklist: function in `src/engine/*.c` + declaration in its
header, expose through `src/api/pin_api.[ch]` if the GUI needs it, add a test in
`tests/engine/` (registered in `src/engine/CMakeLists.txt`), and mirror any new
API/struct/enum in `Interop/Native*.cs`. Prefer reusing an existing call (e.g.
`pin_check_output` already returns a message the GUI shows) over a new export.

## Where things are (gui/windows/PinnacleCapture)

- `MainWindow.xaml` / `.xaml.cs`: the one window. Analog panel is `[0]`, DV/HDV
  panel is `[1]` inside `PanelSwitch`. Click handlers, dialogs, pickers.
  `StartCaptureAsync` is the capture entry: validates, calls `VM.CheckOutput`,
  shows dialogs, then `VM.StartCapture`.
- `ViewModels/MainViewModel.cs`: all state (CommunityToolkit.Mvvm
  `[ObservableProperty]`; `partial void OnXChanged` hooks). `BuildCaptureOpts`
  turns UI state into `PinCaptureOpts`; `LoadSettings` / `SaveSetting` persist
  (keys `gui.*`, stored via the core settings); `ApplyLaunch` applies command
  line presets.
- `ViewModels/KindSettingsViewModel.cs` + `Views/KindSettingsView.xaml`: the
  per-kind (DV, HDV) format / aspect / split / idle / passes options.
- `Interop/`: `Native.cs` (P/Invoke), `NativeStructs.cs`, `NativeEnums.cs`. Enum
  members are prefixed like `PinStatus.ErrArg` (not `Arg`).
- Output path = `OutputDir` + `Name` (analog: `AnalogOutputDir`/`AnalogName`,
  DV/HDV shared: `DvOutputDir`/`DvName`); `AnalogOutputPath` / `DvOutputPath` are
  computed. The name box is also the embedded title (no separate Title field).
  The directory button uses `FolderPicker`. Old single-path settings
  (`gui.output_*`) are migrated in `LoadOutput`.

## Gotchas

- `x:Bind` with `UpdateSourceTrigger=PropertyChanged` is needed for TextBoxes
  whose value is read at Start (otherwise the value commits on focus loss).
- Pickers need `WinRT.Interop.InitializeWithWindow.Initialize(picker, _hwnd)`.
  `FileSavePicker` creates an empty placeholder file; `FolderPicker` doesn't.
- Edits made with Python/sed: C source needs doubled backslashes; heredoc
  Python strings halve them. Check escapes (`"\\"`) in the result.
- Git reports CRLF to LF warnings on commit; harmless.

## Build / verify

Use `scripts\build.ps1` (see the build-and-test skill). It builds the core,
runs ctest, then the GUI. The XAML/C# compiler errors appear before the copy
step. If it fails with MSB3027 "file is locked by PinnacleCapture (pid)", the
app is running from `build\dist`: the code compiled fine, but ask the user to
close it (don't kill it) and re-run. Don't run `ctest` directly from
`build\core` (exit 0xc0000139 = DLL path not set); the script sets PATH.

Commit and push straight to main (single-developer repo).
