#pragma once
#include <queue>
#include <mutex>
#include <vector>
#include <string>
#include "audio-defs.h"

// ===================================
// SOUNDFONT INFRASTRUCTURE (TinySoundFont)
// ===================================
// Forward declaration — tsf is defined in soundfont-voice.cpp (TSF_IMPLEMENTATION).
struct tsf;

static const int MAX_SOUNDFONTS = 4;

struct SoundfontEntry {
    tsf* handle = nullptr;
    std::mutex mutex;            // Protects handle from concurrent audio/JNI access
    int instrumentId = -1;       // Which Instrument slot owns this (-1 = free)
    std::string filePath;
    // fileData removed: per-track clones are gone; master handle is used directly via MIDI channels.
};

extern SoundfontEntry soundfonts[MAX_SOUNDFONTS];

// Sample-accurate note scheduling: notes carry exact target frame numbers;
// the audio callback triggers them at precise moments.
struct ScheduledNote {
    int64_t targetFrame;     // Exact audio frame to trigger this note
    int sampleId;            // Which sample to play (0-255)
    int trackId;             // Which track/voice (0-7)
    float frequency;         // Target playback frequency
    float baseFrequency;     // Sample's base frequency
    float volume;            // Instrument volume (0.0-1.0) — maps to MOD_SRC_INSTR_VOL
    float phraseVolume;      // Phrase step volume (0.0-1.0) — maps to MOD_SRC_PHRASE_VOL
    float pan;               // Stereo pan position (0.0=left, 0.5=center, 1.0=right)
    int startPointOverride;  // Optional start point override (-1 = use instrument default)
    int endPointOverride;    // Optional end point override for CUT slice mode (-1 = use instrument default)

    int tableId;             // Table to use (-1 = no table)
    int tableTicRate;        // Ticks per table row advance (default 6)

    // Note info for special TIC modes
    int noteOctave;          // Octave of note (0-9) for TICFC mode
    int notePitch;           // Pitch of note (0-11, C=0) for TICFE mode

    // Pitch modulation parameters — applied at note trigger
    // These are applied when the note triggers, allowing per-note pitch effects
    float pslInitialOffset;  // PSL: Initial pitch offset in semitones (0 = no PSL)
    float pslDuration;       // PSL: Slide duration in ticks (0 = no slide)
    float pbnRate;           // PBN: Semitones per tick (0 = no bend)
    float vibratoSpeed;      // PVB/PVX: LFO speed in Hz (0 = no vibrato)
    float vibratoDepth;      // PVB/PVX: Depth in semitones (0 = no vibrato)

    // Table start row override (THO effect from phrase)
    int tableStartRow;       // -1 = default (0 or TIC00 continuity), 0-15 = forced start row

    // SoundFont fields (only used when isSoundfont == true)
    bool isSoundfont = false;   // When true, use tsf path instead of voice pool
    int  sfSlot      = -1;      // Index into soundfonts[] array
    int  midiNote    = 60;      // MIDI note 0-127
    int  midiVelocity = 100;    // MIDI velocity 0-127
    int  sfBank      = 0;       // SF2 bank number (0-127)
    int  sfPreset    = 0;       // SF2 preset number within bank (0-127)
    float detuneSemitones = 0.0f; // SF: static fine pitch offset in semitones (instrument detune)

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
        // LOGT, not LOGD: the audio callback takes this mutex every frame, so an always-on
        // logging syscall while holding it is a priority-inversion / dropout hazard.
        LOGT("📅 Scheduled note: frame=%lld, sample=%d, track=%d, freq=%.2f",
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
        // LOGT, not LOGD — see NoteQueue::schedule.
        LOGT("🔪 Scheduled kill: frame=%lld, track=%d", (long long)kill.targetFrame, kill.trackId);
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

// Scheduled parameter update (e.g. Vxx on empty step — update phraseVol at exact frame)
struct ScheduledParamUpdate {
    int64_t targetFrame;     // Exact audio frame to apply the update
    int trackId;             // Which track's active voice to update
    int sourceId;            // ModSourceId to write (e.g. MOD_SRC_PHRASE_VOL)
    float value;             // New value to write

    bool operator>(const ScheduledParamUpdate& other) const {
        return targetFrame > other.targetFrame;
    }
};

class ParamUpdateQueue {
private:
    std::priority_queue<ScheduledParamUpdate, std::vector<ScheduledParamUpdate>, std::greater<ScheduledParamUpdate>> queue;
    std::mutex mutex;

public:
    void schedule(const ScheduledParamUpdate& update) {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push(update);
    }

    bool hasUpdateAt(int64_t currentFrame) {
        std::lock_guard<std::mutex> lock(mutex);
        if (queue.empty()) return false;
        return queue.top().targetFrame <= currentFrame;
    }

    ScheduledParamUpdate pop() {
        std::lock_guard<std::mutex> lock(mutex);
        ScheduledParamUpdate u = queue.top();
        queue.pop();
        return u;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        while (!queue.empty()) queue.pop();
    }

    void clearFrom(int64_t fromFrame) {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<ScheduledParamUpdate> keep;
        while (!queue.empty()) {
            ScheduledParamUpdate u = queue.top(); queue.pop();
            if (u.targetFrame < fromFrame) keep.push_back(u);
        }
        for (auto& u : keep) queue.push(u);
    }
};

// Pre-converted EQ band params (Hz/dB/Q) — populated by setInstrumentEqSlot().
struct EqBandData {
    int   type   = 0;       // 0=off 1=loShelf 2=bell 3=hiShelf
    float freqHz = 1000.0f; // 20–20000 Hz
    float gainDb = 0.0f;    // −12..+12 dB
    float q      = 1.0f;    // 0.1–10.0
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
    int filterType;     // 0=off, 1=lp, 2=hp, 3=bp, 4=notch, 5=peak
    int filterCut;      // 0-255 (cutoff frequency)
    int filterRes;      // 0-255 (resonance)
    int filterDrive;    // 0-255 (SVF resonance saturation; 128 = DaisySP default)

    // EQ parameters (pre-converted; set by setInstrumentEqSlot)
    EqBandData eqBands[3];
    bool eqActive = false;  // true when at least one band is non-bypass

    // Send levels (float 0.0–1.0; set by setInstrumentSendLevels)
    float reverbSend = 0.0f;
    float delaySend  = 0.0f;

    InstrumentParams() : startPoint(0), endPoint(255), reverse(false),
                         loopMode(0), loopStart(0), drive(0), crush(0), downsample(0),
                         filterType(0), filterCut(128), filterRes(0), filterDrive(128),
                         eqActive(false), reverbSend(0.0f), delaySend(0.0f) {}
};

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

// Tables are mini-sequencers that run alongside playing voices.
// Each table has 16 rows with transpose, volume, and 3 FX columns.

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
