[CmdletBinding()]
param(
    [string]$PackageDir,
    [string]$OutputPath,
    [ValidatePattern('^[A-Z0-9_]{1,32}$')]
    [string]$VolumeId = 'V9XREMOTE',
    [switch]$Validate
)

# Writes a plain ISO9660 Level 2 data disc from a single flat directory of
# uppercase-named files, with no external tools. Windows 9x CDFS reads Level 2
# names (up to 30 characters) natively, so no Joliet, Rock Ridge, or El Torito
# boot records are needed.

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($PackageDir)) { $PackageDir = Join-Path $repoRoot 'build\install' }
if ([string]::IsNullOrWhiteSpace($OutputPath)) { $OutputPath = Join-Path $repoRoot 'build\V9XREMOTE.ISO' }
$PackageDir = [IO.Path]::GetFullPath($PackageDir)
$OutputPath = [IO.Path]::GetFullPath($OutputPath)

$SECTOR = 2048

function New-BothEndianU16 {
    param([uint16]$Value)
    $bytes = New-Object byte[] 4
    $bytes[0] = [byte]($Value -band 0xff)
    $bytes[1] = [byte](($Value -shr 8) -band 0xff)
    $bytes[2] = $bytes[1]
    $bytes[3] = $bytes[0]
    return ,$bytes
}

function New-BothEndianU32 {
    param([uint32]$Value)
    $bytes = New-Object byte[] 8
    $bytes[0] = [byte]($Value -band 0xff)
    $bytes[1] = [byte](($Value -shr 8) -band 0xff)
    $bytes[2] = [byte](($Value -shr 16) -band 0xff)
    $bytes[3] = [byte](($Value -shr 24) -band 0xff)
    $bytes[4] = $bytes[3]
    $bytes[5] = $bytes[2]
    $bytes[6] = $bytes[1]
    $bytes[7] = $bytes[0]
    return ,$bytes
}

function New-LittleEndianU32 {
    param([uint32]$Value)
    return ,([BitConverter]::GetBytes($Value))
}

function New-BigEndianU32 {
    param([uint32]$Value)
    $bytes = [BitConverter]::GetBytes($Value)
    [Array]::Reverse($bytes)
    return ,$bytes
}

function New-PaddedAscii {
    param([string]$Text, [int]$Length)
    $padded = $Text.PadRight($Length).Substring(0, $Length)
    return ,([Text.Encoding]::ASCII.GetBytes($padded))
}

function New-RecordingDate {
    param([datetime]$Utc)
    $bytes = New-Object byte[] 7
    $bytes[0] = [byte]($Utc.Year - 1900)
    $bytes[1] = [byte]$Utc.Month
    $bytes[2] = [byte]$Utc.Day
    $bytes[3] = [byte]$Utc.Hour
    $bytes[4] = [byte]$Utc.Minute
    $bytes[5] = [byte]$Utc.Second
    $bytes[6] = 0
    return ,$bytes
}

function New-DecimalDate {
    param([datetime]$Utc)
    $text = $Utc.ToString('yyyyMMddHHmmss') + '00'
    $bytes = New-Object byte[] 17
    [Array]::Copy([Text.Encoding]::ASCII.GetBytes($text), $bytes, 16)
    $bytes[16] = 0
    return ,$bytes
}

function Get-DirectoryRecordLength {
    param([int]$IdentifierLength)
    $length = 33 + $IdentifierLength
    if ($length % 2 -ne 0) { $length++ }
    return $length
}

function New-DirectoryRecord {
    param(
        [uint32]$ExtentLba,
        [uint32]$DataLength,
        [byte]$Flags,
        [byte[]]$Identifier,
        [datetime]$Stamp
    )
    $recordLength = Get-DirectoryRecordLength -IdentifierLength $Identifier.Length
    $record = New-Object byte[] $recordLength
    $record[0] = [byte]$recordLength
    $record[1] = 0
    [Array]::Copy((New-BothEndianU32 $ExtentLba), 0, $record, 2, 8)
    [Array]::Copy((New-BothEndianU32 $DataLength), 0, $record, 10, 8)
    [Array]::Copy((New-RecordingDate $Stamp), 0, $record, 18, 7)
    $record[25] = $Flags
    [Array]::Copy((New-BothEndianU16 1), 0, $record, 28, 4)
    $record[32] = [byte]$Identifier.Length
    [Array]::Copy($Identifier, 0, $record, 33, $Identifier.Length)
    return ,$record
}

if (-not (Test-Path -LiteralPath $PackageDir -PathType Container)) {
    throw "Package directory was not found: $PackageDir"
}
$files = @(Get-ChildItem -LiteralPath $PackageDir -File | Sort-Object Name)
if ($files.Count -eq 0) { throw "Package directory is empty: $PackageDir" }
foreach ($file in $files) {
    if ($file.Name -cnotmatch '^[A-Z0-9_]{1,26}(\.[A-Z0-9_]{1,3})?$' -or $file.Name.Length -gt 30) {
        throw "File name '$($file.Name)' is not a valid ISO9660 Level 2 name (uppercase A-Z, 0-9, _; at most 30 characters)."
    }
    if ($file.Length -gt 0x7fffffffL) { throw "File '$($file.Name)' is too large for this writer." }
}

# ISO9660 orders directory records by identifier. For 8.3 names the separator
# and version characters sort below every d-character, so ordinal order on the
# full identifier matches the required padded-field collation.
$entries = @($files | ForEach-Object {
    [pscustomobject]@{
        File = $_
        Identifier = [Text.Encoding]::ASCII.GetBytes($_.Name + ';1')
        Stamp = $_.LastWriteTimeUtc
        Lba = [uint32]0
        Sectors = [int][math]::Ceiling($_.Length / $SECTOR)
    }
} | Sort-Object { [Text.Encoding]::ASCII.GetString($_.Identifier) })

$volumeStamp = ($files | Sort-Object LastWriteTimeUtc | Select-Object -Last 1).LastWriteTimeUtc

# Layout: 0-15 system area, 16 PVD, 17 terminator, 18 L path table,
# 19 M path table, 20+ root directory extent, then file data.
$pathTableLba = @{ L = [uint32]18; M = [uint32]19 }
$rootDirLba = [uint32]20

# First pass: size the root directory extent. A directory record must not
# span a sector boundary; records that would cross are pushed to the next
# sector with zero padding.
$rootDirBytes = 34 + 34
foreach ($entry in $entries) {
    $recordLength = Get-DirectoryRecordLength -IdentifierLength $entry.Identifier.Length
    $sectorOffset = $rootDirBytes % $SECTOR
    if ($sectorOffset + $recordLength -gt $SECTOR) {
        $rootDirBytes += $SECTOR - $sectorOffset
    }
    $rootDirBytes += $recordLength
}
$rootDirSectors = [int][math]::Ceiling($rootDirBytes / $SECTOR)
$rootDirDataLength = [uint32]($rootDirSectors * $SECTOR)

$nextLba = $rootDirLba + [uint32]$rootDirSectors
foreach ($entry in $entries) {
    $entry.Lba = $nextLba
    $nextLba += [uint32]$entry.Sectors
}
$volumeSpaceSize = $nextLba

# Root directory extent bytes.
$rootDir = New-Object byte[] ($rootDirSectors * $SECTOR)
$rootPosition = 0
$dotRecord = New-DirectoryRecord -ExtentLba $rootDirLba -DataLength $rootDirDataLength -Flags 2 -Identifier ([byte[]]@(0)) -Stamp $volumeStamp
[Array]::Copy($dotRecord, 0, $rootDir, $rootPosition, $dotRecord.Length); $rootPosition += $dotRecord.Length
$dotDotRecord = New-DirectoryRecord -ExtentLba $rootDirLba -DataLength $rootDirDataLength -Flags 2 -Identifier ([byte[]]@(1)) -Stamp $volumeStamp
[Array]::Copy($dotDotRecord, 0, $rootDir, $rootPosition, $dotDotRecord.Length); $rootPosition += $dotDotRecord.Length
foreach ($entry in $entries) {
    $record = New-DirectoryRecord -ExtentLba $entry.Lba -DataLength ([uint32]$entry.File.Length) -Flags 0 -Identifier $entry.Identifier -Stamp $entry.Stamp
    $sectorOffset = $rootPosition % $SECTOR
    if ($sectorOffset + $record.Length -gt $SECTOR) {
        $rootPosition += $SECTOR - $sectorOffset
    }
    [Array]::Copy($record, 0, $rootDir, $rootPosition, $record.Length)
    $rootPosition += $record.Length
}

# Path tables: a single root entry.
function New-PathTable {
    param([ValidateSet('L', 'M')][string]$Format)
    $table = New-Object byte[] 10
    $table[0] = 1
    $table[1] = 0
    if ($Format -eq 'L') { $extent = New-LittleEndianU32 $rootDirLba } else { $extent = New-BigEndianU32 $rootDirLba }
    [Array]::Copy($extent, 0, $table, 2, 4)
    if ($Format -eq 'L') { $table[6] = 1; $table[7] = 0 } else { $table[6] = 0; $table[7] = 1 }
    $table[8] = 0
    $table[9] = 0
    return ,$table
}
$pathTableSize = [uint32]10

# Primary Volume Descriptor.
$pvd = New-Object byte[] $SECTOR
$pvd[0] = 1
[Array]::Copy([Text.Encoding]::ASCII.GetBytes('CD001'), 0, $pvd, 1, 5)
$pvd[6] = 1
[Array]::Copy((New-PaddedAscii 'V9X REMOTE AGENT' 32), 0, $pvd, 8, 32)
[Array]::Copy((New-PaddedAscii $VolumeId 32), 0, $pvd, 40, 32)
[Array]::Copy((New-BothEndianU32 $volumeSpaceSize), 0, $pvd, 80, 8)
[Array]::Copy((New-BothEndianU16 1), 0, $pvd, 120, 4)
[Array]::Copy((New-BothEndianU16 1), 0, $pvd, 124, 4)
[Array]::Copy((New-BothEndianU16 ([uint16]$SECTOR)), 0, $pvd, 128, 4)
[Array]::Copy((New-BothEndianU32 $pathTableSize), 0, $pvd, 132, 8)
[Array]::Copy((New-LittleEndianU32 $pathTableLba.L), 0, $pvd, 140, 4)
[Array]::Copy((New-BigEndianU32 $pathTableLba.M), 0, $pvd, 148, 4)
$pvdRootRecord = New-DirectoryRecord -ExtentLba $rootDirLba -DataLength $rootDirDataLength -Flags 2 -Identifier ([byte[]]@(0)) -Stamp $volumeStamp
[Array]::Copy($pvdRootRecord, 0, $pvd, 156, 34)
[Array]::Copy((New-PaddedAscii '' 128), 0, $pvd, 190, 128)
[Array]::Copy((New-PaddedAscii 'V9X REMOTE AGENT' 128), 0, $pvd, 318, 128)
[Array]::Copy((New-PaddedAscii 'MAKE-INSTALL-MEDIA.PS1' 128), 0, $pvd, 446, 128)
[Array]::Copy((New-PaddedAscii 'V9X REMOTE AGENT' 128), 0, $pvd, 574, 128)
[Array]::Copy((New-PaddedAscii '' 37), 0, $pvd, 702, 37)
[Array]::Copy((New-PaddedAscii '' 37), 0, $pvd, 739, 37)
[Array]::Copy((New-PaddedAscii '' 37), 0, $pvd, 776, 37)
[Array]::Copy((New-DecimalDate $volumeStamp), 0, $pvd, 813, 17)
[Array]::Copy((New-DecimalDate $volumeStamp), 0, $pvd, 830, 17)
[Array]::Copy([Text.Encoding]::ASCII.GetBytes('0000000000000000'), 0, $pvd, 847, 16)
[Array]::Copy([Text.Encoding]::ASCII.GetBytes('0000000000000000'), 0, $pvd, 864, 16)
$pvd[881] = 1

# Volume Descriptor Set Terminator.
$terminator = New-Object byte[] $SECTOR
$terminator[0] = 255
[Array]::Copy([Text.Encoding]::ASCII.GetBytes('CD001'), 0, $terminator, 1, 5)
$terminator[6] = 1

$outputDir = Split-Path -Parent $OutputPath
if (-not (Test-Path -LiteralPath $outputDir)) {
    New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
}
$stream = [IO.File]::Create($OutputPath)
try {
    $zeroSector = New-Object byte[] $SECTOR
    for ($index = 0; $index -lt 16; $index++) { $stream.Write($zeroSector, 0, $SECTOR) }
    $stream.Write($pvd, 0, $SECTOR)
    $stream.Write($terminator, 0, $SECTOR)
    $lTable = New-PathTable -Format 'L'
    $stream.Write($lTable, 0, $lTable.Length)
    $stream.Write($zeroSector, 0, $SECTOR - $lTable.Length)
    $mTable = New-PathTable -Format 'M'
    $stream.Write($mTable, 0, $mTable.Length)
    $stream.Write($zeroSector, 0, $SECTOR - $mTable.Length)
    $stream.Write($rootDir, 0, $rootDir.Length)
    foreach ($entry in $entries) {
        $data = [IO.File]::ReadAllBytes($entry.File.FullName)
        $stream.Write($data, 0, $data.Length)
        $padding = ($entry.Sectors * $SECTOR) - $data.Length
        if ($padding -gt 0) { $stream.Write($zeroSector, 0, $padding) }
    }
} finally {
    $stream.Dispose()
}

$isoInfo = Get-Item -LiteralPath $OutputPath
$result = [ordered]@{
    Iso = $OutputPath
    VolumeId = $VolumeId
    Files = $entries.Count
    Sectors = [int]$volumeSpaceSize
    Bytes = $isoInfo.Length
    Validated = $false
}

if ($Validate) {
    $mounted = Mount-DiskImage -ImagePath $OutputPath -PassThru
    try {
        $volume = $mounted | Get-Volume
        if ($null -eq $volume -or [string]::IsNullOrEmpty($volume.DriveLetter)) {
            throw 'The mounted ISO did not expose a drive letter.'
        }
        $mountRoot = "$($volume.DriveLetter):\"
        foreach ($entry in $entries) {
            $mountedFile = Join-Path $mountRoot $entry.File.Name
            if (-not (Test-Path -LiteralPath $mountedFile)) {
                throw "File '$($entry.File.Name)' is missing from the mounted ISO."
            }
            $sourceHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $entry.File.FullName).Hash
            $isoHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $mountedFile).Hash
            if ($sourceHash -ne $isoHash) {
                throw "File '$($entry.File.Name)' hash mismatch between package and ISO."
            }
        }
        $result.Validated = $true
    } finally {
        Dismount-DiskImage -ImagePath $OutputPath | Out-Null
    }
}

[pscustomobject]$result
