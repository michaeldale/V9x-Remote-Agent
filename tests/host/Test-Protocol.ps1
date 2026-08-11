$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $repoRoot 'scripts\V9xProtocol.ps1')

function Assert-Equal($Expected, $Actual, [string]$Message) {
    if ($Expected -ne $Actual) { throw "$Message Expected '$Expected', got '$Actual'." }
}

$payload = New-V9xHelloPayload -ClientLabel 'fragment-test'
$frameBytes = New-V9xFrameBytes -Type 1 -RequestId 42 -Payload $payload
$stream = [IO.MemoryStream]::new($frameBytes)
$frame = Read-V9xFrame -Stream $stream
Assert-Equal 0x0100 $frame.Version 'Protocol version mismatch.'
Assert-Equal 1 $frame.Type 'Type mismatch.'
Assert-Equal 42 $frame.RequestId 'Request ID mismatch.'
Assert-Equal $payload.Length $frame.Payload.Length 'Payload length mismatch.'

$helloPayload = [Collections.Generic.List[byte]]::new()
$helloPayload.AddRange([BitConverter]::GetBytes([uint16]0x0100))
$helloPayload.AddRange([BitConverter]::GetBytes([uint16]0))
$helloPayload.AddRange([BitConverter]::GetBytes([uint32]3))
$helloPayload.AddRange([BitConverter]::GetBytes([uint32]65536))
$helloPayload.AddRange([BitConverter]::GetBytes([uint32]7))
$helloPayload.AddRange([BitConverter]::GetBytes([uint16]9869))
$helloPayload.AddRange([BitConverter]::GetBytes([uint16]0x0101))
Add-V9xString -Bytes $helloPayload -Value 'test-build'
$hello = ConvertFrom-V9xHelloPayload -Payload $helloPayload.ToArray()
Assert-Equal 'test-build' $hello.BuildId 'Build ID mismatch.'
Assert-Equal 7 $hello.BootCounter 'Boot counter mismatch.'

$m4HelloPayload = [Collections.Generic.List[byte]]::new()
$m4HelloPayload.AddRange([BitConverter]::GetBytes([uint16]0x0100))
$m4HelloPayload.AddRange([BitConverter]::GetBytes([uint16]0))
$m4HelloPayload.AddRange([BitConverter]::GetBytes([uint32]1023))
$m4HelloPayload.AddRange([BitConverter]::GetBytes([uint32]65536))
$m4HelloPayload.AddRange([BitConverter]::GetBytes([uint32]12))
$m4HelloPayload.AddRange([BitConverter]::GetBytes([uint16]9869))
$m4HelloPayload.AddRange([BitConverter]::GetBytes([uint16]0x0101))
Add-V9xString -Bytes $m4HelloPayload -Value 'fixture-m4'
Add-V9xString -Bytes $m4HelloPayload -Value 'driver-42'
$m4HelloPayload.Add(1)
$m4HelloPayload.Add(0)
$m4HelloPayload.AddRange([BitConverter]::GetBytes([uint16]0))
$m4HelloPayload.AddRange([BitConverter]::GetBytes([uint32]800))
$m4HelloPayload.AddRange([BitConverter]::GetBytes([uint32]600))
$m4HelloPayload.AddRange([BitConverter]::GetBytes([uint32]16))
Add-V9xString -Bytes $m4HelloPayload -Value '0.0.0.0'
Add-V9xString -Bytes $m4HelloPayload -Value '192.168.1.20'
$m4Hello = ConvertFrom-V9xHelloPayload -Payload $m4HelloPayload.ToArray()
Assert-Equal 1023 $m4Hello.Capabilities 'M4 capability mismatch.'
Assert-Equal 'driver-42' $m4Hello.PendingJob 'Resume token mismatch.'
Assert-Equal $true $m4Hello.DesktopReady 'Desktop readiness mismatch.'
Assert-Equal 800 $m4Hello.ScreenWidth 'Screen width mismatch.'
Assert-Equal 600 $m4Hello.ScreenHeight 'Screen height mismatch.'
Assert-Equal 16 $m4Hello.BitsPerPixel 'Screen depth mismatch.'
Assert-Equal '0.0.0.0' $m4Hello.ListenAddress 'Listen address mismatch.'
Assert-Equal '192.168.1.20' $m4Hello.AllowedClient 'Allowed-client mismatch.'

$bad = [byte[]]$frameBytes.Clone()
$bad[0] = 0
$threw = $false
try { [void](Read-V9xFrame -Stream ([IO.MemoryStream]::new($bad))) } catch { $threw = $true }
Assert-Equal $true $threw 'Bad magic was not rejected.'

$oversized = [byte[]]$frameBytes.Clone()
[Array]::Copy([BitConverter]::GetBytes([uint32]65537), 0, $oversized, 16, 4)
$threw = $false
try { [void](Read-V9xFrame -Stream ([IO.MemoryStream]::new($oversized))) } catch { $threw = $true }
Assert-Equal $true $threw 'Oversized payload was not rejected.'

$threw = $false
try { [void](New-V9xHelloPayload -ClientLabel 'non-ascii-é') } catch { $threw = $true }
Assert-Equal $true $threw 'Non-ASCII protocol text was not rejected.'

$execPayload = New-V9xExecPayload -Mode Direct -Application 'C:\TEST.EXE' `
    -CommandLine '/quiet' -WorkingDirectory 'C:\WORK' `
    -TimeoutMilliseconds 12345 -StdoutLimit 4096 -StderrLimit 2048
Assert-Equal 0 $execPayload[0] 'Direct execution mode mismatch.'
Assert-Equal 12345 ([BitConverter]::ToUInt32($execPayload, 4)) 'Execution timeout mismatch.'
Assert-Equal 4096 ([BitConverter]::ToUInt32($execPayload, 8)) 'Stdout limit mismatch.'
Assert-Equal 2048 ([BitConverter]::ToUInt32($execPayload, 12)) 'Stderr limit mismatch.'
$execOffset = 16
Assert-Equal 'C:\TEST.EXE' (Read-V9xString -Bytes $execPayload -Offset ([ref]$execOffset)) 'Application mismatch.'
Assert-Equal '/quiet' (Read-V9xString -Bytes $execPayload -Offset ([ref]$execOffset)) 'Arguments mismatch.'
Assert-Equal 'C:\WORK' (Read-V9xString -Bytes $execPayload -Offset ([ref]$execOffset)) 'Working directory mismatch.'
Assert-Equal $execPayload.Length $execOffset 'Execution payload has trailing data.'

$completePayload = [byte[]]::new(28)
[Array]::Copy([BitConverter]::GetBytes([uint32]0), 0, $completePayload, 0, 4)
[Array]::Copy([BitConverter]::GetBytes([uint32]7), 0, $completePayload, 4, 4)
[Array]::Copy([BitConverter]::GetBytes([uint32]321), 0, $completePayload, 12, 4)
[Array]::Copy([BitConverter]::GetBytes([uint32]12), 0, $completePayload, 16, 4)
[Array]::Copy([BitConverter]::GetBytes([uint32]5), 0, $completePayload, 20, 4)
[Array]::Copy([BitConverter]::GetBytes([uint32]0x11), 0, $completePayload, 24, 4)
$complete = ConvertFrom-V9xExecComplete -Payload $completePayload
Assert-Equal 7 $complete.ExitCode 'Execution exit code mismatch.'
Assert-Equal 321 $complete.ElapsedMilliseconds 'Execution elapsed time mismatch.'
Assert-Equal $true $complete.StdoutTruncated 'Execution truncation flag mismatch.'
Assert-Equal $true $complete.PipeCapture 'Execution capture flag mismatch.'

$errorPayload = [Collections.Generic.List[byte]]::new()
$errorPayload.AddRange([BitConverter]::GetBytes([uint32]10))
$errorPayload.AddRange([BitConverter]::GetBytes([uint32]87))
Add-V9xString -Bytes $errorPayload -Value 'thread failed'
$guestError = ConvertFrom-V9xErrorPayload -Payload $errorPayload.ToArray()
Assert-Equal 10 $guestError.Status 'Guest error status mismatch.'
Assert-Equal 87 $guestError.NativeError 'Guest native error mismatch.'
Assert-Equal 'thread failed' $guestError.Detail 'Guest error detail mismatch.'

$crcBytes = [Text.Encoding]::ASCII.GetBytes('123456789')
[uint32]$runningCrc = Update-V9xCrc32 -Crc ([uint32]::MaxValue) -Bytes $crcBytes
Assert-Equal ([uint32]3421780262) ([uint32]($runningCrc -bxor [uint32]::MaxValue)) 'CRC32 mismatch.'

$writePayload = New-V9xOpenWritePayload -Path 'C:\V9XREMOTE\TEST.BIN' `
    -Size 123 -Crc32 ([uint32]0x11223344)
Assert-Equal 123 ([BitConverter]::ToUInt32($writePayload, 0)) 'Upload size mismatch.'
Assert-Equal ([uint32]0x11223344) ([BitConverter]::ToUInt32($writePayload, 4)) 'Upload CRC mismatch.'
$pathOffset = 8
Assert-Equal 'C:\V9XREMOTE\TEST.BIN' (Read-V9xString -Bytes $writePayload -Offset ([ref]$pathOffset)) 'Upload path mismatch.'

$statPayload = [byte[]]::new(16)
$statPayload[0] = 1; $statPayload[1] = 0
[Array]::Copy([BitConverter]::GetBytes([uint32]456), 0, $statPayload, 4, 4)
[Array]::Copy([BitConverter]::GetBytes([uint32]0x20), 0, $statPayload, 8, 4)
$stat = ConvertFrom-V9xStatPayload -Payload $statPayload
Assert-Equal $true $stat.Exists 'Stat existence mismatch.'
Assert-Equal 456 $stat.Size 'Stat size mismatch.'

$screenPayload = [Collections.Generic.List[byte]]::new()
$screenPayload.AddRange([BitConverter]::GetBytes([uint32]640))
$screenPayload.AddRange([BitConverter]::GetBytes([uint32]480))
$screenPayload.AddRange([BitConverter]::GetBytes([uint32]8))
$screenPayload.AddRange([BitConverter]::GetBytes([uint32]921654))
$screenPayload.AddRange([BitConverter]::GetBytes([uint32]0x12345678))
Add-V9xString -Bytes $screenPayload -Value 'C:\V9XREMOTE\TEMP\SCREEN.BMP'
$screen = ConvertFrom-V9xScreenshotPayload -Payload $screenPayload.ToArray()
Assert-Equal 640 $screen.Width 'Screenshot width mismatch.'
Assert-Equal 480 $screen.Height 'Screenshot height mismatch.'
Assert-Equal ([uint32]0x12345678) $screen.Crc32 'Screenshot CRC mismatch.'
Assert-Equal 'C:\V9XREMOTE\TEMP\SCREEN.BMP' $screen.GuestPath 'Screenshot path mismatch.'

Write-Output 'PASS: host framing, execution payloads, parsing, and limits'
