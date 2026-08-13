[CmdletBinding()]
param(
    [string]$BuildId = "local",
    [ValidatePattern('^[A-Za-z0-9._+-]+$')]
    [string]$PackageName = "install"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$outputDir = Join-Path $repoRoot "build\guest"
$packageDir = Join-Path $repoRoot "build\$PackageName"

if ($BuildId -notmatch '^[A-Za-z0-9._+-]+$') {
    throw "BuildId may contain only letters, digits, dot, underscore, plus, and hyphen."
}

$versionHeader = Get-Content -LiteralPath (Join-Path $repoRoot 'include\v9xremote\version.h') -Raw
if ($versionHeader -notmatch '#define V9X_AGENT_VERSION "([^"]+)"') {
    throw 'V9X_AGENT_VERSION was not found in include\v9xremote\version.h.'
}
$agentVersion = $Matches[1]
$changelogHead = @(Get-Content -LiteralPath (Join-Path $repoRoot 'CHANGELOG.md') |
    Where-Object { $_ -match '^## ' })[0]
if ($changelogHead -notmatch "^## $([regex]::Escape($agentVersion))(\s|$)") {
    throw "CHANGELOG.md top entry '$changelogHead' does not match version $agentVersion from version.h."
}

$watcomRoot = $env:WATCOM
if (-not $watcomRoot -and (Test-Path -LiteralPath "C:\WATCOM")) {
    $watcomRoot = "C:\WATCOM"
}
if (-not $watcomRoot) {
    throw "Open Watcom was not found. Set WATCOM or install it at C:\WATCOM."
}

$toolDir = Join-Path $watcomRoot "binnt64"
$compiler = Join-Path $toolDir "wcc386.exe"
$linker = Join-Path $toolDir "wlink.exe"
$dumper = Join-Path $toolDir "wdump.exe"
$libraries = @(
    (Join-Path $watcomRoot "lib386\nt\kernel32.lib"),
    (Join-Path $watcomRoot "lib386\nt\wsock32.lib"),
    (Join-Path $watcomRoot "lib386\nt\user32.lib"),
    (Join-Path $watcomRoot "lib386\nt\gdi32.lib"),
    (Join-Path $watcomRoot "lib386\nt\shell32.lib")
)
$required = @($compiler, $linker, $dumper) + $libraries
$missing = @($required | Where-Object { -not (Test-Path -LiteralPath $_) })
if ($missing.Count -ne 0) {
    throw "Required Open Watcom inputs are missing: $($missing -join ', ')"
}

$env:WATCOM = $watcomRoot
$env:Path = "$toolDir;$(Join-Path $watcomRoot 'binnt');$env:Path"
$env:INCLUDE = "$(Join-Path $repoRoot 'include');$(Join-Path $watcomRoot 'h');$(Join-Path $watcomRoot 'h\nt')"
New-Item -ItemType Directory -Force -Path $outputDir, $packageDir | Out-Null

$sources = @(
    "src\common\bounds.c",
    "src\common\crc32.c",
    "src\common\frame.c",
    "src\guest\agent.c",
    "src\guest\entry.c",
    "src\guest\execute.c",
    "src\guest\files.c",
    "src\guest\input.c",
    "src\guest\logging.c",
    "src\guest\power.c",
    "src\guest\protocol.c",
    "src\guest\screenshot.c",
    "src\guest\tray.c"
)
$objects = @()
foreach ($relativeSource in $sources) {
    $source = Join-Path $repoRoot $relativeSource
    $objectName = ($relativeSource -replace '[\\/]', '_') -replace '\.c$', '.obj'
    $object = Join-Path $outputDir $objectName
    & $compiler "-bt=nt" "-zq" "-wx" "-zl" "-s" "-os" `
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
    "format windows nt",
    "runtime windows=4.0",
    "option quiet",
    "option nodefaultlibs",
    "option start='_V9xAgentEntry@0'",
    "option stack=65536",
    "option map='$mapFile'",
    "name '$executable'"
)
$linkLines += $objects | ForEach-Object { "file '$_'" }
$linkLines += $libraries | ForEach-Object { "library '$_'" }
Set-Content -LiteralPath $linkFile -Encoding Ascii -Value $linkLines
& $linker "@$linkFile"
if ($LASTEXITCODE -ne 0) {
    throw "Open Watcom failed to link V9XAGNT.EXE."
}

$bytes = [System.IO.File]::ReadAllBytes($executable)
$peOffset = if ($bytes.Length -ge 64) { [BitConverter]::ToInt32($bytes, 0x3c) } else { -1 }
if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4d -or $bytes[1] -ne 0x5a -or
    $peOffset -lt 0 -or $peOffset + 1 -ge $bytes.Length -or
    $bytes[$peOffset] -ne 0x50 -or $bytes[$peOffset + 1] -ne 0x45) {
    throw "Guest output is not a valid MZ/PE executable."
}
$imageText = [Text.Encoding]::ASCII.GetString($bytes)
if (-not $imageText.Contains($BuildId)) {
    throw "Guest output does not contain build ID '$BuildId'."
}

$dumpText = (@(& $dumper -e $executable 2>&1)) -join "`n"
if ($LASTEXITCODE -ne 0) { throw "wdump could not inspect the guest executable." }
$dllNames = @([regex]::Matches($dumpText, 'DLL name = <([^>]+)>') |
    ForEach-Object { $_.Groups[1].Value.ToUpperInvariant() } | Sort-Object -Unique)
$unexpected = @($dllNames | Where-Object {
    $_ -notin @('KERNEL32.DLL', 'WSOCK32.DLL', 'USER32.DLL', 'GDI32.DLL',
                 'SHELL32.DLL')
})
if ($unexpected.Count -ne 0) {
    throw "Guest output imports unexpected DLLs: $($unexpected -join ', ')"
}
if ($dumpText -notmatch '(?im)subsystem major version number\s+=\s+0004H' -or
    $dumpText -notmatch '(?im)subsystem minor version number\s+=\s+0000H') {
    throw 'Guest output does not target Windows subsystem version 4.0.'
}
if ($dumpText -match '(?i)GetCommandLineW|GetModuleFileNameW|__CHK|MSVCR|UCRT') {
    throw "Guest output contains a forbidden runtime or Unicode startup import."
}
foreach ($requiredImport in @('Accept', 'Bind', 'CreateFileA', 'CreateMutexA',
                               'CreatePipe', 'CreateProcessA', 'CreateThread',
                               'DeleteFileA', 'DuplicateHandle',
                               'FindClose', 'FindFirstFileA',
                               'FindNextFileA', 'GetExitCodeProcess',
                               'GetFileAttributesA', 'GetFileSize',
                               'GetPrivateProfileIntA', 'GetPrivateProfileStringA',
                               'GetVersion', 'ExitWindowsEx', 'FindWindowA',
                               'GetSystemMetrics', 'Inet_addr', 'Listen', 'MoveFileA',
                               'PeekNamedPipe', 'Recv', 'Send', 'Socket',
                               'TerminateProcess', 'WSAStartup',
                               'GetCursorPos', 'SetCursorPos', 'MapVirtualKeyA',
                               'VkKeyScanA', 'keybd_event', 'mouse_event',
                               'CreateWindowExA', 'DestroyWindow', 'LoadIconA',
                               'Shell_NotifyIconA', 'gethostname', 'gethostbyname',
                               'inet_ntoa')) {
    if ($dumpText -notmatch "(?im)\s$([regex]::Escape($requiredImport))\s*$") {
        throw "Guest output is missing expected import $requiredImport."
    }
}
if ($dllNames -contains 'GDI32.DLL') {
    throw 'The long-lived guest agent must not import GDI32.DLL; capture belongs in V9XSHOT.EXE.'
}

$helperSource = Join-Path $repoRoot 'src\guest\screenshot_helper.c'
$helperObject = Join-Path $outputDir 'src_guest_screenshot_helper.obj'
$helperExecutable = Join-Path $outputDir 'V9XSHOT.EXE'
$helperMapFile = Join-Path $outputDir 'V9XSHOT.MAP'
$helperLinkFile = Join-Path $outputDir 'V9XSHOT.LNK'
& $compiler '-bt=nt' '-zq' '-wx' '-zl' '-s' '-os' "-fo=$helperObject" $helperSource
if ($LASTEXITCODE -ne 0) {
    throw 'Open Watcom failed to compile src\guest\screenshot_helper.c.'
}
$helperLinkLines = @(
    'format windows nt',
    'runtime windows=4.0',
    'option quiet',
    'option nodefaultlibs',
    "option start='_V9xScreenshotEntry@0'",
    'option stack=65536',
    "option map='$helperMapFile'",
    "name '$helperExecutable'",
    "file '$helperObject'",
    "library '$(Join-Path $watcomRoot 'lib386\nt\kernel32.lib')'",
    "library '$(Join-Path $watcomRoot 'lib386\nt\user32.lib')'",
    "library '$(Join-Path $watcomRoot 'lib386\nt\gdi32.lib')'"
)
Set-Content -LiteralPath $helperLinkFile -Encoding Ascii -Value $helperLinkLines
& $linker "@$helperLinkFile"
if ($LASTEXITCODE -ne 0) { throw 'Open Watcom failed to link V9XSHOT.EXE.' }
$helperDumpText = (@(& $dumper -e $helperExecutable 2>&1)) -join "`n"
if ($LASTEXITCODE -ne 0) { throw 'wdump could not inspect V9XSHOT.EXE.' }
foreach ($requiredImport in @('BitBlt', 'CreateCompatibleBitmap',
                               'CreateCompatibleDC', 'GetDIBits',
                               'SetErrorMode')) {
    if ($helperDumpText -notmatch "(?im)\s$([regex]::Escape($requiredImport))\s*$") {
        throw "Screenshot helper is missing expected import $requiredImport."
    }
}

Copy-Item -LiteralPath $executable -Destination (Join-Path $packageDir 'V9XAGNT.EXE') -Force
Copy-Item -LiteralPath $helperExecutable -Destination (Join-Path $packageDir 'V9XSHOT.EXE') -Force
foreach ($name in @('INSTALL.BAT', 'UNINSTALL.BAT', 'UPDATE.BAT', 'INSTALL.REG',
                     'REMOVE.REG', 'UPDATE.REG', 'AGENT.INI', 'README.TXT')) {
    $sourceLines = @(Get-Content -LiteralPath (Join-Path $repoRoot "packaging\win98se\$name"))
    if ($name -eq 'README.TXT') {
        $sourceLines = $sourceLines -replace '@VERSION@', $agentVersion
        if ($sourceLines.Count -ge 2 -and $sourceLines[1] -match '^=+$') {
            $sourceLines[1] = '=' * $sourceLines[0].Length
        }
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
    ScreenshotHelper = $helperExecutable
    Package = $packageDir
    BuildId = $BuildId
    Bytes = $bytes.Length
    Imports = $dllNames
}
