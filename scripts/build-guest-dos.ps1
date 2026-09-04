[CmdletBinding()]
param(
    [string]$BuildId = "local",
    [ValidatePattern('^[A-Za-z0-9._+-]+$')]
    [string]$PackageName = "dos-install"
)

# Builds the DOS (DOS/4GW protected-mode) agent V9XAGNT.EXE and assembles the
# install package under build\<PackageName>. Unlike build-guest.ps1 this target
# links the normal Watcom DOS C runtime plus Watt-32, and has no PE import audit
# (that check is Win32-specific). Requires Open Watcom and Watt-32.

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$outputDir = Join-Path $repoRoot "build\guest-dos"
$packageDir = Join-Path $repoRoot "build\$PackageName"

if ($BuildId -notmatch '^[A-Za-z0-9._+-]+$') {
    throw "BuildId may contain only letters, digits, dot, underscore, plus, and hyphen."
}

$versionHeader = Get-Content -LiteralPath (Join-Path $repoRoot 'include\v9xremote\version.h') -Raw
if ($versionHeader -notmatch '#define V9X_AGENT_VERSION "([^"]+)"') {
    throw 'V9X_AGENT_VERSION was not found in include\v9xremote\version.h.'
}
$agentVersion = $Matches[1]

$watcomRoot = $env:WATCOM
if (-not $watcomRoot -and (Test-Path -LiteralPath "C:\WATCOM")) {
    $watcomRoot = "C:\WATCOM"
}
if (-not $watcomRoot) {
    throw "Open Watcom was not found. Set WATCOM or install it at C:\WATCOM."
}

# Watt-32: WATT_ROOT (its own convention) or WATT32 env, else a vendored copy.
$wattRoot = $env:WATT_ROOT
if (-not $wattRoot) { $wattRoot = $env:WATT32 }
if (-not $wattRoot -and (Test-Path -LiteralPath (Join-Path $repoRoot 'third_party\watt32'))) {
    $wattRoot = Join-Path $repoRoot 'third_party\watt32'
}
if (-not $wattRoot) {
    throw "Watt-32 was not found. Set WATT_ROOT (or WATT32) to your Watt-32 tree, or vendor it at third_party\watt32."
}
$wattInc = Join-Path $wattRoot 'inc'
# 32-bit flat (DOS/4GW) Watcom import library; name can vary by Watt-32 build.
$wattLib = Join-Path $wattRoot 'lib\wattcpwf.lib'
if (-not (Test-Path -LiteralPath $wattLib)) {
    $alt = Get-ChildItem -LiteralPath (Join-Path $wattRoot 'lib') -Filter 'wattcpw*.lib' -ErrorAction SilentlyContinue |
        Sort-Object Name | Select-Object -First 1
    if ($alt) { $wattLib = $alt.FullName }
}

$toolDir = Join-Path $watcomRoot "binnt64"
$compiler = Join-Path $toolDir "wcc386.exe"
$linker = Join-Path $toolDir "wlink.exe"
$dos4gw = Join-Path $watcomRoot "binw\dos4gw.exe"
$required = @($compiler, $linker, $wattInc, $wattLib)
$missing = @($required | Where-Object { -not (Test-Path -LiteralPath $_) })
if ($missing.Count -ne 0) {
    throw "Required build inputs are missing: $($missing -join ', ')"
}

$env:WATCOM = $watcomRoot
$env:Path = "$toolDir;$(Join-Path $watcomRoot 'binnt');$env:Path"
$env:INCLUDE = "$(Join-Path $repoRoot 'include');$wattInc;$(Join-Path $watcomRoot 'h')"
New-Item -ItemType Directory -Force -Path $outputDir, $packageDir | Out-Null

$sources = @(
    "src\common\bounds.c",
    "src\common\crc32.c",
    "src\common\frame.c",
    "src\guest-dos\config.c",
    "src\guest-dos\net.c",
    "src\guest-dos\serve.c",
    "src\guest-dos\exec_dos.c",
    "src\guest-dos\files_dos.c",
    "src\guest-dos\main.c"
)
$objects = @()
foreach ($relativeSource in $sources) {
    $source = Join-Path $repoRoot $relativeSource
    $objectName = ($relativeSource -replace '[\\/]', '_') -replace '\.c$', '.obj'
    $object = Join-Path $outputDir $objectName
    # -bt=dos -mf : 32-bit flat model for the DOS/4GW extender.
    & $compiler "-bt=dos" "-mf" "-zq" "-zl" "-s" "-ox" `
        "-dV9X_BUILD_ID=`"$BuildId`"" "-fo=$object" $source
    if ($LASTEXITCODE -ne 0) {
        throw "Open Watcom failed to compile $relativeSource."
    }
    $objects += $object
}

$executable = Join-Path $outputDir "V9XAGNT.EXE"
$mapFile = Join-Path $outputDir "V9XAGNT.MAP"
$linkFile = Join-Path $outputDir "V9XAGNT.LNK"
$linkLines = @(
    "system dos4g",
    "option quiet",
    "option map='$mapFile'",
    "name '$executable'"
)
$linkLines += $objects | ForEach-Object { "file '$_'" }
$linkLines += "library '$wattLib'"
Set-Content -LiteralPath $linkFile -Encoding Ascii -Value $linkLines
& $linker "@$linkFile"
if ($LASTEXITCODE -ne 0) {
    throw "Open Watcom failed to link V9XAGNT.EXE."
}

$bytes = [System.IO.File]::ReadAllBytes($executable)
if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4d -or $bytes[1] -ne 0x5a) {
    throw "Guest output is not a valid MZ executable."
}
$imageText = [Text.Encoding]::ASCII.GetString($bytes)
if (-not $imageText.Contains($BuildId)) {
    throw "Guest output does not contain build ID '$BuildId'."
}

Copy-Item -LiteralPath $executable -Destination (Join-Path $packageDir 'V9XAGNT.EXE') -Force
if (Test-Path -LiteralPath $dos4gw) {
    Copy-Item -LiteralPath $dos4gw -Destination (Join-Path $packageDir 'DOS4GW.EXE') -Force
} else {
    Write-Warning "dos4gw.exe not found at $dos4gw; add DOS4GW.EXE to the package manually."
}
foreach ($name in @('INSTALL.BAT', 'AGENT.INI', 'WATTCP.CFG', 'README.TXT')) {
    $sourceLines = @(Get-Content -LiteralPath (Join-Path $repoRoot "packaging\dos\$name"))
    if ($name -eq 'README.TXT') {
        $sourceLines = $sourceLines -replace '@VERSION@', $agentVersion
    }
    Set-Content -LiteralPath (Join-Path $packageDir $name) -Encoding Ascii -Value $sourceLines
}
$hashLines = Get-ChildItem -LiteralPath $packageDir -File |
    Where-Object Name -ne 'SHA256.TXT' |
    Sort-Object Name |
    ForEach-Object { "{0} *{1}" -f (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant(), $_.Name }
Set-Content -LiteralPath (Join-Path $packageDir 'SHA256.TXT') -Encoding Ascii -Value $hashLines

[pscustomobject]@{
    Executable = $executable
    Package = $packageDir
    BuildId = $BuildId
    Bytes = $bytes.Length
    Watt32 = $wattLib
}
