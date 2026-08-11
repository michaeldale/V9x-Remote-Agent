[CmdletBinding()]
param(
    [string]$HostName = '127.0.0.1',
    [int]$Port = 9869,
    [Parameter(Mandatory = $true)][string]$ExpectedFile,
    [Parameter(Mandatory = $true)][string]$GuestPath,
    [Parameter(Mandatory = $true)][string]$GuestDirectory
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $repoRoot 'scripts\V9xProtocol.ps1')

function Open-V9xTestConnection {
    $client = [Net.Sockets.TcpClient]::new($HostName, $Port)
    $stream = $client.GetStream()
    $stream.ReadTimeout = 10000
    [uint32]$helloId = [uint32](Get-Random -Minimum 1 -Maximum ([int]::MaxValue))
    $bytes = New-V9xFrameBytes -Type 0x0001 -RequestId $helloId -Payload (New-V9xHelloPayload -ClientLabel 'live-safety-test')
    $stream.Write($bytes, 0, $bytes.Length)
    $hello = Read-V9xFrame -Stream $stream
    if ($hello.Type -ne 0x8001 -or $hello.RequestId -ne $helloId) { throw 'HELLO failed.' }
    return [pscustomobject]@{ Client = $client; Stream = $stream; RequestId = [uint32]($helloId + 1) }
}

$badBytes = [Text.Encoding]::ASCII.GetBytes('BAD')
$connection = Open-V9xTestConnection
try {
    $payload = New-V9xOpenWritePayload -Path $GuestPath -Size 3 -Crc32 0
    $bytes = New-V9xFrameBytes -Type 0x0023 -RequestId $connection.RequestId -Payload $payload
    $connection.Stream.Write($bytes, 0, $bytes.Length)
    $ready = Read-V9xFrame -Stream $connection.Stream
    if ($ready.Type -ne 0x8023) { throw 'Guest did not accept safety-test upload.' }
    ++$connection.RequestId
    $chunk = [byte[]]::new(7)
    [Array]::Copy($badBytes, 0, $chunk, 4, 3)
    $bytes = New-V9xFrameBytes -Type 0x0024 -RequestId $connection.RequestId -Payload $chunk
    $connection.Stream.Write($bytes, 0, $bytes.Length)
    $ack = Read-V9xFrame -Stream $connection.Stream
    if ($ack.Type -ne 0x8024) { throw 'Guest did not acknowledge safety-test chunk.' }
    ++$connection.RequestId
    $bytes = New-V9xFrameBytes -Type 0x0025 -RequestId $connection.RequestId
    $connection.Stream.Write($bytes, 0, $bytes.Length)
    $rejection = Read-V9xFrame -Stream $connection.Stream
    if ($rejection.Type -ne 0x8fff) { throw 'Guest committed an upload with a bad CRC.' }
    $errorInfo = ConvertFrom-V9xErrorPayload -Payload $rejection.Payload
    if ($errorInfo.Status -ne 14) { throw "Expected CRC error 14, received $($errorInfo.Status)." }
} finally {
    $connection.Client.Dispose()
}

$expectedBytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $ExpectedFile))
$connection = Open-V9xTestConnection
try {
    $bytes = New-V9xFrameBytes -Type 0x0026 -RequestId $connection.RequestId `
        -Payload (New-V9xPathPayload -Path $GuestPath)
    $connection.Stream.Write($bytes, 0, $bytes.Length)
    $download = [IO.MemoryStream]::new()
    while ($true) {
        $frame = Read-V9xFrame -Stream $connection.Stream
        if ($frame.Type -eq 0x9026) {
            if ([BitConverter]::ToUInt32($frame.Payload, 0) -ne $download.Length) {
                throw 'Safety-test download offset mismatch.'
            }
            $download.Write($frame.Payload, 4, $frame.Payload.Length - 4)
        } elseif ($frame.Type -eq 0x8026) {
            break
        } else {
            throw "Unexpected safety-test download frame 0x$('{0:x4}' -f $frame.Type)."
        }
    }
    if ([Convert]::ToBase64String($download.ToArray()) -ne
        [Convert]::ToBase64String($expectedBytes)) {
        throw 'Known-good guest destination changed after rejected CRC.'
    }
} finally {
    $connection.Client.Dispose()
}

$connection = Open-V9xTestConnection
try {
    $bytes = New-V9xFrameBytes -Type 0x0021 -RequestId $connection.RequestId `
        -Payload (New-V9xPathPayload -Path $GuestDirectory)
    $connection.Stream.Write($bytes, 0, $bytes.Length)
    $frame = Read-V9xFrame -Stream $connection.Stream
    if ($frame.Type -ne 0x8021) { throw 'Safety-test directory listing failed.' }
    $debris = @(ConvertFrom-V9xListPayload -Payload $frame.Payload |
        Where-Object { $_.Name -like '*.PART' -or $_.Name -like '*.BAK' })
    if ($debris.Count -ne 0) { throw "Transfer debris remains: $($debris.Name -join ', ')" }
} finally {
    $connection.Client.Dispose()
}

Write-Output 'PASS: bad-CRC commit rejected; destination preserved; no transfer debris'
