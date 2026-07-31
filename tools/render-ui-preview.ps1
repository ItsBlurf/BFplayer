[CmdletBinding()]
param(
    [ValidateSet('Empty', 'AddMedia')]
    [string]$View = 'Empty',
    [string]$OutFile = ''
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $OutFile) {
    $relativeOutFile = if ($View -eq 'AddMedia') {
        'docs\ui-add-media-preview.png'
    }
    else {
        'docs\ui-preview.png'
    }
    $OutFile = Join-Path $root $relativeOutFile
}
$fontCollection = [Drawing.Text.PrivateFontCollection]::new()
$fontCollection.AddFontFile((Join-Path $root 'assets\fonts\NotoSans-Regular.ttf'))
$family = $fontCollection.Families[0]
$bitmap = [Drawing.Bitmap]::new(1920, 1080)
$graphics = [Drawing.Graphics]::FromImage($bitmap)
$graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::None
$graphics.TextRenderingHint = [Drawing.Text.TextRenderingHint]::AntiAliasGridFit

function Brush([string]$hex) {
    [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml($hex))
}

function Fill([string]$hex, [int]$x, [int]$y, [int]$w, [int]$h) {
    $brush = Brush $hex
    $graphics.FillRectangle($brush, $x, $y, $w, $h)
    $brush.Dispose()
}

function Round-Path([int]$x, [int]$y, [int]$w, [int]$h, [int]$r) {
    $path = [Drawing.Drawing2D.GraphicsPath]::new()
    $diameter = $r * 2
    $path.AddArc($x, $y, $diameter, $diameter, 180, 90)
    $path.AddArc($x + $w - $diameter, $y, $diameter, $diameter, 270, 90)
    $path.AddArc($x + $w - $diameter, $y + $h - $diameter, $diameter, $diameter, 0, 90)
    $path.AddArc($x, $y + $h - $diameter, $diameter, $diameter, 90, 90)
    $path.CloseFigure()
    return $path
}

function Fill-Round(
    [string]$hex,
    [int]$x,
    [int]$y,
    [int]$w,
    [int]$h,
    [int]$r,
    [string]$border = '') {
    $path = Round-Path $x $y $w $h $r
    $brush = Brush $hex
    $graphics.FillPath($brush, $path)
    if ($border) {
        $pen = [Drawing.Pen]::new([Drawing.ColorTranslator]::FromHtml($border), 1)
        $graphics.DrawPath($pen, $path)
        $pen.Dispose()
    }
    $brush.Dispose()
    $path.Dispose()
}

function Draw-Hint(
    [string]$button,
    [string]$action,
    [int]$x,
    [Drawing.Font]$font,
    [Drawing.Brush]$textBrush) {
    $buttonWidth = switch ($button) {
        'TOUCH' { 46 }
        'OPTIONS' { 32 }
        'L3' { 38 }
        'R3' { 38 }
        default { 30 }
    }
    $cy = 1035
    $actionWidth = [int]$graphics.MeasureString($action, $font).Width
    $chipWidth = 15 + $buttonWidth + 10 + $actionWidth + 17
    Fill-Round '#0d1b30' $x ($cy - 27) $chipWidth 54 14 '#1f3a60'
    $gx = $x + 15
    $pen = [Drawing.Pen]::new([Drawing.ColorTranslator]::FromHtml('#c6d3ea'), 2)
    switch ($button) {
        'X' {
            $graphics.FillEllipse((Brush '#111d32'), $gx - 1, $cy - 16, 32, 32)
            [void]($pen.Color = [Drawing.ColorTranslator]::FromHtml('#4ec2ee'))
            $graphics.DrawLine($pen, $gx + 7, $cy - 8, $gx + 23, $cy + 8)
            $graphics.DrawLine($pen, $gx + 23, $cy - 8, $gx + 7, $cy + 8)
        }
        'O' {
            $graphics.FillEllipse((Brush '#111d32'), $gx - 1, $cy - 16, 32, 32)
            [void]($pen.Color = [Drawing.ColorTranslator]::FromHtml('#f4697e'))
            $graphics.DrawEllipse($pen, $gx + 6, $cy - 9, 18, 18)
        }
        'SQUARE' {
            $graphics.FillEllipse((Brush '#111d32'), $gx - 1, $cy - 16, 32, 32)
            [void]($pen.Color = [Drawing.ColorTranslator]::FromHtml('#e16dd7'))
            $graphics.DrawRectangle($pen, $gx + 7, $cy - 8, 16, 16)
        }
        'TRIANGLE' {
            $graphics.FillEllipse((Brush '#111d32'), $gx - 1, $cy - 16, 32, 32)
            [void]($pen.Color = [Drawing.ColorTranslator]::FromHtml('#60d39d'))
            $points = [Drawing.Point[]]@(
                [Drawing.Point]::new($gx + 15, $cy - 10),
                [Drawing.Point]::new($gx + 24, $cy + 8),
                [Drawing.Point]::new($gx + 6, $cy + 8))
            $graphics.DrawPolygon($pen, $points)
        }
        'DPAD' {
            $graphics.FillRectangle((Brush '#c6d3ea'), $gx + 10, $cy - 15, 10, 30)
            $graphics.FillRectangle((Brush '#c6d3ea'), $gx, $cy - 5, 30, 10)
        }
        'TOUCH' {
            $graphics.DrawRectangle($pen, $gx, $cy - 13, 45, 27)
            $graphics.DrawLine($pen, $gx + 7, $cy - 7, $gx + 38, $cy - 7)
        }
        'OPTIONS' {
            $graphics.DrawLine($pen, $gx + 5, $cy - 8, $gx + 27, $cy - 8)
            $graphics.DrawLine($pen, $gx + 5, $cy, $gx + 27, $cy)
            $graphics.DrawLine($pen, $gx + 5, $cy + 8, $gx + 27, $cy + 8)
        }
        default {
            $graphics.DrawEllipse($pen, $gx + 2, $cy - 17, 34, 34)
            $buttonSize = $graphics.MeasureString($button, $font)
            $graphics.DrawString(
                $button,
                $font,
                $textBrush,
                $gx + ($buttonWidth - $buttonSize.Width) / 2,
                $cy - $buttonSize.Height / 2)
        }
    }
    $actionX = $gx + $buttonWidth + 10
    $graphics.DrawString($action, $font, $textBrush, $actionX, 1021)
    $pen.Dispose()
    return $x + $chipWidth + 12
}

try {
    Fill '#040811' 0 0 1920 1080
    for ($band = 0; $band -lt 8; $band++) {
        $shade = 14 + $band * 3
        Fill ([string]::Format('#{0:x2}{1:x2}{2:x2}', 5 + $band, 11 + $band * 2, 23 + $band * 4)) 0 (192 + $band * 100) 1920 100
    }
    Fill '#070e1b' 0 0 1920 190
    Fill '#0c1c34' 0 0 1920 6
    Fill '#306fcd' 0 188 1920 2
    Fill '#152948' 0 190 1920 2

    $titleFont = [Drawing.Font]::new($family, 48, [Drawing.FontStyle]::Regular, [Drawing.GraphicsUnit]::Pixel)
    $rowFont = [Drawing.Font]::new($family, 27, [Drawing.FontStyle]::Regular, [Drawing.GraphicsUnit]::Pixel)
    $footerFont = [Drawing.Font]::new($family, 22, [Drawing.FontStyle]::Regular, [Drawing.GraphicsUnit]::Pixel)
    $white = Brush '#f0f4ff'
    $muted = Brush '#9daac4'

    $border = [Drawing.Pen]::new([Drawing.ColorTranslator]::FromHtml('#192a47'), 1)

    Fill '#0a1221' 0 990 1920 90
    Fill '#1c3050' 0 990 1920 2
    $hintX = 48
    if ($View -eq 'AddMedia') {
        $graphics.DrawString('Add Media Source', $titleFont, $white, 58, 38)
        $graphics.DrawString('/mnt  |  2 ITEMS', $rowFont, $muted, 58, 119)

        Fill-Round '#081120' 42 214 1228 744 18 '#1d365b'
        Fill-Round '#091324' 1310 214 552 744 18 '#1d365b'

        Fill-Round '#19447f' 56 226 1200 60 11 '#509eff'
        Fill-Round '#f4b22a' 60 235 6 42 3
        $graphics.DrawString('FOLDER   usb0', $rowFont, $white, 84, 241)
        Fill-Round '#0d1b30' 56 292 1200 60 11
        $graphics.DrawString('FOLDER   usb1', $rowFont, $white, 84, 307)

        $helpRows = @(
            'SELECTED ITEM',
            'Cross      Open folder / add movie',
            'Triangle   Add as one TV show',
            'Square     Import as mixed library',
            'Whole-library import is opt-in'
        )
        $helpY = 352
        foreach ($helpRow in $helpRows) {
            $helpBrush = if ($helpY -eq 352) { $white } else { $muted }
            $graphics.DrawString($helpRow, $rowFont, $helpBrush, 1364, $helpY)
            $helpY += 82
        }
        $graphics.DrawString(
            'SELECT A SOURCE ON THE LEFT',
            $rowFont,
            $muted,
            1400,
            897)

        $hintX = Draw-Hint 'X' 'Open / Add Movie' $hintX $footerFont $muted
        $hintX = Draw-Hint 'TRIANGLE' 'Add TV Folder' $hintX $footerFont $muted
        $hintX = Draw-Hint 'SQUARE' 'Import Selected Folder' $hintX $footerFont $muted
        $hintX = Draw-Hint 'O' 'Up' $hintX $footerFont $muted
        $null = Draw-Hint 'OPTIONS' 'Close' $hintX $footerFont $muted
    }
    else {
        $graphics.DrawString('Library', $titleFont, $white, 58, 38)
        $graphics.DrawString('ALL MEDIA  |  0 ITEMS  |  SORT: SMART', $rowFont, $muted, 58, 119)

        Fill-Round '#081120' 42 214 1836 744 18 '#1d365b'

        $emptyTitle = 'Your library is empty'
        $emptyHelp = 'Press Cross to choose a movie or TV-show folder.'
        $titleSize = $graphics.MeasureString($emptyTitle, $titleFont)
        $helpSize = $graphics.MeasureString($emptyHelp, $rowFont)
        $graphics.DrawString($emptyTitle, $titleFont, $white, 960 - $titleSize.Width / 2, 486)
        Fill-Round '#1a4984' ([int](960 - ($helpSize.Width + 76) / 2)) 574 ([int]($helpSize.Width + 76)) 64 14 '#53a4ff'
        $graphics.DrawString($emptyHelp, $rowFont, $white, 960 - $helpSize.Width / 2, 587)

        $hintX = Draw-Hint 'X' 'Add Media' $hintX $footerFont $muted
        $null = Draw-Hint 'OPTIONS' 'Menu' $hintX $footerFont $muted
    }

    $target = [IO.Path]::GetFullPath($OutFile)
    $directory = Split-Path -Parent $target
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $bitmap.Save($target, [Drawing.Imaging.ImageFormat]::Png)
    Write-Output $target
}
finally {
    if ($border) { $border.Dispose() }
    if ($white) { $white.Dispose() }
    if ($muted) { $muted.Dispose() }
    if ($titleFont) { $titleFont.Dispose() }
    if ($rowFont) { $rowFont.Dispose() }
    if ($footerFont) { $footerFont.Dispose() }
    $graphics.Dispose()
    $bitmap.Dispose()
    $fontCollection.Dispose()
}
