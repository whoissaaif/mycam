<#
  Renders MyCam's Aero (Windows 7) style icons. Re-run after changing the design:
    powershell -ExecutionPolicy Bypass -File design\tools\make_icons.ps1

  Outputs
    pc\companion\res\app.ico, tray_<state>.ico          Windows app + tray icons (16-256 px)
    design\assets\app-256.png                            large app icon for other artwork
    app\src\main\res\mipmap-*\ic_launcher_foreground.png Android adaptive launcher icon (foreground)
    app\src\main\res\mipmap-*\ic_launcher(_round).png    Android legacy launcher icons (API < 26)
    app\src\main\res\drawable-nodpi\status_<state>.png   Android status-card icons
    design\previews\icons.png                            review sheet
#>
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\aero_draw.ps1"
$root = Resolve-Path "$PSScriptRoot\..\.."
$resDir = Join-Path $root 'pc\companion\res'
$previewDir = Join-Path $root 'design\previews'
$assetDir = Join-Path $root 'design\assets'
$androidRes = Join-Path $root 'app\src\main\res'
New-Item -ItemType Directory -Force $resDir, $previewDir, $assetDir | Out-Null

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

function Save-Png($bmp, [string]$path) {
    New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
}

# --- Windows icons --------------------------------------------------------------------------------
$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$states = 'app', 'disconnected', 'ready', 'streaming', 'paused', 'error'
$rendered = @{}
foreach ($st in $states) {
    $drawState = if ($st -eq 'app') { 'ready' } else { $st }
    $rendered[$st] = foreach ($s in $sizes) { Render-Icon $s $drawState }
    $name = if ($st -eq 'app') { 'app.ico' } else { "tray_$st.ico" }
    Write-Ico (Join-Path $resDir $name) $rendered[$st]
    Write-Host "wrote $name"
}
Save-Png ($rendered['app'] | Where-Object { $_.Width -eq 256 }) (Join-Path $assetDir 'app-256.png')

# --- Android --------------------------------------------------------------------------------------
$densities = [ordered]@{ mdpi = 1; hdpi = 1.5; xhdpi = 2; xxhdpi = 3; xxxhdpi = 4 }
foreach ($d in $densities.Keys) {
    $k = $densities[$d]

    # Adaptive foreground: 108 dp canvas; keep the webcam inside the 66 dp safe zone.
    $fs = [int](108 * $k)
    $fg, $g = New-Canvas $fs $fs
    $icon = [int](64 * $k)
    Draw-Webcam $g $icon (($fs - $icon) / 2) (($fs - $icon) / 2)
    $g.Dispose()
    Save-Png $fg (Join-Path $androidRes "mipmap-$d\ic_launcher_foreground.png")

    # Legacy icons (API < 26): aurora tile + webcam, rounded square and round.
    $ls = [int](48 * $k)
    foreach ($round in $false, $true) {
        $bmp, $g = New-Canvas $ls $ls
        $tile, $tg = New-Canvas $ls $ls
        Fill-Aurora $tg $ls $ls
        Draw-Glow $tg ($ls / 2) ($ls * 0.45) ($ls * 0.5) ($ls * 0.45) (C 120 190 230 255)
        $tg.Dispose()
        $clip = New-Object System.Drawing.Drawing2D.GraphicsPath
        if ($round) { $clip.AddEllipse(1, 1, $ls - 2, $ls - 2) }
        else {
            $r = $ls * 0.36; $e = $ls - 1
            $clip.AddArc(1, 1, $r, $r, 180, 90); $clip.AddArc($e - $r, 1, $r, $r, 270, 90)
            $clip.AddArc($e - $r, $e - $r, $r, $r, 0, 90); $clip.AddArc(1, $e - $r, $r, $r, 90, 90); $clip.CloseFigure()
        }
        $tb = New-Object System.Drawing.TextureBrush($tile)
        $g.FillPath($tb, $clip); $tb.Dispose(); $tile.Dispose()
        $ic = [int]($ls * 0.72)
        Draw-Webcam $g $ic (($ls - $ic) / 2) (($ls - $ic) / 2)
        $g.Dispose()
        $file = if ($round) { 'ic_launcher_round.png' } else { 'ic_launcher.png' }
        Save-Png $bmp (Join-Path $androidRes "mipmap-$d\$file")
    }
}
foreach ($st in 'disconnected', 'ready', 'streaming', 'paused', 'error') {
    Save-Png (Render-Icon 192 $st) (Join-Path $androidRes "drawable-nodpi\status_$st.png")
}
Write-Host 'wrote Android launcher and status icons'

# --- Preview sheet --------------------------------------------------------------------------------
$rowH = 300; $labelW = 140
$sheet = New-Object System.Drawing.Bitmap ($labelW + 1100), ($rowH * $states.Count)
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
$legacy = [System.Drawing.Image]::FromFile((Join-Path $androidRes 'mipmap-xxxhdpi\ic_launcher.png'))
$roundIcon = [System.Drawing.Image]::FromFile((Join-Path $androidRes 'mipmap-xxxhdpi\ic_launcher_round.png'))
$g.DrawString('Android launcher', $font, $ink, $labelW + 870, 10)
$g.DrawImage($legacy, $labelW + 870, 40); $g.DrawImage($roundIcon, $labelW + 870, 250)
$legacy.Dispose(); $roundIcon.Dispose()
$g.Dispose()
$sheet.Save((Join-Path $previewDir 'icons.png'), [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host 'wrote design\previews\icons.png'
