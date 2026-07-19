# Credits & Acknowledgements

PocketTracker stands on a lot of excellent open-source work. Thank you to everyone below.

---

## Libraries

| Library | License | Use in PocketTracker |
|---|---|---|
| [Oboe](https://github.com/google/oboe) | Apache 2.0 | Low-latency Android audio stream |
| [DaisySP](https://github.com/electro-smith/DaisySP) | MIT **and** LGPL-2.1 (mixed — see note) | SVF filter, ReverbSc, DelayLine, Compressor, Limiter, BitCrush |
| [TinySoundFont](https://github.com/schellingb/TinySoundFont) | MIT | SF2 / SoundFont2 synthesizer (with per-channel rendering fork) |
| [KissFFT](https://github.com/mborgerding/kissfft) | BSD-3-Clause | FFT for spectrum analyzer and transient detection |
| [Soundpipe](https://github.com/PaulBatchelor/Soundpipe) (pareq stub) | MIT | Parametric EQ biquad |
| [skoomaDust](https://github.com/skoomabwoy/skoomaDust) | GPL-3.0 | Lo-fi effect chain; includes APComp FET compressor by Alain Paul (BSD-3-Clause) |
| [dr_libs](https://github.com/mackron/dr_libs) (dr_mp3 / dr_flac) | Public domain (MIT-0) | Native MP3 / FLAC decoding |
| [stb_vorbis](https://github.com/nothings/stb) | Public domain (MIT) | Native OGG Vorbis decoding |
| [libopus / opusfile](https://opus-codec.org/) | BSD-3-Clause | Native Opus decoding |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | Parsing `.ptp` / `.pti` project + instrument JSON |
| [Jetpack Compose](https://developer.android.com/jetpack/compose) | Apache 2.0 | Android UI toolkit |
| [Kotlinx Serialization](https://github.com/Kotlin/kotlinx.serialization) | Apache 2.0 | JSON project save / load |

Each library keeps its own license. PocketTracker as a whole is distributed under GPL-3.0-or-later (see [`LICENSE`](LICENSE)).

**Full notices for everything compiled into the audio engine — the code that ships in both the APK
and the Linux port — are in [`licenses/THIRD-PARTY-NOTICES.md`](licenses/THIRD-PARTY-NOTICES.md),**
which is the single source of truth and travels inside the release artifact.

> **Note on DaisySP:** it is not uniformly MIT. Of the eight files PocketTracker compiles, five are
> MIT (`svf`, `overdrive`, `decimator`, `limiter`, `crossfade`) and **three are LGPL-2.1** —
> `compressor` (GRAME / Centre National de Creation Musicale), `balance` (Barry Vercoe, john ffitch,
> Gabriel Maldonado) and `reverbsc` (Sean Costello, Istvan Varga, Paul Batchelor) — because those
> descend from Csound and Faust rather than from Electrosmith's own code. LGPL-2.1 §3 permits
> applying the ordinary GPL v2 "or any later version", so they are distributed here under
> GPL-3.0-or-later along with the rest of the app. Per-file detail in the notices file above.

---

## DSP algorithm references

- **SOLA time-stretch** — Roucos & Wilgus (1985), Verhelst & Roelands (1993). The "Akai-cyclic" mode matches the algorithm used in Akai S950 / S1000 samplers; the characteristic grit on jungle breaks is intentional.
- **Biquad filter design** — Robert Bristow-Johnson, *Audio EQ Cookbook* (1994, rev. 2016).
- **OTT 3-band compressor** — parameters matched to the Xfer Records vitOTT plugin. Downward: −27 dBFS / 8:1; upward: −35 dBFS / 4:1; ~8 dB neutral zone.
- **Spectral flux transient detection** — Brossier et al., "Fast labelling of notes in music signals," ICASSP 2004.

---

## Contributors

- [@skoomabwoy](https://github.com/skoomabwoy) — authored the [skoomaDust](https://github.com/skoomabwoy/skoomaDust) lo-fi effect chain integrated into PocketTracker; ongoing technical advice throughout the project.

---

## Inspiration

- [Dirtywave M8](https://dirtywave.com/) — the portable tracker that proved the concept.
- [LGPT / Little Piggy Tracker](https://github.com/Mdashdotdashn/LittleGPTracker) — open-source tracker heritage.
- [LSDJ](https://www.littlesounddj.com/) — the Game Boy tracker that defined the form.
