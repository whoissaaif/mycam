<#
  Renders the pictures the PC's MyCam camera shows when there is no live video, in the Aero style:
    frame_paused.png   "Camera paused"           (user pause, or Windows locked)
    frame_waiting.png  "Waiting for your phone"  (no phone / camera starting / phone problem)
  1280x720 PNGs into pc\companion\res (embedded in the companion), plus previews.
    powershell -ExecutionPolicy Bypass -File design\tools\make_status_frames.ps1
#>
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\aero_draw.ps1"
$root = Resolve-Path "$PSScriptRoot\..\.."
$resDir = Join-Path $root 'pc\companion\res'
$previewDir = Join-Path $root 'design\previews'

$W = 1280; $H = 720

function Status-Frame([string]$kind, [string]$title, [string]$subtitle) {
    $bmp, $g = New-Canvas $W $H $false
    Fill-Aurora $g $W $H
    $cx = $W / 2; $cy = $H * 0.38
    Draw-Glow $g $cx $cy 300 230 (C 120 190 230 255)
    if ($kind -eq 'paused') {
        Draw-BadgeAt $g $cx $cy 118 'paused' 6
    } else {
        $s = 270
        Draw-Webcam $g $s ($cx - $s / 2) ($cy - $s * 0.48)
    }
    $fmt = New-Object System.Drawing.StringFormat; $fmt.Alignment = 'Center'
    $big = New-Object System.Drawing.Font('Segoe UI Light', 64, [System.Drawing.GraphicsUnit]::Pixel)
    $small = New-Object System.Drawing.Font('Segoe UI', 28, [System.Drawing.GraphicsUnit]::Pixel)
    $shadow = New-Object System.Drawing.SolidBrush((C 90 0 10 30))
    $white = New-Object System.Drawing.SolidBrush((C 255 255 255 255))
    $soft = New-Object System.Drawing.SolidBrush((C 220 215 232 250))
    $g.DrawString($title, $big, $shadow, (New-Object System.Drawing.RectangleF 2, 492, $W, 90), $fmt)
    $g.DrawString($title, $big, $white, (New-Object System.Drawing.RectangleF 0, 490, $W, 90), $fmt)
    $g.DrawString($subtitle, $small, $soft, (New-Object System.Drawing.RectangleF 0, 578, $W, 50), $fmt)
    $mark = New-Object System.Drawing.Font('Segoe UI Semibold', 22, [System.Drawing.GraphicsUnit]::Pixel)
    $g.DrawString('MyCam', $mark, $soft, 30, $H - 52)
    $g.Dispose()
    $bmp
}

$paused = Status-Frame 'paused' 'Camera paused' 'Video is turned off for now.'
$waiting = Status-Frame 'waiting' 'Waiting for the phone' 'Connect your phone with a USB cable and open MyCam.'
$paused.Save((Join-Path $resDir 'frame_paused.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$waiting.Save((Join-Path $resDir 'frame_waiting.png'), [System.Drawing.Imaging.ImageFormat]::Png)

$sheet = New-Object System.Drawing.Bitmap 1280, 368
$g = [System.Drawing.Graphics]::FromImage($sheet)
$g.InterpolationMode = 'HighQualityBicubic'
$g.Clear((C 255 240 240 240))
$g.DrawImage($paused, 4, 4, 632, 356); $g.DrawImage($waiting, 644, 4, 632, 356)
$g.Dispose()
$sheet.Save((Join-Path $previewDir 'status-frames.png'), [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host 'wrote pc\companion\res\frame_*.png and design\previews\status-frames.png'
