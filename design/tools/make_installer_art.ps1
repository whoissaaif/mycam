<#
  Renders the installer wizard artwork in the Aero (Windows 7) style: a deep-blue "aurora" side panel
  with the MyCam webcam, and a small header image. Run make_icons.ps1 first (it writes app-256.png).
    powershell -ExecutionPolicy Bypass -File design\tools\make_installer_art.ps1

  Outputs installer\art\wizard-<scale>.bmp and installer\art\small-<scale>.bmp at the sizes Inno Setup's
  modern wizard expects for 100/150/200 % display scaling, plus design\previews\installer-art.png.
#>
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Resolve-Path "$PSScriptRoot\..\.."
$artDir = Join-Path $root 'installer\art'
New-Item -ItemType Directory -Force $artDir | Out-Null
$icon = [System.Drawing.Image]::FromFile((Join-Path $root 'design\assets\app-256.png'))

function C([int]$a, [int]$r, [int]$g, [int]$b) { [System.Drawing.Color]::FromArgb($a, $r, $g, $b) }

function New-Canvas([int]$w, [int]$h) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
    $g.TextRenderingHint = 'AntiAliasGridFit'
    @($bmp, $g)
}

# A soft translucent light band following a curve: the Win7 "aurora" swoosh.
function Draw-Swoosh($g, [single]$w, [single]$h, [single[]]$ys, [single]$thickness, $color) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $top = @(([System.Drawing.PointF]::new(-0.2 * $w, $ys[0] * $h)), ([System.Drawing.PointF]::new(0.35 * $w, $ys[1] * $h)),
             ([System.Drawing.PointF]::new(0.7 * $w, $ys[2] * $h)), ([System.Drawing.PointF]::new(1.2 * $w, $ys[3] * $h)))
    $bottom = @(([System.Drawing.PointF]::new(1.2 * $w, $ys[3] * $h + $thickness)), ([System.Drawing.PointF]::new(0.7 * $w, $ys[2] * $h + $thickness * 1.6)),
                ([System.Drawing.PointF]::new(0.35 * $w, $ys[1] * $h + $thickness * 0.8)), ([System.Drawing.PointF]::new(-0.2 * $w, $ys[0] * $h + $thickness * 0.4)))
    $path.AddBeziers([System.Drawing.PointF[]]$top)
    $path.AddBeziers([System.Drawing.PointF[]]$bottom)
    $path.CloseFigure()
    $brush = New-Object System.Drawing.Drawing2D.PathGradientBrush($path)
    $brush.CenterColor = $color
    $brush.SurroundColors = [System.Drawing.Color[]]@((C 0 $color.R $color.G $color.B))
    $g.FillPath($brush, $path)
    $brush.Dispose(); $path.Dispose()
}

function Wizard-Image([int]$w, [int]$h) {
    $bmp, $g = New-Canvas $w $h
    $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush((New-Object System.Drawing.Rectangle 0, 0, $w, $h), (C 255 31 98 164), (C 255 8 34 74), 90)
    $blend = New-Object System.Drawing.Drawing2D.ColorBlend 3
    $blend.Colors = [System.Drawing.Color[]]@((C 255 46 126 190), (C 255 18 72 136), (C 255 6 28 62))
    $blend.Positions = [single[]]@(0, 0.45, 1)
    $bg.InterpolationColors = $blend
    $g.FillRectangle($bg, 0, 0, $w, $h); $bg.Dispose()

    Draw-Swoosh $g $w $h @(0.62, 0.52, 0.66, 0.5) ($h * 0.09) (C 150 120 220 240)
    Draw-Swoosh $g $w $h @(0.74, 0.7, 0.8, 0.66) ($h * 0.05) (C 120 150 240 170)
    Draw-Swoosh $g $w $h @(0.3, 0.38, 0.26, 0.34) ($h * 0.06) (C 70 200 230 255)

    # Glow behind the icon, then the icon.
    $s = [single]($w * 0.68); $x = ($w - $s) / 2; $y = $h * 0.14
    $glowPath = New-Object System.Drawing.Drawing2D.GraphicsPath
    $glowPath.AddEllipse($x - $s * 0.25, $y - $s * 0.2, $s * 1.5, $s * 1.4)
    $glow = New-Object System.Drawing.Drawing2D.PathGradientBrush($glowPath)
    $glow.CenterColor = C 110 190 230 255
    $glow.SurroundColors = [System.Drawing.Color[]]@((C 0 190 230 255))
    $g.FillPath($glow, $glowPath); $glow.Dispose(); $glowPath.Dispose()
    $g.DrawImage($icon, $x, $y, $s, $s)

    # Wordmark.
    $font = New-Object System.Drawing.Font('Segoe UI Light', [single]($w * 0.15), [System.Drawing.GraphicsUnit]::Pixel)
    $small = New-Object System.Drawing.Font('Segoe UI', [single]($w * 0.068), [System.Drawing.GraphicsUnit]::Pixel)
    $fmt = New-Object System.Drawing.StringFormat; $fmt.Alignment = 'Center'
    $white = New-Object System.Drawing.SolidBrush((C 255 255 255 255))
    $soft = New-Object System.Drawing.SolidBrush((C 210 220 236 250))
    $g.DrawString('MyCam', $font, $white, (New-Object System.Drawing.RectangleF 0, ($y + $s + $h * 0.04), $w, ($w * 0.25)), $fmt)
    $g.DrawString('Your phone, your webcam', $small, $soft, (New-Object System.Drawing.RectangleF 0, ($y + $s + $h * 0.04 + $w * 0.21), $w, ($w * 0.12)), $fmt)
    $g.Dispose()
    $bmp
}

function Small-Image([int]$w, [int]$h) {
    $bmp, $g = New-Canvas $w $h
    $g.Clear((C 255 255 255 255)) # Inno's header strip is white.
    $s = [single]([Math]::Min($w, $h) * 0.96)
    $g.DrawImage($icon, ($w - $s) / 2, ($h - $s) / 2, $s, $s)
    $g.Dispose()
    $bmp
}

# Inno Setup 6 modern-wizard sizes for 100 %, 150 % and 200 % scaling.
$wizard = @{ 100 = @(164, 314); 150 = @(246, 459); 200 = @(328, 604) }
$smallSizes = @{ 100 = @(55, 55); 150 = @(83, 80); 200 = @(110, 106) }
$previews = @()
foreach ($scale in 100, 150, 200) {
    $b = Wizard-Image $wizard[$scale][0] $wizard[$scale][1]
    $b.Save((Join-Path $artDir "wizard-$scale.bmp"), [System.Drawing.Imaging.ImageFormat]::Bmp)
    if ($scale -eq 200) { $previews += $b }
    $sb = Small-Image $smallSizes[$scale][0] $smallSizes[$scale][1]
    $sb.Save((Join-Path $artDir "small-$scale.bmp"), [System.Drawing.Imaging.ImageFormat]::Bmp)
    if ($scale -eq 200) { $previews += $sb }
}
$sheet = New-Object System.Drawing.Bitmap 480, 620
$g = [System.Drawing.Graphics]::FromImage($sheet)
$g.Clear((C 255 240 240 240))
$g.DrawImage($previews[0], 8, 8)
$g.DrawImage($previews[1], 350, 8)
$g.Dispose()
$sheet.Save((Join-Path $root 'design\previews\installer-art.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$icon.Dispose()
Write-Host "wrote installer\art\*.bmp and design\previews\installer-art.png"
