<#
.SYNOPSIS
  Installs the MSYS2 packages scripts\build.ps1 and scripts\package-windows.ps1 need.

.DESCRIPTION
  Runs pacman -S --needed in an existing MSYS2 installation (https://www.msys2.org) for
  the toolchain of -Arch:

    x64     UCRT64 gcc, cmake, ninja, libusb, pkgconf, nasm
    arm64   CLANGARM64 clang, cmake, ninja, libusb, pkgconf (MSYS2 has no gcc there)

  plus make, diffutils (FFmpeg's build) and NSIS (the x86_64 package on both: NSIS makes
  x86 installers anyway), and with -Tests the ffmpeg command-line tool of that toolchain.
  Packages already installed are left alone.

  Not installed by this script, only checked (a warning when missing):
    * the .NET 10 SDK, for the GUI (winget install Microsoft.DotNet.SDK.10)
    * Visual Studio 2022+ or its Build Tools with the C++ x64/x86 and ARM64 build tools, for
      libwdi in the installer (scripts\package-windows.ps1; see packaging\windows\README.md)

.PARAMETER Arch     x64 or arm64 (default: this machine's).
.PARAMETER Msys2    MSYS2 root (default C:\msys64).
.PARAMETER Refresh  Sync the package databases first (pacman -Sy). For a fresh MSYS2 (CI);
                    on an existing installation update it as a whole instead (pacman -Syu).
.PARAMETER Tests    Also install ffmpeg (mingw-w64-ucrt-x86_64-ffmpeg / mingw-w64-clang-aarch64-ffmpeg):
                    the ctest suite makes its test recordings with ffmpeg.exe (tests\data\README.md);
                    without it those tests skip. The core links its own FFmpeg statically and loads
                    none of the DLLs this package puts next to ffmpeg.exe.
#>
[CmdletBinding()]
param(
    [ValidateSet('x64', 'arm64')] [string]$Arch = $(if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' }),
    [string]$Msys2 = 'C:\msys64',
    [switch]$Refresh,
    [switch]$Tests
)

$ErrorActionPreference = 'Stop'

$bash = Join-Path $Msys2 'usr\bin\bash.exe'
if (-not (Test-Path $bash)) { throw "MSYS2 not found in $Msys2 (install it from https://www.msys2.org, or pass -Msys2 <root>)" }

$pkgs = @('make', 'diffutils', 'mingw-w64-ucrt-x86_64-nsis')
if ($Arch -eq 'arm64') {
    $pkgs += @('clang', 'cmake', 'ninja', 'libusb', 'pkgconf' | ForEach-Object { "mingw-w64-clang-aarch64-$_" })
} else {
    $pkgs += @('gcc', 'cmake', 'ninja', 'libusb', 'pkgconf', 'nasm' | ForEach-Object { "mingw-w64-ucrt-x86_64-$_" })
}
if ($Tests) {
    $pkgs += $(if ($Arch -eq 'arm64') { 'mingw-w64-clang-aarch64-ffmpeg' } else { 'mingw-w64-ucrt-x86_64-ffmpeg' })
}

$op = if ($Refresh) { '-Sy' } else { '-S' }
Write-Host "pacman $op --needed $($pkgs -join ' ')"
& $bash -lc "pacman $op --needed --noconfirm $($pkgs -join ' ')"
if ($LASTEXITCODE -ne 0) { throw "pacman failed with exit code $LASTEXITCODE" }

if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    Write-Warning 'dotnet not found: the GUI needs the .NET 10 SDK (winget install Microsoft.DotNet.SDK.10)'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = if (Test-Path $vswhere) {
    & $vswhere -latest -products * -requires Microsoft.Component.MSBuild Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath
}
if (-not $vs) {
    Write-Warning 'No Visual Studio / Build Tools with the C++ x64/x86 and ARM64 build tools: needed by scripts\package-windows.ps1 for libwdi (see packaging\windows\README.md)'
}
Write-Host 'Done.'
