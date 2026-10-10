[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
& (Join-Path $repoRoot 'tests\host\Test-Protocol.ps1')

$watcomRoot = $env:WATCOM
if (-not $watcomRoot -and (Test-Path -LiteralPath 'C:\WATCOM')) { $watcomRoot = 'C:\WATCOM' }
if (-not $watcomRoot) { throw 'Open Watcom is required for the native host tests.' }
$compilerDriver = Join-Path $watcomRoot 'binnt64\wcl386.exe'
if (-not (Test-Path -LiteralPath $compilerDriver)) { throw "Missing $compilerDriver" }
$outputDir = Join-Path $repoRoot 'build\host-tests'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$oldInclude = $env:INCLUDE
$oldPath = $env:Path
$oldWatcom = $env:WATCOM
try {
    $env:WATCOM = $watcomRoot
    $env:INCLUDE = "$(Join-Path $repoRoot 'include');$(Join-Path $watcomRoot 'h');$(Join-Path $watcomRoot 'h\nt')"
    $env:Path = "$(Join-Path $watcomRoot 'binnt64');$(Join-Path $watcomRoot 'binnt');$oldPath"
    Push-Location $outputDir
    try {
        & $compilerDriver '-bt=nt' '-zq' '-wx' '-s' '-fe=test_protocol.exe' `
            (Join-Path $repoRoot 'tests\host\test_protocol.c') `
            (Join-Path $repoRoot 'src\common\frame.c') `
            (Join-Path $repoRoot 'src\common\bounds.c') `
            (Join-Path $repoRoot 'src\common\crc32.c') `
            (Join-Path $repoRoot 'src\common\installid.c') `
            (Join-Path $repoRoot 'src\common\mdns.c')
        if ($LASTEXITCODE -ne 0) { throw 'Open Watcom failed to build native protocol tests.' }
        & (Join-Path $outputDir 'test_protocol.exe')
        if ($LASTEXITCODE -ne 0) { throw "Native protocol tests failed with exit code $LASTEXITCODE." }
        # The PowerShell and Python discovery tests decode this fixture; it
        # must stay what the C encoder actually produces.
        $announce = (& (Join-Path $outputDir 'test_protocol.exe') 'dump-mdns' | Out-String).Trim()
        $fixture = (Get-Content -LiteralPath (Join-Path $repoRoot 'tests\host\mdns-announce.hex') -Raw).Trim()
        if ($announce -ne $fixture) {
            throw 'tests\host\mdns-announce.hex no longer matches the C mDNS encoder; regenerate it with test_protocol.exe dump-mdns.'
        }
    } finally {
        Pop-Location
    }
} finally {
    $env:INCLUDE = $oldInclude
    $env:Path = $oldPath
    $env:WATCOM = $oldWatcom
}
Write-Output 'PASS: native C frame, bounds, and encoding tests'
& (Join-Path $repoRoot 'tests\host\Test-V9xCtl.ps1')
& (Join-Path $repoRoot 'tests\host\Test-V9xFileCtl.ps1')
& (Join-Path $repoRoot 'tests\host\Test-V9xM4Ctl.ps1')
& (Join-Path $repoRoot 'tests\host\Test-V9xDiscovery.ps1')
