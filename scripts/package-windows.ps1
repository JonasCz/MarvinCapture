<#
.SYNOPSIS
  Builds the Windows installer: build\windows-x86_64\installer\MarvinCapture-<version>-windows-x86_64-setup.exe

.DESCRIPTION
  1. Runs scripts\build.ps1 (skip with -SkipBuild to reuse build\windows-x86_64\dist).
  2. Builds libwdi's wdi-simple.exe (the command-line WinUSB driver installer behind Zadig)
     with MSYS2, once, into build\windows-x86_64\libwdi\. Pinned and verified:

       libwdi v1.5.1 source tarball   downloaded from GitHub, SHA256 checked
       WDK 8.0 redistributable MSI    downloaded from download.microsoft.com, SHA256 checked
                                      (WdfCoInstaller01011.dll / winusbcoinstaller2.dll, which
                                      libwdi embeds and its WinUSB INF template references;
                                      libwdi's own CI uses the same files)

     libwdi has no MSYS2 package and its configure insists on a WDK directory for WinUSB,
     so the script extracts the redistributable MSI (msiexec /a, no install) into the source
     tree and builds with: configure --disable-32bit --disable-shared --with-wdkdir=...
     --enable-examples-build. wdi-simple.exe is statically linked and self-contained (it embeds
     the INF/catalog templates, the coinstallers and the elevated helper installer_x64.exe).
     At install time it generates a WinUSB INF for ONE hardware ID, a catalog, signs it with a
     throw-away self-signed certificate (put into LocalMachine Root + TrustedPublisher, private
     key destroyed) and installs it. For a device that is not plugged in it only stages the
     package (SetupCopyOEMInf), so Windows binds it on the next plug-in.
  3. Runs makensis on packaging\windows\MarvinCapture.nsi.

  Requires, besides what build.ps1 needs:
    * NSIS 3.x: the official installer (https://nsis.sourceforge.io/Download, default path
      C:\Program Files (x86)\NSIS) or MSYS2:  pacman -S mingw-w64-ucrt-x86_64-nsis
    * for the libwdi build (first time only): pacman -S --needed autotools
      (autoconf, automake, libtool, make, m4 in the MSYS2 shell; the UCRT64 gcc toolchain from
      docs\building.md is used as compiler) and Windows' tar.exe (Windows 10+).
  Needs network access the first time (the two downloads are cached in the libwdi work dir).

.PARAMETER Config       Release (default) or Debug; passed to build.ps1.
.PARAMETER SkipBuild    Don't run build.ps1; package the existing build\windows-x86_64\dist.
.PARAMETER SkipTests    Passed to build.ps1.
.PARAMETER RebuildWdi   Rebuild wdi-simple.exe even if a cached one exists.
.PARAMETER Msys2        MSYS2 root (default C:\msys64).
.PARAMETER Makensis     Path of makensis.exe (default: search PATH, NSIS and MSYS2 locations).
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')] [string]$Config = 'Release',
    [switch]$SkipBuild,
    [switch]$SkipTests,
    [switch]$RebuildWdi,
    [string]$Msys2 = 'C:\msys64',
    [string]$Makensis
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build\windows-x86_64'
$dist = Join-Path $build 'dist'
$outDir = Join-Path $build 'installer'
$wdiOut = Join-Path $build 'libwdi'
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
$version = (Get-Content (Join-Path $root 'VERSION') -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "VERSION must look like 1.2.3 (got '$version')" }
$exeName = "MarvinCapture-$version-windows-x86_64-setup.exe"
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
    $a = @('-Config', $Config, '-Msys2', $Msys2)
    if ($SkipTests) { $a += '-SkipTests' }
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
$stampText = "libwdi $wdiVersion $wdiSha wdk $wdkSha"
$wdiFresh = (Test-Path $wdiExe) -and (Test-Path $wdiStamp) -and ((Get-Content $wdiStamp -Raw).Trim() -eq $stampText)
if ($RebuildWdi -or -not $wdiFresh) {
    Step "Building libwdi $wdiVersion (wdi-simple.exe)"
    $bash = Join-Path $Msys2 'usr\bin\bash.exe'
    foreach ($t in @($bash, (Join-Path $Msys2 'usr\bin\autoreconf'), (Join-Path $Msys2 'usr\bin\make.exe'),
                     (Join-Path $Msys2 'usr\bin\libtoolize'), (Join-Path $Msys2 'ucrt64\bin\gcc.exe'))) {
        if (-not (Test-Path $t)) {
            throw "$t not found. In an MSYS2 shell run:  pacman -S --needed autotools   (plus the UCRT64 toolchain from docs\building.md)"
        }
    }
    $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
    if (-not (Test-Path $tar)) { throw 'Windows tar.exe not found (needs Windows 10 or later)' }

    # autotools and libtool do not cope with spaces in the source path: use a space-free work dir.
    $work = Join-Path $wdiOut 'work'
    if ($work -match '\s') { $work = Join-Path $env:SystemDrive 'marvincapture-libwdi-work' }
    Write-Host "work dir: $work"
    New-Item -ItemType Directory -Force $wdiOut, $work | Out-Null

    $tgz = Join-Path $work "libwdi-$wdiVersion.tar.gz"
    $msi = Join-Path $work 'wdfcoinstaller.msi'
    Get-Download $wdiUrl $tgz $wdiSha
    Get-Download $wdkUrl $msi $wdkSha

    $srcDir = Join-Path $work "libwdi-$wdiVersion"
    if (Test-Path $srcDir) { Remove-Item -Recurse -Force $srcDir }
    Run $tar @('-xzf', $tgz, '-C', $work)
    if (-not (Test-Path (Join-Path $srcDir 'configure.ac'))) { throw "unexpected libwdi archive layout in $work" }

    # Extract the WDK redistributable into <src>\wdk (administrative install, nothing is installed).
    $wdkDir = Join-Path $srcDir 'wdk'
    $p = Start-Process msiexec.exe -ArgumentList @('/a', "`"$msi`"", '/qn', "TARGETDIR=`"$wdkDir`"") -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "msiexec /a failed with exit code $($p.ExitCode)" }
    $coinst = Get-ChildItem $wdkDir -Recurse -Filter 'WdfCoInstaller01011.dll' |
        Where-Object { $_.FullName -match '\\x64\\|\\amd64\\' } | Select-Object -First 1
    if (-not $coinst) { throw "WdfCoInstaller01011.dll (x64) not found in the extracted WDK redistributable ($wdkDir)" }
    # ...\Windows Kits\8.0\redist\wdf\x64\WdfCoInstaller01011.dll  ->  ...\Windows Kits\8.0
    $kit = $coinst.Directory.Parent.Parent.Parent.FullName
    if (-not (Get-ChildItem (Join-Path $kit 'redist') -Recurse -Filter 'winusbcoinstaller2.dll' -ErrorAction SilentlyContinue)) {
        throw "winusbcoinstaller2.dll not found under $kit\redist"
    }
    $kitRel = ($kit.Substring($srcDir.Length + 1)) -replace '\\', '/'
    Write-Host "WDK redist: $kitRel"

    $script = @'
set -euo pipefail
cd "$(cygpath -u "$WDI_SRC")"
./bootstrap.sh
./configure --build=x86_64-w64-mingw32 --host=x86_64-w64-mingw32 \
    --disable-shared --disable-32bit --disable-debug --enable-examples-build \
    --with-wdkdir="$WDI_KIT" --with-wdfver=1011
make -j"$(nproc)"
ls -l examples/wdi-simple.exe
'@
    $sh = Join-Path $work 'build-wdi.sh'
    [IO.File]::WriteAllText($sh, ($script -replace "`r`n", "`n"), (New-Object Text.UTF8Encoding($false)))
    $env:MSYSTEM = 'UCRT64'
    $env:CHERE_INVOKING = '1'
    $env:WDI_SRC = $srcDir
    $env:WDI_KIT = $kitRel
    Run $bash @('-lc', "bash '$(($sh -replace '\\', '/'))'")

    $built = Join-Path $srcDir 'examples\wdi-simple.exe'
    if (-not (Test-Path $built)) { throw 'wdi-simple.exe was not built' }
    Copy-Item $built $wdiExe -Force
    Copy-Item (Join-Path $srcDir 'COPYING-LGPL') (Join-Path $wdiOut 'libwdi-COPYING-LGPL.txt') -Force
    Copy-Item (Join-Path $srcDir 'COPYING') (Join-Path $wdiOut 'libwdi-COPYING.txt') -Force
    @"
wdi-simple.exe in this folder is built from libwdi $wdiVersion (LGPL-3.0-or-later):
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
    "/DVERSION_NUM=$version.0",
    "/DDIST=$dist",
    "/DROOT=$root",
    "/DWDI_SIMPLE=$wdiExe",
    "/DWDI_DOCS=$wdiDocs",
    "/DLICENSE_FILE=$lic",
    "/DOUTFILE=$out",
    (Join-Path $src 'MarvinCapture.nsi')
)
Run $nsis $nsisArgs

if (-not (Test-Path $out)) { throw "installer not produced: $out" }
$hash = (Get-FileHash $out -Algorithm SHA256).Hash.ToLower()
$mb = [math]::Round((Get-Item $out).Length / 1MB, 1)
Write-Host "`nDone: $out ($mb MB)" -ForegroundColor Green
Write-Host "sha256: $hash"
