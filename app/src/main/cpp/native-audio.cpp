#include <jni.h>
#include <oboe/Oboe.h>
#include <android/log.h>
#include <vector>
#include <queue>
#include <mutex>
#include <atomic>
#include <cmath>
#include <string>
#include <cstring>

// TinySoundFont — single-header SF2/SF3 renderer (MIT license)
// NOTE: TSF_IMPLEMENTATION must be defined in exactly one .cpp file
#define TSF_IMPLEMENTATION
#include "tsf.h"

#define LOG_TAG "NativeAudio"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

const int MAX_VOICES = 8;  // Reduced for testing
const int DECLICK_SAMPLES = 64;  // ~1.45ms anti-click fade at 44100Hz

// ===================================
// EFFECT TYPE CONSTANTS (must match EffectProcessor.kt)
// ===================================
const int FX_NONE = 0x00;
const int FX_ARC = 0x03;       // Cxx - Arpeggio Config
const int FX_HOP = 0x08;       // Hxx - Table hop (repeat-count jump, FF = stop table)
const int FX_TIC = 0x09;       // Txx - Table tick rate (01-FB = tics/row, FC-FF = special modes)
const int FX_ARPEGGIO = 0x0A;  // Axx - Arpeggio
const int FX_KILL = 0x0B;      // K00 - Kill voice
const int FX_OFFSET = 0x0F;    // Oxx - Sample offset
const int FX_REPEAT = 0x12;    // Rxx - Retrigger
const int FX_THO = 0x15;       // THO 0X - Table hop to row X (simple unconditional jump)
const int FX_VOLUME = 0x16;    // Vxx - Volume

// ===================================
// BIQUAD FILTER COEFFICIENT CALCULATION
// ===================================
// Calculates resonant low-pass, high-pass, and band-pass filter coefficients
// Using Robert Bristow-Johnson's Audio EQ Cookbook formulas

inline void calculateBiquadCoeffs(
        int filterType,     // 0=off, 1=lp, 2=hp, 3=bp
        int cutParam,       // 0-255 (cutoff frequency parameter)
        int resParam,       // 0-255 (resonance parameter)
        float sampleRate,   // Audio sample rate (e.g., 44100 Hz)
        float& b0, float& b1, float& b2,  // Output: feedforward coefficients
        float& a1, float& a2              // Output: feedback coefficients
) {
    if (filterType == 0) {
        // Filter off: pass-through (unity gain)
        b0 = 1.0f; b1 = 0.0f; b2 = 0.0f;
        a1 = 0.0f; a2 = 0.0f;
        return;
    }

    // Map cutoff parameter (0-255) to frequency (20 Hz - 20 kHz)
    // Use exponential curve for musical feel
    float cutoff = 20.0f * powf(1000.0f, cutParam / 255.0f);  // 20 Hz to 20 kHz
    cutoff = fminf(cutoff, sampleRate * 0.45f);  // Limit to below Nyquist

    // Map resonance parameter (0-255) to Q factor (0.5 - 20.0)
    // Higher Q = sharper resonance peak
    float Q = 0.5f + (resParam / 255.0f) * 19.5f;  // 0.5 to 20.0

    // Calculate intermediate values
    float w0 = 2.0f * M_PI * cutoff / sampleRate;  // Angular frequency
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    float alpha = sinw0 / (2.0f * Q);  // Bandwidth parameter

    // Calculate coefficients based on filter type
    float a0;  // Normalization coefficient

    if (filterType == 1) {
        // LOW-PASS filter
        b0 = (1.0f - cosw0) / 2.0f;
        b1 = 1.0f - cosw0;
        b2 = (1.0f - cosw0) / 2.0f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cosw0;
        a2 = 1.0f - alpha;
    } else if (filterType == 2) {
        // HIGH-PASS filter
        b0 = (1.0f + cosw0) / 2.0f;
        b1 = -(1.0f + cosw0);
        b2 = (1.0f + cosw0) / 2.0f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cosw0;
        a2 = 1.0f - alpha;
    } else if (filterType == 3) {
        // BAND-PASS filter (constant skirt gain)
        b0 = alpha;
        b1 = 0.0f;
        b2 = -alpha;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cosw0;
        a2 = 1.0f - alpha;
    } else {
        // Unknown type, pass-through
        b0 = 1.0f; b1 = 0.0f; b2 = 0.0f;
        a1 = 0.0f; a2 = 0.0f;
        return;
    }

    // Normalize coefficients by a0
    b0 /= a0;
    b1 /= a0;
    b2 /= a0;
    a1 /= a0;
    a2 /= a0;
}

// ===================================
// SOUNDFONT INFRASTRUCTURE (TinySoundFont)
// ===================================
// Supports up to MAX_SOUNDFONTS simultaneously loaded SF2/SF3 files.
// tsf is NOT thread-safe — each entry has its own mutex.

static const int MAX_SOUNDFONTS = 4;

struct SoundfontEntry {
    tsf* handle = nullptr;
    std::mutex mutex;            // Protects all note_on/note_off/render calls on master handle
    int instrumentId = -1;       // Which Instrument slot owns this (-1 = free)
    std::string filePath;
    std::vector<uint8_t> fileData; // Raw SF2 bytes kept for per-track clone creation
};

static SoundfontEntry soundfonts[MAX_SOUNDFONTS];

// ===================================
// PHASE 1: NOTE QUEUE INFRASTRUCTURE
// ===================================
// Sample-accurate note scheduling system
// Notes are scheduled with exact target frame numbers
// Audio callback triggers notes at precise moments

struct ScheduledNote {
    int64_t targetFrame;     // Exact audio frame to trigger this note
    int sampleId;            // Which sample to play (0-255)
    int trackId;             // Which track/voice (0-7)
    float frequency;         // Target playback frequency
    float baseFrequency;     // Sample's base frequency
    float volume;            // Playback volume (0.0-1.0)
    float pan;               // Stereo pan position (0.0=left, 0.5=center, 1.0=right)
    int startPointOverride;  // Optional start point override (-1 = use instrument default)

    // Table parameters (Phase 3.5)
    int tableId;             // Table to use (-1 = no table)
    int tableTicRate;        // Ticks per table row advance (default 6)

    // Note info for special TIC modes (Phase 4)
    int noteOctave;          // Octave of note (0-9) for TICFC mode
    int notePitch;           // Pitch of note (0-11, C=0) for TICFE mode

    // Pitch modulation parameters (Phase 7)
    // These are applied when the note triggers, allowing per-note pitch effects
    float pslInitialOffset;  // PSL: Initial pitch offset in semitones (0 = no PSL)
    float pslDuration;       // PSL: Slide duration in ticks (0 = no slide)
    float pbnRate;           // PBN: Semitones per tick (0 = no bend)
    float vibratoSpeed;      // PVB/PVX: LFO speed in Hz (0 = no vibrato)
    float vibratoDepth;      // PVB/PVX: Depth in semitones (0 = no vibrato)

    // Table start row override (Phase 8 - THO effect from phrase)
    int tableStartRow;       // -1 = default (0 or TIC00 continuity), 0-15 = forced start row

    // SoundFont fields (only used when isSoundfont == true)
    bool isSoundfont = false;   // When true, use tsf path instead of voice pool
    int  sfSlot      = -1;      // Index into soundfonts[] array
    int  midiNote    = 60;      // MIDI note 0-127
    int  midiVelocity = 100;    // MIDI velocity 0-127
    int  sfBank      = 0;       // SF2 bank number (0-127)
    int  sfPreset    = 0;       // SF2 preset number within bank (0-127)

    // For priority queue sorting (earliest frame first)
    bool operator>(const ScheduledNote& other) const {
        return targetFrame > other.targetFrame;
    }
};

// Scheduled kill event (for Kill effect K00, and soft note-off for ADSR release)
struct ScheduledKill {
    int64_t targetFrame;     // Exact audio frame to trigger kill
    int trackId;             // Which track to kill (0-7)
    bool softKill = false;   // false = hard stop, true = soft note-off (triggers ADSR release)

    // For priority queue sorting (earliest frame first)
    bool operator>(const ScheduledKill& other) const {
        return targetFrame > other.targetFrame;
    }
};

// Thread-safe note queue
// Audio callback pops notes, Kotlin thread pushes notes
class NoteQueue {
private:
    // Min-heap: earliest targetFrame is always on top
    std::priority_queue<ScheduledNote, std::vector<ScheduledNote>, std::greater<ScheduledNote>> queue;
    std::mutex mutex;

public:
    // Schedule a note to be played at exact frame
    void schedule(const ScheduledNote& note) {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push(note);
        LOGD("📅 Scheduled note: frame=%lld, sample=%d, track=%d, freq=%.2f",
             (long long)note.targetFrame, note.sampleId, note.trackId, note.frequency);
    }

    // Check if any note should trigger at or before this frame
    bool hasNoteAt(int64_t currentFrame) {
        std::lock_guard<std::mutex> lock(mutex);
        if (queue.empty()) return false;
        return queue.top().targetFrame <= currentFrame;
    }

    // Pop the next note (call only if hasNoteAt returns true)
    ScheduledNote pop() {
        std::lock_guard<std::mutex> lock(mutex);
        ScheduledNote note = queue.top();
        queue.pop();
        return note;
    }

    // Clear all scheduled notes (for stop/reset)
    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        while (!queue.empty()) {
            queue.pop();
        }
        LOGD("🗑️ Note queue cleared");
    }

    // Clear only notes scheduled at or after fromFrame (keeps earlier notes intact)
    void clearFrom(int64_t fromFrame) {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<ScheduledNote> keep;
        while (!queue.empty()) {
            ScheduledNote n = queue.top(); queue.pop();
            if (n.targetFrame < fromFrame) keep.push_back(n);
        }
        for (auto& n : keep) queue.push(n);
    }

    // Get queue size (for debugging)
    size_t size() {
        std::lock_guard<std::mutex> lock(mutex);
        return queue.size();
    }
};

// Thread-safe kill queue (for Kill effect K00)
class KillQueue {
private:
    std::priority_queue<ScheduledKill, std::vector<ScheduledKill>, std::greater<ScheduledKill>> queue;
    std::mutex mutex;

public:
    // Schedule a kill event at exact frame
    void schedule(const ScheduledKill& kill) {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push(kill);
        LOGD("🔪 Scheduled kill: frame=%lld, track=%d", (long long)kill.targetFrame, kill.trackId);
    }

    // Check if any kill should trigger at or before this frame
    bool hasKillAt(int64_t currentFrame) {
        std::lock_guard<std::mutex> lock(mutex);
        if (queue.empty()) return false;
        return queue.top().targetFrame <= currentFrame;
    }

    // Pop the next kill (call only if hasKillAt returns true)
    ScheduledKill pop() {
        std::lock_guard<std::mutex> lock(mutex);
        ScheduledKill kill = queue.top();
        queue.pop();
        return kill;
    }

    // Clear all scheduled kills
    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        while (!queue.empty()) {
            queue.pop();
        }
        LOGD("🗑️ Kill queue cleared");
    }

    // Clear only kills scheduled at or after fromFrame
    void clearFrom(int64_t fromFrame) {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<ScheduledKill> keep;
        while (!queue.empty()) {
            ScheduledKill k = queue.top(); queue.pop();
            if (k.targetFrame < fromFrame) keep.push_back(k);
        }
        for (auto& k : keep) queue.push(k);
    }

    // Get queue size (for debugging)
    size_t size() {
        std::lock_guard<std::mutex> lock(mutex);
        return queue.size();
    }
};

// Instrument playback parameters
struct InstrumentParams {
    int startPoint;     // 0-255 (normalized position)
    int endPoint;       // 0-255 (normalized position)
    bool reverse;       // Play backwards
    int loopMode;       // 0=off, 1=forward, 2=ping-pong
    int loopStart;      // 0-255 (normalized position)

    // Distortion/bitcrusher parameters
    int drive;          // 0-255 (pre-gain boost)
    int crush;          // 0-15 (bit depth reduction, 0=off/16-bit, 15=1-bit)
    int downsample;     // 0-15 (sample rate reduction, 0=off, 1=÷2, 2=÷4, etc.)

    // Filter parameters
    int filterType;     // 0=off, 1=lp, 2=hp, 3=bp
    int filterCut;      // 0-255 (cutoff frequency)
    int filterRes;      // 0-255 (resonance)

    InstrumentParams() : startPoint(0), endPoint(255), reverse(false),
                         loopMode(0), loopStart(0), drive(0), crush(0), downsample(0),
                         filterType(0), filterCut(128), filterRes(0) {}
};

// ===================================
// INSTRUMENT MODULATION PARAMS (Phase 4)
// ===================================
// Per-slot modulation configuration set from Kotlin.
// Copied to VoiceModSlot when a note triggers on that instrument.
struct InstrumentModSlot {
    int type;          // 0=NONE, 1=AHD, 2=ADSR, 3=LFO
    int dest;          // 0=NONE, 1=VOL, 2=PAN, 3=PITCH, 4=FINE_PITCH, 5=CUT, 6=RES, 7=STA, 8=MOD_AMT, 9=MOD_RATE, 10=MOD_BOTH
    float amount;      // Modulation depth 0.0-1.0 (normalised from 00-FF)
    int attackSamples; // Attack duration in audio samples
    int holdSamples;   // Hold duration in audio samples (AHD hold; unused in ADSR)
    int decaySamples;  // Decay duration in audio samples
    float sustainLevel; // ADSR: sustain level 0.0-1.0
    float lfoHz;        // LFO: frequency in Hz
    int oscShape;       // LFO: 0=TRI,1=SIN,2=RMP+,3=RMP-,4=EXP+,5=EXP-,6=SQU+,7=SQU-,8=RND,9=DRNK
    int releaseSamples; // ADSR/TRIG: release duration in audio samples (0 = instant)

    InstrumentModSlot() : type(0), dest(0), amount(0.5f),
                          attackSamples(0), holdSamples(0), decaySamples(0),
                          sustainLevel(0.5f), lfoHz(4.0f), oscShape(0), releaseSamples(0) {}
};

// ===================================
// TABLE DATA STRUCTURES (Phase 3.5)
// ===================================
// Tables are mini-sequencers that run alongside playing voices
// Each table has 16 rows with transpose, volume, and 3 FX columns

struct TableRow {
    int8_t transpose;       // Semitones: 00=0, 01-7F=+1 to +127, 80-FF=-128 to -1
    uint8_t volume;         // 00-FF (FF = no change / pass-through)
    uint8_t fx1Type;        // Effect 1 type (0 = none)
    uint8_t fx1Value;       // Effect 1 value
    uint8_t fx2Type;        // Effect 2 type
    uint8_t fx2Value;       // Effect 2 value
    uint8_t fx3Type;        // Effect 3 type
    uint8_t fx3Value;       // Effect 3 value

    TableRow() : transpose(0), volume(0xFF),
                 fx1Type(0), fx1Value(0),
                 fx2Type(0), fx2Value(0),
                 fx3Type(0), fx3Value(0) {}
};

struct Table {
    TableRow rows[16];      // 16 rows per table
    bool loaded;            // Whether this table has been loaded from Kotlin

    Table() : loaded(false) {
        // Rows initialized by default constructor
    }
};

// Convert unsigned transpose byte to signed semitones
inline int transposeToSemitones(uint8_t transpose) {
    if (transpose < 0x80) {
        return transpose;  // 00-7F = 0 to +127
    } else {
        return transpose - 256;  // 80-FF = -128 to -1
    }
}

// ===================================
// IAUDIOVOICE — unified voice interface (UAA Phase 1)
// ===================================
// All concrete voice types (sampler, soundfont, future synths) implement this.
// The mixer loop and effect helpers operate on IAudioVoice* so they are
// source-agnostic — the same code works for every source type.
class IAudioVoice {
public:
    virtual ~IAudioVoice() = default;

    // True while this slot is producing audio (or fading out).
    virtual bool active() const = 0;

    // Hard-stop: silence immediately, mark slot free.
    virtual void hardStop() = 0;

    // Soft note-off: trigger ADSR release (or hard-stop if no release envelope).
    virtual void noteOff() = 0;

    // Real-time volume override (Vxx effect, table volume column).
    virtual void setVolume(float v) = 0;

    // Real-time pan override (table/mod destination).
    virtual void setPan(float pan) = 0;

    // Retrigger the voice at the given start-point (Repeat effect).
    // startPoint: 0-255 normalised, -1 = use current.
    virtual void retrigger(int startPoint) = 0;

    // Change the pitch to midiNote (Arpeggio effect).
    virtual void setMidiNote(int midiNote) = 0;

    // Pitch effects (PSL/PBN/PVB/PVX) — rate/total-frames already converted from ticks by Kotlin.
    // setPitchSlideRaw: slide from current offset to targetSemitones over totalFrames audio frames.
    virtual void setPitchSlideRaw(float targetSemitones, float totalFrames) = 0;
    // setPitchBendRaw: continuous bend at ratePerFrame semitones/frame; 0 = stop.
    virtual void setPitchBendRaw(float ratePerFrame) = 0;
    // setVibratoRaw: LFO vibrato at speed Hz, depth semitones; depth=0 = stop.
    virtual void setVibratoRaw(float speed, float depth) = 0;
    // clearPitchMod: reset all pitch mod state (offset, slide, vibrato).
    virtual void clearPitchMod() = 0;
    // setInitialPitchOffset: set pitchOffset without starting a slide (PSL setup).
    virtual void setInitialPitchOffset(float semitones) = 0;

    // Render up to numFrames of stereo audio into buf (interleaved L/R).
    // Returns the peak level of this block (used for per-track metering).
    virtual float render(float* buf, int numFrames) = 0;

    // Track assignment (0-7), or -1 if unassigned.
    virtual int getTrackId() const = 0;
};

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
    // MODULATION STATE (Phase 4)
    // ===================================
    struct VoiceModSlot {
        int type;          // 0=NONE, 1=AHD, 2=ADSR, 3=LFO
        int dest;          // 0=NONE, 1=VOL, 3=PITCH
        float amount;      // Depth 0.0-1.0
        // AHD/ADSR stage: 0=idle, 1=attack, 2=hold(AHD)/decay(ADSR), 3=decay(AHD)/sustain(ADSR), 4=done
        // LFO stage: 1=running
        int stage;
        float envValue;    // AHD/ADSR: 0.0-1.0; LFO: -1.0 to +1.0
        int stageCounter;  // Samples elapsed in current stage
        int attackSamples;
        int holdSamples;
        int decaySamples;
        float sustainLevel;  // ADSR: sustain level 0.0-1.0
        float lfoHz;         // LFO: frequency in Hz
        float lfoPhase;      // LFO: current phase (0 to 2π)
        int oscShape;        // LFO: oscillator shape (0=TRI, 1=SIN, ...)
        int releaseSamples;  // ADSR/TRIG: release duration in audio samples
        // dest values: 0=NONE, 1=VOL, 2=PAN, 3=PITCH, 4=FINE_PITCH, 5=CUT, 6=RES, 7=STA, 8=MOD_AMT, 9=MOD_RATE, 10=MOD_BOTH

        // Mod-to-mod: computed each audio callback by updateVoiceModulation
        float effectiveAmt;      // amount × incoming MOD_AMT/BOTH scaling
        float effectiveRateMult; // time/freq multiplier from incoming MOD_RATE/BOTH

        // Per-sample interpolation: snapshot envValue before block advance, interpolate in mix loop
        float prevEnvValue;  // envValue at start of this block (= end of previous block)

        VoiceModSlot() : type(0), dest(0), amount(0.5f), effectiveAmt(0.5f), effectiveRateMult(1.0f),
                         stage(0), envValue(0.0f), prevEnvValue(0.0f), stageCounter(0),
                         attackSamples(0), holdSamples(0), decaySamples(0), releaseSamples(0),
                         sustainLevel(0.5f), lfoHz(4.0f), lfoPhase(0.0f), oscShape(0) {}
    };
    VoiceModSlot voiceMods[4]; // 4 mod slots per voice
    float baseVolume;          // Voice volume before modulation
    float modPitchOffset;      // Accumulated pitch offset from PITCH/FINE_PITCH-destination mods (semitones)
    float basePan;             // Pan position at trigger time (0.0=left, 0.5=center, 1.0=right)
    float modPanOffset;        // Accumulated pan offset from PAN-destination mods (±0.5)
    float modCutOffset;        // Accumulated filter cutoff offset from FILTER_CUTOFF mods (±255)
    float modResOffset;        // Accumulated filter resonance offset from FILTER_RES mods (±255)

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
              baseVolume(1.0f), modPitchOffset(0.0f),
              basePan(0.5f), modPanOffset(0.0f), modCutOffset(0.0f), modResOffset(0.0f),
              fadeOutRemaining(0), isFadingOut(false) {}

    void trigger(float* sample, int length, int track, float rate, float vol, float pan,
                 const InstrumentParams& params, float sampleRate, int startPointOverride = -1,
                 int tblId = -1, int tblTicRate = 6, int octave = 4, int pitch = 0, int startRow = 0) {
        sampleData = sample;
        sampleLength = length;
        trackId = track;
        playbackRate = rate;
        basePlaybackRate = rate;  // Store original rate for table transpose
        volume = vol;

        // Calculate constant-power pan gains
        // pan: 0.0=left, 0.5=center, 1.0=right
        float panAngle = pan * (float)M_PI * 0.5f;  // 0 to π/2
        panLeft = cosf(panAngle);
        panRight = sinf(panAngle);

        // Convert normalized 0-255 values to actual sample positions
        // Use startPointOverride if provided (Offset effect), otherwise use instrument default
        int effectiveStartPoint = (startPointOverride >= 0) ? startPointOverride : params.startPoint;
        actualStart = (effectiveStartPoint * length) / 255;
        actualEnd = (params.endPoint * length) / 255;
        actualLoopStart = (params.loopStart * length) / 255;

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
        reverse = params.reverse;
        loopMode = params.loopMode;
        loopingBack = false;

        // Set distortion/bitcrusher parameters
        drive = params.drive;
        crush = params.crush;
        downsample = params.downsample;

        // Set filter parameters and calculate coefficients
        filterType = params.filterType;
        filterCut = params.filterCut;
        filterRes = params.filterRes;
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
        modPitchOffset = 0.0f;
        basePan = pan;
        modPanOffset = 0.0f;
        modCutOffset = 0.0f;
        modResOffset = 0.0f;

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
            if ((voiceMods[m].type == 2 || voiceMods[m].type == 5) && voiceMods[m].stage == 3) {
                voiceMods[m].stage = 4;  // ADSR/TRIG → release
                hasRelease = true;
            }
        }
        if (!hasRelease) stop();
    }

    void setVolume(float v) override { volume = v; baseVolume = v; }

    void setPan(float pan) override {
        basePan = pan;
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

// ===================================
// SOUNDFONTVOICE — wraps a per-track tsf* clone (UAA Phase 2)
// ===================================
// Each of the 8 tracks gets its own SoundfontVoice with an independent tsf* instance.
// This gives accurate per-track metering and will support per-track effects in Phase 3.
struct SoundfontVoice : public IAudioVoice {
    tsf*       handle       = nullptr;  // Per-track clone (created from SoundfontEntry.fileData)
    std::mutex mutex;                   // Protects handle from concurrent Kotlin-thread access
    bool       isActive     = false;
    int        _trackId     = -1;
    int        sourceSfSlot = -1;       // Which soundfonts[] entry was used to create handle
    int        activeNote   = -1;       // Currently playing MIDI note (-1 = none)
    float      noteVolume   = 1.0f;     // Note-level volume (used for real-time vol updates)

    // Pitch effect state (PSL/PBN/PVB/PVX) — applied via TSF pitch wheel each block
    float pitchOffset      = 0.0f;    // Current semitones offset (PSL/PBN accumulator)
    float pitchSlideTarget = 0.0f;    // Target semitones for PSL (0) or PBN (±127)
    float pitchSlideRate   = 0.0f;    // Semitones per audio frame
    bool  pitchSliding     = false;
    float vibratoPhase     = 0.0f;    // LFO phase [0, 2π]
    float vibratoSpeed     = 0.0f;    // LFO frequency in Hz
    float vibratoDepth     = 0.0f;    // LFO depth in semitones
    bool  vibratoActive    = false;

    // IAudioVoice
    bool active()     const override { return isActive; }
    int  getTrackId() const override { return _trackId; }

    void hardStop() override {
        std::lock_guard<std::mutex> lock(mutex);
        if (handle && activeNote >= 0) {
            tsf_channel_note_off(handle, 0, activeNote);
        }
        activeNote = -1;
        isActive   = false;
    }

    void noteOff() override { hardStop(); }  // TSF handles its own release

    void setVolume(float v) override {
        std::lock_guard<std::mutex> lock(mutex);
        noteVolume = v;
        if (handle) tsf_channel_set_volume(handle, 0, v);
    }

    void setPan(float pan) override {
        std::lock_guard<std::mutex> lock(mutex);
        if (handle) tsf_channel_set_pan(handle, 0, pan);
    }

    void retrigger(int /*startPoint*/) override { /* not applicable to SF */ }

    void setMidiNote(int midiNote) override {
        std::lock_guard<std::mutex> lock(mutex);
        if (!handle) return;
        if (activeNote >= 0) tsf_channel_note_off(handle, 0, activeNote);
        tsf_channel_note_on(handle, 0, midiNote, noteVolume);
        activeNote = midiNote;
    }

    // ── Pitch effect interface (IAudioVoice) ────────────────────────────────
    // The pitch state fields are advanced and applied to TSF pitch wheel by applyPitchMod().
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
        // Reset TSF pitch wheel to centre
        std::lock_guard<std::mutex> lock(mutex);
        if (handle) tsf_channel_set_pitchwheel(handle, 0, 8192);
    }
    void setInitialPitchOffset(float semitones) override { pitchOffset = semitones; }
    // ── end IAudioVoice ─────────────────────────────────────────────────────

    // Renders numFrames of stereo audio into buf (caller must zero buf first).
    // Returns peak level for per-track metering.
    // Note: volume column applied externally (trackVolumes * masterVolume in mix loop).
    float render(float* buf, int numFrames) override {
        std::lock_guard<std::mutex> lock(mutex);
        if (!handle) return 0.0f;
        tsf_render_float(handle, buf, numFrames, 0 /* overwrite */);
        float peak = 0.0f;
        for (int i = 0; i < numFrames * 2; i++) {
            peak = fmaxf(peak, fabsf(buf[i]));
        }
        return peak;
    }

    // Trigger a new note (called from audio thread — no mutex needed here since
    // audio thread and render() are sequential within the same callback).
    void triggerNote(int sfSlot, int midiNote, int midiVelocity, float vol, float pan,
                     int bank, int preset, int trackId, float sampleRate) {
        // If this voice has a clone from a different sfSlot, it was already closed in the
        // caller before triggerNote is called.
        sourceSfSlot = sfSlot;
        _trackId     = trackId;
        noteVolume   = vol;
        // Channel 0: set pan, volume, bank/preset, then note-on
        // Note-off any previous note first to avoid voice leaks
        if (activeNote >= 0) {
            tsf_channel_note_off(handle, 0, activeNote);
        }
        tsf_channel_set_pan(handle, 0, pan);
        tsf_channel_set_volume(handle, 0, vol);
        tsf_channel_set_bank_preset(handle, 0, bank, preset);
        tsf_channel_note_on(handle, 0, midiNote, midiVelocity / 127.0f);
        activeNote = midiNote;
        isActive   = true;
    }

    // Reset all pitch effect state (call after triggerNote)
    void resetPitchState() {
        pitchOffset      = 0.0f;
        pitchSlideTarget = 0.0f;
        pitchSlideRate   = 0.0f;
        pitchSliding     = false;
        vibratoPhase     = 0.0f;
        vibratoActive    = false;
    }

    // Advance pitch state and apply to TSF via pitch wheel — call once per audio block,
    // BEFORE render(). No mutex needed: audio callback is single-threaded.
    void applyPitchMod(float sampleRate, int numFrames) {
        if (!handle) return;
        if (!pitchSliding && !vibratoActive) return;

        // Advance pitch slide (PSL / PBN)
        if (pitchSliding) {
            float delta      = pitchSlideTarget - pitchOffset;
            float totalDelta = pitchSlideRate * numFrames;
            if (fabsf(totalDelta) >= fabsf(delta)) {
                pitchOffset = pitchSlideTarget;
                if (fabsf(pitchSlideTarget) < 100.0f) pitchSliding = false;  // PSL reached target
            } else {
                pitchOffset += totalDelta;
            }
        }

        // Advance vibrato LFO (PVB / PVX)
        if (vibratoActive) {
            float inc = (2.0f * (float)M_PI * vibratoSpeed / sampleRate) * numFrames;
            vibratoPhase += inc;
            while (vibratoPhase >= 2.0f * (float)M_PI) vibratoPhase -= 2.0f * (float)M_PI;
        }

        // Compute total pitch mod in semitones
        float pitchMod = pitchOffset;
        if (vibratoActive) pitchMod += sinf(vibratoPhase) * vibratoDepth;

        // Map semitones → MIDI pitch wheel [0, 16383] (center = 8192)
        // Use a fixed range of 48 semitones (4 octaves) to cover all pitch effects.
        constexpr float PITCH_RANGE = 48.0f;
        float clamped    = fmaxf(-PITCH_RANGE, fminf(PITCH_RANGE, pitchMod));
        int   pitchWheel = (int)(8192.0f + clamped / PITCH_RANGE * 8191.0f);
        if (pitchWheel < 0) pitchWheel = 0;
        if (pitchWheel > 16383) pitchWheel = 16383;

        tsf_channel_set_pitchrange(handle, 0, PITCH_RANGE);
        tsf_channel_set_pitchwheel(handle, 0, pitchWheel);
    }

    void close() {
        if (handle) {
            tsf_close(handle);
            handle = nullptr;
        }
        isActive     = false;
        activeNote   = -1;
        sourceSfSlot = -1;
        _trackId     = -1;
    }
};

// Per-track soundfont voices (one per track, lazily cloned from SoundfontEntry)
static SoundfontVoice sfVoices[8];

class AudioEngine : public oboe::AudioStreamDataCallback {
public:
    AudioEngine() {
        for (int i = 0; i < 256; i++) {
            samples[i] = nullptr;
            sampleLengths[i] = 0;
            // Initialize with default parameters
            instrumentParams[i] = InstrumentParams();
        }
        globalFrameCounter = 0;

        // Initialize waveform buffer
        for (int i = 0; i < WAVEFORM_SIZE; i++) {
            waveformBuffer[i] = 0.0f;
        }
        waveformIndex = 0;
        waveformDownsampleCounter = 0;
    }

    ~AudioEngine() {
        closeStream();
        for (int i = 0; i < 256; i++) {
            if (samples[i]) {
                delete[] samples[i];
            }
        }
    }

    bool openStream() {
        oboe::AudioStreamBuilder builder;
        builder.setDataCallback(this);

        // Enable low-latency mode for precise timing
        // This enables MMAP (fast audio path) and reduces buffer size
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);

        // Try exclusive mode first for lowest latency, fallback to shared if not available
        builder.setSharingMode(oboe::SharingMode::Exclusive);

        builder.setFormat(oboe::AudioFormat::Float);
        builder.setChannelCount(oboe::ChannelCount::Stereo);

        // Set sample rate to 44100 Hz (most common for audio samples)
        // This eliminates compensation for 44100 Hz samples (perfect pitch!)
        // Samples at other rates (48000 Hz) will still be compensated
        builder.setSampleRate(44100);

        oboe::Result result = builder.openStream(stream);
        if (result != oboe::Result::OK) {
            LOGE("Failed to create stream: %s", oboe::convertToText(result));
            return false;
        }

        LOGD("Stream: %d Hz (requested 44100), buffer: %d",
             stream->getSampleRate(),
             stream->getBufferSizeInFrames());

        result = stream->requestStart();
        if (result != oboe::Result::OK) {
            LOGE("Failed to start: %s", oboe::convertToText(result));
            return false;
        }

        LOGD("Started OK");
        return true;
    }

    void closeStream() {
        if (stream) {
            stream->stop();
            stream->close();
            stream.reset();
        }
    }

    void loadSample(int id, const float* data, int length) {
        if (id < 0 || id >= 256) return;

        if (samples[id]) {
            delete[] samples[id];
        }

        samples[id] = new float[length];
        for (int i = 0; i < length; i++) {
            samples[id][i] = data[i];
        }
        sampleLengths[id] = length;

        LOGD("Sample %d: %d frames", id, length);
    }

    void clearAllSamples() {
        // Stop all active voices FIRST — they hold direct pointers to sample data.
        // Deleting samples while voices are still reading them causes use-after-free.
        for (int i = 0; i < MAX_VOICES; i++) {
            voices[i].stop();
        }
        // Clear the scheduled note/kill queues so buffered notes don't re-trigger.
        noteQueue.clear();
        killQueue.clear();

        for (int i = 0; i < 256; i++) {
            if (samples[i]) {
                delete[] samples[i];
                samples[i] = nullptr;
            }
            sampleLengths[i] = 0;
        }
        LOGD("All samples cleared");
    }

    void setInstrumentParams(int instrumentId, int start, int end, bool rev, int loop, int loopSt,
                             int drv, int crsh, int dwn, int fType, int fCut, int fRes) {
        if (instrumentId < 0 || instrumentId >= 256) return;

        instrumentParams[instrumentId].startPoint = start;
        instrumentParams[instrumentId].endPoint = end;
        instrumentParams[instrumentId].reverse = rev;
        instrumentParams[instrumentId].loopMode = loop;
        instrumentParams[instrumentId].loopStart = loopSt;
        instrumentParams[instrumentId].drive = drv;
        instrumentParams[instrumentId].crush = crsh;
        instrumentParams[instrumentId].downsample = dwn;
        instrumentParams[instrumentId].filterType = fType;
        instrumentParams[instrumentId].filterCut = fCut;
        instrumentParams[instrumentId].filterRes = fRes;

        LOGD("Instrument %d params: start=%d, end=%d, rev=%d, loop=%d, loopStart=%d, drive=%d, crush=%d, downsample=%d, filter=%d, cut=%d, res=%d",
             instrumentId, start, end, rev, loop, loopSt, drv, crsh, dwn, fType, fCut, fRes);
    }

    void triggerNote(int sampleId, int trackId, float freq, float baseFreq, float vol, float pan = 0.5f) {
        if (sampleId < 0 || sampleId >= 256 || !samples[sampleId]) return;

        // Resume stream if paused (prevents hum when not playing)
        resumeStream();

        // Stop track
        for (int i = 0; i < MAX_VOICES; i++) {
            if (voices[i].trackId == trackId) {
                voices[i].stop();
            }
        }

        // Find free voice
        for (int i = 0; i < MAX_VOICES; i++) {
            if (!voices[i].isActive) {
                float rate = freq / baseFreq;
                float sampleRate = stream ? (float)stream->getSampleRate() : 44100.0f;
                // Use stored instrument parameters
                voices[i].trigger(samples[sampleId], sampleLengths[sampleId], trackId, rate, vol, pan,
                                  instrumentParams[sampleId], sampleRate);
                LOGD("Note: track=%d, sampleId=%d, rate=%.3f, pan=%.2f", trackId, sampleId, rate, pan);
                return;
            }
        }
    }

    void stopTrack(int trackId) {
        // Stop all sampler voices on this track
        for (int i = 0; i < MAX_VOICES; i++) {
            if (voices[i].trackId == trackId && voices[i].isActive) {
                voices[i].stop();
            }
        }
        // Stop any active soundfont note on this track
        if (trackId >= 0 && trackId < 8) {
            sfVoices[trackId].hardStop();
        }
    }

    void stopAll() {
        for (int i = 0; i < MAX_VOICES; i++) {
            voices[i].stop();
        }
        // Stop all soundfont notes on all tracks
        for (int t = 0; t < 8; t++) {
            sfVoices[t].hardStop();
        }
        // Keep stream running so preview notes and future playback work immediately.
        // With all voices stopped and queue cleared, the callback outputs silence.
        LOGD("stopAll: voices and SF notes cleared, stream stays running");
    }

    int getActiveVoiceCount() {
        int count = 0;
        for (int i = 0; i < MAX_VOICES; i++) {
            if (voices[i].isActive) {
                count++;
            }
        }
        return count;
    }

    /**
     * For each of the 8 tracks, encode the active note as (octave * 12 + pitch), or -1 if no
     * voice is currently playing on that track. The caller passes a pre-allocated int[8] array.
     */
    void getTrackActiveNotes(int* out, int trackCount) {
        for (int t = 0; t < trackCount; t++) out[t] = -1;
        for (int v = 0; v < MAX_VOICES; v++) {
            if (!voices[v].isActive) continue;
            int t = voices[v].trackId;
            if (t >= 0 && t < trackCount && out[t] == -1) {
                out[t] = voices[v].noteOctave * 12 + voices[v].notePitch;
            }
        }
    }

    int getSampleRate() {
        if (stream) {
            return stream->getSampleRate();
        }
        return 48000; // Default fallback
    }

    void resumeStream() {
        // Resume stream when playback starts
        if (stream && stream->getState() == oboe::StreamState::Paused) {
            stream->start();
            LOGD("Stream resumed");
        }
    }

    // ===================================
    // CORE AUDIO PROCESSING BLOCK
    // ===================================
    // ALL audio DSP lives here. onAudioReady and renderOffline are thin wrappers.
    // Rule: NEVER add audio processing logic directly to onAudioReady or renderOffline.
    void processAudioBlock(float* output, int numFrames, int channelCount, float sampleRate) {
        // Zero per-track peak accumulators for this block
        for (int t = 0; t < 8; t++) framePeaksPerTrack[t] = 0.0f;

        // PHASE 1: Process note queue at sample-accurate timing
        for (int32_t frame = 0; frame < numFrames; frame++) {
            int64_t currentFrame = globalFrameCounter + frame;

            // Process all scheduled kill events for this exact frame (BEFORE notes)
            while (killQueue.hasKillAt(currentFrame)) {
                ScheduledKill kill = killQueue.pop();
                if (kill.softKill) {
                    triggerNoteOff(kill.trackId);  // Sampler: trigger ADSR release
                    // SF: noteOff (TSF handles its own release envelope internally)
                    if (kill.trackId >= 0 && kill.trackId < 8) {
                        sfVoices[kill.trackId].noteOff();
                    }
                    LOGD("🎵 Note-off: track %d at frame %lld", kill.trackId, (long long)currentFrame);
                } else {
                    for (int v = 0; v < MAX_VOICES; v++) {
                        if (voices[v].trackId == kill.trackId && voices[v].isActive) {
                            voices[v].stop();
                            LOGD("🔪 Killed track %d at frame %lld", kill.trackId, (long long)currentFrame);
                        }
                    }
                    // SF: hard kill (immediate stop)
                    if (kill.trackId >= 0 && kill.trackId < 8) {
                        sfVoices[kill.trackId].hardStop();
                    }
                }
            }

            // Trigger all notes scheduled for this exact frame
            while (noteQueue.hasNoteAt(currentFrame)) {
                ScheduledNote note = noteQueue.pop();

                // ---- SOUNDFONT PATH ----
                // Each track has its own SoundfontVoice with an independent tsf* clone.
                // This gives accurate per-track metering and supports per-track effects (Phase 3).
                if (note.isSoundfont) {
                    int t = note.trackId;
                    if (t >= 0 && t < 8 &&
                        note.sfSlot >= 0 && note.sfSlot < MAX_SOUNDFONTS &&
                        !soundfonts[note.sfSlot].fileData.empty()) {

                        SoundfontVoice& sv = sfVoices[t];
                        // If the track's clone came from a different sfSlot, rebuild it.
                        if (sv.sourceSfSlot != note.sfSlot || sv.handle == nullptr) {
                            sv.close();  // frees old handle if any
                            const auto& fd = soundfonts[note.sfSlot].fileData;
                            sv.handle = tsf_load_memory(fd.data(), (int)fd.size());
                            if (sv.handle) {
                                tsf_set_output(sv.handle, TSF_STEREO_INTERLEAVED, (int)sampleRate, 0.0f);
                            }
                        }
                        if (sv.handle) {
                            sv.triggerNote(note.sfSlot, note.midiNote, note.midiVelocity,
                                           note.volume, note.pan, note.sfBank, note.sfPreset,
                                           t, sampleRate);
                            // Apply pitch effects (PSL / PBN / PVB / PVX)
                            sv.resetPitchState();
                            if (note.pslInitialOffset != 0.0f && note.pslDuration > 0.0f) {
                                sv.pitchOffset      = note.pslInitialOffset;
                                sv.pitchSlideTarget = 0.0f;
                                sv.pitchSlideRate   = -note.pslInitialOffset / note.pslDuration;
                                sv.pitchSliding     = true;
                            }
                            if (fabsf(note.pbnRate) > 0.0001f) {
                                sv.pitchSlideRate   = note.pbnRate;
                                sv.pitchSlideTarget = (note.pbnRate > 0) ? 127.0f : -127.0f;
                                sv.pitchSliding     = true;
                            }
                            if (note.vibratoDepth > 0.01f) {
                                sv.vibratoSpeed  = note.vibratoSpeed;
                                sv.vibratoDepth  = note.vibratoDepth;
                                sv.vibratoActive = true;
                            }
                            LOGD("🎹 SF FIRE (per-track): slot=%d track=%d bank=%d preset=%d midi=%d vel=%d vol=%.2f",
                                 note.sfSlot, t, note.sfBank, note.sfPreset,
                                 note.midiNote, note.midiVelocity, note.volume);
                        } else {
                            LOGD("🎹 SF CLONE FAILED: sfSlot=%d track=%d", note.sfSlot, t);
                        }
                    } else {
                        LOGD("🎹 SF DROPPED: sfSlot=%d track=%d (invalid or no file data)", note.sfSlot, note.trackId);
                    }
                    continue;  // Skip voice pool processing
                }
                // ---- END SOUNDFONT PATH ----

                bool voiceFound = false;

                // TIC00 support: Check if previous voice on this track was using trigger mode
                int savedTableRow = 0;
                bool wasTIC00Mode = false;
                for (int v = 0; v < MAX_VOICES; v++) {
                    if (voices[v].trackId == note.trackId && voices[v].isActive && !voices[v].isFadingOut) {
                        if (voices[v].tableTicRate == 0x00 && voices[v].tableId >= 0) {
                            wasTIC00Mode = true;
                            savedTableRow = (voices[v].tableRow + 1) % 16;
                            LOGD("📋 TIC00: Saving table row %d for track %d retrigger", savedTableRow, note.trackId);
                        }
                    }
                }

                // ---------------------------------------------------------------
                // VOICE ALLOCATION — 3-step priority
                //
                // Problem: "steal old + allocate new" temporarily consumes two
                // slots per track.  When N tracks all trigger at the same frame
                // (phrase boundaries) this exhausts the 8-slot pool even with
                // only 5 active tracks.
                //
                // Step 1 — recycle fading same-track voice (0 extra slots used).
                //           trackId is preserved through startFadeOut() so we can
                //           find and reuse the slot immediately.
                // Step 2 — normal steal: fade old same-track voice, find free slot.
                // Step 3 — last resort: preempt any fading voice (other track).
                //           Produces at most a ~1ms click but prevents silence.
                // ---------------------------------------------------------------

                // Step 1: same-track fading voice → recycle directly
                int targetSlot = -1;
                for (int v = 0; v < MAX_VOICES; v++) {
                    if (voices[v].trackId == note.trackId && voices[v].isFadingOut) {
                        targetSlot = v;
                        break;
                    }
                }

                // Step 2: steal non-fading same-track voice, then find free slot
                if (targetSlot == -1) {
                    for (int v = 0; v < MAX_VOICES; v++) {
                        if (voices[v].trackId == note.trackId && voices[v].isActive && !voices[v].isFadingOut) {
                            voices[v].startFadeOut();
                        }
                    }
                    for (int v = 0; v < MAX_VOICES; v++) {
                        if (!voices[v].isActive) {
                            targetSlot = v;
                            break;
                        }
                    }
                }

                // Step 3: preempt any fading voice (last resort)
                if (targetSlot == -1) {
                    for (int v = 0; v < MAX_VOICES; v++) {
                        if (voices[v].isFadingOut) {
                            targetSlot = v;
                            LOGD("⚠️ Voice pool tight: preempting fading slot %d for track %d", v, note.trackId);
                            break;
                        }
                    }
                }

                if (targetSlot != -1) {
                    int v = targetSlot;
                    if (note.sampleId >= 0 && note.sampleId < 256 && samples[note.sampleId]) {
                        float rate = note.frequency / note.baseFrequency;

                        // M8-style: Check if table's last row has TIC effect
                        int effectiveTicRate = note.tableTicRate;
                        if (note.tableId >= 0 && note.tableId < 256) {
                            std::lock_guard<std::mutex> lock(tableMutex);
                            if (tables[note.tableId].loaded) {
                                TableRow& lastRow = tables[note.tableId].rows[15];
                                auto checkTic = [](uint8_t fxType, uint8_t fxValue) -> int {
                                    if (fxType == FX_TIC) return fxValue;
                                    return -1;
                                };
                                int tic1 = checkTic(lastRow.fx1Type, lastRow.fx1Value);
                                int tic2 = checkTic(lastRow.fx2Type, lastRow.fx2Value);
                                int tic3 = checkTic(lastRow.fx3Type, lastRow.fx3Value);
                                if (tic1 >= 0) {
                                    effectiveTicRate = tic1;
                                    LOGD("📋 M8-style: Using TIC %02X from table %d last row", effectiveTicRate, note.tableId);
                                } else if (tic2 >= 0) {
                                    effectiveTicRate = tic2;
                                    LOGD("📋 M8-style: Using TIC %02X from table %d last row", effectiveTicRate, note.tableId);
                                } else if (tic3 >= 0) {
                                    effectiveTicRate = tic3;
                                    LOGD("📋 M8-style: Using TIC %02X from table %d last row", effectiveTicRate, note.tableId);
                                }
                            }
                        }

                        // Determine table start row
                        int startRow;
                        if (note.tableStartRow >= 0) {
                            startRow = note.tableStartRow % 16;
                        } else if (wasTIC00Mode && effectiveTicRate == 0x00) {
                            startRow = savedTableRow;
                        } else {
                            startRow = 0;
                        }

                        voices[v].trigger(samples[note.sampleId], sampleLengths[note.sampleId],
                                          note.trackId, rate, note.volume, note.pan, instrumentParams[note.sampleId],
                                          sampleRate, note.startPointOverride,
                                          note.tableId, effectiveTicRate, note.noteOctave, note.notePitch, startRow);

                        // PSL: Set initial pitch offset and start slide to note pitch.
                        // pslDuration is already in audio frames (converted by AudioEngine.kt).
                        if (fabsf(note.pslInitialOffset) > 0.001f && note.pslDuration > 0.0f) {
                            voices[v].pitchOffset = note.pslInitialOffset;
                            float totalFrames = fmaxf(1.0f, note.pslDuration);
                            voices[v].pitchSlideTarget = 0.0f;
                            voices[v].pitchSlideRate = -note.pslInitialOffset / totalFrames;
                            voices[v].pitchSliding = true;
                            LOGD("🎵 PSL applied: offset=%.2f, duration=%.0f ticks, rate=%.6f",
                                 note.pslInitialOffset, note.pslDuration, voices[v].pitchSlideRate);
                        }
                        // PBN: Set continuous pitch bend rate.
                        // pbnRate is already in semitones/frame (converted by AudioEngine.kt).
                        if (fabsf(note.pbnRate) > 0.0001f) {
                            voices[v].pitchSlideRate = note.pbnRate;
                            voices[v].pitchSlideTarget = (note.pbnRate > 0) ? 127.0f : -127.0f;
                            voices[v].pitchSliding = true;
                            LOGD("🎵 PBN applied: rate=%.4f semitones/tick", note.pbnRate);
                        }
                        // PVB/PVX: Set vibrato
                        if (note.vibratoDepth > 0.01f) {
                            voices[v].vibratoSpeed = note.vibratoSpeed;
                            voices[v].vibratoDepth = note.vibratoDepth;
                            voices[v].vibratoActive = true;
                            LOGD("🎵 Vibrato applied: speed=%.1fHz, depth=%.2f semitones",
                                 note.vibratoSpeed, note.vibratoDepth);
                        }

                        // Initialize modulation state from instrument mod slots
                        voices[v].baseVolume = note.volume;
                        voices[v].modPitchOffset = 0.0f;
                        for (int m = 0; m < 4; m++) {
                            const InstrumentModSlot& src = instrumentModSlots[note.sampleId][m];
                            Voice::VoiceModSlot& dst = voices[v].voiceMods[m];
                            dst.type = src.type;
                            dst.dest = src.dest;
                            dst.amount = src.amount;
                            dst.attackSamples = src.attackSamples;
                            dst.holdSamples = src.holdSamples;
                            dst.decaySamples = src.decaySamples;
                            dst.sustainLevel = src.sustainLevel;
                            dst.lfoHz = src.lfoHz;
                            dst.oscShape = src.oscShape;
                            dst.lfoPhase = 0.0f;
                            dst.releaseSamples = src.releaseSamples;
                            dst.effectiveAmt = src.amount;
                            dst.effectiveRateMult = 1.0f;
                            dst.prevEnvValue = 0.0f;
                            if (src.type != 0) {
                                dst.stage = 1;
                                dst.envValue = 0.0f;
                                dst.stageCounter = 0;
                            } else {
                                dst.stage = 0;
                                dst.envValue = 0.0f;
                                dst.stageCounter = 0;
                            }
                        }

                        voiceFound = true;
                        LOGD("🎵 Triggered note at frame %lld: sample=%d, track=%d, rate=%.3f, vol=%.4f, pan=%.2f, startOverride=%d, table=%d, tic=%d, oct=%d, pitch=%d, startRow=%d",
                             (long long)currentFrame, note.sampleId, note.trackId, rate, note.volume, note.pan, note.startPointOverride,
                             note.tableId, effectiveTicRate, note.noteOctave, note.notePitch, startRow);
                    } else {
                        if (note.sampleId < 0 || note.sampleId >= 256) {
                            LOGD("❌ Invalid sampleId=%d for note at frame %lld", note.sampleId, (long long)currentFrame);
                        } else {
                            LOGD("❌ Sample %d not loaded! Note at frame %lld cannot play", note.sampleId, (long long)currentFrame);
                        }
                    }
                } else {
                    LOGD("⚠️ No free voice (all 8 fully active) for note at frame %lld, sample=%d", (long long)currentFrame, note.sampleId);
                }
            }
        }

        // ===================================
        // TABLE PROCESSING (Phase 3.5 + Phase 4 special modes)
        // ===================================
        // Process table ticks for each active voice once per callback
        // Special TIC modes (Phase 4):
        //   TIC00 (0x00): Trigger mode - table doesn't advance automatically
        //   TIC01-FB: Standard tic rate (1 tic = 1 audio callback ~6ms)
        //   TICFC (0xFC): Octave map - row = triggered note's octave (0-9)
        //   TICFE (0xFE): Note map - row = triggered note's pitch (0-11)
        //   TICFF (0xFF): 200Hz mode - advance ~1 row per 5ms
        for (int v = 0; v < MAX_VOICES; v++) {
            Voice& voice = voices[v];
            if (!voice.isActive || voice.tableId < 0) continue;

            // Check if table is loaded
            bool tableLoaded = false;
            {
                std::lock_guard<std::mutex> lock(tableMutex);
                tableLoaded = tables[voice.tableId].loaded;
            }
            if (!tableLoaded) continue;

            // Handle special TIC modes
            bool shouldProcessRow = false;
            bool shouldAdvance = false;

            if (voice.tableTicRate == 0x00) {
                // TIC00: Trigger mode - apply row effects ONCE, don't advance automatically
                // Table row only advances when note is retriggered (handled in trigger logic)
                // Only process if we haven't processed this row yet
                shouldProcessRow = (voice.tableRow != voice.lastProcessedRow);
                shouldAdvance = false;
            } else if (voice.tableTicRate == 0xFC || voice.tableTicRate == 0xFE) {
                // TICFC/TICFE: Static mapping modes - row is fixed, process ONCE
                // Only process if we haven't processed this row yet
                shouldProcessRow = (voice.tableRow != voice.lastProcessedRow);
                shouldAdvance = false;
            } else if (voice.tableTicRate == 0xFF) {
                // TICFF: 200Hz mode - faster advancement
                // Accumulate frames; advance table row every (sampleRate/200) samples
                voice.tic200HzAccum += numFrames;
                float samplesPerTic = sampleRate / 200.0f;  // samples per 200Hz tic
                if (voice.tic200HzAccum >= samplesPerTic) {
                    voice.tic200HzAccum -= samplesPerTic;
                    shouldProcessRow = true;
                    shouldAdvance = true;
                }
            } else {
                // Standard tic mode (01-FB): advance every N tics
                voice.tableTicCounter++;
                if (voice.tableTicCounter >= voice.tableTicRate) {
                    voice.tableTicCounter = 0;
                    shouldProcessRow = true;
                    shouldAdvance = true;
                }
            }

            // Process current table row if needed
            if (shouldProcessRow) {

                // Get current table row data
                TableRow row;
                {
                    std::lock_guard<std::mutex> lock(tableMutex);
                    row = tables[voice.tableId].rows[voice.tableRow];
                }

                // Apply transpose (convert semitones to playback rate modifier)
                int semitones = transposeToSemitones(row.transpose);
                voice.tableTranspose = (float)semitones;
                float transposeRatio = powf(2.0f, voice.tableTranspose / 12.0f);
                voice.playbackRate = voice.basePlaybackRate * transposeRatio;

                // Apply volume (FF = no change = 1.0, 00 = silence = 0.0)
                if (row.volume == 0xFF) {
                    voice.tableVolume = 1.0f;  // No change
                } else {
                    voice.tableVolume = row.volume / 255.0f;
                }

                // Process table effects (3 effect slots per row)
                bool hopExecuted = false;
                int hopTarget = -1;

                // Helper lambda to process a single effect
                auto processEffect = [&](uint8_t fxType, uint8_t fxValue) {
                    switch (fxType) {
                        case FX_KILL:
                            // K00 - Kill voice immediately
                            if (fxValue == 0x00) {
                                voice.isActive = false;
                                LOGD("📋 Table effect: KILL voice %d", v);
                            }
                            break;

                        case FX_HOP:
                            // Hxx - HOP effect (Phase 5: repeat count support)
                            // Format: HOP XY where X = repeat count, Y = target row
                            // HOP FF = stop table processing
                            // HOP 0Y = infinite loop to row Y
                            // HOP XY (X>0) = jump to row Y exactly X times, then continue
                            if (fxValue == 0xFF) {
                                // Stop table processing for this voice
                                voice.tableId = -1;
                                voice.hopTargetRow = -1;
                                voice.hopRepeatCount = 0;
                                LOGD("📋 Table HOP FF: stopped table for voice %d", v);
                            } else {
                                int repeatCount = (fxValue >> 4) & 0x0F;  // High nibble = X
                                int targetRow = fxValue & 0x0F;           // Low nibble = Y

                                if (repeatCount == 0) {
                                    // HOP 0Y = Infinite loop to row Y
                                    hopExecuted = true;
                                    hopTarget = targetRow;
                                    LOGD("📋 Table HOP %02X: infinite loop to row %d, voice %d", fxValue, targetRow, v);
                                } else {
                                    // HOP XY (X>0) = Jump X times, then continue
                                    // Initialize counter if this is a new HOP or different target
                                    if (voice.hopTargetRow == -1 || voice.hopTargetRow != targetRow) {
                                        voice.hopRepeatCount = repeatCount;
                                        voice.hopTargetRow = targetRow;
                                        LOGD("📋 Table HOP %02X: initialized counter=%d, target=%d, voice %d",
                                             fxValue, repeatCount, targetRow, v);
                                    }

                                    if (voice.hopRepeatCount > 0) {
                                        voice.hopRepeatCount--;
                                        hopExecuted = true;
                                        hopTarget = targetRow;
                                        LOGD("📋 Table HOP: jump to row %d, %d jumps remaining, voice %d",
                                             targetRow, voice.hopRepeatCount, v);
                                    } else {
                                        // Counter exhausted, don't jump, reset state and continue normally
                                        voice.hopTargetRow = -1;
                                        LOGD("📋 Table HOP: counter exhausted, continuing past row, voice %d", v);
                                    }
                                }
                            }
                            break;

                        case FX_VOLUME:
                            // Vxx - Set volume (overrides volume column)
                            voice.tableVolume = fxValue / 255.0f;
                            break;

                        case FX_OFFSET:
                            // Oxx - Change sample position (relative to current)
                            // Map 00-FF to sample position
                            if (voice.sampleLength > 0) {
                                float normalizedPos = fxValue / 255.0f;
                                voice.position = normalizedPos * (voice.sampleLength - 1);
                            }
                            break;

                        case FX_TIC:
                            // Txx - Set table tick rate (tics per row advance)
                            // 01-FB = standard tic rate (01=fastest, FB=slowest)
                            // FC-FF = special modes (future: octave map, note map, etc.)
                            if (fxValue >= 0x01 && fxValue <= 0xFB) {
                                voice.tableTicRate = fxValue;
                                voice.tableTicCounter = 0;  // Reset counter when rate changes
                                LOGD("📋 Table effect: TIC %02X - set tick rate to %d", fxValue, fxValue);
                            }
                            // Note: TIC00 is handled specially - it means "trigger mode"
                            // where table advances only when the note is triggered
                            break;

                        case FX_THO:
                            // THO 0X - Table hop to row X (simple unconditional jump)
                            // Unlike HOP, no repeat count — always jumps
                            hopExecuted = true;
                            hopTarget = fxValue & 0x0F;
                            LOGD("📋 Table THO %02X: hop to row %d, voice %d", fxValue, hopTarget, v);
                            break;

                        default:
                            // Unknown or unimplemented effect - ignore
                            break;
                    }
                };

                // Process all 3 effect slots
                processEffect(row.fx1Type, row.fx1Value);
                processEffect(row.fx2Type, row.fx2Value);
                processEffect(row.fx3Type, row.fx3Value);

                // Mark this row as processed (before any jumps change tableRow)
                int processedRow = voice.tableRow;
                voice.lastProcessedRow = processedRow;

                // Handle row advancement:
                // - HOP always works (jumps to target row) regardless of shouldAdvance
                // - Normal advancement only happens if shouldAdvance is true and no HOP
                if (hopExecuted && hopTarget >= 0) {
                    // HOP effect: jump to target row (works in all TIC modes including TIC00)
                    voice.tableRow = hopTarget % 16;
                    LOGD("📋 Table HOP: voice %d jumped to row %d", v, voice.tableRow);
                } else if (shouldAdvance) {
                    // Normal advancement (only for non-TIC00/TICFC/TICFE modes)
                    voice.tableRow = (voice.tableRow + 1) % 16;
                }

                // Debug log (only occasionally to avoid spam)
                if (shouldAdvance && voice.tableRow == 0) {
                    LOGD("📋 Table %d loop: voice=%d, transpose=%.0f, vol=%.2f",
                         voice.tableId, v, voice.tableTranspose, voice.tableVolume);
                }
            }
        }

        // ===================================
        // PITCH MODULATION PROCESSING (Phase 6)
        // ===================================
        for (int v = 0; v < MAX_VOICES; v++) {
            Voice& voice = voices[v];
            if (!voice.isActive) continue;
            updateVoicePitchMod(voice, numFrames, sampleRate);
        }

        // ===================================
        // MODULATION PROCESSING (Phase 4 — AHD/ADSR/LFO)
        // ===================================
        // Snapshot envValues before advancing so the mix loop can interpolate
        // per-sample (eliminates block-rate staircase artifacts on short envelopes).
        for (int v = 0; v < MAX_VOICES; v++) {
            if (!voices[v].isActive) continue;
            for (int m = 0; m < 4; m++)
                voices[v].voiceMods[m].prevEnvValue = voices[v].voiceMods[m].envValue;
        }
        for (int v = 0; v < MAX_VOICES; v++) {
            if (!voices[v].isActive) continue;
            updateVoiceModulation(voices[v], numFrames, sampleRate);
        }

        // Apply per-voice PAN and FILTER modulation (once per block)
        for (int v = 0; v < MAX_VOICES; v++) {
            Voice& voice = voices[v];
            if (!voice.isActive) continue;

            // PAN modulation: recalculate stereo gains from basePan + mod offset
            if (fabsf(voice.modPanOffset) > 0.001f) {
                float modPan = fmaxf(0.0f, fminf(1.0f, voice.basePan + voice.modPanOffset));
                float panAngle = modPan * (float)M_PI * 0.5f;
                voice.panLeft = cosf(panAngle);
                voice.panRight = sinf(panAngle);
            }

            // FILTER modulation: recalculate biquad coefficients
            if (voice.filterType != 0 && (fabsf(voice.modCutOffset) > 0.5f || fabsf(voice.modResOffset) > 0.5f)) {
                int modCut = std::max(0, std::min(255, voice.filterCut + (int)voice.modCutOffset));
                int modRes = std::max(0, std::min(255, voice.filterRes + (int)voice.modResOffset));
                calculateBiquadCoeffs(voice.filterType, modCut, modRes, sampleRate,
                                      voice.b0, voice.b1, voice.b2, voice.a1, voice.a2);
            }

            // Auto-stop looping voice when volume envelope completes
            // AHD/DRUM done at stage 4; ADSR/TRIG done at stage 5
            if (voice.loopMode != 0) {
                bool hasVolMod = false, allDone = true;
                for (int m = 0; m < 4; m++) {
                    const Voice::VoiceModSlot& mod = voice.voiceMods[m];
                    if (mod.dest == 1 && (mod.type == 1 || mod.type == 2 || mod.type == 4 || mod.type == 5)) {
                        hasVolMod = true;
                        int doneStage = (mod.type == 2 || mod.type == 5) ? 5 : 4;
                        if (mod.stage < doneStage) allDone = false;
                    }
                }
                if (hasVolMod && allDone) voice.isActive = false;
            }
        }

        // Mix voices
        for (int v = 0; v < MAX_VOICES; v++) {
            Voice& voice = voices[v];
            if (!voice.isActive || !voice.sampleData) continue;

            // Get modulated playback rate (includes pitch slide + vibrato)
            float modulatedRate = getModulatedPlaybackRate(voice);

            for (int i = 0; i < numFrames; i++) {
                int idx = (int)voice.position;
                float frac = voice.position - (float)idx;  // Fractional part for interpolation

                // Bounds check - need idx+1 for interpolation
                if (idx < 0 || idx >= voice.sampleLength - 1) {
                    // Handle edge case: exactly at last sample
                    if (idx == voice.sampleLength - 1 && frac == 0.0f) {
                        float sample = voice.sampleData[idx] * voice.volume;
                        float sampleL = sample * voice.panLeft;
                        float sampleR = sample * voice.panRight;
                        output[i * channelCount] += sampleL;       // Left
                        output[i * channelCount + 1] += sampleR;   // Right
                        // Track peak for metering (max of L/R for mono track meters)
                        if (voice.trackId >= 0 && voice.trackId < 8) {
                            float peakLevel = fmaxf(fabsf(sampleL), fabsf(sampleR));
                            framePeaksPerTrack[voice.trackId] = fmaxf(framePeaksPerTrack[voice.trackId], peakLevel);
                        }
                    }
                    voice.isActive = false;
                    break;
                }

                // ===================================
                // SIGNAL CHAIN: Downsample → Crush → Interpolate → Drive → Volume
                // Applying downsample/crush BEFORE interpolation creates artifacts
                // that interact with pitch-shifting for authentic lo-fi texture
                // ===================================

                // Read two adjacent samples for interpolation
                float sample1 = voice.sampleData[idx];
                float sample2 = voice.sampleData[idx + 1];

                // STEP 1: DOWNSAMPLE (sample rate reduction via sample-and-hold)
                // Apply to raw samples BEFORE interpolation to create aliasing
                if (voice.downsample > 0) {
                    int downsampleFactor = 1 << voice.downsample;  // 2^downsample
                    // Quantize position to downsample grid (creates sample-and-hold effect)
                    int quantizedIdx = (idx / downsampleFactor) * downsampleFactor;
                    sample1 = voice.sampleData[quantizedIdx];
                    sample2 = voice.sampleData[quantizedIdx];  // No interpolation between samples
                }

                // STEP 2: CRUSH (bit depth reduction)
                // Apply to raw samples BEFORE interpolation
                if (voice.crush > 0) {
                    int bits = 16 - voice.crush;  // 0=16-bit (off), 1=15-bit, ..., 15=1-bit
                    if (bits < 1) bits = 1;  // Minimum 1 bit
                    int levels = 1 << bits;  // 2^bits quantization levels
                    sample1 = floorf(sample1 * levels) / levels;
                    sample2 = floorf(sample2 * levels) / levels;
                }

                // STEP 3: LINEAR INTERPOLATION (pitch shifting)
                // Now interpolate the crushed/downsampled samples
                float interpolatedSample = sample1 + (sample2 - sample1) * frac;

                // STEP 4: DRIVE (pre-gain boost with soft clipping for overdrive character)
                float processedSample = interpolatedSample;
                if (voice.drive > 0) {
                    float driveGain = voice.drive / 128.0f;  // 00=0x, 80=1.0x, FF=2.0x
                    processedSample *= driveGain;
                    // Soft clip using tanh for smooth overdrive/distortion character
                    processedSample = tanhf(processedSample);
                }

                // STEP 5: FILTER (resonant biquad filter)
                // Apply only if filter is enabled (filterType != 0)
                if (voice.filterType != 0) {
                    // Biquad filter equation (Direct Form I):
                    // y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
                    float x0 = processedSample;
                    float y0 = voice.b0 * x0 + voice.b1 * voice.x1 + voice.b2 * voice.x2
                               - voice.a1 * voice.y1 - voice.a2 * voice.y2;

                    // Update history buffers
                    voice.x2 = voice.x1;
                    voice.x1 = x0;
                    voice.y2 = voice.y1;
                    voice.y1 = y0;

                    processedSample = y0;
                }

                // STEP 6: Apply volume after effects, with modulation (Phase 4)
                // AHD/ADSR: (envValue-1)*effectiveAmt — fade-in from -depth at start to 0 at peak
                // LFO:  tremolo — multiply by (1 + envValue*effectiveAmt), envValue in [-1,+1]
                // effectiveAmt already incorporates mod-to-mod scaling (Phase 4.4)
                //
                // Per-sample interpolation (decay only):
                // updateVoiceModulation() advances envValue once per block, which causes a
                // discrete amplitude step at each callback boundary — audible as crackling on
                // short envelopes.  We fix this by linearly interpolating envValue from its
                // previous-block value (prevEnvValue) to its current-block value (envValue)
                // but ONLY when the envelope is falling (decay/release).  Rising transitions
                // (attack) use envValue directly so ATK=00 stays instant.
                float t = (numFrames > 1) ? (float)(i + 1) / (float)numFrames : 1.0f;
                float finalVol = voice.volume;
                for (int m = 0; m < 4; m++) {
                    const Voice::VoiceModSlot& mod = voice.voiceMods[m];
                    if (mod.type == 0 || mod.stage == 0) continue;
                    if (mod.dest == 1) { // VOL destination
                        if (mod.type == 3) {
                            // LFO: bipolar tremolo — interpolate for smooth low-rate modulation
                            float envAtI = mod.prevEnvValue + (mod.envValue - mod.prevEnvValue) * t;
                            finalVol = fmaxf(0.0f, finalVol * (1.0f + envAtI * mod.effectiveAmt));
                        } else {
                            // AHD/DRUM/ADSR/TRIG: only interpolate on decay (falling envelope)
                            float envAtI = (mod.envValue < mod.prevEnvValue)
                                ? mod.prevEnvValue + (mod.envValue - mod.prevEnvValue) * t
                                : mod.envValue;
                            finalVol = fmaxf(0.0f, finalVol + (envAtI - 1.0f) * mod.effectiveAmt);
                        }
                    }
                    // PITCH dest: already accumulated into voice.modPitchOffset by updateVoiceModulation
                }
                float sample = processedSample * finalVol * voice.tableVolume;

                // STEP 7: Apply real-time track and master volume
                // trackId is preserved through fade-out so fading voices still
                // use the correct track volume (more accurate than neutral 1.0).
                float trackVol, masterVol;
                {
                    std::lock_guard<std::mutex> lock(volumeMutex);
                    trackVol = (voice.trackId >= 0 && voice.trackId < 8) ? trackVolumes[voice.trackId] : 1.0f;
                    masterVol = masterVolume;
                }
                sample = sample * trackVol * masterVol;

                // STEP 8: Anti-click fades (fade-in + end-of-sample fade-out)
                sample *= voice.antiClickFade();

                // STEP 8b: Voice-steal fade-out multiplier (inline — slot stays reserved while fading)
                // The main mix loop handles this so there is no separate drain loop needed.
                if (voice.isFadingOut) {
                    sample *= (float)voice.fadeOutRemaining / (float)DECLICK_SAMPLES;
                    if (--voice.fadeOutRemaining <= 0) {
                        voice.isFadingOut = false;
                        voice.isActive = false;
                    }
                }

                // Apply pan and write to stereo channels
                float sampleL = sample * voice.panLeft;
                float sampleR = sample * voice.panRight;
                output[i * channelCount] += sampleL;
                output[i * channelCount + 1] += sampleR;

                // Track peak for metering — exclude fading voices to avoid ghost
                // meters after a note is stolen (trackId is now preserved through fade).
                if (!voice.isFadingOut && voice.trackId >= 0 && voice.trackId < 8) {
                    float peakLevel = fmaxf(fabsf(sampleL), fabsf(sampleR));
                    framePeaksPerTrack[voice.trackId] = fmaxf(framePeaksPerTrack[voice.trackId], peakLevel);
                }

                // Exit loop immediately if voice became inactive (fade complete or sample end)
                if (!voice.isActive) break;

                // Update position based on playback mode
                // Use modulatedRate which includes pitch slide + vibrato modulation
                if (voice.loopMode == 2) {
                    // Ping-pong loop
                    if (voice.loopingBack) {
                        voice.position -= modulatedRate;
                        if (voice.position <= voice.actualLoopStart) {
                            voice.loopingBack = false;
                            voice.position = (float)voice.actualLoopStart;
                        }
                    } else {
                        voice.position += modulatedRate;
                        if (voice.position >= voice.actualEnd) {
                            voice.loopingBack = true;
                            voice.position = (float)voice.actualEnd;
                        }
                    }
                } else if (voice.reverse) {
                    // Reverse playback
                    voice.position -= modulatedRate;
                    if (voice.position <= voice.actualStart) {
                        if (voice.loopMode == 1) {
                            // Forward loop (restart from loop point)
                            voice.position = (float)voice.actualLoopStart;
                        } else {
                            // No loop, stop
                            voice.isActive = false;
                            break;
                        }
                    }
                } else {
                    // Forward playback
                    voice.position += modulatedRate;
                    if (voice.position >= voice.actualEnd) {
                        if (voice.loopMode == 1) {
                            // Forward loop (restart from loop point)
                            voice.position = (float)voice.actualLoopStart;
                        } else {
                            // No loop, stop
                            voice.isActive = false;
                            break;
                        }
                    }
                }
            }
        }

        // ===================================
        // SOUNDFONT VOICE RENDERING (UAA Phase 2)
        // ===================================
        // Each track's SoundfontVoice renders independently → accurate per-track metering.
        // Track and master volume applied here (not at note-on time).
        {
            float sfBuf[2048];  // 1024 frames * 2 channels — safe for any Oboe buffer size
            float masterVol;
            { std::lock_guard<std::mutex> vlock(volumeMutex); masterVol = masterVolume; }
            for (int t = 0; t < 8; t++) {
                if (!sfVoices[t].isActive || !sfVoices[t].handle) continue;
                sfVoices[t].applyPitchMod((float)sampleRate, numFrames);
                memset(sfBuf, 0, sizeof(float) * numFrames * 2);
                float peak = sfVoices[t].render(sfBuf, numFrames);
                framePeaksPerTrack[t] = fmaxf(framePeaksPerTrack[t], peak);
                float trackVol;
                { std::lock_guard<std::mutex> vlock(volumeMutex); trackVol = trackVolumes[t]; }
                for (int i = 0; i < numFrames * 2; i++) {
                    output[i] += sfBuf[i] * trackVol * masterVol;
                }
            }
        }

        // Brickwall limiter at -0.1 dBFS — hard clip both channels.
        // Threshold = 10^(-0.1/20) ≈ 0.9886. Prevents inter-sample clipping on DAC.
        {
            constexpr float LIMITER_THRESHOLD = 0.98855f;
            for (int i = 0; i < numFrames; i++) {
                output[i * channelCount]     = fmaxf(-LIMITER_THRESHOLD, fminf(LIMITER_THRESHOLD, output[i * channelCount]));
                output[i * channelCount + 1] = fmaxf(-LIMITER_THRESHOLD, fminf(LIMITER_THRESHOLD, output[i * channelCount + 1]));
            }
        }

        globalFrameCounter += numFrames;
    }

    // ===================================
    // LIVE AUDIO CALLBACK (thin wrapper — no DSP here!)
    // ===================================
    oboe::DataCallbackResult onAudioReady(
            oboe::AudioStream *audioStream,
            void *audioData,
            int32_t numFrames) override {

        // Set flush-to-zero mode once at audio thread start.
        // Prevents denormal floats in biquad filter history from causing 100-1000x CPU slowdown
        // on ARM Cortex-A53 during silence.
        static std::once_flag ftzFlag;
        std::call_once(ftzFlag, []() {
#if defined(__aarch64__)
            uint64_t fpcr;
            asm volatile("mrs %0, fpcr" : "=r"(fpcr));
            fpcr |= (1ULL << 24);  // FZ bit: flush denormals to zero
            asm volatile("msr fpcr, %0" : : "r"(fpcr));
#elif defined(__arm__)
            uint32_t fpscr;
            asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
            fpscr |= (1U << 24);  // FZ bit
            asm volatile("vmsr fpscr, %0" : : "r"(fpscr));
#endif
        });

        float *output = static_cast<float*>(audioData);
        int channelCount = audioStream->getChannelCount();

        // Silence output
        for (int i = 0; i < numFrames * channelCount; i++) {
            output[i] = 0.0f;
        }

        // During offline WAV render: output silence and let renderOffline process the queue.
        if (isOfflineRendering.load()) {
            return oboe::DataCallbackResult::Continue;
        }

        float sampleRate = (float)audioStream->getSampleRate();
        processAudioBlock(output, numFrames, channelCount, sampleRate);

        // Capture waveform for oscilloscope (left channel only, with downsampling)
        {
            std::lock_guard<std::mutex> lock(waveformMutex);
            for (int i = 0; i < numFrames; i++) {
                waveformDownsampleCounter++;
                if (waveformDownsampleCounter >= WAVEFORM_DOWNSAMPLE) {
                    waveformBuffer[waveformIndex] = output[i * channelCount];
                    waveformIndex = (waveformIndex + 1) % WAVEFORM_SIZE;
                    waveformDownsampleCounter = 0;
                }
            }
        }

        // Update peak levels for mixer meters (live-only — not needed during WAV export)
        {
            std::lock_guard<std::mutex> lock(peakMutex);

            for (int t = 0; t < 8; t++) {
                trackPeaks[t] *= PEAK_DECAY;
            }
            masterPeakL *= PEAK_DECAY;
            masterPeakR *= PEAK_DECAY;

            // Per-track peaks accumulated by processAudioBlock in this callback
            for (int t = 0; t < 8; t++) {
                trackPeaks[t] = fmaxf(trackPeaks[t], framePeaksPerTrack[t]);
            }

            // Master peaks from output buffer
            float maxL = 0.0f, maxR = 0.0f;
            for (int i = 0; i < numFrames; i++) {
                float absL = fabsf(output[i * channelCount]);
                float absR = fabsf(output[i * channelCount + 1]);
                if (absL > maxL) maxL = absL;
                if (absR > maxR) maxR = absR;
            }
            masterPeakL = fmaxf(masterPeakL, maxL);
            masterPeakR = fmaxf(masterPeakR, maxR);
        }

        return oboe::DataCallbackResult::Continue;
    }

    // Get current global frame counter (for scheduling notes from Kotlin)
    int64_t getCurrentFrame() {
        return globalFrameCounter;
    }

    // Schedule a note to be played at exact frame
    void scheduleNote(int64_t targetFrame, int sampleId, int trackId,
                      float frequency, float baseFrequency, float volume, float pan = 0.5f,
                      int startPointOverride = -1, int tableId = -1, int tableTicRate = 6,
                      int noteOctave = 4, int notePitch = 0,
                      float pslInitialOffset = 0.0f, float pslDuration = 0.0f,
                      float pbnRate = 0.0f, float vibratoSpeed = 0.0f, float vibratoDepth = 0.0f,
                      int tableStartRow = -1) {
        ScheduledNote note = {
                .targetFrame = targetFrame,
                .sampleId = sampleId,
                .trackId = trackId,
                .frequency = frequency,
                .baseFrequency = baseFrequency,
                .volume = volume,
                .pan = pan,
                .startPointOverride = startPointOverride,
                .tableId = tableId,
                .tableTicRate = tableTicRate,
                .noteOctave = noteOctave,
                .notePitch = notePitch,
                .pslInitialOffset = pslInitialOffset,
                .pslDuration = pslDuration,
                .pbnRate = pbnRate,
                .vibratoSpeed = vibratoSpeed,
                .vibratoDepth = vibratoDepth,
                .tableStartRow = tableStartRow
        };
        noteQueue.schedule(note);
    }

    // Schedule a soundfont note (public method — called from JNI)
    void scheduleSoundfontNote(int64_t targetFrame, int trackId, int sfSlot,
                               int midiNote, int midiVelocity, float vol, float pan,
                               int bank, int preset,
                               float pslInitialOffset, float pslDuration,
                               float pbnRate, float vibratoSpeed, float vibratoDepth) {
        ScheduledNote note{};
        note.targetFrame      = targetFrame;
        note.trackId          = trackId;
        note.isSoundfont      = true;
        note.sfSlot           = sfSlot;
        note.midiNote         = midiNote;
        note.midiVelocity     = midiVelocity;
        note.volume           = vol;
        note.pan              = pan;
        note.sfBank           = bank;
        note.sfPreset         = preset;
        note.sampleId         = -1;
        note.frequency        = 440.0f;
        note.baseFrequency    = 440.0f;
        note.startPointOverride = -1;
        note.tableId          = -1;
        note.tableTicRate     = 6;
        note.noteOctave       = 4;
        note.notePitch        = 0;
        note.pslInitialOffset = pslInitialOffset;
        note.pslDuration      = pslDuration;
        note.pbnRate          = pbnRate;
        note.vibratoSpeed     = vibratoSpeed;
        note.vibratoDepth     = vibratoDepth;
        note.tableStartRow    = -1;
        noteQueue.schedule(note);
    }

    // Schedule a kill event (for Kill effect K00)
    void scheduleKill(int64_t targetFrame, int trackId) {
        ScheduledKill kill = {
                .targetFrame = targetFrame,
                .trackId = trackId
        };
        killQueue.schedule(kill);
    }

    // Schedule a soft note-off (triggers ADSR release instead of hard stop)
    void scheduleNoteOff(int64_t targetFrame, int trackId) {
        ScheduledKill kill = {
                .targetFrame = targetFrame,
                .trackId = trackId,
                .softKill = true
        };
        killQueue.schedule(kill);
    }

    // Clear all scheduled notes
    void clearScheduledNotes() {
        noteQueue.clear();
        killQueue.clear();  // Also clear kill events
    }

    // Clear only notes/kills at or after fromFrame (leaves the current phrase intact)
    void clearScheduledNotesFrom(int64_t fromFrame) {
        noteQueue.clearFrom(fromFrame);
        killQueue.clearFrom(fromFrame);
    }

    // ===================================
    // TABLE METHODS (Phase 3.5)
    // ===================================

    // Load table data from Kotlin
    // rowData format: 16 rows × 8 bytes = 128 bytes
    // Each row: [transpose, volume, fx1Type, fx1Value, fx2Type, fx2Value, fx3Type, fx3Value]
    void loadTable(int tableId, const uint8_t* rowData) {
        if (tableId < 0 || tableId >= 256) return;

        std::lock_guard<std::mutex> lock(tableMutex);
        Table& table = tables[tableId];

        for (int row = 0; row < 16; row++) {
            int offset = row * 8;
            table.rows[row].transpose = (int8_t)rowData[offset + 0];
            table.rows[row].volume = rowData[offset + 1];
            table.rows[row].fx1Type = rowData[offset + 2];
            table.rows[row].fx1Value = rowData[offset + 3];
            table.rows[row].fx2Type = rowData[offset + 4];
            table.rows[row].fx2Value = rowData[offset + 5];
            table.rows[row].fx3Type = rowData[offset + 6];
            table.rows[row].fx3Value = rowData[offset + 7];
        }
        table.loaded = true;

        LOGD("📋 Loaded table %d", tableId);
    }

    // Check if a table is loaded
    bool isTableLoaded(int tableId) {
        if (tableId < 0 || tableId >= 256) return false;
        std::lock_guard<std::mutex> lock(tableMutex);
        return tables[tableId].loaded;
    }

    // Get current table row for a voice (for UI feedback)
    int getVoiceTableRow(int trackId) {
        for (int v = 0; v < MAX_VOICES; v++) {
            if (voices[v].isActive && voices[v].trackId == trackId) {
                return voices[v].tableRow;
            }
        }
        return -1;  // No active voice on this track
    }

    // Get table ID for a voice
    int getVoiceTableId(int trackId) {
        for (int v = 0; v < MAX_VOICES; v++) {
            if (voices[v].isActive && voices[v].trackId == trackId) {
                return voices[v].tableId;
            }
        }
        return -1;  // No active voice on this track
    }

    // Set table row for a voice (THO effect from phrase on empty step)
    void setVoiceTableRow(int trackId, int row) {
        for (int v = 0; v < MAX_VOICES; v++) {
            if (voices[v].isActive && voices[v].trackId == trackId) {
                voices[v].tableRow = row % 16;
                voices[v].lastProcessedRow = -1;  // Force re-processing of new row
                LOGD("📋 THO: Set voice %d (track %d) table row to %d", v, trackId, voices[v].tableRow);
                return;
            }
        }
        LOGD("📋 THO: No active voice on track %d, ignoring", trackId);
    }

    // Get waveform data for oscilloscope display
    void getWaveform(float* outBuffer, int bufferSize) {
        std::lock_guard<std::mutex> lock(waveformMutex);

        // Copy from circular buffer in the correct order
        // Start from current index and wrap around for scrolling effect
        for (int i = 0; i < bufferSize && i < WAVEFORM_SIZE; i++) {
            int readIndex = (waveformIndex + i) % WAVEFORM_SIZE;
            outBuffer[i] = waveformBuffer[readIndex];
        }
    }

    // Get per-track peak levels for mixer meters
    void getTrackPeaks(float* outBuffer) {
        std::lock_guard<std::mutex> lock(peakMutex);
        for (int i = 0; i < 8; i++) {
            outBuffer[i] = trackPeaks[i];
        }
    }

    // Get master peak levels (stereo) for mixer meters
    void getMasterPeaks(float* outBuffer) {
        std::lock_guard<std::mutex> lock(peakMutex);
        outBuffer[0] = masterPeakL;
        outBuffer[1] = masterPeakR;
    }

    // Decay peaks manually (call when audio stream is not running)
    void decayPeaks() {
        std::lock_guard<std::mutex> lock(peakMutex);
        const float MANUAL_DECAY = 0.92f;  // Slightly faster for visual feedback

        for (int t = 0; t < 8; t++) {
            trackPeaks[t] *= MANUAL_DECAY;
            if (trackPeaks[t] < 0.001f) trackPeaks[t] = 0.0f;
        }
        masterPeakL *= MANUAL_DECAY;
        masterPeakR *= MANUAL_DECAY;
        if (masterPeakL < 0.001f) masterPeakL = 0.0f;
        if (masterPeakR < 0.001f) masterPeakR = 0.0f;
    }

    // Decay waveform buffer (call when audio stream is not running)
    void decayWaveform() {
        std::lock_guard<std::mutex> lock(waveformMutex);
        const float WAVEFORM_DECAY = 0.90f;

        for (int i = 0; i < WAVEFORM_SIZE; i++) {
            waveformBuffer[i] *= WAVEFORM_DECAY;
            if (fabsf(waveformBuffer[i]) < 0.001f) waveformBuffer[i] = 0.0f;
        }
    }

    // Set real-time track volume (affects playback immediately, including SF channels)
    void setTrackVolume(int trackId, float volume) {
        if (trackId < 0 || trackId >= 8) return;
        { std::lock_guard<std::mutex> lock(volumeMutex); trackVolumes[trackId] = volume; }
        // For SoundfontVoice the track volume is applied at mix time (trackVolumes[t] * masterVol)
        // so no per-voice update needed. The setVolume call below handles note-level vol only.
        LOGD("🔊 Track %d volume set to %.2f", trackId, volume);
    }

    // Set real-time master volume (affects playback immediately)
    void setMasterVolume(float volume) {
        std::lock_guard<std::mutex> lock(volumeMutex);
        masterVolume = volume;
        LOGD("🔊 Master volume set to %.2f", volume);
    }

    // Get current track volume
    float getTrackVolume(int trackId) {
        if (trackId < 0 || trackId >= 8) return 1.0f;
        std::lock_guard<std::mutex> lock(volumeMutex);
        return trackVolumes[trackId];
    }

    // Get current master volume
    float getMasterVolume() {
        std::lock_guard<std::mutex> lock(volumeMutex);
        return masterVolume;
    }

    // ===================================
    // PITCH MODULATION METHODS (Phase 6)
    // ===================================

    // Returns the active voice for a given track, checking SF voices first.
    // Sampler voices are in the 8-slot pool (any slot may own any track).
    // SF voices are per-track (sfVoices[trackId]).
    IAudioVoice* findActiveVoiceForTrack(int trackId) {
        if (trackId >= 0 && trackId < 8 && sfVoices[trackId].isActive) {
            return &sfVoices[trackId];
        }
        for (int v = 0; v < MAX_VOICES; v++) {
            if (voices[v].isActive && voices[v].trackId == trackId) {
                return &voices[v];
            }
        }
        return nullptr;
    }

    // ── Pitch effect setters — work for both Sampler and SoundFont voices ──

    // Set pitch slide for a voice (PSL effect).
    // Slides from current pitch offset to targetSemitones over durationTicks ticks.
    void setPitchSlide(int trackId, float targetSemitones, float durationTicks, int tempo) {
        IAudioVoice* v = findActiveVoiceForTrack(trackId);
        if (!v) return;
        float sr = stream ? (float)stream->getSampleRate() : 44100.0f;
        float framesPerTic = sr / (tempo / 60.0f * 4.0f * 12.0f);
        float totalFrames = fmaxf(1.0f, framesPerTic * durationTicks);
        v->setPitchSlideRaw(targetSemitones, totalFrames);
        LOGD("🎵 Pitch slide: track=%d, to=%.2f over %.0f frames", trackId, targetSemitones, totalFrames);
    }

    // Set continuous pitch bend (PBN effect).
    // semitonesPerStep is the rate in semitones/step (PBN value / 16); 0 = stop.
    void setPitchBend(int trackId, float semitonesPerStep, int tempo) {
        IAudioVoice* v = findActiveVoiceForTrack(trackId);
        if (!v) return;
        if (fabsf(semitonesPerStep) < 0.0001f) {
            v->setPitchBendRaw(0.0f);
            LOGD("🎵 Pitch bend stopped: track=%d", trackId);
        } else {
            float sr = stream ? (float)stream->getSampleRate() : 44100.0f;
            float framesPerStep = sr / (tempo / 60.0f * 4.0f * 12.0f) * 12.0f;
            float ratePerFrame = semitonesPerStep / framesPerStep;
            v->setPitchBendRaw(ratePerFrame);
            LOGD("🎵 Pitch bend: track=%d, rate=%.4f semitones/step", trackId, semitonesPerStep);
        }
    }

    // Set vibrato (PVB/PVX effect). depth=0 stops vibrato.
    void setVibrato(int trackId, float speed, float depth) {
        IAudioVoice* v = findActiveVoiceForTrack(trackId);
        if (!v) return;
        v->setVibratoRaw(speed, depth);
        if (depth < 0.01f) {
            LOGD("🎵 Vibrato stopped: track=%d", trackId);
        } else {
            LOGD("🎵 Vibrato: track=%d, speed=%.1fHz, depth=%.2f semitones", trackId, speed, depth);
        }
    }

    // Clear all pitch modulation for a voice (PSL/PBN/PVB/PVX reset).
    void clearPitchMod(int trackId) {
        IAudioVoice* v = findActiveVoiceForTrack(trackId);
        if (!v) return;
        v->clearPitchMod();
        LOGD("🎵 Pitch mod cleared: track=%d", trackId);
    }

    // Set initial pitch offset (PSL setup: call before setPitchSlide).
    void setInitialPitchOffset(int trackId, float semitones) {
        IAudioVoice* v = findActiveVoiceForTrack(trackId);
        if (!v) return;
        v->setInitialPitchOffset(semitones);
        LOGD("🎵 Pitch offset set: track=%d, offset=%.2f semitones", trackId, semitones);
    }

    // ===================================
    // MODULATION METHODS (Phase 4 — AHD)
    // ===================================

    // Set per-instrument modulation slot (called from Kotlin before scheduling each note)
    // attackSamples/holdSamples/decaySamples/releaseSamples are converted from ticks by Kotlin.
    // sustainLevel: ADSR sustain level 0.0-1.0
    // lfoHz: LFO frequency in Hz; oscShape: LFO shape index (0=TRI,1=SIN,...)
    // releaseSamples: ADSR/TRIG release duration (0 = instant cut on triggerNoteOff)
    void setInstrumentModulation(int sampleId, int slotIndex,
                                 int type, int dest, float amount,
                                 int attackSamples, int holdSamples, int decaySamples,
                                 float sustainLevel, float lfoHz, int oscShape,
                                 int releaseSamples = 0) {
        if (sampleId < 0 || sampleId >= 256 || slotIndex < 0 || slotIndex >= 4) return;
        InstrumentModSlot& slot = instrumentModSlots[sampleId][slotIndex];
        slot.type = type;
        slot.dest = dest;
        slot.amount = amount;
        slot.attackSamples = attackSamples;
        slot.holdSamples = holdSamples;
        slot.decaySamples = decaySamples;
        slot.sustainLevel = sustainLevel;
        slot.lfoHz = lfoHz;
        slot.oscShape = oscShape;
        slot.releaseSamples = releaseSamples;
    }

    // Smart note-off: trigger ADSR/TRIG release if available, otherwise hard-stop.
    // Called at the K00 kill frame (via scheduleNoteOff softKill) or at step end.
    // - ADSR/TRIG with releaseSamples > 0 and active (stage 1-3): transitions to Release.
    // - ADSR/TRIG already in Release (stage 4): left to play out.
    // - AHD, DRUM, LFO, or no vol mod: hard-stops the voice immediately.
    void triggerNoteOff(int trackId) {
        for (int v = 0; v < MAX_VOICES; v++) {
            if (!voices[v].isActive || voices[v].trackId != trackId) continue;
            bool hasRelease = false;
            for (int m = 0; m < 4; m++) {
                Voice::VoiceModSlot& mod = voices[v].voiceMods[m];
                if (mod.dest == 1 && (mod.type == 2 || mod.type == 5)) {
                    if (mod.stage >= 1 && mod.stage <= 3 && mod.releaseSamples > 0) {
                        // Transition Attack / Decay / Sustain → Release
                        mod.stage = 4;
                        mod.stageCounter = 0;
                        // envValue stays at current level; release ramps it to 0
                        hasRelease = true;
                    } else if (mod.stage == 4) {
                        // Already in Release — let it play out naturally
                        hasRelease = true;
                    }
                }
            }
            if (!hasRelease) {
                // No ADSR release available (AHD / DRUM / LFO / no-mod / release=0)
                // → hard-stop so looping samples don't play forever
                voices[v].stop();
            }
        }
    }

    // Clear all modulation slots for an instrument
    void clearInstrumentModulation(int sampleId) {
        if (sampleId < 0 || sampleId >= 256) return;
        for (int m = 0; m < 4; m++) {
            instrumentModSlots[sampleId][m] = InstrumentModSlot();
        }
    }

    // Advance modulation stages for one voice (called once per audio callback).
    // Handles AHD (type=1), ADSR (type=2), LFO (type=3), DRUM (type=4), TRIG (type=5).
    // Phase 4.4: Mod-to-mod routing: dest=8 (MOD_AMT), 9 (MOD_RATE), 10 (MOD_BOTH).
    // Accumulates modPitchOffset from PITCH-destination (dest=3) slots.
    void updateVoiceModulation(Voice& voice, int numFrames, float sampleRate = 44100.0f) {
        voice.modPitchOffset = 0.0f;  // Reset accumulators each callback
        voice.modPanOffset = 0.0f;
        voice.modCutOffset = 0.0f;
        voice.modResOffset = 0.0f;

        float sr = sampleRate;

        // ── Phase 4.4: Mod-to-mod routing ──────────────────────────────────────
        // Each slot routes its output to the NEXT slot (0→1, 1→2, 2→3, 3→0).
        // Uses previous frame's envValue to avoid circular-dependency issues.
        // amtScale[N] multiplies slot N's effective amount.
        // rateMult[N] multiplies slot N's effective time/freq (higher = faster).
        {
            float amtScale[4]  = {1.0f, 1.0f, 1.0f, 1.0f};
            float rateMult[4]  = {1.0f, 1.0f, 1.0f, 1.0f};
            for (int m = 0; m < 4; m++) {
                const Voice::VoiceModSlot& src = voice.voiceMods[m];
                if (src.type == 0 || src.stage == 0) continue;
                if (src.dest != 8 && src.dest != 9 && src.dest != 10) continue;
                int target = (m + 1) % 4;
                // Normalize envValue to 0-1: envelopes are 0-1, LFOs are -1 to +1
                float norm = (src.type == 3) ? (src.envValue * 0.5f + 0.5f)
                                              : fmaxf(0.0f, src.envValue);
                // src.amount (depth of routing): 0.5 → 1.0x scaling at full signal
                float scale = fminf(2.0f, norm * src.amount * 2.0f);
                if (src.dest == 8 || src.dest == 10) amtScale[target] *= scale;
                if (src.dest == 9 || src.dest == 10) rateMult[target] *= fmaxf(0.05f, scale);
            }
            for (int m = 0; m < 4; m++) {
                voice.voiceMods[m].effectiveAmt      = voice.voiceMods[m].amount * amtScale[m];
                voice.voiceMods[m].effectiveRateMult = rateMult[m];
            }
        }
        // ───────────────────────────────────────────────────────────────────────

        for (int m = 0; m < 4; m++) {
            Voice::VoiceModSlot& mod = voice.voiceMods[m];
            if (mod.type == 0 || mod.stage == 0) continue;

            if (mod.type == 1 || mod.type == 4) {
                // ── AHD / DRUM: Attack → Hold → Decay ──
                if (mod.stage == 4) continue;
                mod.stageCounter += numFrames;
                // Effective stage durations: higher rateMult → shorter stages (faster)
                float rMult = fmaxf(0.01f, mod.effectiveRateMult);
                int effAttack = mod.attackSamples > 0 ? (int)fmaxf(1.0f, mod.attackSamples / rMult) : 0;
                int effHold   = mod.holdSamples   > 0 ? (int)fmaxf(1.0f, mod.holdSamples   / rMult) : 0;
                int effDecay  = mod.decaySamples  > 0 ? (int)fmaxf(1.0f, mod.decaySamples  / rMult) : 0;
                switch (mod.stage) {
                    case 1: // Attack: ramp 0 → 1
                        if (effAttack > 0) {
                            mod.envValue = fminf(1.0f, (float)mod.stageCounter / effAttack);
                            if (mod.stageCounter >= effAttack) {
                                mod.envValue = 1.0f;
                                mod.stage = 2; mod.stageCounter = 0;
                            }
                        } else {
                            mod.envValue = 1.0f;
                            mod.stage = 2; mod.stageCounter = 0;
                        }
                        break;
                    case 2: // Hold: stay at 1
                        mod.envValue = 1.0f;
                        if (effHold == 0 || mod.stageCounter >= effHold) {
                            mod.stage = 3; mod.stageCounter = 0;
                        }
                        break;
                    case 3: // Decay: ramp 1 → 0
                        if (effDecay > 0) {
                            mod.envValue = fmaxf(0.0f, 1.0f - (float)mod.stageCounter / effDecay);
                            if (mod.stageCounter >= effDecay) {
                                mod.envValue = 0.0f;
                                mod.stage = 4;
                            }
                        } else {
                            mod.envValue = 0.0f;
                            mod.stage = 4;
                        }
                        break;
                }

            } else if (mod.type == 2 || mod.type == 5) {
                // ── ADSR / TRIG: Attack → Decay → Sustain → Release ──
                // Stage 5 = done (unlike AHD where stage 4 = done)
                if (mod.stage == 5) continue;
                mod.stageCounter += numFrames;
                float rMult    = fmaxf(0.01f, mod.effectiveRateMult);
                int effAttack  = mod.attackSamples  > 0 ? (int)fmaxf(1.0f, mod.attackSamples  / rMult) : 0;
                int effDecay   = mod.decaySamples   > 0 ? (int)fmaxf(1.0f, mod.decaySamples   / rMult) : 0;
                int effRelease = mod.releaseSamples > 0 ? (int)fmaxf(1.0f, mod.releaseSamples / rMult) : 0;
                switch (mod.stage) {
                    case 1: // Attack: ramp 0 → 1
                        if (effAttack > 0) {
                            mod.envValue = fminf(1.0f, (float)mod.stageCounter / effAttack);
                            if (mod.stageCounter >= effAttack) {
                                mod.envValue = 1.0f;
                                mod.stage = 2; mod.stageCounter = 0;
                            }
                        } else {
                            mod.envValue = 1.0f;
                            mod.stage = 2; mod.stageCounter = 0;
                        }
                        break;
                    case 2: // Decay: ramp 1 → sustainLevel
                        if (effDecay > 0) {
                            float t = fminf(1.0f, (float)mod.stageCounter / effDecay);
                            mod.envValue = 1.0f - t * (1.0f - mod.sustainLevel);
                            if (mod.stageCounter >= effDecay) {
                                mod.envValue = mod.sustainLevel;
                                mod.stage = 3; mod.stageCounter = 0;
                            }
                        } else {
                            mod.envValue = mod.sustainLevel;
                            mod.stage = 3; mod.stageCounter = 0;
                        }
                        break;
                    case 3: // Sustain: hold at sustainLevel (until triggerNoteOff)
                        mod.envValue = mod.sustainLevel;
                        break;
                    case 4: // Release: ramp sustainLevel → 0 (triggered by triggerNoteOff)
                        if (effRelease > 0) {
                            mod.envValue = fmaxf(0.0f,
                                mod.sustainLevel * (1.0f - (float)mod.stageCounter / effRelease));
                            if (mod.stageCounter >= effRelease) {
                                mod.envValue = 0.0f;
                                mod.stage = 5;
                            }
                        } else {
                            mod.envValue = 0.0f;
                            mod.stage = 5;
                        }
                        break;
                }

            } else if (mod.type == 3) {
                // ── LFO: phase-based oscillator ──
                // effectiveRateMult speeds up LFO frequency (2.0 = 2× faster)
                float effHz       = mod.lfoHz * mod.effectiveRateMult;
                float phaseAdvance = 2.0f * (float)M_PI * effHz / sr * numFrames;
                mod.lfoPhase += phaseAdvance;
                while (mod.lfoPhase >= 2.0f * (float)M_PI) mod.lfoPhase -= 2.0f * (float)M_PI;

                float norm = mod.lfoPhase / (2.0f * (float)M_PI);  // 0.0 to 1.0
                switch (mod.oscShape) {
                    case 0: // TRI: triangle wave (-1 to +1)
                        if      (norm < 0.25f) mod.envValue = norm * 4.0f;
                        else if (norm < 0.75f) mod.envValue = 1.0f - (norm - 0.25f) * 4.0f;
                        else                   mod.envValue = (norm - 1.0f) * 4.0f;
                        break;
                    case 1: // SIN
                        mod.envValue = sinf(mod.lfoPhase);
                        break;
                    case 2: // RMP+ (sawtooth rising: -1 to +1)
                        mod.envValue = norm * 2.0f - 1.0f;
                        break;
                    case 3: // RMP- (sawtooth falling: +1 to -1)
                        mod.envValue = 1.0f - norm * 2.0f;
                        break;
                    case 6: // SQU+ (square, starts high)
                        mod.envValue = (norm < 0.5f) ? 1.0f : -1.0f;
                        break;
                    case 7: // SQU- (square, starts low)
                        mod.envValue = (norm < 0.5f) ? -1.0f : 1.0f;
                        break;
                    default: // EXP+/EXP-/RND/DRNK — fall back to SIN
                        mod.envValue = sinf(mod.lfoPhase);
                        break;
                }
            }

            // Accumulate modulation to destinations
            // Done stages (AHD=4, ADSR=5) have envValue=0 so contribute nothing
            switch (mod.dest) {
                case 2: // PAN: ±0.5 range (shifts centre ±0.5)
                    voice.modPanOffset += mod.envValue * mod.effectiveAmt * 0.5f;
                    break;
                case 3: // PITCH: up to ±12 semitones (1 octave)
                    voice.modPitchOffset += mod.envValue * mod.effectiveAmt * 12.0f;
                    break;
                case 4: // FINE_PITCH: up to ±1 semitone (fine detune / subtle vibrato)
                    voice.modPitchOffset += mod.envValue * mod.effectiveAmt * 1.0f;
                    break;
                case 5: // FILTER_CUTOFF: up to ±255 cutoff param units
                    voice.modCutOffset += mod.envValue * mod.effectiveAmt * 255.0f;
                    break;
                case 6: // FILTER_RES: up to ±255 resonance param units
                    voice.modResOffset += mod.envValue * mod.effectiveAmt * 255.0f;
                    break;
                default:
                    break; // VOL(1) handled in mix loop, STA(7)/MOD_*(8-10) handled elsewhere
            }
        }
    }

    // Update pitch modulation for a single voice (called per frame in audio callback)
    void updateVoicePitchMod(Voice& voice, int numFrames, float sampleRate) {
        // Process pitch slide
        if (voice.pitchSliding) {
            float delta = voice.pitchSlideTarget - voice.pitchOffset;

            // Check if we've reached or passed the target
            float totalDelta = voice.pitchSlideRate * numFrames;
            if (fabsf(totalDelta) >= fabsf(delta)) {
                // Reached target
                voice.pitchOffset = voice.pitchSlideTarget;

                // Only stop if this was a PSL (finite slide), not PBN (continuous)
                // PBN has extreme target values (±127)
                if (fabsf(voice.pitchSlideTarget) < 100.0f) {
                    voice.pitchSliding = false;
                }
            } else {
                voice.pitchOffset += totalDelta;
            }
        }

        // Process vibrato LFO
        if (voice.vibratoActive) {
            // Advance LFO phase
            // Phase increment per frame = 2π × frequency / sample_rate
            float phaseIncrement = (2.0f * (float)M_PI * voice.vibratoSpeed / sampleRate) * numFrames;
            voice.vibratoPhase += phaseIncrement;

            // Wrap phase to [0, 2π]
            while (voice.vibratoPhase >= 2.0f * (float)M_PI) {
                voice.vibratoPhase -= 2.0f * (float)M_PI;
            }
        }
    }

    // Get modulated playback rate including pitch offset, vibrato, and mod-slot pitch
    float getModulatedPlaybackRate(Voice& voice) {
        float pitchMod = voice.pitchOffset + voice.modPitchOffset;

        // Add vibrato modulation (sine wave)
        if (voice.vibratoActive) {
            pitchMod += sinf(voice.vibratoPhase) * voice.vibratoDepth;
        }

        // Convert total semitones to rate multiplier
        // rate = 2^(semitones/12)
        float rateMod = powf(2.0f, pitchMod / 12.0f);

        return voice.playbackRate * rateMod;
    }

    // ===================================
    // OFFLINE RENDER (for WAV export — thin wrapper)
    // ===================================
    // Delegates entirely to processAudioBlock for identical behavior to live playback.
    // Tables, modulation, anti-click, limiter — all work automatically.
    void renderOffline(int numFrames, float* output, int sampleRate) {
        for (int i = 0; i < numFrames * 2; i++) output[i] = 0.0f;

        const int BLOCK_SIZE = 256;  // Match live callback granularity for identical behavior
        int rendered = 0;
        while (rendered < numFrames) {
            int chunk = std::min(BLOCK_SIZE, numFrames - rendered);
            processAudioBlock(output + rendered * 2, chunk, 2, (float)sampleRate);
            rendered += chunk;
        }
        // globalFrameCounter already updated by processAudioBlock each chunk
    }

    // Reset frame counter (for starting a new render)
    void resetFrameCounter() {
        globalFrameCounter = 0;
    }

    // Get current frame counter
    int64_t getFrameCounter() {
        return globalFrameCounter;
    }

    // Offline rendering flag: when true, onAudioReady outputs silence instead of audio.
    // Prevents the live stream from consuming note queue entries during WAV export.
    void setOfflineRendering(bool offline) {
        isOfflineRendering.store(offline);
        LOGD("🎬 Offline rendering: %s", offline ? "ON" : "OFF");
    }

private:
    std::shared_ptr<oboe::AudioStream> stream;
    Voice voices[MAX_VOICES];
    float* samples[256];
    int sampleLengths[256];
    InstrumentParams instrumentParams[256];
    InstrumentModSlot instrumentModSlots[256][4]; // [sampleId][slotIndex]

    // Table data (Phase 3.5)
    Table tables[256];             // 256 tables, each with 16 rows
    std::mutex tableMutex;         // Protect table data during load/access

    // PHASE 1: Sample-accurate timing infrastructure
    NoteQueue noteQueue;           // Thread-safe queue of scheduled notes
    KillQueue killQueue;           // Thread-safe queue of scheduled kill events
    int64_t globalFrameCounter;    // Total frames processed since start
    std::atomic<bool> isOfflineRendering{false};  // True during WAV export → onAudioReady outputs silence

    // Oscilloscope waveform buffer (circular buffer for recent output)
    static const int WAVEFORM_SIZE = 620;
    float waveformBuffer[WAVEFORM_SIZE];
    int waveformIndex = 0;
    std::mutex waveformMutex;

    // Per-block per-track peaks: written by processAudioBlock, read by onAudioReady for meters
    float framePeaksPerTrack[8] = {0};

    // Peak level tracking for mixer meters
    float trackPeaks[8] = {0};      // Per-track peak levels (0.0 - 1.0)
    float masterPeakL = 0;          // Master left channel peak
    float masterPeakR = 0;          // Master right channel peak
    std::mutex peakMutex;
    static constexpr float PEAK_DECAY = 0.95f;  // Decay rate per callback (smooth falloff)

    // Real-time volume control (can be changed without rescheduling notes)
    float trackVolumes[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    float masterVolume = 1.0f;
    std::mutex volumeMutex;

    // Downsampling for oscilloscope (capture every Nth sample)
    // Lower = faster scrolling (more zoomed in), Higher = slower scrolling (more time visible)
    // Adjust this value to control oscilloscope speed:
    //   1 = 14ms visible (super fast), 10 = 140ms, 20 = 280ms, 50 = 700ms, etc.
    static const int WAVEFORM_DOWNSAMPLE = 1;  // Capture every 10th sample
    int waveformDownsampleCounter = 0;
};

static AudioEngine* engine = nullptr;

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1create(JNIEnv *env, jobject thiz) {
    if (!engine) {
        engine = new AudioEngine();
        return engine->openStream() ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1delete(JNIEnv *env, jobject thiz) {
    delete engine;
    engine = nullptr;
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1loadSample(
        JNIEnv *env, jobject thiz, jint id, jfloatArray data) {
    if (!engine) return;

    jsize len = env->GetArrayLength(data);
    jfloat* arr = env->GetFloatArrayElements(data, nullptr);

    engine->loadSample(id, arr, len);

    env->ReleaseFloatArrayElements(data, arr, JNI_ABORT);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1triggerNote(
        JNIEnv *env, jobject thiz, jint sid, jint tid, jfloat f, jfloat bf, jfloat v) {
    if (engine) {
        engine->triggerNote(sid, tid, f, bf, v);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1stopTrack(JNIEnv *env, jobject thiz, jint tid) {
    if (engine) {
        engine->stopTrack(tid);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1stopAll(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->stopAll();
    }
}

JNIEXPORT jint JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1getActiveVoiceCount(JNIEnv *env, jobject thiz) {
    if (engine) {
        return engine->getActiveVoiceCount();
    }
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1getSampleRate(JNIEnv *env, jobject thiz) {
    if (engine) {
        return engine->getSampleRate();
    }
    return 48000; // Default fallback
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1setInstrumentParams(
        JNIEnv *env, jobject thiz, jint instrumentId, jint start, jint end,
        jboolean reverse, jint loopMode, jint loopStart,
        jint drive, jint crush, jint downsample,
        jint filterType, jint filterCut, jint filterRes) {
    if (engine) {
        engine->setInstrumentParams(instrumentId, start, end, reverse, loopMode, loopStart,
                                    drive, crush, downsample, filterType, filterCut, filterRes);
    }
}

// ===================================
// PHASE 1: NOTE QUEUE JNI METHODS
// ===================================

JNIEXPORT jlong JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1getCurrentFrame(JNIEnv *env, jobject thiz) {
    if (engine) {
        return (jlong)engine->getCurrentFrame();
    }
    return 0;
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1scheduleNote(
        JNIEnv *env, jobject thiz, jlong targetFrame, jint sampleId, jint trackId,
        jfloat frequency, jfloat baseFrequency, jfloat volume) {
    if (engine) {
        engine->scheduleNote(targetFrame, sampleId, trackId, frequency, baseFrequency, volume);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1clearScheduledNotes(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->clearScheduledNotes();
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1resumeStream(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->resumeStream();
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_TrackerAudioEngine_native_1getWaveform(JNIEnv *env, jobject thiz, jfloatArray outArray) {
    if (engine && outArray != nullptr) {
        jsize length = env->GetArrayLength(outArray);
        float* buffer = new float[length];

        // Get waveform from engine
        engine->getWaveform(buffer, length);

        // Copy to Java array
        env->SetFloatArrayRegion(outArray, 0, length, buffer);

        delete[] buffer;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// JNI METHODS FOR OboeAudioBackend (Refactoring Phase 1)
// ═══════════════════════════════════════════════════════════════════════════
// These are the new platform-agnostic JNI methods that match the OboeAudioBackend class
// in platform/android/OboeAudioBackend.kt

JNIEXPORT jboolean JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1create(JNIEnv *env, jobject thiz) {
    if (!engine) {
        engine = new AudioEngine();
        return engine->openStream() ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1delete(JNIEnv *env, jobject thiz) {
    if (engine) {
        delete engine;
        engine = nullptr;
        LOGD("✅ Audio engine deleted");
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1loadSample(
        JNIEnv *env, jobject thiz, jint id, jfloatArray data) {
    if (!engine) return;

    jsize len = env->GetArrayLength(data);
    jfloat* arr = env->GetFloatArrayElements(data, nullptr);

    engine->loadSample(id, arr, len);

    env->ReleaseFloatArrayElements(data, arr, JNI_ABORT);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1clearAllSamples(
        JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->clearAllSamples();
    }
}

JNIEXPORT jintArray JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getTrackActiveNotes(
        JNIEnv *env, jobject thiz) {
    jintArray result = env->NewIntArray(8);
    if (!result) return result;
    int notes[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    if (engine) {
        engine->getTrackActiveNotes(notes, 8);
    }
    env->SetIntArrayRegion(result, 0, 8, notes);
    return result;
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1scheduleNote(
        JNIEnv *env, jobject thiz, jlong targetFrame, jint sampleId, jint trackId,
        jfloat frequency, jfloat baseFrequency, jfloat volume, jfloat pan, jint startPointOverride) {
    if (engine) {
        engine->scheduleNote(targetFrame, sampleId, trackId, frequency, baseFrequency, volume, pan, startPointOverride);
    }
}

JNIEXPORT jlong JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getCurrentFrame(JNIEnv *env, jobject thiz) {
    if (engine) {
        return (jlong)engine->getCurrentFrame();
    }
    return 0;
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1clearScheduledNotes(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->clearScheduledNotes();
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1clearScheduledNotesFrom(
        JNIEnv *env, jobject thiz, jlong fromFrame) {
    if (engine) {
        engine->clearScheduledNotesFrom((int64_t)fromFrame);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1resumeStream(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->resumeStream();
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1stopAll(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->stopAll();
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1killTrack(JNIEnv *env, jobject thiz, jint trackId) {
    if (engine) {
        engine->stopTrack(trackId);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1scheduleKill(JNIEnv *env, jobject thiz, jlong targetFrame, jint trackId) {
    if (engine) {
        engine->scheduleKill(targetFrame, trackId);
    }
}

JNIEXPORT jint JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getSampleRate(JNIEnv *env, jobject thiz) {
    if (engine) {
        return engine->getSampleRate();
    }
    return 44100; // Default fallback
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getWaveform(JNIEnv *env, jobject thiz, jfloatArray outArray) {
    if (engine && outArray != nullptr) {
        jsize length = env->GetArrayLength(outArray);
        float* buffer = new float[length];

        // Get waveform from engine
        engine->getWaveform(buffer, length);

        // Copy to Java array
        env->SetFloatArrayRegion(outArray, 0, length, buffer);

        delete[] buffer;
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setInstrumentParams(
        JNIEnv *env, jobject thiz, jint instrumentId, jint start, jint end,
        jboolean reverse, jint loopMode, jint loopStart,
        jint drive, jint crush, jint downsample,
        jint filterType, jint filterCut, jint filterRes) {
    if (engine) {
        engine->setInstrumentParams(instrumentId, start, end, reverse, loopMode, loopStart,
                                    drive, crush, downsample, filterType, filterCut, filterRes);
    }
}

// ===================================
// MIXER PEAK METER JNI METHODS
// ===================================

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getTrackPeaks(JNIEnv *env, jobject thiz, jfloatArray outArray) {
    if (engine && outArray != nullptr) {
        float buffer[8];
        engine->getTrackPeaks(buffer);
        env->SetFloatArrayRegion(outArray, 0, 8, buffer);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getMasterPeaks(JNIEnv *env, jobject thiz, jfloatArray outArray) {
    if (engine && outArray != nullptr) {
        float buffer[2];
        engine->getMasterPeaks(buffer);
        env->SetFloatArrayRegion(outArray, 0, 2, buffer);
    }
}

// ===================================
// OFFLINE RENDER JNI METHODS (for WAV export)
// ===================================

JNIEXPORT jfloatArray JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1renderFrames(
        JNIEnv *env, jobject thiz, jint numFrames, jint sampleRate) {
    if (!engine) {
        return nullptr;
    }

    // Create output array for interleaved stereo
    jfloatArray result = env->NewFloatArray(numFrames * 2);
    if (result == nullptr) {
        return nullptr;
    }

    // Allocate temporary buffer
    std::vector<float> buffer(numFrames * 2);

    // Render frames
    engine->renderOffline(numFrames, buffer.data(), sampleRate);

    // Copy to Java array
    env->SetFloatArrayRegion(result, 0, numFrames * 2, buffer.data());

    return result;
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1resetFrameCounter(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->resetFrameCounter();
    }
}

JNIEXPORT jlong JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getFrameCounter(JNIEnv *env, jobject thiz) {
    if (engine) {
        return engine->getFrameCounter();
    }
    return 0;
}

// ===================================
// PHASE 1 BUG FIXES: DECAY AND REAL-TIME VOLUME
// ===================================

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1decayPeaks(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->decayPeaks();
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1decayWaveform(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->decayWaveform();
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setTrackVolume(JNIEnv *env, jobject thiz, jint trackId, jfloat volume) {
    if (engine) {
        engine->setTrackVolume(trackId, volume);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setMasterVolume(JNIEnv *env, jobject thiz, jfloat volume) {
    if (engine) {
        engine->setMasterVolume(volume);
    }
}

// ===================================
// TABLE JNI METHODS (Phase 3.5)
// ===================================

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1loadTable(
        JNIEnv *env, jobject thiz, jint tableId, jbyteArray rowData) {
    if (!engine || rowData == nullptr) return;

    jsize len = env->GetArrayLength(rowData);
    if (len != 128) {
        LOGE("❌ loadTable: Invalid rowData length %d (expected 128)", len);
        return;
    }

    jbyte* data = env->GetByteArrayElements(rowData, nullptr);
    engine->loadTable(tableId, reinterpret_cast<uint8_t*>(data));
    env->ReleaseByteArrayElements(rowData, data, JNI_ABORT);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1scheduleNoteWithTable(
        JNIEnv *env, jobject thiz, jlong targetFrame, jint sampleId, jint trackId,
        jfloat frequency, jfloat baseFrequency, jfloat volume, jfloat pan,
        jint startPointOverride, jint tableId, jint tableTicRate,
        jint noteOctave, jint notePitch,
        jfloat pslInitialOffset, jfloat pslDuration,
        jfloat pbnRate, jfloat vibratoSpeed, jfloat vibratoDepth,
        jint tableStartRow) {
    if (engine) {
        engine->scheduleNote(targetFrame, sampleId, trackId, frequency, baseFrequency,
                             volume, pan, startPointOverride, tableId, tableTicRate,
                             noteOctave, notePitch,
                             pslInitialOffset, pslDuration, pbnRate, vibratoSpeed, vibratoDepth,
                             tableStartRow);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setVoiceTableRow(
        JNIEnv *env, jobject thiz, jint trackId, jint row) {
    if (engine) {
        engine->setVoiceTableRow(trackId, row);
    }
}

JNIEXPORT jint JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getVoiceTableRow(
        JNIEnv *env, jobject thiz, jint trackId) {
    if (engine) {
        return engine->getVoiceTableRow(trackId);
    }
    return -1;
}

JNIEXPORT jint JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getVoiceTableId(
        JNIEnv *env, jobject thiz, jint trackId) {
    if (engine) {
        return engine->getVoiceTableId(trackId);
    }
    return -1;
}

// ===================================
// PITCH MODULATION JNI METHODS (Phase 6)
// ===================================

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setPitchSlide(
        JNIEnv *env, jobject thiz, jint trackId, jfloat targetSemitones, jfloat durationTicks, jint tempo) {
    if (engine) {
        engine->setPitchSlide(trackId, targetSemitones, durationTicks, tempo);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setPitchBend(
        JNIEnv *env, jobject thiz, jint trackId, jfloat semitonesPerTick, jint tempo) {
    if (engine) {
        engine->setPitchBend(trackId, semitonesPerTick, tempo);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setVibrato(
        JNIEnv *env, jobject thiz, jint trackId, jfloat speed, jfloat depth) {
    if (engine) {
        engine->setVibrato(trackId, speed, depth);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1clearPitchMod(
        JNIEnv *env, jobject thiz, jint trackId) {
    if (engine) {
        engine->clearPitchMod(trackId);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setInitialPitchOffset(
        JNIEnv *env, jobject thiz, jint trackId, jfloat semitones) {
    if (engine) {
        engine->setInitialPitchOffset(trackId, semitones);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setInstrumentModulation(
        JNIEnv *env, jobject thiz,
        jint sampleId, jint slotIndex, jint type, jint dest, jfloat amount,
        jint attackSamples, jint holdSamples, jint decaySamples,
        jfloat sustainLevel, jfloat lfoHz, jint oscShape, jint releaseSamples) {
    if (engine) {
        engine->setInstrumentModulation(sampleId, slotIndex, type, dest, amount,
                                        attackSamples, holdSamples, decaySamples,
                                        sustainLevel, lfoHz, oscShape, releaseSamples);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1clearInstrumentModulation(
        JNIEnv *env, jobject thiz, jint sampleId) {
    if (engine) {
        engine->clearInstrumentModulation(sampleId);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1triggerNoteOff(
        JNIEnv *env, jobject thiz, jint trackId) {
    if (engine) {
        engine->triggerNoteOff(trackId);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1scheduleNoteOff(
        JNIEnv *env, jobject thiz, jlong targetFrame, jint trackId) {
    if (engine) {
        engine->scheduleNoteOff(targetFrame, trackId);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setOfflineRendering(
        JNIEnv *env, jobject thiz, jboolean rendering) {
    if (engine) engine->setOfflineRendering(rendering == JNI_TRUE);
}

// ===================================
// SOUNDFONT JNI FUNCTIONS
// ===================================

JNIEXPORT jint JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1loadSoundfont(
        JNIEnv *env, jobject thiz, jint instrumentId, jstring path) {
    const char* pathStr = env->GetStringUTFChars(path, nullptr);
    if (!pathStr) return -1;

    // Find a free slot; if none, evict the slot whose instrumentId is lowest (oldest heuristic)
    int slot = -1;
    for (int i = 0; i < MAX_SOUNDFONTS; i++) {
        if (soundfonts[i].handle == nullptr) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        // Evict slot with smallest instrumentId (oldest loaded)
        int minId = INT_MAX;
        for (int i = 0; i < MAX_SOUNDFONTS; i++) {
            if (soundfonts[i].instrumentId < minId) {
                minId = soundfonts[i].instrumentId;
                slot = i;
            }
        }
        // Free the evicted slot
        std::lock_guard<std::mutex> sfLock(soundfonts[slot].mutex);
        tsf_close(soundfonts[slot].handle);
        soundfonts[slot].handle = nullptr;
        soundfonts[slot].fileData.clear();
        soundfonts[slot].instrumentId = -1;
        soundfonts[slot].filePath.clear();
        // Close any per-track voices cloned from this slot
        for (int t = 0; t < 8; t++) {
            if (sfVoices[t].sourceSfSlot == slot) sfVoices[t].close();
        }
        LOGD("🎹 Evicted soundfont slot %d to make room for instrumentId %d", slot, (int)instrumentId);
    }

    // Load file bytes into memory for per-track cloning
    {
        FILE* f = fopen(pathStr, "rb");
        if (!f) {
            LOGE("❌ Failed to open soundfont: %s", pathStr);
            env->ReleaseStringUTFChars(path, pathStr);
            return -1;
        }
        fseek(f, 0, SEEK_END);
        long fileSize = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (fileSize <= 0) {
            LOGE("❌ Empty soundfont file: %s", pathStr);
            fclose(f);
            env->ReleaseStringUTFChars(path, pathStr);
            return -1;
        }

        std::lock_guard<std::mutex> sfLock(soundfonts[slot].mutex);
        soundfonts[slot].fileData.resize((size_t)fileSize);
        if (fread(soundfonts[slot].fileData.data(), 1, (size_t)fileSize, f) != (size_t)fileSize) {
            LOGE("❌ Failed to read soundfont: %s", pathStr);
            fclose(f);
            soundfonts[slot].fileData.clear();
            env->ReleaseStringUTFChars(path, pathStr);
            return -1;
        }
        fclose(f);

        // Create master handle from memory for preset queries (not used for audio rendering)
        soundfonts[slot].handle = tsf_load_memory(soundfonts[slot].fileData.data(),
                                                   (int)soundfonts[slot].fileData.size());
        if (!soundfonts[slot].handle) {
            LOGE("❌ Failed to parse soundfont: %s", pathStr);
            soundfonts[slot].fileData.clear();
            env->ReleaseStringUTFChars(path, pathStr);
            return -1;
        }
        int sampleRate = engine ? engine->getSampleRate() : 44100;
        tsf_set_output(soundfonts[slot].handle, TSF_STEREO_INTERLEAVED, sampleRate, 0.0f);
        soundfonts[slot].instrumentId = (int)instrumentId;
        soundfonts[slot].filePath = pathStr;
    }

    env->ReleaseStringUTFChars(path, pathStr);
    LOGD("🎹 Loaded soundfont slot %d for instrumentId %d", slot, (int)instrumentId);
    return slot;
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1setSoundfontPreset(
        JNIEnv *env, jobject thiz, jint sfSlot, jint bank, jint preset) {
    if (sfSlot < 0 || sfSlot >= MAX_SOUNDFONTS || !soundfonts[sfSlot].handle) return;
    // Preset is set per-note via tsf_channel_set_presetnumber; this is a no-op stored in Kotlin.
    LOGD("🎹 setSoundfontPreset slot=%d bank=%d preset=%d (applied per-note)", (int)sfSlot, (int)bank, (int)preset);
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1scheduleSoundfontNote(
        JNIEnv *env, jobject thiz, jlong frame, jint trackId, jint sfSlot,
        jint midiNote, jint velocity, jfloat vol, jfloat pan, jint bank, jint preset,
        jfloat pslInitialOffset, jfloat pslDuration, jfloat pbnRate,
        jfloat vibratoSpeed, jfloat vibratoDepth) {
    if (engine) {
        engine->scheduleSoundfontNote((int64_t)frame, (int)trackId, (int)sfSlot,
                                      (int)midiNote, (int)velocity,
                                      (float)vol, (float)pan, (int)bank, (int)preset,
                                      (float)pslInitialOffset, (float)pslDuration,
                                      (float)pbnRate, (float)vibratoSpeed, (float)vibratoDepth);
    }
}

JNIEXPORT void JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1unloadSoundfont(
        JNIEnv *env, jobject thiz, jint sfSlot) {
    if (sfSlot < 0 || sfSlot >= MAX_SOUNDFONTS) return;
    // Close per-track voices cloned from this slot BEFORE closing the master handle
    for (int t = 0; t < 8; t++) {
        if (sfVoices[t].sourceSfSlot == (int)sfSlot) sfVoices[t].close();
    }
    std::lock_guard<std::mutex> sfLock(soundfonts[sfSlot].mutex);
    if (soundfonts[sfSlot].handle) {
        tsf_close(soundfonts[sfSlot].handle);
        soundfonts[sfSlot].handle = nullptr;
    }
    soundfonts[sfSlot].fileData.clear();
    soundfonts[sfSlot].instrumentId = -1;
    soundfonts[sfSlot].filePath.clear();
    LOGD("🎹 Unloaded soundfont slot %d", (int)sfSlot);
}

JNIEXPORT jstring JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getSoundfontPresetName(
        JNIEnv *env, jobject thiz, jint sfSlot, jint bank, jint preset) {
    if (sfSlot < 0 || sfSlot >= MAX_SOUNDFONTS || !soundfonts[sfSlot].handle) {
        return env->NewStringUTF("---");
    }
    std::lock_guard<std::mutex> sfLock(soundfonts[sfSlot].mutex);
    const char* name = tsf_bank_get_presetname(soundfonts[sfSlot].handle, (int)bank, (int)preset);
    return env->NewStringUTF(name ? name : "---");
}

// Returns [bank, preset_number] of the first preset in the SF2, or [-1, -1] if none loaded.
// Used to initialize sfBank/sfPreset when a soundfont is first loaded.
JNIEXPORT jintArray JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getSoundfontFirstBankPreset(
        JNIEnv *env, jobject thiz, jint sfSlot) {
    jintArray result = env->NewIntArray(2);
    jint values[2] = {-1, -1};
    if (sfSlot >= 0 && sfSlot < MAX_SOUNDFONTS && soundfonts[sfSlot].handle) {
        std::lock_guard<std::mutex> sfLock(soundfonts[sfSlot].mutex);
        tsf* f = soundfonts[sfSlot].handle;
        if (f->presetNum > 0) {
            values[0] = (jint)f->presets[0].bank;
            values[1] = (jint)f->presets[0].preset;
        }
    }
    env->SetIntArrayRegion(result, 0, 2, values);
    return result;
}

// Returns the total number of presets in the SF2 file, or 0 if not loaded.
JNIEXPORT jint JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getSoundfontPresetCount(
        JNIEnv *env, jobject thiz, jint sfSlot) {
    if (sfSlot < 0 || sfSlot >= MAX_SOUNDFONTS || !soundfonts[sfSlot].handle) return 0;
    std::lock_guard<std::mutex> sfLock(soundfonts[sfSlot].mutex);
    return (jint)soundfonts[sfSlot].handle->presetNum;
}

// Returns [bank, preset_number] of the preset at the given index in the SF2, or [-1, -1] on error.
JNIEXPORT jintArray JNICALL
Java_com_conanizer_pockettracker_platform_android_OboeAudioBackend_native_1getSoundfontPresetAt(
        JNIEnv *env, jobject thiz, jint sfSlot, jint index) {
    jintArray result = env->NewIntArray(2);
    jint values[2] = {-1, -1};
    if (sfSlot >= 0 && sfSlot < MAX_SOUNDFONTS && soundfonts[sfSlot].handle) {
        std::lock_guard<std::mutex> sfLock(soundfonts[sfSlot].mutex);
        tsf* f = soundfonts[sfSlot].handle;
        if (index >= 0 && index < f->presetNum) {
            values[0] = (jint)f->presets[index].bank;
            values[1] = (jint)f->presets[index].preset;
        }
    }
    env->SetIntArrayRegion(result, 0, 2, values);
    return result;
}

}