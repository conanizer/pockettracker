# Build the Windows desktop package: build/windows/PocketTracker-<version>-windows-x64.zip
#
# Convergence plan A3. Run from the repo root, after building the shell:
#
#     cmake -S linux -B linux/build -DCMAKE_BUILD_TYPE=Release
#     cmake --build linux/build --config Release
#     powershell -ExecutionPolicy Bypass -File linux/build-windows.ps1
#
# This script PACKAGES, it does not build — unlike build-portmaster.sh, whose container owns its
# whole toolchain. Here the compiler lives behind vcvars64.bat in whatever Visual Studio the machine
# happens to have, and a packaging script that tried to find it would be guessing. It fails loudly
# if the exe is missing or older than the sources instead.
#
# ⚠️ Nothing here is filtered down to the word "error". Every check prints what it actually found,
# not a verdict — this project has been burned by a "PASS" printed by a check whose two inputs had
# both failed and compared equal.
#
# ⚠️ SDL2 IS INSIDE THE EXE, and that is the difference from the PortMaster package that matters
# most here. A handheld links the CFW's own libSDL2 and ships none; a Windows box has no system SDL
# at all, so linux/CMakeLists.txt falls through to FetchContent and links it statically. That makes
# this zip a binary distribution of SDL2 and puts its notice in the artifact — see the SDL2 section
# of licenses/THIRD-PARTY-NOTICES.md, which said the opposite until this package existed.

$ErrorActionPreference = 'Stop'
# Both, and they are not interchangeable: ZipFile/ZipFileExtensions come from the FileSystem
# assembly while ZipArchive/ZipArchiveMode come from System.IO.Compression itself.
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.IO.Compression

$repo  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'linux\build'
$out   = Join-Path $repo 'build\windows'
$stage = Join-Path $out  'stage'

# ─── 1/5  find what was built ───────────────────────────────────────────────────────────────────
Write-Output ""
Write-Output "############ 1/5  locate the build ############"

# Both layouts, because the generator decides: ninja/make put the exe in the build root, while the
# Visual Studio generators (the default on a bare windows-latest runner) put it in Release/.
$exe = @(
    (Join-Path $build 'pockettracker-sdl.exe'),
    (Join-Path $build 'Release\pockettracker-sdl.exe')
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $exe) {
    throw "no pockettracker-sdl.exe under $build - build the shell first (see the header of this file)"
}
Write-Output "exe: $exe"
Write-Output ("     {0:N0} bytes, built {1}" -f (Get-Item $exe).Length, (Get-Item $exe).LastWriteTime)

# ⚠️ STALENESS IS A HARD FAILURE, not a warning. Packaging an exe older than the code it claims to
# be is the single most expensive mistake available in this repo — six separate sessions have been
# burned by a stale artifact, and unlike those, this one would ship. Sources only (*.cpp/*.h/CMake),
# so editing a README does not trip it.
$newest = Get-ChildItem -Recurse -File `
              (Join-Path $repo 'linux'), (Join-Path $repo 'native') `
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

# The version, from the same place linux/CMakeLists.txt read it — see the comment there.
$gradle = Get-Content (Join-Path $repo 'app\build.gradle.kts') -Raw
if ($gradle -notmatch 'versionName[ \t]*=[ \t]*"([0-9]+\.[0-9]+\.[0-9]+)"') {
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
# ONE top-level folder, deliberately: a zip that unpacks eight loose files into someone's Downloads
# folder is a zip they will lose. And the exe is renamed on the way in — the build target is
# `pockettracker-sdl` for every platform, `PocketTracker.exe` is what a Windows user should see.
# build-portmaster.sh does the same thing (pockettracker-sdl -> pockettracker.aarch64); the build
# output name and the shipped name are allowed to differ, and the packaging script is the seam.
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
$app = Join-Path $stage 'PocketTracker'
New-Item -ItemType Directory -Force -Path (Join-Path $app 'licenses') | Out-Null

Copy-Item $exe (Join-Path $app 'PocketTracker.exe')
Copy-Item (Join-Path $PSScriptRoot 'windows\README.txt') (Join-Path $app 'README.txt')

# PocketTracker is GPL-3.0 and statically links its decoders, so their notices ship with the binary
# that contains them rather than only with the source tree that built it. A missing one is a licence
# breach in the artifact, which is the only thing a user ever receives.
$lic = Join-Path $app 'licenses'
Copy-Item (Join-Path $repo 'LICENSE')                                    (Join-Path $lic 'LICENSE')
Copy-Item (Join-Path $repo 'licenses\THIRD-PARTY-NOTICES.md')            $lic
Copy-Item (Join-Path $repo 'native\vendor\ogg\COPYING')                  (Join-Path $lic 'libogg-COPYING')
Copy-Item (Join-Path $repo 'native\vendor\opus\COPYING')                 (Join-Path $lic 'libopus-COPYING')
Copy-Item (Join-Path $repo 'native\vendor\opus\LICENSE_PLEASE_READ.txt') (Join-Path $lic 'libopus-LICENSE_PLEASE_READ.txt')

# ⚠️ SDL's licence comes out of the SOURCE THAT WAS ACTUALLY COMPILED, not from a copy kept in this
# repo. FetchContent put it in the build tree, so the licence that ships is the licence of the code
# that shipped, by construction — and if the SDL2 pin in linux/CMakeLists.txt ever moves, this file
# moves with it rather than quietly describing the old one.
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
# Latin-1 maps one byte to one char, so the file's bytes can be searched as text without a decoder
# rewriting anything.
$text   = [System.Text.Encoding]::GetEncoding(28591).GetString($bytes)

if ($bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { throw "not a PE image - no MZ header" }
$pe = [BitConverter]::ToInt32($bytes, 0x3C)
$machine = [BitConverter]::ToUInt16($bytes, $pe + 4)
if ($machine -ne 0x8664) { throw ("not x64 - PE machine type is 0x{0:X4}" -f $machine) }
Write-Output "PE image           : x86-64"

# ⚠️ IS THE SCAN EVEN WORKING? A check for the ABSENCE of something passes just as happily when the
# thing doing the looking is broken - a mistyped path, an empty read, an encoding that mangles the
# bytes - and it would then wave through exactly the defect it exists to catch. So prove the scan
# finds a string that must be there before trusting it not to find the ones that must not.
if ($text -notmatch 'KERNEL32\.dll') {
    throw "the import scan cannot even find KERNEL32.dll - the scan is broken, not the exe"
}
Write-Output "import scan        : working (KERNEL32.dll found)"

# --- the failure this whole package exists to prevent -------------------------------------------
# Before A3 the exe imported MSVCP140.dll, VCRUNTIME140.dll and VCRUNTIME140_1.dll. Those live in
# the Visual C++ redistributable, which a developer's machine has and a stranger's does not: they
# would double-click and get a Windows error dialog naming a DLL. It is invisible on every box that
# can build the thing, which is every box this shell had ever run on.
# linux/CMakeLists.txt's CMAKE_MSVC_RUNTIME_LIBRARY is the fix; this is the assertion.
$dynamicCrt = @('VCRUNTIME140.dll', 'VCRUNTIME140_1.dll', 'MSVCP140.dll', 'MSVCR120.dll') |
              Where-Object { $text -match [regex]::Escape($_) }
if ($dynamicCrt) {
    throw ("the exe imports the DYNAMIC VC runtime ($($dynamicCrt -join ', ')). A machine without " +
           "Visual Studio cannot run it. Check CMAKE_MSVC_RUNTIME_LIBRARY in linux/CMakeLists.txt.")
}
Write-Output "VC runtime         : static (no VCRUNTIME/MSVCP imports)"

# --- SDL2 must be IN the exe, not beside it -----------------------------------------------------
# The mirror image of build-portmaster.sh's check, which fails if SDL2 got statically bundled. Same
# question, opposite correct answer, because the two artifacts have opposite linkage models.
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
# ⚠️ Asserts THIS icon, not "an icon". The obvious check — ExtractAssociatedIcon returning
# non-null — passes on any exe Windows can find a default for, so it would go green on a build where
# the .rc was dropped entirely; "a check that passes on both is not a check". The linker embeds each
# RT_ICON's DIB bytes verbatim, so a distinctive slice out of the middle of the 128x128 frame is a
# discriminator only the right binary can satisfy.
#
# (It also has no System.Drawing dependency, which matters: that assembly is not loadable from
# PowerShell 7, and CI must be able to run this script under either interpreter.)
$icoBytes = [System.IO.File]::ReadAllBytes((Join-Path $PSScriptRoot 'windows\PocketTracker.ico'))
$e128     = 6 + 16 * 5                                        # the 128x128 directory entry
$off128   = [BitConverter]::ToUInt32($icoBytes, $e128 + 12)
$slice    = [System.Text.Encoding]::GetEncoding(28591).GetString($icoBytes, $off128 + 20040, 64)
if (-not $text.Contains($slice)) {
    throw ("the exe does not contain the pixels of linux/windows/PocketTracker.ico - the icon " +
           "resource is missing or is not ours (check the .rc and that enable_language(RC) ran)")
}
Write-Output "icon resource      : present and matches PocketTracker.ico (SDL takes the first RT_GROUP_ICON as the window icon)"

# --- no realtime-thread debug instrumentation in a shipped binary -------------------------------
if ($text -match 'KILDBG') {
    throw "built from the KIL investigation branch - it printf()s on the audio thread"
}
Write-Output "debug instr.       : none"

# --- every statically linked component must have a notice ---------------------------------------
# ⚠️ DERIVED FROM THE TREE, NOT FROM A LIST SOMEONE MUST REMEMBER TO UPDATE — same mechanism and
# same reasoning as build-portmaster.sh: the same commit that forgets the notice would forget the
# list entry, so `native/vendor/` IS the list.
#
# ⚠️ SDL2 IS THE ONE EXCEPTION AND HAS TO BE NAMED BY HAND. It is fetched at configure time and has
# never lived in native/vendor/, so the derived list cannot see it — and on THIS artifact, unlike
# the PortMaster one, it is compiled in. A guard that cannot see the component most specific to the
# package it guards is worth stating out loud rather than leaving to be discovered.
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

# ⚠️ THE ENTRY NAMES ARE BUILT BY HAND, with forward slashes, because BOTH of the obvious ways to
# do this get it wrong on Windows PowerShell 5.1. Compress-Archive is the known offender — but
# [ZipFile]::CreateFromDirectory writes backslashes too on .NET Framework 4.x (it uses
# Path.DirectorySeparatorChar; the fix only landed in .NET Core 3.0), and this script used it and
# said so in a comment until the read-back check at step 5 printed
# `PocketTracker\licenses\libogg-COPYING` and refused it.
#
# The zip spec (APPNOTE 4.4.17.1) requires forward slashes. Explorer forgives backslashes, which is
# exactly why this would have shipped: on the machine that builds the package it looks perfect, and
# on a Mac or a Linux box the whole tree arrives as single files with backslashes in their names.
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
# ⚠️ EVERYTHING ABOVE INSPECTED THE STAGING DIRECTORY, and the zip is what ships. A broken archive
# step could still have dropped any of it. Same discipline as build-portmaster.sh reading its
# launch script's bytes back out rather than trusting the copy that produced them.
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
                          'PocketTracker/licenses/SDL2-LICENSE.txt',
                          'PocketTracker/licenses/libogg-COPYING',
                          'PocketTracker/licenses/libopus-COPYING',
                          'PocketTracker/licenses/libopus-LICENSE_PLEASE_READ.txt')) {
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
