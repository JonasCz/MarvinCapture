# Windows installer size

The v1.0 Windows installer (`MarvinCapture-1.0-windows-x86_64-setup.exe`) is 65 MB;
unpacked it is 239 MB in 549 files. The macOS and Linux packages are a few MB
because the UI framework and the language runtime are part of the OS there. The
Windows GUI is a self-contained WinUI 3 / .NET 10 app: it ships the .NET runtime
and the Windows App SDK (WinUI) with it, because Windows has neither built in.

## What v1.0 contains (uncompressed)

| Part | Size | |
|---|---|---|
| Windows App SDK AI / ML / Search / Widgets | ~58 MB | `onnxruntime.dll` 22 MB, `DirectML.dll` 19 MB, `Microsoft.Windows.AI.*`, `Microsoft.Windows.Search.dll`, `Microsoft.Windows.Widgets.dll`, `Microsoft.Asg.SemanticIndex...`, `PerceptiveStreaming.dll`. Not used by the app; pulled in by the `Microsoft.WindowsAppSDK` metapackage |
| .NET 10 runtime | ~64 MB | 166 `System.*` assemblies, CoreLib, coreclr, clrjit; untrimmed |
| WinUI 3 + InteractiveExperiences | ~40 MB | `Microsoft.ui.xaml.dll`, controls, composition |
| `Microsoft.Windows.SDK.NET.dll` | 25 MB | C#/WinRT projection of all Windows APIs; untrimmed |
| .NET debugger support | ~4 MB | `mscordaccore*`, `mscordbi`, DiaSymReader, `createdump.exe` |
| WinUI `.mui` resources | ~4 MB | 86 language folders; the app is English only |
| The app itself | ~8 MB | `MarvinCaptureGUI.dll` 0.5 MB, `marvin-core.dll` 3.5 MB, firmware, `wdi-simple.exe` 3 MB |

## What changed

In `gui/windows/MarvinCaptureGUI/MarvinCaptureGUI.csproj` and `scripts/build.ps1`:

1. **Windows App SDK components instead of the metapackage.** Since 1.8 the
   `Microsoft.WindowsAppSDK` package is a metapackage over components. The app now
   references only `Foundation`, `InteractiveExperiences`, `WinUI` and `DWrite`
   (at the versions the 2.5.1 metapackage pins), not `AI`, `ML`, `Search`,
   `Widgets` or `Runtime` (`Runtime` is only for framework-dependent apps).
   When updating the Windows App SDK, take the component versions from the new
   metapackage's `.nuspec` (nuget.org, "Dependencies").
2. **`dotnet publish` for Release** (`scripts\build.ps1`; Debug stays `dotnet build`),
   with:
   - `PublishTrimmed` + `TrimMode=partial`: trims only assemblies marked trimmable
     (the framework, the Windows SDK projection), not the app, WinUI or the toolkit.
   - `BuiltInComInteropSupport=true`: trimming turns it off by default, but the app
     uses classic `[ComImport]` COM (WASAPI audio monitor, `ITaskbarList3`,
     `ISwapChainPanelNative` for the D3D preview).
   - `PublishReadyToRun`: precompiled code for faster startup. ILLink writes IL-only
     assemblies, so without it the trimmed framework would be JIT-compiled on every
     start (slower than v1.0, whose framework was precompiled).
   - `SatelliteResourceLanguages=en`.
3. **Pruning after publish** (target `PinnaclePrunePublish`, off with
   `-p:PinnaclePrunePublish=false`): the .NET debugger DLLs and `createdump.exe`, and
   all WinUI `.mui` language folders except `en-us`.
4. `build.ps1` empties `dist\` before assembling it, so no files from an older build
   end up in the installer.
5. The csproj copies `marvin-core.dll`, libusb, libwinpthread and `firmware\` into the
   publish folder too (before, only into `OutDir`).

The changes were made on Linux and not built: only `dotnet restore` was checked (it
resolves; the AI/ML/Search/Widgets packages are gone from the graph). Everything
below has to be done on Windows.

## Next steps: build and test on Windows

### 1. Build

```powershell
scripts\build.ps1 -Clean
```

- Look at the publish output for trim warnings (`IL2xxx`, `IL3xxx`). For the full
  list instead of one per assembly:
  ```powershell
  dotnet publish gui\windows\MarvinCaptureGUI -c Release -p:Platform=x64 -o $env:TEMP\mc-pub -p:TrimmerSingleWarn=false
  ```
  Warnings from WinRT/CsWinRT/WinUI assemblies are expected with `partial` and are
  usually harmless; warnings in `MarvinCaptureGUI` itself need a look.
- If the build fails on the package change (missing types, XAML compiler errors),
  first try adding back only the metapackage (`Microsoft.WindowsAppSDK` 2.5.1) to
  confirm that's the cause, then see which component is missing.

### 2. Check the size

```powershell
"{0:N1} MB, {1} files" -f ((gci build\windows-x86_64\dist -Recurse -File | measure Length -Sum).Sum / 1MB), (gci build\windows-x86_64\dist -Recurse -File).Count
gci build\windows-x86_64\dist -File | sort Length -Desc | select -First 25 Name, @{n='MB';e={[math]::Round($_.Length/1MB,1)}}
```

- None of `onnxruntime.dll`, `DirectML.dll`, `Microsoft.Windows.AI.*`,
  `Microsoft.Windows.Search.dll`, `Microsoft.Windows.Widgets.dll`, `mscordaccore*`,
  `mscordbi.dll`, `createdump.exe` should be there; the only language folder is `en-us`.
- `Microsoft.Windows.SDK.NET.dll` should be much smaller than 25 MB.
- To see what ReadyToRun costs, publish once with `-p:PublishReadyToRun=false` and
  compare the size and the startup time (below). R2R makes the managed DLLs roughly
  2-3x larger than IL; keep it if startup is clearly faster.

### 3. Test the app (from `dist\`, and later from the installed copy)

Trimming problems show up at run time, not at build time, so go through the
features that use reflection-like or COM paths:

- [ ] Starts, main window with Mica backdrop, title-bar logo (SVG), window position
      restored from the last run.
- [ ] Device list (with and without a device plugged in), format / standard
      combo boxes, the per-kind settings view, slider controls (picture adjustments).
- [ ] **Preview** (D3D11 swap chain through `ISwapChainPanelNative`): live picture
      and the idle Marvin animation without a signal.
- [ ] **Audio monitor** (WASAPI `[ComImport]`): unmute, hear audio; unplug/switch the
      default output device while it plays.
- [ ] **Taskbar** (`ITaskbarList3`, window subclass callback): taskbar progress /
      thumbnail buttons / flashing during and after a capture.
- [ ] Folder picker (Browse for the output folder), opening the folder / links.
- [ ] A short DV capture and, if possible, analog and HDV; level meter moves.
- [ ] Settings survive a restart.
- [ ] No `%LOCALAPPDATA%\PinnacleOSS\crash.log` afterwards.
- [ ] Startup time vs v1.0 (cold start after a reboot and warm start), e.g. with a
      stopwatch from double-click to the window, or
      `Measure-Command { Start-Process dist\MarvinCaptureGUI.exe; ... }`.
- [ ] Windows set to another display language (e.g. German): WinUI's own strings
      (text box context menu, etc.) should fall back to English and nothing should
      be blank. If they are blank, keep more languages (drop the `.mui` part of
      `PinnaclePrunePublish`, about 4 MB uncompressed).

If something fails only in the trimmed build, check it with
`-p:PublishTrimmed=false` to confirm trimming is the cause, then fix the code
(preferred) or root the assembly/type with
`<TrimmerRootAssembly Include="..." />` / `[DynamicDependency]`.

### 4. Installer

```powershell
scripts\package-windows.ps1 -SkipBuild
```

- Compare the size with v1.0's 65 MB (rough expectation: about 25-35 MB).
- Install over v1.0 (upgrade path), run the checklist above briefly on the
  installed copy, uninstall.

## Further options (not done)

- **`TrimMode=full`**: also trims the app, WinUI and CsWinRT projections; more
  savings, more risk. Needs the trim warnings cleaned up first.
- **NativeAOT** (`PublishAot`): would need `[GeneratedComInterface]` instead of
  `[ComImport]` (WASAPI, taskbar, swap-chain panel), `LibraryImport` for the two
  `DllImport`s in `WasapiAudioMonitorService.cs`, `[UnmanagedCallersOnly]` for the
  window subclass callback, partial-property `[ObservableProperty]` (MVVMTK0045) and
  `[GeneratedBindableCustomProperty]` on the bound view models. Smaller and fastest
  startup, but a real porting job; the WinUI native DLLs (~40 MB) stay either way.
- **Framework-dependent** (`WindowsAppSDKSelfContained=false` / `SelfContained=false`):
  smallest download, but users then need the Windows App SDK runtime and the
  .NET 10 Desktop Runtime installed.
