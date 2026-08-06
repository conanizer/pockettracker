# make-image-fixtures.ps1 — provenance AND regenerator for the D2 PNG decode fixtures.
#
# The sibling of golden/make-golden-media.cpp: it both documents how these files were made and remakes
# them. tools/ptdecode (ctest d-image-decode) decodes them and asserts every pixel against the SAME
# formulas below, so this is the independent oracle the decoder is checked against.
#
# ⚠️ It uses GDI+ (System.Drawing) ON PURPOSE — an encoder wholly independent of BOTH stb_image (the
# reader under test) AND ptshot's hand-rolled PNG writer. GDI+ writes real zlib-compressed,
# adaptively-filtered PNGs, exercising inflate + un-filter paths a stored/filter-0 writer cannot
# produce, so no "two things corrupted the same way" is possible. PNG stores STRAIGHT (non-
# premultiplied) alpha and stb_image applies no gamma on the 8-bit path, so a decoded pixel equals
# exactly what SetPixel drew — which is why the ground truth is simply the formulas here.
#
# Windows-only (GDI+). It does NOT run in CI and does not need to: the three .png files are COMMITTED
# and decoded on every platform. Run it only to regenerate them, and update tools/ptdecode/main.cpp's
# expected values in the same commit if you change a formula.
#
#   powershell -ExecutionPolicy Bypass -File testdata/images/make-image-fixtures.ps1

Add-Type -AssemblyName System.Drawing

$outDir = $PSScriptRoot

function Save-Png($bmp, $name) {
    $path = Join-Path $outDir $name
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    # Read IHDR back independently of stb_image: width/height (big-endian), bit depth, colour type.
    $b  = [System.IO.File]::ReadAllBytes($path)
    $w  = ($b[16] -shl 24) -bor ($b[17] -shl 16) -bor ($b[18] -shl 8) -bor $b[19]
    $h  = ($b[20] -shl 24) -bor ($b[21] -shl 16) -bor ($b[22] -shl 8) -bor $b[23]
    "{0,-18} {1}x{2}  bitdepth={3} colortype={4}  ({5} bytes)" -f $name, $w, $h, $b[24], $b[25], $b.Length
}

# ── rgb_3x2.png : 24bpp -> PNG colour type 2 (RGB, no alpha). Geometry + channel order. ───────────
# R=20+30x+3y  G=50+40y+5x  B=210-25x-15y   (x in 0..2, y in 0..1; all in 0..255, all 6 distinct)
$rgb = New-Object System.Drawing.Bitmap 3, 2, ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
for ($y = 0; $y -lt 2; $y++) {
  for ($x = 0; $x -lt 3; $x++) {
    $rgb.SetPixel($x, $y, [System.Drawing.Color]::FromArgb(255, 20 + 30*$x + 3*$y, 50 + 40*$y + 5*$x, 210 - 25*$x - 15*$y))
  }
}
Save-Png $rgb "rgb_3x2.png"

# ── rgba_2x2.png : 32bpp -> PNG colour type 6 (RGBA). The alpha path, alpha in {255,170,85,0}. ────
$rgba = New-Object System.Drawing.Bitmap 2, 2, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$rgba.SetPixel(0, 0, [System.Drawing.Color]::FromArgb(255, 200, 10, 20))
$rgba.SetPixel(1, 0, [System.Drawing.Color]::FromArgb(170, 30, 180, 40))
$rgba.SetPixel(0, 1, [System.Drawing.Color]::FromArgb(85,  50, 60, 200))
$rgba.SetPixel(1, 1, [System.Drawing.Color]::FromArgb(0,   70, 80, 90))
Save-Png $rgba "rgba_2x2.png"

# ── gradient_16x16.png : 32bpp, pixel(x,y)=ARGB(255, x*16, y*16, (x+y)*8). 16 rows -> real adaptive
#    filtering + zlib compression, the inflate + un-filter paths. ──────────────────────────────────
$grad = New-Object System.Drawing.Bitmap 16, 16, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
for ($y = 0; $y -lt 16; $y++) {
  for ($x = 0; $x -lt 16; $x++) {
    $grad.SetPixel($x, $y, [System.Drawing.Color]::FromArgb(255, $x*16, $y*16, ($x+$y)*8))
  }
}
Save-Png $grad "gradient_16x16.png"
