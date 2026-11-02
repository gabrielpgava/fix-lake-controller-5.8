#Requires -Version 5.1
[CmdletBinding()]
param([string]$OutputPath)
$ErrorActionPreference = 'Stop'
if (!$OutputPath) { $OutputPath = Join-Path (Split-Path $PSScriptRoot -Parent) 'src\LakeWindowedPatch.ico' }
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
Add-Type -AssemblyName System.Drawing

function New-RoundedPath([float]$X, [float]$Y, [float]$Width, [float]$Height, [float]$Radius) {
    $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $diameter = $Radius * 2
    $path.AddArc($X, $Y, $diameter, $diameter, 180, 90)
    $path.AddArc($X + $Width - $diameter, $Y, $diameter, $diameter, 270, 90)
    $path.AddArc($X + $Width - $diameter, $Y + $Height - $diameter, $diameter, $diameter, 0, 90)
    $path.AddArc($X, $Y + $Height - $diameter, $diameter, $diameter, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-IconPng([int]$Size) {
    $bitmap = [System.Drawing.Bitmap]::new($Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $graphics.ScaleTransform($Size / 256.0, $Size / 256.0)
    $graphics.Clear([System.Drawing.Color]::Transparent)

    $tile = New-RoundedPath 9 9 238 238 48
    $graphics.SetClip($tile)
    $background = [System.Drawing.Drawing2D.LinearGradientBrush]::new(
        [System.Drawing.RectangleF]::new(9, 9, 238, 238),
        [System.Drawing.Color]::FromArgb(255, 10, 28, 52),
        [System.Drawing.Color]::FromArgb(255, 23, 77, 111),
        [System.Drawing.Drawing2D.LinearGradientMode]::ForwardDiagonal)
    $graphics.FillPath($background, $tile)

    $titleFont = [System.Drawing.Font]::new('Segoe UI', 46, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $subtitleFont = [System.Drawing.Font]::new('Segoe UI', 15, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $white = [System.Drawing.Brushes]::White
    $center = [System.Drawing.StringFormat]::new()
    $center.Alignment = [System.Drawing.StringAlignment]::Center
    $center.LineAlignment = [System.Drawing.StringAlignment]::Center
    $graphics.DrawString('LAKE', $titleFont, $white, [System.Drawing.RectangleF]::new(20, 28, 216, 55), $center)
    $subtitleBrush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(220, 214, 236, 248))
    $graphics.DrawString('CONTROLLER', $subtitleFont, $subtitleBrush,
        [System.Drawing.RectangleF]::new(24, 83, 208, 25), $center)

    $railPen = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(190, 161, 208, 230), 7)
    $railPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $railPen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $knobBrushes = @(
        [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 107, 211, 255)),
        [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 255, 255, 255)),
        [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 107, 211, 255))
    )
    foreach ($x in @(83, 128, 173)) { $graphics.DrawLine($railPen, $x, 126, $x, 177) }
    foreach ($knob in @(@{ X = 83; Y = 143; Brush = $knobBrushes[0] },
            @{ X = 128; Y = 163; Brush = $knobBrushes[1] }, @{ X = 173; Y = 136; Brush = $knobBrushes[2] })) {
        $graphics.FillEllipse($knob.Brush, $knob.X - 8, $knob.Y - 8, 16, 16)
        $graphics.DrawEllipse([System.Drawing.Pens]::White, $knob.X - 8, $knob.Y - 8, 16, 16)
    }

    $graphics.TranslateTransform(128, 215)
    $graphics.RotateTransform(-12)
    $badgeBackground = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 205, 36, 44))
    $graphics.FillRectangle($badgeBackground, -180, -30, 360, 60)
    $badgeFont = [System.Drawing.Font]::new('Arial', 31, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $badgeBrush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::White)
    $graphics.DrawString('PATCHED', $badgeFont, $badgeBrush, [System.Drawing.RectangleF]::new(-145, -26, 290, 52), $center)
    $graphics.ResetTransform()
    $graphics.ResetClip()
    $outline = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(230, 116, 199, 240), 5)
    $graphics.DrawPath($outline, $tile)

    $stream = [System.IO.MemoryStream]::new()
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $bytes = $stream.ToArray()
    $stream.Dispose()
    $graphics.Dispose()
    $bitmap.Dispose()
    $tile.Dispose()
    $background.Dispose()
    $titleFont.Dispose()
    $subtitleFont.Dispose()
    $subtitleBrush.Dispose()
    $center.Dispose()
    $railPen.Dispose()
    foreach ($brush in $knobBrushes) { $brush.Dispose() }
    $badgeFont.Dispose()
    $badgeBrush.Dispose()
    $badgeBackground.Dispose()
    $outline.Dispose()
    return ,$bytes
}

$sizes = @(16, 24, 32, 48, 64, 128, 256)
$images = foreach ($size in $sizes) { [pscustomobject]@{ Size = $size; Bytes = (New-IconPng $size) } }
$directory = Split-Path $OutputPath -Parent
[System.IO.Directory]::CreateDirectory($directory) | Out-Null
$temporary = $OutputPath + '.' + [guid]::NewGuid().ToString('N') + '.tmp'
$stream = [System.IO.File]::Open($temporary, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
try {
    $writer = [System.IO.BinaryWriter]::new($stream)
    $writer.Write([UInt16]0)
    $writer.Write([UInt16]1)
    $writer.Write([UInt16]$images.Count)
    $offset = 6 + 16 * $images.Count
    foreach ($image in $images) {
        $dimension = if ($image.Size -eq 256) { 0 } else { [byte]$image.Size }
        $writer.Write([byte]$dimension)
        $writer.Write([byte]$dimension)
        $writer.Write([byte]0)
        $writer.Write([byte]0)
        $writer.Write([UInt16]1)
        $writer.Write([UInt16]32)
        $writer.Write([UInt32]$image.Bytes.Length)
        $writer.Write([UInt32]$offset)
        $offset += $image.Bytes.Length
    }
    foreach ($image in $images) { $writer.Write([byte[]]$image.Bytes) }
    $writer.Flush()
    $writer.Dispose()
} finally { $stream.Dispose() }
if (Test-Path -LiteralPath $OutputPath) { [System.IO.File]::Replace($temporary, $OutputPath, [NullString]::Value) }
else { [System.IO.File]::Move($temporary, $OutputPath) }
Write-Host "Generated PATCHED Lake Controller icon: $OutputPath"
