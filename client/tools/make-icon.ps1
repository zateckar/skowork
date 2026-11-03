# Makes client/src/app/workbench.ico (improve-window-frame D4): the Workbench mark, an Emerald rounded square with an
# Electric Green "S", as PNG images at the icon sizes Windows asks for.
param([string] $Out = "$PSScriptRoot\..\src\app\workbench.ico")
Add-Type -AssemblyName System.Drawing

$emerald = [System.Drawing.Color]::FromArgb(255, 0x0E, 0x3A, 0x2F)
$green = [System.Drawing.Color]::FromArgb(255, 0x78, 0xFA, 0xAE)
$sizes = 16, 20, 24, 32, 40, 48, 64, 256

function Mark([int] $size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)
    # The square: a small margin at the larger sizes, corners about a fifth of the side.
    $m = if ($size -ge 32) { $size / 16.0 } else { 0.5 }
    $s = $size - 2 * $m
    $r = $s * 0.22
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddArc($m, $m, 2 * $r, 2 * $r, 180, 90)
    $p.AddArc($m + $s - 2 * $r, $m, 2 * $r, 2 * $r, 270, 90)
    $p.AddArc($m + $s - 2 * $r, $m + $s - 2 * $r, 2 * $r, 2 * $r, 0, 90)
    $p.AddArc($m, $m + $s - 2 * $r, 2 * $r, 2 * $r, 90, 90)
    $p.CloseFigure()
    $g.FillPath((New-Object System.Drawing.SolidBrush $emerald), $p)
    # The "S", centred on its ink; larger at the small sizes, where it must stay legible.
    $t = New-Object System.Drawing.Drawing2D.GraphicsPath
    $family = New-Object System.Drawing.FontFamily 'Segoe UI'
    $t.AddString('S', $family, [int][System.Drawing.FontStyle]::Bold, [float]($s * $(if ($size -le 24) { 1.0 } else { 0.86 })), (New-Object System.Drawing.PointF 0, 0),
        [System.Drawing.StringFormat]::GenericTypographic)
    $b = $t.GetBounds()
    $mx = New-Object System.Drawing.Drawing2D.Matrix
    $mx.Translate([float]($size / 2.0 - $b.X - $b.Width / 2.0), [float]($size / 2.0 - $b.Y - $b.Height / 2.0))
    $t.Transform($mx)
    $g.FillPath((New-Object System.Drawing.SolidBrush $green), $t)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return , $ms.ToArray()
}

# ICO: a directory of PNG images.
$images = foreach ($size in $sizes) { , (Mark $size) }
$file = [System.IO.MemoryStream]::new()
$w = [System.IO.BinaryWriter]::new($file)
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $d = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $w.Write([byte]$d); $w.Write([byte]$d); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$images[$i].Length); $w.Write([uint32]$offset)
    $offset += $images[$i].Length
}
foreach ($img in $images) { $w.Write($img) }
$w.Flush()
[System.IO.File]::WriteAllBytes([System.IO.Path]::GetFullPath($Out), $file.ToArray())
