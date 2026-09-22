<#
.SYNOPSIS
  Capture and analyse USB transfers using Windows' built-in xHCI ETW provider.

.DESCRIPTION
  USBPcap 1.5.4 records nothing at all from the Pinnacle on this machine (see
  docs/capture-tooling.md). This uses Microsoft-Windows-USB-USBXHCI instead, which
  lives inside the host-controller driver and therefore sees every transfer.

  IMPORTANT: ETW gives transfer METADATA only - slot, endpoint, byte counts, timing.
  It does NOT give payload bytes, so it cannot reverse-engineer the wire protocol on
  its own. Use it to confirm which endpoint/alt-setting a capture uses and to measure
  throughput.

.EXAMPLE
  .\etw-usb.ps1 -Seconds 15
#>
[CmdletBinding()]
param(
    [int]    $Seconds = 15,
    [string] $OutDir  = (Join-Path $PSScriptRoot '..\traces')
)

$ErrorActionPreference = 'Continue'
$XHCI = '{30E1D284-5D88-459C-83FD-6345B39B19EC}'   # Microsoft-Windows-USB-USBXHCI

$isAdmin = ([Security.Principal.WindowsPrincipal] `
            [Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host 'Not elevated - relaunching as Administrator...' -ForegroundColor Yellow
    Start-Process -FilePath (Get-Process -Id $PID).Path -Verb RunAs -Wait -ArgumentList @(
        '-NoProfile','-ExecutionPolicy','Bypass','-File',$PSCommandPath,
        '-Seconds',$Seconds,'-OutDir',$OutDir)
    return
}

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
$etl = Join-Path $OutDir ("{0}-usbxhci.etl" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))

logman stop pinnacle_usb -ets *>$null
Write-Host "Tracing xHCI for $Seconds s..." -ForegroundColor Cyan
logman create trace pinnacle_usb -ets -p $XHCI 0xffffffffffffffff 0xff `
        -o $etl -nb 128 640 -bs 1024 -max 512 | Out-Null
Start-Sleep -Seconds $Seconds
logman stop pinnacle_usb -ets | Out-Null
Start-Sleep -Seconds 2

$real = Get-ChildItem ($etl -replace '\.etl$','*.etl') -EA SilentlyContinue |
        Sort-Object Length -Descending | Select-Object -First 1
if (-not $real) { Write-Error 'No ETL produced'; return }
Write-Host ("ETL: {0} ({1:N0} bytes)" -f $real.FullName, $real.Length) -ForegroundColor Green

# ---------------- analysis ----------------
function Get-EpName([int]$dci) {
    if ($dci -eq 1) { return 'EP0 control' }
    $n   = [int][math]::Floor($dci / 2)   # [int] matters: X2 cannot format a double
    $in  = ($dci % 2) -eq 1
    $hex = '{0:X2}' -f $(if ($in) { $n + 128 } else { $n })
    if ($in) { "EP$n IN  (0x$hex)" } else { "EP$n OUT (0x$hex)" }
}

$ev = Get-WinEvent -Path $real.FullName -Oldest -ErrorAction SilentlyContinue
Write-Host ("`ntotal events: {0}" -f $ev.Count)

Write-Host "`n--- controllers (Id 3) ---" -ForegroundColor Cyan
$ev | Where-Object Id -eq 3 | ForEach-Object {
    $d = ([xml]$_.ToXml()).Event.EventData.Data
    [pscustomobject]@{
        Handle = ($d | Where-Object Name -eq 'fid_UcxController').'#text'
        PciBus = ($d | Where-Object Name -eq 'fid_PciBus').'#text'
    }
} | Sort-Object Handle -Unique | Format-Table -AutoSize

Write-Host "--- devices (Id 4): slot is PER-CONTROLLER, mind the handle ---" -ForegroundColor Cyan
$ev | Where-Object Id -eq 4 | ForEach-Object {
    $d = ([xml]$_.ToXml()).Event.EventData.Data
    $ports = @($d | Where-Object Name -eq 'PortPath' | ForEach-Object { $_.'#text' } | Where-Object { $_ -ne '0' })
    [pscustomobject]@{
        Slot = [int]($d | Where-Object Name -eq 'fid_SlotId').'#text'
        Port = ($ports -join '.')
        Speed= ($d | Where-Object Name -eq 'DeviceSpeed').'#text'
        Cfg  = ($d | Where-Object Name -eq 'fid_ConfigurationValue').'#text'
        Alt  = ($d | Where-Object Name -eq 'fid_AlternateSetting').'#text'
        Ctl  = ($d | Where-Object Name -eq 'fid_UcxController').'#text'
    }
} | Sort-Object Ctl,Slot -Unique | Format-Table -AutoSize

Write-Host "--- transfers submitted (Id 41) ---" -ForegroundColor Cyan
$agg = @{}
foreach ($e in ($ev | Where-Object Id -eq 41)) {
    $p = $e.Properties
    $k = "$($p[0].Value)/$($p[1].Value)"
    if (-not $agg.ContainsKey($k)) {
        $agg[$k] = [pscustomobject]@{ Slot=$p[0].Value; DCI=$p[1].Value; N=0; Bytes=[int64]0 }
    }
    $agg[$k].N++
    $agg[$k].Bytes += [int64]$p[3].Value
}
$agg.Values | Sort-Object Bytes -Descending | Select-Object -First 12 | ForEach-Object {
    Write-Host ("  slot={0,-4} {1,-20} xfers={2,-7} bytes={3,14:N0}  {4,7:N2} MB/s" -f `
        $_.Slot, (Get-EpName $_.DCI), $_.N, $_.Bytes, ($_.Bytes / $Seconds / 1MB))
}
Write-Host "`n(Bytes = requested buffer size, not necessarily bytes filled.)" -ForegroundColor DarkGray
