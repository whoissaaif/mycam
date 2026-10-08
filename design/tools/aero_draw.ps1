<#
  Shared GDI+ drawing for MyCam's Aero (Windows 7) artwork. Dot-source it:  . "$PSScriptRoot\aero_draw.ps1"
  Used by make_icons.ps1, make_installer_art.ps1 and make_status_frames.ps1.
#>
Add-Type -AssemblyName System.Drawing

function C([int]$a, [int]$r, [int]$g, [int]$b) { [System.Drawing.Color]::FromArgb($a, $r, $g, $b) }

function New-Canvas([int]$w, [int]$h, [bool]$alpha = $true) {
    $format = if ($alpha) { [System.Drawing.Imaging.PixelFormat]::Format32bppArgb } else { [System.Drawing.Imaging.PixelFormat]::Format24bppRgb }
    $bmp = New-Object System.Drawing.Bitmap $w, $h, $format
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
    $g.CompositingQuality = 'HighQuality'; $g.TextRenderingHint = 'AntiAliasGridFit'
    if ($alpha) { $g.Clear([System.Drawing.Color]::Transparent) }
    @($bmp, $g)
}

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

# The MyCam webcam in an s x s box at (ox, oy). Small sizes drop the stand so the lens stays readable.
function Draw-Webcam($g, [int]$s, [single]$ox = 0, [single]$oy = 0) {
    $small = $s -le 24
    if ($small) { $cx = $ox + $s / 2; $cy = $oy + $s / 2; $R = $s * 0.47 } else { $cx = $ox + $s / 2; $cy = $oy + $s * 0.44; $R = $s * 0.37 }

    if (-not $small) {
        $neck = New-Object System.Drawing.RectangleF(($cx - 0.07 * $s), ($cy + 0.8 * $R), (0.14 * $s), (0.2 * $s))
        $nb = New-Object System.Drawing.Drawing2D.LinearGradientBrush($neck, (C 255 120 132 146), (C 255 200 210 220), 0)
        $g.FillRectangle($nb, $neck); $nb.Dispose()
        $base = New-Object System.Drawing.RectangleF(($ox + $s * 0.24), ($oy + $s * 0.83), ($s * 0.52), ($s * 0.12))
        $bb = New-Object System.Drawing.Drawing2D.LinearGradientBrush($base, (C 255 236 241 246), (C 255 112 126 142), 90)
        $g.FillEllipse($bb, $base); $bb.Dispose()
        $pen = New-Object System.Drawing.Pen((C 160 60 74 90), [single]([Math]::Max(1, $s / 128)))
        $g.DrawEllipse($pen, $base); $pen.Dispose()
    }

    Fill-Sphere $g $cx $cy $R (C 255 255 255 255) (C 255 128 142 160)
    $rim = New-Object System.Drawing.Pen((C 200 52 66 84), [single]([Math]::Max(1, $s / 96)))
    $g.DrawEllipse($rim, $cx - $R, $cy - $R, 2 * $R, 2 * $R); $rim.Dispose()

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

# Glossy status badge (streaming / paused / error) centred at (cx, cy) with radius r.
function Draw-BadgeAt($g, [single]$cx, [single]$cy, [single]$r, [string]$state, [single]$ring) {
    $colors = @{
        streaming = @((C 255 150 240 120), (C 255 16 140 30))
        paused    = @((C 255 255 228 130), (C 255 214 128 0))
        error     = @((C 255 255 150 140), (C 255 196 24 24))
    }
    if (-not $colors.ContainsKey($state)) { return }
    $white = New-Object System.Drawing.SolidBrush((C 255 255 255 255))
    $rr = $r + $ring
    $g.FillEllipse($white, $cx - $rr, $cy - $rr, 2 * $rr, 2 * $rr)
    Fill-Sphere $g $cx $cy $r $colors[$state][0] $colors[$state][1] -0.3 -0.45
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
            if ($r -ge 7) { $g.FillEllipse($white, $cx - 0.28 * $r, $cy - 0.28 * $r, 0.56 * $r, 0.56 * $r) }
        }
    }
    $white.Dispose()
    Draw-Gloss $g $cx $cy $r 140
}

# Badge in the bottom-right corner of an s x s icon.
function Draw-Badge($g, [int]$s, [string]$state) {
    $r = if ($s -le 24) { $s * 0.27 } else { $s * 0.22 }
    $c = $s - $r - [Math]::Max(0.5, $s * 0.01)
    Draw-BadgeAt $g $c $c $r $state ([Math]::Max(1, $s / 40))
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

# Webcam icon for a state: 'ready' (plain), 'disconnected' (grayscale), 'streaming' / 'paused' / 'error' (badge).
function Render-Icon([int]$s, [string]$state) {
    $bmp, $g = New-Canvas $s $s
    Draw-Webcam $g $s
    $g.Dispose()
    if ($state -eq 'disconnected') { ToGray $bmp }
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'; $g.PixelOffsetMode = 'HighQuality'
    Draw-Badge $g $s $state
    $g.Dispose()
    $bmp
}

# A soft translucent light band following a curve: the Win7 "aurora" swoosh.
function Draw-Swoosh($g, [single]$w, [single]$h, [single[]]$ys, [single]$thickness, $color) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $top = @([System.Drawing.PointF]::new(-0.2 * $w, $ys[0] * $h), [System.Drawing.PointF]::new(0.35 * $w, $ys[1] * $h),
             [System.Drawing.PointF]::new(0.7 * $w, $ys[2] * $h), [System.Drawing.PointF]::new(1.2 * $w, $ys[3] * $h))
    $bottom = @([System.Drawing.PointF]::new(1.2 * $w, $ys[3] * $h + $thickness), [System.Drawing.PointF]::new(0.7 * $w, $ys[2] * $h + $thickness * 1.6),
                [System.Drawing.PointF]::new(0.35 * $w, $ys[1] * $h + $thickness * 0.8), [System.Drawing.PointF]::new(-0.2 * $w, $ys[0] * $h + $thickness * 0.4))
    $path.AddBeziers([System.Drawing.PointF[]]$top)
    $path.AddBeziers([System.Drawing.PointF[]]$bottom)
    $path.CloseFigure()
    $brush = New-Object System.Drawing.Drawing2D.PathGradientBrush($path)
    $brush.CenterColor = $color
    $brush.SurroundColors = [System.Drawing.Color[]]@((C 0 $color.R $color.G $color.B))
    $g.FillPath($brush, $path)
    $brush.Dispose(); $path.Dispose()
}

# Deep-blue Win7 aurora background (fills the whole w x h canvas).
function Fill-Aurora($g, [single]$w, [single]$h) {
    $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush((New-Object System.Drawing.RectangleF 0, 0, $w, $h), (C 255 31 98 164), (C 255 8 34 74), 90)
    $blend = New-Object System.Drawing.Drawing2D.ColorBlend 3
    $blend.Colors = [System.Drawing.Color[]]@((C 255 46 126 190), (C 255 18 72 136), (C 255 6 28 62))
    $blend.Positions = [single[]]@(0, 0.45, 1)
    $bg.InterpolationColors = $blend
    $g.FillRectangle($bg, 0, 0, $w, $h); $bg.Dispose()
    Draw-Swoosh $g $w $h @(0.62, 0.52, 0.66, 0.5) ($h * 0.09) (C 150 120 220 240)
    Draw-Swoosh $g $w $h @(0.74, 0.7, 0.8, 0.66) ($h * 0.05) (C 120 150 240 170)
    Draw-Swoosh $g $w $h @(0.3, 0.38, 0.26, 0.34) ($h * 0.06) (C 70 200 230 255)
}

# Soft radial glow centred at (cx, cy).
function Draw-Glow($g, [single]$cx, [single]$cy, [single]$rx, [single]$ry, $color) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddEllipse($cx - $rx, $cy - $ry, 2 * $rx, 2 * $ry)
    $brush = New-Object System.Drawing.Drawing2D.PathGradientBrush($path)
    $brush.CenterColor = $color
    $brush.SurroundColors = [System.Drawing.Color[]]@((C 0 $color.R $color.G $color.B))
    $g.FillPath($brush, $path)
    $brush.Dispose(); $path.Dispose()
}
