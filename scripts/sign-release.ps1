[CmdletBinding()]
param(
    # The release to sign, e.g. 0.12.0 (tag v0.12.0).
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+(\.\d+)?$')]
    [string]$Version,
    # The public repo (owner/name). Default: the publish mirror's origin.
    [string]$Repo,
    # Sign a local zip instead of downloading the release asset (testing).
    [string]$Zip,
    # Attach SIGNED.TXT to the GitHub release.
    [switch]$Upload
)

# Writes and signs SIGNED.TXT for a release's self-update package
# (docs\remote-help.md). Run on the developer's PC after CI has published
# the release: it downloads v9x-remote-agent-<version>-win32.zip from the
# release (the exact bytes clients will get), lists its size and SHA-256,
# signs that with %USERPROFILE%\.v9x\release-signing.key using
# build\tools\v9xsign.exe, verifies the result, and with -Upload attaches
# SIGNED.TXT to the release. Then import it on the update server with the
# updates_publish_github_release MCP tool (app v9x-remote-agent).

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$signer = Join-Path $repoRoot 'build\tools\v9xsign.exe'
$keyFile = Join-Path $env:USERPROFILE '.v9x\release-signing.key'
if (-not (Test-Path -LiteralPath $signer)) { throw "Missing $signer; run scripts\build-host-tests.ps1." }
if (-not (Test-Path -LiteralPath $keyFile)) { throw "Missing release signing key $keyFile." }

# The key must be the one the shipped agents trust.
$header = Get-Content -LiteralPath (Join-Path $repoRoot 'include\v9xremote\release_key.h') -Raw
if ($header -notmatch 'V9X_RELEASE_PUBLIC_KEY_HEX\s*\\\s*"([0-9a-f]{64})"') {
    throw 'V9X_RELEASE_PUBLIC_KEY_HEX was not found in release_key.h.'
}
$expectedPublic = $Matches[1]
$public = (& $signer public "@$keyFile" | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $public -ne $expectedPublic) {
    throw "The key in $keyFile is not the release key in release_key.h."
}

$name = "v9x-remote-agent-$Version-win32.zip"
$outDir = Join-Path $repoRoot "build\release-signing\$Version"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
if (-not $Repo -and -not $Zip) {
    $mirrorConfig = Join-Path $repoRoot 'build\github-mirror\.git\config'
    if (Test-Path -LiteralPath $mirrorConfig) {
        $origin = (Get-Content -LiteralPath $mirrorConfig -Raw)
        if ($origin -match 'url\s*=\s*https://github\.com/([^/\s]+/[^/\s]+?)(\.git)?\s') { $Repo = $Matches[1] }
    }
    if (-not $Repo) { throw 'Pass -Repo owner/name (no publish mirror to read it from).' }
}
if ($Zip) {
    $zipPath = (Resolve-Path -LiteralPath $Zip).Path
} else {
    $zipPath = Join-Path $outDir $name
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force -Confirm:$false }
    gh release download "v$Version" --repo $Repo --pattern $name --dir $outDir
    if ($LASTEXITCODE -ne 0) { throw "Could not download $name from release v$Version of $Repo." }
}

$size = (Get-Item -LiteralPath $zipPath).Length
$sha = (Get-FileHash -Algorithm SHA256 -LiteralPath $zipPath).Hash.ToLowerInvariant()
$build = "v$Version"
$body = "[V9xRemoteRelease]`r`nSchema=1`r`nApp=v9x-remote-agent`r`nVersion=$Version`r`nBuild=$build`r`n`r`n" +
        "[Family.win32]`r`nFile=$name`r`nSize=$size`r`nSha256=$sha`r`n`r`n"
$bodyPath = Join-Path $outDir 'SIGNED.BODY'
[IO.File]::WriteAllText($bodyPath, $body, [Text.Encoding]::ASCII)
$signature = (& $signer sign "@$keyFile" $bodyPath | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $signature -notmatch '^[0-9a-f]{128}$') { throw 'Signing failed.' }
& $signer verify $expectedPublic $signature $bodyPath | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'The new signature does not verify.' }
$signedPath = Join-Path $outDir 'SIGNED.TXT'
[IO.File]::WriteAllText($signedPath, $body + "[Signature]`r`nEd25519=$signature`r`n", [Text.Encoding]::ASCII)
Remove-Item -LiteralPath $bodyPath -Force -Confirm:$false

if ($Upload) {
    gh release upload "v$Version" $signedPath --repo $Repo --clobber
    if ($LASTEXITCODE -ne 0) { throw 'Uploading SIGNED.TXT failed.' }
}

[pscustomobject]@{
    Version = $Version
    Package = $zipPath
    Size = $size
    Sha256 = $sha
    Signed = $signedPath
    Uploaded = [bool]$Upload
}
