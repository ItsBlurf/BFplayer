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

function Draw-Logo([int]$cx, [int]$cy, [int]$size) {
    $graphics.DrawImage(
        $logo,
        [Drawing.Rectangle]::new(
            $cx - [int]($size / 2),
            $cy - [int]($size / 2),
            $size,
            $size))
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
    $logo = [Drawing.Image]::FromFile((Join-Path $root 'assets\icon0.png'))
    Fill '#050913' 0 0 1920 1080
    Fill '#0c1426' 0 0 1920 202
    Fill '#192c4e' 0 202 1920 8
    Fill '#2f89ff' 0 202 1920 2
    Draw-Logo 82 92 112

    $titleFont = [Drawing.Font]::new($family, 48, [Drawing.FontStyle]::Regular, [Drawing.GraphicsUnit]::Pixel)
    $rowFont = [Drawing.Font]::new($family, 27, [Drawing.FontStyle]::Regular, [Drawing.GraphicsUnit]::Pixel)
    $footerFont = [Drawing.Font]::new($family, 22, [Drawing.FontStyle]::Regular, [Drawing.GraphicsUnit]::Pixel)
    $white = Brush '#f0f4ff'
    $muted = Brush '#9daac4'

    $graphics.DrawString('PS5 Media Center', $titleFont, $white, 146, 38)
    Fill '#14223d' 146 112 575 46
    Fill '#f4b22a' 146 112 5 46
    $graphics.DrawString('ALL MEDIA  |  0 ITEMS  |  SORT: SMART', $rowFont, $muted, 166, 119)

    Fill '#09101e' 42 214 1228 744
    Fill '#0b1425' 1310 214 552 744
    $border = [Drawing.Pen]::new([Drawing.ColorTranslator]::FromHtml('#192a47'), 1)
    $graphics.DrawRectangle($border, 42, 214, 1228, 744)
    $graphics.DrawRectangle($border, 1310, 214, 552, 744)
    Fill '#0f1c33' 1344 248 484 602
    $graphics.DrawRectangle($border, 1344, 248, 484, 602)

    Draw-Logo 656 522 174
    $emptyTitle = 'Your library is empty'
    $emptyHelp = 'Press the touchpad to add a movie or TV-show folder.'
    $titleSize = $graphics.MeasureString($emptyTitle, $titleFont)
    $helpSize = $graphics.MeasureString($emptyHelp, $rowFont)
    $graphics.DrawString($emptyTitle, $titleFont, $white, 656 - $titleSize.Width / 2, 665)
    $graphics.DrawString($emptyHelp, $rowFont, $muted, 656 - $helpSize.Width / 2, 730)

    Draw-Logo 1586 534 150
    $artText = 'NO LOCAL ARTWORK'
    $artSize = $graphics.MeasureString($artText, $rowFont)
    $graphics.DrawString($artText, $rowFont, $muted, 1586 - $artSize.Width / 2, 897)

    Fill '#0a1221' 0 990 1920 90
    Fill '#1c3050' 0 990 1920 2
    $hintX = 48
    $hintX = Draw-Hint 'X' 'Play' $hintX $footerFont $muted
    $hintX = Draw-Hint 'O' 'Queue' $hintX $footerFont $muted
    $hintX = Draw-Hint 'DPAD' 'Category' $hintX $footerFont $muted
    $hintX = Draw-Hint 'L3' 'Favorite' $hintX $footerFont $muted
    $hintX = Draw-Hint 'TOUCH' 'Add Media' $hintX $footerFont $muted
    $hintX = Draw-Hint 'R3' 'Sort' $hintX $footerFont $muted
    $hintX = Draw-Hint 'SQUARE' 'Rescan' $hintX $footerFont $muted
    $null = Draw-Hint 'OPTIONS' 'Exit' $hintX $footerFont $muted

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
    if ($logo) { $logo.Dispose() }
    $graphics.Dispose()
    $bitmap.Dispose()
    $fontCollection.Dispose()
}
