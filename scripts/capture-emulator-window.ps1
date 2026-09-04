# Capture an emulator's window to PNG from the host, without involving the
# guest at all.
#
# Why this exists: the agent's own `screenshot` verb needs Explorer, so it
# returns exit 43 for exactly the states you most want to look at - a guest
# sitting at a logon dialog, a BIOS prompt, a boot menu, or Windows hung on its
# shutdown splash. This reads the emulator's window instead, so those states
# are observable. It is a diagnostic of last resort, not a replacement for
# `v9xctl screenshot`: it captures the host's rendering of the window, so
# window scaling and the emulator's own menus and toolbar are included.
#
# PrintWindow with PW_RENDERFULLCONTENT is used so a window that is not on top
# still renders.
[CmdletBinding(DefaultParameterSetName = 'ByName')]
param(
    [Parameter(ParameterSetName = 'ByPid', Mandatory = $true)]
    [int]$ProcessId,
    # Matched against the process command line, so the 86Box -P profile path is
    # enough: a VM directory name identifies its window.
    [Parameter(ParameterSetName = 'ByName', Mandatory = $true, Position = 0)]
    [string]$VmName,
    [string]$ProcessName = '86Box',
    [Parameter(Mandatory = $true)]
    [Alias('OutFile')]
    [string]$Destination,
    [switch]$Json
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Drawing;
using System.Runtime.InteropServices;

public class V9xWindowCapture
{
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

    public static void Capture(IntPtr window, string path)
    {
        RECT rect;
        if (!GetClientRect(window, out rect)) throw new Exception("GetClientRect failed.");
        int width = rect.Right - rect.Left, height = rect.Bottom - rect.Top;
        if (width <= 0 || height <= 0) throw new Exception("The window has no client area.");
        using (Bitmap bitmap = new Bitmap(width, height))
        {
            using (Graphics graphics = Graphics.FromImage(bitmap))
            {
                IntPtr hdc = graphics.GetHdc();
                try { PrintWindow(window, hdc, 2u); }   // 2 = PW_RENDERFULLCONTENT
                finally { graphics.ReleaseHdc(hdc); }
            }
            bitmap.Save(path, System.Drawing.Imaging.ImageFormat.Png);
        }
    }
}
'@ -ReferencedAssemblies System.Drawing

if ($PSCmdlet.ParameterSetName -eq 'ByName') {
    $candidates = @(Get-CimInstance Win32_Process -Filter "Name='$ProcessName.exe'" |
        Where-Object { $_.CommandLine -like "*$VmName*" })
    if ($candidates.Count -eq 0) {
        throw "No $ProcessName process has '$VmName' in its command line. Is the VM running?"
    }
    if ($candidates.Count -gt 1) {
        throw "'$VmName' matches $($candidates.Count) running $ProcessName processes; pass -ProcessId instead."
    }
    $ProcessId = [int]$candidates[0].ProcessId
}

$process = Get-Process -Id $ProcessId
$window = $process.MainWindowHandle
if ($window -eq [IntPtr]::Zero) {
    throw "Process $ProcessId has no main window (still starting, or running headless)."
}

$directory = Split-Path -Parent $Destination
if ($directory -and -not (Test-Path -LiteralPath $directory)) {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}
[V9xWindowCapture]::Capture($window, $Destination)

$file = Get-Item -LiteralPath $Destination
$result = [pscustomobject]@{
    Success     = $true
    ProcessId   = $ProcessId
    ProcessName = $process.ProcessName
    Destination = $file.FullName
    Bytes       = $file.Length
}
if ($Json) { $result | ConvertTo-Json -Compress } else { $result | Format-List }
exit 0
