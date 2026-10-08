<#
  Renders the installer wizard artwork in the Aero (Windows 7) style: a deep-blue "aurora" side panel
  with the MyCam webcam, and a small header image.
    powershell -ExecutionPolicy Bypass -File design\tools\make_installer_art.ps1

  Outputs installer\art\wizard-<scale>.bmp and installer\art\small-<scale>.bmp at the sizes Inno Setup's
  modern wizard expects for 100/150/200 % display scaling, plus design\previews\installer-art.png.
#>
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\aero_draw.ps1"
$root = Resolve-Path "$PSScriptRoot\..\.."
$artDir = Join-Path $root 'installer\art'
New-Item -ItemType Directory -Force $artDir | Out-Null

function Wizard-Image([int]$w, [int]$h) {
    $bmp, $g = New-Canvas $w $h $false
    Fill-Aurora $g $w $h

    $s = [int]($w * 0.68); $x = ($w - $s) / 2; $y = $h * 0.14
    Draw-Glow $g ($x + $s / 2) ($y + $s * 0.5) ($s * 0.75) ($s * 0.7) (C 110 190 230 255)
    Draw-Webcam $g $s $x $y

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
    $bmp, $g = New-Canvas $w $h $false
    $g.Clear((C 255 255 255 255)) # Inno's header strip is white.
    $s = [int]([Math]::Min($w, $h) * 0.96)
    Draw-Webcam $g $s (($w - $s) / 2) (($h - $s) / 2)
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
    $sb = Small-Image $smallSizes[$scale][0] $smallSizes[$scale][1]
    $sb.Save((Join-Path $artDir "small-$scale.bmp"), [System.Drawing.Imaging.ImageFormat]::Bmp)
    if ($scale -eq 200) { $previews += $b, $sb }
}
$sheet = New-Object System.Drawing.Bitmap 480, 620
$g = [System.Drawing.Graphics]::FromImage($sheet)
$g.Clear((C 255 240 240 240))
$g.DrawImage($previews[0], 8, 8)
$g.DrawImage($previews[1], 350, 8)
$g.Dispose()
$sheet.Save((Join-Path $root 'design\previews\installer-art.png'), [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host 'wrote installer\art\*.bmp and design\previews\installer-art.png'
