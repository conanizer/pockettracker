# Regenerate shell/miyoo/boxart.png — the image Onion shows beside PocketTracker in its ports list.
#
# NO BUILD RUNS THIS, the same way shell/windows/make-icon.ps1 is not run by a build: the PNG is
# committed, because build-miyoo.sh runs under WSL/Linux where there is no image tooling to rely on,
# and artwork that regenerates itself on every build is a diff nobody asked for.
#
#     pwsh -File shell/miyoo/make-boxart.ps1
#
# 256x360 is the size of Onion's own port artwork, read off the images its Ports Collection ships
# (static/packages/.../Roms/PORTS/Imgs/*.png), not guessed. It is portrait because the list draws it
# as a game's box; the logo is square, so it sits in the upper part with the name beneath it.
#
# ⚠️ THE FILE NAME MUST MATCH THE SHORTCUT'S. Onion pairs Roms/PORTS/Imgs/<name>.png with
# Roms/PORTS/Shortcuts/<name>.port by base name alone, and a mismatch is simply no picture —
# build-miyoo.sh stages this file under the shortcut's name rather than under this one.

Add-Type -AssemblyName System.Drawing

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$src  = Join-Path $here "..\..\docs\images\logo-app.png"
$dst  = Join-Path $here "boxart.png"

$W = 256
$H = 360

$in  = [System.Drawing.Image]::FromFile((Resolve-Path $src))
$out = New-Object System.Drawing.Bitmap $W, $H, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g   = [System.Drawing.Graphics]::FromImage($out)

# HighQualityBicubic: the logo is a photographic-looking render with fine louvre lines across the
# top, and nearest-neighbour turns those into moire when it is rescaled.
$g.InterpolationMode  = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.PixelOffsetMode    = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
$g.SmoothingMode      = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
$g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality

# Onion draws this over its own theme background, and a transparent PNG there reads as a hole rather
# than as a floating logo. The fill is the app's own near-black.
$bg = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 18, 18, 22))
$g.FillRectangle($bg, 0, 0, $W, $H)

# ⚠️ NO CAPTION UNDER IT. The artwork already carries the wordmark, and a name printed beneath
# it reads as the app being called "PocketTracker PocketTracker" — Onion prints the shortcut
# name under the box itself.
$logo = $W
$g.DrawImage($in, (New-Object System.Drawing.Rectangle 0, ([int](($H - $logo) / 2)), $logo, $logo))

$out.Save($dst, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $out.Dispose(); $in.Dispose()

Write-Host "wrote $dst  ($((Get-Item $dst).Length) bytes)"
