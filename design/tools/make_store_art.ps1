<#
  Renders the Google Play store listing art in the Aero (Windows 7) style.
    powershell -ExecutionPolicy Bypass -File design\tools\make_store_art.ps1

  Outputs
    design\assets\store\play-icon-512.png        512 x 512, 32-bit PNG, full-bleed square (Play rounds the corners)
    design\assets\store\feature-graphic.png      1024 x 500, 24-bit PNG (no alpha, as Play requires)
    design\previews\store-art.png                review sheet
#>
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\aero_draw.ps1"
$root = Resolve-Path "$PSScriptRoot\..\.."
$storeDir = Join-Path $root 'design\assets\store'
New-Item -ItemType Directory -Force $storeDir | Out-Null

# --- 512 px icon: the launcher icon's aurora tile and webcam, edge to edge -------------------------
function Store-Icon([int]$s) {
    $bmp, $g = New-Canvas $s $s
    Fill-Aurora $g $s $s
    Draw-Glow $g ($s / 2) ($s * 0.45) ($s * 0.5) ($s * 0.45) (C 120 190 230 255)
    # Play masks to a rounded square (about 20 % radius); keep the webcam well inside it.
    $ic = [int]($s * 0.7)
    Draw-Webcam $g $ic (($s - $ic) / 2) (($s - $ic) / 2)
    $g.Dispose()
    $bmp
}

# --- 1024 x 500 feature graphic --------------------------------------------------------------------
function Feature-Graphic([int]$w, [int]$h) {
    $bmp, $g = New-Canvas $w $h $false
    Fill-Aurora $g $w $h

    # Webcam on the left third, with a soft glow behind it.
    $s = [int]($h * 0.7); $x = $w * 0.08; $y = ($h - $s) / 2
    Draw-Glow $g ($x + $s / 2) ($y + $s * 0.5) ($s * 0.8) ($s * 0.75) (C 110 190 230 255)
    Draw-Webcam $g $s $x $y

    # Title block on the right. Play may overlay a play button in the centre for videos only, so the
    # text sits right of centre and away from the edges.
    $tx = $w * 0.46; $tw = $w * 0.5
    $title = New-Object System.Drawing.Font('Segoe UI Light', [single]($h * 0.22), [System.Drawing.GraphicsUnit]::Pixel)
    $tag = New-Object System.Drawing.Font('Segoe UI', [single]($h * 0.075), [System.Drawing.GraphicsUnit]::Pixel)
    $small = New-Object System.Drawing.Font('Segoe UI', [single]($h * 0.05), [System.Drawing.GraphicsUnit]::Pixel)
    $white = New-Object System.Drawing.SolidBrush((C 255 255 255 255))
    $soft = New-Object System.Drawing.SolidBrush((C 235 214 228 245))
    $faint = New-Object System.Drawing.SolidBrush((C 210 170 200 235))
    $shadow = New-Object System.Drawing.SolidBrush((C 90 0 20 50))

    $ty = $h * 0.22
    $g.DrawString('MyCam', $title, $shadow, $tx + 2, $ty + 3)
    $g.DrawString('MyCam', $title, $white, $tx, $ty)
    $g.DrawString('Your phone, your webcam', $tag, $soft, (New-Object System.Drawing.RectangleF ($tx + $h * 0.012), ($ty + $h * 0.30), $tw, ($h * 0.12)))
    $dot = [string][char]0x00B7
    $line = "Wired USB  $dot  Plug and play  $dot  Windows 11"
    $g.DrawString($line, $small, $faint, (New-Object System.Drawing.RectangleF ($tx + $h * 0.014), ($ty + $h * 0.44), $tw, ($h * 0.1)))

    # Glass highlight across the top, like the Aero title bar sheen.
    $sheen = New-Object System.Drawing.Drawing2D.LinearGradientBrush((New-Object System.Drawing.RectangleF 0, 0, $w, ($h * 0.35)), (C 45 255 255 255), (C 0 255 255 255), 90)
    $g.FillRectangle($sheen, 0, 0, $w, $h * 0.35); $sheen.Dispose()
    $g.Dispose()
    $bmp
}

$icon = Store-Icon 512
$icon.Save((Join-Path $storeDir 'play-icon-512.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$feature = Feature-Graphic 1024 500
$feature.Save((Join-Path $storeDir 'feature-graphic.png'), [System.Drawing.Imaging.ImageFormat]::Png)

# Review sheet: the icon with Play's rounded mask, and the feature graphic.
$sheet = New-Object System.Drawing.Bitmap 1592, 540
$g = [System.Drawing.Graphics]::FromImage($sheet)
$g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'
$g.Clear((C 255 240 240 240))
$g.DrawImage($feature, 20, 20)
$mask = New-Object System.Drawing.Drawing2D.GraphicsPath
$r = 512 * 0.4; $ox = 1060; $oy = 20; $e = 511
$mask.AddArc($ox, $oy, $r, $r, 180, 90); $mask.AddArc($ox + $e - $r, $oy, $r, $r, 270, 90)
$mask.AddArc($ox + $e - $r, $oy + $e - $r, $r, $r, 0, 90); $mask.AddArc($ox, $oy + $e - $r, $r, $r, 90, 90); $mask.CloseFigure()
$tb = New-Object System.Drawing.TextureBrush($icon)
$tb.TranslateTransform($ox, $oy)
$g.FillPath($tb, $mask); $tb.Dispose()
$g.Dispose()
$sheet.Save((Join-Path $root 'design\previews\store-art.png'), [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host 'wrote design\assets\store\*.png and design\previews\store-art.png'
