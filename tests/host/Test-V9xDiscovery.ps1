$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$protocolScript = Join-Path $repoRoot 'scripts\V9xProtocol.ps1'
$clientScript = Join-Path $repoRoot 'scripts\v9xctl.ps1'
. $protocolScript

# mdns-announce.hex is the announcement src\common\mdns.c builds for the
# fixture service in tests\host\test_protocol.c (build-host-tests.ps1 checks
# the two still agree), so this decodes the agent's real wire format.
$hex = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'mdns-announce.hex') -Raw).Trim()
$announce = [byte[]]::new($hex.Length / 2)
for ($index = 0; $index -lt $announce.Length; ++$index) {
    $announce[$index] = [Convert]::ToByte($hex.Substring($index * 2, 2), 16)
}
$fixtureId = '1a2b3c4d-0000-4000-8000-000000000001'

$packet = ConvertFrom-V9xMdnsPacket -Packet $announce
if (-not $packet.IsResponse -or $packet.Records.Count -ne 5) { throw 'Announcement did not decode to five records.' }
$agents = @(ConvertTo-V9xDiscoveredAgents -Replies @([pscustomobject]@{ Source = '10.1.1.1'; Packet = $packet }))
if ($agents.Count -ne 1) { throw "Expected one agent, got $($agents.Count)." }
if ($agents[0].Instance -ne 'WIN98-1a2b3c4d' -or $agents[0].InstallId -ne $fixtureId -or
    $agents[0].Address -ne '192.168.1.50' -or $agents[0].Port -ne 9869 -or
    $agents[0].AgentVersion -ne '0.8.0' -or $agents[0].RespondedFrom -ne '10.1.1.1') {
    throw "Announcement folded wrongly: $($agents[0] | ConvertTo-Json -Compress)"
}

# A goodbye (every TTL 0) after the announcement removes the agent.
$goodbye = [byte[]]$announce.Clone()
$goodbyePacket = ConvertFrom-V9xMdnsPacket -Packet $goodbye
foreach ($record in $goodbyePacket.Records) { $record.Ttl = 0 }
$gone = @(ConvertTo-V9xDiscoveredAgents -Replies @(
    [pscustomobject]@{ Source = '10.1.1.1'; Packet = $packet },
    [pscustomobject]@{ Source = '10.1.1.1'; Packet = $goodbyePacket }))
if ($gone.Count -ne 0) { throw 'A goodbye did not remove the agent.' }

# Compression pointers and a bad pointer loop.
$query = New-V9xMdnsQuery -Id 0x1234
if ($query.Length -ne 39 -or $query[0] -ne 0x12 -or $query[1] -ne 0x34 -or $query[5] -ne 1 -or $query[36] -ne 12) {
    throw 'mDNS query encoding changed.'
}
$pointer = [byte[]](0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0) + $query[12..34] +
    [byte[]](0, 12, 0, 1, 0, 0, 0, 10, 0, 2, 0xc0, 12)
$decoded = ConvertFrom-V9xMdnsPacket -Packet $pointer
if ($decoded.Records[0].Data -ne '_v9xremote._tcp.local') { throw 'Compression pointer did not decode.' }
$loop = [byte[]](0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0xc0, 12, 0, 12, 0, 1, 0, 0, 0, 10, 0, 0)
$threw = $false
try { [void](ConvertFrom-V9xMdnsPacket -Packet $loop) } catch { $threw = $true }
if (-not $threw) { throw 'A compression pointer loop was accepted.' }

# End to end: a fake responder on a loopback UDP port answers v9xctl's
# unicast query, then a fake agent answers the TCP session that -Id opens.
# The announcement's A record and SRV port are pointed at that fake agent.
$tcpProbe = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$tcpProbe.Start()
$tcpPort = ([Net.IPEndPoint]$tcpProbe.LocalEndpoint).Port
$tcpProbe.Stop()
$udpProbe = [Net.Sockets.UdpClient]::new([Net.IPEndPoint]::new([Net.IPAddress]::Loopback, 0))
$udpPort = ([Net.IPEndPoint]$udpProbe.Client.LocalEndPoint).Port
$udpProbe.Dispose()

$local = [byte[]]$announce.Clone()
$local[$local.Length - 4] = 127; $local[$local.Length - 3] = 0
$local[$local.Length - 2] = 0; $local[$local.Length - 1] = 1
for ($index = 0; $index -lt $local.Length - 16; ++$index) {
    if ($local[$index] -eq 0 -and $local[$index + 1] -eq 0x21 -and
        $local[$index + 2] -eq 0x80 -and $local[$index + 3] -eq 1) {
        $local[$index + 14] = [byte]($tcpPort -shr 8)
        $local[$index + 15] = [byte]($tcpPort -band 0xff)
        break
    }
}

$responder = Start-Job -ArgumentList $udpPort, $tcpPort, $protocolScript, $local, $fixtureId -ScriptBlock {
    param($UdpPort, $TcpPort, $ProtocolScript, [byte[]]$Answer, $InstallId)
    $ErrorActionPreference = 'Stop'
    . $ProtocolScript
    $udp = [Net.Sockets.UdpClient]::new([Net.IPEndPoint]::new([Net.IPAddress]::Loopback, $UdpPort))
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $TcpPort)
    $client = $null
    try {
        $listener.Start()
        # 1: discover.  2: info -Id.
        for ($round = 0; $round -lt 2; ++$round) {
            $remote = [Net.IPEndPoint]::new([Net.IPAddress]::Any, 0)
            $request = $udp.Receive([ref]$remote)
            if (($request[2] -band 0x80) -ne 0) { throw 'Expected a DNS query.' }
            $name = [Text.Encoding]::ASCII.GetString($request, 12, $request.Length - 12)
            if ($name -notmatch '_v9xremote') { throw 'Query was not for _v9xremote._tcp.' }
            [void]$udp.Send($Answer, $Answer.Length, $remote)
        }

        $client = $listener.AcceptTcpClient()
        $stream = $client.GetStream()
        $hello = Read-V9xFrame -Stream $stream
        if ($hello.Type -ne 0x0001) { throw 'Expected HELLO_REQUEST.' }
        $payload = [Collections.Generic.List[byte]]::new()
        $payload.AddRange([BitConverter]::GetBytes([uint16]0x0100))
        $payload.AddRange([BitConverter]::GetBytes([uint16]0))
        $payload.AddRange([BitConverter]::GetBytes([uint32]0x4003))
        $payload.AddRange([BitConverter]::GetBytes([uint32]65536))
        $payload.AddRange([BitConverter]::GetBytes([uint32]5))
        $payload.AddRange([BitConverter]::GetBytes([uint16]$TcpPort))
        $payload.AddRange([BitConverter]::GetBytes([uint16]0x0101))
        Add-V9xString -Bytes $payload -Value 'fixture-discovery'
        $bytes = New-V9xFrameBytes -Type 0x8001 -RequestId $hello.RequestId -Payload $payload.ToArray()
        $stream.Write($bytes, 0, $bytes.Length)

        $info = Read-V9xFrame -Stream $stream
        if ($info.Type -ne 0x0030) { throw 'Expected INFO_REQUEST.' }
        if ($info.Payload.Length -ne 4 -or [BitConverter]::ToUInt32($info.Payload, 0) -ne 1) {
            throw 'INFO request did not ask for the extended reply.'
        }
        $payload = [Collections.Generic.List[byte]]::new()
        $payload.AddRange([BitConverter]::GetBytes([uint32]5))
        $payload.AddRange([BitConverter]::GetBytes([uint32]1000))
        $payload.AddRange([BitConverter]::GetBytes([uint32]0x4003))
        $payload.AddRange([BitConverter]::GetBytes([uint16]$TcpPort))
        $payload.AddRange([BitConverter]::GetBytes([uint16]0x0101))
        $payload.AddRange([BitConverter]::GetBytes([uint32]0x0a04))
        foreach ($value in @('0.8.0', 'fixture-discovery', 'WIN98', 'Windows 4.10',
                             'C:\WINDOWS\SYSTEM', 'C:\WINDOWS', 'C:\', '')) {
            Add-V9xString -Bytes $payload -Value $value
        }
        $payload.AddRange([byte[]]::new(16))
        Add-V9xString -Bytes $payload -Value '0.0.0.0'
        Add-V9xString -Bytes $payload -Value ''
        Add-V9xString -Bytes $payload -Value $InstallId
        $bytes = New-V9xFrameBytes -Type 0x8030 -RequestId $info.RequestId -Payload $payload.ToArray()
        $stream.Write($bytes, 0, $bytes.Length)
    } finally {
        if ($null -ne $client) { $client.Dispose() }
        $listener.Stop()
        $udp.Dispose()
    }
}

try {
    Start-Sleep -Milliseconds 700
    $discovered = & $clientScript discover -QueryAddress "127.0.0.1:$udpPort" -DiscoverSeconds 2 -Json | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0) { throw "discover exited $LASTEXITCODE." }
    if ($discovered.Count -ne 1 -or $discovered.Agents[0].InstallId -ne $fixtureId -or
        $discovered.Agents[0].Address -ne '127.0.0.1' -or $discovered.Agents[0].Port -ne $tcpPort) {
        throw "discover returned $($discovered | ConvertTo-Json -Compress -Depth 4)"
    }

    $info = & $clientScript info -Id $fixtureId -QueryAddress "127.0.0.1:$udpPort" -DiscoverSeconds 2 -Json | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0) { throw "info -Id exited $LASTEXITCODE." }
    if ($info.InstallId -ne $fixtureId -or $info.ComputerName -ne 'WIN98') {
        throw "info -Id returned $($info | ConvertTo-Json -Compress)"
    }
    if (-not (Wait-Job -Job $responder -Timeout 10)) { throw 'Fake responder did not finish.' }
    Receive-Job -Job $responder | Out-Null
} finally {
    Stop-Job -Job $responder -ErrorAction SilentlyContinue
    Remove-Job -Job $responder -Force -ErrorAction SilentlyContinue
}

# Nobody answering is exit 20, like any unreachable agent.
$silent = [Net.Sockets.UdpClient]::new([Net.IPEndPoint]::new([Net.IPAddress]::Loopback, 0))
try {
    $silentPort = ([Net.IPEndPoint]$silent.Client.LocalEndPoint).Port
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $clientScript discover `
            -QueryAddress "127.0.0.1:$silentPort" -DiscoverSeconds 1 -Json 2>&1 | Out-Null
        $silentExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    if ($silentExit -ne 20) { throw "discover with no answer exited $silentExit, expected 20." }
    $global:LASTEXITCODE = 0   # the expected 20 must not leak into the suite's exit code
} finally {
    $silent.Dispose()
}

Write-Output 'PASS: mDNS discovery codec, discover verb and -Id resolution against fixture responder'
