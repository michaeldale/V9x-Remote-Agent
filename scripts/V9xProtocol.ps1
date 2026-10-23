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
    # Only present when the request carried V9X_INFO_FLAG_EXTENDED (agent 0.8.0+).
    $installId = ''
    if ($offset -lt $Payload.Length) {
        $installId = Read-V9xString -Bytes $Payload -Offset ([ref]$offset)
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
        InstallId = $installId
    }
}

# INVENTORY_RESPONSE: u16 record count, then per record a type string, a u16
# field count and that many (key, value) strings. A key repeated inside one
# record (several net "ip" fields) is returned as an array.
function ConvertFrom-V9xInventoryPayload {
    param([byte[]]$Payload)
    if ($Payload.Length -lt 2) { throw 'Truncated INVENTORY response.' }
    $count = [BitConverter]::ToUInt16($Payload, 0)
    $offset = 2
    $records = New-Object System.Collections.Generic.List[object]
    for ($recordIndex = 0; $recordIndex -lt $count; ++$recordIndex) {
        $recordType = Read-V9xString -Bytes $Payload -Offset ([ref]$offset)
        if ($offset + 2 -gt $Payload.Length) { throw 'Truncated INVENTORY record.' }
        $fieldCount = [BitConverter]::ToUInt16($Payload, $offset)
        $offset += 2
        $fields = [ordered]@{}
        for ($fieldIndex = 0; $fieldIndex -lt $fieldCount; ++$fieldIndex) {
            $key = Read-V9xString -Bytes $Payload -Offset ([ref]$offset)
            $value = Read-V9xString -Bytes $Payload -Offset ([ref]$offset)
            if ($fields.Contains($key)) {
                $fields[$key] = @($fields[$key]) + $value
            } else {
                $fields[$key] = $value
            }
        }
        $records.Add([pscustomobject]@{ Record = $recordType; Fields = [pscustomobject]$fields })
    }
    if ($offset -ne $Payload.Length) { throw 'INVENTORY response contains trailing data.' }
    return ,$records.ToArray()
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
        [switch]$ShowWindow,
        [switch]$Detach
    )
    $bytes = [Collections.Generic.List[byte]]::new()
    [byte]$modeByte = if ($Mode -eq 'Shell') { 1 } else { 0 }
    $bytes.Add($modeByte)
    $bytes.Add([byte][bool]$ShowWindow)
    [uint16]$optionBits = if ($Detach) { 1 } else { 0 }
    $bytes.AddRange([BitConverter]::GetBytes($optionBits))
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
        Detached = [bool]($flags -band 0x00000040)
        Orphaned = [bool]($flags -band 0x00000080)
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

function New-V9xDownloadPayload {
    param(
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$Destination
    )
    $bytes = [Collections.Generic.List[byte]]::new()
    Add-V9xString -Bytes $bytes -Value $Url
    Add-V9xString -Bytes $bytes -Value $Destination
    return $bytes.ToArray()
}

function ConvertFrom-V9xDownloadComplete {
    param([byte[]]$Payload)
    if ($Payload.Length -ne 12) { throw 'Invalid DOWNLOAD_COMPLETE payload length.' }
    [pscustomobject]@{
        HttpStatus = [BitConverter]::ToUInt32($Payload, 0)
        Size = [BitConverter]::ToUInt32($Payload, 4)
        Crc32 = [BitConverter]::ToUInt32($Payload, 8)
    }
}

function New-V9xUpdatePayload {
    param(
        [uint32]$AgentSize,
        [uint32]$AgentCrc32,
        [uint32]$HelperSize,
        [uint32]$HelperCrc32
    )
    $bytes = [Collections.Generic.List[byte]]::new()
    $bytes.AddRange([BitConverter]::GetBytes($AgentSize))
    $bytes.AddRange([BitConverter]::GetBytes($AgentCrc32))
    $bytes.AddRange([BitConverter]::GetBytes($HelperSize))
    $bytes.AddRange([BitConverter]::GetBytes($HelperCrc32))
    return $bytes.ToArray()
}

function ConvertFrom-V9xUpdateAccepted {
    param([byte[]]$Payload)
    if ($Payload.Length -ne 8) { throw 'Invalid UPDATE_ACCEPTED payload length.' }
    [pscustomobject]@{
        BootCounter = [BitConverter]::ToUInt32($Payload, 0)
        AgentSize = [BitConverter]::ToUInt32($Payload, 4)
    }
}

function Add-V9xInputAction {
    param([Collections.Generic.List[byte]]$Bytes, [hashtable]$Action)
    switch ($Action.Op) {
        'move' {
            $Bytes.Add([byte]1)
            $Bytes.AddRange([BitConverter]::GetBytes([int]$Action.X))
            $Bytes.AddRange([BitConverter]::GetBytes([int]$Action.Y))
        }
        'moverel' {
            $Bytes.Add([byte]7)
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

# ---- File tools, processes, registry (agent 0.13.0+) -------------------
# FILE_DELETE 0x0028 (u32 flags: 1 recursive, 2 force; path), FILE_HASH
# 0x0029 (path), FILE_READ_TEXT 0x002A (u32 tail, u32 max bytes, match, path)
# need V9X_CAP_FILE_TOOLS 0x00040000; PROCESS_LIST 0x0080 / PROCESS_KILL
# 0x0081 need 0x00080000; REG_QUERY 0x0090 needs 0x00100000.

# Names and text from the guest are in the Windows ANSI code page, not ASCII.
$script:V9xAnsi = [Text.Encoding]::GetEncoding(1252)

function Read-V9xAnsiString {
    param([byte[]]$Bytes, [ref]$Offset)
    if ($Offset.Value + 2 -gt $Bytes.Length) { throw 'Truncated protocol string length.' }
    $length = [BitConverter]::ToUInt16($Bytes, $Offset.Value)
    $Offset.Value += 2
    if ($Offset.Value + $length -gt $Bytes.Length) { throw 'Truncated protocol string.' }
    $value = $script:V9xAnsi.GetString($Bytes, $Offset.Value, $length)
    $Offset.Value += $length
    return $value
}

function New-V9xDeletePayload {
    param([Parameter(Mandatory = $true)][string]$Path, [switch]$Recurse, [switch]$Force)
    $flags = [uint32]0
    if ($Recurse) { $flags = $flags -bor 1 }
    if ($Force) { $flags = $flags -bor 2 }
    $bytes = [Collections.Generic.List[byte]]::new()
    $bytes.AddRange([BitConverter]::GetBytes($flags))
    Add-V9xString -Bytes $bytes -Value $Path
    return $bytes.ToArray()
}

function ConvertFrom-V9xDeletePayload {
    param([byte[]]$Payload)
    if ($Payload.Length -ne 8) { throw 'Invalid FILE_DELETE_RESPONSE payload.' }
    [pscustomobject]@{
        FilesDeleted = [BitConverter]::ToUInt32($Payload, 0)
        DirectoriesDeleted = [BitConverter]::ToUInt32($Payload, 4)
    }
}

function ConvertFrom-V9xHashPayload {
    param([byte[]]$Payload)
    if ($Payload.Length -ne 40) { throw 'Invalid FILE_HASH_RESPONSE payload.' }
    [pscustomobject]@{
        Size = [BitConverter]::ToUInt32($Payload, 0)
        Crc32 = ('{0:X8}' -f [BitConverter]::ToUInt32($Payload, 4))
        Sha256 = (($Payload[8..39] | ForEach-Object { $_.ToString('X2') }) -join '')
    }
}

function New-V9xReadTextPayload {
    param([Parameter(Mandatory = $true)][string]$Path, [uint32]$Tail = 0,
          [uint32]$MaxBytes = 0, [string]$Match = '')
    $bytes = [Collections.Generic.List[byte]]::new()
    $bytes.AddRange([BitConverter]::GetBytes($Tail))
    $bytes.AddRange([BitConverter]::GetBytes($MaxBytes))
    Add-V9xString -Bytes $bytes -Value $Match
    Add-V9xString -Bytes $bytes -Value $Path
    return $bytes.ToArray()
}

function ConvertFrom-V9xReadTextPayload {
    param([byte[]]$Payload)
    if ($Payload.Length -lt 24) { throw 'Truncated FILE_READ_TEXT_RESPONSE payload.' }
    $length = [BitConverter]::ToUInt32($Payload, 20)
    if (24 + $length -ne $Payload.Length) { throw 'FILE_READ_TEXT_RESPONSE length mismatch.' }
    $flags = [BitConverter]::ToUInt32($Payload, 16)
    [pscustomobject]@{
        FileSize = [BitConverter]::ToUInt32($Payload, 0)
        Lines = [BitConverter]::ToUInt32($Payload, 4)
        Matched = [BitConverter]::ToUInt32($Payload, 8)
        Returned = [BitConverter]::ToUInt32($Payload, 12)
        Truncated = [bool]($flags -band 1)
        LongLinesCut = [bool]($flags -band 2)
        Text = $script:V9xAnsi.GetString($Payload, 24, $length)
    }
}

function ConvertFrom-V9xProcessListPayload {
    param([byte[]]$Payload)
    if ($Payload.Length -lt 4) { throw 'Truncated PROCESS_LIST_RESPONSE payload.' }
    $count = [BitConverter]::ToUInt32($Payload, 0)
    $offset = 4
    $list = [Collections.Generic.List[object]]::new()
    for ($index = 0; $index -lt $count; ++$index) {
        if ($offset + 20 -gt $Payload.Length) { throw 'Truncated process entry.' }
        $processId = [BitConverter]::ToUInt32($Payload, $offset)
        $parent = [BitConverter]::ToUInt32($Payload, $offset + 4)
        $threads = [BitConverter]::ToUInt32($Payload, $offset + 8)
        $flags = [BitConverter]::ToUInt32($Payload, $offset + 12)
        $job = [BitConverter]::ToUInt32($Payload, $offset + 16)
        $offset += 20
        $path = Read-V9xAnsiString -Bytes $Payload -Offset ([ref]$offset)
        $list.Add([pscustomobject]@{
            Pid = $processId; ParentPid = $parent; Threads = $threads
            Name = Split-Path -Leaf $path; Path = $path
            Agent = [bool]($flags -band 2); AgentJob = [bool]($flags -band 1)
            JobRequestId = $job
        })
    }
    if ($offset -ne $Payload.Length) { throw 'PROCESS_LIST_RESPONSE contains trailing data.' }
    return $list.ToArray()
}

# WINDOW_LIST 0x0062 (empty) -> 0x8062: u32 count, u32 truncated, then per
# window u32 hwnd, u32 pid, u32 flags (1 foreground, 2 minimized), i32 left,
# top, right, bottom, class string, title string. V9X_CAP_WINDOWS 0x00200000.
function ConvertFrom-V9xWindowListPayload {
    param([byte[]]$Payload)
    if ($Payload.Length -lt 8) { throw 'Truncated WINDOW_LIST_RESPONSE payload.' }
    $count = [BitConverter]::ToUInt32($Payload, 0)
    $offset = 8
    $list = [Collections.Generic.List[object]]::new()
    for ($index = 0; $index -lt $count; ++$index) {
        if ($offset + 28 -gt $Payload.Length) { throw 'Truncated window entry.' }
        $flags = [BitConverter]::ToUInt32($Payload, $offset + 8)
        $left = [BitConverter]::ToInt32($Payload, $offset + 12)
        $top = [BitConverter]::ToInt32($Payload, $offset + 16)
        $right = [BitConverter]::ToInt32($Payload, $offset + 20)
        $bottom = [BitConverter]::ToInt32($Payload, $offset + 24)
        $window = [ordered]@{
            Handle = [BitConverter]::ToUInt32($Payload, $offset)
            Pid = [BitConverter]::ToUInt32($Payload, $offset + 4)
            Foreground = [bool]($flags -band 1); Minimized = [bool]($flags -band 2)
            X = $left; Y = $top; Width = $right - $left; Height = $bottom - $top
        }
        $offset += 28
        $window.Class = Read-V9xAnsiString -Bytes $Payload -Offset ([ref]$offset)
        $window.Title = Read-V9xAnsiString -Bytes $Payload -Offset ([ref]$offset)
        $list.Add([pscustomobject]$window)
    }
    if ($offset -ne $Payload.Length) { throw 'WINDOW_LIST_RESPONSE contains trailing data.' }
    return $list.ToArray()
}

$script:V9xRegHives = [ordered]@{
    'HKCR' = 0x80000000L; 'HKEY_CLASSES_ROOT' = 0x80000000L
    'HKCU' = 0x80000001L; 'HKEY_CURRENT_USER' = 0x80000001L
    'HKLM' = 0x80000002L; 'HKEY_LOCAL_MACHINE' = 0x80000002L
    'HKU' = 0x80000003L; 'HKEY_USERS' = 0x80000003L
    'HKCC' = 0x80000005L; 'HKEY_CURRENT_CONFIG' = 0x80000005L
    'HKDD' = 0x80000006L; 'HKEY_DYN_DATA' = 0x80000006L
}

function New-V9xRegQueryPayload {
    param([Parameter(Mandatory = $true)][string]$Key)
    $parts = $Key.Replace('/', '\').TrimEnd('\').Split([char[]]'\', 2)
    $hiveName = $parts[0].TrimEnd(':').ToUpperInvariant()
    if (-not $script:V9xRegHives.Contains($hiveName)) {
        throw "Unknown registry hive '$($parts[0])' (HKLM, HKCU, HKCR, HKU, HKCC, HKDD)."
    }
    $bytes = [Collections.Generic.List[byte]]::new()
    $bytes.AddRange([BitConverter]::GetBytes([uint32]$script:V9xRegHives[$hiveName]))
    Add-V9xString -Bytes $bytes -Value $(if ($parts.Count -gt 1) { $parts[1] } else { '' })
    return $bytes.ToArray()
}

function ConvertFrom-V9xRegQueryPayload {
    param([byte[]]$Payload)
    if ($Payload.Length -lt 12) { throw 'Truncated REG_QUERY_RESPONSE payload.' }
    $subkeyCount = [BitConverter]::ToUInt32($Payload, 0)
    $valueCount = [BitConverter]::ToUInt32($Payload, 4)
    $flags = [BitConverter]::ToUInt32($Payload, 8)
    $offset = 12
    $subkeys = @(for ($index = 0; $index -lt $subkeyCount; ++$index) {
        Read-V9xAnsiString -Bytes $Payload -Offset ([ref]$offset)
    })
    $typeNames = @{ 0 = 'REG_NONE'; 1 = 'REG_SZ'; 2 = 'REG_EXPAND_SZ'; 3 = 'REG_BINARY'
                    4 = 'REG_DWORD'; 5 = 'REG_DWORD_BIG_ENDIAN'; 7 = 'REG_MULTI_SZ' }
    $values = @(for ($index = 0; $index -lt $valueCount; ++$index) {
        $name = Read-V9xAnsiString -Bytes $Payload -Offset ([ref]$offset)
        if ($offset + 8 -gt $Payload.Length) { throw 'Truncated registry value.' }
        $type = [BitConverter]::ToUInt32($Payload, $offset)
        $length = [BitConverter]::ToUInt32($Payload, $offset + 4)
        $offset += 8
        if ($offset + $length -gt $Payload.Length) { throw 'Truncated registry data.' }
        $raw = [byte[]]@(if ($length -gt 0) { $Payload[$offset..($offset + $length - 1)] })
        $offset += $length
        $data = switch ($type) {
            { $_ -in 1, 2 } { $script:V9xAnsi.GetString($raw).TrimEnd([char]0) }
            4 { if ($raw.Length -ge 4) { [BitConverter]::ToUInt32($raw, 0) } else { $null } }
            7 { @($script:V9xAnsi.GetString($raw).Split([char]0) | Where-Object { $_ -ne '' }) }
            default { ($raw | ForEach-Object { $_.ToString('X2') }) -join ' ' }
        }
        [pscustomobject]@{
            Name = $(if ($name -eq '') { '(Default)' } else { $name })
            Type = $(if ($typeNames.ContainsKey([int]$type)) { $typeNames[[int]$type] } else { "type $type" })
            Data = $data
        }
    })
    if ($offset -ne $Payload.Length) { throw 'REG_QUERY_RESPONSE contains trailing data.' }
    [pscustomobject]@{ Subkeys = $subkeys; Values = $values; Truncated = [bool]($flags -band 1) }
}

# SCREENSHOT_REQUEST: the guest path, then (V9X_CAP_SCREENSHOT_OPTIONS
# 0x00020000, agent 0.13.0+) six u32s: flags (0x1 compact = lossless 8-bit
# palette/RLE8 when <= 256 colours, 0x2 gray), region x, y, width, height
# (width or height 0 = whole screen) and scale percent (1-100, 0 = 100).
# Without -Options the payload is the bare path every agent accepts.
function New-V9xScreenshotPayload {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [uint32[]]$Options
    )
    $bytes = [Collections.Generic.List[byte]]::new()
    Add-V9xString -Bytes $bytes -Value $Path
    if ($null -ne $Options) {
        if ($Options.Count -ne 6) { throw 'Screenshot options are six u32 values.' }
        foreach ($value in $Options) { $bytes.AddRange([BitConverter]::GetBytes([uint32]$value)) }
    }
    return $bytes.ToArray()
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

$script:V9xCrcNative = $null

# A PowerShell loop manages about 2.5 MiB/s, which made a 300 MiB put spend
# two minutes hashing. Large buffers go through a compiled loop instead,
# compiled on first use because Add-Type starts csc (about a second);
# small transfers never pay for it. Falls back to the script loop if
# compilation is unavailable.
function Initialize-V9xCrcNative {
    if ($null -ne $script:V9xCrcNative) { return $script:V9xCrcNative }
    $script:V9xCrcNative = $false
    try {
        if (-not ('V9xCrc32Native' -as [type])) {
            Add-Type -Language CSharp -TypeDefinition @'
public static class V9xCrc32Native
{
    private static readonly uint[] Table = Build();
    private static uint[] Build()
    {
        uint[] table = new uint[256];
        for (uint i = 0; i < 256; ++i)
        {
            uint entry = i;
            for (int bit = 0; bit < 8; ++bit)
                entry = (entry & 1) != 0 ? (entry >> 1) ^ 0xEDB88320u : entry >> 1;
            table[i] = entry;
        }
        return table;
    }
    public static uint Update(uint crc, byte[] bytes, int offset, int count)
    {
        for (int i = 0; i < count; ++i)
            crc = (crc >> 8) ^ Table[(crc ^ bytes[offset + i]) & 0xFF];
        return crc;
    }
}
'@
        }
        $script:V9xCrcNative = $true
    } catch { }
    return $script:V9xCrcNative
}

function Update-V9xCrc32 {
    param([uint32]$Crc, [byte[]]$Bytes, [int]$Offset = 0, [int]$Count = $Bytes.Length)
    if ($Count -ge 4096 -and (Initialize-V9xCrcNative)) {
        return [V9xCrc32Native]::Update($Crc, $Bytes, $Offset, $Count)
    }
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

# ---- mDNS / DNS-SD discovery (RFC 6762, RFC 6763): codec only. ----
# DNS is big-endian, unlike the rest of the protocol. Mirrors src\common\mdns.c
# and the Python codec in mcp\v9x_mcp.py.
$script:V9xMdnsService = '_v9xremote._tcp.local'

function Add-V9xDnsName {
    param([Collections.Generic.List[byte]]$Bytes, [string]$Name)
    foreach ($label in ($Name.TrimEnd('.') -split '\.')) {
        $encoded = [Text.Encoding]::ASCII.GetBytes($label)
        if ($encoded.Length -lt 1 -or $encoded.Length -gt 63) { throw "Invalid DNS label '$label'." }
        $Bytes.Add([byte]$encoded.Length)
        $Bytes.AddRange($encoded)
    }
    $Bytes.Add([byte]0)
}

function New-V9xMdnsQuery {
    param([uint16]$Id = 0, [string]$Name = $script:V9xMdnsService, [uint16]$Type = 12)
    $bytes = [Collections.Generic.List[byte]]::new()
    $bytes.Add([byte]($Id -shr 8))
    $bytes.Add([byte]($Id -band 0xff))
    $bytes.AddRange([byte[]](0, 0, 0, 1, 0, 0, 0, 0, 0, 0))
    Add-V9xDnsName -Bytes $bytes -Name $Name
    $bytes.AddRange([byte[]](($Type -shr 8), ($Type -band 0xff), 0, 1))
    return $bytes.ToArray()
}

function Read-V9xDnsName {
    param([byte[]]$Bytes, [ref]$Offset)
    $labels = New-Object System.Collections.Generic.List[string]
    $at = $Offset.Value
    $jumped = $false
    $jumps = 0
    while ($true) {
        if ($at -ge $Bytes.Length) { throw 'Truncated DNS name.' }
        $length = [int]$Bytes[$at]
        if ($length -eq 0) {
            if (-not $jumped) { $Offset.Value = $at + 1 }
            break
        }
        if (($length -band 0xc0) -eq 0xc0) {
            $jumps++
            if ($at + 1 -ge $Bytes.Length -or $jumps -gt 16) { throw 'Invalid DNS compression pointer.' }
            if (-not $jumped) { $Offset.Value = $at + 2 }
            $jumped = $true
            $at = (($length -band 0x3f) -shl 8) -bor [int]$Bytes[$at + 1]
            continue
        }
        if (($length -band 0xc0) -ne 0 -or $at + 1 + $length -gt $Bytes.Length) { throw 'Invalid DNS label.' }
        $labels.Add([Text.Encoding]::ASCII.GetString($Bytes, $at + 1, $length))
        $at += 1 + $length
    }
    return ($labels -join '.')
}

function Read-V9xBigEndian {
    param([byte[]]$Bytes, [int]$Offset, [int]$Count)
    [uint32]$value = 0
    for ($index = 0; $index -lt $Count; ++$index) {
        $value = [uint32](($value * 256) + $Bytes[$Offset + $index])
    }
    return $value
}

# One DNS message -> header fields plus every answer/authority/additional
# record. Data is decoded for A (dotted text), PTR (name), SRV (object) and
# TXT (string array); other types keep their raw bytes.
function ConvertFrom-V9xMdnsPacket {
    param([byte[]]$Packet)
    if ($Packet.Length -lt 12) { throw 'Truncated DNS header.' }
    $questions = Read-V9xBigEndian -Bytes $Packet -Offset 4 -Count 2
    $recordCount = (Read-V9xBigEndian -Bytes $Packet -Offset 6 -Count 2) +
        (Read-V9xBigEndian -Bytes $Packet -Offset 8 -Count 2) +
        (Read-V9xBigEndian -Bytes $Packet -Offset 10 -Count 2)
    $offset = 12
    for ($index = 0; $index -lt $questions; ++$index) {
        [void](Read-V9xDnsName -Bytes $Packet -Offset ([ref]$offset))
        $offset += 4
    }
    $records = New-Object System.Collections.Generic.List[object]
    for ($index = 0; $index -lt $recordCount; ++$index) {
        $name = Read-V9xDnsName -Bytes $Packet -Offset ([ref]$offset)
        if ($offset + 10 -gt $Packet.Length) { throw 'Truncated DNS record.' }
        $type = [int](Read-V9xBigEndian -Bytes $Packet -Offset $offset -Count 2)
        $class = [int](Read-V9xBigEndian -Bytes $Packet -Offset ($offset + 2) -Count 2)
        $ttl = Read-V9xBigEndian -Bytes $Packet -Offset ($offset + 4) -Count 4
        $dataLength = [int](Read-V9xBigEndian -Bytes $Packet -Offset ($offset + 8) -Count 2)
        $offset += 10
        if ($offset + $dataLength -gt $Packet.Length) { throw 'Truncated DNS record data.' }
        $dataOffset = $offset
        $data = $null
        switch ($type) {
            1 {
                if ($dataLength -ne 4) { throw 'Invalid DNS A record.' }
                $data = '{0}.{1}.{2}.{3}' -f $Packet[$offset], $Packet[$offset + 1], $Packet[$offset + 2], $Packet[$offset + 3]
            }
            12 { $data = Read-V9xDnsName -Bytes $Packet -Offset ([ref]$dataOffset) }
            33 {
                if ($dataLength -lt 7) { throw 'Invalid DNS SRV record.' }
                $targetOffset = $offset + 6
                $data = [pscustomobject]@{
                    Priority = [int](Read-V9xBigEndian -Bytes $Packet -Offset $offset -Count 2)
                    Weight = [int](Read-V9xBigEndian -Bytes $Packet -Offset ($offset + 2) -Count 2)
                    Port = [int](Read-V9xBigEndian -Bytes $Packet -Offset ($offset + 4) -Count 2)
                    Target = Read-V9xDnsName -Bytes $Packet -Offset ([ref]$targetOffset)
                }
            }
            16 {
                $strings = New-Object System.Collections.Generic.List[string]
                $at = $offset
                while ($at -lt $offset + $dataLength) {
                    $length = [int]$Packet[$at]
                    if ($at + 1 + $length -gt $offset + $dataLength) { throw 'Invalid DNS TXT record.' }
                    if ($length -gt 0) { $strings.Add([Text.Encoding]::ASCII.GetString($Packet, $at + 1, $length)) }
                    $at += 1 + $length
                }
                $data = $strings.ToArray()
            }
            default {
                $raw = [byte[]]::new($dataLength)
                [Array]::Copy($Packet, $offset, $raw, 0, $dataLength)
                $data = $raw
            }
        }
        $offset += $dataLength
        $records.Add([pscustomobject]@{
            Name = $name; Type = $type; Class = ($class -band 0x7fff)
            CacheFlush = (($class -band 0x8000) -ne 0); Ttl = $ttl; Data = $data
        })
    }
    [pscustomobject]@{
        Id = [int](Read-V9xBigEndian -Bytes $Packet -Offset 0 -Count 2)
        IsResponse = (($Packet[2] -band 0x80) -ne 0)
        Records = $records.ToArray()
    }
}

# Folds the records of every reply (in arrival order) into one object per
# advertised agent. A PTR with TTL 0 is a goodbye and removes the agent.
# Replies are @{ Source = '<ip>'; Packet = <ConvertFrom-V9xMdnsPacket result> }.
function ConvertTo-V9xDiscoveredAgents {
    param([object[]]$Replies)
    $instances = [ordered]@{}
    $sources = @{}
    $srv = @{}
    $txt = @{}
    $addresses = @{}
    foreach ($reply in $Replies) {
        foreach ($record in $reply.Packet.Records) {
            $key = $record.Name.ToLowerInvariant()
            if ($record.Type -eq 12 -and $key -eq $script:V9xMdnsService) {
                $instance = [string]$record.Data
                $instanceKey = $instance.ToLowerInvariant()
                if ($record.Ttl -eq 0) {
                    $instances.Remove($instanceKey)
                } else {
                    $instances[$instanceKey] = $instance
                    $sources[$instanceKey] = $reply.Source
                }
            } elseif ($record.Type -eq 33) {
                $srv[$key] = $record.Data
            } elseif ($record.Type -eq 16) {
                $txt[$key] = $record.Data
            } elseif ($record.Type -eq 1 -and $record.Ttl -ne 0) {
                $addresses[$key] = $record.Data
            }
        }
    }
    $agents = New-Object System.Collections.Generic.List[object]
    foreach ($instanceKey in $instances.Keys) {
        $instance = $instances[$instanceKey]
        $fields = @{}
        if ($txt.ContainsKey($instanceKey)) {
            foreach ($entry in $txt[$instanceKey]) {
                $split = $entry.IndexOf('=')
                if ($split -gt 0) { $fields[$entry.Substring(0, $split)] = $entry.Substring($split + 1) }
            }
        }
        $service = if ($srv.ContainsKey($instanceKey)) { $srv[$instanceKey] } else { $null }
        $hostName = if ($null -ne $service) { $service.Target } else { '' }
        $address = $sources[$instanceKey]
        if ($hostName -and $addresses.ContainsKey($hostName.ToLowerInvariant())) {
            $address = $addresses[$hostName.ToLowerInvariant()]
        }
        $suffix = '.' + $script:V9xMdnsService
        $label = if ($instance.ToLowerInvariant().EndsWith($suffix)) {
            $instance.Substring(0, $instance.Length - $suffix.Length)
        } else { $instance }
        $agents.Add([pscustomobject]@{
            Instance = $label
            InstallId = if ($fields.ContainsKey('id')) { $fields['id'] } else { '' }
            Address = $address
            Port = if ($null -ne $service) { $service.Port } else { 9869 }
            HostName = $hostName
            ComputerName = if ($fields.ContainsKey('name')) { $fields['name'] } else { '' }
            AgentVersion = if ($fields.ContainsKey('ver')) { $fields['ver'] } else { '' }
            BuildId = if ($fields.ContainsKey('build')) { $fields['build'] } else { '' }
            WindowsVersion = if ($fields.ContainsKey('os')) { $fields['os'] } else { '' }
            AllowDosVm = if ($fields.ContainsKey('dos')) { $fields['dos'] -eq '1' } else { $null }
            RespondedFrom = $sources[$instanceKey]
        })
    }
    return $agents.ToArray()
}

# Remote help (docs\remote-help.md): a Stream that tunnels the protocol to an
# opted-in machine through the update server. Server, operator key, signer
# and signing key default as in mcp\v9x_relay.py.
function New-V9xRelayStream {
    param([string]$Machine, [string]$Server, [int]$OpenWaitSeconds = 150)
    if (-not ('V9xRelayStream' -as [type])) {
        Add-Type -Path (Join-Path $PSScriptRoot 'V9xRelayStream.cs') `
            -ReferencedAssemblies 'System.Web.Extensions'
    }
    $home9x = Join-Path $env:USERPROFILE '.v9x'
    if (-not $Server) { $Server = $env:V9X_RELAY_SERVER }
    if (-not $Server) { $Server = 'https://michaeldale.com.au' }
    $operatorKey = $env:V9X_OPERATOR_KEY
    if (-not $operatorKey -and (Test-Path -LiteralPath (Join-Path $home9x 'operator.key'))) {
        $operatorKey = (Get-Content -LiteralPath (Join-Path $home9x 'operator.key') -Raw).Trim()
    }
    if (-not $operatorKey) {
        throw "No operator key: put the key from the plugin's Machines page in $home9x\operator.key or V9X_OPERATOR_KEY."
    }
    $signer = $env:V9XSIGN
    if (-not $signer) { $signer = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\tools\v9xsign.exe' }
    if (-not (Test-Path -LiteralPath $signer)) {
        throw "v9xsign.exe not found at $signer; run scripts\build-host-tests.ps1 or set V9XSIGN."
    }
    $signingKey = Join-Path $home9x 'relay-signing.key'
    if (-not (Test-Path -LiteralPath $signingKey)) {
        throw "No relay signing key at $signingKey (v9xsign keygen)."
    }
    return [V9xRelayStream]::new($Server, $operatorKey, $signer, $signingKey, $Machine, $OpenWaitSeconds)
}
