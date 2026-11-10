#pragma once
#include <queue>
#include <mutex>
#include <vector>
#include <atomic>
#include <cstdint>
#include "audio-defs.h"
#include "songcore/program.h"   // NoteOnPayload — a queued note carries it until the trigger
#include "table-lanes.h"

// A note at its exact frame; the audio block triggers it there.
struct ScheduledNote {
    int64_t targetFrame;
    int sampleId;            // 0-255
    int trackId;             // 0-7, 8 = preview lane
    float frequency;
    float baseFrequency;     // the sample's root
    float volume;            // instrument volume 0-1 → MOD_SRC_INSTR_VOL
    float phraseVolume;      // step volume 0-1 → MOD_SRC_PHRASE_VOL
    float pan;               // 0 left, 0.5 centre, 1 right
    int startPointOverride;  // -1 = the instrument's
    int endPointOverride;    // CUT slice end; -1 = the instrument's
    // A slice's exact window, which the 0-255 overrides above only approximate. -1 = none; an end of
    // -1 = the END cell's. ⚠️ Defaulted: `ScheduledNote{}` must not read as a slice at frame 0.
    int sliceStartFrame = -1;
    int sliceEndFrame   = -1;

    int tableId;             // -1 = none
    int tableTicRate;        // ticks per table row

    int noteOctave;          // 0-9, for TICFC
    int notePitch;           // 0-11 (C = 0), for TICFE

    // ⚠️ Already per FRAME — voice_derive.h converts out of the ticks/steps the cells are written in.
    float pslInitialOffset;  // PSL start offset, semitones (0 = none)
    float pslDuration;       // PSL slide length, frames
    float pbnRate;           // PBN, semitones per frame
    float vibratoSpeed;      // PVB/PVX, Hz
    float vibratoDepth;      // PVB/PVX, semitones

    int tableStartRow;       // THO: -1 = default (0 or TIC00 continuity), else 0-15

    // LEG: the slide onto the note still sounding, in frames (0 = at once); -1 = a plain note.
    float legatoFrames = -1.0f;

    // SoundFont notes only.
    bool isSoundfont = false;
    int  sfSlot      = -1;      // into soundfonts[]
    int  midiNote    = 60;
    int  midiVelocity = 100;
    int  sfBank      = 0;
    int  sfPreset    = 0;
    float detuneSemitones = 0.0f; // the instrument's fine detune

    // ⚠️ With `instrumentId >= 0` every field above is UNSET: it is derived at the trigger from the
    // program table, so a table row can change the instrument on the hit. -1 = arrived derived
    // (previews, retrigger, MIDI in, the file browser).
    int instrumentId = -1;
    songcore::NoteOnPayload noteOn{};   // pitch, velocity, pan, PSL/PBN/vibrato
    TableCarry carry{};                 // filled at the trigger by the INS walk
    int  tempo        = 120;            // tempo at scheduling — the note's tick→frame scale
    bool rootAudition = false;          // INSTRUMENT-screen root preview only
    uint32_t gen = 0;                   // stamped by the queue — see CancelLedger

    bool operator>(const ScheduledNote& other) const {
        return targetFrame > other.targetFrame;
    }
};

// The engine's own numbers, unrelated to event.h's NOTE_OFF_* — the consumer translates.
enum KillMode : uint8_t {
    KILL_HARD    = 0,   // an audition ending, a track handed to MIDI out — a sampler fades, a SoundFont releases
    KILL_SOFT    = 1,   // an OFF note — ADSR release, else a declick fade
    KILL_KEY_OFF = 2,   // a KEY released (MIDI in) — a one-shot IGNORES it and plays out
    KILL_CUT     = 3,   // KIL, a held audition let go of — KILL_FADE_SAMPLES and gone, no release tail at all
};

struct ScheduledKill {
    int64_t targetFrame;
    int trackId;
    KillMode mode = KILL_HARD;
    uint32_t gen = 0;        // stamped by the queue — see CancelLedger

    bool operator>(const ScheduledKill& other) const {
        return targetFrame > other.targetFrame;
    }
};

// A LIVE launch drops what one track had queued from the rewind frame on — lazily: the UI records
// "generation g of lane L ended at frame F", and the drain skips an entry a later cancel covers. The
// heap is never rebuilt, so the UI holds the queue mutex no longer than a push.
// ⚠️ Every cancel since the entry's generation is checked, not just the latest: a note queued between
// two rewinds is kept. An entry older than HISTORY cancels counts as cancelled.
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

// A min-heap of scheduled events by frame. The UI thread pushes; so does the audio thread for a live
// key, before the block's drainUntil.
template <typename T>
class EventQueue {
public:
    // Reserved up front: a live key pushes from the audio thread, where growing would allocate.
    explicit EventQueue(const char* clearedLog = nullptr)
        : queue(std::greater<T>(), reserved()), clearedLog_(clearedLog) {}

    void schedule(const T& event) {
        std::lock_guard<std::mutex> lock(mutex);
        T stamped = event;
        stamped.gen = cancels.current(event.trackId);
        queue.push(stamped);
    }

    // Appends every event due by `maxFrame` to `out`, earliest first, under one lock. Cancelled ones
    // are dropped here.
    void drainUntil(int64_t maxFrame, std::vector<T>& out) {
        std::lock_guard<std::mutex> lock(mutex);
        while (!queue.empty() && queue.top().targetFrame <= maxFrame) {
            const T& e = queue.top();
            if (!cancels.cancelled(e.gen, e.trackId, e.targetFrame)) out.push_back(e);
            queue.pop();
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        while (!queue.empty()) queue.pop();
        if (clearedLog_) LOGD("%s", clearedLog_);
    }

    // Drops the events queued at or after `fromFrame`; no lock (see CancelLedger).
    // ⚠️ `trackId >= 0` drops one track's only — each track rewinds to its own phrase boundary, and
    // another track's events past that frame will not be queued again.
    void clearFrom(int64_t fromFrame, int trackId = -1) { cancels.cancel(fromFrame, trackId); }

private:
    static std::vector<T> reserved() {
        std::vector<T> v;
        v.reserve(64);
        return v;
    }
    std::priority_queue<T, std::vector<T>, std::greater<T>> queue;
    std::mutex mutex;
    CancelLedger cancels;
    const char* clearedLog_;
};

struct NoteQueue : EventQueue<ScheduledNote> { NoteQueue() : EventQueue("🗑️ Note queue cleared") {} };
struct KillQueue : EventQueue<ScheduledKill> { KillQueue() : EventQueue("🗑️ Kill queue cleared") {} };

// Every live change to a sounding voice or the mixer goes through this queue, so it is written on the
// audio thread at the step's exact frame. The numbers are never stored.
enum ParamUpdateAction {
    PARAM_UPDATE_MOD_SOURCE = 0,  // modSourceValues[sourceId] = value                   [Vxx]
    PARAM_UPDATE_PITCH_BEND,      // the track's note: setPitchBendRaw(value)            [PBN]
    PARAM_UPDATE_VIBRATO,         // the track's note: setVibratoRaw(value, value2)      [PVB/PVX]
    PARAM_UPDATE_TABLE_ROW,       // the track's voices: every table column to row (int)value [THO]
    // `sourceId` = the CC id (songcore/event.h), `value` 0-1; applied by applyVoiceCc, as table rows are.
    PARAM_UPDATE_VOICE_CC,         // PAN, REV, DEL, CUT, RES, LPF/HPF/BPF, DRV, CRU, FIN, LPO
    PARAM_UPDATE_REVERSE,         // sampler voice: reverse=(value!=0); value2!=0 → snap pos to new-dir boundary [BCK]
    // A slot names a preset; BANDS carry values in `eqBands` (an AUS/AUF morph). (int)value < 0 = bypass.
    PARAM_UPDATE_EQ_SLOT,         // the voice's EQ   [EQN]
    PARAM_UPDATE_EQ_BANDS,        //                  [EQN + AUS/AUF]
    PARAM_UPDATE_MASTER_EQ,       // the master EQ    [EQM]
    PARAM_UPDATE_MASTER_EQ_BANDS, //                  [EQM + AUS/AUF]
    // ⚠️ The block snapshots the faders above its frame loop; these arms write the member AND that
    // snapshot, or the change lands a block late.
    PARAM_UPDATE_TRACK_VOL,       // mixer: trackVolumes[trackId] = value          [VTR]
    PARAM_UPDATE_MASTER_VOL,      // mixer: masterVolume = value (global)          [VMV]
    // The delay moves its head per sample itself, so no snapshot to write.
    PARAM_UPDATE_DELAY_TIME,      // global: the delay's echo time = value*255, free scale      [TIM]
    // ⚠️ Per instrument (`instrId`), no value: a voice copies its instrument's filter, drive, crush and
    // sends at the trigger, so an edit reaches sounding voices only through this.
    PARAM_UPDATE_INSTRUMENT,      // every sounding voice of `instrId` re-reads that instrument
    PARAM_UPDATE_WHEEL,           // the track's voices, fading ones too: midiWheel = value   [MIDI in]
    PARAM_UPDATE_CHORD_GLIDE,     // the track's chord: each note to `chordSemis`, over `value` frames [CHD + AUS/AUF]
};

// One EQ setting in the file's hex, not Hz/dB/Q: interpolating hex keeps a sweep linear in
// log-frequency. `applyEqBandsToChain` converts, as setEqBand() does.
struct EqBandsHex {
    int type[3] = {};                    // 0 OFF | 1 LOSHELF | 2 LOWCUT | 3 BELL | 4 HISHELF | 5 HICUT
    int freq[3] = { 128, 128, 128 };     // 00-FF → 20-20000 Hz, log
    int gain[3] = { 120, 120, 120 };     // 0-240 → −12.0..+12.0 dB (120 = flat)
    int q[3]    = { 128, 128, 128 };     // 00-FF → 0.1-10.0, log
};

struct ScheduledParamUpdate {
    int64_t targetFrame;
    int trackId;
    int sourceId;            // ModSourceId (MOD_SOURCE) or CC id (VOICE_CC)
    float value;             // mod-source value / bend rate / vibrato speed / table row
    int action = PARAM_UPDATE_MOD_SOURCE;
    float value2 = 0.0f;     // vibrato depth
    // ⚠️ Call sites initialise this struct positionally and stop before here — add fields below, never above.
    EqBandsHex eqBands{};    // EQ_BANDS / MASTER_EQ_BANDS only
    int instrId = -1;        // INSTRUMENT only
    float chordSemis[4] = {};  // CHORD_GLIDE only: semitones from the note, lowest first
    uint32_t gen = 0;        // stamped by the queue — see CancelLedger

    bool operator>(const ScheduledParamUpdate& other) const {
        return targetFrame > other.targetFrame;
    }
};

// ⚠️ The global actions (MASTER_EQ / _VOL) carry the authoring track's id and are dropped with it by
// clearFrom — that track will emit them again.
struct ParamUpdateQueue : EventQueue<ScheduledParamUpdate> {};
