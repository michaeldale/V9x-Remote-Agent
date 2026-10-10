<#
.SYNOPSIS
Drive a Windows 9x guest running the V9x Remote Agent: run programs, move
files, take screenshots, inject input, reboot with proof.

.DESCRIPTION
One bounded operation per invocation. The first argument is the verb; the
remaining parameters depend on the verb. Every call opens a new connection to
the agent, so the target is chosen per call with -Host and -Port (default
127.0.0.1:9869, which is the usual emulator port forward).

Run with no verb, or with the verb 'help', for a compact verb list with
examples. Add -Json for compact machine-readable output. Exit codes are
deterministic and listed in AGENTS.md (20 means the agent was unreachable).

.PARAMETER Action
The verb: discover, ping, info, inventory, exec, shell, stat, list, mkdir, put, get, push-tree,
download, update, reboot, shutdown, wait-desktop, screenshot, input, help.

.PARAMETER EndpointHost
Address of the agent (alias -Host). 127.0.0.1 for an emulator with a port
forward, or the guest's LAN address for a physical machine or bridged VM.

.PARAMETER Port
TCP port the agent listens on (default 9869, the 'port' key in AGENT.INI).

.PARAMETER Id
Install ID of the agent (the install_id in its AGENT.INI, shown by info,
inventory and discover). The target is found by mDNS discovery instead of
-Host/-Port, and found again if it comes back from a reboot on a new address.
Needs agent 0.8.0+ with discovery=1.

.PARAMETER QueryAddress
Send the discovery query unicast to this address[:port] (default port 5353)
instead of multicasting it on every interface.

.PARAMETER DiscoverSeconds
How long discover (and -Id) listen for answers (default 3).

.PARAMETER ConnectTimeoutSeconds
How long to wait for the TCP connection and for each response (default 10).

.PARAMETER Json
Emit the result as one compact JSON object instead of a formatted list.

.EXAMPLE
.\scripts\v9xctl.ps1 ping
Liveness check against the default 127.0.0.1:9869.

.EXAMPLE
.\scripts\v9xctl.ps1 info -Host 192.168.10.98 -Port 9869 -Json
Agent version and state on a physical machine at 192.168.10.98.

.EXAMPLE
.\scripts\v9xctl.ps1 exec -Application C:\WINDOWS\NOTEPAD.EXE -TimeoutSeconds 30 -Json
Run a program, capture its output, and fail after 30 seconds.

.EXAMPLE
.\scripts\v9xctl.ps1 shell -Command "DIR C:\ /W" -Json
Run a COMMAND.COM built-in (CMD.EXE on Windows 2000).

.EXAMPLE
.\scripts\v9xctl.ps1 put -Source .\out\MYAPP.EXE -Destination C:\V9XREMOTE\JOBS\myjob\MYAPP.EXE
Upload one file, CRC32-verified.

.EXAMPLE
.\scripts\v9xctl.ps1 screenshot -OutFile .\shot.png -Host 127.0.0.1 -Port 9869
Capture the guest screen to a PNG.

.EXAMPLE
.\scripts\v9xctl.ps1 reboot -JobId myjob-reboot-1 -WaitSeconds 180 -Json
Reboot and wait for the agent to come back with proof.

.LINK
AGENTS.md
docs/host-cli.md
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ArgumentCompleter({
        param($commandName, $parameterName, $wordToComplete)
        @('discover', 'ping', 'info', 'inventory', 'exec', 'shell', 'stat', 'list', 'mkdir',
          'put', 'get', 'push-tree', 'download', 'update', 'reboot', 'shutdown',
          'wait-desktop', 'screenshot', 'input', 'help') |
            Where-Object { $_ -like "$wordToComplete*" }
    })]
    [string]$Action,
    [Alias('Host')]
    [string]$EndpointHost = '127.0.0.1',
    [ValidateRange(1, 65535)]
    [int]$Port = 9869,
    [ValidateRange(1, 120)]
    [int]$ConnectTimeoutSeconds = 10,
    [string]$Application,
    [string]$Arguments = '',
    [Alias('Command')]
    [string]$ShellCommand,
    [string]$WorkingDirectory = '',
    [string]$Source,
    [Alias('OutFile')]
    [string]$Destination,
    [Alias('Path')]
    [string]$RemotePath,
    [string]$Url,
    [string]$JobId,
    [ValidateRange(5, 600)]
    [int]$WaitSeconds = 120,
    [ValidateRange(1, 3600)]
    [int]$TimeoutSeconds = 60,
    [ValidateRange(0, 1048576)]
    [int]$StdoutLimit = 262144,
    [ValidateRange(0, 1048576)]
    [int]$StderrLimit = 262144,
    [uint32[]]$ExpectedExitCode = @(0),
    [switch]$ShowWindow,
    [switch]$Detach,
    [string]$Sequence,
    [ValidatePattern('^$|^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}$')]
    [string]$Id,
    [string]$QueryAddress,
    [ValidateRange(1, 30)]
    [int]$DiscoverSeconds = 3,
    [switch]$Json
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'V9xProtocol.ps1')

$script:V9xVkNames = @{
    'ENTER' = 0x0D; 'RETURN' = 0x0D; 'TAB' = 0x09; 'ESC' = 0x1B; 'ESCAPE' = 0x1B
    'SPACE' = 0x20; 'BACKSPACE' = 0x08; 'BKSP' = 0x08; 'DELETE' = 0x2E; 'DEL' = 0x2E
    'INSERT' = 0x2D; 'INS' = 0x2D; 'HOME' = 0x24; 'END' = 0x23
    'PAGEUP' = 0x21; 'PGUP' = 0x21; 'PAGEDOWN' = 0x22; 'PGDN' = 0x22
    'UP' = 0x26; 'DOWN' = 0x28; 'LEFT' = 0x25; 'RIGHT' = 0x27
    'CTRL' = 0x11; 'CONTROL' = 0x11; 'ALT' = 0x12; 'MENU' = 0x12; 'SHIFT' = 0x10
    'WIN' = 0x5B; 'LWIN' = 0x5B; 'RWIN' = 0x5C
    'F1' = 0x70; 'F2' = 0x71; 'F3' = 0x72; 'F4' = 0x73; 'F5' = 0x74; 'F6' = 0x75
    'F7' = 0x76; 'F8' = 0x77; 'F9' = 0x78; 'F10' = 0x79; 'F11' = 0x7A; 'F12' = 0x7B
}

function ConvertTo-V9xVirtualKey {
    param([string]$Name)
    $upper = $Name.ToUpperInvariant()
    if ($script:V9xVkNames.ContainsKey($upper)) { return [int]$script:V9xVkNames[$upper] }
    if ($upper.Length -eq 1) {
        $code = [int][char]$upper
        if (($code -ge 0x41 -and $code -le 0x5A) -or ($code -ge 0x30 -and $code -le 0x39)) {
            return $code
        }
    }
    throw "Unknown key name '$Name'."
}

function Resolve-V9xButton {
    param([string]$Name)
    switch (($Name).ToLowerInvariant()) {
        '' { return 0 }
        'left' { return 0 }
        'right' { return 1 }
        'middle' { return 2 }
        default { throw "Unknown mouse button '$Name'." }
    }
}

function ConvertTo-V9xInputActions {
    param([string]$Sequence)
    $actions = [Collections.Generic.List[hashtable]]::new()
    foreach ($segment in ($Sequence -split '[;\r\n]+')) {
        $trimmed = $segment.Trim()
        if ($trimmed -eq '') { continue }
        $space = $trimmed.IndexOf(' ')
        $verb = if ($space -lt 0) { $trimmed } else { $trimmed.Substring(0, $space) }
        $rest = if ($space -lt 0) { '' } else { $trimmed.Substring($space + 1).Trim() }
        switch ($verb.ToLowerInvariant()) {
            'move' {
                $coordinates = $rest -split '[, ]+'
                if ($coordinates.Count -ne 2) { throw "move needs X,Y (got '$rest')." }
                $actions.Add(@{ Op = 'move'; X = [int]$coordinates[0]; Y = [int]$coordinates[1] })
            }
            'moverel' {
                $coordinates = $rest -split '[, ]+'
                if ($coordinates.Count -ne 2) { throw "moverel needs DX,DY (got '$rest')." }
                $actions.Add(@{ Op = 'moverel'; X = [int]$coordinates[0]; Y = [int]$coordinates[1] })
            }
            'click' {
                $button = Resolve-V9xButton $rest
                $actions.Add(@{ Op = 'button'; Button = $button; Down = $true })
                $actions.Add(@{ Op = 'button'; Button = $button; Down = $false })
            }
            'doubleclick' {
                $button = Resolve-V9xButton $rest
                for ($i = 0; $i -lt 2; $i++) {
                    $actions.Add(@{ Op = 'button'; Button = $button; Down = $true })
                    $actions.Add(@{ Op = 'button'; Button = $button; Down = $false })
                }
            }
            'down' { $actions.Add(@{ Op = 'button'; Button = (Resolve-V9xButton $rest); Down = $true }) }
            'up' { $actions.Add(@{ Op = 'button'; Button = (Resolve-V9xButton $rest); Down = $false }) }
            'wheel' { $actions.Add(@{ Op = 'wheel'; Notches = [int]$rest }) }
            'delay' { $actions.Add(@{ Op = 'delay'; Ms = [int]$rest }) }
            'type' { $actions.Add(@{ Op = 'type'; Text = $rest }) }
            'key' {
                $names = $rest -split '\+'
                $vks = @($names | ForEach-Object { ConvertTo-V9xVirtualKey $_.Trim() })
                foreach ($vk in $vks) { $actions.Add(@{ Op = 'key'; Vk = $vk; Down = $true }) }
                for ($i = $vks.Count - 1; $i -ge 0; $i--) {
                    $actions.Add(@{ Op = 'key'; Vk = $vks[$i]; Down = $false })
                }
            }
            default { throw "Unknown input verb '$verb'." }
        }
    }
    if ($actions.Count -eq 0) { throw 'The input sequence contained no actions.' }
    return $actions.ToArray()
}

function Connect-V9xSession {
    param([string]$HostName, [int]$TcpPort, [int]$TimeoutSeconds)
    $sessionClient = [Net.Sockets.TcpClient]::new()
    try {
        $connect = $sessionClient.BeginConnect($HostName, $TcpPort, $null, $null)
        if (-not $connect.AsyncWaitHandle.WaitOne($TimeoutSeconds * 1000)) {
            throw [TimeoutException]::new("Connection to ${HostName}:$TcpPort timed out.")
        }
        $sessionClient.EndConnect($connect)
        $sessionStream = $sessionClient.GetStream()
        $sessionStream.ReadTimeout = $TimeoutSeconds * 1000
        $sessionStream.WriteTimeout = $TimeoutSeconds * 1000
        [uint32]$sessionRequestId = [uint32](Get-Random -Minimum 1 -Maximum ([int]::MaxValue))
        $helloBytes = New-V9xFrameBytes -Type 0x0001 -RequestId $sessionRequestId `
            -Payload (New-V9xHelloPayload)
        $sessionStream.Write($helloBytes, 0, $helloBytes.Length)
        $helloFrame = Read-V9xFrame -Stream $sessionStream
        if ($helloFrame.RequestId -ne $sessionRequestId -or $helloFrame.Type -ne 0x8001) {
            throw "Invalid HELLO response (type 0x$('{0:x4}' -f $helloFrame.Type))."
        }
        $sessionHello = ConvertFrom-V9xHelloPayload -Payload $helloFrame.Payload
        if ($sessionHello.ProtocolVersion -ne $script:V9xVersion) {
            throw 'No compatible protocol version.'
        }
        return [pscustomobject]@{
            Client = $sessionClient
            Stream = $sessionStream
            RequestId = $sessionRequestId
            Hello = $sessionHello
        }
    } catch {
        $sessionClient.Dispose()
        throw
    }
}

function Get-V9xLocalIPv4 {
    [Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces() |
        Where-Object {
            $_.OperationalStatus -eq [Net.NetworkInformation.OperationalStatus]::Up -and
            $_.NetworkInterfaceType -ne [Net.NetworkInformation.NetworkInterfaceType]::Loopback -and
            $_.SupportsMulticast
        } |
        ForEach-Object { $_.GetIPProperties().UnicastAddresses } |
        Where-Object { $_.Address.AddressFamily -eq [Net.Sockets.AddressFamily]::InterNetwork } |
        ForEach-Object { $_.Address }
}

# One-shot mDNS query (RFC 6762 5.1) for _v9xremote._tcp.local. It is sent from
# an ephemeral port, so agents answer by unicast straight back to it and the
# host's own mDNS service keeps port 5353. Multicast goes out of every up IPv4
# interface; -Target sends the query unicast to one address[:port] instead,
# for a guest behind a port forward or on a routed subnet. Returns one object
# per agent; stops early once InstallId (when given) has answered.
function Invoke-V9xDiscovery {
    param([int]$Seconds, [string]$Target, [string]$InstallId)
    $query = New-V9xMdnsQuery -Id ([uint16](Get-Random -Minimum 1 -Maximum 65535))
    $sockets = New-Object System.Collections.Generic.List[Net.Sockets.UdpClient]
    $replies = New-Object System.Collections.Generic.List[object]
    try {
        if ($Target) {
            $parts = $Target.Split(':')
            $targetPort = if ($parts.Count -gt 1) { [int]$parts[1] } else { 5353 }
            $endpoint = [Net.IPEndPoint]::new([Net.IPAddress]::Parse($parts[0]), $targetPort)
            $socket = [Net.Sockets.UdpClient]::new(0)
            $sockets.Add($socket)
            [void]$socket.Send($query, $query.Length, $endpoint)
        } else {
            $group = [Net.IPEndPoint]::new([Net.IPAddress]::Parse('224.0.0.251'), 5353)
            $locals = @(Get-V9xLocalIPv4)
            if ($locals.Count -eq 0) { $locals = @([Net.IPAddress]::Any) }
            foreach ($local in $locals) {
                $socket = $null
                try {
                    $socket = [Net.Sockets.UdpClient]::new([Net.IPEndPoint]::new($local, 0))
                    if ($local -ne [Net.IPAddress]::Any) {
                        $socket.Client.SetSocketOption([Net.Sockets.SocketOptionLevel]::IP,
                            [Net.Sockets.SocketOptionName]::MulticastInterface, $local.GetAddressBytes())
                    }
                    $socket.Client.SetSocketOption([Net.Sockets.SocketOptionLevel]::IP,
                        [Net.Sockets.SocketOptionName]::MulticastTimeToLive, 255)
                    [void]$socket.Send($query, $query.Length, $group)
                    $sockets.Add($socket)
                } catch {
                    if ($null -ne $socket) { $socket.Dispose() }
                }
            }
            if ($sockets.Count -eq 0) { throw [Net.Sockets.SocketException]::new(10051) }
        }
        $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
        while ([DateTime]::UtcNow -lt $deadline) {
            $idle = $true
            foreach ($socket in $sockets) {
                while ($socket.Available -gt 0) {
                    $idle = $false
                    $remote = [Net.IPEndPoint]::new([Net.IPAddress]::Any, 0)
                    try {
                        $bytes = $socket.Receive([ref]$remote)
                    } catch [Net.Sockets.SocketException] {
                        break   # ICMP unreachable surfaces as a reset; ignore it
                    }
                    try { $packet = ConvertFrom-V9xMdnsPacket -Packet $bytes } catch { continue }
                    if (-not $packet.IsResponse) { continue }
                    $replies.Add([pscustomobject]@{ Source = $remote.Address.ToString(); Packet = $packet })
                    if ($InstallId -and @(ConvertTo-V9xDiscoveredAgents -Replies $replies.ToArray() |
                            Where-Object { $_.InstallId -eq $InstallId }).Count -ne 0) {
                        $deadline = [DateTime]::UtcNow
                    }
                }
            }
            if ($idle) { Start-Sleep -Milliseconds 50 }
        }
    } finally {
        foreach ($socket in $sockets) { $socket.Dispose() }
    }
    return ConvertTo-V9xDiscoveredAgents -Replies $replies.ToArray()
}

# -Id: find the agent with that install ID and point -Host/-Port at it.
function Update-V9xTargetFromId {
    param([int]$Seconds)
    $found = @(Invoke-V9xDiscovery -Seconds $Seconds -Target $QueryAddress -InstallId $Id |
        Where-Object { $_.InstallId -eq $Id })
    if ($found.Count -eq 0) { return $false }
    $script:EndpointHost = $found[-1].Address
    $script:Port = [int]$found[-1].Port
    return $true
}

function Read-V9xOperationFrame {
    param(
        [IO.Stream]$Stream,
        [uint32]$RequestId,
        [uint16[]]$ExpectedTypes,
        [int]$GuestErrorExitCode = 23
    )
    $frame = Read-V9xFrame -Stream $Stream
    if ($frame.RequestId -ne $RequestId) {
        throw "Response request ID $($frame.RequestId) does not match $RequestId."
    }
    if ($frame.Type -eq 0x8fff) {
        $guestError = ConvertFrom-V9xErrorPayload -Payload $frame.Payload
        $exception = [InvalidOperationException]::new(
            "Guest error $($guestError.Status) (Win32 $($guestError.NativeError)): $($guestError.Detail)")
        $exception.Data['V9xExitCode'] = $GuestErrorExitCode
        throw $exception
    }
    if ($ExpectedTypes -notcontains $frame.Type) {
        throw "Unexpected response type 0x$('{0:x4}' -f $frame.Type)."
    }
    return $frame
}

function Invoke-V9xMkdirInternal {
    param([IO.Stream]$Stream, [ref]$RequestId, [string]$Path)
    [uint32]$id = $RequestId.Value
    $payload = New-V9xPathPayload -Path $Path
    $bytes = New-V9xFrameBytes -Type 0x0022 -RequestId $id -Payload $payload
    $Stream.Write($bytes, 0, $bytes.Length)
    $frame = Read-V9xOperationFrame -Stream $Stream -RequestId $id `
        -ExpectedTypes 0x8022 -GuestErrorExitCode 40
    if ($frame.Payload.Length -ne 4) { throw 'Invalid FILE_MKDIR_RESPONSE payload.' }
    $RequestId.Value = [uint32]($id + 1)
    return [bool][BitConverter]::ToUInt32($frame.Payload, 0)
}

function Invoke-V9xPutInternal {
    param([IO.Stream]$Stream, [ref]$RequestId, [string]$LocalPath,
          [string]$GuestPath)
    $file = Get-Item -LiteralPath $LocalPath -ErrorAction Stop
    if ($file.PSIsContainer) { throw "Local source is a directory: $LocalPath" }
    if ($file.Length -gt 268435456) { throw "Local source exceeds the 256 MiB protocol limit: $LocalPath" }
    [uint32]$size = $file.Length
    [uint32]$crc = Get-V9xFileCrc32 -LiteralPath $file.FullName
    [uint32]$id = $RequestId.Value
    $payload = New-V9xOpenWritePayload -Path $GuestPath -Size $size -Crc32 $crc
    $bytes = New-V9xFrameBytes -Type 0x0023 -RequestId $id -Payload $payload
    $Stream.Write($bytes, 0, $bytes.Length)
    $frame = Read-V9xOperationFrame -Stream $Stream -RequestId $id `
        -ExpectedTypes 0x8023 -GuestErrorExitCode 40
    if ($frame.Payload.Length -ne 8 -or
        [BitConverter]::ToUInt32($frame.Payload, 0) -ne $size -or
        [BitConverter]::ToUInt32($frame.Payload, 4) -ne $crc) {
        throw 'Guest upload-ready metadata does not match the local file.'
    }

    $input = [IO.File]::OpenRead($file.FullName)
    try {
        $buffer = [byte[]]::new(32768)
        [uint32]$offset = 0
        while (($count = $input.Read($buffer, 0, $buffer.Length)) -ne 0) {
            ++$id
            $chunk = [byte[]]::new($count + 4)
            [Array]::Copy([BitConverter]::GetBytes($offset), 0, $chunk, 0, 4)
            [Array]::Copy($buffer, 0, $chunk, 4, $count)
            $bytes = New-V9xFrameBytes -Type 0x0024 -RequestId $id -Payload $chunk
            $Stream.Write($bytes, 0, $bytes.Length)
            $frame = Read-V9xOperationFrame -Stream $Stream -RequestId $id `
                -ExpectedTypes 0x8024 -GuestErrorExitCode 40
            $offset = [uint32]($offset + $count)
            if ($frame.Payload.Length -ne 4 -or
                [BitConverter]::ToUInt32($frame.Payload, 0) -ne $offset) {
                throw 'Guest upload acknowledgement has the wrong offset.'
            }
        }
    } finally {
        $input.Dispose()
    }
    ++$id
    $bytes = New-V9xFrameBytes -Type 0x0025 -RequestId $id
    $Stream.Write($bytes, 0, $bytes.Length)
    $frame = Read-V9xOperationFrame -Stream $Stream -RequestId $id `
        -ExpectedTypes 0x8025 -GuestErrorExitCode 40
    if ($frame.Payload.Length -ne 8 -or
        [BitConverter]::ToUInt32($frame.Payload, 0) -ne $size -or
        [BitConverter]::ToUInt32($frame.Payload, 4) -ne $crc) {
        throw 'Guest upload completion metadata does not match the local file.'
    }
    $RequestId.Value = [uint32]($id + 1)
    return [pscustomobject]@{ Size = $size; Crc32 = $crc; Source = $file.FullName; Destination = $GuestPath }
}

function Invoke-V9xGetInternal {
    param([IO.Stream]$Stream, [ref]$RequestId, [string]$GuestPath,
          [string]$LocalPath)
    [uint32]$id = $RequestId.Value
    $destinationFull = [IO.Path]::GetFullPath($LocalPath)
    $destinationParent = Split-Path -Parent $destinationFull
    if ($destinationParent -and -not (Test-Path -LiteralPath $destinationParent)) {
        New-Item -ItemType Directory -Force -Path $destinationParent | Out-Null
    }
    $temporary = $destinationFull + '.v9x.part.' + [Guid]::NewGuid().ToString('N')
    try {
        $payload = New-V9xPathPayload -Path $GuestPath
        $bytes = New-V9xFrameBytes -Type 0x0026 -RequestId $id -Payload $payload
        $Stream.Write($bytes, 0, $bytes.Length)
        $output = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew,
                                 [IO.FileAccess]::Write, [IO.FileShare]::None)
        [uint32]$received = 0
        [uint32]$runningCrc = [uint32]::MaxValue
        $completeFrame = $null
        try {
            while ($null -eq $completeFrame) {
                $frame = Read-V9xOperationFrame -Stream $Stream -RequestId $id `
                    -ExpectedTypes 0x9026,0x8026 -GuestErrorExitCode 40
                if ($frame.Type -eq 0x9026) {
                    if ($frame.Payload.Length -lt 4 -or
                        [BitConverter]::ToUInt32($frame.Payload, 0) -ne $received) {
                        throw 'Download chunk offset mismatch.'
                    }
                    $count = $frame.Payload.Length - 4
                    $output.Write($frame.Payload, 4, $count)
                    $runningCrc = Update-V9xCrc32 -Crc $runningCrc `
                        -Bytes $frame.Payload -Offset 4 -Count $count
                    $received = [uint32]($received + $count)
                } else {
                    $completeFrame = $frame
                }
            }
        } finally {
            $output.Dispose()
        }
        if ($completeFrame.Payload.Length -ne 8) { throw 'Invalid FILE_READ_COMPLETE payload.' }
        [uint32]$expectedSize = [BitConverter]::ToUInt32($completeFrame.Payload, 0)
        [uint32]$expectedCrc = [BitConverter]::ToUInt32($completeFrame.Payload, 4)
        [uint32]$actualCrc = $runningCrc -bxor [uint32]::MaxValue
        if ($received -ne $expectedSize -or $actualCrc -ne $expectedCrc) {
            $exception = [InvalidDataException]::new('Downloaded file size or CRC32 does not match the guest.')
            $exception.Data['V9xExitCode'] = 40
            throw $exception
        }
        Move-Item -LiteralPath $temporary -Destination $destinationFull -Force
        $temporary = $null
        $RequestId.Value = [uint32]($id + 1)
        return [pscustomobject]@{
            Source = $GuestPath; Destination = $destinationFull
            Size = $received; Crc32 = $actualCrc
        }
    } finally {
        if ($temporary -and (Test-Path -LiteralPath $temporary -PathType Leaf)) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
}

function Join-V9xGuestPath {
    param([string]$Parent, [string]$Child)
    if ([string]::IsNullOrEmpty($Child)) { return $Parent }
    if ($Parent.EndsWith('\')) { return $Parent + $Child.TrimStart('\') }
    return $Parent + '\' + $Child.TrimStart('\')
}

function Initialize-V9xGuestDirectory {
    param([IO.Stream]$Stream, [ref]$RequestId, [string]$Path)
    $normalized = $Path.Replace('/', '\').TrimEnd('\')
    if ($normalized -notmatch '^([A-Za-z]:\\)(.*)$') {
        [void](Invoke-V9xMkdirInternal -Stream $Stream -RequestId $RequestId -Path $normalized)
        return
    }
    $current = $Matches[1]
    foreach ($segment in ($Matches[2] -split '\\' | Where-Object { $_ })) {
        $current = Join-V9xGuestPath -Parent $current -Child $segment
        [void](Invoke-V9xMkdirInternal -Stream $Stream -RequestId $RequestId -Path $current)
    }
}

# Verb catalogue: drives usage output and required-parameter checks. Order is
# the order verbs are listed in the help text.
$script:V9xVerbs = [ordered]@{
    'discover' = @{
        Purpose = 'Find agents on the LAN by mDNS (agent 0.8.0+ with discovery=1); no connection'
        Usage = 'discover [-DiscoverSeconds <n>] [-QueryAddress <ip[:port]>] [-Id <install id>]'
        Example = 'discover -DiscoverSeconds 5 -Json'
    }
    'ping' = @{
        Purpose = 'Liveness check; returns uptime and boot counter'
        Usage = 'ping'
        Example = 'ping -Host 127.0.0.1 -Port 9869'
    }
    'info' = @{
        Purpose = 'Agent version, build ID, computer name, screen mode, pending job'
        Usage = 'info'
        Example = 'info -Json'
    }
    'inventory' = @{
        Purpose = 'Hardware/software inventory: OS build, CPU, memory, drives, display, IPs, BIOS, devices (agent 0.7.0+)'
        Usage = 'inventory'
        Example = 'inventory -Json'
    }
    'exec' = @{
        Purpose = 'Run a Win32 EXE directly and capture stdout/stderr'
        Usage = 'exec -Application <guest EXE> [-Arguments <text>] [-WorkingDirectory <guest dir>] [-TimeoutSeconds <n>] [-ExpectedExitCode <n>] [-ShowWindow] [-Detach]'
        Example = 'exec -Application C:\WINDOWS\NOTEPAD.EXE -Arguments C:\README.TXT -TimeoutSeconds 30 -Json'
        Required = @{ Application = '-Application' }
    }
    'shell' = @{
        Purpose = 'Run a command through COMMAND.COM /C, or CMD.EXE /C on Windows 2000 (built-ins, batch files)'
        Usage = 'shell -Command <text> [-TimeoutSeconds <n>] [-Detach]'
        Example = 'shell -Command "DIR C:\ /W" -Json'
        Required = @{ ShellCommand = '-Command' }
    }
    'stat' = @{
        Purpose = 'Existence, size and attributes of one guest path'
        Usage = 'stat -Path <guest path>'
        Example = 'stat -Path C:\WINDOWS\WIN.INI'
        Required = @{ RemotePath = '-Path' }
    }
    'list' = @{
        Purpose = 'Directory listing (bounded to 16 KiB)'
        Usage = 'list -Path <guest dir>'
        Example = 'list -Path C:\V9XREMOTE\JOBS -Json'
        Required = @{ RemotePath = '-Path' }
    }
    'mkdir' = @{
        Purpose = 'Create a guest directory (idempotent)'
        Usage = 'mkdir -Path <guest dir>'
        Example = 'mkdir -Path C:\V9XREMOTE\JOBS\myjob'
        Required = @{ RemotePath = '-Path' }
    }
    'put' = @{
        Purpose = 'Upload one file to the guest, CRC32-verified'
        Usage = 'put -Source <local file> -Destination <guest path>'
        Example = 'put -Source .\out\MYAPP.EXE -Destination C:\V9XREMOTE\JOBS\myjob\MYAPP.EXE'
        Required = @{ Source = '-Source'; Destination = '-Destination' }
    }
    'get' = @{
        Purpose = 'Download one file from the guest, CRC32-verified'
        Usage = 'get -Source <guest path> -Destination <local file>'
        Example = 'get -Source C:\V9XREMOTE\AGENT.LOG -Destination .\agent.log'
        Required = @{ Source = '-Source'; Destination = '-Destination' }
    }
    'push-tree' = @{
        Purpose = 'Upload a directory tree'
        Usage = 'push-tree -Source <local dir> -Destination <guest dir>'
        Example = 'push-tree -Source .\build\install -Destination C:\V9XREMOTE\JOBS\install'
        Required = @{ Source = '-Source'; Destination = '-Destination' }
    }
    'download' = @{
        Purpose = 'Guest fetches an http:// URL to a guest path'
        Usage = 'download -Url <http://...> -Destination <guest path>'
        Example = 'download -Url http://10.0.2.2:8000/DRIVER.ZIP -Destination C:\V9XREMOTE\JOBS\drv\DRIVER.ZIP'
        Required = @{ Url = '-Url'; Destination = '-Destination' }
    }
    'update' = @{
        Purpose = 'Upgrade the agent from an install-package folder (hot; staged with a reboot for agents before 0.6.0)'
        Usage = 'update -Source <install package dir> [-WaitSeconds <n>]'
        Example = 'update -Source .\build\install -WaitSeconds 60 -Json'
        Required = @{ Source = '-Source' }
    }
    'reboot' = @{
        Purpose = 'Reboot the guest and prove it came back'
        Usage = 'reboot [-JobId <token>] [-WaitSeconds <n>]'
        Example = 'reboot -JobId myjob-reboot-1 -WaitSeconds 180 -Json'
    }
    'shutdown' = @{
        Purpose = 'Controlled shutdown'
        Usage = 'shutdown [-JobId <token>]'
        Example = 'shutdown -JobId myjob-off-1'
    }
    'wait-desktop' = @{
        Purpose = 'Block until the Explorer desktop is ready'
        Usage = 'wait-desktop [-WaitSeconds <n>]'
        Example = 'wait-desktop -WaitSeconds 120 -Json'
    }
    'screenshot' = @{
        Purpose = 'Capture the guest screen (PNG/JPEG/GIF/TIFF/BMP by extension)'
        Usage = 'screenshot -OutFile <local image>'
        Example = 'screenshot -OutFile .\shot.png'
        Required = @{ Destination = '-OutFile' }
    }
    'input' = @{
        Purpose = 'Inject mouse/keyboard actions'
        Usage = 'input -Sequence "<verb args>; <verb args>; ..."'
        Example = 'input -Sequence "move 100,200; click left; type Hello; key ENTER"'
        Required = @{ Sequence = '-Sequence' }
    }
}

# The version lives in include\v9xremote\version.h (the single source of
# truth); read it rather than duplicating it here.
function Get-V9xToolVersion {
    $header = Join-Path (Split-Path -Parent $PSScriptRoot) 'include\v9xremote\version.h'
    if (Test-Path -LiteralPath $header -PathType Leaf) {
        $text = Get-Content -LiteralPath $header -Raw
        if ($text -match '#define V9X_AGENT_VERSION "([^"]+)"') { return $Matches[1] }
    }
    return 'unknown'
}

function Write-V9xUsage {
    param([IO.TextWriter]$Writer, [string]$Problem)
    if ($Problem) {
        $Writer.WriteLine("Usage error: $Problem")
        $Writer.WriteLine()
    }
    $Writer.WriteLine("v9xctl $(Get-V9xToolVersion) - drive a Windows 9x guest running the V9x Remote Agent")
    $Writer.WriteLine("Protocol version $('{0}.{1}' -f ($script:V9xVersion -shr 8), ($script:V9xVersion -band 0xFF))")
    $Writer.WriteLine()
    $Writer.WriteLine('  .\scripts\v9xctl.ps1 <verb> [verb options] [-Host <address>] [-Port <n>] [-Json]')
    $Writer.WriteLine()
    $Writer.WriteLine('Connection (every call opens a fresh connection; the target is chosen per call):')
    $Writer.WriteLine('  -Host <address>             agent address; default 127.0.0.1 (emulator port forward)')
    $Writer.WriteLine('  -Port <n>                   agent TCP port; default 9869 (AGENT.INI "port")')
    $Writer.WriteLine('  -ConnectTimeoutSeconds <n>  connect/response timeout; default 10')
    $Writer.WriteLine('  -Id <install id>            find the agent by install ID over mDNS instead of -Host/-Port')
    $Writer.WriteLine('  -QueryAddress <ip[:port]>   send the mDNS query to one address, not multicast')
    $Writer.WriteLine('  -DiscoverSeconds <n>        how long discovery listens; default 3')
    $Writer.WriteLine('  -Json                       compact JSON output for scripts and agents')
    $Writer.WriteLine()
    $Writer.WriteLine('Verbs:')
    foreach ($name in $script:V9xVerbs.Keys) {
        $Writer.WriteLine(('  {0,-14}{1}' -f $name, $script:V9xVerbs[$name].Purpose))
    }
    $Writer.WriteLine(('  {0,-14}{1}' -f 'help', 'This text (also: -? and Get-Help .\scripts\v9xctl.ps1 -Examples)'))
    $Writer.WriteLine()
    $Writer.WriteLine('Examples:')
    $Writer.WriteLine('  .\scripts\v9xctl.ps1 ping')
    $Writer.WriteLine('  .\scripts\v9xctl.ps1 info -Host 192.168.10.98 -Port 9869 -Json')
    $Writer.WriteLine('  .\scripts\v9xctl.ps1 exec -Application C:\WINDOWS\NOTEPAD.EXE -TimeoutSeconds 30 -Json')
    $Writer.WriteLine('  .\scripts\v9xctl.ps1 shell -Command "DIR C:\ /W"')
    $Writer.WriteLine('  .\scripts\v9xctl.ps1 put -Source .\out\MYAPP.EXE -Destination C:\V9XREMOTE\JOBS\myjob\MYAPP.EXE')
    $Writer.WriteLine('  .\scripts\v9xctl.ps1 screenshot -OutFile .\shot.png -Host 127.0.0.1 -Port 9869')
    $Writer.WriteLine('  .\scripts\v9xctl.ps1 reboot -JobId myjob-reboot-1 -WaitSeconds 180 -Json')
    $Writer.WriteLine()
    $Writer.WriteLine('Exit codes: 0 ok, 10 usage, 20 agent unreachable, 22/23 protocol, 30-33 execution,')
    $Writer.WriteLine('40 file transfer, 41-42 power control, 43 screenshot, 44 desktop not ready.')
    $Writer.WriteLine('Full reference: AGENTS.md and docs\host-cli.md.')
}

function Write-V9xVerbUsage {
    param([IO.TextWriter]$Writer, [string]$Verb, [string]$Problem)
    $entry = $script:V9xVerbs[$Verb]
    $Writer.WriteLine("Usage error: $Problem")
    $Writer.WriteLine()
    $Writer.WriteLine("  $($entry.Purpose).")
    $Writer.WriteLine()
    $Writer.WriteLine("  .\scripts\v9xctl.ps1 $($entry.Usage) [-Host <address>] [-Port <n>] [-Json]")
    $Writer.WriteLine()
    $Writer.WriteLine('Example:')
    $Writer.WriteLine("  .\scripts\v9xctl.ps1 $($entry.Example)")
    $Writer.WriteLine()
    $Writer.WriteLine("Target: -Host $EndpointHost -Port $Port (defaults 127.0.0.1 and 9869). Run '.\scripts\v9xctl.ps1 help' for all verbs.")
}

if ([string]::IsNullOrWhiteSpace($Action) -or $Action -in @('help', '-h', '--help', '/?')) {
    if ([string]::IsNullOrWhiteSpace($Action)) {
        Write-V9xUsage -Writer ([Console]::Error) -Problem 'no verb given.'
        exit 10
    }
    Write-V9xUsage -Writer ([Console]::Out)
    exit 0
}
$Action = $Action.ToLowerInvariant()
if (-not $script:V9xVerbs.Contains($Action)) {
    Write-V9xUsage -Writer ([Console]::Error) -Problem "unknown verb '$Action'."
    exit 10
}
$verbEntry = $script:V9xVerbs[$Action]
if ($verbEntry.ContainsKey('Required')) {
    $missing = @($verbEntry.Required.Keys |
        Where-Object { [string]::IsNullOrWhiteSpace((Get-Variable -Name $_ -ValueOnly)) } |
        ForEach-Object { $verbEntry.Required[$_] } | Sort-Object)
    if ($missing.Count -ne 0) {
        Write-V9xVerbUsage -Writer ([Console]::Error) -Verb $Action `
            -Problem "$Action requires $($missing -join ' and ')."
        exit 10
    }
}
if ($Action -eq 'put' -and -not (Test-Path -LiteralPath $Source -PathType Leaf)) {
    [Console]::Error.WriteLine("Local source file not found: $Source")
    exit 11
}
if ($Action -eq 'push-tree' -and -not (Test-Path -LiteralPath $Source -PathType Container)) {
    [Console]::Error.WriteLine("Local source directory not found: $Source")
    exit 11
}

if ($Action -eq 'discover') {
    try {
        $agents = @(Invoke-V9xDiscovery -Seconds $DiscoverSeconds -Target $QueryAddress -InstallId $Id)
    } catch [Net.Sockets.SocketException] {
        [Console]::Error.WriteLine("Discovery error: $($_.Exception.Message)")
        exit 20
    }
    if ($Id) { $agents = @($agents | Where-Object { $_.InstallId -eq $Id }) }
    if ($Json) {
        [pscustomobject]@{ Success = $true; Count = $agents.Count; Agents = $agents } |
            ConvertTo-Json -Depth 4 -Compress
    } elseif ($agents.Count -eq 0) {
        [Console]::Error.WriteLine('No agents answered. Discovery needs discovery=1 in the guest''s AGENT.INI and a LAN the host shares with it (bridged or physical, not SLiRP NAT); -QueryAddress <ip[:port]> queries one address directly.')
    } else {
        $agents | Format-Table Instance, Address, Port, AgentVersion, InstallId -AutoSize
    }
    exit $(if ($agents.Count -eq 0) { 20 } else { 0 })
}
if ($Id) {
    $resolved = $false
    try { $resolved = Update-V9xTargetFromId -Seconds $DiscoverSeconds } catch [Net.Sockets.SocketException] { }
    if (-not $resolved) {
        [Console]::Error.WriteLine("No agent with install ID $Id answered discovery within $DiscoverSeconds s.")
        exit 20
    }
}

$legacyUpdate = $false
if ($Action -eq 'update') {
    # Hot update arrived in 0.6.0, but the capability bit it is checked by
    # (0x200, DRIVER_UPDATE) is older: 0.5.x sets it too and answers the
    # UPDATE request with UNSUPPORTED_OPERATION, after both binaries have been
    # uploaded. So decide by the version INFO reports. Asked before this
    # verb's own connection opens, because 0.5.x serves one client at a time.
    # Hashtable splatting: an array splatted into a script passes '-Host' as
    # a positional value, not as a parameter name.
    $target = @{ EndpointHost = $EndpointHost; Port = $Port
                 ConnectTimeoutSeconds = $ConnectTimeoutSeconds; Json = $true }
    if ($Id) { $target['Id'] = $Id }
    $agentInfo = $null
    try { $agentInfo = (& $PSCommandPath -Action info @target) -join "`n" | ConvertFrom-Json } catch { }
    if ($null -ne $agentInfo) {
        $agentVersion = $null
        try { $agentVersion = [version]$agentInfo.AgentVersion } catch { }
        $legacyUpdate = ([uint32]$agentInfo.Capabilities -band [uint32]0x00000200) -eq 0 -or
            ($null -ne $agentVersion -and $agentVersion -lt [version]'0.6.0')
    }
}

$client = $null
$downloadTemp = $null
$exitCategory = 22
try {
    $session = Connect-V9xSession -HostName $EndpointHost -TcpPort $Port `
        -TimeoutSeconds $ConnectTimeoutSeconds
    $client = $session.Client
    $stream = $session.Stream
    [uint32]$requestId = $session.RequestId
    $hello = $session.Hello

    ++$requestId
    if ($Action -in @('ping', 'info')) {
        $requestType = if ($Action -eq 'ping') { [uint16]0x0002 } else { [uint16]0x0030 }
        $expectedType = if ($Action -eq 'ping') { [uint16]0x8002 } else { [uint16]0x8030 }
        # INSTALL_ID (0x4000) agents take a flags word; bit 0 asks for the
        # install ID after allowed_client. Older agents require an empty body.
        $requestPayload = [byte[]]@()
        if ($Action -eq 'info' -and ($hello.Capabilities -band [uint32]0x00004000) -ne 0) {
            $requestPayload = [BitConverter]::GetBytes([uint32]1)
        }
        $requestBytes = New-V9xFrameBytes -Type $requestType -RequestId $requestId -Payload $requestPayload
        $stream.Write($requestBytes, 0, $requestBytes.Length)
        $frame = Read-V9xFrame -Stream $stream
        if ($frame.RequestId -ne $requestId) { throw 'Response request ID does not match.' }
        if ($frame.Type -eq 0x8fff) {
            $exitCategory = 23
            $guestError = ConvertFrom-V9xErrorPayload -Payload $frame.Payload
            throw "Guest error $($guestError.Status) (Win32 $($guestError.NativeError)): $($guestError.Detail)"
        }
        if ($frame.Type -ne $expectedType) { throw "Unexpected response type 0x$('{0:x4}' -f $frame.Type)." }

        if ($Action -eq 'ping') {
            if ($frame.Payload.Length -ne 8) { throw 'Invalid PING response length.' }
            $result = [pscustomobject]@{
                Success = $true
                RequestId = $requestId
                AgentBuild = $hello.BuildId
                BootCounter = [BitConverter]::ToUInt32($frame.Payload, 4)
                UptimeMilliseconds = [BitConverter]::ToUInt32($frame.Payload, 0)
                Endpoint = "${EndpointHost}:$Port"
            }
        } else {
            $info = ConvertFrom-V9xInfoPayload -Payload $frame.Payload
            $result = [pscustomobject]@{
                Success = $true
                RequestId = $requestId
                ProtocolVersion = $hello.ProtocolVersion
                MaxPayload = $hello.MaxPayload
                BootCounter = $info.BootCounter
                UptimeMilliseconds = $info.UptimeMilliseconds
                Capabilities = $info.Capabilities
                AgentVersion = $info.AgentVersion
                BuildId = $info.BuildId
                ComputerName = $info.ComputerName
                WindowsVersion = $info.WindowsVersion
                RawWindowsVersion = $info.RawWindowsVersion
                SystemDirectory = $info.SystemDirectory
                WindowsDirectory = $info.WindowsDirectory
                CurrentDirectory = $info.CurrentDirectory
                WinsockVersion = $info.WinsockVersion
                Port = $info.Port
                PendingJob = $info.PendingJob
                DesktopReady = $info.DesktopReady
                ScreenWidth = $info.ScreenWidth
                ScreenHeight = $info.ScreenHeight
                BitsPerPixel = $info.BitsPerPixel
                ListenAddress = $info.ListenAddress
                AllowedClient = $info.AllowedClient
                InstallId = $info.InstallId
            }
        }
        if ($Json) { $result | ConvertTo-Json -Depth 4 -Compress } else { $result | Format-List }
        exit 0
    }

    if ($Action -eq 'inventory') {
        if (($hello.Capabilities -band [uint32]0x00002000) -eq 0) {
            $exitCategory = 23
            throw "Agent build '$($hello.BuildId)' does not advertise inventory support (needs agent 0.7.0+)."
        }
        $bytes = New-V9xFrameBytes -Type 0x0070 -RequestId $requestId
        $stream.Write($bytes, 0, $bytes.Length)
        $frame = Read-V9xOperationFrame -Stream $stream -RequestId $requestId `
            -ExpectedTypes 0x8070 -GuestErrorExitCode 23
        $records = ConvertFrom-V9xInventoryPayload -Payload $frame.Payload
        if ($Json) {
            [pscustomobject]@{
                Success = $true; RequestId = $requestId; AgentBuild = $hello.BuildId
                Endpoint = "${EndpointHost}:$Port"; Records = $records
            } | ConvertTo-Json -Depth 6 -Compress
        } else {
            foreach ($record in $records) {
                $pairs = foreach ($property in $record.Fields.PSObject.Properties) {
                    "$($property.Name)=$(@($property.Value) -join ',')"
                }
                '{0,-8} {1}' -f $record.Record, ($pairs -join '  ')
            }
        }
        exit 0
    }

    if ($Action -eq 'wait-desktop') {
        if (($hello.Capabilities -band [uint32]0x00000001) -eq 0) {
            $exitCategory = 23
            throw "Agent build '$($hello.BuildId)' does not advertise information support."
        }
        $deadline = [DateTime]::UtcNow.AddSeconds($WaitSeconds)
        $info = $null
        while ([DateTime]::UtcNow -lt $deadline) {
            $bytes = New-V9xFrameBytes -Type 0x0030 -RequestId $requestId
            $stream.Write($bytes, 0, $bytes.Length)
            $frame = Read-V9xOperationFrame -Stream $stream -RequestId $requestId `
                -ExpectedTypes 0x8030 -GuestErrorExitCode 44
            $info = ConvertFrom-V9xInfoPayload -Payload $frame.Payload
            if ($info.DesktopReady) { break }
            ++$requestId
            Start-Sleep -Milliseconds 1000
        }
        if ($null -eq $info -or -not $info.DesktopReady) {
            $exception = [InvalidOperationException]::new(
                "Windows desktop did not become ready within $WaitSeconds seconds.")
            $exception.Data['V9xExitCode'] = 44
            throw $exception
        }
        $result = [pscustomobject]@{
            Success = $true; RequestId = $requestId; AgentBuild = $info.BuildId
            BootCounter = $info.BootCounter; PendingJob = $info.PendingJob
            DesktopReady = $info.DesktopReady; ScreenWidth = $info.ScreenWidth
            ScreenHeight = $info.ScreenHeight; BitsPerPixel = $info.BitsPerPixel
            Endpoint = "${EndpointHost}:$Port"
        }
        if ($Json) { $result | ConvertTo-Json -Depth 4 -Compress } else { $result | Format-List }
        exit 0
    }

    if ($Action -in @('reboot', 'shutdown')) {
        if (($hello.Capabilities -band [uint32]0x00000080) -eq 0) {
            $exitCategory = 23
            throw "Agent build '$($hello.BuildId)' does not advertise power-control support."
        }
        if ([string]::IsNullOrWhiteSpace($JobId)) {
            $JobId = ('m4-{0}-{1:x4}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'),
                      (Get-Random -Minimum 1 -Maximum 65535))
        }
        if ([Text.Encoding]::ASCII.GetByteCount($JobId) -gt 63) {
            $exitCategory = 10
            throw 'JobId must be at most 63 ASCII bytes.'
        }
        $powerType = if ($Action -eq 'reboot') { [uint16]0x0040 } else { [uint16]0x0041 }
        $powerResponse = if ($Action -eq 'reboot') { [uint16]0x8040 } else { [uint16]0x8041 }
        $bytes = New-V9xFrameBytes -Type $powerType -RequestId $requestId `
            -Payload (New-V9xPathPayload -Path $JobId)
        $stream.Write($bytes, 0, $bytes.Length)
        $frame = Read-V9xOperationFrame -Stream $stream -RequestId $requestId `
            -ExpectedTypes $powerResponse -GuestErrorExitCode 41
        if ($frame.Payload.Length -lt 6) { throw 'Truncated power-control acceptance payload.' }
        $acceptedBootCounter = [BitConverter]::ToUInt32($frame.Payload, 0)
        $powerOffset = 4
        $acceptedJob = Read-V9xString -Bytes $frame.Payload -Offset ([ref]$powerOffset)
        if ($powerOffset -ne $frame.Payload.Length -or $acceptedJob -ne $JobId) {
            throw 'Power-control acceptance token does not match the request.'
        }

        if ($Action -eq 'shutdown') {
            $result = [pscustomobject]@{
                Success = $true; RequestId = $requestId; AgentBuild = $hello.BuildId
                Action = 'Shutdown'; JobId = $JobId; BootCounter = $acceptedBootCounter
                Accepted = $true; Endpoint = "${EndpointHost}:$Port"
            }
        } else {
            $oldBootCounter = $hello.BootCounter
            $client.Dispose()
            $client = $null
            $deadline = [DateTime]::UtcNow.AddSeconds($WaitSeconds)
            $resumed = $null
            $lastReconnectError = $null
            while ([DateTime]::UtcNow -lt $deadline -and $null -eq $resumed) {
                Start-Sleep -Milliseconds 500
                try {
                    $candidate = Connect-V9xSession -HostName $EndpointHost -TcpPort $Port `
                        -TimeoutSeconds ([Math]::Min($ConnectTimeoutSeconds, 3))
                    if ($candidate.Hello.BootCounter -ne $oldBootCounter -and
                        $candidate.Hello.PendingJob -eq $JobId) {
                        $resumed = $candidate
                    } else {
                        $candidate.Client.Dispose()
                    }
                } catch {
                    $lastReconnectError = $_.Exception.Message
                    # -Id: the guest may come back on a new address (DHCP); look it up again.
                    if ($Id) { try { [void](Update-V9xTargetFromId -Seconds 2) } catch { } }
                }
            }
            if ($null -eq $resumed) {
                $exception = [InvalidOperationException]::new(
                    "Guest did not reconnect with a new boot counter and resume token '$JobId' within $WaitSeconds seconds. Last error: $lastReconnectError")
                $exception.Data['V9xExitCode'] = 42
                throw $exception
            }
            $client = $resumed.Client
            $result = [pscustomobject]@{
                Success = $true; RequestId = $requestId; AgentBuild = $resumed.Hello.BuildId
                Action = 'Reboot'; JobId = $JobId; PreviousBootCounter = $oldBootCounter
                BootCounter = $resumed.Hello.BootCounter; PendingJob = $resumed.Hello.PendingJob
                DesktopReady = $resumed.Hello.DesktopReady
                ScreenWidth = $resumed.Hello.ScreenWidth; ScreenHeight = $resumed.Hello.ScreenHeight
                BitsPerPixel = $resumed.Hello.BitsPerPixel; Reconnected = $true
                ListenAddress = $resumed.Hello.ListenAddress
                AllowedClient = $resumed.Hello.AllowedClient
                Endpoint = "${EndpointHost}:$Port"
            }
        }
        if ($Json) { $result | ConvertTo-Json -Depth 4 -Compress } else { $result | Format-List }
        exit 0
    }

    if ($Action -eq 'screenshot') {
        if (($hello.Capabilities -band [uint32]0x00000100) -eq 0) {
            $exitCategory = 23
            throw "Agent build '$($hello.BuildId)' does not advertise screenshot support."
        }
        $guestScreenshot = if ([string]::IsNullOrWhiteSpace($RemotePath)) {
            'C:\V9XREMOTE\TEMP\SCREEN.BMP'
        } else { $RemotePath }
        $bytes = New-V9xFrameBytes -Type 0x0050 -RequestId $requestId `
            -Payload (New-V9xPathPayload -Path $guestScreenshot)
        $stream.Write($bytes, 0, $bytes.Length)
        $frame = Read-V9xOperationFrame -Stream $stream -RequestId $requestId `
            -ExpectedTypes 0x8050 -GuestErrorExitCode 43
        $capture = ConvertFrom-V9xScreenshotPayload -Payload $frame.Payload
        $formatMap = @{
            '.png' = 'Png'; '.jpg' = 'Jpeg'; '.jpeg' = 'Jpeg'; '.gif' = 'Gif'
            '.tif' = 'Tiff'; '.tiff' = 'Tiff'
        }
        $outExtension = [IO.Path]::GetExtension($Destination).ToLowerInvariant()
        $convertTo = if ($formatMap.ContainsKey($outExtension)) { $formatMap[$outExtension] } else { $null }
        $localTarget = if ($convertTo) {
            Join-Path ([IO.Path]::GetTempPath()) ("v9x-screenshot-{0}.bmp" -f ([Guid]::NewGuid().ToString('N')))
        } else { $Destination }
        ++$requestId
        try {
            $transfer = Invoke-V9xGetInternal -Stream $stream -RequestId ([ref]$requestId) `
                -GuestPath $capture.GuestPath -LocalPath $localTarget
            if ($transfer.Size -ne $capture.FileBytes -or $transfer.Crc32 -ne $capture.Crc32) {
                $exception = [InvalidDataException]::new(
                    'Downloaded screenshot metadata does not match the capture response.')
                $exception.Data['V9xExitCode'] = 40
                throw $exception
            }
            if ($convertTo) {
                try {
                    Add-Type -AssemblyName System.Drawing
                    $sourceImage = [Drawing.Image]::FromFile($localTarget)
                    try {
                        $bitmap = [Drawing.Bitmap]::new($sourceImage)
                        try {
                            $imageFormat = [Drawing.Imaging.ImageFormat]::$convertTo
                            $bitmap.Save($Destination, $imageFormat)
                        } finally { $bitmap.Dispose() }
                    } finally { $sourceImage.Dispose() }
                } catch {
                    $exception = [InvalidDataException]::new(
                        "Failed to convert screenshot BMP to $convertTo`: $($_.Exception.Message)")
                    $exception.Data['V9xExitCode'] = 40
                    throw $exception
                }
            }
        } finally {
            if ($convertTo -and (Test-Path -LiteralPath $localTarget)) {
                Remove-Item -LiteralPath $localTarget -Force -ErrorAction SilentlyContinue
            }
        }
        $result = [pscustomobject]@{
            Success = $true; RequestId = [uint32]($requestId - 1); AgentBuild = $hello.BuildId
            GuestPath = $capture.GuestPath
            Destination = if ($convertTo) { (Resolve-Path -LiteralPath $Destination).Path } else { $transfer.Destination }
            Format = if ($convertTo) { $convertTo.ToLowerInvariant() } else { 'bmp' }
            Width = $capture.Width; Height = $capture.Height
            SourceBitsPerPixel = $capture.SourceBitsPerPixel; Bytes = $capture.FileBytes
            Crc32 = ('{0:X8}' -f $capture.Crc32)
        }
        if ($Json) { $result | ConvertTo-Json -Depth 4 -Compress } else { $result | Format-List }
        exit 0
    }

    if ($Action -eq 'input') {
        if (($hello.Capabilities -band [uint32]0x00000400) -eq 0) {
            $exitCategory = 23
            throw "Agent build '$($hello.BuildId)' does not advertise input support."
        }
        if ([string]::IsNullOrWhiteSpace($Sequence)) {
            $exitCategory = 10
            throw 'input requires -Sequence, e.g. "move 100,200; click left; type Hello; key ENTER".'
        }
        $actions = ConvertTo-V9xInputActions -Sequence $Sequence
        $payload = New-V9xInputPayload -Actions $actions
        $bytes = New-V9xFrameBytes -Type 0x0060 -RequestId $requestId -Payload $payload
        $stream.Write($bytes, 0, $bytes.Length)
        $frame = Read-V9xOperationFrame -Stream $stream -RequestId $requestId `
            -ExpectedTypes 0x8060 -GuestErrorExitCode 23
        $inputResult = ConvertFrom-V9xInputPayload -Payload $frame.Payload
        $result = [pscustomobject]@{
            Success = $true; RequestId = $requestId; AgentBuild = $hello.BuildId
            ActionsPerformed = $inputResult.ActionsPerformed
            CursorX = $inputResult.CursorX; CursorY = $inputResult.CursorY
            Endpoint = "${EndpointHost}:$Port"
        }
        if ($Json) { $result | ConvertTo-Json -Depth 4 -Compress } else { $result | Format-List }
        exit 0
    }

    if ($Action -eq 'download') {
        if (($hello.Capabilities -band [uint32]0x00001000) -eq 0) {
            $exitCategory = 23
            throw "Agent build '$($hello.BuildId)' does not advertise HTTP download support."
        }
        $payload = New-V9xDownloadPayload -Url $Url -Destination $Destination
        $bytes = New-V9xFrameBytes -Type 0x0027 -RequestId $requestId -Payload $payload
        $stream.ReadTimeout = 600000
        $stream.Write($bytes, 0, $bytes.Length)
        $completeFrame = $null
        [uint32]$lastProgress = 0
        while ($null -eq $completeFrame) {
            $frame = Read-V9xOperationFrame -Stream $stream -RequestId $requestId `
                -ExpectedTypes 0x9027,0x8027 -GuestErrorExitCode 40
            if ($frame.Type -eq 0x9027) {
                if ($frame.Payload.Length -ge 4) {
                    $lastProgress = [BitConverter]::ToUInt32($frame.Payload, 0)
                }
            } else {
                $completeFrame = $frame
            }
        }
        $download = ConvertFrom-V9xDownloadComplete -Payload $completeFrame.Payload
        $result = [pscustomobject]@{
            Success = $true; RequestId = $requestId; AgentBuild = $hello.BuildId
            Url = $Url; Destination = $Destination; HttpStatus = $download.HttpStatus
            Bytes = $download.Size; Crc32 = ('{0:X8}' -f $download.Crc32)
            Endpoint = "${EndpointHost}:$Port"
        }
        if ($Json) { $result | ConvertTo-Json -Depth 4 -Compress } else { $result | Format-List }
        exit 0
    }

    if ($legacyUpdate) {
        # Agents before 0.6.0 cannot hot-update. Drive the staged route they do
        # support, with every guest path fixed and 8.3: COMMAND.COM runs
        # INSTALL.BAT now and UPDATE.BAT from RunServices at the next boot, and
        # a long folder name there stops the boot at a prompt. Each step is this
        # script's own verb, run in-process on a fresh connection.
        $packageRoot = (Get-Item -LiteralPath $Source).FullName
        foreach ($name in 'V9XAGNT.EXE', 'V9XSHOT.EXE', 'INSTALL.BAT', 'UPDATE.BAT', 'README.TXT') {
            if (-not (Test-Path -LiteralPath (Join-Path $packageRoot $name) -PathType Leaf)) {
                $exitCategory = 11
                throw "Install package must contain $name for a staged update: $packageRoot"
            }
        }
        $packageVersion = ''
        $readmeTitle = Get-Content -LiteralPath (Join-Path $packageRoot 'README.TXT') -TotalCount 1
        if ($readmeTitle -match '([0-9]+\.[0-9]+\.[0-9]+)\s*$') { $packageVersion = $Matches[1] }
        $oldBuild = $hello.BuildId
        $oldBootCounter = $hello.BootCounter
        $client.Dispose()
        $client = $null
        $legacyPackage = 'C:\V9XPKG'
        function Invoke-V9xLegacyStep([string]$Step, [int]$FailExit, [hashtable]$StepArgs) {
            $all = $target.Clone()
            foreach ($key in $StepArgs.Keys) { $all[$key] = $StepArgs[$key] }
            $output = & $PSCommandPath @all
            $code = $LASTEXITCODE
            $parsed = $null
            if ($output) { try { $parsed = ($output -join "`n") | ConvertFrom-Json } catch { } }
            if ($code -ne 0) {
                $exception = [InvalidOperationException]::new("Staged update: $Step failed (exit $code).")
                $exception.Data['V9xExitCode'] = $(if ($code -eq 20) { 20 } else { $FailExit })
                throw $exception
            }
            $parsed
        }
        [void](Invoke-V9xLegacyStep 'copying the package to C:\V9XPKG' 41 `
            @{ Action = 'push-tree'; Source = $packageRoot; Destination = $legacyPackage })
        $install = Invoke-V9xLegacyStep 'running INSTALL.BAT' 41 `
            @{ Action = 'shell'; ShellCommand = 'INSTALL.BAT'
               WorkingDirectory = $legacyPackage; TimeoutSeconds = 120 }
        if ($install.Stdout -notmatch 'update staged') {
            $exception = [InvalidOperationException]::new(
                "Staged update: INSTALL.BAT did not stage the update. Its output: $($install.Stdout.Trim())")
            $exception.Data['V9xExitCode'] = 41
            throw $exception
        }
        $staged = Invoke-V9xLegacyStep 'checking V9XNEW.EXE' 41 `
            @{ Action = 'stat'; RemotePath = 'C:\V9XREMOTE\V9XNEW.EXE' }
        if (-not $staged.Exists) {
            $exception = [InvalidOperationException]::new('Staged update: V9XNEW.EXE is missing after INSTALL.BAT.')
            $exception.Data['V9xExitCode'] = 41
            throw $exception
        }
        $rebootJob = 'v9x-update-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
        [void](Invoke-V9xLegacyStep 'rebooting to apply UPDATE.BAT' 42 `
            @{ Action = 'reboot'; JobId = $rebootJob; WaitSeconds = [Math]::Max($WaitSeconds, 300) })
        $after = Invoke-V9xLegacyStep 'reading the new agent version' 42 @{ Action = 'info' }
        $leftover = Invoke-V9xLegacyStep 'checking UPDATE.BAT cleaned up' 42 `
            @{ Action = 'stat'; RemotePath = 'C:\V9XREMOTE\V9XNEW.EXE' }
        if ($leftover.Exists -or ($packageVersion -and $after.AgentVersion -ne $packageVersion)) {
            $exception = [InvalidOperationException]::new(
                "Staged update did not apply: agent is $($after.AgentVersion) $($after.BuildId), package is $packageVersion. UPDATE.BAT logs failures to C:\V9XREMOTE\UPDATE.LOG.")
            $exception.Data['V9xExitCode'] = 42
            throw $exception
        }
        # Best effort: the package copy is only scratch.
        [void](& $PSCommandPath -Action shell -ShellCommand "DELTREE /Y $legacyPackage" @target)
        $result = [pscustomobject]@{
            Success = $true; RequestId = $requestId; Mode = 'Staged'
            PreviousBuild = $oldBuild; PreviousBootCounter = $oldBootCounter
            AgentVersion = $after.AgentVersion; AgentBuild = $after.BuildId
            BootCounter = $after.BootCounter; Reconnected = $true
            Endpoint = "${EndpointHost}:$Port"
        }
        if ($Json) { $result | ConvertTo-Json -Depth 4 -Compress } else { $result | Format-List }
        exit 0
    }

    if ($Action -eq 'update') {
        $packageRoot = (Get-Item -LiteralPath $Source).FullName
        $agentSource = Join-Path $packageRoot 'V9XAGNT.EXE'
        $helperSource = Join-Path $packageRoot 'V9XSHOT.EXE'
        if (-not (Test-Path -LiteralPath $agentSource -PathType Leaf) -or
            -not (Test-Path -LiteralPath $helperSource -PathType Leaf)) {
            $exitCategory = 11
            throw "Install package must contain V9XAGNT.EXE and V9XSHOT.EXE: $packageRoot"
        }
        $agentPut = Invoke-V9xPutInternal -Stream $stream -RequestId ([ref]$requestId) `
            -LocalPath $agentSource -GuestPath 'C:\V9XREMOTE\V9XNEW.EXE'
        $helperPut = Invoke-V9xPutInternal -Stream $stream -RequestId ([ref]$requestId) `
            -LocalPath $helperSource -GuestPath 'C:\V9XREMOTE\V9XSNEW.EXE'
        $payload = New-V9xUpdatePayload -AgentSize $agentPut.Size -AgentCrc32 $agentPut.Crc32 `
            -HelperSize $helperPut.Size -HelperCrc32 $helperPut.Crc32
        $bytes = New-V9xFrameBytes -Type 0x0042 -RequestId $requestId -Payload $payload
        $stream.Write($bytes, 0, $bytes.Length)
        $frame = Read-V9xOperationFrame -Stream $stream -RequestId $requestId `
            -ExpectedTypes 0x8042 -GuestErrorExitCode 41
        $accepted = ConvertFrom-V9xUpdateAccepted -Payload $frame.Payload
        $oldBootCounter = $hello.BootCounter
        $oldBuild = $hello.BuildId
        $client.Dispose()
        $client = $null
        $deadline = [DateTime]::UtcNow.AddSeconds($WaitSeconds)
        $resumed = $null
        $lastReconnectError = $null
        while ([DateTime]::UtcNow -lt $deadline -and $null -eq $resumed) {
            Start-Sleep -Milliseconds 500
            try {
                $candidate = Connect-V9xSession -HostName $EndpointHost -TcpPort $Port `
                    -TimeoutSeconds ([Math]::Min($ConnectTimeoutSeconds, 3))
                if ($candidate.Hello.BootCounter -ne $oldBootCounter) {
                    $resumed = $candidate
                } else {
                    $candidate.Client.Dispose()
                }
            } catch {
                $lastReconnectError = $_.Exception.Message
                # -Id: the guest may come back on a new address (DHCP); look it up again.
                if ($Id) { try { [void](Update-V9xTargetFromId -Seconds 2) } catch { } }
            }
        }
        if ($null -eq $resumed) {
            $exception = [InvalidOperationException]::new(
                "Agent did not relaunch with a new boot counter within $WaitSeconds seconds. Last error: $lastReconnectError")
            $exception.Data['V9xExitCode'] = 42
            throw $exception
        }
        $client = $resumed.Client
        $result = [pscustomobject]@{
            Success = $true; RequestId = $requestId
            PreviousBuild = $oldBuild; PreviousBootCounter = $oldBootCounter
            AgentBuild = $resumed.Hello.BuildId; BootCounter = $resumed.Hello.BootCounter
            AgentBytes = $accepted.AgentSize; Reconnected = $true
            Endpoint = "${EndpointHost}:$Port"
        }
        if ($Json) { $result | ConvertTo-Json -Depth 4 -Compress } else { $result | Format-List }
        exit 0
    }

    if ($Action -in @('stat', 'list', 'mkdir', 'put', 'get', 'push-tree')) {
        $requiredCapability = if ($Action -in @('stat', 'list', 'get')) {
            [uint32]0x00000020
        } else {
            [uint32]0x00000040
        }
        if (($hello.Capabilities -band $requiredCapability) -eq 0) {
            $exitCategory = 23
            throw "Agent build '$($hello.BuildId)' does not advertise $Action support."
        }

        if ($Action -eq 'stat') {
            $payload = New-V9xPathPayload -Path $RemotePath
            $bytes = New-V9xFrameBytes -Type 0x0020 -RequestId $requestId -Payload $payload
            $stream.Write($bytes, 0, $bytes.Length)
            $frame = Read-V9xOperationFrame -Stream $stream -RequestId $requestId `
                -ExpectedTypes 0x8020 -GuestErrorExitCode 40
            $stat = ConvertFrom-V9xStatPayload -Payload $frame.Payload
            $result = [pscustomobject]@{
                Success = $true; RequestId = $requestId; AgentBuild = $hello.BuildId
                Path = $RemotePath; Exists = $stat.Exists; IsDirectory = $stat.IsDirectory
                Size = $stat.Size; Attributes = $stat.Attributes; NativeError = $stat.NativeError
            }
        } elseif ($Action -eq 'list') {
            $payload = New-V9xPathPayload -Path $RemotePath
            $bytes = New-V9xFrameBytes -Type 0x0021 -RequestId $requestId -Payload $payload
            $stream.Write($bytes, 0, $bytes.Length)
            $frame = Read-V9xOperationFrame -Stream $stream -RequestId $requestId `
                -ExpectedTypes 0x8021 -GuestErrorExitCode 40
            $entries = @(ConvertFrom-V9xListPayload -Payload $frame.Payload)
            $result = [pscustomobject]@{
                Success = $true; RequestId = $requestId; AgentBuild = $hello.BuildId
                Path = $RemotePath; Count = $entries.Count; Entries = $entries
            }
        } elseif ($Action -eq 'mkdir') {
            $created = Invoke-V9xMkdirInternal -Stream $stream -RequestId ([ref]$requestId) -Path $RemotePath
            $result = [pscustomobject]@{
                Success = $true; RequestId = [uint32]($requestId - 1)
                AgentBuild = $hello.BuildId; Path = $RemotePath; Created = $created
            }
        } elseif ($Action -eq 'put') {
            $transfer = Invoke-V9xPutInternal -Stream $stream -RequestId ([ref]$requestId) `
                -LocalPath $Source -GuestPath $Destination
            $result = [pscustomobject]@{
                Success = $true; RequestId = [uint32]($requestId - 1)
                AgentBuild = $hello.BuildId; Source = $transfer.Source
                Destination = $transfer.Destination; Bytes = $transfer.Size
                Crc32 = ('{0:X8}' -f $transfer.Crc32)
            }
        } elseif ($Action -eq 'get') {
            $transfer = Invoke-V9xGetInternal -Stream $stream -RequestId ([ref]$requestId) `
                -GuestPath $Source -LocalPath $Destination
            $result = [pscustomobject]@{
                Success = $true; RequestId = [uint32]($requestId - 1); AgentBuild = $hello.BuildId
                Source = $transfer.Source; Destination = $transfer.Destination
                Bytes = $transfer.Size; Crc32 = ('{0:X8}' -f $transfer.Crc32)
            }
        } else {
            $sourceRoot = (Get-Item -LiteralPath $Source).FullName.TrimEnd('\')
            $guestRoot = $Destination.Replace('/', '\')
            if ($guestRoot -notmatch '^[A-Za-z]:\\$') { $guestRoot = $guestRoot.TrimEnd('\') }
            Initialize-V9xGuestDirectory -Stream $stream -RequestId ([ref]$requestId) -Path $guestRoot
            $directories = @(Get-ChildItem -LiteralPath $sourceRoot -Directory -Recurse | Sort-Object FullName)
            foreach ($directory in $directories) {
                $relative = $directory.FullName.Substring($sourceRoot.Length).TrimStart('\')
                $guestDirectory = Join-V9xGuestPath -Parent $guestRoot -Child $relative
                [void](Invoke-V9xMkdirInternal -Stream $stream -RequestId ([ref]$requestId) -Path $guestDirectory)
            }
            $files = @(Get-ChildItem -LiteralPath $sourceRoot -File -Recurse | Sort-Object FullName)
            [uint64]$totalBytes = 0
            foreach ($file in $files) {
                $relative = $file.FullName.Substring($sourceRoot.Length).TrimStart('\')
                $guestFile = Join-V9xGuestPath -Parent $guestRoot -Child $relative
                $transfer = Invoke-V9xPutInternal -Stream $stream -RequestId ([ref]$requestId) `
                    -LocalPath $file.FullName -GuestPath $guestFile
                $totalBytes += $transfer.Size
            }
            $result = [pscustomobject]@{
                Success = $true; RequestId = [uint32]($requestId - 1)
                AgentBuild = $hello.BuildId; Source = $sourceRoot
                Destination = $guestRoot; Directories = $directories.Count + 1
                Files = $files.Count; Bytes = $totalBytes
            }
        }
        if ($Json) { $result | ConvertTo-Json -Depth 6 -Compress } else { $result | Format-List }
        exit 0
    }

    $requiredCapability = if ($Action -eq 'exec') { [uint32]0x00000004 } else { [uint32]0x00000008 }
    if (($hello.Capabilities -band $requiredCapability) -eq 0) {
        $exitCategory = 23
        throw "Agent build '$($hello.BuildId)' does not advertise $Action support."
    }
    if ($Detach -and ($hello.Capabilities -band [uint32]0x00000800) -eq 0) {
        $exitCategory = 23
        throw "Agent build '$($hello.BuildId)' does not advertise detached execution."
    }
    $mode = if ($Action -eq 'exec') { 'Direct' } else { 'Shell' }
    $execApplication = if ($Action -eq 'exec') { $Application } else { '' }
    $execCommand = if ($Action -eq 'exec') { $Arguments } else { $ShellCommand }
    $execPayload = New-V9xExecPayload -Mode $mode -Application $execApplication `
        -CommandLine $execCommand -WorkingDirectory $WorkingDirectory `
        -TimeoutMilliseconds ([uint32]($TimeoutSeconds * 1000)) `
        -StdoutLimit ([uint32]$StdoutLimit) -StderrLimit ([uint32]$StderrLimit) `
        -ShowWindow:$ShowWindow -Detach:$Detach
    $requestBytes = New-V9xFrameBytes -Type 0x0010 -RequestId $requestId -Payload $execPayload
    $stream.ReadTimeout = ($TimeoutSeconds + 15) * 1000
    $stream.Write($requestBytes, 0, $requestBytes.Length)

    $accepted = Read-V9xFrame -Stream $stream
    if ($accepted.RequestId -ne $requestId) { throw 'EXEC_ACCEPTED request ID does not match.' }
    if ($accepted.Type -eq 0x8fff) {
        $exitCategory = 23
        $guestError = ConvertFrom-V9xErrorPayload -Payload $accepted.Payload
        throw "Guest execution error $($guestError.Status) (Win32 $($guestError.NativeError)): $($guestError.Detail)"
    }
    if ($accepted.Type -ne 0x8010 -or $accepted.Payload.Length -ne 4) {
        throw "Expected EXEC_ACCEPTED, received 0x$('{0:x4}' -f $accepted.Type)."
    }

    $stdout = [IO.MemoryStream]::new()
    $stderr = [IO.MemoryStream]::new()
    $complete = $null
    while ($null -eq $complete) {
        $frame = Read-V9xFrame -Stream $stream
        if ($frame.RequestId -ne $requestId) { throw 'Execution stream request ID does not match.' }
        switch ($frame.Type) {
            0x9010 { $stdout.Write($frame.Payload, 0, $frame.Payload.Length) }
            0x9011 { $stderr.Write($frame.Payload, 0, $frame.Payload.Length) }
            0x8011 { $complete = ConvertFrom-V9xExecComplete -Payload $frame.Payload }
            0x8fff {
                $exitCategory = 23
                $guestError = ConvertFrom-V9xErrorPayload -Payload $frame.Payload
                throw "Guest execution error $($guestError.Status) (Win32 $($guestError.NativeError)): $($guestError.Detail)"
            }
            default { throw "Unexpected execution frame type 0x$('{0:x4}' -f $frame.Type)." }
        }
    }

    $stdoutText = [Text.Encoding]::ASCII.GetString($stdout.ToArray())
    $stderrText = [Text.Encoding]::ASCII.GetString($stderr.ToArray())
    $outcomeExit = 0
    if ($complete.ResultCategory -eq 1) { $outcomeExit = 30 }
    elseif ($complete.ResultCategory -eq 2) { $outcomeExit = 32 }
    elseif ($complete.ResultCategory -eq 3) { $outcomeExit = 33 }
    elseif ($complete.ResultCategory -ne 0) { $outcomeExit = 30 }
    elseif ($ExpectedExitCode -notcontains $complete.ExitCode) { $outcomeExit = 31 }
    $result = [pscustomobject]@{
        Success = ($outcomeExit -eq 0)
        RequestId = $requestId
        AgentBuild = $hello.BuildId
        BootCounter = $hello.BootCounter
        Mode = $mode
        ExitCode = $complete.ExitCode
        ResultCategory = $complete.ResultCategory
        NativeError = $complete.NativeError
        ElapsedMilliseconds = $complete.ElapsedMilliseconds
        Stdout = $stdoutText
        Stderr = $stderrText
        StdoutBytes = $complete.StdoutBytes
        StderrBytes = $complete.StderrBytes
        StdoutTruncated = $complete.StdoutTruncated
        StderrTruncated = $complete.StderrTruncated
        TimedOut = $complete.TimedOut
        Cancelled = $complete.Cancelled
        CaptureMode = if ($complete.PipeCapture) { 'pipes' } else { 'none' }
        GuiWindow = $complete.GuiWindow
        Detached = $complete.Detached
        Orphaned = $complete.Orphaned
    }
    if ($Json) { $result | ConvertTo-Json -Depth 4 -Compress } else { $result | Format-List }
    exit $outcomeExit
} catch [Net.Sockets.SocketException] {
    [Console]::Error.WriteLine("Transport error: $($_.Exception.Message)")
    [Console]::Error.WriteLine("Target was ${EndpointHost}:$Port. Is the guest up and the agent's port forwarded? Choose another target with -Host <address> -Port <n>.")
    exit 20
} catch [IO.IOException] {
    [Console]::Error.WriteLine("Transport error: $($_.Exception.Message)")
    [Console]::Error.WriteLine("Target was ${EndpointHost}:$Port. Is the guest up and the agent's port forwarded? Choose another target with -Host <address> -Port <n>.")
    exit 20
} catch [TimeoutException] {
    [Console]::Error.WriteLine("Transport error: $($_.Exception.Message)")
    [Console]::Error.WriteLine("Target was ${EndpointHost}:$Port. Is the guest up and the agent's port forwarded? Choose another target with -Host <address> -Port <n>.")
    exit 20
} catch {
    [Console]::Error.WriteLine("Protocol error: $($_.Exception.Message)")
    if ($_.Exception.Data.Contains('V9xExitCode')) {
        exit [int]$_.Exception.Data['V9xExitCode']
    }
    exit $exitCategory
} finally {
    if ($null -ne $client) { $client.Dispose() }
    if ($downloadTemp -and (Test-Path -LiteralPath $downloadTemp -PathType Leaf)) {
        Remove-Item -LiteralPath $downloadTemp -Force
    }
}
