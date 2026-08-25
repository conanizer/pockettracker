# Regenerate shell/miyoo/icon.png — the OnionOS app-shelf icon.
#
# NO BUILD RUNS THIS, the same way shell/windows/make-icon.ps1 is not run by a build: the PNG is
# committed, because build-miyoo.sh runs under WSL/Linux where there is no image tooling to rely on,
# and an icon that regenerates itself on every build is a diff nobody asked for.
#
#     pwsh -File shell/miyoo/make-icon.ps1
#
# 74x74 is Onion's own size, read off its shipped icons (static/build/Icons/Default/app/*.png), not
# guessed. The source is the SAME artwork the Android launcher uses, so a user who has both sees one
# app rather than two.

Add-Type -AssemblyName System.Drawing

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$src  = Join-Path $here "..\..\docs\images\logo-app.png"
$dst  = Join-Path $here "icon.png"

$in  = [System.Drawing.Image]::FromFile((Resolve-Path $src))
$out = New-Object System.Drawing.Bitmap 74, 74, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g   = [System.Drawing.Graphics]::FromImage($out)

# HighQualityBicubic on a 256 -> 74 reduction: the logo is a photographic-looking render with fine
# louvre lines across the top, and nearest-neighbour turns those into moire.
$g.InterpolationMode  = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.PixelOffsetMode    = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
$g.SmoothingMode      = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
$g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
$g.DrawImage($in, (New-Object System.Drawing.Rectangle 0, 0, 74, 74))

$out.Save($dst, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $out.Dispose(); $in.Dispose()

Write-Host "wrote $dst  ($((Get-Item $dst).Length) bytes)"
