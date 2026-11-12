$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$ctl = Join-Path $repoRoot 'scripts\v9xctl.ps1'
$protocol = Join-Path $repoRoot 'scripts\V9xProtocol.ps1'
$probe = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$probe.Start(); $port = ([Net.IPEndPoint]$probe.LocalEndpoint).Port; $probe.Stop()

$server = Start-Job -ArgumentList $port, $protocol -ScriptBlock {
    param($Port, $Protocol)
    $ErrorActionPreference = 'Stop'
    . $Protocol
    function Reply($s, $r, [uint16]$type, [byte[]]$payload) {
        $bytes = New-V9xFrameBytes -Type $type -RequestId $r.RequestId -Payload $payload
        $s.Write($bytes, 0, $bytes.Length)
    }
    function Hello($listener, [uint32]$caps = 0x02010083, [uint32]$boot = 1, [string]$job = '') {
        $c = $listener.AcceptTcpClient(); $s = $c.GetStream(); $s.ReadTimeout = 10000
        $r = Read-V9xFrame -Stream $s
        if ($r.Type -ne 1) { throw 'Expected HELLO.' }
        $p = [Collections.Generic.List[byte]]::new()
        $p.AddRange([BitConverter]::GetBytes([uint16]0x100)); $p.AddRange([byte[]](0, 0))
        foreach ($v in @($caps, 65536, $boot)) { $p.AddRange([BitConverter]::GetBytes([uint32]$v)) }
        $p.AddRange([BitConverter]::GetBytes([uint16]9869)); $p.AddRange([BitConverter]::GetBytes([uint16]0x101))
        Add-V9xString -Bytes $p -Value 'operator-fixture'; Add-V9xString -Bytes $p -Value $job
        $p.AddRange([byte[]]::new(16))
        Reply $s $r 0x8001 $p.ToArray()
        return $c
    }
    function Desktop($s, [int]$flags, [int]$result, [int]$boot = 1) {
        $r = Read-V9xFrame -Stream $s
        if ($r.Type -ne 0x65 -or $r.Payload.Length -ne 8 -or
            [BitConverter]::ToUInt32($r.Payload, 0) -ne $flags -or
            [BitConverter]::ToUInt32($r.Payload, 4) -ne 5000) { throw 'Invalid desktop wait request.' }
        $p = [Collections.Generic.List[byte]]::new()
        foreach ($v in @($result, $boot, 1024, 768, 0, 100)) { $p.AddRange([BitConverter]::GetBytes([uint32]$v)) }
        Reply $s $r 0x8065 $p.ToArray()
    }
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $Port)
    try {
        $listener.Start()
        foreach ($case in @(@(1, 3), @(0, 1), @(1, 2))) {
            $c = Hello $listener
            try { Desktop $c.GetStream() $case[0] $case[1] } finally { $c.Dispose() }
        }
        # An old agent must receive no power command at all.
        $c = Hello $listener 0x83
        try { if ($c.GetStream().ReadByte() -ne -1) { throw 'Sent power request before capability refusal.' } }
        finally { $c.Dispose() }
        # Reset proof must be established before desktop cancellation.
        $c = Hello $listener
        try {
            $s = $c.GetStream(); $r = Read-V9xFrame -Stream $s
            if ($r.Type -ne 0x43) { throw 'Expected RESET.' }
            $off = 0; $job = Read-V9xAnsiString -Bytes $r.Payload -Offset ([ref]$off)
            $p = [Collections.Generic.List[byte]]::new(); $p.AddRange([BitConverter]::GetBytes([uint32]1))
            Add-V9xString -Bytes $p -Value $job
            Reply $s $r 0x8043 $p.ToArray()
        } finally { $c.Dispose() }
        $c = Hello $listener 0x02010083 2 $job
        try { Desktop $c.GetStream() 1 3 2 } finally { $c.Dispose() }
        # The helper's -Dismiss should only send a desktop wait, no registry work.
        $c = Hello $listener
        try { Desktop $c.GetStream() 1 3 } finally { $c.Dispose() }
        # Verify the simple positional paths on the wire (aliases included).
        foreach ($type in @(0x20, 0x21)) {
            $c = Hello $listener 0x63
            try {
                $s = $c.GetStream(); $r = Read-V9xFrame -Stream $s
                $off = 0; $path = Read-V9xAnsiString -Bytes $r.Payload -Offset ([ref]$off)
                if ($r.Type -ne $type -or $path -ne 'C:\TEST' -or $off -ne $r.Payload.Length) { throw 'Positional path mismatch.' }
                $p = if ($type -eq 0x20) { [byte[]]::new(16) } else { [byte[]](0, 0, 0, 0) }
                Reply $s $r ($type -bor 0x8000) $p
            } finally { $c.Dispose() }
        }
        # Registry set/delete share one wire contract, returning old data.
        foreach ($op in @(2, 1, 3)) {
            $c = Hello $listener 0x3fffffff
            try {
                $s = $c.GetStream(); $r = Read-V9xFrame -Stream $s
                $at = 8; $key = Read-V9xAnsiString -Bytes $r.Payload -Offset ([ref]$at)
                $name = Read-V9xAnsiString -Bytes $r.Payload -Offset ([ref]$at)
                $size = [BitConverter]::ToUInt32($r.Payload, $at); $at += 4
                if ($r.Type -ne 0x91 -or [BitConverter]::ToUInt32($r.Payload, 0) -ne 0x80000001L -or
                    [BitConverter]::ToUInt32($r.Payload, 4) -ne $op -or $key -ne 'Software\Test' -or
                    $name -ne 'Value' -or $at + $size -ne $r.Payload.Length) { throw 'Registry write mismatch.' }
                if ($op -eq 2 -and [BitConverter]::ToUInt32($r.Payload, $at) -ne 3) { throw 'DWORD mismatch.' }
                if ($op -eq 1 -and [Text.Encoding]::ASCII.GetString($r.Payload, $at, $size) -ne "640,480`0") { throw 'String mismatch.' }
                if ($op -eq 3 -and $size -ne 0) { throw 'Deletion has data.' }
                Reply $s $r 0x8091 ([byte[]](1,0,0,0,4,0,0,0,4,0,0,0,2,0,0,0))
            } finally { $c.Dispose() }
        }
        # Focus selection and reporting happen in one guest request.
        $c = Hello $listener 0x3fffffff
        try {
            $s = $c.GetStream(); $r = Read-V9xFrame -Stream $s; $at = 0
            $title = Read-V9xAnsiString -Bytes $r.Payload -Offset ([ref]$at)
            if ($r.Type -ne 0x66 -or $title -ne '[Game]' -or
                [Convert]::ToBase64String($r.Payload[$at..($r.Payload.Length-1)]) -ne [Convert]::ToBase64String([byte[]](1,0,5,1,0,113))) { throw 'Targeted input mismatch.' }
            $p = [Collections.Generic.List[byte]]::new()
            foreach ($v in @(1,10,20)) { $p.AddRange([BitConverter]::GetBytes([uint32]$v)) }
            foreach ($title in @('[Game]', '[Game]', 'Desktop')) {
                $p.AddRange([BitConverter]::GetBytes([uint32]99)); $p.AddRange([BitConverter]::GetBytes([uint32]123))
                Add-V9xString -Bytes $p -Value $title
            }
            Reply $s $r 0x8066 $p.ToArray()
        } finally { $c.Dispose() }
        # A guest-side process wait replaces repeated PROCESS_LIST probes.
        $c = Hello $listener 0x3fffffff
        try {
            $s = $c.GetStream(); $r = Read-V9xFrame -Stream $s
            if ($r.Type -ne 0x82 -or [BitConverter]::ToUInt32($r.Payload, 0) -ne 123 -or
                [BitConverter]::ToUInt32($r.Payload, 4) -gt 5000) { throw 'Process wait mismatch.' }
            $p = [Collections.Generic.List[byte]]::new(); $p.AddRange([byte[]](3,0,0,0,10,0,0,0))
            Add-V9xString -Bytes $p -Value 'GAME.EXE'; Reply $s $r 0x8082 $p.ToArray()
        } finally { $c.Dispose() }
        # Kill -> confirm exit -> restore, never restore while it still runs.
        $c = Hello $listener 0x3fffffff
        try {
            $s = $c.GetStream(); $r = Read-V9xFrame -Stream $s
            if ($r.Type -ne 0x80) { throw 'Expected process list before kill.' }
            $p = [Collections.Generic.List[byte]]::new()
            $p.AddRange([byte[]](1,0,0,0)); $p.AddRange([BitConverter]::GetBytes([uint32]123)); $p.AddRange([byte[]]::new(16))
            Add-V9xString -Bytes $p -Value 'C:\GAME.EXE'; Reply $s $r 0x8080 $p.ToArray()
            $r = Read-V9xFrame -Stream $s; if ($r.Type -ne 0x81) { throw 'Expected kill.' }
            Reply $s $r 0x8081 ([byte[]](123,0,0,0,0,0,0,0))
            $r = Read-V9xFrame -Stream $s; if ($r.Type -ne 0x82) { throw 'Expected exit wait before restore.' }
            Reply $s $r 0x8082 ([byte[]](2,0,0,0,0,0,0,0,0,0))
            $r = Read-V9xFrame -Stream $s
            if ($r.Type -ne 0x67 -or [BitConverter]::ToUInt32($r.Payload, 0) -ne 1) { throw 'Expected display restore.' }
            $p = [Collections.Generic.List[byte]]::new()
            foreach ($v in @(7,0,1024,768,32,60,1024,768,32,60)) { $p.AddRange([BitConverter]::GetBytes([uint32]$v)) }
            Reply $s $r 0x8067 $p.ToArray()
        } finally { $c.Dispose() }
        # A failed restore after a completed kill keeps the kill result.
        $c = Hello $listener 0x3fffffff
        try {
            $s = $c.GetStream(); $r = Read-V9xFrame -Stream $s
            if ($r.Type -ne 0x80) { throw 'Expected process list before kill.' }
            $p = [Collections.Generic.List[byte]]::new()
            $p.AddRange([byte[]](1,0,0,0)); $p.AddRange([BitConverter]::GetBytes([uint32]123)); $p.AddRange([byte[]]::new(16))
            Add-V9xString -Bytes $p -Value 'C:\GAME.EXE'; Reply $s $r 0x8080 $p.ToArray()
            $r = Read-V9xFrame -Stream $s; if ($r.Type -ne 0x81) { throw 'Expected kill by pid.' }
            Reply $s $r 0x8081 ([byte[]](123,0,0,0,0,0,0,0))
            $r = Read-V9xFrame -Stream $s; if ($r.Type -ne 0x82) { throw 'Expected exit wait.' }
            Reply $s $r 0x8082 ([byte[]](3,0,0,0,0,0,0,0,0,0))
            $r = Read-V9xFrame -Stream $s; if ($r.Type -ne 0x67) { throw 'Expected display restore.' }
            $p = [Collections.Generic.List[byte]]::new()
            $p.AddRange([BitConverter]::GetBytes([uint32]5)); $p.AddRange([BitConverter]::GetBytes([uint32]0))
            Add-V9xString -Bytes $p -Value 'ChangeDisplaySettings failed'
            Reply $s $r 0x8fff $p.ToArray()
        } finally { $c.Dispose() }
        'SERVER_PASS'
    } finally { $listener.Stop() }
}

function Run([string[]]$Arguments, [int]$Expected = 0, [string]$Script = $ctl) {
    $saved = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try {
        $output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Script @Arguments -Host 127.0.0.1 -Port $port -Json 2>&1)
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $saved }
    if ($code -ne $Expected) { throw "$($Arguments -join ' '): expected exit $Expected, got ${code}: $($output -join ' ')" }
    if ($Expected -eq 0 -or $Expected -eq 51) {
        return ($output | Where-Object { $_ -is [string] -and $_.StartsWith('{') } | Select-Object -Last 1 | ConvertFrom-Json)
    }
}
try {
    $r = Run @('wait-desktop', '-DismissLogon', '-WaitSeconds', '5')
    if (-not $r.DesktopReady -or -not $r.LogonDismissed -or $r.ScreenWidth -ne 1024) { throw 'Dismissal result mismatch.' }
    $r = Run @('wait-desktop', '-WaitSeconds', '5')
    if (-not $r.DesktopReady -or $r.LogonDismissed) { throw 'Plain wait sent cancellation.' }
    Run @('wait-desktop', '-DismissLogon', '-WaitSeconds', '5') 44
    Run @('reboot', '-DismissLogon', '-WaitSeconds', '5') 23
    $r = Run @('reset', '-DismissLogon', '-JobId', 'operator-reset', '-WaitSeconds', '5')
    if (-not $r.Reconnected -or -not $r.LogonDismissed -or -not $r.DesktopReady -or $r.BootCounter -ne 2) { throw 'Reset result mismatch.' }
    $r = Run @('-Dismiss', '-DesktopTimeoutSeconds', '5') 0 (Join-Path $repoRoot 'scripts\set-autologon.ps1')
    if (-not $r.DialogCleared -or -not $r.LogonDismissed) { throw 'Helper result mismatch.' }
    Run @('stat', 'C:\TEST') | Out-Null
    Run @('ls', 'C:\TEST') | Out-Null
    foreach ($valueArgs in @(@('-Set','-Dword','3'), @('-Set','-String','640,480'), @('-DeleteValue'))) {
        $r = Run (@('reg','-Key','HKCU\Software\Test','-Name','Value') + $valueArgs)
        if (-not $r.Previous.Exists -or $r.Previous.Data -ne 2) { throw 'Previous registry value mismatch.' }
    }
    $r = Run @('input','-Window','[Game]','-Sequence','type q')
    if ($r.KeyboardWindow.Title -ne '[Game]' -or $r.ForegroundAfter.Title -ne 'Desktop') { throw 'Input foreground report mismatch.' }
    $r = Run @('wait-process','-Pid','123','-WaitSeconds','5')
    if (-not $r.WasRunning -or $r.Exited[0] -ne 'GAME.EXE') { throw 'Native wait result mismatch.' }
    $r = Run @('kill','-Pid','123','-RestoreDisplay')
    if (-not $r.Display.Restored -or $r.Display.ModeDiffers) { throw 'Kill/restore result mismatch.' }
    $r = Run @('kill','-Pid','123','-RestoreDisplay') 51
    if ($r.Pid -ne 123 -or $r.Display.Restored -or $r.Display.Error -notmatch 'ChangeDisplaySettings') { throw 'Kill result lost on restore failure.' }
    # All of these must fail before connecting.
    Run @('stat', 'C:\TEST', '-Path', 'C:\OTHER') 10
    Run @('get', 'C:\TEST', '-Source', 'C:\OTHER', '-Destination', 'unused') 10
    Run @('input', '-Sequence', 'key ENTER', '-DismissLogon') 10
    Run @('shutdown', '-DismissLogon') 10
    Run @('reg','-Set','-Key','HKCU\Software\Test','-Name','Value','-String','x','-Dword','1') 10
    if (-not (Wait-Job $server -Timeout 10)) { throw 'Operator fixture did not finish.' }
    if (@(Receive-Job $server -ErrorAction Stop) -notcontains 'SERVER_PASS') { throw 'Operator fixture failed.' }
} catch {
    $problem = $_.Exception.Message
    $diagnostic = @(Receive-Job $server -Keep -ErrorAction SilentlyContinue 2>&1)
    throw "$problem Server: $($diagnostic -join ' ')"
} finally {
    Stop-Job $server -ErrorAction SilentlyContinue
    Remove-Job $server -Force -ErrorAction SilentlyContinue
}
Write-Output 'PASS: operator commands: logon, registry, targeted input, process wait, display restore and positional paths'
