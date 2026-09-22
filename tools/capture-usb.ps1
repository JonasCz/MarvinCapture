<#
.SYNOPSIS
  Capture USB traffic for the Pinnacle 500-USB with USBPcap, then summarise it.

.DESCRIPTION
  Avoids the common USBPcap pitfalls:
    * self-elevates (USBPcap's control device requires Administrator)
    * captures from ALL devices on the root hub (-A) plus --inject-descriptors,
      so a changed USB device address never makes you miss the device again
    * uses the maximum 128 MB internal buffer, because DV runs ~3.6 MB/s and the
      default buffer overflows and silently drops packets
    * refuses to start if another process already holds the USBPcap filter device
      (only ONE capture per \\.\USBPcapN is allowed - this is what blocks Wireshark)

.EXAMPLE
  .\capture-usb.ps1 -Seconds 30 -Label playing
  # Press Play on the camera and start the transfer in your capture app FIRST,
  # then run this while data is actually flowing.

.EXAMPLE
  .\capture-usb.ps1 -Seconds 20 -Label plugin
  # Start this, THEN plug the device in, to catch enumeration + any firmware load.
#>
[CmdletBinding()]
param(
    [int]    $Seconds    = 30,
    [string] $Label      = 'trace',
    [string] $FilterDev  = '\\.\USBPcap1',
    [string] $OutDir     = (Join-Path $PSScriptRoot '..\traces')
)

$ErrorActionPreference = 'Stop'

# ---- self-elevate -----------------------------------------------------------
$isAdmin = ([Security.Principal.WindowsPrincipal] `
            [Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    Write-Host 'Not elevated - relaunching as Administrator (approve the UAC prompt)...' -ForegroundColor Yellow
    $psExe = (Get-Process -Id $PID).Path
    $argList = @('-NoProfile','-ExecutionPolicy','Bypass','-File',$PSCommandPath,
                 '-Seconds',$Seconds,'-Label',$Label,'-FilterDev',$FilterDev,'-OutDir',$OutDir)
    Start-Process -FilePath $psExe -Verb RunAs -ArgumentList $argList -Wait
    return
}

# ---- locate tools -----------------------------------------------------------
$usbpcap = 'C:\Program Files\USBPcap\USBPcapCMD.exe'
$tshark  = 'C:\Program Files\Wireshark\tshark.exe'
if (-not (Test-Path $usbpcap)) { throw "USBPcapCMD not found at $usbpcap" }

# ---- refuse to collide with an existing capture ------------------------------
$existing = Get-Process -Name USBPcapCMD -ErrorAction SilentlyContinue |
            Where-Object { $_.Id -ne $PID }
if ($existing) {
    Write-Warning ("Another USBPcapCMD is running (PID {0}). Only one capture per " +
                   "filter device is allowed - close it first (Ctrl+C in its window), " +
                   "or it will block this capture AND Wireshark." -f ($existing.Id -join ', '))
    return
}

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out   = Join-Path $OutDir "$stamp-$Label.pcap"

$capArgs = @(
    '-d', $FilterDev,
    '-A',                       # every device on this root hub
    '--inject-descriptors',     # so we can map address -> VID:PID afterwards
    '-b', '134217728',          # 128 MB buffer: DV needs it
    '-s', '65535',              # full snaplen: do not truncate DV payload
    '-o', $out
)

Write-Host "Capturing $Seconds s from $FilterDev -> $out" -ForegroundColor Cyan
$p = Start-Process -FilePath $usbpcap -ArgumentList $capArgs -PassThru -WindowStyle Hidden

for ($i = $Seconds; $i -gt 0; $i--) {
    Write-Host ("`r  {0,3}s remaining   file: {1,10:N0} bytes" -f `
        $i, $(if (Test-Path $out) { (Get-Item $out).Length } else { 0 })) -NoNewline
    Start-Sleep -Seconds 1
}
Write-Host ''

if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Seconds 2

if (-not (Test-Path $out)) { throw 'No capture file was produced.' }
$len = (Get-Item $out).Length
Write-Host ("Wrote {0:N0} bytes to {1}" -f $len, $out) -ForegroundColor Green

# ---- summarise --------------------------------------------------------------
if (Test-Path $tshark) {
    Write-Host "`n=== address -> VID:PID ===" -ForegroundColor Cyan
    $map = @{}
    & $tshark -r $out -Y 'usb.idVendor' -T fields `
              -e usb.device_address -e usb.idVendor -e usb.idProduct 2>$null |
        Sort-Object -Unique | ForEach-Object {
            $f = $_ -split "`t"
            if ($f.Count -ge 3) {
                $map[$f[0]] = "$($f[1]):$($f[2])"
                $tag = if ($f[1] -eq '0x2304') { '   <-- PINNACLE' } else { '' }
                "  addr {0,-4} {1}:{2}{3}" -f $f[0], $f[1], $f[2], $tag
            }
        }

    $pin = ($map.GetEnumerator() | Where-Object { $_.Value -like '0x2304:*' } |
            Select-Object -First 1).Key

    if ($pin) {
        Write-Host "`n=== Pinnacle (addr $pin) traffic by endpoint ===" -ForegroundColor Cyan
        $rows = & $tshark -r $out -Y "usb.device_address==$pin" -T fields `
                    -e usb.endpoint_address -e usb.transfer_type -e usb.data_len `
                    -e frame.time_relative 2>$null
        if (-not $rows) {
            Write-Warning 'No packets at all from the Pinnacle.'
        } else {
            $real = $rows | Where-Object { ($_ -split "`t")[3] -as [double] -gt 0.0001 }
            $rows | Group-Object { $p = $_ -split "`t"; "EP $($p[0]) type=$($p[1])" } |
                ForEach-Object {
                    $bytes = ($_.Group | ForEach-Object { [int](($_ -split "`t")[2]) } |
                              Measure-Object -Sum).Sum
                    "  {0,-22} packets={1,-8} bytes={2:N0}" -f $_.Name, $_.Count, $bytes
                }
            $realCount = @($real).Count
            Write-Host ("`n  real (non-injected) packets: {0}" -f $realCount)
            if ($realCount -eq 0) {
                Write-Warning ('Only injected descriptors were seen - the device was IDLE. ' +
                               'Make sure the camera is PLAYING and a transfer is actually ' +
                               'running before/while you capture.')
            }
        }
    } else {
        Write-Warning 'Pinnacle (VID 0x2304) not seen on this root hub - try -FilterDev \\.\USBPcap2 or 3.'
    }
}
