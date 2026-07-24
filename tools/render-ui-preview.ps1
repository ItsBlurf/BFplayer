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

function Draw-PlayMark([int]$cx, [int]$cy, [int]$size) {
    $half = [int]($size / 2)
    $points = [Drawing.Point[]]@(
        [Drawing.Point]::new($cx - $half, $cy - $half),
        [Drawing.Point]::new($cx + $half, $cy),
        [Drawing.Point]::new($cx - $half, $cy + $half)
    )
    $gold = Brush '#f4b22a'
    $graphics.FillPolygon($gold, $points)
    $gold.Dispose()
    Fill '#0d1e4d' ($cx - 9) ($cy - $half - [int]($size / 3)) 18 ([int]($size * 1.67))
    Fill '#c2e8ff' ($cx - 6) ($cy - $half - [int]($size / 3)) 12 ([int]($size * 1.5))
    Fill '#26b8ff' ($cx - 2) ($cy - $half - [int]($size / 3) + 3) 4 ([int]($size * 1.45))
    Fill '#4785ff' ($cx - 24) ($cy - $half - [int]($size / 4)) 48 7
    Fill '#e7f9ff' ($cx - 3) ($cy - 3) 7 7
}

try {
    Fill '#050913' 0 0 1920 1080
    Fill '#0c1426' 0 0 1920 202
    Fill '#192c4e' 0 202 1920 8
    Fill '#2f89ff' 0 202 1920 2
    Draw-PlayMark 82 92 54

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

    Draw-PlayMark 656 522 116
    $emptyTitle = 'Your library is empty'
    $emptyHelp = 'Press the touchpad to add a movie or TV-show folder.'
    $titleSize = $graphics.MeasureString($emptyTitle, $titleFont)
    $helpSize = $graphics.MeasureString($emptyHelp, $rowFont)
    $graphics.DrawString($emptyTitle, $titleFont, $white, 656 - $titleSize.Width / 2, 665)
    $graphics.DrawString($emptyHelp, $rowFont, $muted, 656 - $helpSize.Width / 2, 730)

    Draw-PlayMark 1586 534 88
    $artText = 'NO LOCAL ARTWORK'
    $artSize = $graphics.MeasureString($artText, $rowFont)
    $graphics.DrawString($artText, $rowFont, $muted, 1586 - $artSize.Width / 2, 897)

    Fill '#0a1221' 0 990 1920 90
    Fill '#1c3050' 0 990 1920 2
    $graphics.DrawString(
        'CROSS  PLAY     CIRCLE  QUEUE     D-PAD  CATEGORY     L3  FAVORITE     TOUCHPAD  ADD MEDIA     OPTIONS  EXIT',
        $footerFont,
        $muted,
        58,
        1021)

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
