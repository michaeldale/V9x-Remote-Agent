$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$protocolScript = Join-Path $repoRoot 'scripts\V9xProtocol.ps1'
$clientScript = Join-Path $repoRoot 'scripts\v9xctl.ps1'
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('v9x-file-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$uploadPath = Join-Path $testRoot 'upload.bin'
$downloadPath = Join-Path $testRoot 'download.bin'
$testBytes = [byte[]]::new(40000)
for ($index = 0; $index -lt $testBytes.Length; ++$index) { $testBytes[$index] = [byte]($index % 251) }
[IO.File]::WriteAllBytes($uploadPath, $testBytes)

$portProbe = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$portProbe.Start()
$port = ([Net.IPEndPoint]$portProbe.LocalEndpoint).Port
$portProbe.Stop()

$server = Start-Job -ArgumentList $port, $protocolScript, $testBytes -ScriptBlock {
    param($Port, $ProtocolScript, [byte[]]$ExpectedBytes)
    $ErrorActionPreference = 'Stop'
    . $ProtocolScript
    function Send-Hello($Stream, $RequestId) {
        $payload = [Collections.Generic.List[byte]]::new()
        $payload.AddRange([BitConverter]::GetBytes([uint16]0x0100))
        $payload.AddRange([BitConverter]::GetBytes([uint16]0))
        $payload.AddRange([BitConverter]::GetBytes([uint32]0x7f))
        $payload.AddRange([BitConverter]::GetBytes([uint32]65536))
        $payload.AddRange([BitConverter]::GetBytes([uint32]10))
        $payload.AddRange([BitConverter]::GetBytes([uint16]9869))
        $payload.AddRange([BitConverter]::GetBytes([uint16]0x0101))
        Add-V9xString -Bytes $payload -Value 'file-fixture'
        $frame = New-V9xFrameBytes -Type 0x8001 -RequestId $RequestId -Payload $payload.ToArray()
        $Stream.Write($frame, 0, $frame.Length)
    }
    function Test-BytesEqual([byte[]]$Left, [byte[]]$Right) {
        if ($Left.Length -ne $Right.Length) { return $false }
        for ($i = 0; $i -lt $Left.Length; ++$i) {
            if ($Left[$i] -ne $Right[$i]) { return $false }
        }
        return $true
    }
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $Port)
    try {
        $listener.Start()
        $putClient = $listener.AcceptTcpClient()
        try {
            $stream = $putClient.GetStream()
            $hello = Read-V9xFrame -Stream $stream
            Send-Hello $stream $hello.RequestId
            $open = Read-V9xFrame -Stream $stream
            if ($open.Type -ne 0x0023) { throw 'Expected FILE_OPEN_WRITE.' }
            $declaredSize = [BitConverter]::ToUInt32($open.Payload, 0)
            $declaredCrc = [BitConverter]::ToUInt32($open.Payload, 4)
            $ready = [byte[]]::new(8)
            [Array]::Copy([BitConverter]::GetBytes($declaredSize), 0, $ready, 0, 4)
            [Array]::Copy([BitConverter]::GetBytes($declaredCrc), 0, $ready, 4, 4)
            $frame = New-V9xFrameBytes -Type 0x8023 -RequestId $open.RequestId -Payload $ready
            $stream.Write($frame, 0, $frame.Length)
            $received = [IO.MemoryStream]::new()
            while ($true) {
                $request = Read-V9xFrame -Stream $stream
                if ($request.Type -eq 0x0025) {
                    $complete = [byte[]]::new(8)
                    [Array]::Copy([BitConverter]::GetBytes([uint32]$received.Length), 0, $complete, 0, 4)
                    [Array]::Copy([BitConverter]::GetBytes($declaredCrc), 0, $complete, 4, 4)
                    $frame = New-V9xFrameBytes -Type 0x8025 -RequestId $request.RequestId -Payload $complete
                    $stream.Write($frame, 0, $frame.Length)
                    break
                }
                if ($request.Type -ne 0x0024) { throw 'Expected FILE_WRITE_CHUNK.' }
                $offset = [BitConverter]::ToUInt32($request.Payload, 0)
                if ($offset -ne $received.Length) { throw 'Upload offset mismatch in fixture.' }
                $received.Write($request.Payload, 4, $request.Payload.Length - 4)
                $ack = [BitConverter]::GetBytes([uint32]$received.Length)
                $frame = New-V9xFrameBytes -Type 0x8024 -RequestId $request.RequestId -Payload $ack
                $stream.Write($frame, 0, $frame.Length)
            }
            if (-not (Test-BytesEqual -Left $received.ToArray() -Right $ExpectedBytes)) {
                throw 'Uploaded bytes do not match fixture input.'
            }
        } finally { $putClient.Dispose() }

        $getClient = $listener.AcceptTcpClient()
        try {
            $stream = $getClient.GetStream()
            $hello = Read-V9xFrame -Stream $stream
            Send-Hello $stream $hello.RequestId
            $read = Read-V9xFrame -Stream $stream
            if ($read.Type -ne 0x0026) { throw 'Expected FILE_OPEN_READ.' }
            [uint32]$offset = 0
            foreach ($count in @(32768, ($ExpectedBytes.Length - 32768))) {
                $chunk = [byte[]]::new($count + 4)
                [Array]::Copy([BitConverter]::GetBytes($offset), 0, $chunk, 0, 4)
                [Array]::Copy($ExpectedBytes, $offset, $chunk, 4, $count)
                $frame = New-V9xFrameBytes -Type 0x9026 -RequestId $read.RequestId -Payload $chunk
                $stream.Write($frame, 0, $frame.Length)
                $offset = [uint32]($offset + $count)
            }
            [uint32]$crc = [uint32]::MaxValue
            $crc = Update-V9xCrc32 -Crc $crc -Bytes $ExpectedBytes
            $crc = [uint32]($crc -bxor [uint32]::MaxValue)
            $complete = [byte[]]::new(8)
            [Array]::Copy([BitConverter]::GetBytes([uint32]$ExpectedBytes.Length), 0, $complete, 0, 4)
            [Array]::Copy([BitConverter]::GetBytes($crc), 0, $complete, 4, 4)
            $frame = New-V9xFrameBytes -Type 0x8026 -RequestId $read.RequestId -Payload $complete
            $stream.Write($frame, 0, $frame.Length)
        } finally { $getClient.Dispose() }
        'SERVER_PASS'
    } finally { $listener.Stop() }
}

try {
    Start-Sleep -Milliseconds 400
    $savedErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $putOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $clientScript `
        put -Source $uploadPath -Destination 'C:\FIXTURE.BIN' -Host 127.0.0.1 -Port $port -Json 2>&1)
    $putExit = $LASTEXITCODE
    $ErrorActionPreference = $savedErrorAction
    if ($putExit -ne 0) { throw "v9xctl put fixture failed: $($putOutput -join ' ')" }
    $ErrorActionPreference = 'Continue'
    $getOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $clientScript `
        get -Source 'C:\FIXTURE.BIN' -Destination $downloadPath -Host 127.0.0.1 -Port $port -Json 2>&1)
    $getExit = $LASTEXITCODE
    $ErrorActionPreference = $savedErrorAction
    if ($getExit -ne 0) {
        $serverFailure = @(Receive-Job -Job $server -Keep -ErrorAction Continue 2>&1)
        throw "v9xctl get fixture failed: $($getOutput -join ' '); server: $($serverFailure -join ' ')"
    }
    if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($uploadPath)) -ne
        [Convert]::ToBase64String([IO.File]::ReadAllBytes($downloadPath))) {
        throw 'Downloaded fixture bytes do not match.'
    }
    if (-not (Wait-Job -Job $server -Timeout 10)) { throw 'File protocol fixture did not finish.' }
    $serverOutput = @(Receive-Job -Job $server -ErrorAction Stop)
    if ($serverOutput -notcontains 'SERVER_PASS') { throw 'File protocol fixture did not report success.' }
} finally {
    Stop-Job -Job $server -ErrorAction SilentlyContinue
    Remove-Job -Job $server -Force -ErrorAction SilentlyContinue
    if ((Resolve-Path -LiteralPath $testRoot -ErrorAction SilentlyContinue).Path.StartsWith([IO.Path]::GetTempPath())) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}

Write-Output 'PASS: v9xctl transactional put/get against protocol fixture'
