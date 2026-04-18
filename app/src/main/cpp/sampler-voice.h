#pragma once
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include "mod-system.h"
#include "filter.h"

struct Voice : public IAudioVoice {
    bool isActive;
    int fadeInRemaining;     // Anti-click: counts down from DECLICK_SAMPLES to 0 at note start
    float* sampleData;
    int sampleLength;
    float position;
    int trackId;
    float playbackRate;
    float basePlaybackRate;  // Original rate without table transpose
    float volume;
    float panLeft;           // Left channel gain (0.0-1.0)
    float panRight;          // Right channel gain (0.0-1.0)

    // Playback parameters (calculated from instrument params)
    int actualStart;     // Actual sample index to start from
    int actualEnd;       // Actual sample index to end at
    int actualLoopStart; // Actual sample index to loop from
    bool reverse;        // Play backwards
    int loopMode;        // 0=off, 1=forward, 2=ping-pong
    bool loopingBack;    // For ping-pong mode direction

    // Distortion/bitcrusher parameters
    int drive;           // 0-255 (pre-gain boost)
    int crush;           // 0-15 (bit depth reduction, 0=off, 15=1-bit)
    int downsample;      // 0-15 (sample rate reduction factor)

    // Filter parameters
    int filterType;      // 0=off, 1=lp, 2=hp, 3=bp
    int filterCut;       // 0-255 (cutoff frequency)
    int filterRes;       // 0-255 (resonance)

    // Biquad filter state (for resonant filters)
    float b0, b1, b2;    // Feedforward coefficients
    float a1, a2;        // Feedback coefficients (a0 is normalized to 1)
    float x1, x2;        // Input history
    float y1, y2;        // Output history

    // Table state (Phase 3.5)
    int tableId;             // -1 = no table, 0-255 = table ID
    int tableRow;            // Current table row (0-15)
    int lastProcessedRow;    // Last row that had effects processed (-1 = none)
    int tableTicRate;        // Ticks per table row advance (special: 0=trigger, FC=octave, FE=note, FF=200Hz)
    int tableTicCounter;     // Counter for tic-based advance
    float tableTranspose;    // Current transpose from table (semitones)
    float tableVolume;       // Current volume multiplier from table (0.0-1.0)

    // Note identity (used by note monitor to show playing note even across empty phrases)
    int noteOctave;          // Octave of the triggered note (0-9), -1 = none
    int notePitch;           // Pitch of the triggered note (0-11, C=0)

    // Special TIC mode support (Phase 4)
    int triggerOctave;       // Octave of triggered note (0-9) for TICFC mode
    int triggerPitch;        // Pitch of triggered note (0-11, C=0) for TICFE mode
    float tic200HzAccum;     // Accumulator for 200Hz mode (TICFF)

    // HOP repeat counter (Phase 5)
    // HOP XY: X = repeat count (0 = infinite), Y = target row
    int hopRepeatCount;      // Number of times left to jump (0 = done or infinite mode)
    int hopTargetRow;        // Target row for active HOP (-1 = no active HOP)

    // ===================================
    // PITCH MODULATION (Phase 6)
    // ===================================
    // Real-time pitch modulation for PSL, PBN, PVB, PVX effects

    // Pitch slide state
    float pitchOffset;           // Current semitones offset from base pitch (can be fractional)
    float pitchSlideTarget;      // Target semitones for pitch slide (PSL effect)
    float pitchSlideRate;        // Semitones per sample (for smooth interpolation)
    bool pitchSliding;           // Whether pitch slide is active

    // Vibrato state (sine wave LFO modulation)
    float vibratoPhase;          // Current LFO phase (0 to 2π)
    float vibratoSpeed;          // LFO frequency in Hz (2-20 Hz typical)
    float vibratoDepth;          // Modulation depth in semitones (0-2 typical, up to 8 for PVX)
    bool vibratoActive;          // Whether vibrato is active

    // ===================================
    // MODULATION STATE (Phase 4 / Phase 5)
    // ===================================
    // VoiceModSlot, voiceMods[4], modSourceValues[], modDestValues[], prevModDestValues[]
    // are in IAudioVoice (mod-system.h) so updateVoiceModulation() runs for all voice types.
    // modPitchOffset, basePan, modPanOffset, modCutOffset, modResOffset, baseVolume removed.
    // Volume flows through params.base[PARAM_VOL] + VOL route → modDestValues[PARAM_VOL].

    // Static note-on sources (captured at trigger, constant for note's lifetime)
    float noteVelocity = 0.0f;  // 0.0–1.0 (note volume proxies velocity)
    float noteKeytrack = 0.0f;  // (midiNote − 60) / 12.0, bipolar
    float noteRandom   = 0.0f;  // random 0.0–1.0

    // Voice-steal fade-out: instead of a hard cut, fade over DECLICK_SAMPLES frames
    int fadeOutRemaining;  // Counts down from DECLICK_SAMPLES to 0 during fade-out
    bool isFadingOut;      // true while the voice-steal fade-out is active

    Voice() : isActive(false), fadeInRemaining(0), sampleData(nullptr), sampleLength(0),
              position(0), trackId(-1), playbackRate(1.0f), basePlaybackRate(1.0f), volume(1.0f),
              panLeft(0.707f), panRight(0.707f),  // Default to center
              actualStart(0), actualEnd(0), actualLoopStart(0),
              reverse(false), loopMode(0), loopingBack(false),
              drive(0), crush(0), downsample(0),
              filterType(0), filterCut(128), filterRes(0),
              b0(1.0f), b1(0.0f), b2(0.0f), a1(0.0f), a2(0.0f),
              x1(0.0f), x2(0.0f), y1(0.0f), y2(0.0f),
              tableId(-1), tableRow(0), lastProcessedRow(-1), tableTicRate(6), tableTicCounter(0),
              tableTranspose(0.0f), tableVolume(1.0f),
              noteOctave(-1), notePitch(0),
              triggerOctave(4), triggerPitch(0), tic200HzAccum(0.0f),
              hopRepeatCount(0), hopTargetRow(-1),
              pitchOffset(0.0f), pitchSlideTarget(0.0f), pitchSlideRate(0.0f), pitchSliding(false),
              vibratoPhase(0.0f), vibratoSpeed(0.0f), vibratoDepth(0.0f), vibratoActive(false),
              fadeOutRemaining(0), isFadingOut(false) {}
              // params (ParamBus) is default-constructed: base={1,0.5,0,128,0}, mod={0}

    void trigger(float* sample, int length, int track, float rate, float instrVol, float phraseVol, float pan,
                 const InstrumentParams& instrParams, float sampleRate, int startPointOverride = -1,
                 int tblId = -1, int tblTicRate = 6, int octave = 4, int pitch = 0, int startRow = 0) {
        sampleData = sample;
        sampleLength = length;
        trackId = track;
        playbackRate = rate;
        basePlaybackRate = rate;  // Store original rate for table transpose
        // voice.volume is neutral (1.0) — instrVol lives in params.base[PARAM_VOL],
        // phraseVol lives in modSourceValues[MOD_SRC_PHRASE_VOL].
        // The fixed VOL route multiplies: TABLE_VOL × phraseVol × instrVol.
        volume = 1.0f;

        // Calculate constant-power pan gains
        // pan: 0.0=left, 0.5=center, 1.0=right
        float panAngle = pan * (float)M_PI * 0.5f;  // 0 to π/2
        panLeft = cosf(panAngle);
        panRight = sinf(panAngle);

        // Convert normalized 0-255 values to actual sample positions
        // Use startPointOverride if provided (Offset effect), otherwise use instrument default
        int effectiveStartPoint = (startPointOverride >= 0) ? startPointOverride : instrParams.startPoint;
        actualStart = (effectiveStartPoint * length) / 255;
        actualEnd = (instrParams.endPoint * length) / 255;
        actualLoopStart = (instrParams.loopStart * length) / 255;

        // Clamp to valid range
        actualStart = std::max(0, std::min(actualStart, length - 1));
        actualEnd = std::max(0, std::min(actualEnd, length - 1));
        actualLoopStart = std::max(actualStart, std::min(actualLoopStart, actualEnd));

        // Ensure start < end
        if (actualStart >= actualEnd) {
            actualStart = 0;
            actualEnd = length - 1;
        }

        // Set playback parameters
        reverse = instrParams.reverse;
        loopMode = instrParams.loopMode;
        loopingBack = false;

        // Set distortion/bitcrusher parameters
        drive = instrParams.drive;
        crush = instrParams.crush;
        downsample = instrParams.downsample;

        // Set filter parameters and calculate coefficients
        filterType = instrParams.filterType;
        filterCut = instrParams.filterCut;
        filterRes = instrParams.filterRes;
        calculateBiquadCoeffs(filterType, filterCut, filterRes, sampleRate,
                              b0, b1, b2, a1, a2);

        // Reset filter history (important to avoid clicks)
        x1 = 0.0f; x2 = 0.0f;
        y1 = 0.0f; y2 = 0.0f;

        // Initialize table state (Phase 3.5)
        tableId = tblId;
        tableRow = startRow % 16;  // Use provided start row, wrap to 0-15
        lastProcessedRow = -1;     // Reset so first row gets processed
        tableTicRate = tblTicRate;
        tableTicCounter = 0;
        tableTranspose = 0.0f;
        tableVolume = 1.0f;

        // Reset HOP state (Phase 5)
        hopRepeatCount = 0;
        hopTargetRow = -1;

        // Reset pitch modulation state (Phase 6)
        // New notes clear all pitch effects (PSL, PBN, PVB, PVX)
        pitchOffset = 0.0f;
        pitchSlideTarget = 0.0f;
        pitchSlideRate = 0.0f;
        pitchSliding = false;
        vibratoPhase = 0.0f;
        vibratoSpeed = 0.0f;
        vibratoDepth = 0.0f;
        vibratoActive = false;
        // Initialize ParamBus base values and clear mod accumulators.
        // filterCut/filterRes already set above from instrParams.
        params.setBase(PARAM_VOL,          instrVol);  // instrVol is depth of fixed VOL route
        params.setBase(PARAM_PAN,          pan);
        params.setBase(PARAM_PITCH,        0.0f);
        params.setBase(PARAM_FILTER_CUT,   (float)filterCut);
        params.setBase(PARAM_FILTER_RES,   (float)filterRes);
        params.setBase(PARAM_DRIVE,        (float)instrParams.drive);
        params.setBase(PARAM_CRUSH,        (float)instrParams.crush);
        params.setBase(PARAM_DOWNSAMPLE,   (float)instrParams.downsample);
        params.setBase(PARAM_SAMPLE_START, (float)effectiveStartPoint);
        params.setBase(PARAM_SAMPLE_END,   (float)instrParams.endPoint);
        params.setBase(PARAM_LOOP_START,   (float)instrParams.loopStart);
        params.resetMods();

        // Capture static mod sources at note-on (constant for this note's lifetime).
        noteVelocity  = instrVol;
        int midiNote  = (octave + 1) * 12 + pitch;
        noteKeytrack  = (float)(midiNote - 60) / 12.0f;
        noteRandom    = (float)(rand() & 0xFFFF) / 65535.0f;

        // Clear all source values; then write static ones.
        // Dynamic slots (ENV/LFO) will be written each block by updateVoiceModulation.
        memset(modSourceValues, 0, sizeof(modSourceValues));
        modSourceValues[MOD_SRC_VELOCITY]  = noteVelocity;
        modSourceValues[MOD_SRC_KEYTRACK]  = noteKeytrack;
        modSourceValues[MOD_SRC_RANDOM]    = noteRandom;
        modSourceValues[MOD_SRC_TABLE_VOL]  = 1.0f;  // Default: full volume when no table active
        modSourceValues[MOD_SRC_PHRASE_VOL] = phraseVol;  // Phrase step volume (constant for note's lifetime)
        // TABLE_PITCH, PITCH_SLIDE, VIBRATO start at 0.0f (memset) — correct defaults.
        // MOD_SRC_NONE remains 0.0f — required by processRoutes via=NONE path.

        // Clear destination arrays for this note.
        memset(modDestValues,     0, sizeof(modDestValues));
        memset(prevModDestValues, 0, sizeof(prevModDestValues));

        // Pre-seed PARAM_VOL so the first block's per-sample interpolation starts at the correct
        // value (instrVol × phraseVol) rather than 0, which would conflict with antiClickFade.
        // TABLE_VOL=1.0 at note-on so the initial route output is instrVol × phraseVol × 1.0.
        modDestValues[PARAM_VOL]     = instrVol * phraseVol;
        prevModDestValues[PARAM_VOL] = instrVol * phraseVol;

        // Store note identity for note monitor display and special TIC modes
        noteOctave = std::max(0, std::min(octave, 9));
        notePitch  = std::max(0, std::min(pitch, 11));
        triggerOctave = noteOctave;
        triggerPitch  = notePitch;
        tic200HzAccum = 0.0f;

        // For special TIC modes, set initial table row based on mode
        // These override the startRow parameter
        if (tblTicRate == 0xFC) {
            // TICFC: Octave map - row = octave (clamped to 0-15)
            tableRow = std::min(triggerOctave, 15);
        } else if (tblTicRate == 0xFE) {
            // TICFE: Note map - row = pitch (0-11, wraps to 0-11)
            tableRow = triggerPitch;
        }
        // Note: For TIC00 (trigger mode), startRow is used directly (set by caller)

        // Set initial position based on direction
        // For reverse: start at actualEnd - 1 (not actualEnd) because we need to read idx+1 for interpolation
        if (reverse) {
            position = (float)(actualEnd > actualStart ? actualEnd - 1 : actualStart);
        } else {
            position = (float)actualStart;
        }

        fadeInRemaining = DECLICK_SAMPLES;  // Anti-click fade-in on every new note
        isFadingOut = false;               // Clear any stale fade state from previous use
        fadeOutRemaining = 0;
        isActive = true;
    }

    void stop() {
        isActive = false;
        isFadingOut = false;
        fadeOutRemaining = 0;
    }

    // Begin a smooth fade-out instead of a hard stop (used by voice stealing).
    // Keeps isActive=true so the slot remains reserved during the fade — the new
    // note is normally allocated to a different free slot.
    // trackId is preserved (NOT cleared) so that Step-1 in the voice allocator
    // can recycle this fading slot directly when the same track fires again,
    // preventing voice-count explosion during simultaneous multi-track triggers.
    void startFadeOut() {
        if (isFadingOut) return;  // Already fading — don't restart
        // isActive stays true: slot stays reserved for the duration of the fade
        isFadingOut = true;
        fadeOutRemaining = DECLICK_SAMPLES;
        // trackId intentionally NOT cleared — see allocator Step 1
    }

    // ── IAudioVoice implementation ──────────────────────────────────────────

    bool active()      const override { return isActive; }
    int  getTrackId()  const override { return trackId; }

    void hardStop() override { stop(); }

    void noteOff() override {
        // Transitions ADSR/TRIG modulators to release stage (same as triggerNoteOff path).
        // For voices with no release envelope this is equivalent to hardStop().
        bool hasRelease = false;
        for (int m = 0; m < 4; m++) {
            VoiceModSlot& vmod = voiceMods[m];
            if ((vmod.type == 2 || vmod.type == 5) && vmod.stage == 3) {
                vmod.stage = 4;  // ADSR/TRIG → release
                hasRelease = true;
            }
        }
        if (!hasRelease) stop();
    }

    void setVolume(float v) override { volume = v; params.setBase(PARAM_VOL, v); }

    void setPan(float pan) override {
        params.setBase(PARAM_PAN, pan);
        float angle = pan * (float)M_PI * 0.5f;
        panLeft  = cosf(angle);
        panRight = sinf(angle);
    }

    void retrigger(int startPoint) override {
        if (!isActive || !sampleData) return;
        if (startPoint >= 0 && startPoint <= 255 && sampleLength > 0) {
            position = (float)((startPoint * sampleLength) / 255);
            position = fmaxf((float)actualStart, fminf(position, (float)(actualEnd - 1)));
        } else {
            position = (float)actualStart;
        }
        fadeInRemaining = DECLICK_SAMPLES;
    }

    void setMidiNote(int midiNote) override {
        // Convert MIDI note to playback rate relative to base frequency.
        // basePlaybackRate was set at trigger time for the original note.
        // New rate = basePlaybackRate × 2^((newMidi - originalMidi) / 12).
        // We approximate originalMidi from noteOctave/notePitch.
        int originalMidi = (noteOctave + 1) * 12 + notePitch;
        float semitones = (float)(midiNote - originalMidi);
        playbackRate = basePlaybackRate * powf(2.0f, semitones / 12.0f);
    }

    // render() is intentionally not implemented on Voice — the mixer loop in
    // processAudioBlock handles Voice rendering inline for cache efficiency.
    // SoundfontVoice (Phase 2) will implement render() fully.
    float render(float* /*buf*/, int /*numFrames*/) override { return 0.0f; }

    // ── Pitch effect interface (IAudioVoice) ────────────────────────────────
    void setPitchSlideRaw(float targetSemitones, float totalFrames) override {
        float delta = targetSemitones - pitchOffset;
        pitchSlideTarget = targetSemitones;
        pitchSlideRate   = delta / totalFrames;
        pitchSliding     = true;
    }
    void setPitchBendRaw(float ratePerFrame) override {
        if (fabsf(ratePerFrame) < 0.000001f) {
            pitchSliding   = false;
            pitchSlideRate = 0.0f;
        } else {
            pitchSlideRate   = ratePerFrame;
            pitchSlideTarget = (ratePerFrame > 0) ? 127.0f : -127.0f;
            pitchSliding     = true;
        }
    }
    void setVibratoRaw(float speed, float depth) override {
        if (depth < 0.01f) {
            vibratoActive = false;
            vibratoDepth  = 0.0f;
        } else {
            vibratoSpeed  = speed;
            vibratoDepth  = depth;
            vibratoActive = true;
        }
    }
    void clearPitchMod() override {
        pitchOffset    = 0.0f;
        pitchSliding   = false;
        pitchSlideRate = 0.0f;
        vibratoActive  = false;
        vibratoDepth   = 0.0f;
    }
    void setInitialPitchOffset(float semitones) override { pitchOffset = semitones; }

    // ── end IAudioVoice ─────────────────────────────────────────────────────

    // Returns a [0..1] fade multiplier and advances fadeInRemaining.
    // Call once per output sample in the mix loop to eliminate clicks.
    float antiClickFade() {
        float fade = 1.0f;
        if (fadeInRemaining > 0) {
            fade = 1.0f - (float)fadeInRemaining / (float)DECLICK_SAMPLES;
            fadeInRemaining--;
        }
        if (loopMode == 0) {
            float remaining = reverse
                ? (position - (float)actualStart)
                : ((float)actualEnd - position);
            if (remaining >= 0.0f && remaining < (float)DECLICK_SAMPLES)
                fade *= remaining / (float)DECLICK_SAMPLES;
        }
        return fade;
    }
};
