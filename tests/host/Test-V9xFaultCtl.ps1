$ErrorActionPreference = 'Stop'
# v9xctl fault handling: wait-fault (an ordinary message box is
# ignored; the fault dialog is owned by another process, as on
# Windows 9x, and found by the image name in its title; -Dismiss closes it),
# close-window, windows -Controls, and exit 23 from an agent without
# V9X_CAP_WINDOW_TOOLS, against a fixture agent that checks each request.
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
    # exec, detach, tools, process, windows, exec pid, window tools
    $allCaps = [uint32](0x7f -bor 0x800 -bor 0x40000 -bor 0x80000 -bor 0x200000 -bor 0x400000 -bor 0x01000000)
    function Send-Hello($Stream, $RequestId, [uint32]$Capabilities) {
        $payload = [Collections.Generic.List[byte]]::new()
        $payload.AddRange([BitConverter]::GetBytes([uint16]0x0100))
        $payload.AddRange([BitConverter]::GetBytes([uint16]0))
        $payload.AddRange([BitConverter]::GetBytes($Capabilities))
        $payload.AddRange([BitConverter]::GetBytes([uint32]65536))
        $payload.AddRange([BitConverter]::GetBytes([uint32]10))
        $payload.AddRange([BitConverter]::GetBytes([uint16]9869))
        $payload.AddRange([BitConverter]::GetBytes([uint16]0x0101))
        Add-V9xString -Bytes $payload -Value 'fault-fixture'
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
    function Processes([object[]]$Entries) {
        $bytes = [Collections.Generic.List[byte]]::new()
        $bytes.AddRange([byte[]](U32 $Entries.Count))
        foreach ($entry in $Entries) {
            $bytes.AddRange([byte[]]((U32 $entry[0]) + (U32 0) + (U32 1) + (U32 0) + (U32 0) + (Str $entry[1])))
        }
        return ,$bytes.ToArray()
    }
    # Each window: handle, pid, class, title.
    function Windows([object[]]$Entries) {
        $bytes = [Collections.Generic.List[byte]]::new()
        $bytes.AddRange([byte[]]((U32 $Entries.Count) + (U32 0)))
        foreach ($entry in $Entries) {
            $bytes.AddRange([byte[]]((U32 $entry[0]) + (U32 $entry[1]) + (U32 0) + (U32 10) + (U32 10) +
                                     (U32 300) + (U32 200) + (Str $entry[2]) + (Str $entry[3])))
        }
        return ,$bytes.ToArray()
    }
    # Window text: handle, pid, title, then children as (class, text, visible).
    function WindowText([uint32]$Handle, [uint32]$OwnerPid, [string]$Title, [object[]]$Children) {
        $bytes = [Collections.Generic.List[byte]]::new()
        $bytes.AddRange([byte[]]((U32 $Handle) + (U32 $OwnerPid) + (U32 $Children.Count) + (U32 0) +
                                 (Str '#32770') + (Str $Title)))
        $id = 1
        foreach ($child in $Children) {
            $bytes.AddRange([byte[]]((U32 (0x900 + $id)) + (U32 $id) + (U32 $(if ($child[2]) { 1 } else { 0 })) +
                                     (Str $child[0]) + (Str $child[1])))
            ++$id
        }
        return ,$bytes.ToArray()
    }
    function Accept-Session($Listener, [uint32]$Capabilities) {
        $client = $Listener.AcceptTcpClient()
        $stream = $client.GetStream()
        $hello = Read-V9xFrame -Stream $stream
        Send-Hello $stream $hello.RequestId $Capabilities
        return [pscustomobject]@{ Client = $client; Stream = $stream }
    }
    $agent = @(0x1000, 'C:\V9XREMOTE\V9XAGNT.EXE')
    $bench = @(0x3000, 'C:\BENCH\BENCH.EXE')
    $box = @(0x50, 0x3000, '#32770', 'BENCH')
    $fault = @(0x60, 0x1, '#32770', 'Bench')
    $faultChildren = @(@('Button', '&Close', $true), @('Button', 'De&bug', $false),
                       @('Edit', "BENCH caused an invalid page fault in`r`nmodule BENCH.EXE at 016f:004010e8.", $true),
                       @('Static', 'This program has performed an illegal operation and will be shut down.', $true))

    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $Port)
    try {
        $listener.Start()

        # wait-fault -Pid 0x3000 -Dismiss.
        $session = Accept-Session $listener $allCaps
        $r = Expect-Request $session.Stream 0x0080 ''
        Reply $session.Stream $r 0x8080 (Processes @($agent, $bench))
        $r = Expect-Request $session.Stream 0x0062 ''
        Reply $session.Stream $r 0x8062 (Windows @(, $box))
        $r = Expect-Request $session.Stream 0x0063 (Hex (U32 0x50))
        Reply $session.Stream $r 0x8063 (WindowText 0x50 0x3000 'BENCH' @(, @('Static', 'Ready.', $true)))
        # Second probe: controls can change without a title change, so the
        # ordinary box is re-read before the new fault dialog.
        $r = Expect-Request $session.Stream 0x0080 ''
        Reply $session.Stream $r 0x8080 (Processes @($agent, $bench))
        $r = Expect-Request $session.Stream 0x0062 ''
        Reply $session.Stream $r 0x8062 (Windows @($box, $fault))
        $r = Expect-Request $session.Stream 0x0063 (Hex (U32 0x50))
        Reply $session.Stream $r 0x8063 (WindowText 0x50 0x3000 'BENCH' @(, @('Static', 'Ready.', $true)))
        $r = Expect-Request $session.Stream 0x0063 (Hex (U32 0x60))
        Reply $session.Stream $r 0x8063 (WindowText 0x60 0x1 'Bench' $faultChildren)
        $r = Expect-Request $session.Stream 0x0064 (Hex (U32 0x60))
        Reply $session.Stream $r 0x8064 (U32 0x60)
        $r = Expect-Request $session.Stream 0x0062 ''
        Reply $session.Stream $r 0x8062 (Windows @(, $box))
        Start-Sleep -Milliseconds 300
        $session.Client.Dispose()

        # One selected-dialog run covers exec forwarding, PID scoping,
        # dismissal, and returning before the subsequent file wait.
        $session = Accept-Session $listener $allCaps
        $r = Read-V9xFrame -Stream $session.Stream
        if ($r.Type -ne 0x0010 -or [BitConverter]::ToUInt16($r.Payload, 2) -ne 3) {
            throw 'exec -WaitForExit did not launch detached with a PID request.'
        }
        $execComplete = (U32 0) + (U32 0) + (U32 0) + (U32 1) + (U32 0) + (U32 0) + (U32 0x60) + (U32 0x3000)
        Reply $session.Stream $r 0x8010 (U32 0)
        Reply $session.Stream $r 0x8011 $execComplete
        $r = Expect-Request $session.Stream 0x0080 ''
        Reply $session.Stream $r 0x8080 (Processes @($agent, $bench))
        $r = Expect-Request $session.Stream 0x0062 ''
        Reply $session.Stream $r 0x8062 (Windows @(@(0x80, 99, '#32770', 'Critical Error'), @(0x81, 0x3000, '#32770', 'Critical Error')))
        foreach ($entry in @(@(0x80, 99), @(0x81, 0x3000))) {
            $r = Expect-Request $session.Stream 0x0063 (Hex (U32 $entry[0]))
            Reply $session.Stream $r 0x8063 (WindowText $entry[0] $entry[1] 'Critical Error' @(, @('Edit', 'Engine failed.', $true)))
        }
        $r = Expect-Request $session.Stream 0x0064 (Hex (U32 0x81))
        Reply $session.Stream $r 0x8064 (U32 0x81)
        $r = Expect-Request $session.Stream 0x0062 ''
        Reply $session.Stream $r 0x8062 (Windows @())
        $session.Client.Dispose()

        # close-window -Title rep: one match, closed, gone on the next look.
        $session = Accept-Session $listener $allCaps
        $r = Expect-Request $session.Stream 0x0062 ''
        Reply $session.Stream $r 0x8062 (Windows @($box, @(0x70, 0x3000, 'Notepad', 'Report - Notepad')))
        $r = Expect-Request $session.Stream 0x0064 (Hex (U32 0x70))
        Reply $session.Stream $r 0x8064 (U32 0x70)
        $r = Expect-Request $session.Stream 0x0062 ''
        Reply $session.Stream $r 0x8062 (Windows @(, $box))
        Start-Sleep -Milliseconds 300
        $session.Client.Dispose()

        # Reuse the window response for both pipeline capture and JSON.
        foreach ($format in @('text', 'json')) {
            $session = Accept-Session $listener $allCaps
            $r = Expect-Request $session.Stream 0x0062 ''
            Reply $session.Stream $r 0x8062 (Windows @($box, @(0x70, 0x3000, 'Notepad', 'Report - Notepad')))
            $r = Expect-Request $session.Stream 0x0063 (Hex (U32 0x50))
            Reply $session.Stream $r 0x8063 (WindowText 0x50 0x3000 'BENCH' @(, @('Static', 'Ready.', $true)))
            $session.Client.Dispose()
        }

        # wait-fault on a 0.15 agent: refused before any request (exit 23).
        $session = Accept-Session $listener ([uint32]($allCaps -band (-bnot 0x01000000)))
        Start-Sleep -Milliseconds 600
        $session.Client.Dispose()
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
    if (-not $jsonLine) { return $null }
    return $jsonLine | ConvertFrom-Json
}

try {
    Start-Sleep -Milliseconds 400
    $fault = Invoke-FixtureClient @('wait-fault', '-Pid', '12288', '-WaitSeconds', '30', '-Dismiss') 50
    if ($fault.Outcome -ne 'Fault' -or -not $fault.Dismissed -or $fault.Fault.Title -ne 'Bench' -or
        $fault.Fault.DialogPid -ne 1 -or ($fault.Fault.Buttons -join ',') -ne 'Close' -or
        $fault.Fault.Message -notlike '*illegal operation*' -or
        $fault.Fault.Details -ne "BENCH caused an invalid page fault in`nmodule BENCH.EXE at 016f:004010e8.") {
        throw "wait-fault result mismatch: $($fault | ConvertTo-Json -Compress -Depth 4)"
    }
    $executed = Invoke-FixtureClient @('exec', '-Application', 'C:\BENCH\BENCH.EXE', '-WaitForExit',
        '-DialogTitle', 'critical error', '-Dismiss', '-WaitForFile', 'C:\NEVER.INI', '-WaitSeconds', '10') 50
    if ($executed.Success -or $executed.Outcome -ne 'Dialog' -or $executed.Fault.Details -ne 'Engine failed.' -or
        $executed.Fault.DialogPid -ne 12288 -or -not $executed.Dismissed) {
        throw 'exec selected-dialog wait did not preserve PID scoping, text, dismissal or early return.'
    }
    $closed = Invoke-FixtureClient @('close-window', '-Title', 'rep')
    if (-not $closed.Closed -or $closed.Window.Handle -ne 0x70) { throw "close-window result mismatch: $($closed | ConvertTo-Json -Compress)" }
    $text = & $clientScript windows -Title bench -Controls -Host 127.0.0.1 -Port $port | Out-String
    if ($LASTEXITCODE -ne 0 -or $text -notmatch 'Ready\.') { throw 'windows -Controls text was not on the success pipeline.' }
    $listed = Invoke-FixtureClient @('windows', '-Title', 'bench', '-Controls')
    if ($listed.Count -ne 1 -or $listed.Windows[0].Controls[0].Text -ne 'Ready.') {
        throw "windows -Controls result mismatch: $($listed | ConvertTo-Json -Compress -Depth 4)"
    }
    Invoke-FixtureClient @('wait-fault', '-Name', 'BENCH.EXE', '-WaitSeconds', '10') 23 | Out-Null
    Invoke-FixtureClient @('windows', '-Dismiss') 10 | Out-Null
    Invoke-FixtureClient @('wait-fault', '-DialogTitle', 'Critical Error') 10 | Out-Null
    Invoke-FixtureClient @('exec', '-Application', 'C:\BENCH.EXE', '-DialogTitle', 'Critical Error') 10 | Out-Null
    if (-not (Wait-Job -Job $server -Timeout 10)) { throw 'Fault fixture did not finish.' }
    $serverOutput = @(Receive-Job -Job $server -ErrorAction Stop)
    if ($serverOutput -notcontains 'SERVER_PASS') { throw 'Fault fixture did not report success.' }
} catch {
    $clientFailure = $_.Exception.Message
    $serverDiagnostics = @(Receive-Job -Job $server -Keep -ErrorAction SilentlyContinue 2>&1)
    throw "$clientFailure Server diagnostics: $($serverDiagnostics -join ' | ')"
} finally {
    Stop-Job -Job $server -ErrorAction SilentlyContinue
    Remove-Job -Job $server -Force -ErrorAction SilentlyContinue
}

Write-Output 'PASS: v9xctl wait-fault (-Dismiss, dialog owned by another process), close-window and windows -Controls against protocol fixture'
