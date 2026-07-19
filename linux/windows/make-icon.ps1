# Regenerate PocketTracker.ico from docs/images/logo-plain.png.
#
#     powershell -ExecutionPolicy Bypass -File linux/windows/make-icon.ps1
#
# The .ico is COMMITTED — this script is not part of any build, and CI never runs it. It exists so
# the icon is a derived artifact with a stated source rather than a binary someone once dropped in
# the tree, which is the same reason `licenses/THIRD-PARTY-NOTICES.md` is the source of truth for
# the notices instead of a folder of files.
#
# Why logo-plain.png and not the other three: an icon is judged at 16x16 in a taskbar, and that is
# the only one of them that survives it. logo-iso-transp is thin grey strokes on transparent with a
# wide margin (invisible on a light background, illegible when small); logo-dark is the device shot
# with "pocket TRACKER" set in type that turns to mush below ~48px; logo-plain is the PT mark in
# solid white blocks, full-bleed, on near-black. Chunky and high-contrast is what scales down.
#
# ⚠️ No ImageMagick, no Pillow, no conversion site. Python on this box needs elevation and a build
# asset should not depend on a tool the next person has to install, so the ICO container is written
# out by hand below. It is a 6-byte header, a 16-byte directory entry per size, and the images.

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$src  = Join-Path $repo 'docs\images\logo-plain.png'
$out  = Join-Path $PSScriptRoot 'PocketTracker.ico'

# 16..128 are written as DIBs and 256 as an embedded PNG. That split is the convention every icon
# tool follows: PNG entries are Vista+ only, and while every Windows this app targets reads them at
# any size, the small sizes are the ones passed through the most legacy shell code paths, so they
# stay in the format that has always worked. 256 is PNG because a 256x256 DIB is 256 KB of mostly
# black and the PNG is a few.
$sizes = @(16, 24, 32, 48, 64, 128, 256)

$source = New-Object System.Drawing.Bitmap($src)
Write-Output ("source: {0}  {1}x{2}  {3}" -f (Split-Path -Leaf $src), $source.Width, $source.Height, $source.PixelFormat)

# ─── Crop the dead margin before scaling ────────────────────────────────────────────────────────
# logo-plain.png is the PT mark centred in ~17% of empty background on every side. Downscaling the
# whole 1000px canvas to 16px spends a quarter of the icon's width on nothing and blurs the mark
# into a grey smear — this was looked at, at 8x magnification, not assumed: the 32px frame read
# fine and the 16px one did not. Cropping to the mark first buys back roughly a third of the linear
# resolution at every size, which is the difference between blocks and mush at 16px.
#
# The bounding box is found by LUMINANCE, not alpha: this source has an opaque near-black
# background (Format24bppRgb), so there is no alpha to test. Anything above the threshold is mark.
$minX = $source.Width; $minY = $source.Height; $maxX = -1; $maxY = -1
$rect = New-Object System.Drawing.Rectangle(0, 0, $source.Width, $source.Height)
$d    = $source.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                         [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$row  = New-Object byte[] ($source.Width * 4)
for ($y = 0; $y -lt $source.Height; $y++) {
    [System.Runtime.InteropServices.Marshal]::Copy([IntPtr]::Add($d.Scan0, $y * $d.Stride), $row, 0, $row.Length)
    for ($x = 0; $x -lt $source.Width; $x++) {
        # BGRA; the mark is white (~255) and the background near-black (~13), so any mid threshold
        # separates them and the exact value does not matter.
        if ($row[$x * 4 + 1] -gt 60) {
            if ($x -lt $minX) { $minX = $x }
            if ($x -gt $maxX) { $maxX = $x }
            if ($y -lt $minY) { $minY = $y }
            if ($y -gt $maxY) { $maxY = $y }
        }
    }
}
$source.UnlockBits($d)
if ($maxX -lt 0) { throw "found no mark in $src - every pixel is below the luminance threshold" }

# Square it off around the centre of the mark, then add a little padding back so the icon is not
# wall-to-wall — Windows' own icons sit in a small margin and one without looks oversized beside
# them.
$cx   = ($minX + $maxX) / 2.0
$cy   = ($minY + $maxY) / 2.0
$side = [Math]::Max($maxX - $minX, $maxY - $minY) * 1.16
$crop = New-Object System.Drawing.Rectangle(
            [int][Math]::Round($cx - $side / 2), [int][Math]::Round($cy - $side / 2),
            [int][Math]::Round($side), [int][Math]::Round($side))
Write-Output ("mark:   {0},{1} to {2},{3}   cropping to {4},{5} {6}x{7}" -f
              $minX, $minY, $maxX, $maxY, $crop.X, $crop.Y, $crop.Width, $crop.Height)

# ─── Render the cropped region once per size ────────────────────────────────────────────────────
$frames = @{}
foreach ($n in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap($n, $n, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g   = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode  = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode    = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.SmoothingMode      = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g.DrawImage($source, (New-Object System.Drawing.Rectangle(0, 0, $n, $n)), $crop.X, $crop.Y,
                 $crop.Width, $crop.Height, [System.Drawing.GraphicsUnit]::Pixel)
    $g.Dispose()
    $frames[$n] = $bmp
}
$source.Dispose()

# ─── Encode each size into the bytes the directory will point at ────────────────────────────────
function ConvertTo-IcoDib([System.Drawing.Bitmap]$bmp) {
    # A DIB icon entry is a BITMAPINFOHEADER whose biHeight is DOUBLED (the colour bitmap stacked on
    # a 1bpp AND mask), both stored bottom-up. The mask is all zeros: these are 32bpp images and
    # Windows composites them through the alpha channel, but the mask must still be PRESENT and
    # correctly padded or the icon reads as garbage below the halfway line.
    $n    = $bmp.Width
    $rect = New-Object System.Drawing.Rectangle(0, 0, $n, $n)
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                          [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $rowBytes = $n * 4
    $pixels   = New-Object byte[] ($rowBytes * $n)
    for ($y = 0; $y -lt $n; $y++) {
        $srcRow = [IntPtr]::Add($data.Scan0, $y * $data.Stride)
        [System.Runtime.InteropServices.Marshal]::Copy($srcRow, $pixels, $y * $rowBytes, $rowBytes)
    }
    $bmp.UnlockBits($data)

    $maskRow = [int][Math]::Ceiling($n / 32.0) * 4   # 1bpp, each row padded to 4 bytes
    $ms = New-Object System.IO.MemoryStream
    $w  = New-Object System.IO.BinaryWriter($ms)
    $w.Write([uint32]40)          # biSize
    $w.Write([int32]$n)           # biWidth
    $w.Write([int32]($n * 2))     # biHeight — colour + mask
    $w.Write([uint16]1)           # biPlanes
    $w.Write([uint16]32)          # biBitCount
    $w.Write([uint32]0)           # biCompression = BI_RGB
    $w.Write([uint32]($rowBytes * $n + $maskRow * $n))  # biSizeImage
    $w.Write([int32]0); $w.Write([int32]0)              # pels-per-metre
    $w.Write([uint32]0); $w.Write([uint32]0)            # biClrUsed / biClrImportant
    for ($y = $n - 1; $y -ge 0; $y--) { $w.Write($pixels, $y * $rowBytes, $rowBytes) }   # bottom-up
    $w.Write((New-Object byte[] ($maskRow * $n)))                                        # AND mask
    $w.Flush()
    # ⚠️ The leading comma is load-bearing. PowerShell UNROLLS an array returned from a function
    # into its elements, so a bare `return $ms.ToArray()` hands back several thousand loose bytes
    # that the caller recollects as an Object[] — which then binds to BinaryWriter.Write(char[])
    # instead of Write(byte[]) and silently writes the wrong bytes. The first cut of this script did
    # exactly that and produced a 3,841-byte .ico for ~105 KB of images; it was caught only because
    # the check at the bottom decodes the FILE.
    return ,$ms.ToArray()
}

$images = @()
foreach ($n in $sizes) {
    if ($n -eq 256) {
        $ms = New-Object System.IO.MemoryStream
        $frames[$n].Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $images += ,$ms.ToArray()
    } else {
        $images += ,(ConvertTo-IcoDib $frames[$n])
    }
}

# ─── The container ──────────────────────────────────────────────────────────────────────────────
$ms = New-Object System.IO.MemoryStream
$w  = New-Object System.IO.BinaryWriter($ms)
$w.Write([uint16]0)                # reserved
$w.Write([uint16]1)                # type 1 = icon
$w.Write([uint16]$sizes.Count)

$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $n = $sizes[$i]
    # 256 is written as 0 in a single byte, which is how the format says "256" and the reason the
    # dimension fields cannot express anything larger.
    $dim = if ($n -eq 256) { 0 } else { $n }
    $w.Write([byte]$dim)           # bWidth
    $w.Write([byte]$dim)           # bHeight
    $w.Write([byte]0)              # bColorCount — 0 for >8bpp
    $w.Write([byte]0)              # bReserved
    $w.Write([uint16]1)            # wPlanes
    $w.Write([uint16]32)           # wBitCount
    $w.Write([uint32]$images[$i].Length)
    $w.Write([uint32]$offset)
    $offset += $images[$i].Length
}
foreach ($img in $images) { $w.Write($img) }
$w.Flush()
[System.IO.File]::WriteAllBytes($out, $ms.ToArray())
foreach ($n in $sizes) { $frames[$n].Dispose() }

# ─── Verify the FILE, not the loop that wrote it ────────────────────────────────────────────────
# Everything above inspected variables in memory. This re-reads the bytes off disk and walks the
# directory, so a frame written at the wrong offset or with a wrong declared length shows up as a
# number here rather than as a blank taskbar button on someone else's machine. The last entry's end
# offset landing exactly on the file length is what proves the whole chain adds up.
#
# ⚠️ NOT `New-Object System.Drawing.Icon($out, 256, 256)`, which is what this check used first. It
# reported the largest frame as 128x128 and looked like a real defect in the 256 entry — but .NET
# Framework's Icon class predates PNG-compressed entries and silently skips them, while Windows
# itself (Vista+) reads them fine. The instrument was wrong, not the file. So the 256 frame is
# verified the way the claim is actually stated: pull its payload out and decode it as a PNG.
Write-Output ""
Write-Output ("wrote: {0}  ({1:N0} bytes)" -f $out, (Get-Item $out).Length)

$bytes  = [System.IO.File]::ReadAllBytes($out)
$count  = [BitConverter]::ToUInt16($bytes, 4)
# ⚠️ Walk a CURSOR through every entry rather than only checking where the last one ends. The first
# cut of this check kept just the final offset+length and compared that against the file size — and
# when it was driven red by corrupting the 16x16 entry's declared length, IT DID NOT FIRE, because
# a wrong length in any entry but the last moves nothing it was looking at. A check that can only
# see the last of seven frames is not a check on the directory. Every entry must start exactly
# where its predecessor ended, and the last must end exactly at EOF.
$cursor = 6 + 16 * $count
for ($i = 0; $i -lt $count; $i++) {
    $e   = 6 + 16 * $i
    $len = [BitConverter]::ToUInt32($bytes, $e + 8)
    $off = [BitConverter]::ToUInt32($bytes, $e + 12)
    # 0x0 in the directory means 256; a PNG payload starts with the 8-byte signature, a DIB with
    # its 40-byte biSize.
    $isPng = $bytes[$off] -eq 0x89 -and $bytes[$off+1] -eq 0x50
    $kind  = if ($isPng) { 'PNG' } else { 'DIB' }
    $dim   = if ($bytes[$e] -eq 0) { 256 } else { $bytes[$e] }
    Write-Output ("  {0,3}x{1,-3} {2}  {3,7:N0} bytes at {4,7:N0}" -f $dim, $dim, $kind, $len, $off)
    if ($off -ne $cursor) {
        throw "frame $i ($dim px) starts at $off but the previous frame ended at $cursor"
    }
    $cursor = $off + $len
}
if ($cursor -ne $bytes.Length) {
    throw "the frames end at $cursor but the file is $($bytes.Length) bytes - the directory does not add up"
}
$e   = 6 + 16 * ($count - 1)
$len = [BitConverter]::ToUInt32($bytes, $e + 8)
$off = [BitConverter]::ToUInt32($bytes, $e + 12)
$png = [System.Drawing.Image]::FromStream((New-Object System.IO.MemoryStream(,$bytes[$off..($off + $len - 1)])))
Write-Output ("  256 frame payload decodes as {0}x{1}" -f $png.Width, $png.Height)
if ($png.Width -ne 256) { throw "the 256 frame decoded as $($png.Width)px" }
$png.Dispose()
Write-Output ("ok: {0} frames, directory consistent with the file length" -f $count)
