[CmdletBinding()]
param(
    [switch]$SkipTests
)

# Builds the complete release artifact set into build\release:
#   v9xremote-<version>.zip   host scripts, MCP server, docs, guest package
#   V9XREMOTE.ISO             first-install CD image of the guest package
#   SHA256SUMS                digests of both artifacts

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

$versionHeader = Get-Content -LiteralPath (Join-Path $repoRoot 'include\v9xremote\version.h') -Raw
if ($versionHeader -notmatch '#define V9X_AGENT_VERSION "([^"]+)"') {
    throw 'V9X_AGENT_VERSION was not found in include\v9xremote\version.h.'
}
$version = $Matches[1]

& (Join-Path $PSScriptRoot 'build-guest.ps1') -BuildId "v$version" | Out-Host
if ($LASTEXITCODE -ne 0 -and $null -ne $LASTEXITCODE) { throw 'Guest build failed.' }

if (-not $SkipTests) {
    & (Join-Path $PSScriptRoot 'build-host-tests.ps1') | Out-Host
    if ($LASTEXITCODE -ne 0 -and $null -ne $LASTEXITCODE) { throw 'Host tests failed.' }
    if (Test-Path -LiteralPath (Join-Path $repoRoot 'mcp\test_v9x_mcp.py')) {
        $python = Get-Command python -ErrorAction SilentlyContinue
        if ($null -eq $python) { $python = Get-Command python3 -ErrorAction SilentlyContinue }
        if ($null -ne $python) {
            & $python.Source -m unittest discover -s (Join-Path $repoRoot 'mcp') -p 'test_*.py' | Out-Host
            if ($LASTEXITCODE -ne 0) { throw 'MCP server tests failed.' }
        } else {
            Write-Warning 'Python was not found; skipping MCP server tests.'
        }
    }
}

$isoPath = Join-Path $repoRoot 'build\V9XREMOTE.ISO'
& (Join-Path $PSScriptRoot 'make-install-media.ps1') -OutputPath $isoPath -Validate | Out-Host

$releaseRoot = Join-Path $repoRoot 'build\release'
$stageName = "v9xremote-$version"
$stageDir = Join-Path $releaseRoot $stageName
if (Test-Path -LiteralPath $stageDir) { Remove-Item -LiteralPath $stageDir -Recurse -Force -Confirm:$false }
New-Item -ItemType Directory -Force -Path $stageDir | Out-Null

Copy-Item -Path (Join-Path $repoRoot 'build\install') -Destination (Join-Path $stageDir 'install') -Recurse
New-Item -ItemType Directory -Force -Path (Join-Path $stageDir 'scripts') | Out-Null
foreach ($script in @('v9xctl.ps1', 'V9xProtocol.ps1', 'make-install-media.ps1')) {
    Copy-Item -LiteralPath (Join-Path $repoRoot "scripts\$script") -Destination (Join-Path $stageDir 'scripts')
}
if (Test-Path -LiteralPath (Join-Path $repoRoot 'mcp')) {
    Copy-Item -Path (Join-Path $repoRoot 'mcp') -Destination (Join-Path $stageDir 'mcp') -Recurse
}
Copy-Item -Path (Join-Path $repoRoot 'docs') -Destination (Join-Path $stageDir 'docs') -Recurse
foreach ($file in @('README.md', 'AGENTS.md', 'LICENSE', 'CHANGELOG.md', 'SECURITY.md')) {
    Copy-Item -LiteralPath (Join-Path $repoRoot $file) -Destination $stageDir
}

$zipPath = Join-Path $releaseRoot "$stageName.zip"
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force -Confirm:$false }
Compress-Archive -Path $stageDir -DestinationPath $zipPath
Copy-Item -LiteralPath $isoPath -Destination (Join-Path $releaseRoot 'V9XREMOTE.ISO') -Force

$sums = @()
foreach ($artifact in @("$stageName.zip", 'V9XREMOTE.ISO')) {
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $releaseRoot $artifact)).Hash.ToLowerInvariant()
    $sums += "{0} *{1}" -f $hash, $artifact
}
Set-Content -LiteralPath (Join-Path $releaseRoot 'SHA256SUMS') -Encoding Ascii -Value $sums

[pscustomobject]@{
    Version = $version
    Zip = $zipPath
    Iso = (Join-Path $releaseRoot 'V9XREMOTE.ISO')
    Sums = (Join-Path $releaseRoot 'SHA256SUMS')
}
