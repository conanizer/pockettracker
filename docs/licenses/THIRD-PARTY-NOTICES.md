# Third-party notices

PocketTracker is distributed under **GPL-3.0-or-later** (see `LICENSE` at the repo root).
It statically links the components below, so **their notices travel with the binary**, not merely
with the source tree that built it. A user who receives only the artifact must still receive these.

This file is the single source of truth for the notices and is shipped verbatim in:

- the **PortMaster zip** → `pockettracker/licenses/THIRD-PARTY-NOTICES.md` (`shell/build-portmaster.sh`)
- the **Windows zip** → `licenses/THIRD-PARTY-NOTICES.md` (`shell/build-windows.ps1`)
- the **Linux tarball** → `licenses/THIRD-PARTY-NOTICES.md` (`shell/build-linux.sh`)
- the **APK** → `assets/licenses/THIRD-PARTY-NOTICES.md` (`app/build.gradle.kts`, `stageLicenseAssets`)
- the **repo** → `licenses/THIRD-PARTY-NOTICES.md`

⚠️ **Scope: what a recipient of a BINARY must receive** — which is a wider set than "what is
statically linked into the engine", and the difference is where the gaps kept being found. Most of
this file is the engine, i.e. what ships in *both* the APK and the Linux port. Three entries are not:

| Not engine-compiled | Why it is here anyway | Where it ships |
|---|---|---|
| Linux Biolinum (font asset) | The OFL requires its licence to accompany the font | wherever `assets/fonts/` travel |
| Oboe, the AndroidX libraries | Apache-2.0 §4(a) requires supplying a *copy of the License* | the APK only |

**Naming a licence is not supplying one.** `CREDITS.md` attributes everything the project builds on
and is the right place for "what is this and who wrote it", but a licence whose terms demand that the
text accompany the binary cannot be discharged by a citation — so the text lives here, in the file
that ships in every artifact. `CREDITS.md` ships beside it for the attribution half.

⚠️ **When you vendor a new library, add it here in the same commit.** The build asserts this file is
present in the artifact, but no automated check can know a component was *added* — that part is a
habit, not a guard.

---

## KissFFT — BSD-3-Clause

Used for: FFT behind the spectrum analyzer and the transient detector (`native/kissfft/`).

> ⚠️ **The vendored copy of KissFFT arrived with its notice stripped** — no copyright line existed in
> any of the five files. BSD-3-Clause requires the notice be retained in **source** redistributions
> and reproduced in **binary** ones, so both the header banner (restored in `native/kissfft/*`) and
> this section are obligations, not courtesies. Copyright line and SPDX identifier taken from
> upstream `COPYING` (github.com/mborgerding/kissfft).

```
Copyright (c) 2003-2010 Mark Borgerding. All rights reserved.

SPDX-License-Identifier: BSD-3-Clause

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

---

## DaisySP — MIT **and** LGPL-2.1 (mixed; see the split below)

Used for: SVF filter, overdrive, decimator/bitcrush, limiter, compressor, crossfade, balance,
ReverbSc (`native/effects/primitives/daisysp/`).

⚠️ **DaisySP is not uniformly MIT, and the files PocketTracker compiles land on both sides.**
Three of the eight are LGPL-2.1 because they descend from Csound and Faust/GRAME rather than from
Electrosmith's own code. Verified by reading the banner of each compiled file:

| File | Licence | Copyright |
|---|---|---|
| `svf` | MIT | (c) 2020 Electrosmith, Corp |
| `overdrive` | MIT | (c) 2020 Electrosmith, Corp, Emilie Gillet |
| `decimator` | MIT | (c) 2020 Electrosmith, Corp |
| `limiter` | MIT | (c) 2020 Electrosmith, Corp, Emilie Gillet |
| `crossfade` | MIT | (c) 2020 Electrosmith, Corp, Paul Batchelor |
| **`compressor`** | **LGPL-2.1** | (c) 2023 Electrosmith, Corp, GRAME, Centre National de Creation Musicale |
| **`balance`** | **LGPL-2.1** | (c) 2023 Electrosmith, Corp, Barry Vercoe, john ffitch, Gabriel Maldonado |
| **`reverbsc`** | **LGPL-2.1** | (c) 2023 Electrosmith, Corp, Sean Costello, Istvan Varga, Paul Batchelor |

**On the LGPL-2.1 three:** PocketTracker as a whole is GPL-3.0-or-later. LGPL-2.1 **§3** expressly
permits applying "the ordinary GNU General Public License" version 2 "or any later version" to a
given copy of the library, so these three are distributed here under **GPL-3.0-or-later**, and the
`LICENSE` text shipped beside this file is their governing text. There is no compatibility problem,
but the attribution above is required.

The five MIT files are covered by the MIT text below.

```
MIT License

Copyright (c) Electrosmith, Corp and the contributors named per-file above

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## TinySoundFont (tsf) — MIT

Used for: SF2 / SoundFont2 synthesis, with a per-channel rendering fork (`native/vendor/tsf/`).
Notice as carried in `tsf.h`.

```
Copyright (C) 2017-2025 Bernhard Schelling
Based on SFZero, Copyright (C) 2012 Steve Folta (https://github.com/stevefolta/SFZero)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## Soundpipe (pareq) — MIT

Used for: the parametric-EQ biquad (`native/effects/soundpipe/pareq.c`), a stub extracted from
Soundpipe by Paul Batchelor (https://github.com/PaulBatchelor/Soundpipe). MIT text as above,
copyright Paul Batchelor.

---

## skoomaDust — GPL-3.0-or-later (includes APComp, BSD-3-Clause)

Used for: the lo-fi DUST effect chain (`native/effects/modules/dust-chain.{h,cpp}`), contributed by
[@skoomabwoy](https://github.com/skoomabwoy/skoomaDust). Same licence as PocketTracker itself
(GPL-3.0-or-later) — `LICENSE` is the governing text.

It embeds the **APComp** FET compressor by **Alain Paul / AP Mastering**, under **BSD-3-Clause**;
the copyright is preserved in `dust-chain.cpp`. BSD-3-Clause text as in the KissFFT section above,
substituting that copyright holder.

---

## dr_libs — dr_mp3 / dr_flac — public domain **or** MIT-0 (dual, at your option)

Used for: native MP3 and FLAC decoding (`native/vendor/dr_mp3/`, `native/vendor/dr_flac/`).
By David Reid (mackron). Both files carry the full dual-licence statement at the end of the header;
PocketTracker exercises no option and redistributes them unchanged.

---

## stb_vorbis — public domain **or** MIT (dual, at your option)

Used for: native OGG Vorbis decoding (`native/vendor/stb_vorbis/stb_vorbis.c`).
Copyright (c) 2017 Sean Barrett. The full dual-licence statement is at the end of that file.

---

## FAAD2 (libfaad) — GPL-2.0-or-later

Used for: native AAC decoding of ISO-BMFF container samples — `.m4a` / `.mp4` / `.m4b` / `.mov` /
`.3gp` (`native/vendor/faad2/`, upstream https://github.com/knik0/faad2, version 2.11.2; see
`native/vendor/faad2/PT-VENDORING.md`). Copyright © the FAAD2 authors (see
`native/vendor/faad2/AUTHORS` upstream); the governing text is `native/vendor/faad2/COPYING`.

**GPL-2.0-or-later**, and that is a deliberate choice, not an accident: it is **why FAAD2 was picked
over fdk-aac**, whose licence the FSF considers GPL-incompatible. GPL-2.0-**or-later** may be used
under GPL-3.0, so it is compatible with PocketTracker's own GPL-3.0 licence (`LICENSE`). FAAD2 is
**statically linked into the `pockettracker` engine library**, so it ships in **every** artifact — the
APK, the PortMaster zip and the Windows zip alike. As with all of
PocketTracker, complete corresponding source is available under the project's GPL-3.0 terms.
`COPYING` additionally states that non-GPL use requires a separate commercial licence from the
authors; PocketTracker's use is GPL, so that clause does not apply here.

---

## minimp4 — CC0-1.0 / public domain

Used for: demuxing the ISO-BMFF container (the box parsing that feeds FAAD2 above) —
`native/vendor/minimp4/minimp4.h`, a single-header library by lieff (upstream
https://github.com/lieff/minimp4; see `native/vendor/minimp4/PT-VENDORING.md`). Dedicated to the
public domain under CC0-1.0; the full dedication is in the header comment. Only the demuxer is used
(the muxer half is dropped by the linker). Statically linked into the `pockettracker` engine library,
so it ships in every artifact; CC0 carries no reproduce-in-binary obligation, but the notice is
recorded here regardless — the same rule every vendored component follows.

---

## stb_image — public domain **or** MIT (dual, at your option)

Used for: decoding the touch-skin, CRT-overlay and theme PNGs (`native/vendor/stb_image/stb_image.h`,
v2.30). Copyright (c) 2017 Sean Barrett. The full dual-licence statement is at the end of that file.

⚠️ **Shell-side, not in the engine** — the same footnote the SDL2 section carries. `stb_image` is
compiled into the SDL *shell* (`shell/image.cpp`), so it ships in **every** artifact — the
**PortMaster** and **Windows** packages, and the APK's SDL-app `.so`. It is **not** linked into the
`pockettracker` engine library, but every artifact runs the shell, so that distinction does not
change what is distributed. It is under
`native/vendor/` only because that is where the licence guard looks; see
`native/vendor/stb_image/PT-VENDORING.md`. The dual grant carries no reproduce-in-binary obligation
(the public-domain arm has no conditions at all), but the notice is recorded here regardless — the
same rule every vendored component follows.

---

## stb_truetype — public domain **or** MIT (dual, at your option)

Used for: rasterizing the PORTRAIT2 device skin's button-label glyphs from the app's Helvetica
(`native/vendor/stb_truetype/stb_truetype.h`, v1.26). Copyright (c) 2017 Sean Barrett. The full
dual-licence statement is at the end of that file.

⚠️ **Shell-side, not in the engine** — the same footnote stb_image carries. `stb_truetype` is
compiled into the SDL *shell* (`shell/font_raster.cpp`), so it ships in **every** artifact — the
**PortMaster** and **Windows** packages, and the APK's SDL-app `.so`. It is **not** linked into the
`pockettracker` engine library, but every artifact runs the shell, so that distinction does not
change what is distributed. It is under
`native/vendor/` only because that is where the licence guard looks; see
`native/vendor/stb_truetype/PT-VENDORING.md`. The dual grant carries no reproduce-in-binary
obligation (the public-domain arm has no conditions at all), but the notice is recorded here
regardless — the same rule every vendored component follows.

---

## nlohmann/json — MIT

Used for: parsing `.ptp` / `.pti` project and instrument JSON (`native/vendor/nlohmann/json.hpp`,
v3.11.3). Copyright (c) 2013-2022 Niels Lohmann. `SPDX-License-Identifier: MIT` is carried
throughout the header; MIT text as above.

---

## libogg — BSD-3-Clause

Used for: Ogg container parsing under Opus/Vorbis (`native/vendor/ogg/`).
Copyright (c) 2002, Xiph.org Foundation. **Full text ships separately** as
`licenses/libogg-COPYING` (copied verbatim from `native/vendor/ogg/COPYING`).

---

## libopus — BSD-3-Clause

Used for: native Opus decoding (`native/vendor/opus/`). Copyright (c) 2001-2011 Xiph.Org,
Skype Limited, Octasic, Jean-Marc Valin, Timothy B. Terriberry, CSIRO, Gregory Maxwell,
Mark Borgerding, Erik de Castro Lopo. **Full text ships separately** as `licenses/libopus-COPYING`
(copied verbatim from `native/vendor/opus/COPYING`), alongside `libopus-LICENSE_PLEASE_READ.txt`,
which is upstream's patent/licensing note and is **not optional reading** despite the name.

---

## opusfile — BSD-3-Clause

Used for: Opus stream/file decoding on top of libopus (`native/vendor/opusfile/`).

> ⚠️ **The vendored copy of opusfile does not include its `COPYING`** — every source file says the
> work is "GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE", and that file was not
> carried across when the library was vendored. The notice below reproduces upstream's
> (github.com/xiph/opusfile) so the pointer in those headers resolves to something.

```
Copyright (c) 1994-2013 Xiph.Org Foundation and contributors

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

- Neither the name of the Xiph.Org Foundation nor the names of its contributors
  may be used to endorse or promote products derived from this software without
  specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

---

## SDL2 — zlib licence (**shipped on Windows and in the APK**, linked-not-shipped on the handhelds)

⚠️ **The answer differs per artifact**, so "does PocketTracker ship an SDL2 binary?" has no single
answer — and the question that binds is always about the thing a user actually receives, not about
the source tree.

| artifact | how SDL2 is linked | ships an SDL2 binary? |
|---|---|---|
| **PortMaster zip** (`shell/build-portmaster.sh`) | dynamically, against the **device's own** `libSDL2-2.0.so.0` — the copy its CFW patched for that hardware's display and audio | **no** (the build asserts `libs.aarch64` is absent) |
| **Windows zip** (`shell/build-windows.ps1`) | **statically, into `PocketTracker.exe`** — a Windows box has no system SDL2, so `shell/CMakeLists.txt` falls through to FetchContent | **yes — inside the exe** |
| **APK** | **dynamically, against a `libSDL2.so` built from the vendored source** (`native/vendor/SDL2/`) and packaged in the APK — F-Droid builds offline from source and rejects prebuilts, so Android is the one target that carries SDL in-tree | **yes — `lib/<abi>/libSDL2.so`, one per ABI** |
| **Miyoo/OnionOS zip** (`shell/build-miyoo.sh`) | dynamically, against a **prebuilt `mmiyoo` fork we bundle** — this device has no system SDL2 at all, so the PortMaster row's rule inverts and the build asserts `libs/` is **present** | **yes — `libs/libSDL2-2.0.so.0`**, and see the fork's own section below |

So the Windows package carries the notice below, and `build-windows.ps1` copies it out of the SDL
source tree that was actually compiled (`_deps/sdl2-src/LICENSE.txt`) rather than from a stale copy
in this repo — the licence that ships is then the licence of the code that shipped, by construction.

⚠️ **On the APK's obligation specifically:** zlib's three conditions bind *source* distributions
(clause 3 is "may not be removed or altered from any **source** distribution"); unlike the
BSD-3-Clause components in this file, it imposes no reproduce-in-binary-form requirement. So
shipping `libSDL2.so` in the APK adds no obligation beyond the notice recorded here. The APK carries
this file, `LICENSE`, `CREDITS.md` and the OFL text as `assets/licenses/`; no in-app screen displays
them. Stated rather than assumed, because "we now ship one more library" is the kind of change that
looks like it must have made a compliance problem worse.

⚠️ **The vendor-directory guard covers SDL2 on one side only.** `build-portmaster.sh` derives its
component list from `native/vendor/*/` precisely so that vendoring a library and forgetting its
notice fails the build, and `native/vendor/SDL2/` is in the tree, so the derived guard covers it —
including, over-inclusively, in the PortMaster zip, which links SDL rather than shipping it (see the
note at that loop). **The Windows build is the exception**: it takes its SDL from FetchContent, not
from `native/vendor/`, so the derived guard is structurally blind there and `build-windows.ps1`
checks for the SDL notice **by name**.

Used for: window, renderer, audio output, gamepad and keyboard input — the whole of `shell/`, on
every platform including Android. **Two versions, deliberately** — different linkage models get
different version policies:

| | version | why |
|---|---|---|
| Android (vendored, `native/vendor/SDL2/`) | **2.30.9** (`release-2.30.9`, commit `c98c4fbf`) | the APK bundles its own, so it should carry a current 2.x |
| Windows (FetchContent pin, `shell/CMakeLists.txt`) | **2.30.9** | built from source into the exe |
| PortMaster (`SDL2_TAG`, the *link floor*) | **2.0.18** | ports are expected to link the CFW's own libSDL2; this is the compatibility floor they are built against, not a copy we ship |
| Miyoo/OnionOS (`SDL2_TAG`, the *shipped binary*) | **2.0.20**, the `mmiyoo` fork — see below | not a floor but the exact build that ships, so it is pinned to one tag |

```
Copyright (C) 1997-2024 Sam Lantinga <slouken@libsdl.org>

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

---

## SDL2 `mmiyoo` fork — GPL-3.0 (**Miyoo/OnionOS package only**)

`XK9274/sdl2_miyoo`, the SDL 2.0.20 fork carrying video (`MI GFX`), audio (`MI AO`), joystick and
haptic drivers for the SigmaStar SSD202D. Shipped as `libs/libSDL2-2.0.so.0` in the OnionOS package
and **nowhere else** — that device has no system SDL2, so unlike every other handheld artifact this
one carries the binary.

The fork's tree holds two licence files and both ship, because they cover different halves of it:

| file in the fork | ships as | covers |
|---|---|---|
| `LICENSE.txt` | `licenses/libSDL2-zlib-LICENSE.txt` | upstream SDL2, zlib, © 1997-2022 Sam Lantinga |
| `LICENSE` | `licenses/libSDL2-miyoo-fork-GPL-3.0.txt` | the fork as distributed, GPL-3.0 |

PocketTracker is GPL-3.0 itself, so linking against a GPL-3.0 library raises no compatibility
question. `build-miyoo.sh` copies both texts **out of the fork's own tree at the pinned tag**, so
what ships is the licence of the code that shipped.

⚠️ **THE `libneonarmmiyoo.so` BESIDE IT IS NOT A THIRD-PARTY COMPONENT — IT IS OURS.** The fork
resolves 25 symbols from a library of that name (`neon_memcpy` and 24 integer scalers) and lists it
as a `DT_NEEDED`, so the package cannot start without a file of that name defining every one of
them. The one the fork's own release carries cannot be redistributed: its repository
(`XK9274/neon-arm-library-miyoo`) has no licence file at all and its README states that the
maintainer did not write the source and does not know its origin, which leaves it under default
copyright. What ships instead is `shell/miyoo/neon_compat.c`, PocketTracker's own plain-C
implementation of the same 25 entry points, covered by the GPL-3.0 in `licenses/LICENSE` with
everything else of ours. Nothing from that repository is in the package.

---

## Linux Biolinum — SIL Open Font License 1.1

Used for: the D-pad ARROW glyphs (↑↓←→) on the PORTRAIT2 touch skin's virtual buttons
(`app/src/main/assets/fonts/LinBiolinum_Rah.ttf`). Helvetica, the button-label font, ships no arrow
glyphs, so this font supplies exactly those four.

⚠️ **This is a bundled FONT ASSET, not an engine-compiled component** — unlike everything above it, it
is not statically linked. It ships wherever the app's `assets/` travel: the **APK** for certain, and
any desktop/handheld build that bundles `assets/fonts/`. Documented here (the single source of truth
for shipped notices) rather than in `CREDITS.md`, because the OFL requires its licence to accompany
the font in binary distributions — the full licence text is `licenses/OFL-1.1-LinuxBiolinum.txt`.

Linux Biolinum is dual-licensed GPL-2.0-or-later (with a font exception) **and** SIL OFL 1.1;
PocketTracker uses it under the OFL.

```
Copyright (c) 2003–2012, Philipp H. Poll (www.linuxlibertine.org | gillian at linuxlibertine.org),
with Reserved Font Name "Linux Libertine" and "Biolinum".

This Font Software is licensed under the SIL Open Font License, Version 1.1.
Full text: licenses/OFL-1.1-LinuxBiolinum.txt (and http://scripts.sil.org/OFL).
```

---

## Oboe and AndroidX — Apache-2.0 (**APK only**)

Used for: Oboe is the low-latency Android audio stream (`native/` links it behind `if(ANDROID)`);
`androidx.core:core-ktx` handles window insets and `androidx.core:core-splashscreen` the Android 12+
splash screen, both in the launcher activity.

⚠️ **Four AndroidX artifacts reach the APK, not the two `app/build.gradle.kts` names** — `core-ktx`
drags in `androidx.core:core` and `androidx.core:core-viewtree` transitively. All four are Apache-2.0
under the same copyright, so one copy of the text covers them; the count is recorded because "what we
declared" and "what we shipped" are different questions, and only the second one binds.

⚠️ **These are resolved by Gradle, not compiled from `native/`** — they exist only in the APK, and no
Linux, Windows or PortMaster artifact contains a byte of them. They are listed in `CREDITS.md` with
everything else the project builds on.

⚠️ **They are recorded HERE as well because naming a licence is not supplying one.** Apache-2.0 §4(a)
requires that a redistributor "give any other recipients of the Work or Derivative Works a copy of
this License" — an obligation `CREDITS.md` cannot discharge by citing it. This is the same reason the
Linux Biolinum font is documented here rather than there: the scope of this file is *what a recipient
of a binary must receive*, which is a wider set than *what is statically linked*.

⚠️ **The AndroidX AARs do carry `META-INF/androidx/…/LICENSE.txt`, and it never reached a user** —
AGP strips `META-INF` when packaging, verified by `unzip -l` on the built APK: zero entries matching
`META-INF/.*LICENSE`. So the dependency shipped while its licence did not, which is precisely the gap
this section closes. Oboe's AAR carries no licence file at all. **None of them ships a `NOTICE`
file**, so §4(d) adds no attribution addendum to reproduce alongside the text.

The text below is the canonical Apache License 2.0, reproduced verbatim from
`https://www.apache.org/licenses/LICENSE-2.0.txt` (sha256
`cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30`), and verified byte-for-byte
against the independent copy bundled with Android Studio.

```

                                 Apache License
                           Version 2.0, January 2004
                        http://www.apache.org/licenses/

   TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION

   1. Definitions.

      "License" shall mean the terms and conditions for use, reproduction,
      and distribution as defined by Sections 1 through 9 of this document.

      "Licensor" shall mean the copyright owner or entity authorized by
      the copyright owner that is granting the License.

      "Legal Entity" shall mean the union of the acting entity and all
      other entities that control, are controlled by, or are under common
      control with that entity. For the purposes of this definition,
      "control" means (i) the power, direct or indirect, to cause the
      direction or management of such entity, whether by contract or
      otherwise, or (ii) ownership of fifty percent (50%) or more of the
      outstanding shares, or (iii) beneficial ownership of such entity.

      "You" (or "Your") shall mean an individual or Legal Entity
      exercising permissions granted by this License.

      "Source" form shall mean the preferred form for making modifications,
      including but not limited to software source code, documentation
      source, and configuration files.

      "Object" form shall mean any form resulting from mechanical
      transformation or translation of a Source form, including but
      not limited to compiled object code, generated documentation,
      and conversions to other media types.

      "Work" shall mean the work of authorship, whether in Source or
      Object form, made available under the License, as indicated by a
      copyright notice that is included in or attached to the work
      (an example is provided in the Appendix below).

      "Derivative Works" shall mean any work, whether in Source or Object
      form, that is based on (or derived from) the Work and for which the
      editorial revisions, annotations, elaborations, or other modifications
      represent, as a whole, an original work of authorship. For the purposes
      of this License, Derivative Works shall not include works that remain
      separable from, or merely link (or bind by name) to the interfaces of,
      the Work and Derivative Works thereof.

      "Contribution" shall mean any work of authorship, including
      the original version of the Work and any modifications or additions
      to that Work or Derivative Works thereof, that is intentionally
      submitted to Licensor for inclusion in the Work by the copyright owner
      or by an individual or Legal Entity authorized to submit on behalf of
      the copyright owner. For the purposes of this definition, "submitted"
      means any form of electronic, verbal, or written communication sent
      to the Licensor or its representatives, including but not limited to
      communication on electronic mailing lists, source code control systems,
      and issue tracking systems that are managed by, or on behalf of, the
      Licensor for the purpose of discussing and improving the Work, but
      excluding communication that is conspicuously marked or otherwise
      designated in writing by the copyright owner as "Not a Contribution."

      "Contributor" shall mean Licensor and any individual or Legal Entity
      on behalf of whom a Contribution has been received by Licensor and
      subsequently incorporated within the Work.

   2. Grant of Copyright License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      copyright license to reproduce, prepare Derivative Works of,
      publicly display, publicly perform, sublicense, and distribute the
      Work and such Derivative Works in Source or Object form.

   3. Grant of Patent License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      (except as stated in this section) patent license to make, have made,
      use, offer to sell, sell, import, and otherwise transfer the Work,
      where such license applies only to those patent claims licensable
      by such Contributor that are necessarily infringed by their
      Contribution(s) alone or by combination of their Contribution(s)
      with the Work to which such Contribution(s) was submitted. If You
      institute patent litigation against any entity (including a
      cross-claim or counterclaim in a lawsuit) alleging that the Work
      or a Contribution incorporated within the Work constitutes direct
      or contributory patent infringement, then any patent licenses
      granted to You under this License for that Work shall terminate
      as of the date such litigation is filed.

   4. Redistribution. You may reproduce and distribute copies of the
      Work or Derivative Works thereof in any medium, with or without
      modifications, and in Source or Object form, provided that You
      meet the following conditions:

      (a) You must give any other recipients of the Work or
          Derivative Works a copy of this License; and

      (b) You must cause any modified files to carry prominent notices
          stating that You changed the files; and

      (c) You must retain, in the Source form of any Derivative Works
          that You distribute, all copyright, patent, trademark, and
          attribution notices from the Source form of the Work,
          excluding those notices that do not pertain to any part of
          the Derivative Works; and

      (d) If the Work includes a "NOTICE" text file as part of its
          distribution, then any Derivative Works that You distribute must
          include a readable copy of the attribution notices contained
          within such NOTICE file, excluding those notices that do not
          pertain to any part of the Derivative Works, in at least one
          of the following places: within a NOTICE text file distributed
          as part of the Derivative Works; within the Source form or
          documentation, if provided along with the Derivative Works; or,
          within a display generated by the Derivative Works, if and
          wherever such third-party notices normally appear. The contents
          of the NOTICE file are for informational purposes only and
          do not modify the License. You may add Your own attribution
          notices within Derivative Works that You distribute, alongside
          or as an addendum to the NOTICE text from the Work, provided
          that such additional attribution notices cannot be construed
          as modifying the License.

      You may add Your own copyright statement to Your modifications and
      may provide additional or different license terms and conditions
      for use, reproduction, or distribution of Your modifications, or
      for any such Derivative Works as a whole, provided Your use,
      reproduction, and distribution of the Work otherwise complies with
      the conditions stated in this License.

   5. Submission of Contributions. Unless You explicitly state otherwise,
      any Contribution intentionally submitted for inclusion in the Work
      by You to the Licensor shall be under the terms and conditions of
      this License, without any additional terms or conditions.
      Notwithstanding the above, nothing herein shall supersede or modify
      the terms of any separate license agreement you may have executed
      with Licensor regarding such Contributions.

   6. Trademarks. This License does not grant permission to use the trade
      names, trademarks, service marks, or product names of the Licensor,
      except as required for reasonable and customary use in describing the
      origin of the Work and reproducing the content of the NOTICE file.

   7. Disclaimer of Warranty. Unless required by applicable law or
      agreed to in writing, Licensor provides the Work (and each
      Contributor provides its Contributions) on an "AS IS" BASIS,
      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
      implied, including, without limitation, any warranties or conditions
      of TITLE, NON-INFRINGEMENT, MERCHANTABILITY, or FITNESS FOR A
      PARTICULAR PURPOSE. You are solely responsible for determining the
      appropriateness of using or redistributing the Work and assume any
      risks associated with Your exercise of permissions under this License.

   8. Limitation of Liability. In no event and under no legal theory,
      whether in tort (including negligence), contract, or otherwise,
      unless required by applicable law (such as deliberate and grossly
      negligent acts) or agreed to in writing, shall any Contributor be
      liable to You for damages, including any direct, indirect, special,
      incidental, or consequential damages of any character arising as a
      result of this License or out of the use or inability to use the
      Work (including but not limited to damages for loss of goodwill,
      work stoppage, computer failure or malfunction, or any and all
      other commercial damages or losses), even if such Contributor
      has been advised of the possibility of such damages.

   9. Accepting Warranty or Additional Liability. While redistributing
      the Work or Derivative Works thereof, You may choose to offer,
      and charge a fee for, acceptance of support, warranty, indemnity,
      or other liability obligations and/or rights consistent with this
      License. However, in accepting such obligations, You may act only
      on Your own behalf and on Your sole responsibility, not on behalf
      of any other Contributor, and only if You agree to indemnify,
      defend, and hold each Contributor harmless for any liability
      incurred by, or claims asserted against, such Contributor by reason
      of your accepting any such warranty or additional liability.

   END OF TERMS AND CONDITIONS

   APPENDIX: How to apply the Apache License to your work.

      To apply the Apache License to your work, attach the following
      boilerplate notice, with the fields enclosed by brackets "[]"
      replaced with your own identifying information. (Don't include
      the brackets!)  The text should be enclosed in the appropriate
      comment syntax for the file format. We also recommend that a
      file or class name and description of purpose be included on the
      same "printed page" as the copyright notice for easier
      identification within third-party archives.

   Copyright [yyyy] [name of copyright owner]

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
```
