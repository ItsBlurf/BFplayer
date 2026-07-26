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

    $border = [Drawing.Pen]::new([Drawing.ColorTranslator]::FromHtml('#192a47'), 1)

    Fill '#0a1221' 0 990 1920 90
    Fill '#1c3050' 0 990 1920 2
    $hintX = 48
    if ($View -eq 'AddMedia') {
        $graphics.DrawString('Add Media Source', $titleFont, $white, 58, 38)
        $graphics.DrawString('/mnt  |  2 ITEMS', $rowFont, $muted, 58, 119)

        Fill '#09101e' 42 214 1228 744
        Fill '#0b1425' 1310 214 552 744
        $graphics.DrawRectangle($border, 42, 214, 1228, 744)
        $graphics.DrawRectangle($border, 1310, 214, 552, 744)

        Fill '#1a4984' 56 226 1200 60
        Fill '#f4b22a' 56 226 7 60
        $graphics.DrawString('FOLDER   usb0', $rowFont, $white, 84, 241)
        Fill '#0d192c' 56 292 1200 60
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

        Fill '#09101e' 42 214 1836 744
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
