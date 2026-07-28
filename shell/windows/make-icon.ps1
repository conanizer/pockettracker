# Regenerate PocketTracker.ico from docs/images/logo-app.png.
#
#     powershell -ExecutionPolicy Bypass -File shell/windows/make-icon.ps1
#
# The .ico is COMMITTED — this script is not part of any build, and CI never runs it. It exists so
# the icon is a derived artifact with a stated source rather than a binary someone once dropped in
# the tree, which is the same reason `licenses/THIRD-PARTY-NOTICES.md` is the source of truth for
# the notices instead of a folder of files.
#
# The source is logo-app.png — the full-bleed device-shot mark (the rainbow grille + "pocket
# TRACKER") that is PocketTracker's icon across every platform. Cross-platform consistency is the
# reason it is used here rather than logo-plain.png: the app should show ONE icon whether it is on a
# phone's launcher, a handheld's game list, or a Windows taskbar. An earlier version of this script
# preferred logo-plain (the chunky PT blocks) on the argument that a detailed device shot turns to
# mush at 16px — that tradeoff is real and is accepted here in favour of the single recognisable
# mark. logo-app.png is a 256×256 export of the master icon (docs/internal/images/logo-500.ico,
# frame 0), extracted lossless; keep them in step if the master ever changes.
#
# ⚠️ No ImageMagick, no Pillow, no conversion site. Python on this box needs elevation and a build
# asset should not depend on a tool the next person has to install, so the ICO container is written
# out by hand below. It is a 6-byte header, a 16-byte directory entry per size, and the images.

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$src  = Join-Path $repo 'docs\images\logo-app.png'
$out  = Join-Path $PSScriptRoot 'PocketTracker.ico'

# 16..128 are written as DIBs and 256 as an embedded PNG. That split is the convention every icon
# tool follows: PNG entries are Vista+ only, and while every Windows this app targets reads them at
# any size, the small sizes are the ones passed through the most legacy shell code paths, so they
# stay in the format that has always worked. 256 is PNG because a 256x256 DIB is 256 KB of mostly
# black and the PNG is a few.
$sizes = @(16, 24, 32, 48, 64, 128, 256)

$source = New-Object System.Drawing.Bitmap($src)
Write-Output ("source: {0}  {1}x{2}  {3}" -f (Split-Path -Leaf $src), $source.Width, $source.Height, $source.PixelFormat)

# ─── Render the whole source once per size ──────────────────────────────────────────────────────
# No crop: logo-app.png is a full-bleed square — the device fills the frame edge to edge, so there
# is no dead margin to reclaim the way logo-plain (the PT mark floating in background) had. Scaling
# the entire image into each n×n frame is the whole job. HighQualityBicubic is what keeps the small
# sizes as legible as this detailed source allows.
$frames = @{}
foreach ($n in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap($n, $n, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g   = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode  = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode    = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.SmoothingMode      = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g.DrawImage($source, (New-Object System.Drawing.Rectangle(0, 0, $n, $n)),
                 0, 0, $source.Width, $source.Height, [System.Drawing.GraphicsUnit]::Pixel)
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
