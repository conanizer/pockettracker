#pragma once
#include <queue>
#include <mutex>
#include <vector>
#include <string>
#include <atomic>
#include <cstdint>
#include <cstring>
#include "audio-defs.h"
#include "songcore/program.h"   // NoteOnPayload — a queued note carries it until the trigger

// ===================================
// SOUNDFONT INFRASTRUCTURE (TinySoundFont)
// ===================================
// Forward declaration — tsf is defined in soundfont-voice.cpp (TSF_IMPLEMENTATION).
struct tsf;

// A slot holds ONE PRESET, not a whole bank, so eight tracks on eight different sounds of the same
// file want eight slots — plus the preview lane, plus room for the file the user is scrolling through
// on the INSTRUMENT screen. A trimmed preset is one to three megabytes, so the headroom is cheap.
// ⚠️ `SfBigBlock g_sfBig[MAX_SOUNDFONTS + 2]` in soundfont-voice.cpp sizes off this.
static const int MAX_SOUNDFONTS = 12;

// ⚠️ **The audio thread reads `handle` and `gen` and nothing else; every other field is the UI's.**
// It takes no lock: a handle it loaded stays valid until its block ends, because `freeSoundfontSlot`
// swaps the pointer out and waits for that block boundary before `tsf_close`. A voice records `gen`
// when it is armed, and one whose slot has been freed since is detached at the top of the next block.
struct SoundfontEntry {
    std::atomic<tsf*>     handle{nullptr};
    std::atomic<uint32_t> gen{0};  // bumped by every free
    int instrumentId = -1;       // Which Instrument slot owns this (-1 = free)
    // ⚠️ **The IDENTITY of a slot is the path AND the bank AND the preset**, because what is loaded
    // is one preset trimmed out of the file rather than the file. Two instruments on the same .sf2 at
    // different sounds are two slots; matching on the path alone would hand the second one whichever
    // sound loaded first, with nothing to hear but the wrong instrument.
    std::string filePath;
    int bank = -1, preset = -1;
    std::atomic<uint64_t> lastUsed{0};  // Monotonic use tick for LRU eviction; 0 = never used
    // Rendered via the master tsf handle on per-track MIDI channels (no per-track clones).
};

extern SoundfontEntry soundfonts[MAX_SOUNDFONTS];

// Monotonic "use" tick for true-LRU SoundFont eviction. Bumped on load and on each note trigger;
// eviction drops the slot with the smallest tick (genuinely least-recently-used, so the SF playing
// right now is never evicted). A function-local static gives one shared counter across translation
// units with no ODR-prone global definition.
inline uint64_t nextSfUseTick() {
    static std::atomic<uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

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

    // Pitch modulation parameters — applied at note trigger, allowing per-note pitch effects.
    // ⚠️ UNITS: songcore/voice_derive.h converts PSL and PBN out of the tick/step domain the FX cells
    // are authored in BEFORE they reach this queue (`pslDur * framesPerTic`, `pbnRate / framesPerStep`).
    // What arrives here is already per-FRAME, and the engine applies it without further scaling.
    float pslInitialOffset;  // PSL: Initial pitch offset in semitones (0 = no PSL)
    float pslDuration;       // PSL: Slide duration in FRAMES (0 = no slide)
    float pbnRate;           // PBN: Semitones per FRAME (0 = no bend)
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

    // ── Deferred resolution: the instrument as a NUMBER ──────────────────────────────────────────
    // ⚠️ **A NOTE IS QUEUED ABOUT TWO PHRASES BEFORE IT SOUNDS.** When `instrumentId >= 0` every
    // field above is still UNSET and gets derived at the trigger instead, from the engine's program
    // table — which is what lets a table row name a different instrument on the hit itself.
    // -1 means the note arrived already derived (previews, retrigger, MIDI in, the file browser).
    int instrumentId = -1;
    songcore::NoteOnPayload noteOn{};   // the note-level half: pitch, velocity, pan, PSL/PBN/vibrato
    int  tempo        = 120;            // the tempo the note was SCHEDULED at — its tick→frame scale
    bool rootAudition = false;          // INSTRUMENT-screen root preview; the sequencer never sets it
    uint32_t gen = 0;                   // stamped by the queue, never by a caller — see CancelLedger

    // For priority queue sorting (earliest frame first)
    bool operator>(const ScheduledNote& other) const {
        return targetFrame > other.targetFrame;
    }
};

// Scheduled kill event (for Kill effect K00, soft note-off for ADSR release, and a live key let go of)
//
// ⚠️ **ONE `mode`, NOT A SECOND BOOL BESIDE `softKill`.** Two bools can encode a state that means
// nothing (hard AND key-release), and the dispatch would have to pick a winner somewhere; an enum
// cannot be put into that state at all. Same rule the repo applies to every "derive it from the data"
// case — the numbers below are internal to the engine and unrelated to event.h's NOTE_OFF_* wire
// values, which is why the consumer translates rather than casts.
enum KillMode : uint8_t {
    KILL_HARD    = 0,   // K00 / killTrack — declick fade, whatever the instrument is
    KILL_SOFT    = 1,   // KIL's soft note-off — ADSR release, else a declick fade
    KILL_KEY_OFF = 2,   // a KEY released (MIDI in, plan §4.1) — a one-shot IGNORES it and plays out
    KILL_CUT     = 3,   // a held audition let go of — KILL_FADE_SAMPLES and gone, no release tail at all
};

struct ScheduledKill {
    int64_t targetFrame;     // Exact audio frame to trigger kill
    int trackId;             // Which track to kill (0-7)
    KillMode mode = KILL_HARD;
    uint32_t gen = 0;        // stamped by the queue, never by a caller — see CancelLedger

    // For priority queue sorting (earliest frame first)
    bool operator>(const ScheduledKill& other) const {
        return targetFrame > other.targetFrame;
    }
};

// A live edit rolls one track's lookahead back and drops what that track had already queued from
// the rollback frame on. The drop is LAZY: the UI only records "generation g of lane L ended at frame
// F" here, and the audio thread skips an entry at drain time when a cancel issued after it was queued
// reaches its frame. Nothing walks or rebuilds the heap, and the UI holds the queue mutex for a push
// and nothing longer — so the audio thread, which drains under the same mutex, never waits on a
// rebuild the UI thread was preempted in the middle of.
//
// ⚠️ The check scans every cancel since the entry's generation, not just the latest: two rollbacks
// at different frames both bound what they threw away, and a note queued between them is kept.
// The history is a ring of HISTORY cancels per lane; an entry that outlives more than that is treated
// as cancelled. A kept entry drains before the next phrase boundary, so reaching that takes HISTORY
// edits on one track inside one phrase.
class CancelLedger {
public:
    static constexpr int LANES   = 10;   // tracks 0-8 (8 is the preview lane) + one for trackId -1
    static constexpr int HISTORY = 64;   // a power of two: the slot is `gen & (HISTORY - 1)`

    static int lane(int trackId) { return (trackId >= 0 && trackId < LANES - 1) ? trackId : LANES - 1; }

    uint32_t current(int trackId) const { return gen_[lane(trackId)].load(std::memory_order_relaxed); }

    // UI thread. `trackId < 0` ends every lane's generation, the global one included.
    void cancel(int64_t fromFrame, int trackId) {
        if (trackId < 0) { for (int l = 0; l < LANES; ++l) cancelLane(l, fromFrame); return; }
        cancelLane(lane(trackId), fromFrame);
    }

    // Audio thread. True when a cancel issued after generation `gen` covers `targetFrame`.
    bool cancelled(uint32_t gen, int trackId, int64_t targetFrame) const {
        const int l = lane(trackId);
        const uint32_t now = gen_[l].load(std::memory_order_acquire);
        if (now - gen > (uint32_t)HISTORY) return true;
        for (uint32_t g = gen; g != now; ++g)
            if (from_[l][g & (HISTORY - 1)].load(std::memory_order_relaxed) <= targetFrame) return true;
        return false;
    }

private:
    void cancelLane(int l, int64_t fromFrame) {
        const uint32_t g = gen_[l].load(std::memory_order_relaxed);
        from_[l][g & (HISTORY - 1)].store(fromFrame, std::memory_order_relaxed);
        gen_[l].store(g + 1, std::memory_order_release);   // publishes the slot write above
    }
    std::atomic<uint32_t> gen_[LANES]{};
    std::atomic<int64_t>  from_[LANES][HISTORY]{};
};

/** A vector with room for the queue's typical load, to seed a heap with — see NoteQueue's constructor. */
template <typename T>
inline std::vector<T> reserved() {
    std::vector<T> v;
    v.reserve(64);
    return v;
}

// Thread-safe note queue
// Audio callback pops notes; the UI thread pushes notes, and so does the audio thread itself for a
// live key (from inside its own drain, before the block's drainUntil — never from another thread
// than those two).
class NoteQueue {
private:
    // Min-heap: earliest targetFrame is always on top
    std::priority_queue<ScheduledNote, std::vector<ScheduledNote>, std::greater<ScheduledNote>> queue;
    std::mutex mutex;
    CancelLedger cancels;

public:
    // The heap's storage is reserved once: a live key schedules from INSIDE the audio callback, and a
    // push that grew the vector there would be an allocation on the audio thread. 64 is the same
    // typical bound the per-block drain buffers use; past it the vector grows, once.
    NoteQueue() : queue(std::greater<ScheduledNote>(), reserved<ScheduledNote>()) {}

    // Schedule a note to be played at exact frame
    void schedule(const ScheduledNote& note) {
        std::lock_guard<std::mutex> lock(mutex);
        ScheduledNote stamped = note;
        stamped.gen = cancels.current(note.trackId);
        queue.push(stamped);
        // LOGT, not LOGD: the audio callback takes this mutex once per block (drainUntil), so
        // an always-on logging syscall while holding it is a priority-inversion / dropout hazard.
        LOGT("📅 Scheduled note: frame=%lld, sample=%d, track=%d, freq=%.2f",
             (long long)note.targetFrame, note.sampleId, note.trackId, note.frequency);
    }

    // Drain every note with targetFrame <= maxFrame into `out` (ascending frame order, since the
    // heap pops earliest-first) under a SINGLE lock. Lets the audio callback dispatch a whole
    // block's worth of notes without taking this mutex once per frame. `out` is appended to.
    // A note a rollback cancelled is popped here and goes nowhere.
    void drainUntil(int64_t maxFrame, std::vector<ScheduledNote>& out) {
        std::lock_guard<std::mutex> lock(mutex);
        while (!queue.empty() && queue.top().targetFrame <= maxFrame) {
            const ScheduledNote& n = queue.top();
            if (!cancels.cancelled(n.gen, n.trackId, n.targetFrame)) out.push_back(n);
            queue.pop();
        }
    }

    // Clear all scheduled notes (for stop/reset)
    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        while (!queue.empty()) {
            queue.pop();
        }
        LOGD("🗑️ Note queue cleared");
    }

    // Drop the notes already queued at or after fromFrame (earlier ones play). O(1), no lock: the
    // notes stay in the heap and are skipped when drained — see CancelLedger.
    //
    // ⚠️ `trackId >= 0` clears ONE track's, and the sequencer needs that: the eight song tracks each
    // roll their lookahead back to their own phrase boundary, so a live edit must drop exactly the
    // notes the track being rolled back is about to schedule again — and nothing another track has
    // already queued past that frame and will not.
    void clearFrom(int64_t fromFrame, int trackId = -1) { cancels.cancel(fromFrame, trackId); }
};

// Thread-safe kill queue (for Kill effect K00)
class KillQueue {
private:
    std::priority_queue<ScheduledKill, std::vector<ScheduledKill>, std::greater<ScheduledKill>> queue;
    std::mutex mutex;
    CancelLedger cancels;

public:
    KillQueue() : queue(std::greater<ScheduledKill>(), reserved<ScheduledKill>()) {}   // see NoteQueue

    // Schedule a kill event at exact frame
    void schedule(const ScheduledKill& kill) {
        std::lock_guard<std::mutex> lock(mutex);
        ScheduledKill stamped = kill;
        stamped.gen = cancels.current(kill.trackId);
        queue.push(stamped);
        // LOGT, not LOGD — see NoteQueue::schedule.
        LOGT("🔪 Scheduled kill: frame=%lld, track=%d", (long long)kill.targetFrame, kill.trackId);
    }

    // Drain every kill with targetFrame <= maxFrame into `out` (ascending order). See NoteQueue.
    void drainUntil(int64_t maxFrame, std::vector<ScheduledKill>& out) {
        std::lock_guard<std::mutex> lock(mutex);
        while (!queue.empty() && queue.top().targetFrame <= maxFrame) {
            const ScheduledKill& k = queue.top();
            if (!cancels.cancelled(k.gen, k.trackId, k.targetFrame)) out.push_back(k);
            queue.pop();
        }
    }

    // Clear all scheduled kills
    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        while (!queue.empty()) {
            queue.pop();
        }
        LOGD("🗑️ Kill queue cleared");
    }

    // Drop the kills queued at or after fromFrame; `trackId >= 0` clears one track's. See
    // NoteQueue::clearFrom for why the filter exists.
    void clearFrom(int64_t fromFrame, int trackId = -1) { cancels.cancel(fromFrame, trackId); }
};

// Action discriminator for ScheduledParamUpdate. Every live change to a sounding voice or the mixer is
// routed through this queue, so the write happens on the audio thread (no off-thread race) and lands
// at the exact step frame instead of whenever the look-ahead scheduler reached the step. The numbers
// live only in the queue — nothing stores them.
enum ParamUpdateAction {
    PARAM_UPDATE_MOD_SOURCE = 0,  // modSourceValues[sourceId] = value                   [Vxx]
    PARAM_UPDATE_PITCH_BEND,      // the track's note: setPitchBendRaw(value)            [PBN]
    PARAM_UPDATE_VIBRATO,         // the track's note: setVibratoRaw(value, value2)      [PVB/PVX]
    PARAM_UPDATE_TABLE_ROW,       // sampler voice: every table column to row (int)value [THO]
    // A per-voice controller: `sourceId` is its CC id (songcore/event.h), `value` the 0-1 CC value.
    // PAN, REV, DEL, CUT, RES, LPF/HPF/BPF, DRV, CRU, FIN, LPO — one action, one apply function
    // (applyVoiceCc), shared with the table rows.
    PARAM_UPDATE_VOICE_CC,
    PARAM_UPDATE_REVERSE,         // sampler voice: reverse=(value!=0); value2!=0 → snap pos to new-dir boundary [BCK]
    // A slot reads the preset bank; BANDS carry the values themselves, in `eqBands`, because an
    // AUS/AUF morph sets the EQ to a setting no preset holds. (int)value < 0 = bypass.
    PARAM_UPDATE_EQ_SLOT,         // the voice's EQ   [EQN]
    PARAM_UPDATE_EQ_BANDS,        //                  [EQN + AUS/AUF]
    PARAM_UPDATE_MASTER_EQ,       // the master EQ    [EQM]
    PARAM_UPDATE_MASTER_EQ_BANDS, //                  [EQM + AUS/AUF]
    // ⚠️ THE MIXER FADERS TOUCH NO VOICE, and their apply arms carry a trap the others do not:
    // processAudioBlock reads trackVolumes[]/masterVolume once, above the frame loop, and the hot
    // loops read the snapshot. Writing only the member would apply a whole block late — audible as a
    // ramp that lags, and invisible to anything that only reads back the member. Both arms write the
    // member AND the in-scope snapshot.
    PARAM_UPDATE_TRACK_VOL,       // mixer: trackVolumes[trackId] = value          [VTR]
    PARAM_UPDATE_MASTER_VOL,      // mixer: masterVolume = value (global)          [VMV]
    // The only action that reaches a SEND BUS. No snapshot write: the delay module owns its head
    // position and moves it per sample from inside its own `process`.
    PARAM_UPDATE_DELAY_TIME,      // global: the delay's echo time = value*255, free scale      [TIM]
    // ⚠️ SCOPED TO AN INSTRUMENT RATHER THAN A TRACK, and carries no value: a voice copies its
    // instrument's filter, drive, crush and sends when it is triggered, so an edit to any of them is
    // inaudible until the next note unless the voices already sounding are told to read them again.
    // It names the instrument in `instrId` and the engine re-reads the rest.
    PARAM_UPDATE_INSTRUMENT,      // every sounding voice of `instrId` re-reads that instrument
};

// One EQ setting as AUTHORED HEX — the domain the project file and the FX cells are written in, not
// the Hz/dB/Q the engine runs on. It crosses the seam in this form because interpolating hex is what
// makes a frequency sweep linear in log-frequency; `applyEqBandsToChain` does the conversion, using
// the same arithmetic as setEqBand().
struct EqBandsHex {
    int type[3] = {};                    // 0 OFF | 1 LOSHELF | 2 LOWCUT | 3 BELL | 4 HISHELF | 5 HICUT
    int freq[3] = { 128, 128, 128 };     // 00-FF → 20-20000 Hz, log
    int gain[3] = { 120, 120, 120 };     // 0-240 → −12.0..+12.0 dB (120 = flat)
    int q[3]    = { 128, 128, 128 };     // 00-FF → 0.1-10.0, log
};

// Scheduled parameter update (e.g. Vxx on empty step — update phraseVol at exact frame)
struct ScheduledParamUpdate {
    int64_t targetFrame;     // Exact audio frame to apply the update
    int trackId;             // Which track's active voice to update
    int sourceId;            // ModSourceId (PARAM_UPDATE_MOD_SOURCE) or CC id (PARAM_UPDATE_VOICE_CC)
    float value;             // New value: mod-source value / bend rate / vibrato speed / table row
    int action = PARAM_UPDATE_MOD_SOURCE;  // discriminator (default keeps Vxx call sites unchanged)
    float value2 = 0.0f;     // second arg: vibrato depth (PARAM_UPDATE_VIBRATO)
    // ⚠️ LAST, and defaulted: every other call site aggregate-initialises this struct positionally and
    // stops before here. A field inserted above instead would silently re-bind all of them.
    EqBandsHex eqBands{};    // PARAM_UPDATE_EQ_BANDS / PARAM_UPDATE_MASTER_EQ_BANDS only
    int instrId = -1;        // PARAM_UPDATE_INSTRUMENT only — which instrument's voices re-read it
    uint32_t gen = 0;        // stamped by the queue, never by a caller — see CancelLedger

    bool operator>(const ScheduledParamUpdate& other) const {
        return targetFrame > other.targetFrame;
    }
};

class ParamUpdateQueue {
private:
    std::priority_queue<ScheduledParamUpdate, std::vector<ScheduledParamUpdate>, std::greater<ScheduledParamUpdate>> queue;
    std::mutex mutex;
    CancelLedger cancels;

public:
    ParamUpdateQueue() : queue(std::greater<ScheduledParamUpdate>(), reserved<ScheduledParamUpdate>()) {}   // see NoteQueue

    void schedule(const ScheduledParamUpdate& update) {
        std::lock_guard<std::mutex> lock(mutex);
        ScheduledParamUpdate stamped = update;
        stamped.gen = cancels.current(update.trackId);
        queue.push(stamped);
    }

    // Drain every update with targetFrame <= maxFrame into `out` (ascending order). See NoteQueue.
    void drainUntil(int64_t maxFrame, std::vector<ScheduledParamUpdate>& out) {
        std::lock_guard<std::mutex> lock(mutex);
        while (!queue.empty() && queue.top().targetFrame <= maxFrame) {
            const ScheduledParamUpdate& u = queue.top();
            if (!cancels.cancelled(u.gen, u.trackId, u.targetFrame)) out.push_back(u);
            queue.pop();
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        while (!queue.empty()) queue.pop();
    }

    // `trackId >= 0` clears one track's — see NoteQueue::clearFrom. ⚠️ The two GLOBAL actions
    // (PARAM_UPDATE_MASTER_EQ / _VOL) carry the trackId of the track that AUTHORED them, and go with
    // it: the track being rolled back is the one that will emit them again.
    void clearFrom(int64_t fromFrame, int trackId = -1) { cancels.cancel(fromFrame, trackId); }
};

// Pre-converted EQ band params (Hz/dB/Q) — populated by setInstrumentEqSlot().
struct EqBandData {
    // ⚠️ 0=OFF 1=LOSHELF 2=LOWCUT 3=BELL 4=HISHELF 5=HICUT. The number IS the identity (it is what the
    // project file stores), and LOWCUT/HICUT were appended — so this is not the order the names
    // suggest. Defined once in effects/modules/eq-module.h; append there, never insert.
    int   type   = 0;
    float freqHz = 1000.0f; // 20–20000 Hz
    float gainDb = 0.0f;    // −12..+12 dB
    float q      = 1.0f;    // 0.1–10.0
};

// Instrument playback parameters
struct InstrumentParams {
    int startPoint;     // 0-255 (normalized position)
    int endPoint;       // 0-255 (normalized position)
    bool reverse;       // Play backwards
    // ⚠️ 3 (OSCILLATOR) is a FORWARD loop whose SCAN RATE is retuned so one trip round the loop is
    // one cycle of the played note. So the note's pitch comes from the note and the loop LENGTH
    // becomes a timbre control — the opposite of mode 1, where a short loop IS the pitch.
    int loopMode;       // 0=off, 1=forward, 2=ping-pong, 3=oscillator
    int loopStart;      // 0-255 (normalized position)
    int loopEnd;        // 0-255 (normalized position); loop region top. 255 = sample end.

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

    // ⚠️ THE EXACT-FRAME WINDOW: −1 = unset, and then startPoint/endPoint above decide. When set it
    // REPLACES them, in frames, because 0-255 cannot express a frame.
    //
    // startPoint/endPoint are eighths of a percent of the buffer: on a 2-second 44.1 kHz sample one
    // step is 346 frames, ~8 ms. That is the right grain for a playback parameter you dial by ear, and
    // the wrong one for the sample editor's audition, which exists to let you hear the exact boundary
    // CROP is about to cut at — dozens of single-frame nudges land inside one step and the audition
    // does not change, then the crop applies the frame you actually chose.
    //
    // Set only by setInstrumentFrameWindow, and CLEARED by every setInstrumentParams push — so an
    // ordinary push of the instrument is what ends a preview's window, and no caller has to remember.
    //
    // ⚠️ Read at TRIGGER, and then carried on the voice (`Voice::windowStartFrame`), because the mix
    // loop re-derives the endpoints from startPoint/endPoint every block. Clearing this mid-note ends
    // the window for the NEXT note, never for one already ringing.
    int startFrame = -1;
    int endFrame   = -1;

    InstrumentParams() : startPoint(0), endPoint(255), reverse(false),
                         loopMode(0), loopStart(0), loopEnd(255), drive(0), crush(0), downsample(0),
                         filterType(0), filterCut(128), filterRes(0), filterDrive(128),
                         eqActive(false), reverbSend(0.0f), delaySend(0.0f),
                         startFrame(-1), endFrame(-1) {}
};

// Per-slot modulation configuration set from Kotlin.
// Copied to VoiceModSlot when a note triggers on that instrument.
struct InstrumentModSlot {
    int type;          // 0=NONE, 1=AHD, 2=ADSR, 3=LFO, 4=DRUM, 5=TRIG, 6=SCALAR
    int dest;          // 0=NONE, 1=VOL, 2=PAN, 3=PITCH, 4=FINE_PITCH, 5=CUT, 6=RES, 7=STA, 8=MOD_AMT, 9=MOD_RATE, 10=MOD_BOTH
    float amount;      // Modulation depth 0.0-1.0 (normalised from 00-FF)
    int attackSamples; // Attack duration in audio samples
    int holdSamples;   // Hold duration in audio samples (AHD hold; unused in ADSR)
    int decaySamples;  // Decay duration in audio samples
    float sustainLevel; // ADSR: sustain level 0.0-1.0
    float lfoHz;        // LFO: frequency in Hz
    int oscShape;       // LFO: 0=TRI,1=SIN,2=RMP+,3=RMP-,4=EXP+,5=EXP-,6=SQU+,7=SQU-,8=RND,9=DRNK
    int lfoTrigMode;    // LFO: 0=FREE, 1=RETG, 2=HOLD, 3=ONCE
    int releaseSamples; // ADSR/TRIG: release duration in audio samples (0 = instant)

    InstrumentModSlot() : type(0), dest(0), amount(0.5f),
                          attackSamples(0), holdSamples(0), decaySamples(0),
                          sustainLevel(0.5f), lfoHz(4.0f), oscShape(0), lfoTrigMode(1),
                          releaseSamples(0) {}
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

static_assert(sizeof(TableRow) == 8, "a table row is one 64-bit word in TableStore");

/**
 * The engine's 256 tables: one writer (the UI's `loadTable`), readers on the audio thread that never
 * wait.
 *
 * Each table has two copies. The writer fills the one readers are NOT pointed at and then points
 * them at it, so a reader always finds a finished copy; a sequence number per copy catches the one
 * case that can still tear — two writes landing inside a single 128-byte read — and the reader
 * simply reads again. The rows are atomic words so the overlap is defined behaviour.
 */
class TableStore {
  public:
    static constexpr int TABLES = 256;
    static constexpr int ROWS   = 16;

    /** Writer. The caller serialises writers. */
    void write(int id, const TableRow (&rows)[ROWS]) {
        Entry& e = entries_[id];
        const int next = e.current.load(std::memory_order_relaxed) ^ 1;
        Copy& c = e.copies[next];
        const uint32_t s = c.seq.load(std::memory_order_relaxed);
        c.seq.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (int r = 0; r < ROWS; ++r) {
            uint64_t w;
            std::memcpy(&w, &rows[r], sizeof w);
            c.rows[r].store(w, std::memory_order_relaxed);
        }
        c.seq.store(s + 2, std::memory_order_release);
        e.current.store(next, std::memory_order_release);
        e.loaded.store(true, std::memory_order_release);
    }

    /** Any thread, never waits. False when the table has never been loaded. */
    bool read(int id, TableRow (&out)[ROWS]) const {
        const Entry& e = entries_[id];
        if (!e.loaded.load(std::memory_order_acquire)) return false;
        // ⚠️ Bounded: a retry needs the writer to have finished a whole copy during our read, so
        // eight in a row cannot happen at editing speed. If it ever did, the table is skipped for
        // this call rather than read torn.
        for (int attempt = 0; attempt < 8; ++attempt) {
            const Copy& c = e.copies[e.current.load(std::memory_order_acquire)];
            const uint32_t s1 = c.seq.load(std::memory_order_acquire);
            if (s1 & 1u) continue;
            uint64_t words[ROWS];
            for (int r = 0; r < ROWS; ++r) words[r] = c.rows[r].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (c.seq.load(std::memory_order_relaxed) != s1) continue;
            std::memcpy(out, words, sizeof words);
            return true;
        }
        return false;
    }

  private:
    struct Copy {
        std::atomic<uint32_t> seq{0};
        std::atomic<uint64_t> rows[ROWS] = {};
    };
    struct Entry {
        Copy copies[2];
        std::atomic<int>  current{0};
        std::atomic<bool> loaded{false};
    };
    Entry entries_[TABLES];
};

// Convert unsigned transpose byte to signed semitones
inline int transposeToSemitones(uint8_t transpose) {
    if (transpose < 0x80) {
        return transpose;  // 00-7F = 0 to +127
    } else {
        return transpose - 256;  // 80-FF = -128 to -1
    }
}
