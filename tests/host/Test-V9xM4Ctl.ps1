$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$protocolScript = Join-Path $repoRoot 'scripts\V9xProtocol.ps1'
$clientScript = Join-Path $repoRoot 'scripts\v9xctl.ps1'
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('v9x-m4-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$screenshotPath = Join-Path $testRoot 'desktop.bmp'
$resumeToken = 'fixture-resume-42'

$portProbe = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$portProbe.Start()
$port = ([Net.IPEndPoint]$portProbe.LocalEndpoint).Port
$portProbe.Stop()

$server = Start-Job -ArgumentList $port, $protocolScript, $resumeToken -ScriptBlock {
    param($Port, $ProtocolScript, $ResumeToken)
    $ErrorActionPreference = 'Stop'
    . $ProtocolScript

    function Send-Hello($Stream, [uint32]$RequestId, [uint32]$BootCounter,
                        [string]$PendingJob, [bool]$DesktopReady) {
        $payload = [Collections.Generic.List[byte]]::new()
        $payload.AddRange([BitConverter]::GetBytes([uint16]0x0100))
        $payload.AddRange([BitConverter]::GetBytes([uint16]0))
        $payload.AddRange([BitConverter]::GetBytes([uint32]1023))
        $payload.AddRange([BitConverter]::GetBytes([uint32]65536))
        $payload.AddRange([BitConverter]::GetBytes($BootCounter))
        $payload.AddRange([BitConverter]::GetBytes([uint16]9869))
        $payload.AddRange([BitConverter]::GetBytes([uint16]0x0101))
        Add-V9xString -Bytes $payload -Value 'fixture-m4'
        Add-V9xString -Bytes $payload -Value $PendingJob
        $payload.Add([byte][bool]$DesktopReady); $payload.Add(0)
        $payload.AddRange([BitConverter]::GetBytes([uint16]0))
        $payload.AddRange([BitConverter]::GetBytes([uint32]800))
        $payload.AddRange([BitConverter]::GetBytes([uint32]600))
        $payload.AddRange([BitConverter]::GetBytes([uint32]16))
        $frame = New-V9xFrameBytes -Type 0x8001 -RequestId $RequestId -Payload $payload.ToArray()
        $Stream.Write($frame, 0, $frame.Length)
    }

    function Send-Info($Stream, [uint32]$RequestId, [bool]$DesktopReady) {
        $payload = [Collections.Generic.List[byte]]::new()
        $payload.AddRange([BitConverter]::GetBytes([uint32]21))
        $payload.AddRange([BitConverter]::GetBytes([uint32]1234))
        $payload.AddRange([BitConverter]::GetBytes([uint32]1023))
        $payload.AddRange([BitConverter]::GetBytes([uint16]9869))
        $payload.AddRange([BitConverter]::GetBytes([uint16]0x0101))
        $payload.AddRange([BitConverter]::GetBytes([uint32]3221228036))
        foreach ($value in @('0.4.0','fixture-m4','WIN98','Windows 4.10',
                              'C:\WINDOWS\SYSTEM','C:\WINDOWS','C:\')) {
            Add-V9xString -Bytes $payload -Value $value
        }
        Add-V9xString -Bytes $payload -Value $ResumeToken
        $payload.Add([byte][bool]$DesktopReady); $payload.Add(0)
        $payload.AddRange([BitConverter]::GetBytes([uint16]0))
        $payload.AddRange([BitConverter]::GetBytes([uint32]800))
        $payload.AddRange([BitConverter]::GetBytes([uint32]600))
        $payload.AddRange([BitConverter]::GetBytes([uint32]16))
        $frame = New-V9xFrameBytes -Type 0x8030 -RequestId $RequestId -Payload $payload.ToArray()
        $Stream.Write($frame, 0, $frame.Length)
    }

    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $Port)
    try {
        $listener.Start()

        $client = $listener.AcceptTcpClient()
        try {
            $stream = $client.GetStream()
            $hello = Read-V9xFrame -Stream $stream
            Send-Hello $stream $hello.RequestId 20 '' $false
            $power = Read-V9xFrame -Stream $stream
            if ($power.Type -ne 0x0040) { throw 'Expected REBOOT_REQUEST.' }
            $tokenOffset = 0
            $token = Read-V9xString -Bytes $power.Payload -Offset ([ref]$tokenOffset)
            if ($token -ne $ResumeToken) { throw 'Reboot token mismatch.' }
            $accepted = [Collections.Generic.List[byte]]::new()
            $accepted.AddRange([BitConverter]::GetBytes([uint32]20))
            Add-V9xString -Bytes $accepted -Value $token
            $frame = New-V9xFrameBytes -Type 0x8040 -RequestId $power.RequestId -Payload $accepted.ToArray()
            $stream.Write($frame, 0, $frame.Length)
            Start-Sleep -Milliseconds 250
        } finally { $client.Dispose() }

        $client = $listener.AcceptTcpClient()
        try {
            $stream = $client.GetStream()
            $hello = Read-V9xFrame -Stream $stream
            Send-Hello $stream $hello.RequestId 21 $ResumeToken $false
            Start-Sleep -Milliseconds 750
        } finally { $client.Dispose() }

        $client = $listener.AcceptTcpClient()
        try {
            $stream = $client.GetStream()
            $hello = Read-V9xFrame -Stream $stream
            Send-Hello $stream $hello.RequestId 21 $ResumeToken $false
            $info = Read-V9xFrame -Stream $stream
            if ($info.Type -ne 0x0030) { throw 'Expected first INFO_REQUEST.' }
            Send-Info $stream $info.RequestId $false
            $info = Read-V9xFrame -Stream $stream
            if ($info.Type -ne 0x0030) { throw 'Expected second INFO_REQUEST.' }
            Send-Info $stream $info.RequestId $true
            Start-Sleep -Milliseconds 500
        } finally { $client.Dispose() }

        $bitmap = [byte[]]::new(64)
        $bitmap[0] = [byte][char]'B'; $bitmap[1] = [byte][char]'M'
        for ($index = 2; $index -lt $bitmap.Length; ++$index) { $bitmap[$index] = [byte]$index }
        [uint32]$crc = [uint32]::MaxValue
        $crc = Update-V9xCrc32 -Crc $crc -Bytes $bitmap
        $crc = [uint32]($crc -bxor [uint32]::MaxValue)
        $client = $listener.AcceptTcpClient()
        try {
            $stream = $client.GetStream()
            $hello = Read-V9xFrame -Stream $stream
            Send-Hello $stream $hello.RequestId 21 $ResumeToken $true
            $screen = Read-V9xFrame -Stream $stream
            if ($screen.Type -ne 0x0050) { throw 'Expected SCREENSHOT_REQUEST.' }
            $pathOffset = 0
            $guestPath = Read-V9xString -Bytes $screen.Payload -Offset ([ref]$pathOffset)
            $screenResponse = [Collections.Generic.List[byte]]::new()
            $screenResponse.AddRange([BitConverter]::GetBytes([uint32]800))
            $screenResponse.AddRange([BitConverter]::GetBytes([uint32]600))
            $screenResponse.AddRange([BitConverter]::GetBytes([uint32]16))
            $screenResponse.AddRange([BitConverter]::GetBytes([uint32]$bitmap.Length))
            $screenResponse.AddRange([BitConverter]::GetBytes($crc))
            Add-V9xString -Bytes $screenResponse -Value $guestPath
            $frame = New-V9xFrameBytes -Type 0x8050 -RequestId $screen.RequestId -Payload $screenResponse.ToArray()
            $stream.Write($frame, 0, $frame.Length)
            $read = Read-V9xFrame -Stream $stream
            if ($read.Type -ne 0x0026) { throw 'Expected screenshot FILE_OPEN_READ.' }
            $chunk = [byte[]]::new($bitmap.Length + 4)
            [Array]::Copy([BitConverter]::GetBytes([uint32]0), 0, $chunk, 0, 4)
            [Array]::Copy($bitmap, 0, $chunk, 4, $bitmap.Length)
            $frame = New-V9xFrameBytes -Type 0x9026 -RequestId $read.RequestId -Payload $chunk
            $stream.Write($frame, 0, $frame.Length)
            $complete = [byte[]]::new(8)
            [Array]::Copy([BitConverter]::GetBytes([uint32]$bitmap.Length), 0, $complete, 0, 4)
            [Array]::Copy([BitConverter]::GetBytes($crc), 0, $complete, 4, 4)
            $frame = New-V9xFrameBytes -Type 0x8026 -RequestId $read.RequestId -Payload $complete
            $stream.Write($frame, 0, $frame.Length)
            Start-Sleep -Milliseconds 500
        } finally { $client.Dispose() }
        'SERVER_PASS'
    } finally {
        $listener.Stop()
    }
}

function Invoke-FixtureClient([string[]]$Arguments) {
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $clientScript `
            @Arguments -Host 127.0.0.1 -Port $port -Json 2>&1)
    } finally {
        $ErrorActionPreference = $savedPreference
    }
    if ($LASTEXITCODE -ne 0) {
        throw "v9xctl $($Arguments -join ' ') fixture failed with exit code ${LASTEXITCODE}: $($output -join ' ')"
    }
    $jsonLine = $output | Where-Object { $_ -is [string] -and $_.StartsWith('{') } | Select-Object -Last 1
    if (-not $jsonLine) { throw 'v9xctl fixture returned no JSON.' }
    return $jsonLine | ConvertFrom-Json
}

try {
    Start-Sleep -Milliseconds 400
    $reboot = Invoke-FixtureClient @('reboot','-JobId',$resumeToken,'-WaitSeconds','10')
    if (-not $reboot.Success -or -not $reboot.Reconnected -or
        $reboot.PreviousBootCounter -ne 20 -or $reboot.BootCounter -ne 21 -or
        $reboot.PendingJob -ne $resumeToken) {
        throw 'Reboot/reconnect result did not prove boot and job identity.'
    }
    $desktop = Invoke-FixtureClient @('wait-desktop','-WaitSeconds','10')
    if (-not $desktop.Success -or -not $desktop.DesktopReady -or
        $desktop.ScreenWidth -ne 800 -or $desktop.ScreenHeight -ne 600) {
        throw 'Desktop-ready result mismatch.'
    }
    $screen = Invoke-FixtureClient @('screenshot','-Destination',$screenshotPath)
    if (-not $screen.Success -or $screen.Width -ne 800 -or
        $screen.Height -ne 600 -or $screen.Bytes -ne 64) {
        throw 'Screenshot result mismatch.'
    }
    $screenBytes = [IO.File]::ReadAllBytes($screenshotPath)
    if ($screenBytes.Length -ne 64 -or $screenBytes[0] -ne [byte][char]'B' -or
        $screenBytes[1] -ne [byte][char]'M') {
        throw 'Screenshot download bytes mismatch.'
    }
    if (-not (Wait-Job -Job $server -Timeout 5)) { throw 'M4 fixture did not finish.' }
    $serverOutput = @(Receive-Job -Job $server -ErrorAction Stop)
    if ($serverOutput -notcontains 'SERVER_PASS') { throw 'M4 fixture did not report success.' }
} catch {
    $clientFailure = $_.Exception.Message
    $serverDiagnostics = @(Receive-Job -Job $server -Keep -ErrorAction SilentlyContinue 2>&1)
    throw "$clientFailure Server diagnostics: $($serverDiagnostics -join ' | ')"
} finally {
    Stop-Job -Job $server -ErrorAction SilentlyContinue
    Remove-Job -Job $server -Force -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $testRoot) { Remove-Item -LiteralPath $testRoot -Recurse -Force }
}

Write-Output 'PASS: M4 reboot proof, desktop readiness, and screenshot transfer fixtures'
