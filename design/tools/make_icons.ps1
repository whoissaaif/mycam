<#
  Renders MyCam's Aero (Windows 7) style icons with GDI+ and writes multi-size .ico files plus a preview
  sheet. Re-run after changing the design:  powershell -ExecutionPolicy Bypass -File design\tools\make_icons.ps1

  Outputs
    pc\companion\res\app.ico                 glossy webcam (exe, settings window, installer)
    pc\companion\res\tray_<state>.ico        tray status icons: disconnected, ready, streaming, paused, error
    design\previews\icons.png                every icon at every size, for review

  Style (see IMPROVEMENTS.md section 7): silver glossy sphere, deep blue glass lens with a white glare,
  soft top gloss, and a glossy status badge (bottom right) for tray states.
#>
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Resolve-Path "$PSScriptRoot\..\.."
$resDir = Join-Path $root 'pc\companion\res'
$previewDir = Join-Path $root 'design\previews'
New-Item -ItemType Directory -Force $resDir, $previewDir | Out-Null

$sizes = 16, 20, 24, 32, 40, 48, 64, 256

function C([int]$a, [int]$r, [int]$g, [int]$b) { [System.Drawing.Color]::FromArgb($a, $r, $g, $b) }

function Circle-Path([single]$cx, [single]$cy, [single]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddEllipse($cx - $r, $cy - $r, 2 * $r, 2 * $r)
    $p
}

# Radial-gradient filled circle with the bright centre offset (light from the top left).
function Fill-Sphere($g, [single]$cx, [single]$cy, [single]$r, $inner, $outer, [single]$hx = -0.35, [single]$hy = -0.4) {
    $path = Circle-Path $cx $cy $r
    $brush = New-Object System.Drawing.Drawing2D.PathGradientBrush($path)
    $brush.CenterPoint = New-Object System.Drawing.PointF(($cx + $hx * $r), ($cy + $hy * $r))
    $brush.CenterColor = $inner
    $brush.SurroundColors = [System.Drawing.Color[]]@($outer)
    $g.FillPath($brush, $path)
    $brush.Dispose(); $path.Dispose()
}

# The Aero gloss: a soft white ellipse over the top half.
function Draw-Gloss($g, [single]$cx, [single]$cy, [single]$r, [int]$alpha) {
    $rect = New-Object System.Drawing.RectangleF(($cx - 0.78 * $r), ($cy - 0.92 * $r), (1.56 * $r), (1.0 * $r))
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, (C $alpha 255 255 255), (C 0 255 255 255), 90)
    $brush.WrapMode = [System.Drawing.Drawing2D.WrapMode]::TileFlipXY # No bright seam where the gradient would wrap.
    $g.FillEllipse($brush, $rect)
    $brush.Dispose()
}

function Draw-Webcam($g, [int]$s, [bool]$gray) {
    $small = $s -le 24
    # Small sizes drop the stand and use the whole canvas for the head, so the lens stays readable.
    if ($small) { $cx = $s / 2; $cy = $s / 2; $R = $s * 0.47 } else { $cx = $s / 2; $cy = $s * 0.44; $R = $s * 0.37 }

    if (-not $small) {
        # Stand: neck and an elliptical silver base.
        $neck = New-Object System.Drawing.RectangleF(($cx - 0.07 * $s), ($cy + 0.8 * $R), (0.14 * $s), (0.2 * $s))
        $nb = New-Object System.Drawing.Drawing2D.LinearGradientBrush($neck, (C 255 120 132 146), (C 255 200 210 220), 0)
        $g.FillRectangle($nb, $neck); $nb.Dispose()
        $base = New-Object System.Drawing.RectangleF(($s * 0.24), ($s * 0.83), ($s * 0.52), ($s * 0.12))
        $bb = New-Object System.Drawing.Drawing2D.LinearGradientBrush($base, (C 255 236 241 246), (C 255 112 126 142), 90)
        $g.FillEllipse($bb, $base); $bb.Dispose()
        $pen = New-Object System.Drawing.Pen((C 160 60 74 90), [single]([Math]::Max(1, $s / 128)))
        $g.DrawEllipse($pen, $base); $pen.Dispose()
    }

    # Head: silver sphere with a thin dark rim.
    Fill-Sphere $g $cx $cy $R (C 255 255 255 255) (C 255 128 142 160)
    $rim = New-Object System.Drawing.Pen((C 200 52 66 84), [single]([Math]::Max(1, $s / 96)))
    $g.DrawEllipse($rim, $cx - $R, $cy - $R, 2 * $R, 2 * $R); $rim.Dispose()

    # Lens: dark bezel, blue glass, pupil, glare.
    Fill-Sphere $g $cx $cy (0.62 * $R) (C 255 70 84 100) (C 255 18 26 36) 0.2 0.3
    Fill-Sphere $g $cx $cy (0.47 * $R) (C 255 74 160 238) (C 255 6 34 78) -0.1 -0.1
    $pupil = New-Object System.Drawing.SolidBrush((C 255 4 12 24))
    $g.FillEllipse($pupil, $cx - 0.2 * $R, $cy - 0.2 * $R, 0.4 * $R, 0.4 * $R); $pupil.Dispose()
    $glare = New-Object System.Drawing.SolidBrush((C 230 255 255 255))
    $g.FillEllipse($glare, $cx - 0.36 * $R, $cy - 0.38 * $R, 0.3 * $R, 0.22 * $R); $glare.Dispose()
    if (-not $small) {
        $dot = New-Object System.Drawing.SolidBrush((C 200 255 255 255))
        $g.FillEllipse($dot, $cx + 0.16 * $R, $cy + 0.18 * $R, 0.09 * $R, 0.09 * $R); $dot.Dispose()
    }

    Draw-Gloss $g $cx $cy $R 110
}

function Draw-Badge($g, [int]$s, [string]$state) {
    $colors = @{
        streaming = @((C 255 150 240 120), (C 255 16 140 30))
        paused    = @((C 255 255 228 130), (C 255 214 128 0))
        error     = @((C 255 255 150 140), (C 255 196 24 24))
    }
    if (-not $colors.ContainsKey($state)) { return }
    $r = if ($s -le 24) { $s * 0.27 } else { $s * 0.22 }
    $cx = $s - $r - [Math]::Max(0.5, $s * 0.01); $cy = $s - $r - [Math]::Max(0.5, $s * 0.01)

    # White ring separates the badge from the webcam behind it.
    $ring = New-Object System.Drawing.SolidBrush((C 255 255 255 255))
    $rr = $r + [Math]::Max(1, $s / 40)
    $g.FillEllipse($ring, $cx - $rr, $cy - $rr, 2 * $rr, 2 * $rr); $ring.Dispose()
    Fill-Sphere $g $cx $cy $r $colors[$state][0] $colors[$state][1] -0.3 -0.45

    $white = New-Object System.Drawing.SolidBrush((C 255 255 255 255))
    switch ($state) {
        'paused' {
            $w = 0.24 * $r; $h = 0.95 * $r
            $g.FillRectangle($white, $cx - 0.42 * $r, $cy - $h / 2, $w, $h)
            $g.FillRectangle($white, $cx + 0.18 * $r, $cy - $h / 2, $w, $h)
        }
        'error' {
            $g.FillRectangle($white, $cx - 0.12 * $r, $cy - 0.6 * $r, 0.24 * $r, 0.7 * $r)
            $g.FillEllipse($white, $cx - 0.13 * $r, $cy + 0.28 * $r, 0.26 * $r, 0.26 * $r)
        }
        'streaming' {
            if ($s -ge 32) { $g.FillEllipse($white, $cx - 0.28 * $r, $cy - 0.28 * $r, 0.56 * $r, 0.56 * $r) }
        }
    }
    $white.Dispose()
    Draw-Gloss $g $cx $cy $r 140
}

function ToGray([System.Drawing.Bitmap]$bmp) {
    for ($y = 0; $y -lt $bmp.Height; $y++) {
        for ($x = 0; $x -lt $bmp.Width; $x++) {
            $p = $bmp.GetPixel($x, $y)
            $l = [int](0.3 * $p.R + 0.59 * $p.G + 0.11 * $p.B)
            $l = [int](128 + ($l - 128) * 0.85) # Slightly flatter, reads as "inactive".
            $bmp.SetPixel($x, $y, [System.Drawing.Color]::FromArgb([int]($p.A * 0.8), $l, $l, $l))
        }
    }
}

function Render([int]$s, [string]$state) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'; $g.PixelOffsetMode = 'HighQuality'; $g.CompositingQuality = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)
    Draw-Webcam $g $s ($state -eq 'disconnected')
    $g.Dispose()
    if ($state -eq 'disconnected') { ToGray $bmp }
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'; $g.PixelOffsetMode = 'HighQuality'
    Draw-Badge $g $s $state
    $g.Dispose()
    $bmp
}

function Write-Ico([string]$path, [System.Drawing.Bitmap[]]$bitmaps) {
    $pngs = foreach ($b in $bitmaps) { $ms = New-Object System.IO.MemoryStream; $b.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png); , $ms.ToArray() }
    $out = New-Object System.IO.MemoryStream
    $w = New-Object System.IO.BinaryWriter $out
    $w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$bitmaps.Count)
    $offset = 6 + 16 * $bitmaps.Count
    for ($i = 0; $i -lt $bitmaps.Count; $i++) {
        $d = if ($bitmaps[$i].Width -ge 256) { 0 } else { $bitmaps[$i].Width }
        $w.Write([byte]$d); $w.Write([byte]$d); $w.Write([byte]0); $w.Write([byte]0)
        $w.Write([UInt16]1); $w.Write([UInt16]32); $w.Write([UInt32]$pngs[$i].Length); $w.Write([UInt32]$offset)
        $offset += $pngs[$i].Length
    }
    foreach ($p in $pngs) { $w.Write($p) }
    $w.Flush()
    [System.IO.File]::WriteAllBytes($path, $out.ToArray())
}

$states = 'app', 'disconnected', 'ready', 'streaming', 'paused', 'error'
$rendered = @{}
foreach ($st in $states) {
    $drawState = if ($st -eq 'app') { 'ready' } else { $st }
    $rendered[$st] = foreach ($s in $sizes) { Render $s $drawState }
    $name = if ($st -eq 'app') { 'app.ico' } else { "tray_$st.ico" }
    Write-Ico (Join-Path $resDir $name) $rendered[$st]
    Write-Host "wrote $name"
}

# Large PNG of the app icon for other artwork (installer, Android launcher source).
$assetDir = Join-Path $root 'design\assets'
New-Item -ItemType Directory -Force $assetDir | Out-Null
($rendered['app'] | Where-Object { $_.Width -eq 256 }).Save((Join-Path $assetDir 'app-256.png'), [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host 'wrote design\assets\app-256.png'

# Preview sheet: one row per icon, every size at 1x, plus 16/24/32 enlarged 4x (nearest) to judge pixels.
$cell = 272; $rowH = 300; $labelW = 140
$sheet = New-Object System.Drawing.Bitmap ($labelW + $cell * 2 + 600), ($rowH * $states.Count)
$g = [System.Drawing.Graphics]::FromImage($sheet)
$g.Clear((C 255 240 240 240))
$font = New-Object System.Drawing.Font('Segoe UI', 13)
$ink = New-Object System.Drawing.SolidBrush((C 255 30 30 30))
for ($row = 0; $row -lt $states.Count; $row++) {
    $st = $states[$row]; $y0 = $row * $rowH + 10
    $g.DrawString($st, $font, $ink, 10, $y0 + 120)
    $x = $labelW
    foreach ($b in $rendered[$st]) { if ($b.Width -eq 256) { $g.DrawImage($b, $x, $y0) } }
    $x += 270
    foreach ($b in $rendered[$st]) { if ($b.Width -lt 256) { $g.DrawImage($b, $x, $y0 + 200 - $b.Height); $x += $b.Width + 10 } }
    $g.InterpolationMode = 'NearestNeighbor'; $g.PixelOffsetMode = 'Half'
    $x = $labelW + 270
    foreach ($b in $rendered[$st]) { if ($b.Width -in 16, 24, 32) { $g.DrawImage($b, $x, $y0, $b.Width * 4, $b.Height * 4); $x += $b.Width * 4 + 16 } }
    $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'Default'
}
$g.Dispose()
$sheet.Save((Join-Path $previewDir 'icons.png'), [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host 'wrote design\previews\icons.png'
