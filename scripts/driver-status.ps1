<#
.SYNOPSIS
    Read-only status report for the Pinnacle 500-USB driver situation on Windows.

.DESCRIPTION
    Prints:
      - Any USB\VID_2304&PID_* device currently present, and what driver/service/INF
        is bound to it (vendor MarvinAVS64.sys, WinUSB, libusbK, or none).
      - The Pinnacle driver packages present in the driver store (pnputil /enum-drivers).
      - State of the root-enumerated "Pinnacle Marvin Bus 64" virtual device and its service.
      - Any filter drivers (UpperFilters/LowerFilters) on the relevant device or class keys.

    This script performs NO changes. It does not call pnputil /add-driver, /delete-driver,
    /disable-device, Disable-PnpDevice, or write to the registry. See docs/windows-driver.md
    for the (manual, Zadig-based) procedure to actually switch drivers.

.NOTES
    Safe to run at any time, with or without the device plugged in, with or without
    Administrator rights (some details, like a service's exact binary path, print less
    without elevation, but nothing here requires it).
#>

[CmdletBinding()]
param()

$ErrorActionPreference = 'Continue'

function Write-Section {
    param([string]$Title)
    Write-Host ""
    Write-Host "== $Title ==" -ForegroundColor Cyan
}

function Get-ServiceDetail {
    param([string]$ServiceName)
    $qc = & sc.exe qc $ServiceName 2>$null
    $q  = & sc.exe query $ServiceName 2>$null
    if (-not $qc -or ($qc -join '') -match 'FAILED') {
        return $null
    }
    $binary = ($qc | Select-String 'BINARY_PATH_NAME\s*:\s*(.+)').Matches |
        ForEach-Object { $_.Groups[1].Value.Trim() } | Select-Object -First 1
    $startType = ($qc | Select-String 'START_TYPE\s*:\s*\d+\s*(.+)').Matches |
        ForEach-Object { $_.Groups[1].Value.Trim() } | Select-Object -First 1
    $state = ($q | Select-String 'STATE\s*:\s*\d+\s*(.+)').Matches |
        ForEach-Object { $_.Groups[1].Value.Trim() } | Select-Object -First 1
    [PSCustomObject]@{
        Service   = $ServiceName
        Binary    = $binary
        StartType = $startType
        State     = $state
    }
}

Write-Host "Pinnacle 500-USB / Marvin family — Windows driver status (read-only)" -ForegroundColor Yellow
Write-Host "Generated $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"

# ---------------------------------------------------------------------------
Write-Section "1. USB\VID_2304&PID_* devices currently present"

$pnpDevices = Get-PnPDevice -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -match '^USB\\VID_2304' }

if (-not $pnpDevices) {
    Write-Host "No USB\VID_2304&PID_* device is currently enumerated (device unplugged, or never plugged into this Windows install)." -ForegroundColor DarkYellow
} else {
    foreach ($dev in $pnpDevices) {
        Write-Host ""
        Write-Host "Instance ID : $($dev.InstanceId)"
        Write-Host "Description : $($dev.FriendlyName) / $($dev.Description)"
        Write-Host "Class       : $($dev.Class)"
        Write-Host "Status      : $($dev.Status)"

        $driverProp = Get-PnPDeviceProperty -InstanceId $dev.InstanceId -KeyName 'DEVPKEY_Device_DriverInfSection' -ErrorAction SilentlyContinue
        $infProp    = Get-PnPDeviceProperty -InstanceId $dev.InstanceId -KeyName 'DEVPKEY_Device_DriverInfPath' -ErrorAction SilentlyContinue
        $svcProp    = Get-PnPDeviceProperty -InstanceId $dev.InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue
        $classGuidProp = Get-PnPDeviceProperty -InstanceId $dev.InstanceId -KeyName 'DEVPKEY_Device_ClassGuid' -ErrorAction SilentlyContinue

        if ($infProp.Data)    { Write-Host "Bound INF   : $($infProp.Data)" }
        if ($driverProp.Data) { Write-Host "INF section : $($driverProp.Data)" }
        if ($svcProp.Data) {
            Write-Host "Service     : $($svcProp.Data)"
            switch -Regex ($svcProp.Data) {
                'WinUSB'          { Write-Host "  -> Bound to WinUSB. libusb should be able to open this device." -ForegroundColor Green }
                'libusbK|libusb0' { Write-Host "  -> Bound to libusbK/libusb0. libusb should be able to open this device." -ForegroundColor Green }
                'PinnacleMarvinAVS' { Write-Host "  -> Bound to the VENDOR driver (MarvinAVS64.sys). libusb_open() will fail with LIBUSB_ERROR_NOT_SUPPORTED / access denied until this is switched to WinUSB (see docs/windows-driver.md)." -ForegroundColor DarkYellow }
                default           { Write-Host "  -> Unrecognised service; check manually." -ForegroundColor DarkYellow }
            }
        } else {
            Write-Host "Service     : (none — device has no driver bound, e.g. shows as 'Unknown device' or with a yellow bang)" -ForegroundColor DarkYellow
        }

        # Hardware key filters (per-device UpperFilters/LowerFilters), read-only.
        $enumPath = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)"
        if (Test-Path $enumPath) {
            $upper = (Get-ItemProperty -Path $enumPath -Name 'UpperFilters' -ErrorAction SilentlyContinue).UpperFilters
            $lower = (Get-ItemProperty -Path $enumPath -Name 'LowerFilters' -ErrorAction SilentlyContinue).LowerFilters
            if ($upper) { Write-Host "UpperFilters (this device) : $($upper -join ', ')" -ForegroundColor DarkYellow }
            if ($lower) { Write-Host "LowerFilters (this device) : $($lower -join ', ')" -ForegroundColor DarkYellow }
            if (-not $upper -and -not $lower) { Write-Host "Filters     : none on this device's hardware key" }
        }
    }
}

# ---------------------------------------------------------------------------
Write-Section "2. Pinnacle / Marvin driver packages in the driver store (pnputil /enum-drivers)"

$enumDrivers = & pnputil.exe /enum-drivers 2>$null
if ($enumDrivers) {
    $blocks = ($enumDrivers -join "`n") -split "(?=Published Name\s*:)"
    foreach ($block in $blocks) {
        if ($block -match 'Original Name\s*:\s*(marvin\w*\.inf|pclebend64\.inf|dvcaudio64\.inf|dvcvideo64\.inf)' ) {
            Write-Host ""
            ($block.Trim() -split "`n") | Where-Object { $_.Trim() } | ForEach-Object { Write-Host $_ }
        }
    }
} else {
    Write-Host "pnputil /enum-drivers returned nothing (unexpected)." -ForegroundColor Red
}

# ---------------------------------------------------------------------------
Write-Section "3. Pinnacle services (kernel drivers)"

foreach ($svcName in @('PinnacleMarvinAVS', 'MarvinBus')) {
    $detail = Get-ServiceDetail -ServiceName $svcName
    if ($detail) {
        Write-Host ""
        Write-Host "Service     : $($detail.Service)"
        Write-Host "Binary      : $($detail.Binary)"
        Write-Host "Start type  : $($detail.StartType)"
        Write-Host "State       : $($detail.State)"
    } else {
        Write-Host ""
        Write-Host "Service '$svcName' not found (package not installed, or already removed)." -ForegroundColor DarkYellow
    }
}

# ---------------------------------------------------------------------------
Write-Section "4. Root-enumerated 'Pinnacle Marvin Bus 64' virtual device"

$marvinBus = Get-PnPDevice -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -match '^ROOT\\SYSTEM\\' -and $_.FriendlyName -match 'Marvin' }

if ($marvinBus) {
    foreach ($dev in $marvinBus) {
        Write-Host "Instance ID : $($dev.InstanceId)"
        Write-Host "Description : $($dev.FriendlyName)"
        Write-Host "Status      : $($dev.Status)"
    }
    Write-Host "This is a SOFTWARE device (root\MarvinBus), not the physical USB device. It does not need to be touched to use WinUSB on the USB device — see docs/windows-driver.md section 1." -ForegroundColor DarkGray
} else {
    Write-Host "Not present (package removed, or has never started)." -ForegroundColor DarkYellow
}

# ---------------------------------------------------------------------------
Write-Section "5. Global class filters that could affect this device"

$classKeys = @{
    'USB (Universal Serial Bus controllers)' = '{36FC9E60-C465-11CF-8056-444553540000}'
    'Media (Sound, video and game controllers)' = '{4D36E96C-E325-11CE-BFC1-08002BE10318}'
    'System devices' = '{4D36E97D-E325-11CE-BFC1-08002BE10318}'
}
foreach ($name in $classKeys.Keys) {
    $guid = $classKeys[$name]
    $path = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$guid"
    $upper = (Get-ItemProperty -Path $path -Name 'UpperFilters' -ErrorAction SilentlyContinue).UpperFilters
    $lower = (Get-ItemProperty -Path $path -Name 'LowerFilters' -ErrorAction SilentlyContinue).LowerFilters
    Write-Host ""
    Write-Host "Class: $name"
    Write-Host "  UpperFilters: $(if ($upper) { $upper -join ', ' } else { '(none)' })"
    Write-Host "  LowerFilters: $(if ($lower) { $lower -join ', ' } else { '(none)' })"
}

# ---------------------------------------------------------------------------
Write-Section "6. Vendor install package / uninstall entry"

$uninstallHits = @()
foreach ($root in @(
    'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
    'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*'
)) {
    $uninstallHits += Get-ItemProperty -Path $root -ErrorAction SilentlyContinue |
        Where-Object { $_.DisplayName -match 'Pinnacle|Dazzle|Marvin' }
}
if ($uninstallHits) {
    $uninstallHits | Select-Object DisplayName, DisplayVersion, Publisher, UninstallString -Unique |
        Format-List | Out-String | Write-Host
} else {
    Write-Host "No matching entry found under Add/Remove Programs registry keys." -ForegroundColor DarkYellow
}

# ---------------------------------------------------------------------------
Write-Section "7. Any vendor user-mode process currently running"

$procs = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match 'Pinnacle|Marvin|Dazzle' -and $_.Name -notmatch 'MarvinCaptureGUI' }
if ($procs) {
    $procs | Select-Object ProcessId, Name, ExecutablePath | Format-Table -AutoSize | Out-String | Write-Host
    Write-Host "NOTE: a running vendor process could hold the device open and compete with libusb." -ForegroundColor DarkYellow
} else {
    Write-Host "None found. (MarvinCaptureGUI.exe, if listed elsewhere, is this repo's own GUI, not vendor software.)"
}

Write-Host ""
Write-Host "Done. This script made no changes. See docs/windows-driver.md for the Zadig procedure." -ForegroundColor Yellow
