Set-StrictMode -Version Latest

$script:V9xMagic = [byte[]](0x56, 0x39, 0x58, 0x52)
$script:V9xVersion = [uint16]0x0100
$script:V9xHeaderSize = 24
$script:V9xMaxPayload = 65536
$script:V9xCrcTable = $null

function Read-V9xExact {
    param([IO.Stream]$Stream, [int]$Length)
    $buffer = [byte[]]::new($Length)
    $offset = 0
    while ($offset -lt $Length) {
        $count = $Stream.Read($buffer, $offset, $Length - $offset)
        if ($count -eq 0) { throw [IO.EndOfStreamException]::new('Peer closed the connection.') }
        $offset += $count
    }
    return $buffer
}

function New-V9xFrameBytes {
    param([uint16]$Type, [uint32]$RequestId, [byte[]]$Payload = @(), [uint32]$Flags = 0)
    if ($RequestId -eq 0) { throw 'RequestId must be nonzero.' }
    if ($Payload.Length -gt $script:V9xMaxPayload) { throw 'Payload exceeds 65,536 bytes.' }
    $bytes = [byte[]]::new($script:V9xHeaderSize + $Payload.Length)
    [Array]::Copy($script:V9xMagic, 0, $bytes, 0, 4)
    [Array]::Copy([BitConverter]::GetBytes($script:V9xVersion), 0, $bytes, 4, 2)
    [Array]::Copy([BitConverter]::GetBytes($Type), 0, $bytes, 6, 2)
    [Array]::Copy([BitConverter]::GetBytes($RequestId), 0, $bytes, 8, 4)
    [Array]::Copy([BitConverter]::GetBytes($Flags), 0, $bytes, 12, 4)
    [Array]::Copy([BitConverter]::GetBytes([uint32]$Payload.Length), 0, $bytes, 16, 4)
    if ($Payload.Length) { [Array]::Copy($Payload, 0, $bytes, 24, $Payload.Length) }
    return $bytes
}

function Read-V9xFrame {
    param([IO.Stream]$Stream)
    $header = Read-V9xExact -Stream $Stream -Length $script:V9xHeaderSize
    if ($header[0] -ne 0x56 -or $header[1] -ne 0x39 -or $header[2] -ne 0x58 -or $header[3] -ne 0x52) {
        throw 'Invalid V9XR frame magic.'
    }
    $length = [BitConverter]::ToUInt32($header, 16)
    if ($length -gt $script:V9xMaxPayload) { throw 'Peer payload exceeds protocol limit.' }
    if ([BitConverter]::ToUInt32($header, 20) -ne 0) { throw 'Reserved header field is nonzero.' }
    [pscustomobject]@{
        Version = [BitConverter]::ToUInt16($header, 4)
        Type = [BitConverter]::ToUInt16($header, 6)
        RequestId = [BitConverter]::ToUInt32($header, 8)
        Flags = [BitConverter]::ToUInt32($header, 12)
        Payload = if ($length) { Read-V9xExact -Stream $Stream -Length $length } else { [byte[]]@() }
    }
}

function Add-V9xString {
    param([Collections.Generic.List[byte]]$Bytes, [string]$Value)
    foreach ($character in $Value.ToCharArray()) {
        if ([int]$character -eq 0 -or [int]$character -gt 127) {
            throw 'Protocol v1 strings must contain non-NUL 7-bit ASCII only.'
        }
    }
    $encoded = [Text.Encoding]::ASCII.GetBytes($Value)
    if ($encoded.Length -gt 65535) { throw 'String exceeds protocol limit.' }
    $Bytes.AddRange([BitConverter]::GetBytes([uint16]$encoded.Length))
    $Bytes.AddRange($encoded)
}

function Read-V9xString {
    param([byte[]]$Bytes, [ref]$Offset)
    if ($Offset.Value + 2 -gt $Bytes.Length) { throw 'Truncated protocol string length.' }
    $length = [BitConverter]::ToUInt16($Bytes, $Offset.Value)
    $Offset.Value += 2
    if ($Offset.Value + $length -gt $Bytes.Length) { throw 'Truncated protocol string.' }
    $value = [Text.Encoding]::ASCII.GetString($Bytes, $Offset.Value, $length)
    $Offset.Value += $length
    return $value
}

function New-V9xHelloPayload {
    param([string]$ClientLabel = 'v9xctl')
    $bytes = [Collections.Generic.List[byte]]::new()
    $bytes.AddRange([BitConverter]::GetBytes($script:V9xVersion))
    $bytes.AddRange([BitConverter]::GetBytes($script:V9xVersion))
    Add-V9xString -Bytes $bytes -Value $ClientLabel
    return $bytes.ToArray()
}

function ConvertFrom-V9xHelloPayload {
    param([byte[]]$Payload)
    if ($Payload.Length -lt 22) { throw 'Truncated HELLO response.' }
    $offset = 20
    $buildId = Read-V9xString -Bytes $Payload -Offset ([ref]$offset)
    $pendingJob = ''
    $desktopReady = $false
    [uint32]$screenWidth = 0
    [uint32]$screenHeight = 0
    [uint32]$bitsPerPixel = 0
    $listenAddress = ''
    $allowedClient = ''
    if ($offset -lt $Payload.Length) {
        $pendingJob = Read-V9xString -Bytes $Payload -Offset ([ref]$offset)
    }
    if ($offset + 16 -le $Payload.Length) {
        $desktopReady = [bool]$Payload[$offset]
        $screenWidth = [BitConverter]::ToUInt32($Payload, $offset + 4)
        $screenHeight = [BitConverter]::ToUInt32($Payload, $offset + 8)
        $bitsPerPixel = [BitConverter]::ToUInt32($Payload, $offset + 12)
        $offset += 16
    }
    if ($offset -lt $Payload.Length) {
        $listenAddress = Read-V9xString -Bytes $Payload -Offset ([ref]$offset)
    }
    if ($offset -lt $Payload.Length) {
        $allowedClient = Read-V9xString -Bytes $Payload -Offset ([ref]$offset)
    }
    if ($offset -ne $Payload.Length) { throw 'HELLO response contains trailing data.' }
    [pscustomobject]@{
        ProtocolVersion = [BitConverter]::ToUInt16($Payload, 0)
        Capabilities = [BitConverter]::ToUInt32($Payload, 4)
        MaxPayload = [BitConverter]::ToUInt32($Payload, 8)
        BootCounter = [BitConverter]::ToUInt32($Payload, 12)
        Port = [BitConverter]::ToUInt16($Payload, 16)
        WinsockVersion = [BitConverter]::ToUInt16($Payload, 18)
        BuildId = $buildId
        PendingJob = $pendingJob
        DesktopReady = $desktopReady
        ScreenWidth = $screenWidth
        ScreenHeight = $screenHeight
        BitsPerPixel = $bitsPerPixel
        ListenAddress = $listenAddress
        AllowedClient = $allowedClient
    }
}

function ConvertFrom-V9xInfoPayload {
    param([byte[]]$Payload)
    if ($Payload.Length -lt 20) { throw 'Truncated INFO response.' }
    $offset = 20
    $names = 'AgentVersion','BuildId','ComputerName','WindowsVersion','SystemDirectory','WindowsDirectory','CurrentDirectory'
    $values = @{}
    foreach ($name in $names) { $values[$name] = Read-V9xString -Bytes $Payload -Offset ([ref]$offset) }
    $pendingJob = ''
    $desktopReady = $false
    [uint32]$screenWidth = 0
    [uint32]$screenHeight = 0
    [uint32]$bitsPerPixel = 0
    $listenAddress = ''
    $allowedClient = ''
    if ($offset -lt $Payload.Length) {
        $pendingJob = Read-V9xString -Bytes $Payload -Offset ([ref]$offset)
    }
    if ($offset + 16 -le $Payload.Length) {
        $desktopReady = [bool]$Payload[$offset]
        $screenWidth = [BitConverter]::ToUInt32($Payload, $offset + 4)
        $screenHeight = [BitConverter]::ToUInt32($Payload, $offset + 8)
        $bitsPerPixel = [BitConverter]::ToUInt32($Payload, $offset + 12)
        $offset += 16
    }
    if ($offset -lt $Payload.Length) {
        $listenAddress = Read-V9xString -Bytes $Payload -Offset ([ref]$offset)
    }
    if ($offset -lt $Payload.Length) {
        $allowedClient = Read-V9xString -Bytes $Payload -Offset ([ref]$offset)
    }
    if ($offset -ne $Payload.Length) { throw 'INFO response contains trailing data.' }
    [pscustomobject]@{
        BootCounter = [BitConverter]::ToUInt32($Payload, 0)
        UptimeMilliseconds = [BitConverter]::ToUInt32($Payload, 4)
        Capabilities = [BitConverter]::ToUInt32($Payload, 8)
        Port = [BitConverter]::ToUInt16($Payload, 12)
        WinsockVersion = [BitConverter]::ToUInt16($Payload, 14)
        RawWindowsVersion = [BitConverter]::ToUInt32($Payload, 16)
        AgentVersion = $values.AgentVersion
        BuildId = $values.BuildId
        ComputerName = $values.ComputerName
        WindowsVersion = $values.WindowsVersion
        SystemDirectory = $values.SystemDirectory
        WindowsDirectory = $values.WindowsDirectory
        CurrentDirectory = $values.CurrentDirectory
        PendingJob = $pendingJob
        DesktopReady = $desktopReady
        ScreenWidth = $screenWidth
        ScreenHeight = $screenHeight
        BitsPerPixel = $bitsPerPixel
        ListenAddress = $listenAddress
        AllowedClient = $allowedClient
    }
}

function New-V9xExecPayload {
    param(
        [ValidateSet('Direct', 'Shell')][string]$Mode,
        [string]$Application = '',
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$CommandLine,
        [string]$WorkingDirectory = '',
        [uint32]$TimeoutMilliseconds = 60000,
        [ValidateRange(0, 1048576)][uint32]$StdoutLimit = 262144,
        [ValidateRange(0, 1048576)][uint32]$StderrLimit = 262144,
        [switch]$ShowWindow
    )
    $bytes = [Collections.Generic.List[byte]]::new()
    [byte]$modeByte = if ($Mode -eq 'Shell') { 1 } else { 0 }
    $bytes.Add($modeByte)
    $bytes.Add([byte][bool]$ShowWindow)
    $bytes.AddRange([BitConverter]::GetBytes([uint16]0))
    $bytes.AddRange([BitConverter]::GetBytes($TimeoutMilliseconds))
    $bytes.AddRange([BitConverter]::GetBytes($StdoutLimit))
    $bytes.AddRange([BitConverter]::GetBytes($StderrLimit))
    Add-V9xString -Bytes $bytes -Value $Application
    Add-V9xString -Bytes $bytes -Value $CommandLine
    Add-V9xString -Bytes $bytes -Value $WorkingDirectory
    return $bytes.ToArray()
}

function ConvertFrom-V9xExecComplete {
    param([byte[]]$Payload)
    if ($Payload.Length -ne 28) { throw 'Invalid EXEC_COMPLETE payload length.' }
    $flags = [BitConverter]::ToUInt32($Payload, 24)
    [pscustomobject]@{
        ResultCategory = [BitConverter]::ToUInt32($Payload, 0)
        ExitCode = [BitConverter]::ToUInt32($Payload, 4)
        NativeError = [BitConverter]::ToUInt32($Payload, 8)
        ElapsedMilliseconds = [BitConverter]::ToUInt32($Payload, 12)
        StdoutBytes = [BitConverter]::ToUInt32($Payload, 16)
        StderrBytes = [BitConverter]::ToUInt32($Payload, 20)
        Flags = $flags
        StdoutTruncated = [bool]($flags -band 0x00000001)
        StderrTruncated = [bool]($flags -band 0x00000002)
        TimedOut = [bool]($flags -band 0x00000004)
        Cancelled = [bool]($flags -band 0x00000008)
        PipeCapture = [bool]($flags -band 0x00000010)
        GuiWindow = [bool]($flags -band 0x00000020)
    }
}

function ConvertFrom-V9xErrorPayload {
    param([byte[]]$Payload)
    if ($Payload.Length -lt 10) { throw 'Truncated ERROR_RESPONSE payload.' }
    $offset = 8
    $detail = Read-V9xString -Bytes $Payload -Offset ([ref]$offset)
    if ($offset -ne $Payload.Length) { throw 'ERROR_RESPONSE contains trailing data.' }
    [pscustomobject]@{
        Status = [BitConverter]::ToUInt32($Payload, 0)
        NativeError = [BitConverter]::ToUInt32($Payload, 4)
        Detail = $detail
    }
}

function New-V9xPathPayload {
    param([Parameter(Mandatory = $true)][string]$Path)
    $bytes = [Collections.Generic.List[byte]]::new()
    Add-V9xString -Bytes $bytes -Value $Path
    return $bytes.ToArray()
}

function New-V9xOpenWritePayload {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [uint32]$Size,
        [uint32]$Crc32
    )
    $bytes = [Collections.Generic.List[byte]]::new()
    $bytes.AddRange([BitConverter]::GetBytes($Size))
    $bytes.AddRange([BitConverter]::GetBytes($Crc32))
    Add-V9xString -Bytes $bytes -Value $Path
    return $bytes.ToArray()
}

function Add-V9xInputAction {
    param([Collections.Generic.List[byte]]$Bytes, [hashtable]$Action)
    switch ($Action.Op) {
        'move' {
            $Bytes.Add([byte]1)
            $Bytes.AddRange([BitConverter]::GetBytes([int]$Action.X))
            $Bytes.AddRange([BitConverter]::GetBytes([int]$Action.Y))
        }
        'button' {
            $Bytes.Add([byte]2)
            $Bytes.Add([byte]$Action.Button)
            $Bytes.Add([byte]([int][bool]$Action.Down))
        }
        'wheel' {
            $Bytes.Add([byte]3)
            $Bytes.AddRange([BitConverter]::GetBytes([int16]$Action.Notches))
        }
        'key' {
            $Bytes.Add([byte]4)
            $Bytes.Add([byte]$Action.Vk)
            $Bytes.Add([byte]([int][bool]$Action.Down))
        }
        'type' {
            $Bytes.Add([byte]5)
            foreach ($character in ([string]$Action.Text).ToCharArray()) {
                if ([int]$character -gt 127) { throw 'Type text must be 7-bit ASCII.' }
            }
            $encoded = [Text.Encoding]::ASCII.GetBytes([string]$Action.Text)
            if ($encoded.Length -gt 65535) { throw 'Type text exceeds protocol limit.' }
            $Bytes.AddRange([BitConverter]::GetBytes([uint16]$encoded.Length))
            $Bytes.AddRange($encoded)
        }
        'delay' {
            $Bytes.Add([byte]6)
            $Bytes.AddRange([BitConverter]::GetBytes([uint16]$Action.Ms))
        }
        default { throw "Unknown input action '$($Action.Op)'." }
    }
}

function New-V9xInputPayload {
    param([hashtable[]]$Actions)
    if ($Actions.Count -gt 256) { throw 'At most 256 input actions per request.' }
    $bytes = [Collections.Generic.List[byte]]::new()
    $bytes.AddRange([BitConverter]::GetBytes([uint16]$Actions.Count))
    foreach ($action in $Actions) { Add-V9xInputAction -Bytes $bytes -Action $action }
    return $bytes.ToArray()
}

function ConvertFrom-V9xInputPayload {
    param([byte[]]$Payload)
    if ($Payload.Length -ne 12) { throw 'Invalid INPUT_RESPONSE payload length.' }
    [pscustomobject]@{
        ActionsPerformed = [BitConverter]::ToUInt32($Payload, 0)
        CursorX = [BitConverter]::ToInt32($Payload, 4)
        CursorY = [BitConverter]::ToInt32($Payload, 8)
    }
}

function ConvertFrom-V9xStatPayload {
    param([byte[]]$Payload)
    if ($Payload.Length -ne 16) { throw 'Invalid FILE_STAT_RESPONSE payload.' }
    [pscustomobject]@{
        Exists = [bool]$Payload[0]
        IsDirectory = [bool]$Payload[1]
        Size = [BitConverter]::ToUInt32($Payload, 4)
        Attributes = [BitConverter]::ToUInt32($Payload, 8)
        NativeError = [BitConverter]::ToUInt32($Payload, 12)
    }
}

function ConvertFrom-V9xListPayload {
    param([byte[]]$Payload)
    if ($Payload.Length -lt 4) { throw 'Truncated FILE_LIST_RESPONSE payload.' }
    $count = [BitConverter]::ToUInt32($Payload, 0)
    $offset = 4
    $entries = [Collections.Generic.List[object]]::new()
    for ([uint32]$index = 0; $index -lt $count; ++$index) {
        if ($offset + 8 -gt $Payload.Length) { throw 'Truncated directory entry.' }
        $attributes = [BitConverter]::ToUInt32($Payload, $offset)
        $size = [BitConverter]::ToUInt32($Payload, $offset + 4)
        $offset += 8
        $name = Read-V9xString -Bytes $Payload -Offset ([ref]$offset)
        $entries.Add([pscustomobject]@{
            Name = $name
            Size = $size
            Attributes = $attributes
            IsDirectory = [bool]($attributes -band 0x10)
        })
    }
    if ($offset -ne $Payload.Length) { throw 'Directory listing contains trailing data.' }
    return $entries.ToArray()
}

function ConvertFrom-V9xScreenshotPayload {
    param([byte[]]$Payload)
    if ($Payload.Length -lt 22) { throw 'Truncated SCREENSHOT_RESPONSE payload.' }
    $offset = 20
    $path = Read-V9xString -Bytes $Payload -Offset ([ref]$offset)
    if ($offset -ne $Payload.Length) { throw 'SCREENSHOT_RESPONSE contains trailing data.' }
    [pscustomobject]@{
        Width = [BitConverter]::ToUInt32($Payload, 0)
        Height = [BitConverter]::ToUInt32($Payload, 4)
        SourceBitsPerPixel = [BitConverter]::ToUInt32($Payload, 8)
        FileBytes = [BitConverter]::ToUInt32($Payload, 12)
        Crc32 = [BitConverter]::ToUInt32($Payload, 16)
        GuestPath = $path
    }
}

function Update-V9xCrc32 {
    param([uint32]$Crc, [byte[]]$Bytes, [int]$Offset = 0, [int]$Count = $Bytes.Length)
    if ($null -eq $script:V9xCrcTable) {
        $script:V9xCrcTable = [uint32[]]::new(256)
        for ($tableIndex = 0; $tableIndex -lt 256; ++$tableIndex) {
            [uint32]$entry = $tableIndex
            for ($bit = 0; $bit -lt 8; ++$bit) {
                if ($entry -band 1) {
                    $entry = [uint32](($entry -shr 1) -bxor [uint32]3988292384)
                } else {
                    $entry = [uint32]($entry -shr 1)
                }
            }
            $script:V9xCrcTable[$tableIndex] = $entry
        }
    }
    [uint32]$value = $Crc
    for ($index = 0; $index -lt $Count; ++$index) {
        $tableIndex = [int](($value -bxor $Bytes[$Offset + $index]) -band 0xff)
        $value = [uint32](($value -shr 8) -bxor $script:V9xCrcTable[$tableIndex])
    }
    return $value
}

function Get-V9xFileCrc32 {
    param([Parameter(Mandatory = $true)][string]$LiteralPath)
    $stream = [IO.File]::OpenRead($LiteralPath)
    try {
        $buffer = [byte[]]::new(65536)
        [uint32]$crc = [uint32]::MaxValue
        while (($count = $stream.Read($buffer, 0, $buffer.Length)) -ne 0) {
            $crc = Update-V9xCrc32 -Crc $crc -Bytes $buffer -Count $count
        }
        return [uint32]($crc -bxor [uint32]::MaxValue)
    } finally {
        $stream.Dispose()
    }
}
