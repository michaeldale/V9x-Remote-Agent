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
        # Update and remote-help modules (not named test_update.exe: Windows'
        # installer detection wants elevation for any exe called *update*):
        # Ed25519/SHA-256/SHA-512, inflate
        # and zip reading, the update-server text protocol, the signed
        # release file and the relay chunk framing.
        & $compilerDriver '-bt=nt' '-zq' '-wx' '-s' '-fe=test_crypt.exe' `
            (Join-Path $repoRoot 'tests\host\test_update_main.c') `
            (Join-Path $repoRoot 'tests\host\test_crypto.c') `
            (Join-Path $repoRoot 'tests\host\test_inflate.c') `
            (Join-Path $repoRoot 'tests\host\test_update_proto.c') `
            (Join-Path $repoRoot 'tests\host\test_update_release.c') `
            (Join-Path $repoRoot 'tests\host\test_relay.c') `
            (Join-Path $repoRoot 'src\common\ed25519.c') `
            (Join-Path $repoRoot 'src\common\sha256.c') `
            (Join-Path $repoRoot 'src\common\sha512.c') `
            (Join-Path $repoRoot 'src\common\inflate.c') `
            (Join-Path $repoRoot 'src\common\zipread.c') `
            (Join-Path $repoRoot 'src\common\crc32.c') `
            (Join-Path $repoRoot 'src\common\update_proto.c') `
            (Join-Path $repoRoot 'src\common\update_release.c') `
            (Join-Path $repoRoot 'src\common\relay.c')
        if ($LASTEXITCODE -ne 0) { throw 'Open Watcom failed to build the update/relay host tests.' }
        & (Join-Path $outputDir 'test_crypt.exe')
        if ($LASTEXITCODE -ne 0) { throw "Update/relay host tests failed with exit code $LASTEXITCODE." }
        # The operator's signing tool (releases and remote-help relay
        # batches), built from the same modules the agent verifies with.
        $toolsDir = Join-Path $repoRoot 'build\tools'
        New-Item -ItemType Directory -Force -Path $toolsDir | Out-Null
        & $compilerDriver '-bt=nt' '-zq' '-wx' '-s' "-fe=$(Join-Path $toolsDir 'v9xsign.exe')" `
            (Join-Path $repoRoot 'tools\v9xsign.c') `
            (Join-Path $repoRoot 'src\common\ed25519.c') `
            (Join-Path $repoRoot 'src\common\sha256.c') `
            (Join-Path $repoRoot 'src\common\sha512.c') `
            (Join-Path $repoRoot 'src\common\relay.c')
        if ($LASTEXITCODE -ne 0) { throw 'Open Watcom failed to build tools\v9xsign.c.' }
        # V9XVNC.EXE's DES (VNC authentication) and ZRLE deflate encoder.
        & $compilerDriver '-bt=nt' '-zq' '-wx' '-s' '-fe=test_vnc.exe' `
            (Join-Path $repoRoot 'tests\host\test_vnc.c') `
            (Join-Path $repoRoot 'src\guest\vncdes.c') `
            (Join-Path $repoRoot 'src\guest\vncz.c')
        if ($LASTEXITCODE -ne 0) { throw 'Open Watcom failed to build the VNC host tests.' }
        & (Join-Path $outputDir 'test_vnc.exe')
        if ($LASTEXITCODE -ne 0) { throw "VNC DES tests failed with exit code $LASTEXITCODE." }
        $cases = Join-Path $outputDir 'vnc-deflate.bin'
        & (Join-Path $outputDir 'test_vnc.exe') 'deflate' $cases | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'test_vnc.exe could not write the deflate cases.' }
        # zlib is the reference: every case must inflate back exactly.
        & python (Join-Path $repoRoot 'tests\host\Test-VncDeflate.py') $cases
        if ($LASTEXITCODE -ne 0) { throw 'The ZRLE deflate output did not inflate with zlib.' }
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
