# Third-party notices

PocketTracker is distributed under **GPL-3.0-or-later** (see `LICENSE` at the repo root).
It statically links the components below, so **their notices travel with the binary**, not merely
with the source tree that built it. A user who receives only the artifact must still receive these.

This file is the single source of truth for the notices and is shipped verbatim in:

- the **PortMaster zip** → `pockettracker/licenses/THIRD-PARTY-NOTICES.md` (`shell/build-portmaster.sh`)
- the **Windows zip** → `licenses/THIRD-PARTY-NOTICES.md` (`shell/build-windows.ps1`)
- the **repo** → `licenses/THIRD-PARTY-NOTICES.md`

⚠️ **Scope: this file lists what is compiled into the ENGINE**, i.e. everything that ships in *both*
the APK and the Linux port. Android-only dependencies (Oboe, Jetpack Compose, Kotlinx Serialization)
are resolved by Gradle, are **not** in `native/`, and are covered by `CREDITS.md` instead — the Linux
binary contains none of them (`native/CMakeLists.txt` gates Oboe behind `if(ANDROID)`).

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
`LICENSE` text shipped beside this file is their governing text. There is no compatibility problem —
but the attribution above is required and was previously absent.

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

## SDL2 — zlib licence (**shipped on Windows**, linked-not-shipped on the handhelds)

⚠️ **The answer differs per artifact, and it changed on 2026-07-20.** This section used to say
flatly that PocketTracker ships no SDL2 binary. That was true of every artifact that existed when it
was written, and the Windows desktop package (convergence plan A3) made it false — which is the
same shape as the P5-S1 finding: the notices were accurate about the *source tree* and wrong about
the thing a user actually receives.

| artifact | how SDL2 is linked | ships an SDL2 binary? |
|---|---|---|
| **PortMaster zip** (`shell/build-portmaster.sh`) | dynamically, against the **device's own** `libSDL2-2.0.so.0` — the copy its CFW patched for that hardware's display and audio | **no** (the build asserts `libs.aarch64` is absent) |
| **Windows zip** (`shell/build-windows.ps1`) | **statically, into `PocketTracker.exe`** — a Windows box has no system SDL2, so `shell/CMakeLists.txt` falls through to FetchContent | **yes — inside the exe** |
| **APK** | no SDL2 at all until convergence phase C1 | no |

So the Windows package carries the notice below, and `build-windows.ps1` copies it out of the SDL
source tree that was actually compiled (`_deps/sdl2-src/LICENSE.txt`) rather than from a stale copy
in this repo — the licence that ships is then the licence of the code that shipped, by construction.

⚠️ **The vendor-directory guard cannot catch this one.** `build-portmaster.sh` derives its
component list from `native/vendor/*/` precisely so that vendoring a library and forgetting its
notice fails the build. SDL2 is fetched at configure time and has never been in `native/vendor/`, so
it is invisible to that mechanism; `build-windows.ps1` therefore checks for the SDL notice **by
name**, as a special case, with this paragraph as the reason.

Used for: window, renderer, audio output, gamepad and keyboard input (`shell/`). Version: whatever
`SDL2_TAG` / the FetchContent pin in `shell/CMakeLists.txt` names at build time — currently
`release-2.30.9` on Windows, `release-2.0.18` as the PortMaster link floor.

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
