<#
  Renders MyCam's Windows XP (Luna) style icons: the "Snap" camera logo and its status family.
  Re-run after changing the design:
    powershell -ExecutionPolicy Bypass -File design\tools\make_icons.ps1

  Outputs
    pc\companion\res\app.ico, tray_<state>.ico               Windows app + tray icons (16-256 px)
    design\assets\app-256.png                                large app icon for other artwork
    app\src\main\res\mipmap-*\ic_launcher_foreground.png     Android adaptive icon foreground (the camera)
    app\src\main\res\mipmap-*\ic_launcher(_round).png        Android legacy launcher icons (API < 26)
    app\src\main\res\drawable\ic_launcher_background.xml     adaptive background: the sunny hills (vector)
    app\src\main\res\drawable\ic_launcher_monochrome.xml     themed icon: body, bezel ring, hill line
    app\src\main\res\drawable\ic_stat_webcam.xml             notification icon: white camera outline
    app\src\main\res\mipmap-anydpi-v26\ic_launcher(_round).xml
    app\src\main\res\drawable-nodpi\status_<state>.png       Android status-card icons
    design\previews\icons.png                                review sheet
#>
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\luna_draw.ps1"
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
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

function Write-Text([string]$path, [string]$text) {
    [System.IO.File]::WriteAllText($path, $text.Replace("`r`n", "`n"), (New-Object System.Text.UTF8Encoding $false))
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

# --- Android launcher -----------------------------------------------------------------------------
# The legacy tile: the hills scene with the camera on top.
function Launcher-Tile([int]$ls) {
    $tile = Paint-Hills $ls $ls -horizon 0.62
    $g = [System.Drawing.Graphics]::FromImage($tile); Set-Quality $g
    $ic = [int]($ls * 0.74)
    $cam = Render-Icon $ic 'ready'
    $g.DrawImage($cam, [int](($ls - $ic) / 2), [int](($ls - $ic) / 2 + $ls * 0.01), $ic, $ic); $cam.Dispose()
    $g.Dispose()
    $tile
}

$densities = [ordered]@{ mdpi = 1; hdpi = 1.5; xhdpi = 2; xxhdpi = 3; xxxhdpi = 4 }
foreach ($d in $densities.Keys) {
    $k = $densities[$d]

    # Adaptive foreground: 108 dp canvas; the camera stays inside the 66 dp safe zone.
    $fs = [int](108 * $k)
    $fg, $g = New-Canvas $fs $fs
    $icon = [int](62 * $k)
    $cam = Render-Icon $icon 'ready'
    $g.DrawImage($cam, [int](($fs - $icon) / 2), [int](($fs - $icon) / 2), $icon, $icon); $cam.Dispose()
    $g.Dispose()
    Save-Png $fg (Join-Path $androidRes "mipmap-$d\ic_launcher_foreground.png")

    # Legacy icons (API < 26): rounded square and round.
    $ls = [int](48 * $k)
    $tile = Launcher-Tile $ls
    foreach ($round in $false, $true) {
        $bmp, $g = New-Canvas $ls $ls
        $clip = if ($round) { Circle-Path ($ls / 2) ($ls / 2) ($ls / 2 - 1) } else { RoundRect-Path 1 1 ($ls - 2) ($ls - 2) ($ls * 0.18) }
        $tb = New-Object System.Drawing.TextureBrush($tile)
        $g.FillPath($tb, $clip); $tb.Dispose()
        $pen = New-Object System.Drawing.Pen((C 90 10 50 120), [single]([Math]::Max(1, $ls / 96)))
        $g.DrawPath($pen, $clip); $pen.Dispose(); $clip.Dispose()
        $g.Dispose()
        $file = if ($round) { 'ic_launcher_round.png' } else { 'ic_launcher.png' }
        Save-Png $bmp (Join-Path $androidRes "mipmap-$d\$file")
    }
    $tile.Dispose()
}
foreach ($st in 'disconnected', 'ready', 'streaming', 'paused', 'error') {
    Save-Png (Render-Icon 192 $st) (Join-Path $androidRes "drawable-nodpi\status_$st.png")
}

# --- Android vector drawables ---------------------------------------------------------------------
function F([double]$v) { $v.ToString('0.##', [System.Globalization.CultureInfo]::InvariantCulture) }
function RR([double]$x, [double]$y, [double]$w, [double]$h, [double]$r) {
    "M$(F ($x + $r)),$(F $y)h$(F ($w - 2 * $r))a$(F $r),$(F $r) 0,0 1,$(F $r),$(F $r)v$(F ($h - 2 * $r))a$(F $r),$(F $r) 0,0 1,-$(F $r),$(F $r)h-$(F ($w - 2 * $r))a$(F $r),$(F $r) 0,0 1,-$(F $r),-$(F $r)v-$(F ($h - 2 * $r))a$(F $r),$(F $r) 0,0 1,$(F $r),-$(F $r)z"
}
function CI([double]$cx, [double]$cy, [double]$r) {
    "M$(F ($cx - $r)),$(F $cy)a$(F $r),$(F $r) 0,1 1,$(F (2 * $r)),0a$(F $r),$(F $r) 0,1 1,-$(F (2 * $r)),0z"
}
# A hill filling the lower part of a lens of radius r (crest left of centre), clipped to the glass.
function Hill([double]$cx, [double]$cy, [double]$r) {
    "M$(F ($cx - 1.1 * $r)),$(F ($cy + 0.25 * $r)) C$(F ($cx - 0.6 * $r)),$(F ($cy - 0.45 * $r)) $(F ($cx - 0.1 * $r)),$(F ($cy - 0.4 * $r)) $(F ($cx + 0.35 * $r)),$(F ($cy + 0.0 * $r)) C$(F ($cx + 0.6 * $r)),$(F ($cy + 0.2 * $r)) $(F ($cx + 0.85 * $r)),$(F ($cy + 0.15 * $r)) $(F ($cx + 1.1 * $r)),$(F ($cy + 0.05 * $r)) L$(F ($cx + 1.1 * $r)),$(F ($cy + 1.1 * $r)) L$(F ($cx - 1.1 * $r)),$(F ($cy + 1.1 * $r))z"
}
function Grad([double]$y1,[double]$y2, [string]$c1, [string]$c2) {
@"
        <aapt:attr name="android:fillColor">
            <gradient android:type="linear" android:startX="0" android:startY="$(F $y1)" android:endX="0" android:endY="$(F $y2)">
                <item android:offset="0" android:color="#FF$c1" />
                <item android:offset="1" android:color="#FF$c2" />
            </gradient>
        </aapt:attr>
"@ + "`n"
}
function Cloud([double]$x, [double]$y, [double]$s) {
    # A small cumulus: puffs on a flat rounded base, shaded underside.
    $p = (CI ($x - 6 * $s) ($y - 2.5 * $s) (4 * $s)) + (CI ($x) ($y - 5 * $s) (5.5 * $s)) + (CI ($x + 6.5 * $s) ($y - 2.8 * $s) (4.2 * $s)) + (RR ($x - 10 * $s) ($y - 3 * $s) (21 * $s) (4 * $s) (2 * $s))
@"
    <path android:fillColor="#FFD2DEEC" android:pathData="$(CI ($x - 6 * $s + 0.5 * $s) ($y - 2.5 * $s + 0.7 * $s) (4 * $s))$(CI ($x + 0.6 * $s) ($y - 5 * $s + 0.8 * $s) (5.5 * $s))$(CI ($x + 7 * $s) ($y - 2.8 * $s + 0.7 * $s) (4.2 * $s))$(RR ($x - 9.6 * $s) ($y - 2.4 * $s) (21 * $s) (4 * $s) (2 * $s))" />
    <path android:fillColor="#FFFFFFFF" android:pathData="$p" />
"@ + "`n"
}

$bg = @"
<?xml version="1.0" encoding="utf-8"?>
<!-- Adaptive icon background: MyCam's sunny hills under a blue sky (generated by design/tools/make_icons.ps1). -->
<vector xmlns:android="http://schemas.android.com/apk/res/android"
    xmlns:aapt="http://schemas.android.com/aapt"
    android:width="108dp" android:height="108dp"
    android:viewportWidth="108" android:viewportHeight="108">
    <path android:pathData="M0,0h108v108h-108z">
$(Grad 0 66 '2E7FE0' '9CC8F2')    </path>
$(Cloud 27 30 0.9)$(Cloud 84 22 0.7)$(Cloud 96 52 0.55)    <path android:pathData="M-2,70 C14,62 30,61 44,66 C52,69 58,74 64,80 L64,108 L-2,108 Z">
$(Grad 62 90 'B4E25A' '7CC63A')    </path>
    <path android:pathData="M14,108 C30,86 50,68 70,65 C84,63 98,67 110,73 L110,108 Z">
$(Grad 64 100 '9BD640' '3E9A22')    </path>
    <path android:pathData="M-2,78 C14,74 30,79 46,90 C54,96 60,102 66,110 L-2,110 Z">
$(Grad 74 108 '8ED23C' '2F8A1F')    </path>
    <path android:pathData="M58,110 C70,98 86,93 110,95 L110,110 Z">
$(Grad 93 108 '7CC836' '2F8A1F')    </path>
</vector>
"@
Write-Text (Join-Path $androidRes 'drawable\ic_launcher_background.xml') $bg

# Themed icon (Android 13+): body silhouette with the lens cut out, the bezel ring and a hill line,
# inside the 66 dp safe zone. Geometry follows the logo: body 1.35 : 1, lens left of centre.
$lx = 52.5; $ly = 59
$mono = @"
<?xml version="1.0" encoding="utf-8"?>
<!-- Themed-icon (Android 13+) silhouette of the MyCam camera (generated by design/tools/make_icons.ps1). -->
<vector xmlns:android="http://schemas.android.com/apk/res/android"
    android:width="108dp" android:height="108dp"
    android:viewportWidth="108" android:viewportHeight="108">
    <!-- Shutter button. -->
    <path android:fillColor="#FFFFFFFF" android:pathData="$(RR 63 31.5 12 7 2)" />
    <!-- Body with the lens opening cut out. -->
    <path android:fillColor="#FFFFFFFF" android:fillType="evenOdd"
        android:pathData="$(RR 27 37 54 40 9)$(CI $lx $ly 16)" />
    <!-- Bezel ring. -->
    <path android:fillColor="#FFFFFFFF" android:fillType="evenOdd"
        android:pathData="$(CI $lx $ly 13)$(CI $lx $ly 10)" />
    <!-- Hill in the lens (a gap keeps it apart from the bezel). -->
    <group>
        <clip-path android:pathData="$(CI $lx $ly 8.2)" />
        <path android:fillColor="#FFFFFFFF" android:pathData="$(Hill $lx $ly 8.2)" />
    </group>
</vector>
"@
Write-Text (Join-Path $androidRes 'drawable\ic_launcher_monochrome.xml') $mono

# Notification icon: pure white camera outline with a hill line in the lens, transparent background.
$nx = 11.5; $ny = 13.2
$stat = @"
<?xml version="1.0" encoding="utf-8"?>
<!-- Notification icon: the MyCam camera as a white outline (generated by design/tools/make_icons.ps1). -->
<vector xmlns:android="http://schemas.android.com/apk/res/android"
    android:width="24dp" android:height="24dp"
    android:viewportWidth="24" android:viewportHeight="24">
    <path android:fillColor="#FFFFFFFF" android:pathData="$(RR 14.6 4 4.4 3 1)" />
    <path android:fillColor="#FFFFFFFF" android:fillType="evenOdd"
        android:pathData="$(RR 2 6 20 14.5 3.5)$(RR 3.8 7.8 16.4 10.9 1.8)" />
    <path android:fillColor="#FFFFFFFF" android:fillType="evenOdd"
        android:pathData="$(CI $nx $ny 4.8)$(CI $nx $ny 3.4)" />
    <group>
        <clip-path android:pathData="$(CI $nx $ny 2.5)" />
        <path android:fillColor="#FFFFFFFF" android:pathData="$(Hill $nx $ny 2.5)" />
    </group>
    <path android:fillColor="#FFFFFFFF" android:pathData="$(CI 18 10 1)" />
</vector>
"@
Write-Text (Join-Path $androidRes 'drawable\ic_stat_webcam.xml') $stat

$adaptive = @"
<?xml version="1.0" encoding="utf-8"?>
<!-- XP "Snap" camera on the sunny hills (art: design/tools/make_icons.ps1). -->
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@drawable/ic_launcher_background" />
    <foreground android:drawable="@mipmap/ic_launcher_foreground" />
    <monochrome android:drawable="@drawable/ic_launcher_monochrome" />
</adaptive-icon>
"@
Write-Text (Join-Path $androidRes 'mipmap-anydpi-v26\ic_launcher.xml') $adaptive
Write-Text (Join-Path $androidRes 'mipmap-anydpi-v26\ic_launcher_round.xml') $adaptive
Write-Host 'wrote Android launcher, vector and status icons'

# --- Preview sheet --------------------------------------------------------------------------------
$rowH = 300; $labelW = 140
$sheet = New-Object System.Drawing.Bitmap ($labelW + 1100), ($rowH * $states.Count)
$g = [System.Drawing.Graphics]::FromImage($sheet)
Set-Quality $g
$g.Clear((Hex '#ECE9D8'))
$font = New-Object System.Drawing.Font('Tahoma', 13, [System.Drawing.GraphicsUnit]::Pixel)
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
    $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
}
$legacy = [System.Drawing.Image]::FromFile((Join-Path $androidRes 'mipmap-xxxhdpi\ic_launcher.png'))
$roundIcon = [System.Drawing.Image]::FromFile((Join-Path $androidRes 'mipmap-xxxhdpi\ic_launcher_round.png'))
$g.DrawString('Android launcher', $font, $ink, $labelW + 870, 10)
$g.DrawImage($legacy, $labelW + 870, 40); $g.DrawImage($roundIcon, $labelW + 870, 250)
$legacy.Dispose(); $roundIcon.Dispose()
$g.Dispose()
$sheet.Save((Join-Path $previewDir 'icons.png'), [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host 'wrote design\previews\icons.png'
