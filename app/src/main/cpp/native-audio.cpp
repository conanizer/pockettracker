#include <jni.h>
#include <oboe/Oboe.h>
#include <android/log.h>
#include <vector>
#include <queue>
#include <mutex>
#include <cmath>

#define LOG_TAG "NativeAudio"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

const int MAX_VOICES = 8;  // Reduced for testing

// ===================================
// EFFECT TYPE CONSTANTS (must match EffectProcessor.kt)
// ===================================
const int FX_NONE = 0x00;
const int FX_ARC = 0x03;       // Cxx - Arpeggio Config
const int FX_HOP = 0x08;       // Hxx - Table hop (jump to row, FF = stop table)
const int FX_TIC = 0x09;       // Txx - Table tick rate (01-FB = tics/row, FC-FF = special modes)
const int FX_ARPEGGIO = 0x0A;  // Axx - Arpeggio
const int FX_KILL = 0x0B;      // K00 - Kill voice
const int FX_OFFSET = 0x0F;    // Oxx - Sample offset
const int FX_REPEAT = 0x12;    // Rxx - Retrigger
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

    // For priority queue sorting (earliest frame first)
    bool operator>(const ScheduledNote& other) const {
        return targetFrame > other.targetFrame;
    }
};

// Scheduled kill event (for Kill effect K00)
struct ScheduledKill {
    int64_t targetFrame;     // Exact audio frame to trigger kill
    int trackId;             // Which track to kill (0-7)

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

struct Voice {
    bool isActive;
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

    Voice() : isActive(false), sampleData(nullptr), sampleLength(0),
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
              triggerOctave(4), triggerPitch(0), tic200HzAccum(0.0f),
              hopRepeatCount(0), hopTargetRow(-1),
              pitchOffset(0.0f), pitchSlideTarget(0.0f), pitchSlideRate(0.0f), pitchSliding(false),
              vibratoPhase(0.0f), vibratoSpeed(0.0f), vibratoDepth(0.0f), vibratoActive(false) {}

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

        // Store note info for special TIC modes (Phase 4)
        triggerOctave = std::max(0, std::min(octave, 9));   // Clamp to 0-9
        triggerPitch = std::max(0, std::min(pitch, 11));    // Clamp to 0-11
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

        isActive = true;
    }

    void stop() {
        isActive = false;
    }
};

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
        // Stop all voices on this track
        for (int i = 0; i < MAX_VOICES; i++) {
            if (voices[i].trackId == trackId && voices[i].isActive) {
                voices[i].stop();
            }
        }
    }

    void stopAll() {
        for (int i = 0; i < MAX_VOICES; i++) {
            voices[i].stop();
        }
        // Pause stream to prevent background noise when idle
        if (stream && stream->getState() == oboe::StreamState::Started) {
            stream->pause();
            LOGD("Stream paused (stopAll)");
        }
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

    oboe::DataCallbackResult onAudioReady(
            oboe::AudioStream *audioStream,
            void *audioData,
            int32_t numFrames) override {

        float *output = static_cast<float*>(audioData);
        int channelCount = audioStream->getChannelCount();

        // Clear
        for (int i = 0; i < numFrames * channelCount; i++) {
            output[i] = 0.0f;
        }

        // PHASE 1: Process note queue at sample-accurate timing
        // Check each frame for scheduled notes
        for (int32_t frame = 0; frame < numFrames; frame++) {
            int64_t currentFrame = globalFrameCounter + frame;

            // Process all scheduled kill events for this exact frame (BEFORE notes)
            while (killQueue.hasKillAt(currentFrame)) {
                ScheduledKill kill = killQueue.pop();
                // Stop all voices on this track
                for (int v = 0; v < MAX_VOICES; v++) {
                    if (voices[v].trackId == kill.trackId && voices[v].isActive) {
                        voices[v].stop();
                        LOGD("🔪 Killed track %d at frame %lld", kill.trackId, (long long)currentFrame);
                    }
                }
            }

            // Trigger all notes scheduled for this exact frame
            while (noteQueue.hasNoteAt(currentFrame)) {
                ScheduledNote note = noteQueue.pop();

                // Find free voice (same logic as triggerNote)
                bool voiceFound = false;

                // TIC00 support: Check if previous voice on this track was using trigger mode
                // If so, save its table row so we can advance it on the new voice
                int savedTableRow = 0;
                bool wasTIC00Mode = false;
                for (int v = 0; v < MAX_VOICES; v++) {
                    if (voices[v].trackId == note.trackId && voices[v].isActive) {
                        if (voices[v].tableTicRate == 0x00 && voices[v].tableId >= 0) {
                            wasTIC00Mode = true;
                            savedTableRow = (voices[v].tableRow + 1) % 16;  // Advance and wrap
                            LOGD("📋 TIC00: Saving table row %d for track %d retrigger", savedTableRow, note.trackId);
                        }
                    }
                }

                // First, stop any voice on this track (voice stealing)
                for (int v = 0; v < MAX_VOICES; v++) {
                    if (voices[v].trackId == note.trackId) {
                        voices[v].stop();
                    }
                }

                // Find free voice slot
                bool foundInactiveVoice = false;
                for (int v = 0; v < MAX_VOICES; v++) {
                    if (!voices[v].isActive) {
                        foundInactiveVoice = true;
                        if (note.sampleId >= 0 && note.sampleId < 256 && samples[note.sampleId]) {
                            float rate = note.frequency / note.baseFrequency;
                            float sampleRate = (float)audioStream->getSampleRate();

                            // M8-style: Check if table's last row (row 15) has TIC effect
                            // If so, use that TIC value for the whole table from the start
                            // This saves the first row for other FX commands while still setting global table speed
                            int effectiveTicRate = note.tableTicRate;
                            if (note.tableId >= 0 && note.tableId < 256) {
                                std::lock_guard<std::mutex> lock(tableMutex);
                                if (tables[note.tableId].loaded) {
                                    TableRow& lastRow = tables[note.tableId].rows[15];
                                    // Check all 3 FX slots for TIC effect
                                    // Accept all valid TIC values: 00 (trigger), 01-FB (standard), FC-FF (special)
                                    auto checkTic = [](uint8_t fxType, uint8_t fxValue) -> int {
                                        if (fxType == FX_TIC) {
                                            // All TIC values are valid: 00, 01-FB, FC, FD, FE, FF
                                            return fxValue;
                                        }
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

                            // For TIC00 mode, use saved row from previous voice; otherwise start at row 0
                            int startRow = (wasTIC00Mode && effectiveTicRate == 0x00) ? savedTableRow : 0;

                            voices[v].trigger(samples[note.sampleId], sampleLengths[note.sampleId],
                                              note.trackId, rate, note.volume, note.pan, instrumentParams[note.sampleId],
                                              sampleRate, note.startPointOverride,
                                              note.tableId, effectiveTicRate, note.noteOctave, note.notePitch, startRow);

                            // Apply pitch modulation from scheduled note (Phase 7)
                            // PSL: Set initial offset and start slide to 0
                            if (fabsf(note.pslInitialOffset) > 0.001f && note.pslDuration > 0.0f) {
                                voices[v].pitchOffset = note.pslInitialOffset;
                                // Calculate slide rate based on tempo (assume 120 BPM if not specified)
                                float beatsPerSecond = 120.0f / 60.0f;  // Default tempo
                                float stepsPerBeat = 4.0f;
                                float ticsPerStep = 12.0f;
                                float ticsPerSecond = beatsPerSecond * stepsPerBeat * ticsPerStep;
                                float framesPerTic = sampleRate / ticsPerSecond;
                                float totalFrames = framesPerTic * note.pslDuration;
                                if (totalFrames < 1.0f) totalFrames = 1.0f;
                                voices[v].pitchSlideTarget = 0.0f;  // Slide to the actual note pitch
                                voices[v].pitchSlideRate = -note.pslInitialOffset / totalFrames;
                                voices[v].pitchSliding = true;
                                LOGD("🎵 PSL applied: offset=%.2f, duration=%.0f ticks, rate=%.6f",
                                     note.pslInitialOffset, note.pslDuration, voices[v].pitchSlideRate);
                            }
                            // PBN: Set continuous pitch bend rate
                            if (fabsf(note.pbnRate) > 0.0001f) {
                                float beatsPerSecond = 120.0f / 60.0f;
                                float stepsPerBeat = 4.0f;
                                float ticsPerStep = 12.0f;
                                float ticsPerSecond = beatsPerSecond * stepsPerBeat * ticsPerStep;
                                float framesPerTic = sampleRate / ticsPerSecond;
                                voices[v].pitchSlideRate = note.pbnRate / framesPerTic;
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

                            voiceFound = true;
                            LOGD("🎵 Triggered note at frame %lld: sample=%d, track=%d, rate=%.3f, pan=%.2f, startOverride=%d, table=%d, tic=%d, oct=%d, pitch=%d, startRow=%d",
                                 (long long)currentFrame, note.sampleId, note.trackId, rate, note.pan, note.startPointOverride,
                                 note.tableId, effectiveTicRate, note.noteOctave, note.notePitch, startRow);
                        } else {
                            // Sample not loaded - log specific reason
                            if (note.sampleId < 0 || note.sampleId >= 256) {
                                LOGD("❌ Invalid sampleId=%d for note at frame %lld", note.sampleId, (long long)currentFrame);
                            } else {
                                LOGD("❌ Sample %d not loaded! Note at frame %lld cannot play", note.sampleId, (long long)currentFrame);
                            }
                        }
                        break;
                    }
                }

                if (!voiceFound) {
                    if (!foundInactiveVoice) {
                        LOGD("⚠️ No free voice (all 8 active) for note at frame %lld, sample=%d", (long long)currentFrame, note.sampleId);
                    }
                    // If foundInactiveVoice but !voiceFound, it means sample wasn't loaded (already logged above)
                }
            }
        }

        // Per-track peak accumulators for this callback (for accurate metering)
        float framePeaksPerTrack[8] = {0};

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
                // At 44100Hz with ~256 sample callbacks, that's ~172 callbacks/sec
                // We want 200Hz, so advance roughly every callback with some accumulation
                // 200Hz = advance every 220.5 samples, so accumulate frames
                voice.tic200HzAccum += numFrames;
                float samplesPerTic = 44100.0f / 200.0f;  // ~220.5 samples per tic
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
        // Process pitch slide and vibrato for each active voice once per callback
        for (int v = 0; v < MAX_VOICES; v++) {
            Voice& voice = voices[v];
            if (!voice.isActive) continue;

            // Update pitch slide and vibrato state
            updateVoicePitchMod(voice, numFrames);
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

                // STEP 6: Apply volume after effects (no artificial headroom)
                // Include table volume modifier (Phase 3.5)
                float sample = processedSample * voice.volume * voice.tableVolume;

                // STEP 7: Apply real-time track and master volume
                // This allows volume changes to take effect immediately without rescheduling
                float trackVol, masterVol;
                {
                    std::lock_guard<std::mutex> lock(volumeMutex);
                    trackVol = trackVolumes[voice.trackId];
                    masterVol = masterVolume;
                }
                sample = sample * trackVol * masterVol;

                // Apply pan and write to stereo channels
                float sampleL = sample * voice.panLeft;
                float sampleR = sample * voice.panRight;
                output[i * channelCount] += sampleL;       // Left
                output[i * channelCount + 1] += sampleR;   // Right

                // Track actual audio level for this track's meter (max of L/R)
                if (voice.trackId >= 0 && voice.trackId < 8) {
                    float peakLevel = fmaxf(fabsf(sampleL), fabsf(sampleR));
                    framePeaksPerTrack[voice.trackId] = fmaxf(framePeaksPerTrack[voice.trackId], peakLevel);
                }

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

        // Capture waveform for oscilloscope (left channel only, with downsampling)
        {
            std::lock_guard<std::mutex> lock(waveformMutex);
            for (int i = 0; i < numFrames; i++) {
                // Downsample: only capture every Nth sample
                waveformDownsampleCounter++;
                if (waveformDownsampleCounter >= WAVEFORM_DOWNSAMPLE) {
                    waveformBuffer[waveformIndex] = output[i * channelCount];  // Left channel
                    waveformIndex = (waveformIndex + 1) % WAVEFORM_SIZE;
                    waveformDownsampleCounter = 0;
                }
            }
        }

        // Update peak levels for mixer meters
        {
            std::lock_guard<std::mutex> lock(peakMutex);

            // Apply decay to all peaks first
            for (int t = 0; t < 8; t++) {
                trackPeaks[t] *= PEAK_DECAY;
            }
            masterPeakL *= PEAK_DECAY;
            masterPeakR *= PEAK_DECAY;

            // Use actual measured per-track peaks from this callback
            for (int t = 0; t < 8; t++) {
                trackPeaks[t] = fmaxf(trackPeaks[t], framePeaksPerTrack[t]);
            }

            // Calculate master peaks from actual output
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

        // Update global frame counter for next callback
        globalFrameCounter += numFrames;

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
                      float pbnRate = 0.0f, float vibratoSpeed = 0.0f, float vibratoDepth = 0.0f) {
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
                .vibratoDepth = vibratoDepth
        };
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

    // Clear all scheduled notes
    void clearScheduledNotes() {
        noteQueue.clear();
        killQueue.clear();  // Also clear kill events
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

    // Set real-time track volume (affects playback immediately)
    void setTrackVolume(int trackId, float volume) {
        if (trackId < 0 || trackId >= 8) return;
        std::lock_guard<std::mutex> lock(volumeMutex);
        trackVolumes[trackId] = volume;
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

    // Set pitch slide for a voice (PSL effect)
    // Slides from current pitch offset to target over duration
    void setPitchSlide(int trackId, float targetSemitones, float durationTicks, int tempo) {
        for (int v = 0; v < MAX_VOICES; v++) {
            if (voices[v].isActive && voices[v].trackId == trackId) {
                Voice& voice = voices[v];

                // Calculate frames per tick based on tempo
                // At 44100Hz, 120BPM, 12 tics/step: ~230 samples per tic
                float beatsPerSecond = tempo / 60.0f;
                float stepsPerBeat = 4.0f;  // 16 steps = 4 beats
                float ticsPerStep = 12.0f;  // Standard tics per step
                float ticsPerSecond = beatsPerSecond * stepsPerBeat * ticsPerStep;
                float framesPerTic = 44100.0f / ticsPerSecond;
                float totalFrames = framesPerTic * durationTicks;

                if (totalFrames < 1.0f) totalFrames = 1.0f;

                float delta = targetSemitones - voice.pitchOffset;
                voice.pitchSlideTarget = targetSemitones;
                voice.pitchSlideRate = delta / totalFrames;
                voice.pitchSliding = true;

                LOGD("🎵 Pitch slide: track=%d, from=%.2f to=%.2f over %.0f frames (rate=%.6f)",
                     trackId, voice.pitchOffset, targetSemitones, totalFrames, voice.pitchSlideRate);
                return;
            }
        }
    }

    // Set continuous pitch bend (PBN effect)
    // Bends pitch continuously at specified rate until stopped
    void setPitchBend(int trackId, float semitonesPerTick, int tempo) {
        for (int v = 0; v < MAX_VOICES; v++) {
            if (voices[v].isActive && voices[v].trackId == trackId) {
                Voice& voice = voices[v];

                if (fabsf(semitonesPerTick) < 0.0001f) {
                    // PBN00 = Stop bending
                    voice.pitchSliding = false;
                    voice.pitchSlideRate = 0.0f;
                    LOGD("🎵 Pitch bend stopped: track=%d", trackId);
                } else {
                    // Calculate rate per frame
                    float beatsPerSecond = tempo / 60.0f;
                    float stepsPerBeat = 4.0f;
                    float ticsPerStep = 12.0f;
                    float ticsPerSecond = beatsPerSecond * stepsPerBeat * ticsPerStep;
                    float framesPerTic = 44100.0f / ticsPerSecond;

                    voice.pitchSlideRate = semitonesPerTick / framesPerTic;
                    // Set target far in the direction of bend (will slide until stopped)
                    voice.pitchSlideTarget = (semitonesPerTick > 0) ? 127.0f : -127.0f;
                    voice.pitchSliding = true;

                    LOGD("🎵 Pitch bend: track=%d, rate=%.4f semitones/tic", trackId, semitonesPerTick);
                }
                return;
            }
        }
    }

    // Set vibrato (PVB/PVX effect)
    void setVibrato(int trackId, float speed, float depth) {
        for (int v = 0; v < MAX_VOICES; v++) {
            if (voices[v].isActive && voices[v].trackId == trackId) {
                Voice& voice = voices[v];

                if (depth < 0.01f) {
                    // Stop vibrato
                    voice.vibratoActive = false;
                    voice.vibratoDepth = 0.0f;
                    LOGD("🎵 Vibrato stopped: track=%d", trackId);
                } else {
                    voice.vibratoSpeed = speed;
                    voice.vibratoDepth = depth;
                    voice.vibratoActive = true;
                    // Don't reset phase - allows smooth parameter changes
                    LOGD("🎵 Vibrato: track=%d, speed=%.1fHz, depth=%.2f semitones",
                         trackId, speed, depth);
                }
                return;
            }
        }
    }

    // Clear all pitch modulation for a voice
    void clearPitchMod(int trackId) {
        for (int v = 0; v < MAX_VOICES; v++) {
            if (voices[v].isActive && voices[v].trackId == trackId) {
                Voice& voice = voices[v];
                voice.pitchOffset = 0.0f;
                voice.pitchSliding = false;
                voice.pitchSlideRate = 0.0f;
                voice.vibratoActive = false;
                voice.vibratoDepth = 0.0f;
                LOGD("🎵 Pitch mod cleared: track=%d", trackId);
                return;
            }
        }
    }

    // Set initial pitch offset for a voice (used by PSL portamento effect)
    // This sets the starting pitch offset before calling setPitchSlide
    void setInitialPitchOffset(int trackId, float semitones) {
        for (int v = 0; v < MAX_VOICES; v++) {
            if (voices[v].isActive && voices[v].trackId == trackId) {
                voices[v].pitchOffset = semitones;
                LOGD("🎵 Pitch offset set: track=%d, offset=%.2f semitones", trackId, semitones);
                return;
            }
        }
    }

    // Update pitch modulation for a single voice (called per frame in audio callback)
    void updateVoicePitchMod(Voice& voice, int numFrames) {
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
            float phaseIncrement = (2.0f * (float)M_PI * voice.vibratoSpeed / 44100.0f) * numFrames;
            voice.vibratoPhase += phaseIncrement;

            // Wrap phase to [0, 2π]
            while (voice.vibratoPhase >= 2.0f * (float)M_PI) {
                voice.vibratoPhase -= 2.0f * (float)M_PI;
            }
        }
    }

    // Get modulated playback rate including pitch offset and vibrato
    float getModulatedPlaybackRate(Voice& voice) {
        float pitchMod = voice.pitchOffset;

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
    // OFFLINE RENDER (for WAV export)
    // ===================================
    // Renders audio frames without the Oboe stream.
    // Processes note queue and voices sample-by-sample, returns interleaved stereo output.
    void renderOffline(int numFrames, float* output, int sampleRate) {
        // Clear output buffer
        for (int i = 0; i < numFrames * 2; i++) {
            output[i] = 0.0f;
        }

        // Process sample-by-sample (note triggering + voice mixing together)
        for (int32_t frame = 0; frame < numFrames; frame++) {
            int64_t currentFrame = globalFrameCounter + frame;

            // Process kill events at this frame
            while (killQueue.hasKillAt(currentFrame)) {
                ScheduledKill kill = killQueue.pop();
                for (int v = 0; v < MAX_VOICES; v++) {
                    if (voices[v].trackId == kill.trackId && voices[v].isActive) {
                        voices[v].stop();
                    }
                }
            }

            // Trigger scheduled notes at this frame
            while (noteQueue.hasNoteAt(currentFrame)) {
                ScheduledNote note = noteQueue.pop();

                // Voice stealing: stop any voice on this track
                for (int v = 0; v < MAX_VOICES; v++) {
                    if (voices[v].trackId == note.trackId) {
                        voices[v].stop();
                    }
                }

                // Find free voice slot
                for (int v = 0; v < MAX_VOICES; v++) {
                    if (!voices[v].isActive) {
                        if (note.sampleId >= 0 && note.sampleId < 256 && samples[note.sampleId]) {
                            float rate = note.frequency / note.baseFrequency;
                            voices[v].trigger(samples[note.sampleId], sampleLengths[note.sampleId],
                                              note.trackId, rate, note.volume, note.pan,
                                              instrumentParams[note.sampleId],
                                              (float)sampleRate, note.startPointOverride);

                            // Apply pitch modulation from scheduled note (Phase 7)
                            // PSL: Set initial offset and start slide to 0
                            if (fabsf(note.pslInitialOffset) > 0.001f && note.pslDuration > 0.0f) {
                                voices[v].pitchOffset = note.pslInitialOffset;
                                float beatsPerSecond = 120.0f / 60.0f;
                                float stepsPerBeat = 4.0f;
                                float ticsPerStep = 12.0f;
                                float ticsPerSecond = beatsPerSecond * stepsPerBeat * ticsPerStep;
                                float framesPerTic = (float)sampleRate / ticsPerSecond;
                                float totalFrames = framesPerTic * note.pslDuration;
                                if (totalFrames < 1.0f) totalFrames = 1.0f;
                                voices[v].pitchSlideTarget = 0.0f;
                                voices[v].pitchSlideRate = -note.pslInitialOffset / totalFrames;
                                voices[v].pitchSliding = true;
                            }
                            // PBN: Set continuous pitch bend rate
                            if (fabsf(note.pbnRate) > 0.0001f) {
                                float beatsPerSecond = 120.0f / 60.0f;
                                float stepsPerBeat = 4.0f;
                                float ticsPerStep = 12.0f;
                                float ticsPerSecond = beatsPerSecond * stepsPerBeat * ticsPerStep;
                                float framesPerTic = (float)sampleRate / ticsPerSecond;
                                voices[v].pitchSlideRate = note.pbnRate / framesPerTic;
                                voices[v].pitchSlideTarget = (note.pbnRate > 0) ? 127.0f : -127.0f;
                                voices[v].pitchSliding = true;
                            }
                            // PVB/PVX: Set vibrato
                            if (note.vibratoDepth > 0.01f) {
                                voices[v].vibratoSpeed = note.vibratoSpeed;
                                voices[v].vibratoDepth = note.vibratoDepth;
                                voices[v].vibratoActive = true;
                            }
                        }
                        break;
                    }
                }
            }

            // Mix all active voices for this frame
            float leftSample = 0.0f;
            float rightSample = 0.0f;

            for (int v = 0; v < MAX_VOICES; v++) {
                Voice& voice = voices[v];
                if (!voice.isActive || !voice.sampleData) continue;

                int idx = (int)voice.position;
                float frac = voice.position - (float)idx;

                // Bounds check
                if (idx < 0 || idx >= voice.sampleLength - 1) {
                    if (idx == voice.sampleLength - 1 && frac == 0.0f) {
                        float sample = voice.sampleData[idx] * voice.volume;
                        leftSample += sample * voice.panLeft;
                        rightSample += sample * voice.panRight;
                    }
                    voice.isActive = false;
                    continue;
                }

                // Read samples for interpolation
                float sample1 = voice.sampleData[idx];
                float sample2 = voice.sampleData[idx + 1];

                // Apply downsample effect
                if (voice.downsample > 0) {
                    int dsAmount = voice.downsample;
                    int quantizedIdx = (idx / dsAmount) * dsAmount;
                    sample1 = voice.sampleData[std::min(quantizedIdx, voice.sampleLength - 1)];
                    sample2 = sample1;
                }

                // Apply bit crush effect
                if (voice.crush > 0) {
                    int bits = 16 - voice.crush;
                    if (bits < 1) bits = 1;
                    float levels = (float)(1 << bits);
                    sample1 = floorf(sample1 * levels) / levels;
                    sample2 = floorf(sample2 * levels) / levels;
                }

                // Linear interpolation
                float sample = sample1 + frac * (sample2 - sample1);

                // Apply drive (soft clipping)
                if (voice.drive > 0) {
                    float driveAmount = 1.0f + (voice.drive / 32.0f);
                    sample = sample * driveAmount;
                    sample = tanhf(sample);
                }

                // Apply biquad filter
                if (voice.filterType > 0) {
                    float filtered = voice.b0 * sample +
                                     voice.b1 * voice.x1 +
                                     voice.b2 * voice.x2 -
                                     voice.a1 * voice.y1 -
                                     voice.a2 * voice.y2;
                    voice.x2 = voice.x1;
                    voice.x1 = sample;
                    voice.y2 = voice.y1;
                    voice.y1 = filtered;
                    sample = filtered;
                }

                // Apply volume and pan (no artificial headroom)
                sample = sample * voice.volume;
                leftSample += sample * voice.panLeft;
                rightSample += sample * voice.panRight;

                // Advance position
                voice.position += voice.playbackRate;

                // Handle looping
                if (voice.loopMode > 0) {
                    float loopStart = (float)voice.actualLoopStart;
                    float loopEnd = (float)voice.actualEnd;

                    if (voice.position >= loopEnd) {
                        if (voice.loopMode == 1) {
                            voice.position = loopStart + fmodf(voice.position - loopEnd, loopEnd - loopStart);
                        } else if (voice.loopMode == 2) {
                            voice.playbackRate = -voice.playbackRate;
                            voice.position = loopEnd - 1;
                        }
                    } else if (voice.loopMode == 2 && voice.position < loopStart) {
                        voice.playbackRate = -voice.playbackRate;
                        voice.position = loopStart;
                    }
                }
            }

            // Write mixed output for this frame
            output[frame * 2] = leftSample;
            output[frame * 2 + 1] = rightSample;
        }

        // Update global frame counter
        globalFrameCounter += numFrames;
    }

    // Reset frame counter (for starting a new render)
    void resetFrameCounter() {
        globalFrameCounter = 0;
    }

    // Get current frame counter
    int64_t getFrameCounter() {
        return globalFrameCounter;
    }

private:
    std::shared_ptr<oboe::AudioStream> stream;
    Voice voices[MAX_VOICES];
    float* samples[256];
    int sampleLengths[256];
    InstrumentParams instrumentParams[256];

    // Table data (Phase 3.5)
    Table tables[256];             // 256 tables, each with 16 rows
    std::mutex tableMutex;         // Protect table data during load/access

    // PHASE 1: Sample-accurate timing infrastructure
    NoteQueue noteQueue;           // Thread-safe queue of scheduled notes
    KillQueue killQueue;           // Thread-safe queue of scheduled kill events
    int64_t globalFrameCounter;    // Total frames processed since start

    // Oscilloscope waveform buffer (circular buffer for recent output)
    static const int WAVEFORM_SIZE = 620;
    float waveformBuffer[WAVEFORM_SIZE];
    int waveformIndex = 0;
    std::mutex waveformMutex;

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
Java_com_example_pockettracker_TrackerAudioEngine_native_1create(JNIEnv *env, jobject thiz) {
    if (!engine) {
        engine = new AudioEngine();
        return engine->openStream() ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1delete(JNIEnv *env, jobject thiz) {
    delete engine;
    engine = nullptr;
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1loadSample(
        JNIEnv *env, jobject thiz, jint id, jfloatArray data) {
    if (!engine) return;

    jsize len = env->GetArrayLength(data);
    jfloat* arr = env->GetFloatArrayElements(data, nullptr);

    engine->loadSample(id, arr, len);

    env->ReleaseFloatArrayElements(data, arr, JNI_ABORT);
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1triggerNote(
        JNIEnv *env, jobject thiz, jint sid, jint tid, jfloat f, jfloat bf, jfloat v) {
    if (engine) {
        engine->triggerNote(sid, tid, f, bf, v);
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1stopTrack(JNIEnv *env, jobject thiz, jint tid) {
    if (engine) {
        engine->stopTrack(tid);
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1stopAll(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->stopAll();
    }
}

JNIEXPORT jint JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1getActiveVoiceCount(JNIEnv *env, jobject thiz) {
    if (engine) {
        return engine->getActiveVoiceCount();
    }
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1getSampleRate(JNIEnv *env, jobject thiz) {
    if (engine) {
        return engine->getSampleRate();
    }
    return 48000; // Default fallback
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1setInstrumentParams(
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
Java_com_example_pockettracker_TrackerAudioEngine_native_1getCurrentFrame(JNIEnv *env, jobject thiz) {
    if (engine) {
        return (jlong)engine->getCurrentFrame();
    }
    return 0;
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1scheduleNote(
        JNIEnv *env, jobject thiz, jlong targetFrame, jint sampleId, jint trackId,
        jfloat frequency, jfloat baseFrequency, jfloat volume) {
    if (engine) {
        engine->scheduleNote(targetFrame, sampleId, trackId, frequency, baseFrequency, volume);
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1clearScheduledNotes(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->clearScheduledNotes();
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1resumeStream(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->resumeStream();
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1getWaveform(JNIEnv *env, jobject thiz, jfloatArray outArray) {
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
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1create(JNIEnv *env, jobject thiz) {
    if (!engine) {
        engine = new AudioEngine();
        return engine->openStream() ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1delete(JNIEnv *env, jobject thiz) {
    if (engine) {
        delete engine;
        engine = nullptr;
        LOGD("✅ Audio engine deleted");
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1loadSample(
        JNIEnv *env, jobject thiz, jint id, jfloatArray data) {
    if (!engine) return;

    jsize len = env->GetArrayLength(data);
    jfloat* arr = env->GetFloatArrayElements(data, nullptr);

    engine->loadSample(id, arr, len);

    env->ReleaseFloatArrayElements(data, arr, JNI_ABORT);
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1scheduleNote(
        JNIEnv *env, jobject thiz, jlong targetFrame, jint sampleId, jint trackId,
        jfloat frequency, jfloat baseFrequency, jfloat volume, jfloat pan, jint startPointOverride) {
    if (engine) {
        engine->scheduleNote(targetFrame, sampleId, trackId, frequency, baseFrequency, volume, pan, startPointOverride);
    }
}

JNIEXPORT jlong JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1getCurrentFrame(JNIEnv *env, jobject thiz) {
    if (engine) {
        return (jlong)engine->getCurrentFrame();
    }
    return 0;
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1clearScheduledNotes(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->clearScheduledNotes();
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1resumeStream(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->resumeStream();
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1stopAll(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->stopAll();
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1killTrack(JNIEnv *env, jobject thiz, jint trackId) {
    if (engine) {
        engine->stopTrack(trackId);
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1scheduleKill(JNIEnv *env, jobject thiz, jlong targetFrame, jint trackId) {
    if (engine) {
        engine->scheduleKill(targetFrame, trackId);
    }
}

JNIEXPORT jint JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1getSampleRate(JNIEnv *env, jobject thiz) {
    if (engine) {
        return engine->getSampleRate();
    }
    return 44100; // Default fallback
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1getWaveform(JNIEnv *env, jobject thiz, jfloatArray outArray) {
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
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1setInstrumentParams(
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
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1getTrackPeaks(JNIEnv *env, jobject thiz, jfloatArray outArray) {
    if (engine && outArray != nullptr) {
        float buffer[8];
        engine->getTrackPeaks(buffer);
        env->SetFloatArrayRegion(outArray, 0, 8, buffer);
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1getMasterPeaks(JNIEnv *env, jobject thiz, jfloatArray outArray) {
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
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1renderFrames(
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
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1resetFrameCounter(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->resetFrameCounter();
    }
}

JNIEXPORT jlong JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1getFrameCounter(JNIEnv *env, jobject thiz) {
    if (engine) {
        return engine->getFrameCounter();
    }
    return 0;
}

// ===================================
// PHASE 1 BUG FIXES: DECAY AND REAL-TIME VOLUME
// ===================================

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1decayPeaks(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->decayPeaks();
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1decayWaveform(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->decayWaveform();
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1setTrackVolume(JNIEnv *env, jobject thiz, jint trackId, jfloat volume) {
    if (engine) {
        engine->setTrackVolume(trackId, volume);
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1setMasterVolume(JNIEnv *env, jobject thiz, jfloat volume) {
    if (engine) {
        engine->setMasterVolume(volume);
    }
}

// ===================================
// TABLE JNI METHODS (Phase 3.5)
// ===================================

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1loadTable(
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
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1scheduleNoteWithTable(
        JNIEnv *env, jobject thiz, jlong targetFrame, jint sampleId, jint trackId,
        jfloat frequency, jfloat baseFrequency, jfloat volume, jfloat pan,
        jint startPointOverride, jint tableId, jint tableTicRate,
        jint noteOctave, jint notePitch,
        jfloat pslInitialOffset, jfloat pslDuration,
        jfloat pbnRate, jfloat vibratoSpeed, jfloat vibratoDepth) {
    if (engine) {
        engine->scheduleNote(targetFrame, sampleId, trackId, frequency, baseFrequency,
                             volume, pan, startPointOverride, tableId, tableTicRate,
                             noteOctave, notePitch,
                             pslInitialOffset, pslDuration, pbnRate, vibratoSpeed, vibratoDepth);
    }
}

JNIEXPORT jint JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1getVoiceTableRow(
        JNIEnv *env, jobject thiz, jint trackId) {
    if (engine) {
        return engine->getVoiceTableRow(trackId);
    }
    return -1;
}

JNIEXPORT jint JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1getVoiceTableId(
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
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1setPitchSlide(
        JNIEnv *env, jobject thiz, jint trackId, jfloat targetSemitones, jfloat durationTicks, jint tempo) {
    if (engine) {
        engine->setPitchSlide(trackId, targetSemitones, durationTicks, tempo);
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1setPitchBend(
        JNIEnv *env, jobject thiz, jint trackId, jfloat semitonesPerTick, jint tempo) {
    if (engine) {
        engine->setPitchBend(trackId, semitonesPerTick, tempo);
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1setVibrato(
        JNIEnv *env, jobject thiz, jint trackId, jfloat speed, jfloat depth) {
    if (engine) {
        engine->setVibrato(trackId, speed, depth);
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1clearPitchMod(
        JNIEnv *env, jobject thiz, jint trackId) {
    if (engine) {
        engine->clearPitchMod(trackId);
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1setInitialPitchOffset(
        JNIEnv *env, jobject thiz, jint trackId, jfloat semitones) {
    if (engine) {
        engine->setInitialPitchOffset(trackId, semitones);
    }
}

}