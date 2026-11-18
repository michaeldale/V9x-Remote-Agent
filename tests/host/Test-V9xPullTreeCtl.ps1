$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$protocolScript = Join-Path $repoRoot 'scripts\V9xProtocol.ps1'
$clientScript = Join-Path $repoRoot 'scripts\v9xctl.ps1'
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('v9x-pulltree-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$destination = Join-Path $testRoot 'out'

$portProbe = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$portProbe.Start()
$port = ([Net.IPEndPoint]$portProbe.LocalEndpoint).Port
$portProbe.Stop()

# A guest tree: C:\GRAB holds A.BMP, B.TXT and SUB\C.BMP. pull-tree with
# -Filter *.BMP must list both directories and fetch only the two BMPs, all
# over the one connection.
$server = Start-Job -ArgumentList $port, $protocolScript -ScriptBlock {
    param($Port, $ProtocolScript)
    $ErrorActionPreference = 'Stop'
    . $ProtocolScript
    $files = @{
        'C:\GRAB\A.BMP'     = [byte[]](1..40000 | ForEach-Object { [byte]($_ % 251) })
        'C:\GRAB\B.TXT'     = [Text.Encoding]::ASCII.GetBytes("not wanted`r`n")
        'C:\GRAB\SUB\C.BMP' = [byte[]](1..100 | ForEach-Object { [byte]($_ * 3 % 256) })
    }
    $listings = @{
        'C:\GRAB'     = @(@('A.BMP', 0x20, 40000), @('B.TXT', 0x20, 12), @('SUB', 0x10, 0))
        'C:\GRAB\SUB' = @(, @('C.BMP', 0x20, 100))
    }
    function Send-Frame($Stream, [uint16]$Type, [uint32]$RequestId, [byte[]]$Payload) {
        $frame = New-V9xFrameBytes -Type $Type -RequestId $RequestId -Payload $Payload
        $Stream.Write($frame, 0, $frame.Length)
    }
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $Port)
    $listed = @()
    $read = @()
    try {
        $listener.Start()
        $client = $listener.AcceptTcpClient()
        try {
            $stream = $client.GetStream()
            $hello = Read-V9xFrame -Stream $stream
            $payload = [Collections.Generic.List[byte]]::new()
            $payload.AddRange([BitConverter]::GetBytes([uint16]0x0100))
            $payload.AddRange([BitConverter]::GetBytes([uint16]0))
            $payload.AddRange([BitConverter]::GetBytes([uint32]0x7f))
            $payload.AddRange([BitConverter]::GetBytes([uint32]65536))
            $payload.AddRange([BitConverter]::GetBytes([uint32]10))
            $payload.AddRange([BitConverter]::GetBytes([uint16]9869))
            $payload.AddRange([BitConverter]::GetBytes([uint16]0x0101))
            Add-V9xString -Bytes $payload -Value 'pulltree-fixture'
            Send-Frame $stream 0x8001 $hello.RequestId $payload.ToArray()
            while ($true) {
                try { $request = Read-V9xFrame -Stream $stream } catch { break }
                $offset = 0
                $path = Read-V9xString -Bytes $request.Payload -Offset ([ref]$offset)
                if ($request.Type -eq 0x0021) {
                    $listed += $path
                    $body = [Collections.Generic.List[byte]]::new()
                    $body.AddRange([BitConverter]::GetBytes([uint32]$listings[$path].Count))
                    foreach ($entry in $listings[$path]) {
                        $body.AddRange([BitConverter]::GetBytes([uint32]$entry[1]))
                        $body.AddRange([BitConverter]::GetBytes([uint32]$entry[2]))
                        Add-V9xString -Bytes $body -Value $entry[0]
                    }
                    Send-Frame $stream 0x8021 $request.RequestId $body.ToArray()
                } elseif ($request.Type -eq 0x0026) {
                    $read += $path
                    [byte[]]$data = $files[$path]
                    $chunk = [byte[]]::new($data.Length + 4)
                    [Array]::Copy($data, 0, $chunk, 4, $data.Length)
                    Send-Frame $stream 0x9026 $request.RequestId $chunk
                    $crc = Update-V9xCrc32 -Crc ([uint32]::MaxValue) -Bytes $data
                    $complete = [byte[]]::new(8)
                    [Array]::Copy([BitConverter]::GetBytes([uint32]$data.Length), 0, $complete, 0, 4)
                    [Array]::Copy([BitConverter]::GetBytes([uint32]($crc -bxor [uint32]::MaxValue)), 0, $complete, 4, 4)
                    Send-Frame $stream 0x8026 $request.RequestId $complete
                } else {
                    throw "Unexpected request 0x$('{0:x4}' -f $request.Type)."
                }
            }
        } finally { $client.Dispose() }
        if (($listed -join '|') -ne 'C:\GRAB|C:\GRAB\SUB') { throw "Listed: $($listed -join '|')" }
        if (($read -join '|') -ne 'C:\GRAB\A.BMP|C:\GRAB\SUB\C.BMP') { throw "Read: $($read -join '|')" }
        'SERVER_PASS'
    } finally { $listener.Stop() }
}

try {
    Start-Sleep -Milliseconds 400
    $savedErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $clientScript `
        pull-tree -Source 'C:\GRAB\' -Destination $destination -Filter '*.BMP' `
        -Host 127.0.0.1 -Port $port -Json 2>&1)
    $exit = $LASTEXITCODE
    $ErrorActionPreference = $savedErrorAction
    if ($exit -ne 0) {
        $serverFailure = @(Receive-Job -Job $server -Keep -ErrorAction Continue 2>&1)
        throw "v9xctl pull-tree failed: $($output -join ' '); server: $($serverFailure -join ' ')"
    }
    $result = ($output | Where-Object { "$_".StartsWith('{') } | Select-Object -Last 1) | ConvertFrom-Json
    if ($result.Files -ne 2 -or $result.Directories -ne 2 -or $result.Bytes -ne 40100) {
        throw "Unexpected pull-tree result: $($output -join ' ')"
    }
    if ((Get-Item -LiteralPath (Join-Path $destination 'A.BMP')).Length -ne 40000 -or
        (Get-Item -LiteralPath (Join-Path $destination 'SUB\C.BMP')).Length -ne 100 -or
        (Test-Path -LiteralPath (Join-Path $destination 'B.TXT'))) {
        throw 'pull-tree wrote the wrong local files.'
    }
    if (-not (Wait-Job -Job $server -Timeout 10)) { throw 'pull-tree fixture did not finish.' }
    $serverOutput = @(Receive-Job -Job $server -ErrorAction Stop)
    if ($serverOutput -notcontains 'SERVER_PASS') { throw 'pull-tree fixture did not report success.' }
} finally {
    Stop-Job -Job $server -ErrorAction SilentlyContinue
    Remove-Job -Job $server -Force -ErrorAction SilentlyContinue
    if ((Resolve-Path -LiteralPath $testRoot -ErrorAction SilentlyContinue).Path.StartsWith([IO.Path]::GetTempPath())) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}

Write-Output 'PASS: v9xctl pull-tree lists and fetches a filtered tree over one connection'
