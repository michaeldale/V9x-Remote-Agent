$ErrorActionPreference = 'Stop'
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
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $Port)
    $client = $null
    try {
        $listener.Start()
        $client = $listener.AcceptTcpClient()
        $stream = $client.GetStream()
        $helloRequest = Read-V9xFrame -Stream $stream
        if ($helloRequest.Type -ne 0x0001) { throw 'Expected HELLO_REQUEST.' }

        $helloPayload = [Collections.Generic.List[byte]]::new()
        $helloPayload.AddRange([BitConverter]::GetBytes([uint16]0x0100))
        $helloPayload.AddRange([BitConverter]::GetBytes([uint16]0))
        $helloPayload.AddRange([BitConverter]::GetBytes([uint32]0x1f))
        $helloPayload.AddRange([BitConverter]::GetBytes([uint32]65536))
        $helloPayload.AddRange([BitConverter]::GetBytes([uint32]9))
        $helloPayload.AddRange([BitConverter]::GetBytes([uint16]9869))
        $helloPayload.AddRange([BitConverter]::GetBytes([uint16]0x0101))
        Add-V9xString -Bytes $helloPayload -Value 'fixture-m2'
        $bytes = New-V9xFrameBytes -Type 0x8001 -RequestId $helloRequest.RequestId -Payload $helloPayload.ToArray()
        $stream.Write($bytes, 0, $bytes.Length)

        $execRequest = Read-V9xFrame -Stream $stream
        if ($execRequest.Type -ne 0x0010) { throw 'Expected EXEC_REQUEST.' }
        $acceptedPayload = [BitConverter]::GetBytes([uint32]123)
        $bytes = New-V9xFrameBytes -Type 0x8010 -RequestId $execRequest.RequestId -Payload $acceptedPayload
        $stream.Write($bytes, 0, $bytes.Length)
        $stdout = [Text.Encoding]::ASCII.GetBytes("guest-out`r`n")
        $bytes = New-V9xFrameBytes -Type 0x9010 -RequestId $execRequest.RequestId -Payload $stdout
        $stream.Write($bytes, 0, $bytes.Length)
        $stderr = [Text.Encoding]::ASCII.GetBytes("guest-err`r`n")
        $bytes = New-V9xFrameBytes -Type 0x9011 -RequestId $execRequest.RequestId -Payload $stderr
        $stream.Write($bytes, 0, $bytes.Length)
        $complete = [byte[]]::new(28)
        [Array]::Copy([BitConverter]::GetBytes([uint32]7), 0, $complete, 4, 4)
        [Array]::Copy([BitConverter]::GetBytes([uint32]42), 0, $complete, 12, 4)
        [Array]::Copy([BitConverter]::GetBytes([uint32]11), 0, $complete, 16, 4)
        [Array]::Copy([BitConverter]::GetBytes([uint32]11), 0, $complete, 20, 4)
        [Array]::Copy([BitConverter]::GetBytes([uint32]0x10), 0, $complete, 24, 4)
        $bytes = New-V9xFrameBytes -Type 0x8011 -RequestId $execRequest.RequestId -Payload $complete
        $stream.Write($bytes, 0, $bytes.Length)
        $client.Dispose()
        $client = $null

        $client = $listener.AcceptTcpClient()
        $stream = $client.GetStream()
        $helloRequest = Read-V9xFrame -Stream $stream
        if ($helloRequest.Type -ne 0x0001) { throw 'Expected HELLO_REQUEST (detach).' }
        $helloPayload = [Collections.Generic.List[byte]]::new()
        $helloPayload.AddRange([BitConverter]::GetBytes([uint16]0x0100))
        $helloPayload.AddRange([BitConverter]::GetBytes([uint16]0))
        $helloPayload.AddRange([BitConverter]::GetBytes([uint32]0x81f))
        $helloPayload.AddRange([BitConverter]::GetBytes([uint32]65536))
        $helloPayload.AddRange([BitConverter]::GetBytes([uint32]9))
        $helloPayload.AddRange([BitConverter]::GetBytes([uint16]9869))
        $helloPayload.AddRange([BitConverter]::GetBytes([uint16]0x0101))
        Add-V9xString -Bytes $helloPayload -Value 'fixture-m2'
        $bytes = New-V9xFrameBytes -Type 0x8001 -RequestId $helloRequest.RequestId -Payload $helloPayload.ToArray()
        $stream.Write($bytes, 0, $bytes.Length)

        $execRequest = Read-V9xFrame -Stream $stream
        if ($execRequest.Type -ne 0x0010) { throw 'Expected EXEC_REQUEST (detach).' }
        $optionBits = [BitConverter]::ToUInt16($execRequest.Payload, 2)
        if ($optionBits -ne 1) { throw "Detach request did not set option bit 0x0001 (got $optionBits)." }
        $acceptedPayload = [BitConverter]::GetBytes([uint32]124)
        $bytes = New-V9xFrameBytes -Type 0x8010 -RequestId $execRequest.RequestId -Payload $acceptedPayload
        $stream.Write($bytes, 0, $bytes.Length)
        $complete = [byte[]]::new(28)
        [Array]::Copy([BitConverter]::GetBytes([uint32]3), 0, $complete, 12, 4)
        [Array]::Copy([BitConverter]::GetBytes([uint32]0x40), 0, $complete, 24, 4)
        $bytes = New-V9xFrameBytes -Type 0x8011 -RequestId $execRequest.RequestId -Payload $complete
        $stream.Write($bytes, 0, $bytes.Length)
        'SERVER_PASS'
    } finally {
        if ($null -ne $client) { $client.Dispose() }
        $listener.Stop()
    }
}

try {
    Start-Sleep -Milliseconds 400
    $clientOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $clientScript `
        shell -Command VER -Host 127.0.0.1 -Port $port -ExpectedExitCode 7 -Json 2>&1)
    $clientExit = $LASTEXITCODE
    if ($clientExit -ne 0) { throw "v9xctl fixture test exited $clientExit`: $($clientOutput -join ' ')" }
    $jsonLine = $clientOutput | Where-Object { $_ -is [string] -and $_.StartsWith('{') } | Select-Object -Last 1
    if (-not $jsonLine) { throw 'v9xctl fixture test returned no JSON.' }
    $result = $jsonLine | ConvertFrom-Json
    if (-not $result.Success -or $result.ExitCode -ne 7 -or
        $result.Stdout -ne "guest-out`r`n" -or $result.Stderr -ne "guest-err`r`n" -or
        $result.CaptureMode -ne 'pipes' -or $result.Detached -or $result.Orphaned) {
        throw 'v9xctl fixture result did not preserve execution data.'
    }

    $detachOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $clientScript `
        shell -Command 'SETUP.EXE' -Detach -Host 127.0.0.1 -Port $port -Json 2>&1)
    $detachExit = $LASTEXITCODE
    if ($detachExit -ne 0) { throw "v9xctl detach fixture test exited $detachExit`: $($detachOutput -join ' ')" }
    $jsonLine = $detachOutput | Where-Object { $_ -is [string] -and $_.StartsWith('{') } | Select-Object -Last 1
    if (-not $jsonLine) { throw 'v9xctl detach fixture test returned no JSON.' }
    $detachResult = $jsonLine | ConvertFrom-Json
    if (-not $detachResult.Success -or -not $detachResult.Detached -or
        $detachResult.ExitCode -ne 0 -or $detachResult.CaptureMode -ne 'none' -or
        $detachResult.Stdout -ne '' -or $detachResult.Orphaned) {
        throw 'v9xctl detach fixture result did not report a detached launch.'
    }
    if (-not (Wait-Job -Job $server -Timeout 5)) { throw 'Protocol fixture did not finish.' }
    $serverOutput = @(Receive-Job -Job $server -ErrorAction Stop)
    if ($serverOutput -notcontains 'SERVER_PASS') { throw 'Protocol fixture did not report success.' }
} finally {
    Stop-Job -Job $server -ErrorAction SilentlyContinue
    Remove-Job -Job $server -Force -ErrorAction SilentlyContinue
}

Write-Output 'PASS: v9xctl execution streaming and detached launch against protocol fixture'

