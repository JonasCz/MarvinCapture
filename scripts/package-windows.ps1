<#
.SYNOPSIS
  Builds the Windows installer: build\windows-<arch>\installer\MarvinCapture-<version>-windows-<arch>-setup.exe
  (<arch> = x86_64 or arm64).

.DESCRIPTION
  1. Runs scripts\build.ps1 (skip with -SkipBuild to reuse build\windows-<arch>\dist).
  2. Builds libwdi's wdi-simple.exe (the command-line WinUSB driver installer behind Zadig)
     with Visual Studio (MSBuild), once, into build\windows-libwdi\ (the same file for both
     architectures). Pinned and verified:

       libwdi v1.5.1 source tarball   downloaded from GitHub, SHA256 checked
       WDK 8.0 redistributable MSI    downloaded from download.microsoft.com, SHA256 checked
                                      (WdfCoInstaller01011.dll / winusbcoinstaller2.dll, which
                                      libwdi embeds and its WinUSB INF template references;
                                      libwdi's own CI uses the same files)

     The WDK MSI is only unpacked (msiexec /a, no install) into the source tree. libwdi's
     msvc\config.h is pointed at it, with libusb-win32/libusbK left out, and libwdi.sln is
     built for Win32 Release, as the official Zadig is: a 32-bit wdi-simple.exe that embeds
     the INF/catalog templates, the co-installers and the elevated helpers installer_x86.exe,
     installer_x64.exe and installer_arm64.exe. It runs on x64 Windows and, through x86
     emulation, on Windows on Arm (libwdi: Windows 10 1511 or later), and starts the helper
     for the machine. Only the MSVC build has the ARM64 helper; MinGW/autotools builds of
     libwdi are x86/x64 only.
     At install time it generates a WinUSB INF for ONE hardware ID, a catalog, signs it with a
     throw-away self-signed certificate (put into LocalMachine Root + TrustedPublisher, private
     key destroyed) and installs it. For a device that is not plugged in it only stages the
     package (SetupCopyOEMInf), so Windows binds it on the next plug-in.
  3. Runs makensis on packaging\windows\MarvinCapture.nsi.

  Requires, besides what build.ps1 needs:
    * NSIS 3.x: the official installer (https://nsis.sourceforge.io/Download, default path
      C:\Program Files (x86)\NSIS) or MSYS2:  pacman -S mingw-w64-ucrt-x86_64-nsis
    * for the libwdi build (first time only): Visual Studio 2022 or later, or its Build Tools,
      with MSBuild and the C++ build tools for x64/x86 and for ARM64 (components
      Microsoft.VisualStudio.Component.VC.Tools.x86.x64 and ...VC.Tools.ARM64), and
      Windows' tar.exe (Windows 10+).
  Needs network access the first time (the two downloads are cached in the libwdi work dir).

.PARAMETER Config       Release (default) or Debug; passed to build.ps1.
.PARAMETER SkipBuild    Don't run build.ps1; package the existing build\windows-<arch>\dist.
.PARAMETER SkipTests    Passed to build.ps1.
.PARAMETER RebuildWdi   Rebuild wdi-simple.exe even if a cached one exists.
.PARAMETER Msys2        MSYS2 root (default C:\msys64).
.PARAMETER Makensis     Path of makensis.exe (default: search PATH, NSIS and MSYS2 locations).
.PARAMETER Arch         x64 or arm64 (default: this machine's); passed to build.ps1.
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')] [string]$Config = 'Release',
    [switch]$SkipBuild,
    [switch]$SkipTests,
    [switch]$RebuildWdi,
    [string]$Msys2 = 'C:\msys64',
    [string]$Makensis,
    [ValidateSet('x64', 'arm64')] [string]$Arch = $(if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' })
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$platform = if ($Arch -eq 'arm64') { 'windows-arm64' } else { 'windows-x86_64' }
$build = Join-Path $root "build\$platform"
$dist = Join-Path $build 'dist'
$outDir = Join-Path $build 'installer'
# wdi-simple.exe is the same for both architectures (32-bit, all helpers embedded)
$wdiOut = Join-Path $root 'build\windows-libwdi'
$src = Join-Path $root 'packaging\windows'

# Pinned third-party inputs (see .DESCRIPTION)
$wdiVersion = '1.5.1'
$wdiUrl = "https://github.com/pbatard/libwdi/archive/refs/tags/v$wdiVersion.tar.gz"
$wdiSha = 'a695e93db0977dfdc5c6a99a4ea91b22f9027547d0177b2a0f3075078643c929'
$wdkUrl = 'https://download.microsoft.com/download/0/5/F/05FD6919-6250-425B-86ED-9B095E54065A/wdfcoinstaller.msi'
$wdkSha = '29314207814ce9d5d73695f7e9239539cf37c79e750b9d5ea5a5ef5487a583d6'

function Step($msg) { Write-Host "`n== $msg" -ForegroundColor Cyan }
function Run {
    param([string]$Exe, [string[]]$Arguments)
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed with exit code $LASTEXITCODE" }
}
function Get-Download {
    param([string]$Url, [string]$File, [string]$Sha256)
    if (Test-Path $File) {
        if ((Get-FileHash $File -Algorithm SHA256).Hash.ToLower() -eq $Sha256) { return }
        Remove-Item -Force $File
    }
    Write-Host "Downloading $Url"
    $old = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'   # progress rendering makes Invoke-WebRequest very slow
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -Uri $Url -OutFile $File -UseBasicParsing
    } finally { $ProgressPreference = $old }
    $got = (Get-FileHash $File -Algorithm SHA256).Hash.ToLower()
    if ($got -ne $Sha256) {
        Remove-Item -Force $File
        throw "SHA256 mismatch for $Url`n  expected $Sha256`n  got      $got"
    }
}

# --- version ---------------------------------------------------------------------
$version = [string](Get-Content (Join-Path $root 'VERSION') -Raw | ConvertFrom-Json).version
if ($version -notmatch '^\d+(\.\d+){0,3}$') { throw "VERSION's ""version"" must look like 1.2 or 1.2.3 (got '$version')" }
# four-part for the file version resource
$versionNum = (@($version.Split('.')) + @('0', '0', '0'))[0..3] -join '.'
$exeName = "MarvinCapture-$version-$platform-setup.exe"
Write-Host "MarvinCapture $version"

# --- makensis --------------------------------------------------------------------
$nsis = $null
if ($Makensis) {
    $nsis = $Makensis
} else {
    $cands = @()
    $cmd = Get-Command makensis.exe -ErrorAction SilentlyContinue
    if ($cmd) { $cands += $cmd.Source }
    $cands += (Join-Path ${env:ProgramFiles(x86)} 'NSIS\makensis.exe')
    $cands += (Join-Path $env:ProgramFiles 'NSIS\makensis.exe')
    $cands += (Join-Path $Msys2 'ucrt64\bin\makensis.exe')
    $cands += (Join-Path $Msys2 'clangarm64\bin\makensis.exe')
    $cands += (Join-Path $Msys2 'mingw32\bin\makensis.exe')
    $nsis = $cands | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
}
if (-not $nsis -or -not (Test-Path $nsis)) {
    throw @"
makensis.exe (NSIS 3.x) not found. Install it with one of:
  * the official installer from https://nsis.sourceforge.io/Download (default path
    C:\Program Files (x86)\NSIS is searched), or
  * MSYS2:  pacman -S mingw-w64-ucrt-x86_64-nsis   (found in $Msys2\ucrt64\bin)
or pass -Makensis <path>.
"@
}
$nsisVer = (& $nsis /VERSION) -join ''
Write-Host "makensis: $nsis ($nsisVer)"
if ($nsisVer -match 'v(\d+)\.' -and [int]$Matches[1] -lt 3) { throw "NSIS 3.x is required (found $nsisVer)" }

# --- 1. application build ----------------------------------------------------------
if (-not $SkipBuild) {
    Step 'Building the application (scripts\build.ps1)'
    $a = @{ Config = $Config; Msys2 = $Msys2; Arch = $Arch }
    if ($SkipTests) { $a.SkipTests = $true }
    & (Join-Path $PSScriptRoot 'build.ps1') @a
    if (-not $?) { throw 'build.ps1 failed' }
}
$expect = @('MarvinCaptureGUI.exe', 'MarvinCaptureCLI.exe', 'marvin-core.dll', 'libusb-1.0.dll',
            'libwinpthread-1.dll', 'firmware\fpga-ohci.bin', 'firmware\fpga-capture.bin', 'firmware\fx2-marvin.bin')
$missing = $expect | Where-Object { -not (Test-Path (Join-Path $dist $_)) }
if ($missing) { throw "missing from $dist (run without -SkipBuild): $($missing -join ', ')" }

# --- 2. libwdi: wdi-simple.exe ---------------------------------------------------------
$wdiExe = Join-Path $wdiOut 'wdi-simple.exe'
$wdiStamp = Join-Path $wdiOut 'build.stamp'
$stampText = "libwdi $wdiVersion $wdiSha wdk $wdkSha msvc win32"
$wdiFresh = (Test-Path $wdiExe) -and (Test-Path $wdiStamp) -and ((Get-Content $wdiStamp -Raw).Trim() -eq $stampText)
if ($RebuildWdi -or -not $wdiFresh) {
    Step "Building libwdi $wdiVersion (wdi-simple.exe, MSVC)"
    # MSBuild and the C++ compilers for all three helper architectures
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw "Visual Studio not found ($vswhere is missing). Install the Visual Studio Build Tools, see packaging\windows\README.md" }
    $vsReq = @('-products', '*', '-requires', 'Microsoft.Component.MSBuild',
               'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', 'Microsoft.VisualStudio.Component.VC.Tools.ARM64')
    $vs = & $vswhere -latest @vsReq -format json | ConvertFrom-Json | Select-Object -First 1
    if (-not $vs) {
        throw 'No Visual Studio / Build Tools with MSBuild and the C++ x64/x86 and ARM64 build tools found (components VC.Tools.x86.x64 and VC.Tools.ARM64), see packaging\windows\README.md'
    }
    $msbuild = Get-ChildItem (Join-Path $vs.installationPath 'MSBuild') -Recurse -Filter MSBuild.exe |
        Where-Object { $_.FullName -match '\\Bin\\(amd64\\|arm64\\)?MSBuild\.exe$' } | Select-Object -First 1
    if (-not $msbuild) { throw "MSBuild.exe not found under $($vs.installationPath)" }
    # libwdi's projects ask for v143 (VS 2022); use the installed Visual Studio's own toolset
    $toolset = switch ([int]($vs.installationVersion.Split('.')[0])) { 17 { 'v143' } 18 { 'v145' } default { 'v143' } }
    Write-Host "Visual Studio $($vs.installationVersion) ($toolset): $($msbuild.FullName)"
    $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
    if (-not (Test-Path $tar)) { throw 'Windows tar.exe not found (needs Windows 10 or later)' }

    $work = Join-Path $wdiOut 'work'
    Write-Host "work dir: $work"
    New-Item -ItemType Directory -Force $wdiOut, $work | Out-Null

    $tgz = Join-Path $work "libwdi-$wdiVersion.tar.gz"
    $msi = Join-Path $work 'wdfcoinstaller.msi'
    Get-Download $wdiUrl $tgz $wdiSha
    Get-Download $wdkUrl $msi $wdkSha

    $srcDir = Join-Path $work "libwdi-$wdiVersion"
    if (Test-Path $srcDir) { Remove-Item -Recurse -Force $srcDir }
    Run $tar @('-xzf', $tgz, '-C', $work)
    if (-not (Test-Path (Join-Path $srcDir 'libwdi.sln'))) { throw "unexpected libwdi archive layout in $work" }

    # Extract the WDK redistributable into <src>\wdk (administrative install, nothing is installed).
    $wdkDir = Join-Path $srcDir 'wdk'
    $p = Start-Process msiexec.exe -ArgumentList @('/a', "`"$msi`"", '/qn', "TARGETDIR=`"$wdkDir`"") -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "msiexec /a failed with exit code $($p.ExitCode)" }
    $kit = Join-Path $wdkDir 'Windows Kits\8.0'
    foreach ($wa in 'x86', 'x64') {
        foreach ($f in 'WdfCoInstaller01011.dll', 'winusbcoinstaller2.dll') {
            if (-not (Test-Path (Join-Path $kit "redist\wdf\$wa\$f"))) { throw "$f ($wa) not found under $kit\redist\wdf" }
        }
    }

    # msvc\config.h is libwdi's build configuration for MSVC: point it at the WDK files and
    # leave out libusb-win32 and libusbK (only WinUSB is used). Its defaults keep x86, x64 and
    # ARM64 (OPT_M32/OPT_M64/OPT_ARM): all three elevated helpers get embedded.
    $cfgPath = Join-Path $srcDir 'msvc\config.h'
    $cfg = [IO.File]::ReadAllText($cfgPath)
    $edits = [ordered]@{
        '#define WDK_DIR "C:/Program Files (x86)/Windows Kits/8.0"' = "#define WDK_DIR `"$($kit -replace '\\', '/')`""
        '#define LIBUSB0_DIR "D:/libusb-win32"' = '/* libusb-win32: not embedded */'
        '#define LIBUSBK_DIR "D:/libusbK/bin"'  = '/* libusbK: not embedded */'
    }
    foreach ($k in $edits.Keys) {
        if (-not $cfg.Contains($k)) { throw "msvc\config.h: '$k' not found (libwdi changed?)" }
        $cfg = $cfg.Replace($k, $edits[$k])
    }
    foreach ($opt in 'OPT_M32', 'OPT_M64', 'OPT_ARM') {
        if ($cfg -notmatch "(?m)^#define $opt\b") { throw "msvc\config.h: $opt is not defined (libwdi changed?)" }
    }
    [IO.File]::WriteAllText($cfgPath, $cfg)

    # Win32 Release, like the official Zadig: the 32-bit wdi-simple.exe runs on x64 Windows and
    # (x86 emulation) on Windows on Arm, and starts the helper that matches the machine.
    Run $msbuild.FullName @((Join-Path $srcDir 'libwdi.sln'), '/m', '/nologo', '/v:minimal', '/t:examples\wdi-simple',
                            '/p:Configuration=Release', '/p:Platform=Win32', "/p:PlatformToolset=$toolset")

    $built = Join-Path $srcDir 'Win32\Release\examples\wdi-simple.exe'
    if (-not (Test-Path $built)) { throw "wdi-simple.exe was not built ($built)" }
    foreach ($h in 'Win32', 'x64', 'arm64') {
        if (-not (Test-Path (Join-Path $srcDir "$h\Release\helper"))) { throw "libwdi helper for $h was not built" }
    }
    Copy-Item $built $wdiExe -Force
    Copy-Item (Join-Path $srcDir 'COPYING-LGPL') (Join-Path $wdiOut 'libwdi-COPYING-LGPL.txt') -Force
    Copy-Item (Join-Path $srcDir 'COPYING') (Join-Path $wdiOut 'libwdi-COPYING.txt') -Force
    @"
wdi-simple.exe in this folder is built from libwdi $wdiVersion (LGPL-3.0-or-later) with
Visual Studio (32-bit, with the x86, x64 and ARM64 installer helpers):
  source:  $wdiUrl
  sha256:  $wdiSha
  project: https://github.com/pbatard/libwdi
It embeds the WinUSB co-installers from the Microsoft WDK 8.0 redistributable package
(wdfcoinstaller.msi, redistributable under the WDK redistribution terms).
"@ | Set-Content (Join-Path $wdiOut 'libwdi-SOURCE.txt') -Encoding UTF8
    Set-Content $wdiStamp $stampText
}
# files installed next to wdi-simple.exe
$wdiDocs = Join-Path $wdiOut 'docs'
New-Item -ItemType Directory -Force $wdiDocs | Out-Null
foreach ($f in 'libwdi-COPYING-LGPL.txt', 'libwdi-COPYING.txt', 'libwdi-SOURCE.txt') {
    Copy-Item (Join-Path $wdiOut $f) $wdiDocs -Force
}

# --- 3. installer ----------------------------------------------------------------------
Step 'Building the installer'
New-Item -ItemType Directory -Force $outDir | Out-Null
$out = Join-Path $outDir $exeName
if (Test-Path $out) { Remove-Item -Force $out }
# the licence page wants CRLF line endings
$lic = Join-Path $outDir 'LICENSE.txt'
$licText = [IO.File]::ReadAllText((Join-Path $root 'LICENSE')) -replace "`r?`n", "`r`n"
[IO.File]::WriteAllText($lic, $licText, (New-Object Text.UTF8Encoding($false)))

$nsisArgs = @(
    '/V2',
    "/DVERSION=$version",
    "/DVERSION_NUM=$versionNum",
    "/DARCH=$Arch",
    "/DDIST=$dist",
    "/DROOT=$root",
    "/DLICENSE_FILE=$lic",
    "/DOUTFILE=$out",
    "/DWDI_SIMPLE=$wdiExe",
    "/DWDI_DOCS=$wdiDocs"
)
$nsisArgs += (Join-Path $src 'MarvinCapture.nsi')
Run $nsis $nsisArgs

if (-not (Test-Path $out)) { throw "installer not produced: $out" }
$hash = (Get-FileHash $out -Algorithm SHA256).Hash.ToLower()
$mb = [math]::Round((Get-Item $out).Length / 1MB, 1)
Write-Host "`nDone: $out ($mb MB)" -ForegroundColor Green
Write-Host "sha256: $hash"
