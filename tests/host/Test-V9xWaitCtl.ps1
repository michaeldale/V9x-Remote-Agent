$ErrorActionPreference = 'Stop'
# v9xctl waits for long jobs (0.14.0): wait-process (with a connection that
# drops mid-wait and is reconnected), wait-file -Match, cat -Follow -Until,
# and exec -WaitForExit with and without the agent's pid report, against a
# fixture agent that checks each request.
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$protocolScript = Join-Path $repoRoot 'scripts\V9xProtocol.ps1'
$clientScript = Join-Path $repoRoot 'scripts\v9xctl.ps1'

$portProbe = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$portProbe.Start()
$port = ([Net.IPEndPoint]$portProbe.LocalEndpoint).Port
$portProbe.Stop()

$server = Start-Job -ArgumentList $port, $protocolScript -ScriptBlock {
    param($Port, $ProtocolScript)
    $ErrorActionPreference = 'Stop'
    . $ProtocolScript
    # exec, detach, file read, tools, process, windows, exec pid
    $allCaps = [uint32](0x7f -bor 0x800 -bor 0x40000 -bor 0x80000 -bor 0x200000 -bor 0x400000)
    function Send-Hello($Stream, $RequestId, [uint32]$Capabilities) {
        $payload = [Collections.Generic.List[byte]]::new()
        $payload.AddRange([BitConverter]::GetBytes([uint16]0x0100))
        $payload.AddRange([BitConverter]::GetBytes([uint16]0))
        $payload.AddRange([BitConverter]::GetBytes($Capabilities))
        $payload.AddRange([BitConverter]::GetBytes([uint32]65536))
        $payload.AddRange([BitConverter]::GetBytes([uint32]10))
        $payload.AddRange([BitConverter]::GetBytes([uint16]9869))
        $payload.AddRange([BitConverter]::GetBytes([uint16]0x0101))
        Add-V9xString -Bytes $payload -Value 'wait-fixture'
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
        if ($null -ne $Hex -and $actual -ne $Hex) { throw ('Request 0x{0:X4} payload {1}, expected {2}.' -f $Type, $actual, $Hex) }
        return $request
    }
    function Hex([byte[]]$Bytes) { ($Bytes | ForEach-Object { $_.ToString('X2') }) -join '' }
    function Str([string]$Value) {
        $list = [Collections.Generic.List[byte]]::new(); Add-V9xString -Bytes $list -Value $Value; $list.ToArray()
    }
    function U32([uint32]$Value) { [BitConverter]::GetBytes($Value) }
    function Stat([bool]$Exists, [uint32]$Size) {
        $stat = [byte[]]::new(16)
        if ($Exists) { $stat[0] = 1; [Array]::Copy((U32 $Size), 0, $stat, 4, 4) }
        return ,$stat
    }
    function Processes([object[]]$Entries) {
        $bytes = [Collections.Generic.List[byte]]::new()
        $bytes.AddRange([byte[]](U32 $Entries.Count))
        foreach ($entry in $Entries) {
            $bytes.AddRange([byte[]]((U32 $entry[0]) + (U32 0) + (U32 1) + (U32 0) + (U32 0) + (Str $entry[1])))
        }
        return ,$bytes.ToArray()
    }
    # FILE_READ_TEXT the way the agent answers it, over $Content.
    function Read-TextAnswer([byte[]]$Request, [string]$Content) {
        $tail = [BitConverter]::ToUInt32($Request, 0)
        $max = [BitConverter]::ToUInt32($Request, 4)
        $offset = 8
        $match = Read-V9xAnsiString -Bytes $Request -Offset ([ref]$offset)
        $lines = @($Content.Split("`n") | Select-Object -SkipLast 1)
        if ($Content.Length -and -not $Content.EndsWith("`n")) { $lines = @($Content.Split("`n")) }
        $matched = @($lines | Where-Object { -not $match -or $_.IndexOf($match, [StringComparison]::OrdinalIgnoreCase) -ge 0 })
        $chosen = if ($tail -gt 0 -and $matched.Count -gt $tail) { $matched[($matched.Count - $tail)..($matched.Count - 1)] } else { $matched }
        $text = ''
        $truncated = 0
        foreach ($line in $chosen) {
            if ($text.Length + $line.Length + 1 -gt $max) { $truncated = 1; break }
            $text += "$line`n"
        }
        $returned = @($text.Split("`n") | Select-Object -SkipLast 1).Count
        $bytes = [Text.Encoding]::ASCII.GetBytes($text)
        return ,[byte[]]((U32 $Content.Length) + (U32 $lines.Count) + (U32 $matched.Count) + (U32 $returned) +
                         (U32 $truncated) + (U32 $bytes.Length) + $bytes)
    }
    function Accept-Session($Listener, [uint32]$Capabilities) {
        $client = $Listener.AcceptTcpClient()
        $stream = $client.GetStream()
        $hello = Read-V9xFrame -Stream $stream
        Send-Hello $stream $hello.RequestId $Capabilities
        return [pscustomobject]@{ Client = $client; Stream = $stream }
    }
    function Exec-Reply($Stream, $Request, [bool]$WithPid) {
        Reply $Stream $Request 0x8010 (U32 0)
        $complete = [byte[]]::new($(if ($WithPid) { 32 } else { 28 }))
        [Array]::Copy((U32 0x40), 0, $complete, 24, 4)
        if ($WithPid) { [Array]::Copy((U32 0x3000), 0, $complete, 28, 4) }
        Reply $Stream $Request 0x8011 $complete
    }
    $agent = @(0x1000, 'C:\V9XREMOTE\V9XAGNT.EXE')
    $notepad = @(0x2000, 'C:\WINDOWS\NOTEPAD.EXE')
    $bench = @(0x3000, 'C:\BENCH\BENCH.EXE')

    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $Port)
    try {
        $listener.Start()

        # wait-process -Pid: running twice, then the connection drops; on the
        # reconnected session it is gone.
        $session = Accept-Session $listener $allCaps
        foreach ($round in 1..2) {
            $r = Expect-Request $session.Stream 0x0080 ''
            Reply $session.Stream $r 0x8080 (Processes @($agent, $notepad))
        }
        $session.Client.Dispose()
        $session = Accept-Session $listener $allCaps
        $r = Expect-Request $session.Stream 0x0080 ''
        Reply $session.Stream $r 0x8080 (Processes @(, $agent))
        Start-Sleep -Milliseconds 300
        $session.Client.Dispose()

        # wait-file -Match: missing, then present without the line, then with it.
        $session = Accept-Session $listener $allCaps
        $readHex = Hex ((U32 1) + (U32 4096) + (Str 'complete=yes') + (Str 'C:\R.INI'))
        $r = Expect-Request $session.Stream 0x0020 (Hex (Str 'C:\R.INI'))
        Reply $session.Stream $r 0x8020 (Stat $false 0)
        foreach ($content in @("[Run]`r`nComplete=no`r`n", "[Run]`r`nComplete=yes`r`n")) {
            $r = Expect-Request $session.Stream 0x0020 (Hex (Str 'C:\R.INI'))
            Reply $session.Stream $r 0x8020 (Stat $true $content.Length)
            $r = Expect-Request $session.Stream 0x002A $readHex
            Reply $session.Stream $r 0x802A (Read-TextAnswer $r.Payload $content.Replace("`r", ''))
        }
        Start-Sleep -Milliseconds 300
        $session.Client.Dispose()

        # cat -Follow -Until DONE: two lines exist; then two more land, the
        # last half written ("DO") until a later stat, when it is "DONE".
        $session = Accept-Session $listener $allCaps
        $content = "line one`nline two`n"
        $stats = 0
        try {
            for (;;) {
                $r = Read-V9xFrame -Stream $session.Stream
                if ($r.Type -eq 0x0020) {
                    ++$stats
                    if ($stats -eq 3) { $content += "line three`nDO" }
                    if ($stats -eq 6) { $content += "NE`n" }
                    Reply $session.Stream $r 0x8020 (Stat $true $content.Length)
                } elseif ($r.Type -eq 0x002A) {
                    Reply $session.Stream $r 0x802A (Read-TextAnswer $r.Payload $content)
                } else { throw ('cat -Follow sent 0x{0:X4}.' -f $r.Type) }
            }
        } catch [IO.IOException] { } catch [Management.Automation.MethodInvocationException] { }
        $session.Client.Dispose()

        # exec -WaitForExit on an agent that reports the pid: options 0x0003.
        $execHex = Hex ([byte[]](0, 0) + [BitConverter]::GetBytes([uint16]3) + (U32 60000) + (U32 262144) +
                        (U32 262144) + (Str 'C:\BENCH\BENCH.EXE') + (Str '') + (Str ''))
        $session = Accept-Session $listener $allCaps
        $r = Expect-Request $session.Stream 0x0010 $execHex
        Exec-Reply $session.Stream $r $true
        $r = Expect-Request $session.Stream 0x0080 ''
        Reply $session.Stream $r 0x8080 (Processes @($agent, $bench))
        $r = Expect-Request $session.Stream 0x0080 ''
        Reply $session.Stream $r 0x8080 (Processes @(, $agent))
        Start-Sleep -Milliseconds 300
        $session.Client.Dispose()

        # ... and on a 0.13 agent: plain detach, then wait on the image name.
        $session = Accept-Session $listener ([uint32]($allCaps -band (-bnot 0x400000)))
        $r = Expect-Request $session.Stream 0x0010 ($execHex.Substring(0, 4) + '0100' + $execHex.Substring(8))
        Exec-Reply $session.Stream $r $false
        $r = Expect-Request $session.Stream 0x0080 ''
        Reply $session.Stream $r 0x8080 (Processes @($agent, $bench))
        $r = Expect-Request $session.Stream 0x0080 ''
        Reply $session.Stream $r 0x8080 (Processes @(, $agent))
        Start-Sleep -Milliseconds 300
        $session.Client.Dispose()
        'SERVER_PASS'
    } finally {
        $listener.Stop()
    }
}

function Invoke-FixtureClient([string[]]$Arguments, [int]$ExpectedExit = 0, [switch]$Raw) {
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
    if ($Raw) { return @($output | Where-Object { $_ -is [string] }) }
    if ($ExpectedExit -ne 0) { return }
    $jsonLine = $output | Where-Object { $_ -is [string] -and $_.StartsWith('{') } | Select-Object -Last 1
    if (-not $jsonLine) { throw "v9xctl $($Arguments[0]) returned no JSON: $($output -join ' ')" }
    return $jsonLine | ConvertFrom-Json
}

try {
    Start-Sleep -Milliseconds 400
    $process = Invoke-FixtureClient @('wait-process', '-Pid', '8192', '-WaitSeconds', '30')
    if (-not $process.WasRunning -or ($process.Exited -join ',') -ne 'NOTEPAD.EXE') {
        throw "wait-process result mismatch: $($process | ConvertTo-Json -Compress)"
    }
    $file = Invoke-FixtureClient @('wait-file', '-Path', 'C:\R.INI', '-Match', 'complete=yes', '-WaitSeconds', '10')
    if ($file.Line -ne 'Complete=yes' -or -not $file.Exists) { throw "wait-file -Match result mismatch: $($file | ConvertTo-Json -Compress)" }
    $followed = @(Invoke-FixtureClient @('cat', '-Path', 'C:\B.LOG', '-Follow', '-Until', 'done', '-WaitSeconds', '20') -Raw)
    if (($followed -join '|') -ne 'line three|DONE') { throw "cat -Follow printed '$($followed -join '|')'." }
    $exec = Invoke-FixtureClient @('exec', '-Application', 'C:\BENCH\BENCH.EXE', '-WaitForExit', '-WaitSeconds', '30')
    if ($exec.Pid -ne 0x3000 -or -not $exec.WaitedForExit -or -not $exec.WasRunning -or -not $exec.Detached) {
        throw "exec -WaitForExit result mismatch: $($exec | ConvertTo-Json -Compress)"
    }
    $old = Invoke-FixtureClient @('exec', '-Application', 'C:\BENCH\BENCH.EXE', '-WaitForExit', '-WaitSeconds', '30')
    if ($null -ne $old.Pid -or -not $old.WaitedForExit -or -not $old.WasRunning) {
        throw "exec -WaitForExit (no pid) result mismatch: $($old | ConvertTo-Json -Compress)"
    }
    Invoke-FixtureClient @('wait-process', '-WaitSeconds', '10') 10 | Out-Null
    if (-not (Wait-Job -Job $server -Timeout 10)) { throw 'Wait fixture did not finish.' }
    $serverOutput = @(Receive-Job -Job $server -ErrorAction Stop)
    if ($serverOutput -notcontains 'SERVER_PASS') { throw 'Wait fixture did not report success.' }
} catch {
    $clientFailure = $_.Exception.Message
    $serverDiagnostics = @(Receive-Job -Job $server -Keep -ErrorAction SilentlyContinue 2>&1)
    throw "$clientFailure Server diagnostics: $($serverDiagnostics -join ' | ')"
} finally {
    Stop-Job -Job $server -ErrorAction SilentlyContinue
    Remove-Job -Job $server -Force -ErrorAction SilentlyContinue
}

Write-Output 'PASS: v9xctl wait-process (with reconnect), wait-file -Match, cat -Follow -Until and exec -WaitForExit against protocol fixture'
