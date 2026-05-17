# DSP Settings Guide

Internal constants, mapping ranges, and character notes for every DSP unit in PocketTracker.
Each section documents the user-facing parameters **and** the hardcoded values you can change
in source to shift the character of the effect.

---

## Sample Editor — Destructive FX

These live in the EFFECT row of the sample editor and are applied offline to the sample buffer.

---

### SYNC → TSTRETCH
`app/src/main/cpp/effects/primitives/sola-stretch.h`

SOLA (Synchronized Overlap-Add) time-stretch. Changes duration without affecting pitch.
Akai-cyclic flavor — the grit is intentional, it's the same algorithm Akai S950/S1000 used.

**User control:** DURATION target (4 BAR → 1/32) + project BPM. The ratio is computed automatically.

**Internal knobs:**

| Constant | Default | Effect |
|---|---|---|
| `SEQUENCE_MS` | `40.0f` | Chunk size. Smaller = grittier/choppier, larger = smoother |
| `OVERLAP_MS` | `15.0f` | Crossfade between chunks. Keep at ~25-40% of SEQUENCE_MS |
| `SEEK_MS` | `0.0f` | Splice-point search. 0 = pure cyclic Akai. 10-15 = intelligent/smoother |

**Ratio clamp:** 0.1 – 10.0 (hardcoded in `stretch()`; extreme values produce heavy artifacts).

**Preset feels:**

| Vibe | SEQUENCE | OVERLAP | SEEK |
|---|---|---|---|
| S950 jungle (default) | 40 | 15 | 0 |
| Choppy / vintage | 25 | 8 | 0 |
| S3000 / smoother | 50 | 20 | 12 |
| Clean (no character) | 40 | 15 | 15 |

Keep OVERLAP at roughly 25-40% of SEQUENCE or you get clicks (too low) or comb filtering (too high).

---

### SYNC → RPITCH
`app/src/main/cpp/audio-engine.cpp` → `AudioEngine::pitchShiftSample()`

Destructive pitch shift via linear interpolation resampling. Changes pitch by changing
sample length — a 2x pitch-up halves the buffer, a -12 semitone shift doubles it.
No time-stretching; the loop length changes.

**User control:** DURATION + BPM. The semitone shift is rounded to the nearest integer (±24 max).

**No internal knobs.** The algorithm is pure linear interpolation — no constants to tune.
Character comes entirely from the aliasing/interpolation artifacts of the resampled buffer.

---

### OTT (sample editor)
`app/src/main/cpp/effects/modules/ott-module.h`

Same algorithm as the master bus OTT (3-band bidirectional compressor) applied offline
to the sample buffer. Runs at the sample's own rate via `applySampleFx(fxType=0)`.

**User control:** `fxValue` 00-FF → OTT depth (0=bypass, FF=full).

**Internal constants** (shared with master bus OTT — see Master Bus section below).

---

### DUST (sample editor)
`app/src/main/cpp/effects/modules/dust-chain.h`

skoomaDust applied offline to the sample buffer.
Full signal chain: low-shelf → LP → tube saturation → FET compressor → wow/drift → bitcrush → soft-clip.

**User control:** `fxValue` 00-FF → dust amount (all stages scale with this single knob).

**Internal constants** (shared with master bus DUST — see Master Bus section below).

---

### DRIVE (sample editor)
Offline version of the per-instrument drive chain. See Per-Instrument Chain → Drive below.

**User control:** `fxValue` 00-FF → drive amount.

---

### EQ (sample editor)
Applies a saved EQ preset (slot 00-7F) offline to the sample buffer using the same biquad
coefficients as the real-time per-instrument EQ. See Per-Instrument Chain → Filter below
for the frequency mapping. No internal constants beyond the biquad math.

---

## Per-Instrument Voice Chain

Applied in real-time to every voice. Configured on the instrument screen.
`app/src/main/cpp/effects/instrument-chain.h`

---

### Filter (SVF)
`app/src/main/cpp/effects/modules/filter-module.h`

DaisySP state-variable filter (Andrew Simper / musicdsp.org). Double-sampled for better
HF accuracy. Nonlinear resonance stabilisation via cubic band term gives it character
at high resonance — it won't self-oscillate but it does saturate.

**Types:** LP, HP, BP, Notch, Peak

**User params:**

| Param | Range | Maps to |
|---|---|---|
| CUT | 00-FF | 20 Hz – 20 kHz, exponential: `20 * 1000^(cut/255)` |
| RES | 00-FF | 0.0 – 1.0 linear |

**Internal knob:**

| Field | Default | Effect |
|---|---|---|
| `drive` (init default) | `150` | SVF resonance saturation. `drive / 25.5f` → DaisySP SetDrive 0-10. At 150 ≈ 5.9; higher = more nonlinear character at high resonance. Currently not exposed in the UI — edit the default in FilterModule to shift the baseline character. |

---

### Drive (overdrive)
`app/src/main/cpp/effects/modules/drive-module.h`

DaisySP Overdrive (Emilie Gillet). SoftClip via Padé rational approximant with
drive-dependent gain staging. Below ~SetDrive(0.3) the output attenuates heavily,
so the mapping starts there.

**User param:** DRIVE 00-FF

**Internal mapping:**
```
SetDrive(0.30 + (drive/255.0) * 0.30)
```
Range is 0.30–0.60. Below 0.30 = unusable attenuation (DaisySP quirk). Above 0.60 = very heavy
clipping. Shifting the base up (`0.35`) gives more drive at low settings; narrowing the range
(`* 0.20`) makes the knob less aggressive.

---

### Crush (bitcrush + downsample)
`app/src/main/cpp/effects/modules/crush-module.h`

DaisySP Decimator. Two stages: sample-hold downsampling first, then integer bit-depth reduction.

**User params:**

| Param | Range | Maps to |
|---|---|---|
| CRUSH | 00-0F (0-15) | `SetBitsToCrush(crush)`. 0=bypass, 1=15-bit, 15=1-bit |
| DWNS | 00-0F (0-15) | `SetDownsampleFactor(downsample / 15.0f)`. 0=bypass, 15=max aliasing |

**No additional internal constants.** The 4-bit (0-15) ranges are exposed directly to DaisySP.
Widening to 8-bit (change the `/15.0f` to `/255.0f` and expose 00-FF in the UI) would give
finer control over the downsample.

---

## Send Effects

Stereo send buses; voices contribute according to their per-instrument REV/DEL send level.

---

### Reverb
`app/src/main/cpp/effects/modules/reverb-module.h`

DaisySP ReverbSc (Schroeder-Moorer algorithm). Takes a stereo send-bus input, outputs stereo wet.

**User params (Effects screen):**

| Param | Range | Maps to |
|---|---|---|
| SIZE | 00-FF | `SetFeedback(value / 255.0)`. 0=short, FF=very long |
| DAMP | 00-FF | `SetLpFreq(200 * 100^(value/255))`. 00=200 Hz (dark), FF=20 kHz (bright) |

**Default init values:**
- Feedback: `0x60 / 255 ≈ 0.376`
- LpFreq: `200 * 100^(0x80/255) ≈ 1122 Hz`

**No algorithm-level knobs** — ReverbSc is a fixed Schroeder structure. All character comes
from SIZE and DAMP. The exponential DAMP mapping (`powf(100, x)`) means the low end of
the knob is very dark and the top end opens up fast — change `100.0f` to `10.0f` for a more
linear-feeling sweep.

---

### Delay
`app/src/main/cpp/effects/modules/delay-module.h`

DaisySP DelayLine, stereo (separate L/R lines so panned sources echo on the correct side).

**User params (Effects screen):**

| Param | Range | Maps to |
|---|---|---|
| TIME (free) | 00-FF | `(value/255.0) * 2.0 * sampleRate` samples. 0–2 seconds |
| TIME (sync) | 0-11 | One of 12 subdivisions (1/1 → 1/16D) |
| FDBK | 00-FF | `value / 255.0`. 0=no repeat, FF=infinite |
| REV | 00-FF | Delay→Reverb send. How much delay output feeds reverb input |

**Subdivision table** (SYNC mode, index 00-0B):

```
00=1/1  01=1/2  02=1/4  03=1/8  04=1/16  05=1/32
06=1/2T 07=1/4T 08=1/8T 09=1/4D 10=1/8D  11=1/16D
```

**Internal constant:**
```cpp
static constexpr size_t DELAY_MAX_SAMPLES = 88200;  // 2 seconds at 44100 Hz
```
Raise to `176400` for 4-second maximum (doubles RAM usage: 2 × 176400 × 4 bytes ≈ 1.4 MB).

**Default init:** feedback = `0x60/255 ≈ 0.376`, time = 1/8 note at 120 BPM (125 ms).

---

## Master Bus

Applied after the mix of all tracks. Signal flow: `track mix → limiter → OTT or DUST`.

---

### Limiter
`app/src/main/cpp/effects/modules/limiter-module.h`

DaisySP Limiter (Emilie Gillet / Mutable Instruments). Peak-tracking soft limiter.
Fast attack (α=0.05), slow release (α=0.00002). Output ceiling ≈ −3 dBFS (the 0.7
factor is baked into DaisySP). Always active; not user-configurable.

**Internal knob:**

| Field | Default | Effect |
|---|---|---|
| `preGain` | `1.0f` | Input gain before peak detection. Raise to drive the limiter harder (more saturation, lower ceiling). Currently not exposed in UI. |

---

### OTT (master bus)
`app/src/main/cpp/effects/modules/ott-module.h`

3-band bidirectional compressor. Downward (8:1 above −27 dBFS) + upward (4:1 below −35 dBFS)
per band. VitOTT-matched crossover at 120 Hz / 2500 Hz.

**User control:** OTT depth 00-FF on the Effects screen (0=bypass, FF=full).

**Internal constants:**

| Constant | Value | Effect |
|---|---|---|
| Crossover low | 120 Hz | Low/mid split. Raise to push more content into the mid band |
| Crossover high | 2500 Hz | Mid/high split |
| Downward threshold | −27 dBFS | Above this → compressed. Lower (e.g. −24) for more obvious gain reduction |
| Upward threshold | −35 dBFS | Below this → expanded upward. ~8 dB gap above downward threshold prevents lifting noise floor |
| Downward ratio | 8:1 | Hard-coded. Lower = gentler compression |
| Upward ratio | 4:1 | Hard-coded. Lower = subtler expansion |
| `OUTPUT_GAIN` | `2.0f` (+6 dB) | Post-band gain after compression sum. Reduce to −3 dB (`1.41f`) for a less loud result |
| Band attack/release: Low | 2.8 ms / 40 ms | Slower = more bass pumping character |
| Band attack/release: Mid | 1.4 ms / 28 ms | |
| Band attack/release: High | 0.7 ms / 15 ms | Faster = more transient sparkle |
| `WARMUP_SAMPLES` | `512` (~11.6 ms) | Fade-in on first note after silence. Hides LR4 filter transient |
| `SILENCE_RESET_FRAMES` | `22050` (500 ms) | How long silence must last before auto-reset triggers |

---

### DUST (master bus)
`app/src/main/cpp/effects/modules/dust-chain.h`

skoomaDust — a "lo-fi console" chain. Signal flow:
`low-shelf → LP → tube saturation (Airwindows Tube2) → FET compressor (APComp) → wow/drift → bitcrush → soft-clip → LP2`

**User control:** DUST depth 00-FF on the Effects screen.

**Internal constants:**

**LP filter (tone shaping):**

| Constant | Value | Effect |
|---|---|---|
| `kLPCutoffMax` | 10000 Hz | Cutoff at knob = 0% |
| `kLPCutoffMin` | 5000 Hz | Cutoff at knob = 100% (gets darker as you push) |

**Low-shelf (sub control before tube):**

| Constant | Value | Effect |
|---|---|---|
| `kLowShelfHz` | 80 Hz | Shelf frequency |
| `kLowShelfDb` | −4 dB | Sub-bass cut before tube, prevents harmonics from sub content. Raise toward 0 to keep more bottom end |
| `kLowShelfQ` | 0.7 | Standard Butterworth shelf |

**Tube saturation:**

| Constant | Value | Effect |
|---|---|---|
| `kTubeInputPad` | 0.5 | Pre-gain before tube (−6 dB). Raise for more saturation at lower knob values |
| `kTubePostGainDb` | −4 dB | Post-tube gain. Makes the signal cooler before the compressor sees it. Raise toward 0 for hotter compression |

**Bitcrush:**

| Constant | Value | Effect |
|---|---|---|
| `kBitcrushBits` | 12 | Bits reduced post-compressor. 12 = "S900/S950" territory. 8 = harsh lo-fi. 16 = bypass territory |

**Soft clipper:**

| Constant | Value | Effect |
|---|---|---|
| `kClipKnee` | 0.95 | Knee position (≈ −0.45 dBFS). Below this = transparent. Raise to 1.0 to remove headroom |

**Wow & drift (tape wobble):**

| Constant | Value | Effect |
|---|---|---|
| `kWowFreqMin/Max` | 0.05 / 0.25 Hz | LFO rate at knob 0%/100%. Raise Max for more pitch wobble |
| `kWowAmpMin/Max` | 2 / 12 samples | Modulation depth in samples. 12 samples ≈ 0.27 ms = subtle. Raise for more wow |
| `kDriftLpMin/Max` | 0.01 / 0.07 Hz | Drift (slower, random-walk) rate |
| `kDriftAmpMin/Max` | 2 / 12 samples | Drift depth |
| `kWowSplitHz` | 200 Hz | LF/HF crossover for wow modulation alignment |

**FET compressor (APComp):**

| Constant | Value | Effect |
|---|---|---|
| `kCompThresholdMin` | −25 dB | Threshold at knob = 100%. Raise toward −18 for less compression at full |
| `kCompRatio` | 10:1 | Hard FET character. Lower to 4:1 for gentler feel |
| `kCompAttackSec` | 0.0005 s (0.5 ms) | Very fast — lets transients through. Raise to 0.003 for more peak control |
| `kCompReleaseSec` | 0.150 s (150 ms) | Raise for more obvious pumping |
| `kCompConvexity` | 1.1 | Knee softness exponent. Higher = softer engagement |
| `kCompInertia` | 0.1 | FET-76 velocity overshoot mass. Raise for more "slam" |
| `kCompInertiaDecay` | 0.94 | How fast the inertia decays. Lower = longer overshoot |
| `kCompMakeupDrive` | 0.1 | Extra makeup that scales with knob (+0% at 0, +30% at 100%) |
| `kMakeupEnvAttackMs` | 5 ms | RMS auto-makeup attack |
| `kMakeupEnvReleaseMs` | 300 ms | RMS auto-makeup release |
