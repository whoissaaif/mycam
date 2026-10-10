<#
  Renders the pictures the PC's MyCam camera shows when there is no live video, in the Windows XP style:
  the sunny hills scene (lightly blurred) behind a frosted glass card.
    frame_paused.png   "Camera paused"           (user pause, or Windows locked)
    frame_waiting.png  "Waiting for the phone"   (no phone / camera starting / phone problem)
  1280x720 24-bit PNGs into pc\companion\res (embedded in the companion), plus a preview sheet.
    powershell -ExecutionPolicy Bypass -File design\tools\make_status_frames.ps1

  The Waiting card leaves an empty XP progress track under its text. The virtual camera animates the green
  marquee chunks inside it at runtime. Its rectangle (outer edge of the track, 1280x720 frame pixels) is
  written to design\tools\frame_layout.txt as "marquee x y w h"; keep kMarqueeTrack* in
  pc\companion\marquee.h in sync with it. The track has a 1 px #ACA899 border and 1 px of white padding,
  so the chunks go inside a 2 px inset.
#>
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\luna_draw.ps1"
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$resDir = Join-Path $root 'pc\companion\res'
$previewDir = Join-Path $root 'design\previews'

$W = 1280; $H = 720
$cardW = 760; $cardCenterY = 300          # horizon is at 58 % (418 px); the card sits a little above it
$barW = 300; $barH = 18                 # matches pc/companion/marquee.h (8 px chunks, 2 px gaps)

function Status-Frame([string]$kind, [string]$title, [string]$subtitle) {
    $scene = if ($kind -eq 'waiting') { Paint-Hills $W $H -Morning } else { Paint-Hills $W $H }
    [LunaFx]::Blur($scene, 3)

    $titleFont = New-Object System.Drawing.Font('Trebuchet MS', 64, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $subFont = New-Object System.Drawing.Font('Tahoma', 28, [System.Drawing.GraphicsUnit]::Pixel)
    $fmt = New-Object System.Drawing.StringFormat; $fmt.Alignment = 'Center'

    # Measure, then lay out top to bottom inside the card.
    $mg = [System.Drawing.Graphics]::FromImage($scene)
    $textW = $cardW - 96
    $subSize = $mg.MeasureString($subtitle, $subFont, [int]$textW, $fmt)
    $mg.Dispose()
    $logo = 140; $padTop = 30; $titleH = 78; $subH = [Math]::Ceiling($subSize.Height)
    $cardH = $padTop + $logo + 4 + $titleH + $subH + 34
    if ($kind -eq 'waiting') { $cardH += 22 + $barH }
    $cardX = ($W - $cardW) / 2; $cardY = [Math]::Round($cardCenterY - $cardH / 2)
    $card = RectF $cardX $cardY $cardW $cardH
    Draw-FrostedCard $scene $card 16 12

    $g = [System.Drawing.Graphics]::FromImage($scene)
    Set-Quality $g
    $g.TextRenderingHint = 'AntiAlias'
    $state = if ($kind -eq 'paused') { 'paused' } else { 'disconnected' }
    $icon = Render-Icon $logo $state
    $y = $cardY + $padTop
    $g.DrawImage($icon, [int](($W - $logo) / 2), [int]$y, $logo, $logo); $icon.Dispose()
    $y += $logo + 4
    $titleB = New-Object System.Drawing.SolidBrush((Hex '#1D3F8A'))
    $g.DrawString($title, $titleFont, $titleB, (RectF $cardX $y $cardW $titleH), $fmt)
    $y += $titleH
    $subB = New-Object System.Drawing.SolidBrush((Hex '#33475B'))
    $g.DrawString($subtitle, $subFont, $subB, (RectF ($cardX + 48) $y $textW ($subH + 4)), $fmt)
    $y += $subH
    $bar = $null
    if ($kind -eq 'waiting') {
        $y += 22
        $bx = [int](($W - $barW) / 2); $by = [int]$y
        Draw-ProgressTrack $g $bx $by $barW $barH
        $bar = @($bx, $by, $barW, $barH)
    }
    $titleB.Dispose(); $subB.Dispose()

    # Small "MyCam" wordmark, bottom left.
    $mark = Render-Icon 32 'ready'
    $g.DrawImage($mark, 26, $H - 52, 32, 32); $mark.Dispose()
    $g.Dispose()
    $markFont = New-Object System.Drawing.Font('Trebuchet MS', 24, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    Draw-SoftText $scene 'MyCam' $markFont (Hex '#FFFFFF') 62 ($H - 51) 2.5 170

    @($scene, $bar)
}

$paused, $null = Status-Frame 'paused' 'Camera paused' 'Video is turned off for now.'
$waiting, $bar = Status-Frame 'waiting' 'Waiting for the phone' "Connect your phone with a USB cable,`nor over Wi-Fi, and open MyCam."
$p24 = To-Rgb24 $paused; $p24.Save((Join-Path $resDir 'frame_paused.png'), [System.Drawing.Imaging.ImageFormat]::Png); $p24.Dispose()
$w24 = To-Rgb24 $waiting; $w24.Save((Join-Path $resDir 'frame_waiting.png'), [System.Drawing.Imaging.ImageFormat]::Png); $w24.Dispose()
[System.IO.File]::WriteAllText((Join-Path $PSScriptRoot 'frame_layout.txt'), ("marquee {0} {1} {2} {3}`n" -f $bar[0], $bar[1], $bar[2], $bar[3]))

$sheet = New-Object System.Drawing.Bitmap 1280, 368
$g = [System.Drawing.Graphics]::FromImage($sheet)
$g.InterpolationMode = 'HighQualityBicubic'
$g.Clear((Hex '#ECE9D8'))
$g.DrawImage($paused, 4, 4, 632, 356); $g.DrawImage($waiting, 644, 4, 632, 356)
$g.Dispose()
$sheet.Save((Join-Path $previewDir 'status-frames.png'), [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host ("wrote pc\companion\res\frame_*.png, design\previews\status-frames.png; marquee {0} {1} {2} {3}" -f $bar[0], $bar[1], $bar[2], $bar[3])
