# Build the Windows desktop package: build/windows/PocketTracker-<version>-windows-x64.zip
#
# Run from the repo root, after building the shell:
#
#     cmake -S shell -B shell/build -DCMAKE_BUILD_TYPE=Release
#     cmake --build shell/build --config Release
#     powershell -ExecutionPolicy Bypass -File shell/build-windows.ps1
#
# It packages, it does not build: the compiler sits behind whatever Visual Studio the machine has. It
# fails if the exe is missing or older than the sources.
# ⚠️ Every check prints what it found, not just a verdict.
# ⚠️ SDL2 is inside the exe: Windows has no system SDL, so it is fetched and linked statically, and
# its notice ships in the zip.

$ErrorActionPreference = 'Stop'
# Both: ZipFile/ZipFileExtensions are in the FileSystem assembly, ZipArchive/ZipArchiveMode in the other.
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.IO.Compression

$repo  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'shell\build'
$out   = Join-Path $repo 'build\windows'
$stage = Join-Path $out  'stage'

# ─── 1/5  find what was built ───────────────────────────────────────────────────────────────────
Write-Output ""
Write-Output "############ 1/5  locate the build ############"

# Ninja puts the exe in the build root, the Visual Studio generators in Release/.
$exe = @(
    (Join-Path $build 'pockettracker-sdl.exe'),
    (Join-Path $build 'Release\pockettracker-sdl.exe')
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $exe) {
    throw "no pockettracker-sdl.exe under $build - build the shell first (see the header of this file)"
}
Write-Output "exe: $exe"
Write-Output ("     {0:N0} bytes, built {1}" -f (Get-Item $exe).Length, (Get-Item $exe).LastWriteTime)

# ⚠️ A stale exe is a hard failure: it would ship. Sources only, so a README edit does not trip it.
$newest = Get-ChildItem -Recurse -File `
              (Join-Path $repo 'shell'), (Join-Path $repo 'native') `
              -Include *.cpp, *.h, *.hpp, *.c, *.rc, *.rc.in, CMakeLists.txt |
          Where-Object { $_.FullName -notlike "*\build\*" } |
          Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($newest -and $newest.LastWriteTime -gt (Get-Item $exe).LastWriteTime) {
    Write-Output ""
    Write-Output "  exe   : $((Get-Item $exe).LastWriteTime)"
    Write-Output "  source: $($newest.LastWriteTime)  $($newest.FullName)"
    throw "the exe is OLDER than a source file - rebuild before packaging, or the zip ships code that is not this tree's"
}
Write-Output "not stale: no source newer than the exe"

# The version, from where shell/CMakeLists.txt reads it.
$gradle = Get-Content (Join-Path $repo 'app\build.gradle.kts') -Raw
if ($gradle -notmatch 'versionName[ \t]*=[ \t]*"([0-9]+\.[0-9]+\.[0-9]+(\.[0-9]+)?)"') {
    throw "could not find versionName in app/build.gradle.kts"
}
$version = $Matches[1]
Write-Output "version: $version (from app/build.gradle.kts)"

# ─── 2/5  stage ─────────────────────────────────────────────────────────────────────────────────
Write-Output ""
Write-Output "############ 2/5  stage the package ############"
#
#   PocketTracker-<version>-windows-x64.zip
#   `- PocketTracker/
#      |- PocketTracker.exe
#      |- README.txt
#      `- licenses/
#
# One top-level folder, so the zip does not scatter files into Downloads. The exe is renamed here.
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
$app = Join-Path $stage 'PocketTracker'
New-Item -ItemType Directory -Force -Path (Join-Path $app 'licenses') | Out-Null

Copy-Item $exe (Join-Path $app 'PocketTracker.exe')
Copy-Item (Join-Path $PSScriptRoot 'windows\README.txt') (Join-Path $app 'README.txt')

# The statically linked components' notices ship with the binary that contains them.
$lic = Join-Path $app 'licenses'
Copy-Item (Join-Path $repo 'LICENSE')                                    (Join-Path $lic 'LICENSE')
Copy-Item (Join-Path $repo 'docs\licenses\THIRD-PARTY-NOTICES.md')       $lic
Copy-Item (Join-Path $repo 'CREDITS.md')                                 (Join-Path $lic 'CREDITS.md')
Copy-Item (Join-Path $repo 'native\vendor\ogg\COPYING')                  (Join-Path $lic 'libogg-COPYING')
Copy-Item (Join-Path $repo 'native\vendor\opus\COPYING')                 (Join-Path $lic 'libopus-COPYING')
Copy-Item (Join-Path $repo 'native\vendor\opus\LICENSE_PLEASE_READ.txt') (Join-Path $lic 'libopus-LICENSE_PLEASE_READ.txt')

# The OFL texts ship though no font file does: THIRD-PARTY-NOTICES.md cites them by path, and the 5×5
# UI font compiled into the binary is adapted from QuinqueFive.
Copy-Item (Join-Path $repo 'docs\licenses\OFL-1.1-LiberationSans.txt')   (Join-Path $lic 'OFL-1.1-LiberationSans.txt')
Copy-Item (Join-Path $repo 'docs\licenses\OFL-1.1-QuinqueFive.txt')      (Join-Path $lic 'OFL-1.1-QuinqueFive.txt')

# ⚠️ SDL's licence from the source that was compiled (FetchContent's tree), so it follows the pin.
$sdlLicense = Join-Path $build '_deps\sdl2-src\LICENSE.txt'
if (-not (Test-Path $sdlLicense)) {
    throw ("SDL2's LICENSE.txt is not at $sdlLicense. This package links SDL2 STATICALLY into the " +
           "exe, so shipping without its notice is a licence breach - not a missing nicety. If SDL " +
           "came from somewhere other than FetchContent, copy that build's LICENSE.txt here.")
}
Copy-Item $sdlLicense (Join-Path $lic 'SDL2-LICENSE.txt')
Get-ChildItem $lic | ForEach-Object { Write-Output ("  {0,10:N0}  {1}" -f $_.Length, $_.Name) }

# ─── 3/5  verify the ARTIFACT, not the build log ────────────────────────────────────────────────
Write-Output ""
Write-Output "############ 3/5  verify the ARTIFACT (not the build log) ############"
$staged = Join-Path $app 'PocketTracker.exe'
$bytes  = [System.IO.File]::ReadAllBytes($staged)
# Latin-1: one byte, one char, so the bytes can be searched as text unchanged.
$text   = [System.Text.Encoding]::GetEncoding(28591).GetString($bytes)

if ($bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { throw "not a PE image - no MZ header" }
$pe = [BitConverter]::ToInt32($bytes, 0x3C)
$machine = [BitConverter]::ToUInt16($bytes, $pe + 4)
if ($machine -ne 0x8664) { throw ("not x64 - PE machine type is 0x{0:X4}" -f $machine) }
Write-Output "PE image           : x86-64"

# ⚠️ The control: an absence check passes on a broken scan too, so it must first find what is there.
if ($text -notmatch 'KERNEL32\.dll') {
    throw "the import scan cannot even find KERNEL32.dll - the scan is broken, not the exe"
}
Write-Output "import scan        : working (KERNEL32.dll found)"

# --- the static VC runtime -------------------------------------------------------------------
# A machine without Visual Studio lacks VCRUNTIME140.dll and friends, and nothing shows it on one that
# builds. CMAKE_MSVC_RUNTIME_LIBRARY in shell/CMakeLists.txt.
$dynamicCrt = @('VCRUNTIME140.dll', 'VCRUNTIME140_1.dll', 'MSVCP140.dll', 'MSVCR120.dll') |
              Where-Object { $text -match [regex]::Escape($_) }
if ($dynamicCrt) {
    throw ("the exe imports the DYNAMIC VC runtime ($($dynamicCrt -join ', ')). A machine without " +
           "Visual Studio cannot run it. Check CMAKE_MSVC_RUNTIME_LIBRARY in shell/CMakeLists.txt.")
}
Write-Output "VC runtime         : static (no VCRUNTIME/MSVCP imports)"

# --- SDL2 must be IN the exe, not beside it -----------------------------------------------------
# build-portmaster.sh asks the opposite: there SDL2 must be the device's.
if ($text -match 'SDL2\.dll') {
    throw "the exe imports SDL2.dll - nothing ships it, so it must be linked statically"
}
if (Get-ChildItem $app -Filter *.dll) {
    throw "there are DLLs in the package - this build is meant to be a single self-contained exe"
}
Write-Output "SDL2               : linked statically, no DLLs beside the exe"

# --- the resources: what Explorer and the window will show --------------------------------------
$v = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($staged)
Write-Output ("version resource   : {0} {1}" -f $v.ProductName, $v.FileVersion)
if ($v.FileVersion -ne $version) {
    throw ("the exe says version $($v.FileVersion) but app/build.gradle.kts says $version - this " +
           "exe was not built from this tree")
}
# ⚠️ This icon, not "an icon": Windows finds a default for any exe. The linker embeds each RT_ICON's
# bytes verbatim, so a slice of the 128x128 frame must appear in the exe.
$icoBytes = [System.IO.File]::ReadAllBytes((Join-Path $PSScriptRoot 'windows\PocketTracker.ico'))
$e128     = 6 + 16 * 5                                        # the 128x128 directory entry
$off128   = [BitConverter]::ToUInt32($icoBytes, $e128 + 12)
$slice    = [System.Text.Encoding]::GetEncoding(28591).GetString($icoBytes, $off128 + 20040, 64)
if (-not $text.Contains($slice)) {
    throw ("the exe does not contain the pixels of shell/windows/PocketTracker.ico - the icon " +
           "resource is missing or is not ours (check the .rc and that enable_language(RC) ran)")
}
Write-Output "icon resource      : present and matches PocketTracker.ico (SDL takes the first RT_GROUP_ICON as the window icon)"

# --- no realtime-thread debug instrumentation in a shipped binary -------------------------------
if ($text -match 'KILDBG') {
    throw "built from the KIL investigation branch - it printf()s on the audio thread"
}
Write-Output "debug instr.       : none"

# --- every statically linked component must have a notice ---------------------------------------
# ⚠️ `native/vendor/` is the list; a hand-kept one would be forgotten by the same commit.
# ⚠️ SDL2 is named by hand as well: this exe's SDL is FetchContent's, not native/vendor/SDL2 (the APK's).
$notices  = Get-Content (Join-Path $lic 'THIRD-PARTY-NOTICES.md') -Raw
$vendored = Get-ChildItem (Join-Path $repo 'native\vendor') -Directory | ForEach-Object { $_.Name }
$missing  = @()
Write-Output ""
Write-Output "licence notices:"
foreach ($c in @($vendored + @('kissfft', 'daisysp', 'soundpipe', 'SDL2'))) {
    if ($notices -match [regex]::Escape($c)) { Write-Output "  ok      $c" }
    else { Write-Output "  MISSING $c"; $missing += $c }
}
if ($missing) {
    throw ("statically linked but not named in THIRD-PARTY-NOTICES.md: $($missing -join ', '). " +
           "The artifact is the only thing a user receives. Document it there, then rebuild.")
}

# ─── 4/5  zip ───────────────────────────────────────────────────────────────────────────────────
Write-Output ""
Write-Output "############ 4/5  zip ############"
$zip = Join-Path $out "PocketTracker-$version-windows-x64.zip"

# ⚠️ Entry names by hand, with forward slashes: on PowerShell 5.1 both Compress-Archive and
# CreateFromDirectory write backslashes. Explorer forgives them; a Mac or Linux box unpacks one flat
# file per entry with backslashes in its name. The zip spec requires forward slashes.
$archive = [System.IO.Compression.ZipFile]::Open($zip, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($f in Get-ChildItem $stage -Recurse -File | Sort-Object FullName) {
        $name = $f.FullName.Substring($stage.Length).TrimStart('\').Replace('\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $archive, $f.FullName, $name, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally {
    $archive.Dispose()
}
Write-Output ("{0}  ({1:N0} bytes)" -f $zip, (Get-Item $zip).Length)

# ─── 5/5  read it back OUT of the zip ───────────────────────────────────────────────────────────
# ⚠️ Out of the zip, not the staging folder: the zip is what ships.
Write-Output ""
Write-Output "############ 5/5  read the package back out of the zip ############"
$archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
try {
    foreach ($e in $archive.Entries | Sort-Object FullName) {
        Write-Output ("  {0,10:N0}  {1}" -f $e.Length, $e.FullName)
        if ($e.FullName -match '\\') { throw "entry '$($e.FullName)' has a backslash separator" }
    }
    foreach ($needed in @('PocketTracker/PocketTracker.exe',
                          'PocketTracker/README.txt',
                          'PocketTracker/licenses/LICENSE',
                          'PocketTracker/licenses/THIRD-PARTY-NOTICES.md',
                          'PocketTracker/licenses/CREDITS.md',
                          'PocketTracker/licenses/SDL2-LICENSE.txt',
                          'PocketTracker/licenses/libogg-COPYING',
                          'PocketTracker/licenses/libopus-COPYING',
                          'PocketTracker/licenses/libopus-LICENSE_PLEASE_READ.txt',
                          'PocketTracker/licenses/OFL-1.1-LiberationSans.txt',
                          'PocketTracker/licenses/OFL-1.1-QuinqueFive.txt')) {
        $e = $archive.GetEntry($needed)
        if (-not $e)            { throw "$needed is MISSING from the zip" }
        if ($e.Length -lt 100)  { throw "$needed is only $($e.Length) bytes inside the zip" }
    }
    # The exe byte-for-byte, out of the archive: length alone would not catch a truncated deflate.
    $entry  = $archive.GetEntry('PocketTracker/PocketTracker.exe')
    $reader = New-Object System.IO.BinaryReader($entry.Open())
    $unzipped = $reader.ReadBytes([int]$entry.Length)
    $reader.Close()
    $md5 = [System.Security.Cryptography.MD5]::Create()
    $a = [BitConverter]::ToString($md5.ComputeHash($bytes)).Replace('-', '').ToLower()
    $b = [BitConverter]::ToString($md5.ComputeHash($unzipped)).Replace('-', '').ToLower()
    Write-Output ""
    Write-Output "  staged exe md5   : $a"
    Write-Output "  exe md5 from zip : $b"
    if ($a -ne $b) { throw "the exe inside the zip is not the exe that was verified above" }
} finally {
    $archive.Dispose()
}

Write-Output ""
Write-Output "ok: PocketTracker $version, windows-x64"
Write-Output $zip
