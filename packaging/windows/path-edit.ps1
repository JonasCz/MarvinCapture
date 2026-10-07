<#
.SYNOPSIS
  Adds or removes one directory in the system PATH (HKLM), safely (called by the installer).

.DESCRIPTION
  Works on the registry value directly, in the 64-bit view, so that
    - %SystemRoot%-style references in PATH are NOT expanded (the value is kept REG_EXPAND_SZ),
    - there is no 1024-character truncation (NSIS's string buffer is not involved),
    - the old value is saved to -Backup before every change,
    - nothing is written if the PATH would not change.
  Then broadcasts WM_SETTINGCHANGE("Environment") so new processes (Explorer, new
  terminals) pick the change up.

.PARAMETER Action  Add or Remove.
.PARAMETER Dir     The directory (no trailing backslash).
.PARAMETER Backup  File that receives the previous PATH value.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [ValidateSet('Add', 'Remove')] [string]$Action,
    [Parameter(Mandatory = $true)] [string]$Dir,
    [string]$Backup = (Join-Path $env:TEMP 'MarvinCapture-path-backup.txt')
)

$ErrorActionPreference = 'Stop'

function Normalize([string]$s) {
    return $s.Trim().Trim('"').TrimEnd('\').ToLowerInvariant()
}

try {
    $target = $Dir.Trim().TrimEnd('\')
    $hklm = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,
                                                       [Microsoft.Win32.RegistryView]::Registry64)
    $key = $hklm.OpenSubKey('SYSTEM\CurrentControlSet\Control\Session Manager\Environment', $true)
    if ($null -eq $key) { throw 'cannot open the system Environment registry key' }

    $old = [string]$key.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    $parts = @($old -split ';' | Where-Object { $_.Trim() -ne '' })
    $want = Normalize $target
    $has = $false
    foreach ($p in $parts) { if ((Normalize $p) -eq $want) { $has = $true; break } }

    $new = $null
    if ($Action -eq 'Add') {
        if (-not $has) { $new = (@($parts) + $target) -join ';' }
    } else {
        if ($has) { $new = (@($parts | Where-Object { (Normalize $_) -ne $want })) -join ';' }
    }

    if ($null -eq $new) {
        Write-Host "PATH unchanged ($Action $target)"
    } else {
        try { Set-Content -LiteralPath $Backup -Value $old -Encoding UTF8 } catch { }
        $key.SetValue('Path', $new, [Microsoft.Win32.RegistryValueKind]::ExpandString)
        Write-Host "PATH updated ($Action $target); previous value saved to $Backup"

        Add-Type -Namespace MarvinCapture -Name Native -MemberDefinition @'
[DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint Msg, UIntPtr wParam, string lParam,
    uint fuFlags, uint uTimeout, out UIntPtr lpdwResult);
'@
        $res = [UIntPtr]::Zero
        # HWND_BROADCAST, WM_SETTINGCHANGE, SMTO_ABORTIFHUNG, 5 s
        $null = [MarvinCapture.Native]::SendMessageTimeout([IntPtr]0xffff, 0x1A, [UIntPtr]::Zero,
                                                           'Environment', 2, 5000, [ref]$res)
    }
    $key.Close()
    exit 0
} catch {
    Write-Host "path-edit failed: $($_.Exception.Message)"
    exit 1
}
