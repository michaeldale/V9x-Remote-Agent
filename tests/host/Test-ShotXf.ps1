[CmdletBinding()]
param(
    # Written by `test_shotxf.exe dump <dir>`: <name>.bmp plus <name>.rgb, the
    # top-down R,G,B bytes the C reference decoder produced.
    [Parameter(Mandatory)] [string]$FixtureDir
)

# The agent's shaped screenshots (8-bit palette, RLE8, cropped, scaled) must
# open in System.Drawing exactly as the C reference decodes them, because
# v9xctl converts every screenshot through it.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$fixtures = @(Get-ChildItem -LiteralPath $FixtureDir -Filter '*.bmp')
if ($fixtures.Count -lt 6) { throw "Expected 6 screenshot fixtures in $FixtureDir, found $($fixtures.Count)." }
foreach ($fixture in $fixtures) {
    $expected = [IO.File]::ReadAllBytes([IO.Path]::ChangeExtension($fixture.FullName, '.rgb'))
    $image = [Drawing.Bitmap]::new($fixture.FullName)
    try {
        if ($image.Width * $image.Height * 3 -ne $expected.Length) {
            throw "$($fixture.Name): System.Drawing sees $($image.Width)x$($image.Height)."
        }
        $at = 0
        for ($y = 0; $y -lt $image.Height; ++$y) {
            for ($x = 0; $x -lt $image.Width; ++$x) {
                $pixel = $image.GetPixel($x, $y)
                if ($pixel.R -ne $expected[$at] -or $pixel.G -ne $expected[$at + 1] -or
                    $pixel.B -ne $expected[$at + 2]) {
                    throw "$($fixture.Name): pixel ($x,$y) is $($pixel.R),$($pixel.G),$($pixel.B), expected $($expected[$at]),$($expected[$at + 1]),$($expected[$at + 2])."
                }
                $at += 3
            }
        }
    } finally {
        $image.Dispose()
    }
}
Write-Host "System.Drawing decodes all $($fixtures.Count) screenshot fixtures exactly."
