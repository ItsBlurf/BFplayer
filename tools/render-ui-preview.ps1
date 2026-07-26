[CmdletBinding()]
param(
    [string]$OutFile = (Join-Path $PSScriptRoot '..\docs\ui-preview.png')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
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

function Draw-Hint(
    [string]$button,
    [string]$action,
    [int]$x,
    [Drawing.Font]$font,
    [Drawing.Brush]$textBrush) {
    $pen = [Drawing.Pen]::new([Drawing.ColorTranslator]::FromHtml('#e2ebfb'), 2)
    $buttonWidth = switch ($button) {
        'TOUCH' { 43 }
        'OPTIONS' { 36 }
        'L3' { 38 }
        'R3' { 38 }
        default { 30 }
    }
    $cy = 1035
    switch ($button) {
        'X' {
            $graphics.DrawLine($pen, $x + 5, $cy - 10, $x + 25, $cy + 10)
            $graphics.DrawLine($pen, $x + 25, $cy - 10, $x + 5, $cy + 10)
        }
        'O' { $graphics.DrawEllipse($pen, $x + 4, $cy - 11, 22, 22) }
        'SQUARE' { $graphics.DrawRectangle($pen, $x + 4, $cy - 11, 22, 22) }
        'TRIANGLE' {
            $points = [Drawing.Point[]]@(
                [Drawing.Point]::new($x + 15, $cy - 12),
                [Drawing.Point]::new($x + 27, $cy + 11),
                [Drawing.Point]::new($x + 3, $cy + 11))
            $graphics.DrawPolygon($pen, $points)
        }
        'DPAD' {
            $graphics.DrawRectangle($pen, $x + 11, $cy - 14, 8, 28)
            $graphics.DrawRectangle($pen, $x + 1, $cy - 4, 28, 8)
        }
        'TOUCH' {
            $graphics.DrawRectangle($pen, $x, $cy - 12, 43, 24)
            $graphics.DrawLine($pen, $x + 21, $cy - 9, $x + 21, $cy + 9)
        }
        'OPTIONS' {
            $graphics.DrawLine($pen, $x + 4, $cy - 7, $x + 32, $cy - 7)
            $graphics.DrawLine($pen, $x + 4, $cy, $x + 32, $cy)
            $graphics.DrawLine($pen, $x + 4, $cy + 7, $x + 32, $cy + 7)
        }
        default {
            $graphics.DrawRectangle($pen, $x, $cy - 14, $buttonWidth, 28)
            $buttonSize = $graphics.MeasureString($button, $font)
            $graphics.DrawString(
                $button,
                $font,
                $textBrush,
                $x + ($buttonWidth - $buttonSize.Width) / 2,
                $cy - $buttonSize.Height / 2)
        }
    }
    $actionX = $x + $buttonWidth + 9
    $graphics.DrawString($action, $font, $textBrush, $actionX, 1021)
    $width = [int]$graphics.MeasureString($action, $font).Width
    $pen.Dispose()
    return $actionX + $width + 27
}

try {
    Fill '#050913' 0 0 1920 1080
    Fill '#09101e' 0 0 1920 190
    Fill '#192c4e' 0 190 1920 2

    $titleFont = [Drawing.Font]::new($family, 48, [Drawing.FontStyle]::Regular, [Drawing.GraphicsUnit]::Pixel)
    $rowFont = [Drawing.Font]::new($family, 27, [Drawing.FontStyle]::Regular, [Drawing.GraphicsUnit]::Pixel)
    $footerFont = [Drawing.Font]::new($family, 22, [Drawing.FontStyle]::Regular, [Drawing.GraphicsUnit]::Pixel)
    $white = Brush '#f0f4ff'
    $muted = Brush '#9daac4'

    $graphics.DrawString('Library', $titleFont, $white, 58, 38)
    $graphics.DrawString('ALL MEDIA  |  0 ITEMS  |  SORT: SMART', $rowFont, $muted, 58, 119)

    Fill '#09101e' 42 214 1836 744
    $border = [Drawing.Pen]::new([Drawing.ColorTranslator]::FromHtml('#192a47'), 1)
    $graphics.DrawRectangle($border, 42, 214, 1836, 744)

    $emptyTitle = 'Your library is empty'
    $emptyHelp = 'Press Cross to choose a movie or TV-show folder.'
    $titleSize = $graphics.MeasureString($emptyTitle, $titleFont)
    $helpSize = $graphics.MeasureString($emptyHelp, $rowFont)
    $graphics.DrawString($emptyTitle, $titleFont, $white, 960 - $titleSize.Width / 2, 486)
    Fill '#1a4984' ([int](960 - ($helpSize.Width + 76) / 2)) 574 ([int]($helpSize.Width + 76)) 64
    $actionBorder = [Drawing.Pen]::new([Drawing.ColorTranslator]::FromHtml('#53a4ff'), 1)
    $graphics.DrawRectangle(
        $actionBorder,
        [int](960 - ($helpSize.Width + 76) / 2),
        574,
        [int]($helpSize.Width + 76),
        64)
    $graphics.DrawString($emptyHelp, $rowFont, $white, 960 - $helpSize.Width / 2, 587)

    Fill '#0a1221' 0 990 1920 90
    Fill '#1c3050' 0 990 1920 2
    $hintX = 48
    $hintX = Draw-Hint 'X' 'Add Media' $hintX $footerFont $muted
    $null = Draw-Hint 'OPTIONS' 'Menu' $hintX $footerFont $muted

    $target = [IO.Path]::GetFullPath($OutFile)
    $directory = Split-Path -Parent $target
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $bitmap.Save($target, [Drawing.Imaging.ImageFormat]::Png)
    Write-Output $target
}
finally {
    if ($border) { $border.Dispose() }
    if ($actionBorder) { $actionBorder.Dispose() }
    if ($white) { $white.Dispose() }
    if ($muted) { $muted.Dispose() }
    if ($titleFont) { $titleFont.Dispose() }
    if ($rowFont) { $rowFont.Dispose() }
    if ($footerFont) { $footerFont.Dispose() }
    $graphics.Dispose()
    $bitmap.Dispose()
    $fontCollection.Dispose()
}
