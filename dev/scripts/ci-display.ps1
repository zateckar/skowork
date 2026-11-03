# CI only: GitHub's Windows runners start with a 1024x768 display, smaller than the reference window of the desktop
# tests (1300x730). Switches the primary display to the requested size, or the largest mode below it, and fails when
# the result is still too small for the tests.
#   pwsh dev/scripts/ci-display.ps1 [-Width 1920 -Height 1080]
param([int]$Width = 1920, [int]$Height = 1080, [int]$MinWidth = 1600, [int]$MinHeight = 900)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class WbDisplay {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DEVMODE {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
        public int dmFields;
        public int dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
        public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public short dmLogPixels;
        public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
        public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool EnumDisplaySettings(string device, int mode, ref DEVMODE dm);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int ChangeDisplaySettings(ref DEVMODE dm, int flags);
}
'@

function New-DevMode {
    $dm = New-Object WbDisplay+DEVMODE
    $dm.dmDeviceName = ''
    $dm.dmFormName = ''
    $dm.dmSize = [Runtime.InteropServices.Marshal]::SizeOf($dm)
    $dm
}
function Get-CurrentMode {
    $dm = New-DevMode
    if (-not [WbDisplay]::EnumDisplaySettings([NullString]::Value, -1, [ref]$dm)) { throw 'EnumDisplaySettings failed' }
    $dm
}
function Set-Mode([int]$w, [int]$h) {
    $dm = Get-CurrentMode
    $dm.dmPelsWidth = $w
    $dm.dmPelsHeight = $h
    $dm.dmFields = 0x80000 -bor 0x100000  # DM_PELSWIDTH | DM_PELSHEIGHT
    [WbDisplay]::ChangeDisplaySettings([ref]$dm, 0)
}

$before = Get-CurrentMode
Write-Host ("display before: {0}x{1}" -f $before.dmPelsWidth, $before.dmPelsHeight)
$r = Set-Mode $Width $Height
if ($r -ne 0) {
    # Not offered as is: the largest mode the driver lists that fits the request.
    $modes = for ($i = 0; ; $i++) {
        $dm = New-DevMode
        if (-not [WbDisplay]::EnumDisplaySettings([NullString]::Value, $i, [ref]$dm)) { break }
        [pscustomobject]@{ W = $dm.dmPelsWidth; H = $dm.dmPelsHeight }
    }
    $modes = $modes | Sort-Object W, H -Unique
    Write-Host ("{0}x{1} refused ({2}); modes: {3}" -f $Width, $Height, $r, (($modes | ForEach-Object { "$($_.W)x$($_.H)" }) -join ' '))
    $best = $modes | Where-Object { $_.W -le $Width -and $_.H -le $Height } | Sort-Object { $_.W * $_.H } | Select-Object -Last 1
    if ($best) { $r = Set-Mode $best.W $best.H }
}
$after = Get-CurrentMode
Write-Host ("display after: {0}x{1}" -f $after.dmPelsWidth, $after.dmPelsHeight)
if ($after.dmPelsWidth -lt $MinWidth -or $after.dmPelsHeight -lt $MinHeight) {
    throw ("the display is {0}x{1}; the desktop tests need at least {2}x{3}" -f $after.dmPelsWidth, $after.dmPelsHeight, $MinWidth, $MinHeight)
}
