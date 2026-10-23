$ErrorActionPreference = 'Stop'
# v9xctl delete/hash/cat/ps/kill/reg (agent 0.13.0+) against a fixture agent
# that checks every request byte for byte and answers like the C agent.
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$protocolScript = Join-Path $repoRoot 'scripts\V9xProtocol.ps1'
$clientScript = Join-Path $repoRoot 'scripts\v9xctl.ps1'
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('v9x-tools-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$localFile = Join-Path $testRoot 'driver.bin'
[IO.File]::WriteAllBytes($localFile, [byte[]](1..200 | ForEach-Object { [byte]($_ % 256) }))
$localSha = [byte[]]::new(32)
$hex = (Get-FileHash -LiteralPath $localFile -Algorithm SHA256).Hash
for ($index = 0; $index -lt 32; ++$index) { $localSha[$index] = [Convert]::ToByte($hex.Substring($index * 2, 2), 16) }

$portProbe = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$portProbe.Start()
$port = ([Net.IPEndPoint]$portProbe.LocalEndpoint).Port
$portProbe.Stop()

$server = Start-Job -ArgumentList $port, $protocolScript, $localSha -ScriptBlock {
    param($Port, $ProtocolScript, [byte[]]$Sha)
    $ErrorActionPreference = 'Stop'
    . $ProtocolScript
    $allTools = [uint32](0x7f -bor 0x40000 -bor 0x80000 -bor 0x100000 -bor 0x200000)
    function Send-Hello($Stream, $RequestId, [uint32]$Capabilities) {
        $payload = [Collections.Generic.List[byte]]::new()
        $payload.AddRange([BitConverter]::GetBytes([uint16]0x0100))
        $payload.AddRange([BitConverter]::GetBytes([uint16]0))
        $payload.AddRange([BitConverter]::GetBytes($Capabilities))
        $payload.AddRange([BitConverter]::GetBytes([uint32]65536))
        $payload.AddRange([BitConverter]::GetBytes([uint32]10))
        $payload.AddRange([BitConverter]::GetBytes([uint16]9869))
        $payload.AddRange([BitConverter]::GetBytes([uint16]0x0101))
        Add-V9xString -Bytes $payload -Value 'tools-fixture'
        $frame = New-V9xFrameBytes -Type 0x8001 -RequestId $RequestId -Payload $payload.ToArray()
        $Stream.Write($frame, 0, $frame.Length)
    }
    function Reply($Stream, $Request, [uint16]$Type, [byte[]]$Payload) {
        $frame = New-V9xFrameBytes -Type $Type -RequestId $Request.RequestId -Payload $Payload
        $Stream.Write($frame, 0, $frame.Length)
    }
    function Expect-Request($Stream, [uint16]$Type, [string]$Hex) {
        $request = Read-V9xFrame -Stream $Stream
        if ($request.Type -ne $Type) { throw ('Expected 0x{0:X4}, got 0x{1:X4}.' -f $Type, $request.Type) }
        $actual = ($request.Payload | ForEach-Object { $_.ToString('X2') }) -join ''
        if ($actual -ne $Hex) { throw ('Request 0x{0:X4} payload {1}, expected {2}.' -f $Type, $actual, $Hex) }
        return $request
    }
    function Hex([byte[]]$Bytes) { ($Bytes | ForEach-Object { $_.ToString('X2') }) -join '' }
    function Str([string]$Value) {
        $list = [Collections.Generic.List[byte]]::new(); Add-V9xString -Bytes $list -Value $Value; $list.ToArray()
    }
    function U32([uint32]$Value) { [BitConverter]::GetBytes($Value) }
    function Process-List {
        [byte[]]((U32 2) + (U32 0x1000) + (U32 0) + (U32 3) + (U32 2) + (U32 0) + (Str 'C:\V9XREMOTE\V9XAGNT.EXE') +
                 (U32 0x2000) + (U32 0x1000) + (U32 1) + (U32 1) + (U32 77) + (Str 'C:\WINDOWS\NOTEPAD.EXE'))
    }
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $Port)
    try {
        $listener.Start()
        $steps = @(
            { param($s)  # cat
              $r = Expect-Request $s 0x002A (Hex ((U32 2) + (U32 100) + (Str 'err') + (Str 'C:\LOG.TXT')))
              $text = [Text.Encoding]::ASCII.GetBytes("error two`nerror three`n")
              Reply $s $r 0x802A ([byte[]]((U32 5000) + (U32 90) + (U32 3) + (U32 2) + (U32 0) + (U32 $text.Length) + $text)) },
            { param($s)  # hash
              $r = Expect-Request $s 0x0029 (Hex (Str 'C:\WINDOWS\SYSTEM\V9X.DRV'))
              Reply $s $r 0x8029 ([byte[]]((U32 200) + (U32 0xDEADBEEFL) + $Sha)) },
            { param($s)  # ps
              $r = Expect-Request $s 0x0080 ''
              Reply $s $r 0x8080 (Process-List) },
            { param($s)  # kill -Name: list, then kill the one match
              $r = Expect-Request $s 0x0080 ''
              Reply $s $r 0x8080 (Process-List)
              $r = Expect-Request $s 0x0081 (Hex ((U32 0x2000) + (U32 1)))
              Reply $s $r 0x8081 ([byte[]]((U32 0x2000) + (U32 77))) },
            { param($s)  # reg
              $r = Expect-Request $s 0x0090 (Hex ((U32 0x80000002L) + (Str 'Software\Test')))
              $values = [byte[]]((Str '') + (U32 1) + (U32 6) + [Text.Encoding]::ASCII.GetBytes("hello`0") +
                                 (Str 'Count') + (U32 4) + (U32 4) + (U32 42) +
                                 (Str 'List') + (U32 7) + (U32 6) + [Text.Encoding]::ASCII.GetBytes("a`0bc`0`0") +
                                 (Str 'Blob') + (U32 3) + (U32 2) + [byte[]](0xAB, 0x01))
              Reply $s $r 0x8090 ([byte[]]((U32 1) + (U32 4) + (U32 0) + (Str 'Sub') + $values)) },
            { param($s)  # delete -Recurse -Force
              $r = Expect-Request $s 0x0028 (Hex ((U32 3) + (Str 'C:\JOBS\OLD')))
              Reply $s $r 0x8028 ([byte[]]((U32 5) + (U32 2))) }
        )
        foreach ($step in $steps) {
            $client = $listener.AcceptTcpClient()
            try {
                $stream = $client.GetStream()
                $hello = Read-V9xFrame -Stream $stream
                Send-Hello $stream $hello.RequestId $allTools
                & $step $stream
                Start-Sleep -Milliseconds 300
            } finally { $client.Dispose() }
        }
        # An agent without the tools: v9xctl must refuse before sending.
        $client = $listener.AcceptTcpClient()
        try {
            $stream = $client.GetStream()
            $hello = Read-V9xFrame -Stream $stream
            Send-Hello $stream $hello.RequestId ([uint32]0x7f)
            Start-Sleep -Milliseconds 750
            if ($stream.DataAvailable) { throw 'delete sent a request without V9X_CAP_FILE_TOOLS.' }
        } finally { $client.Dispose() }

        # wait-file: the first connection is dropped before HELLO (a port
        # forward answering while the agent is not listening); v9xctl retries
        # by itself. The file is missing twice, then appears.
        $client = $listener.AcceptTcpClient()
        $client.Dispose()
        $client = $listener.AcceptTcpClient()
        try {
            $stream = $client.GetStream()
            $hello = Read-V9xFrame -Stream $stream
            Send-Hello $stream $hello.RequestId ([uint32]0x7f)
            foreach ($exists in @($false, $false, $true)) {
                $r = Expect-Request $stream 0x0020 (Hex (Str 'C:\JOBS\DONE.TXT'))
                $stat = [byte[]]::new(16)
                if ($exists) { $stat[0] = 1; [Array]::Copy((U32 12), 0, $stat, 4, 4) }
                Reply $stream $r 0x8020 $stat
            }
            Start-Sleep -Milliseconds 300
        } finally { $client.Dispose() }

        # wait-window: no window, then the dialog.
        $client = $listener.AcceptTcpClient()
        try {
            $stream = $client.GetStream()
            $hello = Read-V9xFrame -Stream $stream
            Send-Hello $stream $hello.RequestId $allTools
            $tray = [byte[]]((U32 0x10) + (U32 1) + (U32 0) + (U32 0) + (U32 740) + (U32 1024) + (U32 768) +
                             (Str 'Shell_TrayWnd') + (Str ''))
            $r = Expect-Request $stream 0x0062 ''
            Reply $stream $r 0x8062 ([byte[]]((U32 1) + (U32 0) + $tray))
            $dialog = [byte[]]((U32 0x20) + (U32 2) + (U32 1) + (U32 100) + (U32 50) + (U32 500) + (U32 350) +
                               (Str '#32770') + (Str 'Setup [step 2]'))
            $r = Expect-Request $stream 0x0062 ''
            Reply $stream $r 0x8062 ([byte[]]((U32 2) + (U32 0) + $tray + $dialog))
            Start-Sleep -Milliseconds 300
        } finally { $client.Dispose() }
        'SERVER_PASS'
    } finally {
        $listener.Stop()
    }
}

function Invoke-FixtureClient([string[]]$Arguments, [int]$ExpectedExit = 0) {
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $clientScript `
            @Arguments -Host 127.0.0.1 -Port $port -Json 2>&1)
    } finally {
        $ErrorActionPreference = $savedPreference
    }
    if ($LASTEXITCODE -ne $ExpectedExit) {
        throw "v9xctl $($Arguments[0]) exited $LASTEXITCODE, expected ${ExpectedExit}: $($output -join ' ')"
    }
    $jsonLine = $output | Where-Object { $_ -is [string] -and $_.StartsWith('{') } | Select-Object -Last 1
    if ($ExpectedExit -eq 0 -and -not $jsonLine) { throw "v9xctl $($Arguments[0]) returned no JSON." }
    if ($jsonLine) { return $jsonLine | ConvertFrom-Json }
}

try {
    Start-Sleep -Milliseconds 400
    $cat = Invoke-FixtureClient @('cat', '-Path', 'C:\LOG.TXT', '-Tail', '2', '-Match', 'err', '-MaxBytes', '100')
    if ($cat.Text -ne "error two`nerror three`n" -or $cat.Lines -ne 90 -or $cat.Matched -ne 3 -or
        $cat.Returned -ne 2 -or $cat.Truncated) { throw "cat result mismatch: $($cat | ConvertTo-Json -Compress)" }
    $hash = Invoke-FixtureClient @('hash', '-Path', 'C:\WINDOWS\SYSTEM\V9X.DRV', '-Source', $localFile)
    if (-not $hash.Matches -or $hash.Crc32 -ne 'DEADBEEF' -or $hash.Size -ne 200) {
        throw "hash result mismatch: $($hash | ConvertTo-Json -Compress)"
    }
    $ps = Invoke-FixtureClient @('ps')
    if ($ps.Count -ne 2 -or -not $ps.Processes[0].Agent -or -not $ps.Processes[1].AgentJob -or
        $ps.Processes[1].Name -ne 'NOTEPAD.EXE' -or $ps.Processes[1].JobRequestId -ne 77) {
        throw "ps result mismatch: $($ps | ConvertTo-Json -Compress -Depth 4)"
    }
    $kill = Invoke-FixtureClient @('kill', '-Name', 'notepad.exe')
    if ($kill.Pid -ne 0x2000 -or -not $kill.WasAgentJob -or $kill.JobRequestId -ne 77) {
        throw "kill result mismatch: $($kill | ConvertTo-Json -Compress)"
    }
    $reg = Invoke-FixtureClient @('reg', '-Key', 'HKLM\Software\Test')
    $byName = @{}; foreach ($value in $reg.Values) { $byName[$value.Name] = $value }
    if ($reg.Subkeys -ne 'Sub' -or $byName['(Default)'].Data -ne 'hello' -or $byName['Count'].Data -ne 42 -or
        ($byName['List'].Data -join '|') -ne 'a|bc' -or $byName['Blob'].Data -ne 'AB 01' -or
        $byName['Blob'].Type -ne 'REG_BINARY') {
        throw "reg result mismatch: $($reg | ConvertTo-Json -Compress -Depth 4)"
    }
    $delete = Invoke-FixtureClient @('delete', '-Path', 'C:\JOBS\OLD', '-Recurse', '-Force')
    if ($delete.FilesDeleted -ne 5 -or $delete.DirectoriesDeleted -ne 2) {
        throw "delete result mismatch: $($delete | ConvertTo-Json -Compress)"
    }
    Invoke-FixtureClient @('delete', '-Path', 'C:\X') 23 | Out-Null
    $waited = Invoke-FixtureClient @('wait-file', '-Path', 'C:\JOBS\DONE.TXT', '-WaitSeconds', '10')
    if (-not $waited.Exists -or $waited.Size -ne 12) { throw "wait-file result mismatch: $($waited | ConvertTo-Json -Compress)" }
    # '[' in a title must match literally, not as a wildcard set.
    $window = Invoke-FixtureClient @('wait-window', '-Title', 'setup [step', '-WaitSeconds', '10')
    if ($window.Window.Title -ne 'Setup [step 2]' -or $window.Window.Width -ne 400 -or $window.Window.Height -ne 300 -or
        -not $window.Window.Foreground) { throw "wait-window result mismatch: $($window | ConvertTo-Json -Compress -Depth 4)" }
    if (-not (Wait-Job -Job $server -Timeout 10)) { throw 'Tools fixture did not finish.' }
    $serverOutput = @(Receive-Job -Job $server -ErrorAction Stop)
    if ($serverOutput -notcontains 'SERVER_PASS') { throw 'Tools fixture did not report success.' }
} catch {
    $clientFailure = $_.Exception.Message
    $serverDiagnostics = @(Receive-Job -Job $server -Keep -ErrorAction SilentlyContinue 2>&1)
    throw "$clientFailure Server diagnostics: $($serverDiagnostics -join ' | ')"
} finally {
    Stop-Job -Job $server -ErrorAction SilentlyContinue
    Remove-Job -Job $server -Force -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $testRoot) { Remove-Item -LiteralPath $testRoot -Recurse -Force }
}

Write-Output 'PASS: v9xctl delete, hash, cat, ps, kill, reg, wait-file (with pre-HELLO retry) and wait-window against protocol fixture'
