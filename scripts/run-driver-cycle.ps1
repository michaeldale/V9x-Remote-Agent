[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackagePath,
    [Alias('BuildId')]
    [string]$JobId,
    [Alias('Host')]
    [string]$EndpointHost = '127.0.0.1',
    [ValidateRange(1, 65535)]
    [int]$Port = 9869,
    [string]$PreflightProgram = 'V9X16LD.EXE',
    [string]$PreflightArguments = '/quiet',
    [string]$RepairInf = 'V9XFIX.INF',
    [string]$ResultsDirectory,
    [ValidateRange(30, 600)]
    [int]$BootTimeoutSeconds = 180,
    [switch]$Apply,
    [switch]$ConfirmAlreadyAssociated,
    [switch]$SkipScreenshot,
    [switch]$Json
)

$ErrorActionPreference = 'Stop'
$ctl = Join-Path $PSScriptRoot 'v9xctl.ps1'
$powershell = Join-Path $PSHOME 'powershell.exe'

function Invoke-V9xCtlJson {
    param([string]$Operation, [string[]]$OperationArguments = @())
    $arguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $ctl, $Operation,
        '-Host', $EndpointHost, '-Port', [string]$Port, '-Json'
    ) + $OperationArguments
    $lines = @(& $powershell @arguments 2>&1)
    $nativeExit = $LASTEXITCODE
    if ($nativeExit -ne 0) {
        throw "v9xctl $Operation failed with exit code ${nativeExit}: $($lines -join [Environment]::NewLine)"
    }
    $jsonLine = $lines | Where-Object { $_ -is [string] -and $_.TrimStart().StartsWith('{') } |
        Select-Object -Last 1
    if (-not $jsonLine) { throw "v9xctl $Operation returned no JSON object." }
    return $jsonLine | ConvertFrom-Json
}

if (-not (Test-Path -LiteralPath $PackagePath -PathType Container)) {
    throw "Driver package directory not found: $PackagePath"
}
$package = (Get-Item -LiteralPath $PackagePath).FullName.TrimEnd('\')
$preflight = Join-Path $package $PreflightProgram
if (-not (Test-Path -LiteralPath $preflight -PathType Leaf)) {
    throw "Required unattended preflight program not found: $preflight"
}
if ($Apply) {
    if (-not $ConfirmAlreadyAssociated) {
        throw '-Apply requires -ConfirmAlreadyAssociated. First device association is intentionally unsupported.'
    }
    $repair = Join-Path $package $RepairInf
    if (-not (Test-Path -LiteralPath $repair -PathType Leaf)) {
        throw "Installed-driver repair INF not found: $repair"
    }
    foreach ($driverFile in @('V9XDISP.DRV', 'V9XMINI.VXD')) {
        if (-not (Test-Path -LiteralPath (Join-Path $package $driverFile) -PathType Leaf)) {
            throw "Installed-driver update requires $driverFile in the package."
        }
    }
}
if ([string]::IsNullOrWhiteSpace($JobId)) {
    $JobId = ('drv-{0}-{1:x4}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'),
              (Get-Random -Minimum 1 -Maximum 65535))
}
if ([Text.Encoding]::ASCII.GetByteCount($JobId) -gt 63 -or $JobId -notmatch '^[A-Za-z0-9._-]+$') {
    throw 'JobId must be 1-63 ASCII letters, numbers, dots, underscores, or hyphens.'
}
if ([string]::IsNullOrWhiteSpace($ResultsDirectory)) {
    $ResultsDirectory = Join-Path (Join-Path $PSScriptRoot '..\build\driver-results') $JobId
}
$results = [IO.Path]::GetFullPath($ResultsDirectory)
New-Item -ItemType Directory -Force -Path $results | Out-Null
$guestJob = "C:\V9XREMOTE\JOBS\$JobId"

$initialInfo = Invoke-V9xCtlJson -Operation info
if (([uint32]$initialInfo.Capabilities -band [uint32]0x00000200) -eq 0) {
    throw "Agent build '$($initialInfo.BuildId)' does not advertise guarded driver-update orchestration."
}
$upload = Invoke-V9xCtlJson -Operation push-tree -OperationArguments @(
    '-Source', $package, '-Destination', $guestJob
)
$preflightResult = Invoke-V9xCtlJson -Operation exec -OperationArguments @(
    '-Application', "$guestJob\$PreflightProgram", '-Arguments', $PreflightArguments,
    '-WorkingDirectory', $guestJob, '-TimeoutSeconds', '120'
)

$applyResult = $null
$rebootResult = $null
$desktopResult = $null
$screenshotResult = $null
if ($Apply) {
    $applyResult = Invoke-V9xCtlJson -Operation exec -OperationArguments @(
        '-Application', 'C:\WINDOWS\RUNDLL.EXE',
        '-Arguments', "C:\WINDOWS\SYSTEM\SETUPX.DLL,InstallHinfSection DefaultInstall 132 $guestJob\$RepairInf",
        '-WorkingDirectory', $guestJob, '-TimeoutSeconds', '120'
    )
    $rebootResult = Invoke-V9xCtlJson -Operation reboot -OperationArguments @(
        '-JobId', $JobId, '-WaitSeconds', [string]$BootTimeoutSeconds
    )
    $desktopResult = Invoke-V9xCtlJson -Operation wait-desktop -OperationArguments @(
        '-WaitSeconds', [string]$BootTimeoutSeconds
    )
    if (-not $SkipScreenshot) {
        $desktopBmp = Join-Path $results 'DESKTOP.BMP'
        $screenshotResult = Invoke-V9xCtlJson -Operation screenshot -OperationArguments @(
            '-Destination', $desktopBmp
        )
    }
}

$summary = [pscustomobject]@{
    Success = $true
    JobId = $JobId
    Applied = [bool]$Apply
    Scope = 'already-associated-driver-only'
    PackagePath = $package
    GuestJobPath = $guestJob
    ResultsDirectory = $results
    InitialInfo = $initialInfo
    Upload = $upload
    Preflight = $preflightResult
    DriverRepair = $applyResult
    Reboot = $rebootResult
    Desktop = $desktopResult
    Screenshot = $screenshotResult
}
$summaryPath = Join-Path $results 'cycle.json'
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $summaryPath -Encoding UTF8
if ($Json) { $summary | ConvertTo-Json -Depth 8 -Compress } else { $summary | Format-List }
