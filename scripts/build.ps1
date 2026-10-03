<#
.SYNOPSIS
  Builds everything and assembles a runnable output in build\dist.

.DESCRIPTION
  1. (first time only) builds the minimal static FFmpeg into third_party\.
  2. Configures and builds the native core (marvin-core.dll, the CLIs)
     with MSYS2 UCRT64 gcc into build\core, and runs the ctest suite.
  3. Builds the WinUI 3 app against that core.
  4. Assembles build\dist:

       build\dist\MarvinCaptureGUI.exe       the GUI
       build\dist\MarvinCaptureCLI.exe       the command-line program
       build\dist\marvin-core.dll            the core library (+ libusb-1.0.dll)
       build\dist\firmware\                  FPGA bitstreams
       build\dist\cli\                       pincli, pinanalog, pindeck, pinlist,
                                             with their DLLs and firmware\

  Requires MSYS2 (UCRT64: gcc, cmake, ninja, libusb, pkgconf, nasm, make,
  diffutils) and the .NET 10 SDK. See docs\building.md.

.PARAMETER Config     Release (default) or Debug.
.PARAMETER SkipTests  Don't run ctest.
.PARAMETER SkipGui    Build the native core and CLIs only.
.PARAMETER Clean      Delete build\ first.
.PARAMETER Msys2      MSYS2 root (default C:\msys64).
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')] [string]$Config = 'Release',
    [switch]$SkipTests,
    [switch]$SkipGui,
    [switch]$Clean,
    [string]$Msys2 = 'C:\msys64'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
$core = Join-Path $build 'core'
$dist = Join-Path $build 'dist'

function Step($msg) { Write-Host "`n== $msg" -ForegroundColor Cyan }
function Run {
    # Run a native command, stop on a non-zero exit code.
    param([string]$Exe, [string[]]$Arguments)
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed with exit code $LASTEXITCODE" }
}

$ucrt = Join-Path $Msys2 'ucrt64\bin'
if (-not (Test-Path (Join-Path $ucrt 'gcc.exe'))) {
    throw "MSYS2 UCRT64 gcc not found in $ucrt (use -Msys2 <root>; see docs\building.md)"
}
$env:PATH = "$ucrt;$env:PATH"

if ($Clean -and (Test-Path $build)) {
    Step 'Cleaning build\'
    Remove-Item -Recurse -Force $build
}

# --- 1. FFmpeg ---------------------------------------------------------------
$ffmpeg = Join-Path $root 'third_party\ffmpeg-windows-x86_64\lib\pkgconfig'
# rebuilt when scripts\build-ffmpeg.sh changed since the installed build (a component was added)
$ffStamp = Join-Path $root 'third_party\ffmpeg-windows-x86_64\build-script.sha256'
$ffHash = (Get-FileHash (Join-Path $root 'scripts\build-ffmpeg.sh') -Algorithm SHA256).Hash.ToLower()
$ffStale = (Test-Path $ffmpeg) -and -not ((Test-Path $ffStamp) -and ((Get-Content $ffStamp -Raw).Trim() -eq $ffHash))
if ($ffStale -or -not (Test-Path $ffmpeg)) {
    Step 'Building the minimal static FFmpeg (first time, or build-ffmpeg.sh changed)'
    $sh = Join-Path $Msys2 'usr\bin\bash.exe'
    $env:MSYSTEM = 'UCRT64'
    $env:CHERE_INVOKING = '1'
    Run $sh @('-lc', 'scripts/build-ffmpeg.sh')
}

# --- 2. Native core, CLIs, tests -----------------------------------------------
Step "Configuring the native core ($Config)"
Run cmake @('-G', 'Ninja', '-S', $root, '-B', $core, "-DCMAKE_BUILD_TYPE=$Config")
Step 'Building the native core'
Run cmake @('--build', $core)

if (-not $SkipTests) {
    Step 'Running tests'
    Run ctest @('--test-dir', $core, '--output-on-failure')
}

# --- 3. Assemble the CLI directory ---------------------------------------------
Step 'Assembling build\dist\cli'
$cli = Join-Path $dist 'cli'
New-Item -ItemType Directory -Force $cli | Out-Null
foreach ($f in 'pincli', 'pinanalog', 'pindeck', 'pinlist') {
    Copy-Item (Join-Path $core "$f.exe") $cli -Force
}
Copy-Item (Join-Path $core 'marvin-core.dll') $cli -Force
Copy-Item (Join-Path $core 'libusb-1.0.dll') $cli -Force
# the CLIs import libwinpthread-1.dll; without a copy next to them Windows picks up
# whatever else is on PATH (Git's mingw64, ...) and they die with 0xC0000139
Copy-Item (Join-Path $ucrt 'libwinpthread-1.dll') $cli -Force
Remove-Item -Recurse -Force (Join-Path $cli 'firmware') -ErrorAction SilentlyContinue
Copy-Item (Join-Path $root 'firmware') (Join-Path $cli 'firmware') -Recurse

# MarvinCaptureCLI.exe goes next to the GUI: they share marvin-core.dll and firmware\
# (the GUI build below copies the same files again).
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item (Join-Path $core 'MarvinCaptureCLI.exe') $dist -Force
Copy-Item (Join-Path $core 'marvin-core.dll') $dist -Force
Copy-Item (Join-Path $core 'libusb-1.0.dll') $dist -Force
Copy-Item (Join-Path $ucrt 'libwinpthread-1.dll') $dist -Force
Remove-Item -Recurse -Force (Join-Path $dist 'firmware') -ErrorAction SilentlyContinue
Copy-Item (Join-Path $root 'firmware') (Join-Path $dist 'firmware') -Recurse

# --- 4. GUI --------------------------------------------------------------------
if (-not $SkipGui) {
    Step 'Building the GUI'
    $proj = Join-Path $root 'gui\windows\MarvinCaptureGUI\MarvinCaptureGUI.csproj'
    Run dotnet @('build', $proj, '-c', $Config, '-p:Platform=x64',
                 "-p:PinnacleCoreDir=$core", "-p:OutDir=$dist\")
}

# --- Check ---------------------------------------------------------------------
Step 'Checking build\dist'
$expect = @('MarvinCaptureCLI.exe', 'marvin-core.dll', 'libusb-1.0.dll',
            'firmware\fpga-ohci.bin', 'firmware\fpga-capture.bin', 'firmware\fx2-marvin.bin',
            'cli\pincli.exe', 'cli\marvin-core.dll',
            'cli\libusb-1.0.dll', 'cli\libwinpthread-1.dll',
            'cli\firmware\fpga-ohci.bin', 'cli\firmware\fpga-capture.bin',
            'cli\firmware\fx2-marvin.bin')
if (-not $SkipGui) {
    $expect += 'MarvinCaptureGUI.exe'
}
$missing = $expect | Where-Object { -not (Test-Path (Join-Path $dist $_)) }
if ($missing) { throw "missing from build\dist: $($missing -join ', ')" }

Write-Host "`nDone: $dist" -ForegroundColor Green
