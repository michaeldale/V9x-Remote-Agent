# Report, enable or disable Windows 9x autologon on a guest, and clear a logon
# dialog that is already up.
#
# Why this exists: the agent itself runs fine before anyone logs on (it is
# launched from RunServices, and ping/info/exec/put/get/input all work at the
# logon dialog), but everything that needs the shell does not - screenshot
# returns exit 43 and wait-desktop returns exit 44 for as long as the dialog
# sits there. An unattended machine that prompts therefore stalls every
# automation run at the first wait-desktop. See
# docs/decisions/2026-09-04-prelogon-reachability-and-autologon.md.
#
# Autologon on Windows 9x is two conditions, not a single switch:
#   1. HKLM\Network\Logon\PrimaryProvider is empty ("Windows Logon"). Any
#      network provider here (Client for Microsoft Networks) always prompts.
#   2. A password-list file C:\WINDOWS\<user>.PWL exists and holds a blank
#      password. Windows creates it when a logon completes; it cannot be
#      fabricated from outside, which is why -Enable may need one -Dismiss.
[CmdletBinding(DefaultParameterSetName = 'Status')]
param(
    [Parameter(ParameterSetName = 'Enable', Mandatory = $true)]
    [switch]$Enable,
    [Parameter(ParameterSetName = 'Disable', Mandatory = $true)]
    [switch]$Disable,
    [Parameter(ParameterSetName = 'Dismiss', Mandatory = $true)]
    [switch]$Dismiss,
    [Alias('Host')]
    [string]$EndpointHost = '127.0.0.1',
    [ValidateRange(1, 65535)]
    [int]$Port = 9869,
    [string]$UserName,
    [ValidateRange(5, 600)]
    [int]$DesktopTimeoutSeconds = 120,
    [switch]$Json
)

$ErrorActionPreference = 'Stop'
$ctl = Join-Path $PSScriptRoot 'v9xctl.ps1'
$powershell = Join-Path $PSHOME 'powershell.exe'
$guestJobDirectory = 'C:\V9XREMOTE\JOBS\AUTOLOGN'
$logonKey = 'HKEY_LOCAL_MACHINE\Network\Logon'

function Invoke-V9xCtlJson {
    param([string]$Operation, [string[]]$OperationArguments = @(), [int[]]$AllowExit = @(0))
    $arguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $ctl, $Operation,
        '-Host', $EndpointHost, '-Port', [string]$Port, '-Json'
    ) + $OperationArguments
    $lines = @(& $powershell @arguments 2>&1)
    $nativeExit = $LASTEXITCODE
    if ($AllowExit -notcontains $nativeExit) {
        throw "v9xctl $Operation failed with exit code ${nativeExit}: $($lines -join [Environment]::NewLine)"
    }
    $jsonLine = $lines | Where-Object { $_ -is [string] -and $_.TrimStart().StartsWith('{') } |
        Select-Object -Last 1
    $parsed = if ($jsonLine) { $jsonLine | ConvertFrom-Json } else { $null }
    return [pscustomobject]@{ Exit = $nativeExit; Result = $parsed }
}

# REGEDIT /S rejects an LF-only .REG file on Windows 9x, so every generated
# file is written CRLF regardless of the host's line-ending habits.
function Write-GuestRegFile {
    param([string]$LocalPath, [string]$ValueName, [string]$ValueData)
    $text = "REGEDIT4`r`n`r`n[$logonKey]`r`n`"$ValueName`"=`"$ValueData`"`r`n`r`n"
    [IO.File]::WriteAllText($LocalPath, $text, [Text.Encoding]::ASCII)
}

function Get-GuestLogonState {
    $local = Join-Path ([IO.Path]::GetTempPath()) ("v9xlogon-" + [Guid]::NewGuid().ToString('N') + '.reg')
    $guest = Join-Path $guestJobDirectory 'LOGON.REG'
    try {
        Invoke-V9xCtlJson -Operation 'mkdir' -OperationArguments @('-Path', $guestJobDirectory) | Out-Null
        Invoke-V9xCtlJson -Operation 'exec' -OperationArguments @(
            '-Application', 'C:\WINDOWS\REGEDIT.EXE',
            '-Arguments', "/E $guest $logonKey",
            '-TimeoutSeconds', '60') | Out-Null
        Invoke-V9xCtlJson -Operation 'get' -OperationArguments @(
            '-Source', $guest, '-Destination', $local) | Out-Null
        $text = [IO.File]::ReadAllText($local)
    } finally {
        if (Test-Path -LiteralPath $local) { Remove-Item -LiteralPath $local -Force }
    }

    $provider = if ($text -match '(?im)^"PrimaryProvider"="(.*)"\s*$') { $Matches[1] } else { $null }
    $user = if ($text -match '(?im)^"username"="(.*)"\s*$') { $Matches[1] } else { '' }
    if ($UserName) { $user = $UserName }

    # Windows 9x truncates the password-list file name to 8.3.
    $pwlName = if ($user.Length -gt 8) { $user.Substring(0, 8) } else { $user }
    $pwlPath = if ($pwlName) { "C:\WINDOWS\$pwlName.PWL" } else { $null }
    $pwlPresent = $false
    if ($pwlPath) {
        $stat = Invoke-V9xCtlJson -Operation 'stat' -OperationArguments @('-Path', $pwlPath)
        $pwlPresent = [bool]$stat.Result.Exists
    }
    $info = Invoke-V9xCtlJson -Operation 'info'

    [pscustomobject]@{
        UserName        = $user
        PrimaryProvider = $provider
        PwlPath         = $pwlPath
        PwlPresent      = $pwlPresent
        DesktopReady    = [bool]$info.Result.DesktopReady
        BootCounter     = $info.Result.BootCounter
        Autologon       = ([string]::IsNullOrEmpty($provider) -and $pwlPresent)
    }
}

function Clear-LogonDialog {
    param([switch]$RequireDialog)
    $info = Invoke-V9xCtlJson -Operation 'info'
    if ($info.Result.DesktopReady) {
        if ($RequireDialog) { Write-Verbose 'Desktop already ready; no logon dialog to clear.' }
        return $false
    }
    # The dialog owns the focus at this point in boot and OK is its default
    # button, so a bare ENTER completes it with the blank password Windows then
    # caches in the PWL.
    Invoke-V9xCtlJson -Operation 'input' -OperationArguments @('-Sequence', 'key ENTER') | Out-Null
    $waited = Invoke-V9xCtlJson -Operation 'wait-desktop' -OperationArguments @(
        '-WaitSeconds', [string]$DesktopTimeoutSeconds) -AllowExit @(0, 44)
    if ($waited.Exit -ne 0) {
        throw "Sent ENTER to the logon dialog but the desktop did not become ready within $DesktopTimeoutSeconds seconds."
    }
    return $true
}

$before = Get-GuestLogonState
$action = $PSCmdlet.ParameterSetName
$dialogCleared = $false
$notes = @()

switch ($action) {
    'Enable' {
        if (-not [string]::IsNullOrEmpty($before.PrimaryProvider)) {
            $local = Join-Path ([IO.Path]::GetTempPath()) ("v9xlogon-" + [Guid]::NewGuid().ToString('N') + '.reg')
            $guest = Join-Path $guestJobDirectory 'SETPROV.REG'
            try {
                Write-GuestRegFile -LocalPath $local -ValueName 'PrimaryProvider' -ValueData ''
                Invoke-V9xCtlJson -Operation 'put' -OperationArguments @(
                    '-Source', $local, '-Destination', $guest) | Out-Null
            } finally {
                if (Test-Path -LiteralPath $local) { Remove-Item -LiteralPath $local -Force }
            }
            Invoke-V9xCtlJson -Operation 'exec' -OperationArguments @(
                '-Application', 'C:\WINDOWS\REGEDIT.EXE',
                '-Arguments', "/S $guest",
                '-TimeoutSeconds', '60') | Out-Null
            $notes += "PrimaryProvider was '$($before.PrimaryProvider)'; set to Windows Logon (takes effect next boot)."
        }
        # A PWL cannot be created from outside Windows: it only appears when a
        # logon completes. If the dialog is up right now, completing it here is
        # both the fix and the way the file gets written.
        if (-not $before.PwlPresent) {
            $dialogCleared = Clear-LogonDialog
            if ($dialogCleared) {
                $notes += 'Completed the pending logon dialog with a blank password, which created the PWL.'
            } else {
                $notes += "No PWL and no dialog on screen: reboot, then run -Dismiss once (or -Enable again) to create it."
            }
        }
        # Restore any PWL this script had previously moved aside.
        $parked = Invoke-V9xCtlJson -Operation 'stat' -OperationArguments @('-Path', 'C:\WINDOWS\V9XLOGON.PWL')
        if ($parked.Result.Exists -and -not $before.PwlPresent -and -not $dialogCleared) {
            Invoke-V9xCtlJson -Operation 'shell' -OperationArguments @(
                '-Command', "COPY /Y C:\WINDOWS\V9XLOGON.PWL $($before.PwlPath)",
                '-TimeoutSeconds', '60') | Out-Null
            $notes += 'Restored the password list parked by an earlier -Disable.'
        }
    }
    'Disable' {
        if (-not $before.PwlPresent) {
            $notes += 'No password list present, so the guest already prompts at boot.'
        } else {
            # Parked rather than deleted: -Enable can put it back, and the
            # cached password (blank or not) survives.
            Invoke-V9xCtlJson -Operation 'shell' -OperationArguments @(
                '-Command', "COPY /Y $($before.PwlPath) C:\WINDOWS\V9XLOGON.PWL",
                '-TimeoutSeconds', '60') | Out-Null
            Invoke-V9xCtlJson -Operation 'shell' -OperationArguments @(
                '-Command', "DEL $($before.PwlPath)",
                '-TimeoutSeconds', '60') | Out-Null
            $notes += "Parked $($before.PwlPath) as C:\WINDOWS\V9XLOGON.PWL; the guest prompts from the next boot."
        }
    }
    'Dismiss' {
        $dialogCleared = Clear-LogonDialog -RequireDialog
        $notes += if ($dialogCleared) { 'Cleared the logon dialog; the desktop is ready.' }
                  else { 'Desktop was already ready; nothing to dismiss.' }
    }
    default {
        $notes += if ($before.Autologon) { 'Autologon is on: Windows Logon with a cached password list.' }
                  elseif (-not [string]::IsNullOrEmpty($before.PrimaryProvider)) { "A network provider ('$($before.PrimaryProvider)') is the primary logon, so the guest always prompts." }
                  else { 'Windows Logon is primary but no password list is cached, so the guest prompts at boot.' }
    }
}

$after = if ($action -eq 'Status') { $before } else { Get-GuestLogonState }
$result = [pscustomobject]@{
    Success         = $true
    Action          = $action
    Endpoint        = "${EndpointHost}:$Port"
    UserName        = $after.UserName
    PrimaryProvider = $after.PrimaryProvider
    PwlPath         = $after.PwlPath
    PwlPresent      = $after.PwlPresent
    DesktopReady    = $after.DesktopReady
    BootCounter     = $after.BootCounter
    Autologon       = $after.Autologon
    DialogCleared   = $dialogCleared
    Notes           = $notes
}
if ($Json) { $result | ConvertTo-Json -Depth 4 -Compress } else { $result | Format-List }
exit 0
