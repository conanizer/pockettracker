# Plan: Audio Effects System (MVP Extension Pack 3)

## Overview

Add a complete effects system to PocketTracker modeled after the M8 tracker:
- 3 send buses (Reverb, Delay, Chorus) with per-instrument send levels
- Per-instrument effects (3-band EQ, resonant filter, drive, bitcrush)
- Master bus effects (compressor, 3-band EQ, limiter)
- Visualization (spectrum analyzer, peak meters, limiter gain reduction)

All effects run in the native C++ audio callback (`onAudioReady`).
No UI work in this phase — data structures, DSP, and JNI bridge only.

---

## Source Libraries

| Library | What We Take | License | Integration |
|---------|-------------|---------|-------------|
| **Airwindows** | Chamber, TapeDelay2, Vibrato, Mojo, DeRez2, Pressure4, ADClip7 | MIT | Cherry-pick source files, strip VST2 scaffolding (Surge approach) |
| **Soundpipe** | pareq (parametric EQ) | MIT | Copy single C module |
| **DaisySP** | Svf (state variable filter) | MIT | Copy single C++ class |
| **KissFFT** | FFT for spectrum visualization | BSD | Git submodule or copy |

### Airwindows Integration Pattern (from Surge)

Each Airwindows effect is 3 files: `Effect.h`, `Effect.cpp`, `EffectProc.cpp`.
To extract from the upstream repo:

1. Copy files from `plugins/LinuxVST/src/{EffectName}/source/`
2. Wrap in namespace: `namespace AW_{EffectName} { ... }`
3. Replace `#include "audioeffectx.h"` with our own `AirwinBase.h`
4. Comment out `createEffectInstance()`
5. `AirwinBase.h` provides minimal type stubs:
   ```cpp
   // Minimal base replacing VST2 AudioEffectX
   typedef int32_t VstInt32;
   class AirwinBase {
   public:
       float sampleRate = 44100.0f;
       virtual void setSampleRate(float sr) { sampleRate = sr; }
       virtual float getSampleRate() { return sampleRate; }
       // Parameters stored as float 0.0-1.0
       float params[16] = {};
       float getParameter(VstInt32 index) { return params[index]; }
       void setParameter(VstInt32 index, float value) { params[index] = value; }
       virtual void processReplacing(float **inputs, float **outputs, VstInt32 sampleFrames) = 0;
       virtual ~AirwinBase() = default;
   };
   ```

---

## File Structure

```
app/src/main/cpp/
    native-audio.cpp              // Existing — modified to add effects chain
    CMakeLists.txt                // Existing — add new source files
    effects/
        AirwinBase.h              // Minimal VST2 replacement base class
        airwindows/
            Chamber.h / .cpp / Proc.cpp
            TapeDelay2.h / .cpp / Proc.cpp
            Vibrato.h / .cpp / Proc.cpp
            Mojo.h / .cpp / Proc.cpp
            DeRez2.h / .cpp / Proc.cpp
            Pressure4.h / .cpp / Proc.cpp
            ADClip7.h / .cpp / Proc.cpp
        soundpipe/
            pareq.h / pareq.c     // Parametric EQ from Soundpipe
            sp_utils.h            // sp_data struct, utilities
        daisysp/
            svf.h / svf.cpp       // State variable filter from DaisySP
        EffectBus.h               // Send bus abstraction
        EffectsChain.h            // Master chain orchestration
        AnalysisEngine.h          // FFT, peak/RMS, GR metering
    kissfft/
        kiss_fft.h / kiss_fft.c
        kiss_fftr.h / kiss_fftr.c
```

---

## Phase 1: Foundation — Audio Bus Architecture

### 1.1 Restructure onAudioReady

Current flow:
```
voices → mix to stereo output → done
```

New flow:
```
voices → per-track dry buffer (8 tracks)
    → per-instrument filter + EQ + drive + bitcrush
    → sum to dry mix bus
    → tap send levels → 3 send buses (reverb, delay, chorus)
    → process send buses
    → sum dry + wet returns
    → master compressor (parallel)
    → master EQ
    → master limiter
    → output
```

### 1.2 Buffer Management

All processing happens in the `onAudioReady` callback at buffer size determined by Oboe
(typically 192-480 frames at 48kHz).

```cpp
// Per-callback scratch buffers (allocated once at init, reused)
struct AudioBusses {
    // Per-track mono buffers (before instrument FX)
    float trackBuffers[8][MAX_BUFFER_SIZE];

    // Stereo bus pairs [L][R]
    float dryBus[2][MAX_BUFFER_SIZE];        // Post per-instrument FX, pre-send
    float reverbSend[2][MAX_BUFFER_SIZE];     // Send bus 1
    float delaySend[2][MAX_BUFFER_SIZE];      // Send bus 2
    float chorusSend[2][MAX_BUFFER_SIZE];     // Send bus 3
    float reverbReturn[2][MAX_BUFFER_SIZE];   // Wet returns
    float delayReturn[2][MAX_BUFFER_SIZE];
    float chorusReturn[2][MAX_BUFFER_SIZE];
    float masterBus[2][MAX_BUFFER_SIZE];      // Final mix

    // Analysis
    float analysisBuffer[MAX_BUFFER_SIZE];    // For FFT
};
```

`MAX_BUFFER_SIZE` = 1024 (covers any Oboe buffer size).
Total static allocation: ~100KB. Fits comfortably in 128MB.

### 1.3 New Data Structures (Kotlin side — TrackerData.kt)

```kotlin
@Serializable
data class InstrumentFX(
    // 3-band EQ (low shelf, peaking mid, high shelf)
    var eqLowFreq: Float = 200f,
    var eqLowGain: Float = 0f,      // dB, -12 to +12
    var eqLowQ: Float = 0.7f,
    var eqMidFreq: Float = 2000f,
    var eqMidGain: Float = 0f,
    var eqMidQ: Float = 1.0f,
    var eqHighFreq: Float = 8000f,
    var eqHighGain: Float = 0f,
    var eqHighQ: Float = 0.7f,

    // Filter (DaisySP SVF)
    var filterType: Int = 0,         // 0=off, 1=LP, 2=HP, 3=BP, 4=notch, 5=peak
    var filterCutoff: Float = 20000f,
    var filterResonance: Float = 0f, // 0.0-1.0
    var filterDrive: Float = 0f,     // 0.0-1.0

    // Drive (Airwindows Mojo)
    var driveAmount: Float = 0f,     // 0.0 = off, 1.0 = full

    // Bitcrush (Airwindows DeRez2)
    var bitcrushRate: Float = 0f,    // 0.0 = off
    var bitcrushBits: Float = 0f,
    var bitcrushHard: Float = 1f,    // 0.0 = soft/uLaw, 1.0 = hard/digital
    var bitcrushDryWet: Float = 0f,

    // Send levels (0.0-1.0)
    var reverbSend: Float = 0f,
    var delaySend: Float = 0f,
    var chorusSend: Float = 0f
)

@Serializable
data class MasterFX(
    // Compressor (Airwindows Pressure4, parallel)
    var compPressure: Float = 0f,
    var compSpeed: Float = 0.5f,
    var compMewiness: Float = 0.5f,
    var compDryWet: Float = 0f,      // 0.0 = bypass, parallel mix control

    // Master EQ (3x Soundpipe pareq)
    var eqLowFreq: Float = 200f,
    var eqLowGain: Float = 0f,
    var eqLowQ: Float = 0.7f,
    var eqMidFreq: Float = 2000f,
    var eqMidGain: Float = 0f,
    var eqMidQ: Float = 1.0f,
    var eqHighFreq: Float = 8000f,
    var eqHighGain: Float = 0f,
    var eqHighQ: Float = 0.7f,

    // Limiter (Airwindows ADClip7)
    var limiterBoost: Float = 0f,
    var limiterSoften: Float = 0.5f,
    var limiterEnhance: Float = 0.5f,
    var limiterMode: Int = 0
)

@Serializable
data class SendFX(
    // Reverb (Airwindows Chamber)
    var reverbBigness: Float = 0.5f,
    var reverbLongness: Float = 0.5f,
    var reverbDarkness: Float = 0.5f,
    var reverbDampness: Float = 0.5f,
    var reverbDryWet: Float = 0.5f,

    // Delay (Airwindows TapeDelay2 + custom ducking)
    var delayTime: Float = 0.5f,       // Mapped to BPM-synced values
    var delayFeedback: Float = 0.3f,
    var delayFilterFreq: Float = 0.5f,
    var delayFilterReso: Float = 0.3f,
    var delayFlutter: Float = 0f,       // 0 = clean digital, >0 = tape character
    var delayDucking: Float = 0f,       // 0 = off, 1 = full duck
    var delayDryWet: Float = 0.5f,

    // Chorus (Airwindows Vibrato — chorus/flanger/vibrato)
    var chorusSpeed: Float = 0.3f,
    var chorusDepth: Float = 0.5f,
    var chorusFMSpeed: Float = 0f,
    var chorusFMDepth: Float = 0f,
    var chorusInvWet: Float = 0.7f     // <0.5 = flanger, ~0.7 = chorus, 1.0 = vibrato
)
```

Add to existing `Instrument` class:
```kotlin
@Serializable
data class Instrument(
    // ... existing fields ...
    var fx: InstrumentFX = InstrumentFX()
)
```

Add to existing `Project` class:
```kotlin
@Serializable
data class Project(
    // ... existing fields ...
    var sendFX: SendFX = SendFX(),
    var masterFX: MasterFX = MasterFX()
)
```

---

## Phase 2: Integrate Airwindows Effects

### 2.1 Create AirwinBase.h

Minimal base class that replaces VST2's `AudioEffectX`. See code above in
"Airwindows Integration Pattern" section.

### 2.2 Extract 7 Airwindows Effects

From `https://github.com/airwindows/airwindows`, branch `master`,
directory `plugins/LinuxVST/src/`:

| Effect | Dir Name | Params | Use |
|--------|----------|--------|-----|
| Chamber | `Chamber/source/` | 5 | Send reverb |
| TapeDelay2 | `TapeDelay2/source/` | 6 | Send delay (+ custom ducking) |
| Vibrato | `Vibrato/source/` | 5 | Send chorus/flanger |
| Mojo | `Mojo/source/` | 1 | Per-instrument drive |
| DeRez2 | `DeRez2/source/` | 4 | Per-instrument bitcrush |
| Pressure4 | `Pressure4/source/` | 4 | Master compressor |
| ADClip7 | `ADClip7/source/` | 4 | Master limiter |

For each:
1. Copy `.h`, `.cpp`, `Proc.cpp`
2. Wrap in `namespace AW_Chamber { ... }` etc.
3. Replace `#include "audioeffectx.h"` → `#include "../AirwinBase.h"`
4. Replace `AudioEffectX` base class → `AirwinBase`
5. Comment out `createEffectInstance` function
6. Verify it compiles standalone

### 2.3 Write an Extraction Script

Create `scripts/grab_airwindows.py` (adapted from Surge's approach):
```python
# Usage: python grab_airwindows.py /path/to/airwindows/repo
# Copies and adapts specified effects into app/src/main/cpp/effects/airwindows/
```

This makes it easy to update effects from upstream or add new ones later.

---

## Phase 3: Integrate Soundpipe pareq + DaisySP SVF

### 3.1 Soundpipe pareq

Copy from `https://github.com/PaulBatchelor/Soundpipe`:
- `modules/pareq.c` → `effects/soundpipe/pareq.c`
- Create minimal `sp_utils.h` with just `sp_data` struct (sample rate holder)

The `pareq` module is self-contained. Interface:
```c
int sp_pareq_create(sp_pareq **p);
int sp_pareq_init(sp_data *sp, sp_pareq *p);
int sp_pareq_compute(sp_data *sp, sp_pareq *p, float *in, float *out);
// Parameters: p->fc (center freq), p->v (gain), p->q (Q width), p->mode (0=peak,1=loshelf,2=hishelf)
```

### 3.2 DaisySP SVF

Copy from `https://github.com/electro-smith/DaisySP`:
- `Source/Filters/svf.h` → `effects/daisysp/svf.h`
- `Source/Filters/svf.cpp` → `effects/daisysp/svf.cpp`

Interface:
```cpp
daisysp::Svf filter;
filter.Init(sampleRate);
filter.SetFreq(cutoff);
filter.SetRes(resonance);
filter.SetDrive(drive);
filter.Process(inputSample);
float lpOut = filter.Low();
float hpOut = filter.High();
float bpOut = filter.Band();
```

All 5 outputs (LP/HP/BP/Notch/Peak) available simultaneously from one `Process()` call.

### 3.3 Future Filter Expansion

For a future update, consider adding **Soundpipe sp_moogladder** as a second
filter type ("LADDER" mode alongside "SVF") for fat analog character with
self-oscillating resonance. The SVF covers clean/versatile needs; the ladder
covers classic acid/DnB sounds.

---

## Phase 4: Effects Processing in onAudioReady

### 4.1 EffectsChain Class

```cpp
class EffectsChain {
public:
    void init(float sampleRate);
    void updateParams(/* params from JNI */);

    // Per-instrument effects (called per-track after voice mixing)
    void processInstrumentFX(int instrumentId, float* buffer, int numFrames);

    // Send buses
    void processReverbSend(float** in, float** out, int numFrames);
    void processDelaySend(float** in, float** out, int numFrames);
    void processChorusSend(float** in, float** out, int numFrames);

    // Master chain
    void processMasterCompressor(float** buffer, int numFrames);
    void processMasterEQ(float** buffer, int numFrames);
    void processMasterLimiter(float** buffer, int numFrames);

private:
    // Per-instrument (8 tracks worth)
    daisysp::Svf filters[8];
    sp_pareq instEQ[8][3];                   // 3 bands per track
    AW_Mojo::Mojo* mojo[8];
    AW_DeRez2::DeRez2* derez[8];

    // Send buses
    AW_Chamber::Chamber* reverb;
    AW_TapeDelay2::TapeDelay2* delay;
    AW_Vibrato::Vibrato* chorus;

    // Master
    AW_Pressure4::Pressure4* compressor;
    sp_pareq masterEQ[3];
    AW_ADClip7::ADClip7* limiter;

    // Ducking envelope follower for delay
    float duckEnvelope = 0.0f;
};
```

### 4.2 Modified onAudioReady Flow

```cpp
oboe::DataCallbackResult onAudioReady(
        oboe::AudioStream *audioStream,
        void *audioData,
        int32_t numFrames) override {

    float *output = static_cast<float*>(audioData);
    int channelCount = audioStream->getChannelCount();

    // ---- Step 1: Clear all busses ----
    busses.clear(numFrames);

    // ---- Step 2: Mix voices into per-track buffers ----
    for (int v = 0; v < MAX_VOICES; v++) {
        Voice& voice = voices[v];
        if (!voice.isActive) continue;

        for (int i = 0; i < numFrames; i++) {
            int idx = (int)voice.position;
            if (idx >= voice.sampleLength - 1) {
                voice.isActive = false;
                break;
            }
            float sample = voice.sampleData[idx] * voice.volume;
            busses.trackBuffers[voice.trackId][i] += sample;
            voice.position += voice.playbackRate;
        }
    }

    // ---- Step 3: Per-instrument effects on each track ----
    for (int t = 0; t < 8; t++) {
        int instrumentId = trackInstruments[t]; // Tracked during note trigger
        effectsChain.processInstrumentFX(instrumentId,
                                         busses.trackBuffers[t], numFrames);

        // Get send levels for this instrument
        float revSend = instrumentSendLevels[instrumentId].reverb;
        float dlySend = instrumentSendLevels[instrumentId].delay;
        float chsSend = instrumentSendLevels[instrumentId].chorus;

        for (int i = 0; i < numFrames; i++) {
            float s = busses.trackBuffers[t][i];

            // Pan to stereo dry bus
            float panL = /* from instrument pan */ 1.0f;
            float panR = 1.0f;
            busses.dryBus[0][i] += s * panL * 0.125f; // /8 tracks
            busses.dryBus[1][i] += s * panR * 0.125f;

            // Tap to send buses
            busses.reverbSend[0][i] += s * revSend * 0.125f;
            busses.reverbSend[1][i] += s * revSend * 0.125f;
            busses.delaySend[0][i]  += s * dlySend * 0.125f;
            busses.delaySend[1][i]  += s * dlySend * 0.125f;
            busses.chorusSend[0][i] += s * chsSend * 0.125f;
            busses.chorusSend[1][i] += s * chsSend * 0.125f;
        }
    }

    // ---- Step 4: Process send buses ----
    effectsChain.processReverbSend(busses.reverbSend,
                                    busses.reverbReturn, numFrames);
    effectsChain.processDelaySend(busses.delaySend,
                                   busses.delayReturn, numFrames);
    effectsChain.processChorusSend(busses.chorusSend,
                                    busses.chorusReturn, numFrames);

    // ---- Step 5: Sum dry + wet returns into master bus ----
    for (int i = 0; i < numFrames; i++) {
        for (int ch = 0; ch < 2; ch++) {
            busses.masterBus[ch][i] = busses.dryBus[ch][i]
                                    + busses.reverbReturn[ch][i]
                                    + busses.delayReturn[ch][i]
                                    + busses.chorusReturn[ch][i];
        }
    }

    // ---- Step 6: Master chain ----
    effectsChain.processMasterCompressor(busses.masterBus, numFrames);
    effectsChain.processMasterEQ(busses.masterBus, numFrames);
    effectsChain.processMasterLimiter(busses.masterBus, numFrames);

    // ---- Step 7: Write to output ----
    for (int i = 0; i < numFrames; i++) {
        output[i * channelCount]     = busses.masterBus[0][i];
        output[i * channelCount + 1] = busses.masterBus[1][i];
    }

    // ---- Step 8: Feed analysis engine ----
    analysisEngine.feed(busses.masterBus, numFrames);

    return oboe::DataCallbackResult::Continue;
}
```

### 4.3 Custom Ducking for TapeDelay2

Add after TapeDelay2 processing in `processDelaySend`:

```cpp
void processDelaySend(float** in, float** out, int numFrames) {
    // TapeDelay2 processes the send input
    delay->processReplacing(in, out, numFrames);

    // Apply ducking: envelope follows dry bus level
    float duckAmount = params.delayDucking;
    if (duckAmount > 0.0f) {
        float attack = 0.002f;   // ~2ms
        float release = 0.02f;   // ~20ms
        for (int i = 0; i < numFrames; i++) {
            // Follow dry bus amplitude
            float dryLevel = fabsf(busses.dryBus[0][i]) + fabsf(busses.dryBus[1][i]);
            float speed = (dryLevel > duckEnvelope) ? attack : release;
            duckEnvelope += (dryLevel - duckEnvelope) * speed;
            // Attenuate wet signal
            float duckGain = 1.0f - (duckAmount * fminf(duckEnvelope * 4.0f, 1.0f));
            out[0][i] *= duckGain;
            out[1][i] *= duckGain;
        }
    }
}
```

---

## Phase 5: JNI Bridge

### 5.1 New JNI Methods

```cpp
// Set per-instrument effect parameters
JNIEXPORT void JNICALL native_setInstrumentFX(
    JNIEnv*, jobject, jint instrumentId, jfloatArray params);

// Set send effect parameters
JNIEXPORT void JNICALL native_setSendFX(
    JNIEnv*, jobject, jfloatArray reverbParams,
    jfloatArray delayParams, jfloatArray chorusParams);

// Set master effect parameters
JNIEXPORT void JNICALL native_setMasterFX(
    JNIEnv*, jobject, jfloatArray compParams,
    jfloatArray eqParams, jfloatArray limiterParams);

// Get analysis data (peak levels, RMS, spectrum, GR)
JNIEXPORT jfloatArray JNICALL native_getAnalysisData(
    JNIEnv*, jobject);

// Get per-track peak levels for mixer meters
JNIEXPORT jfloatArray JNICALL native_getTrackLevels(
    JNIEnv*, jobject);
```

### 5.2 Thread Safety

All parameter updates use `std::atomic<float>` or a lock-free parameter
ring buffer. The audio callback never blocks.

```cpp
struct AtomicParams {
    // Per-instrument (simplified — use array of atomics or double-buffer)
    std::atomic<float> filterCutoff[8];
    std::atomic<float> filterResonance[8];
    // ... etc

    // Send FX
    std::atomic<float> reverbParams[5];
    // ... etc
};
```

For complex param sets (many values that must update atomically together),
use a **double-buffer pattern**: JNI writes to buffer A, then atomically swaps
a pointer so the audio thread reads buffer A on next callback.

---

## Phase 6: Visualization

### 6.1 KissFFT Integration

Add as source files (not submodule — only 2 files needed):
- `kiss_fft.c` / `kiss_fft.h` — core FFT
- `kiss_fftr.c` / `kiss_fftr.h` — real-valued FFT wrapper

### 6.2 AnalysisEngine

```cpp
class AnalysisEngine {
public:
    void init(float sampleRate);

    // Called from audio thread — just copies data, no heavy processing
    void feed(float** stereoBuffer, int numFrames);

    // Called from UI thread (via JNI) — runs FFT, computes levels
    void computeSpectrum(float* magnitudes, int numBins);
    void getTrackPeaks(float* peaks);    // 8 track peak levels
    void getMasterPeak(float* peakL, float* peakR);
    void getMasterRMS(float* rmsL, float* rmsR);
    float getLimiterGR();                 // Gain reduction in dB

private:
    // Ring buffer for spectrum analysis
    static const int FFT_SIZE = 512;
    float ringBuffer[FFT_SIZE];
    std::atomic<int> writePos{0};

    // Peak/RMS tracking (updated per callback)
    std::atomic<float> trackPeaks[8];
    std::atomic<float> masterPeakL{0}, masterPeakR{0};
    std::atomic<float> masterRmsL{0}, masterRmsR{0};
    std::atomic<float> limiterGR{0};

    // FFT state (used only from UI thread)
    kiss_fftr_cfg fftCfg;
    float fftInput[FFT_SIZE];
    kiss_fft_cpx fftOutput[FFT_SIZE / 2 + 1];
    float window[FFT_SIZE]; // Hann window
};
```

### 6.3 Kotlin-Side Analysis

```kotlin
class AudioAnalysis(private val engine: TrackerAudioEngine) {
    // Spectrum data (256 bins = FFT_SIZE/2)
    val spectrumBins = FloatArray(256)

    // Per-track peak levels
    val trackPeaks = FloatArray(8)

    // Master metering
    var masterPeakL = 0f
    var masterPeakR = 0f
    var limiterGR = 0f

    fun update() {
        // Called at ~30fps from UI
        engine.getAnalysisData(spectrumBins, trackPeaks, ...)
    }
}
```

### 6.4 Visualization Rendering

The OscilloscopeModule (620x70px) gets a mode toggle:
- **Scope mode** (existing): Waveform display
- **Spectrum mode** (new): FFT magnitude bars, 256 bins mapped to 620px
- **Both modes** should use the same color palette and visual style

Mixer peak meters: vertical bars per track, using `trackPeaks[]` data.
Limiter GR meter: inverted bar (fills downward as gain reduction increases).

All rendered on the existing pixel-perfect 640x480 canvas.

---

## Phase 7: CMake Build Integration

### 7.1 Updated CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.22.1)
project("../../../../Games/PocketTracker")

find_package(oboe REQUIRED CONFIG)

# Collect all effect source files
file(GLOB AIRWINDOWS_SOURCES
        "effects/airwindows/*.cpp"
)

add_library(pockettracker SHARED
        native-audio.cpp
        effects/soundpipe/pareq.c
        effects/daisysp/svf.cpp
        effects/EffectsChain.cpp
        effects/AnalysisEngine.cpp
        kissfft/kiss_fft.c
        kissfft/kiss_fftr.c
        ${AIRWINDOWS_SOURCES}
)

target_include_directories(pockettracker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/effects
        ${CMAKE_CURRENT_SOURCE_DIR}/kissfft
)

# Enable flush-to-zero for ARM (prevents denormal slowdown)
if (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm")
    target_compile_options(pockettracker PRIVATE -ffast-math)
endif ()

target_link_libraries(pockettracker
        oboe::oboe
        android
        log
)
```

---

## Implementation Order

### Step 1: Scaffolding (est. complexity: medium)
- [ ] Create `effects/` directory structure
- [ ] Write `AirwinBase.h`
- [ ] Write extraction script `scripts/grab_airwindows.py`
- [ ] Extract 7 Airwindows effects, verify they compile
- [ ] Copy Soundpipe `pareq` and DaisySP `Svf`, verify compilation
- [ ] Update CMakeLists.txt
- [ ] Verify full project builds

### Step 2: Bus Architecture (est. complexity: high)
- [ ] Add `AudioBusses` struct with scratch buffers
- [ ] Refactor `onAudioReady` to mix voices into per-track buffers
- [ ] Add stereo panning from instrument settings
- [ ] Add send level taps from per-track buffers to send buses
- [ ] Sum dry + wet returns into master bus
- [ ] Verify: audio still works, no artifacts, same sound as before (all FX bypassed)

### Step 3: Per-Instrument Effects (est. complexity: medium)
- [ ] Integrate DaisySP SVF filter per track
- [ ] Integrate Soundpipe pareq (3 instances per track = 3-band EQ)
- [ ] Integrate Airwindows Mojo per track
- [ ] Integrate Airwindows DeRez2 per track
- [ ] Add JNI method: `native_setInstrumentFX`
- [ ] Add Kotlin data classes: `InstrumentFX`
- [ ] Verify: each effect works independently, bypass when params at zero

### Step 4: Send Bus Effects (est. complexity: medium)
- [ ] Integrate Airwindows Chamber (reverb send)
- [ ] Integrate Airwindows TapeDelay2 (delay send)
- [ ] Add custom ducking to delay
- [ ] Integrate Airwindows Vibrato (chorus send)
- [ ] Add JNI method: `native_setSendFX`
- [ ] Add Kotlin data classes: `SendFX`
- [ ] Verify: send levels route correctly, wet/dry mix works

### Step 5: Master Chain (est. complexity: medium)
- [ ] Integrate Airwindows Pressure4 (parallel compressor)
- [ ] Integrate Soundpipe pareq × 3 (master EQ)
- [ ] Integrate Airwindows ADClip7 (limiter)
- [ ] Add JNI method: `native_setMasterFX`
- [ ] Add Kotlin data class: `MasterFX`
- [ ] Verify: master chain processes correctly, limiter prevents clipping

### Step 6: Analysis & Visualization Data (est. complexity: medium)
- [ ] Add KissFFT source files
- [ ] Implement AnalysisEngine (ring buffer, FFT, peak/RMS tracking)
- [ ] Add per-track peak level tracking in audio callback
- [ ] Add limiter gain reduction tracking
- [ ] Add JNI methods: `native_getAnalysisData`, `native_getTrackLevels`
- [ ] Add Kotlin `AudioAnalysis` class
- [ ] Verify: data flows to Kotlin correctly at 30fps

### Step 7: Parameter Thread Safety (est. complexity: low)
- [ ] Implement double-buffer pattern for complex param updates
- [ ] Use `std::atomic<float>` for simple real-time params
- [ ] Verify: no audio glitches during rapid parameter changes

---

## Resource Budget

### RAM (worst case, all effects active)

| Component | RAM |
|-----------|-----|
| Track buffers (8 × 1024 floats) | 32 KB |
| Bus buffers (14 × 1024 floats) | 56 KB |
| Chamber reverb (internal delay lines) | ~80 KB |
| TapeDelay2 (1 sec stereo @ 48kHz) | ~384 KB |
| Vibrato chorus (short delay) | ~8 KB |
| 8× SVF filter state | <1 KB |
| 8× 3-band pareq state | <1 KB |
| 8× Mojo state | <1 KB |
| 8× DeRez2 state | <1 KB |
| Pressure4 state | <1 KB |
| 3× Master pareq state | <1 KB |
| ADClip7 state | <1 KB |
| KissFFT (512-point) | ~4 KB |
| Analysis ring buffers | ~8 KB |
| **Total** | **~575 KB** |

Fits comfortably in 128MB (Miyoo Mini Plus). The delay buffer dominates —
reducing max delay time to 500ms halves it to ~192KB.

### CPU (per audio callback, 480 frames @ 48kHz)

| Component | Cortex-A7 (est.) | Cortex-A55 (est.) |
|-----------|-------------------|---------------------|
| Voice mixing (8 voices) | 0.3% | 0.1% |
| 8× SVF filter | 0.2% | 0.08% |
| 8× 3-band EQ | 0.3% | 0.12% |
| 8× Mojo | 0.1% | 0.04% |
| 8× DeRez2 | 0.1% | 0.04% |
| Chamber reverb | 1.8% | 0.7% |
| TapeDelay2 | 0.3% | 0.1% |
| Vibrato chorus | 0.4% | 0.15% |
| Pressure4 | 0.05% | 0.02% |
| Master EQ (3-band) | 0.1% | 0.04% |
| ADClip7 | 0.05% | 0.02% |
| FFT (512-pt, 30fps) | 0.6% | 0.2% |
| **Total** | **~4.3%** | **~1.6%** |

Well within budget even on Cortex-A7. The reverb is the heaviest single
effect. If CPU becomes an issue, Chamber can be replaced with Freeverb
(~1.0% on A7) at the cost of some sound quality.

### Important: `-ffast-math` and Denormals

Always compile with `-ffast-math` for ARM targets. This enables
flush-to-zero mode which prevents denormal float values from causing
10-100× slowdowns in feedback loops (reverb, delay, filters).

---

## Open Questions / Future Work

1. **Chorus slot**: Airwindows Vibrato (versatile but 5 params) vs simpler
   Chorus (3 params). Decision deferred — evaluate during implementation.

2. **Additional filter types**: Soundpipe `sp_moogladder` as a "LADDER"
   option alongside DaisySP SVF. Low priority — SVF covers most needs.

3. **Multiband OTT compressor**: Requires 3-band crossover + 3× Pressure4.
   Planned for a future extension, not in this phase.

4. **BPM-synced delay**: Map delay time parameter to musical divisions
   (1/4, 1/8, 1/8T, 1/16, etc.) based on project tempo. Should be
   implemented alongside the delay effect.

5. **Ping-pong delay**: Alternate L/R feedback in TapeDelay2. Small
   modification, decision pending.

6. **EQ spectrum visualization**: KissFFT provides magnitude data.
   Rendering is a UI task for a future phase. The data pipeline is
   built in this plan.

7. **Mixer peak meters visual style**: The analysis engine provides peak/RMS
   data per track. Visual rendering is a UI task for a future phase.
