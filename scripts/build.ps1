<#
.SYNOPSIS
  Builds everything and assembles a runnable output in build\windows-<arch>\dist
  (windows-x86_64 or windows-arm64).

.DESCRIPTION
  1. (first time only) builds the minimal static FFmpeg into third_party\.
  2. Configures and builds the native core (marvin-core.dll, MarvinCaptureCLI, MarvinPlayout)
     with MSYS2 (x64: UCRT64 gcc, arm64: CLANGARM64 clang) into
     build\windows-<arch>\core, and runs the ctest suite.
  3. Builds the WinUI 3 app against that core (intermediates in
     build\windows-<arch>\gui). Release uses dotnet publish (trimmed, ReadyToRun,
     without the debugger DLLs and WinUI's non-English resources; see
     docs\windows-size.md), Debug a plain dotnet build.
  4. Assembles build\windows-<arch>\dist (emptied first):

       MarvinCaptureGUI.exe       the GUI
       MarvinCaptureCLI.exe       the command-line program
       MarvinPlayout.exe          plays DV/HDV files out as a virtual deck (docs\playout.md)
       marvin-core.dll            the core library (+ libusb-1.0.dll,
                                  libwinpthread-1.dll)
       firmware\                  FPGA bitstreams

  Build output is per platform (build\<os>-<arch>\), so a macOS or Linux build
  (scripts/build.sh) can live in the same tree.

  Requires MSYS2 (x64: UCRT64 gcc, cmake, ninja, libusb, pkgconf, nasm, make,
  diffutils; arm64: the same from CLANGARM64 with clang instead of gcc and no nasm)
  and the .NET 10 SDK. See docs\building.md.

.PARAMETER Config     Release (default) or Debug.
.PARAMETER SkipTests  Don't run ctest.
.PARAMETER SkipGui    Build the native core and CLI only.
.PARAMETER Clean      Delete build\windows-<arch>\ first (other platforms' output is kept).
.PARAMETER Arch       x64 or arm64 (default: this machine's).
.PARAMETER Msys2      MSYS2 root (default C:\msys64).
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')] [string]$Config = 'Release',
    [switch]$SkipTests,
    [switch]$SkipGui,
    [switch]$Clean,
    [string]$Msys2 = 'C:\msys64',
    [ValidateSet('x64', 'arm64')] [string]$Arch = $(if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' })
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
# x64: MSYS2 UCRT64 (gcc); arm64: CLANGARM64 (clang, MSYS2 has no gcc for Windows on Arm)
if ($Arch -eq 'arm64') {
    $platform = 'windows-arm64'; $msysEnv = 'clangarm64'; $msystem = 'CLANGARM64'; $cc = 'clang'; $dotnetPlatform = 'ARM64'
} else {
    $platform = 'windows-x86_64'; $msysEnv = 'ucrt64'; $msystem = 'UCRT64'; $cc = 'gcc'; $dotnetPlatform = 'x64'
}
# per-platform output dir, named like third_party\ffmpeg-windows-x86_64
$build = Join-Path $root "build\$platform"
$core = Join-Path $build 'core'
$dist = Join-Path $build 'dist'

function Step($msg) { Write-Host "`n== $msg" -ForegroundColor Cyan }
function Run {
    # Run a native command, stop on a non-zero exit code.
    param([string]$Exe, [string[]]$Arguments)
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed with exit code $LASTEXITCODE" }
}

$ucrt = Join-Path $Msys2 "$msysEnv\bin"
if (-not (Test-Path (Join-Path $ucrt "$cc.exe"))) {
    throw "MSYS2 $msystem $cc not found in $ucrt (use -Msys2 <root>; see docs\building.md)"
}
$env:PATH = "$ucrt;$env:PATH"
$env:CC = $cc

if ($Clean -and (Test-Path $build)) {
    Step "Cleaning build\$platform\"
    Remove-Item -Recurse -Force $build
}

# --- 1. FFmpeg ---------------------------------------------------------------
$ffPrefix = Join-Path $root "third_party\ffmpeg-$platform"
$ffmpeg = Join-Path $ffPrefix 'lib\pkgconfig'
# rebuilt when scripts\build-ffmpeg.sh changed since the installed build (a component was added)
$ffStamp = Join-Path $ffPrefix 'build-script.sha256'
$ffHash = (Get-FileHash (Join-Path $root 'scripts\build-ffmpeg.sh') -Algorithm SHA256).Hash.ToLower()
$ffStale = (Test-Path $ffmpeg) -and -not ((Test-Path $ffStamp) -and ((Get-Content $ffStamp -Raw).Trim() -eq $ffHash))
if ($ffStale -or -not (Test-Path $ffmpeg)) {
    Step 'Building the minimal static FFmpeg (first time, or build-ffmpeg.sh changed)'
    $sh = Join-Path $Msys2 'usr\bin\bash.exe'
    $env:MSYSTEM = $msystem
    $env:CHERE_INVOKING = '1'
    Run $sh @('-lc', 'scripts/build-ffmpeg.sh')
}

# --- 2. Native core, CLIs, tests -----------------------------------------------
Step "Configuring the native core ($Config)"
# PIN_FFMPEG_PREFIX: CMake's processor detection is unreliable under MSYS2 (see src\sinks\CMakeLists.txt)
# (the compiler comes from $env:CC, set above)
Run cmake @('-G', 'Ninja', '-S', $root, '-B', $core, "-DCMAKE_BUILD_TYPE=$Config",
            "-DPIN_FFMPEG_PREFIX=$($ffPrefix -replace '\\', '/')")
Step 'Building the native core'
Run cmake @('--build', $core)

if (-not $SkipTests) {
    Step 'Running tests'
    Run ctest @('--test-dir', $core, '--output-on-failure')
}

# --- 3. Assemble build\windows-<arch>\dist -------------------------------------
Step "Assembling build\$platform\dist"
# MarvinCaptureCLI.exe goes next to the GUI: they share marvin-core.dll and firmware\
# (the GUI build below copies the same files again).
# Start from an empty dist\: the installer packs everything in it, so files from an older
# build (e.g. DLLs that are no longer published) must not survive.
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item (Join-Path $core 'MarvinCaptureCLI.exe') $dist -Force
Copy-Item (Join-Path $core 'MarvinPlayout.exe') $dist -Force   # links the core statically, finds firmware\ next to itself
Copy-Item (Join-Path $core 'marvin-core.dll') $dist -Force
Copy-Item (Join-Path $core 'libusb-1.0.dll') $dist -Force
Copy-Item (Join-Path $ucrt 'libwinpthread-1.dll') $dist -Force
# The CLI needs the DLLs copied above to start, so its help text is read only now.
# The help text lives in src\engine\pin_script.c; docs\cli.md is generated from docs\cli.md.template.
$helpLines = & (Join-Path $dist 'MarvinCaptureCLI.exe') --help-text
if ($LASTEXITCODE -ne 0 -or -not $helpLines) { throw "MarvinCaptureCLI.exe --help-text failed (exit code $LASTEXITCODE); docs\cli.md left as it was" }
$helpText = $helpLines -join "`n"
$tmpl = [IO.File]::ReadAllText((Join-Path $root 'docs\cli.md.template'))
$new = $tmpl.Replace('@CLI_HELP@', "``````text`n$helpText`n``````")
$helpDoc = Join-Path $root 'docs\cli.md'
if (-not (Test-Path $helpDoc) -or [IO.File]::ReadAllText($helpDoc) -ne $new) { [IO.File]::WriteAllText($helpDoc, $new) }
Copy-Item (Join-Path $root 'firmware') (Join-Path $dist 'firmware') -Recurse

# --- 4. GUI --------------------------------------------------------------------
if (-not $SkipGui) {
    $proj = Join-Path $root 'gui\windows\MarvinCaptureGUI\MarvinCaptureGUI.csproj'
    if ($Config -eq 'Release') {
        # publish, not build: trimming, ReadyToRun and the pruning of debug DLLs and WinUI
        # languages only happen on publish (see the csproj and docs\windows-size.md)
        Step 'Publishing the GUI (trimmed, ReadyToRun)'
        Run dotnet @('publish', $proj, '-c', $Config, "-p:Platform=$dotnetPlatform",
                     "-p:PinnacleCoreDir=$core", "-p:PinnacleRuntimeDllDir=$ucrt", '-o', "$dist\")
    } else {
        Step 'Building the GUI'
        Run dotnet @('build', $proj, '-c', $Config, "-p:Platform=$dotnetPlatform",
                     "-p:PinnacleCoreDir=$core", "-p:PinnacleRuntimeDllDir=$ucrt", "-p:OutDir=$dist\")
    }
}

# --- Check ---------------------------------------------------------------------
Step "Checking build\$platform\dist"
$expect = @('MarvinCaptureCLI.exe', 'MarvinPlayout.exe', 'marvin-core.dll', 'libusb-1.0.dll', 'libwinpthread-1.dll',
            'firmware\fpga-ohci.bin', 'firmware\fpga-capture.bin', 'firmware\fx2-marvin.bin')
if (-not $SkipGui) {
    $expect += 'MarvinCaptureGUI.exe'
}
$missing = $expect | Where-Object { -not (Test-Path (Join-Path $dist $_)) }
if ($missing) { throw "missing from build\$platform\dist: $($missing -join ', ')" }

Write-Host "`nDone: $dist" -ForegroundColor Green
