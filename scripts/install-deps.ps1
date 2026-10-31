<#
.SYNOPSIS
  Installs the MSYS2 packages scripts\build.ps1 and scripts\package-windows.ps1 need.

.DESCRIPTION
  Runs pacman -S --needed in an existing MSYS2 installation (https://www.msys2.org) for
  the toolchain of -Arch:

    x64     UCRT64 gcc, cmake, ninja, libusb, pkgconf, nasm
    arm64   CLANGARM64 clang, cmake, ninja, libusb, pkgconf (MSYS2 has no gcc there)

  plus make, diffutils (FFmpeg's build), autotools (the libwdi build, x64 only) and NSIS
  (the x86_64 package on both: NSIS makes x86 installers anyway). Packages already
  installed are left alone.

  The .NET 10 SDK is not installed by this script (https://dotnet.microsoft.com/download,
  or winget install Microsoft.DotNet.SDK.10); it only warns when dotnet is missing.

.PARAMETER Arch     x64 or arm64 (default: this machine's).
.PARAMETER Msys2    MSYS2 root (default C:\msys64).
.PARAMETER Refresh  Sync the package databases first (pacman -Sy). For a fresh MSYS2 (CI);
                    on an existing installation update it as a whole instead (pacman -Syu).
#>
[CmdletBinding()]
param(
    [ValidateSet('x64', 'arm64')] [string]$Arch = $(if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' }),
    [string]$Msys2 = 'C:\msys64',
    [switch]$Refresh
)

$ErrorActionPreference = 'Stop'

$bash = Join-Path $Msys2 'usr\bin\bash.exe'
if (-not (Test-Path $bash)) { throw "MSYS2 not found in $Msys2 (install it from https://www.msys2.org, or pass -Msys2 <root>)" }

$pkgs = @('make', 'diffutils', 'mingw-w64-ucrt-x86_64-nsis')
if ($Arch -eq 'arm64') {
    $pkgs += @('clang', 'cmake', 'ninja', 'libusb', 'pkgconf' | ForEach-Object { "mingw-w64-clang-aarch64-$_" })
} else {
    $pkgs += 'autotools'
    $pkgs += @('gcc', 'cmake', 'ninja', 'libusb', 'pkgconf', 'nasm' | ForEach-Object { "mingw-w64-ucrt-x86_64-$_" })
}

$op = if ($Refresh) { '-Sy' } else { '-S' }
Write-Host "pacman $op --needed $($pkgs -join ' ')"
& $bash -lc "pacman $op --needed --noconfirm $($pkgs -join ' ')"
if ($LASTEXITCODE -ne 0) { throw "pacman failed with exit code $LASTEXITCODE" }

if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    Write-Warning 'dotnet not found: the GUI needs the .NET 10 SDK (winget install Microsoft.DotNet.SDK.10)'
}
Write-Host 'Done.'
