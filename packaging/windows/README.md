# Windows installer

NSIS (Modern UI 2) installer for 64-bit Windows, x64 or ARM64 (one setup per architecture,
`-Arch`), with an optional WinUSB driver install for all five supported devices (VID `2304`,
PID `0213`, `0223`, `0212`, `0224`, `0206`).

## Build

Prerequisites (on top of those in `docs/building.md`):

- **NSIS 3.x** (tested layout: 3.11 / 3.13). Either the official installer from
  <https://nsis.sourceforge.io/Download> (found at `C:\Program Files (x86)\NSIS`), or in MSYS2:
  `pacman -S mingw-w64-ucrt-x86_64-nsis`. Use `-Makensis <path>` for anything else.
- **Visual Studio 2022 or later, or the Build Tools for Visual Studio**, only for the first build
  of the driver tool, with the workload "Desktop development with C++" plus the
  **C++ ARM64 build tools** (components `Microsoft.VisualStudio.Component.VC.Tools.x86.x64` and
  `Microsoft.VisualStudio.Component.VC.Tools.ARM64`). Found with `vswhere`. Only libwdi's
  Visual Studio solution builds its ARM64 helper; its MinGW/autotools build is x86/x64 only.
- Windows 10+ `tar.exe` and network access on the first run.

```powershell
.\scripts\package-windows.ps1              # builds the app (build.ps1), the driver tool, the installer
.\scripts\package-windows.ps1 -SkipBuild   # reuse build\windows-<arch>\dist
.\scripts\package-windows.ps1 -Arch arm64  # Windows on Arm (default: this machine's)
```

Output: `build\windows-<arch>\installer\MarvinCapture-<version>-windows-<arch>-setup.exe`
(`<arch>` = `x86_64` or `arm64`, `<version>` from the top-level `VERSION` file).

What the script downloads (once, SHA256-verified, cached under `build\windows-libwdi\work`):

- libwdi v1.5.1 source tarball (GitHub) - built with MSBuild (`libwdi.sln`, x64 Release) into one
  `wdi-simple.exe` that embeds libwdi's elevated helpers for x64 and ARM64 and starts the one for
  the machine; both installers use it (on Windows on Arm it runs through Windows 11's x64
  emulation, so the arm64 setup requires Windows 11). No 32-bit x86 support. The script edits libwdi's
  `msvc\config.h` to point at the WDK files and to leave out libusb-win32 and libusbK.
- The Microsoft WDK 8.0 redistributable `wdfcoinstaller.msi` - only unpacked (`msiexec /a`), not
  installed. libwdi needs its WinUSB co-installer DLLs to build the WinUSB INF template; they are
  embedded in `wdi-simple.exe`.

Delete `build\windows-libwdi` (or pass `-RebuildWdi`) to rebuild the tool.

## What is in here

| File | Purpose |
|---|---|
| `MarvinCapture.nsi` | the installer / uninstaller |
| `driver-install.ps1` | per device: runs `wdi-simple.exe`, then removes stale ("ghost") device entries; logs to `%TEMP%\MarvinCapture-driver-install.log` |
| `driver-uninstall.ps1` | finds libwdi-generated `oem*.inf` for the five IDs by parsing the INF files (locale independent), `pnputil /delete-driver /uninstall /force`, removes the libwdi certificates from Root and TrustedPublisher; logs to `%TEMP%\MarvinCapture-driver-uninstall.log` |
| `path-edit.ps1` | adds/removes the install folder in the system PATH (registry, `REG_EXPAND_SZ`, no NSIS string limit, broadcasts `WM_SETTINGCHANGE`) |

Installer command line: `/S` silent, `/D=C:\dir` install dir (must be last). Uninstaller:
`/S`, `/KEEPDRIVER` (do not remove the driver), `/UPGRADE` (used internally when a newer
installer replaces the old version; keeps driver and PATH entry).

The installer is **not code-signed**: Windows shows SmartScreen / "Unknown publisher" warnings
(see the Windows section of the main README).
