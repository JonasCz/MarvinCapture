<#
.SYNOPSIS
  Installs the WinUSB driver for one MarvinCapture device (called by the installer).

.DESCRIPTION
  Runs libwdi's wdi-simple.exe for USB\VID_<VendorId>&PID_<ProductId>:
  it generates an INF (WinUSB, bound to that one hardware ID), builds a catalog,
  signs it with a throw-away self-signed certificate (added to LocalMachine Root and
  TrustedPublisher, private key destroyed), and installs the package.

    - Device plugged in:   the driver is installed on it right now (replacing the
                           vendor driver MarvinAVS64 if that is bound).
    - Device not plugged:  the package is only copied into the driver store
                           (SetupCopyOEMInf); Windows binds it on the next plug-in.

  For the second case, "ghost" device entries (device seen before, not present now,
  still remembering the vendor driver) are removed afterwards so that the next
  plug-in does a fresh driver ranking, which the new package wins (newer date).

  Everything is logged to -Log. Exit code 0 = driver package installed.

.PARAMETER WdiSimple  Full path of wdi-simple.exe.
.PARAMETER ProductId  USB product ID, 4 hex digits (0213, 0223, ...).
.PARAMETER Name       Device name written into the INF (shown in Device Manager).
.PARAMETER VendorId   USB vendor ID, 4 hex digits (default 2304).
.PARAMETER Log        Log file (appended).
.PARAMETER TimeoutMs  How long wdi-simple waits for other pending driver installs.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$WdiSimple,
    [Parameter(Mandatory = $true)] [ValidatePattern('^[0-9A-Fa-f]{4}$')] [string]$ProductId,
    [Parameter(Mandatory = $true)] [string]$Name,
    [ValidatePattern('^[0-9A-Fa-f]{4}$')] [string]$VendorId = '2304',
    [string]$Log = (Join-Path $env:TEMP 'MarvinCapture-driver-install.log'),
    [int]$TimeoutMs = 90000
)

$ErrorActionPreference = 'Stop'

function Write-Log([string]$msg) {
    $line = '{0:yyyy-MM-dd HH:mm:ss} [{1}] {2}' -f (Get-Date), $ProductId, $msg
    try { Add-Content -LiteralPath $Log -Value $line -Encoding UTF8 } catch { }
    Write-Host $msg
}

# pnputil from the native System32 even if this is a 32-bit PowerShell on 64-bit Windows.
function Get-NativeTool([string]$exe) {
    if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
        return (Join-Path $env:SystemRoot "sysnative\$exe")
    }
    return (Join-Path $env:SystemRoot "System32\$exe")
}

$exit = 1
$work = Join-Path $env:TEMP ('MarvinCapture-wdi-{0}-{1}' -f $ProductId, [Guid]::NewGuid().ToString('N').Substring(0, 8))

try {
    Write-Log "Installing WinUSB driver for '$Name' (USB\VID_$VendorId&PID_$ProductId)"
    if (-not (Test-Path -LiteralPath $WdiSimple)) { throw "wdi-simple.exe not found: $WdiSimple" }

    $drv = Join-Path $work 'driver'
    New-Item -ItemType Directory -Force $drv | Out-Null
    $so = Join-Path $work 'stdout.txt'
    $se = Join-Path $work 'stderr.txt'

    # -t 0 WinUSB, -n name, -m manufacturer, -v/-p ids, -f inf name, -d extraction dir,
    # -o wait for pending installs, -l 1 info-level log. No -s: the output goes to our log.
    $argLine = '-n "{0}" -m "Pinnacle Systems" -v 0x{1} -p 0x{2} -t 0 -f "marvincapture-{2}.inf" -d "{3}" -o {4} -l 1' -f `
        $Name, $VendorId, $ProductId, $drv, $TimeoutMs
    Write-Log "wdi-simple $argLine"

    $p = Start-Process -FilePath $WdiSimple -ArgumentList $argLine -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $so -RedirectStandardError $se
    $null = $p.Handle   # keep the handle so ExitCode is readable after exit
    if (-not $p.WaitForExit(300000)) {
        try { $p.Kill() } catch { }
        throw 'wdi-simple did not finish within 5 minutes'
    }
    $code = $p.ExitCode
    foreach ($f in @($so, $se)) {
        if (Test-Path -LiteralPath $f) {
            foreach ($l in (Get-Content -LiteralPath $f -ErrorAction SilentlyContinue)) {
                if ($l -and $l.Trim()) { Write-Log ('  | ' + $l.Trim()) }
            }
        }
    }
    Write-Log "wdi-simple exit code: $code"
    if ($code -ne 0) { throw "wdi-simple failed (exit code $code)" }

    # Remove "ghost" entries of this device (known to Windows, not currently present), so the
    # next plug-in is ranked afresh and picks the new package instead of the old binding.
    try {
        $pnputil = Get-NativeTool 'pnputil.exe'
        $pattern = "USB\VID_$VendorId&PID_$ProductId*"
        $ghosts = @(Get-PnpDevice -ErrorAction Stop | Where-Object { $_.InstanceId -like $pattern -and $_.Status -eq 'Unknown' })
        foreach ($g in $ghosts) {
            $ErrorActionPreference = 'Continue'
            $o = & $pnputil /remove-device $g.InstanceId 2>&1 | Out-String
            $rc = $LASTEXITCODE
            $ErrorActionPreference = 'Stop'
            Write-Log "removed stale device entry $($g.InstanceId) (pnputil exit $rc)"
        }
    } catch {
        $ErrorActionPreference = 'Stop'
        Write-Log "note: could not clean up stale device entries: $($_.Exception.Message)"
    }

    Write-Log 'OK'
    $exit = 0
} catch {
    Write-Log "FAILED: $($_.Exception.Message)"
    $exit = 1
} finally {
    try { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue } catch { }
}
exit $exit
