[CmdletBinding()]
param(
    [string]$Remote,
    [string]$Message,
    [switch]$Tag,
    [string]$MirrorDir,
    [switch]$DryRun
)

# Publishes the current state of this folder to the public GitHub repo as a
# single diff commit. The private repo stays primary; the public repo never
# receives history, only sanitized snapshots. Run this only when explicitly
# asked to publish.
#
# First run:  .\scripts\publish-github.ps1 -Remote https://github.com/<owner>/v9x-remote-agent.git -Tag
# Later runs: .\scripts\publish-github.ps1 [-Message "..."] [-Tag]

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($MirrorDir)) { $MirrorDir = Join-Path $repoRoot 'build\github-mirror' }

$versionHeader = Get-Content -LiteralPath (Join-Path $repoRoot 'include\v9xremote\version.h') -Raw
if ($versionHeader -notmatch '#define V9X_AGENT_VERSION "([^"]+)"') {
    throw 'V9X_AGENT_VERSION was not found in include\v9xremote\version.h.'
}
$version = $Matches[1]
if ([string]::IsNullOrWhiteSpace($Message)) { $Message = "v9x-remote-agent $version" }

if (-not (Test-Path -LiteralPath (Join-Path $MirrorDir '.git'))) {
    if ([string]::IsNullOrWhiteSpace($Remote)) {
        throw "No mirror exists at $MirrorDir. Pass -Remote for the first publish."
    }
    git clone $Remote $MirrorDir
    if ($LASTEXITCODE -ne 0) { throw "Cloning $Remote failed." }
    git -C $MirrorDir checkout -B main
    if ($LASTEXITCODE -ne 0) { throw 'Preparing the main branch failed.' }
}

# Mirror the working tree. /XD with bare names excludes matching directories
# on both sides, so the mirror's .git survives /MIR deletion.
robocopy $repoRoot $MirrorDir /MIR /XD .git .claude build __pycache__ .pytest_cache /XF *.pyc *.pyo /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed with exit code $LASTEXITCODE." }

# Hard scrub gate: nothing private may reach the mirror.
$violations = @()
$privateName = -join ([char[]](109, 105, 99, 104, 97, 101, 108))
$privateWorkspace = -join ([char[]](67, 58, 92, 101, 118, 101, 114, 121, 116, 104, 105, 110, 103))
$privateProfileRoot = -join ([char[]](67, 58, 92, 85, 115, 101, 114, 115, 92))
$privatePatterns = @($privateWorkspace, $privateProfileRoot, $privateName)
$textFiles = Get-ChildItem -LiteralPath $MirrorDir -Recurse -File |
    Where-Object { $_.FullName -notmatch '\\\.git\\' -and $_.Extension -notin @('.png', '.bmp', '.iso', '.exe', '.zip') }
foreach ($file in $textFiles) {
    $matches2 = Select-String -LiteralPath $file.FullName -Pattern $privatePatterns -SimpleMatch -AllMatches -ErrorAction SilentlyContinue
    foreach ($hit in $matches2) {
        $violations += "{0}:{1}: {2}" -f $file.FullName.Substring($MirrorDir.Length + 1), $hit.LineNumber, $hit.Line.Trim()
    }
}
if ($violations.Count -ne 0) {
    $violations | ForEach-Object { Write-Warning $_ }
    throw "Publish blocked: $($violations.Count) private-path/name hits in the mirror. Fix the sources and re-run."
}

git -C $MirrorDir add -A
if ($LASTEXITCODE -ne 0) { throw 'git add failed.' }
$pending = git -C $MirrorDir status --porcelain
if ([string]::IsNullOrWhiteSpace(($pending -join ''))) {
    Write-Output 'Nothing to publish: the mirror already matches.'
    exit 0
}
git -C $MirrorDir diff --cached --stat | Out-Host

if ($DryRun) {
    Write-Output 'Dry run: no commit, no push. The staged mirror is at:'
    Write-Output "  $MirrorDir"
    exit 0
}

git -C $MirrorDir commit -m $Message
if ($LASTEXITCODE -ne 0) { throw 'git commit failed.' }
if ($Tag) {
    git -C $MirrorDir tag "v$version"
    if ($LASTEXITCODE -ne 0) { throw "Tagging v$version failed (does the tag already exist?)." }
}
git -C $MirrorDir push origin main
if ($LASTEXITCODE -ne 0) { throw 'git push failed.' }
if ($Tag) {
    git -C $MirrorDir push origin "v$version"
    if ($LASTEXITCODE -ne 0) { throw 'Pushing the tag failed.' }
}

[pscustomobject]@{
    Version = $version
    Message = $Message
    Tagged = [bool]$Tag
    Mirror = $MirrorDir
}
