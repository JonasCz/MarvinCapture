# Windows installer

NSIS (Modern UI 2) installer, 64-bit only, with an optional WinUSB driver install for all
five supported devices (VID `2304`, PID `0213`, `0223`, `0212`, `0224`, `0206`).

## Build

Prerequisites (on top of those in `docs/building.md`):

- **NSIS 3.x** (tested layout: 3.11 / 3.13). Either the official installer from
  <https://nsis.sourceforge.io/Download> (found at `C:\Program Files (x86)\NSIS`), or in MSYS2:
  `pacman -S mingw-w64-ucrt-x86_64-nsis`. Use `-Makensis <path>` for anything else.
- **MSYS2 autotools**, only for the first build of the driver tool: `pacman -S --needed autotools`
  (the UCRT64 gcc from `docs/building.md` is the compiler).
- Windows 10+ `tar.exe` and network access on the first run.

```powershell
.\scripts\package-windows.ps1              # builds the app (build.ps1), the driver tool, the installer
.\scripts\package-windows.ps1 -SkipBuild   # reuse build\windows-x86_64\dist
```

Output: `build\windows-x86_64\installer\MarvinCapture-<version>-windows-x86_64-setup.exe`
(`<version>` from the top-level `VERSION` file).

What the script downloads (once, SHA256-verified, cached under `build\windows-x86_64\libwdi\work`,
or `C:\marvincapture-libwdi-work` if the repository path contains spaces):

- libwdi v1.5.1 source tarball (GitHub) - built into `wdi-simple.exe`, static, x64 only.
- The Microsoft WDK 8.0 redistributable `wdfcoinstaller.msi` - only unpacked (`msiexec /a`), not
  installed. libwdi needs its WinUSB co-installer DLLs to build the WinUSB INF template; they are
  embedded in `wdi-simple.exe`.

Delete `build\windows-x86_64\libwdi` (or pass `-RebuildWdi`) to rebuild the tool.

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
