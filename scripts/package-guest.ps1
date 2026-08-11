[CmdletBinding()]
param(
    [string]$BuildId = 'local',
    [string]$PackageName = 'install'
)

& (Join-Path $PSScriptRoot 'build-guest.ps1') -BuildId $BuildId -PackageName $PackageName
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
