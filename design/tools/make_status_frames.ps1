<#
  Renders the pictures the PC's MyCam camera shows when there is no live video, and the hills background
  the phone ships. Everything is composited over design\assets\source\hills-photo.png (the owner's photo;
  see that folder's README for its provenance), lightly blurred, with a frosted glass card on top.
    pc\companion\res\frame_paused.png    "CAMERA PAUSED"          (user pause, or Windows locked)
    pc\companion\res\frame_waiting.png   "WAITING FOR THE PHONE"  (no phone / camera starting / problem)
    app\src\main\res\drawable-nodpi\bg_hills.jpg       phone background (R.drawable.bg_hills)
    app\src\main\res\drawable-nodpi\bg_hills_blur.jpg  blurred backdrop for dialogs
    design\previews\status-frames.png                  review sheet
  Run it with:
    powershell -ExecutionPolicy Bypass -File design\tools\make_status_frames.ps1

  The frames stay 1280x720 24-bit PNGs (the size the companion embeds; see pc\companion\status_images.h).

  The Waiting card leaves an empty XP progress track under its text. The virtual camera animates the green
  marquee chunks inside it at runtime. Its rectangle is FIXED at (490, 447, 300, 18) in frame pixels: it is
  written to design\tools\frame_layout.txt and must keep matching kMarqueeTrack* in pc\companion\marquee.h,
  so the layout below is built around the track rather than the other way round.
#>
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\luna_draw.ps1"
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$resDir = Join-Path $root 'pc\companion\res'
$previewDir = Join-Path $root 'design\previews'
$drawableDir = Join-Path $root 'app\src\main\res\drawable-nodpi'

$W = 1280; $H = 720
$barX = 490; $barY = 447; $barW = 300; $barH = 18   # fixed; see the header and pc\companion\marquee.h
$ink = Hex '#FFFFFF'                                 # card type: white, as in the supplied reference
$shadow = 200                                        # ...with a soft dark shadow so it reads over the glass

function New-Font([string]$family, [double]$px, [bool]$bold = $false) {
    $style = if ($bold) { [System.Drawing.FontStyle]::Bold } else { [System.Drawing.FontStyle]::Regular }
    New-Object System.Drawing.Font($family, [single]$px, $style, [System.Drawing.GraphicsUnit]::Pixel)
}

# The largest Trebuchet Bold that fits `max` pixels once letter-spaced.
function Fit-Title([System.Drawing.Bitmap]$bmp, [string]$text, [double]$px, [double]$spacing, [double]$max) {
    $g = [System.Drawing.Graphics]::FromImage($bmp); $g.TextRenderingHint = 'AntiAlias'
    while ($px -gt 20) {
        $f = New-Font 'Trebuchet MS' $px $true
        if ((Measure-Spaced $g $text $f $spacing) -le $max) { $g.Dispose(); return $f }
        $f.Dispose(); $px -= 2
    }
    $g.Dispose()
    New-Font 'Trebuchet MS' $px $true
}

# The small "MyCam" wordmark in the bottom-left corner: the mark plus the name, both softly shadowed.
function Draw-Wordmark([System.Drawing.Bitmap]$bmp) {
    $g = [System.Drawing.Graphics]::FromImage($bmp); Set-Quality $g
    $mark = Render-Icon 34 'ready'
    $g.DrawImage($mark, 26, $bmp.Height - 54, 34, 34); $mark.Dispose()
    $g.Dispose()
    $font = New-Font 'Trebuchet MS' 25 $true
    Draw-SoftText $bmp 'MyCam' $font (Hex '#FFFFFF') 66 ($bmp.Height - 52) 2.5 180
    $font.Dispose()
}

# --- frame_paused.png: the supplied composition (glass card, crossed-out camcorder, caps title) --------
function Paused-Frame() {
    $scene = Photo-Scene $W $H -blur 3
    $cardW = 560; $cardH = 282; $cardY = 190
    $cardX = ($W - $cardW) / 2
    Draw-FrostedCard $scene (RectF $cardX $cardY $cardW $cardH) 16 26

    $glyphW = 190
    Draw-CamcorderOff $scene ($W / 2) ($cardY + 99) $glyphW (C 235 255 255 255) 1.0

    $title = Fit-Title $scene 'CAMERA PAUSED' 46 6 ($cardW - 96)
    Draw-SpacedText $scene 'CAMERA PAUSED' $title $ink ($W / 2) ($cardY + 187) 6 2.4 215 | Out-Null
    $title.Dispose()
    Draw-Wordmark $scene
    $scene
}

# --- frame_waiting.png: the same scene in a morning tint, with the empty XP progress track -------------
function Waiting-Frame() {
    $scene = Photo-Scene $W $H -Morning -blur 3
    $cardW = 760; $cardY = 127; $cardH = $barY + $barH + 36 - $cardY
    $cardX = ($W - $cardW) / 2
    Draw-FrostedCard $scene (RectF $cardX $cardY $cardW $cardH) 16 26

    $g = [System.Drawing.Graphics]::FromImage($scene); Set-Quality $g
    $mark = Render-Icon 96 'disconnected'
    $g.DrawImage($mark, [int](($W - 96) / 2), $cardY + 40, 96, 96); $mark.Dispose()
    $g.Dispose()

    $title = Fit-Title $scene 'WAITING FOR THE PHONE' 44 6 ($cardW - 96)
    Draw-SpacedText $scene 'WAITING FOR THE PHONE' $title $ink ($W / 2) ($cardY + 162) 6 2.4 215 | Out-Null
    $title.Dispose()

    $sub = New-Font 'Tahoma' 28 $true
    $y = $cardY + 225
    foreach ($line in 'Connect your phone with a USB cable,', 'or over Wi-Fi, and open MyCam.') {
        $mg = [System.Drawing.Graphics]::FromImage($scene); $mg.TextRenderingHint = 'AntiAlias'
        $lw = $mg.MeasureString($line, $sub).Width; $mg.Dispose()
        Draw-SoftText $scene $line $sub $ink (($W - $lw) / 2) $y 1.8 220
        $y += 36
    }
    $sub.Dispose()

    $g = [System.Drawing.Graphics]::FromImage($scene); Set-Quality $g
    Draw-ProgressTrack $g $barX $barY $barW $barH
    $g.Dispose()
    Draw-Wordmark $scene
    $scene
}

$paused = Paused-Frame
$waiting = Waiting-Frame

$p24 = To-Rgb24 $paused; $p24.Save((Join-Path $resDir 'frame_paused.png'), [System.Drawing.Imaging.ImageFormat]::Png); $p24.Dispose()
$w24 = To-Rgb24 $waiting; $w24.Save((Join-Path $resDir 'frame_waiting.png'), [System.Drawing.Imaging.ImageFormat]::Png); $w24.Dispose()
[System.IO.File]::WriteAllText((Join-Path $PSScriptRoot 'frame_layout.txt'), ("marquee {0} {1} {2} {3}`n" -f $barX, $barY, $barW, $barH))

$sheet = New-Object System.Drawing.Bitmap 1280, 368
$g = [System.Drawing.Graphics]::FromImage($sheet)
$g.InterpolationMode = 'HighQualityBicubic'
$g.Clear((Hex '#ECE9D8'))
$g.DrawImage($paused, 4, 4, 632, 356); $g.DrawImage($waiting, 644, 4, 632, 356)
$g.Dispose()
$sheet.Save((Join-Path $previewDir 'status-frames.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$paused.Dispose(); $waiting.Dispose(); $sheet.Dispose()
Write-Host ("wrote pc\companion\res\frame_*.png, design\previews\status-frames.png; marquee {0} {1} {2} {3}" -f $barX, $barY, $barW, $barH)

# --- Phone backgrounds --------------------------------------------------------------------------------
# JPEG, not PNG: the whole APK is about 2.8 MB, so the background has a 300 KB budget and the blurred
# backdrop (which compresses to almost nothing) has 80 KB. 1280 x 854 is the photo's own 3:2 shape, so
# nothing is cropped and a 1080 px wide phone screen still has pixels to spare.
$bg = Photo-Scene 1280 854
Write-Host ("wrote bg_hills.jpg          " + (Save-JpegUnder $bg (Join-Path $drawableDir 'bg_hills.jpg') (300KB)))
$bg.Dispose()
$blur = Photo-Scene 960 640 -blur 24
Write-Host ("wrote bg_hills_blur.jpg     " + (Save-JpegUnder $blur (Join-Path $drawableDir 'bg_hills_blur.jpg') (80KB) @(85, 80, 75)))
$blur.Dispose()
