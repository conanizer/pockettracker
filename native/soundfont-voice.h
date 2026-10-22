#pragma once
#include <cmath>
#include "mods/mod-system.h"
#include "effects/instrument-chain.h"
#include "table-lanes.h"

// tsf is defined in soundfont-voice.cpp; note-queue.h declares soundfonts[].

// A track's SoundFont state. One tsf handle per slot, shared: track N plays on MIDI channel N, the
// preview lane on channel 8. Audio thread only (or a render with the device paused), no lock.
struct SoundfontVoice : public IAudioVoice {
    int   sfSlot      = -1;    // -1 = unassigned
    uint32_t sfGen    = 0;     // the slot's gen when armed; a stale one is detached
    int   _trackId    = -1;    // = the MIDI channel
    int   instrId     = -1;    // -1 = preview; for the instrument spectrum
    bool  isActive    = false;
    int   activeNote  = -1;
    // A chord's other keys on the channel (CHD); whatever ends the key they sound with ends them.
    // ⚠️ Not tied to `activeNote`: a deferred note-off outlives it.
    int   chordWith     = -1;
    int   chordKeys[3]  = {};
    int   chordKeyCount = 0;
    // A strum's keys still to come, each due at `strumAt`, a frame of this block (counted down at
    // every block's end). Ending the chord drops them.
    int   strumKeys[4]  = {};
    int   strumAt[4]    = {};
    int   strumCount    = 0;
    float strumVel      = 0.0f;
    float noteVolume  = 1.0f;  // instrument × phrase × V

    // The instrument's detune, semitones; set at the trigger, survives slides and resetPitchState.
    float detuneSemitones  = 0.0f;

    // PSL/PBN/PVB/PVX, advanced each block and sent as the pitch wheel.
    float pitchOffset      = 0.0f;
    float pitchSlideTarget = 0.0f;
    float pitchSlideRate   = 0.0f;
    bool  pitchSliding     = false;
    float vibratoPhase     = 0.0f;
    float vibratoSpeed     = 0.0f;
    float vibratoDepth     = 0.0f;
    bool  vibratoActive    = false;
    bool  needsPitchReset  = false;

    // Table state, as on Voice.
    int   tableId          = -1;
    TableLane lanes[TABLE_LANES];   // one per FX column
    float tableTranspose   = 0.0f;  // for the debug log
    float tableVolume      = 1.0f;  // for the debug log
    // Picked up through INS rows; every table pitch/volume write adds/multiplies it.
    float carrySemitones   = 0.0f;
    float carryVolume      = 1.0f;
    // A table PAN glides over PAN_GLIDE_SAMPLES.
    Ramp  panGlide;
    // A table VOL blends from the gain it was at: `volGlide` is that gain's share.
    float volGlideFrom     = 0.0f;
    Ramp  volGlide;
    int   noteOctave       = 4;     // for TICFC
    int   notePitch        = 0;     // for TICFE

    // After noteOff(): keep rendering while TSF decays.
    bool  isReleasingOnly  = false;

    // The note's gain, ramped per sample over each rendered piece. ⚠️ Never through
    // tsf_channel_set_volume — one value per render clicks at every block. `volGain` is where the last
    // piece ended; From/To are this piece's.
    float volGain     = 1.0f;
    float volGainFrom = 1.0f;
    float volGainTo   = 1.0f;

    // The stop ramp (STOP, KIL, a short release): 1 to 0 on the mute gate, moving while it runs. Ours,
    // not TSF's (it steps per 64 samples), and on the gate so the sends fade with it.
    Ramp  stopFade;
    // The frame in this block the ramp starts at, as Voice::fadeStartFrame; zeroed after every block.
    int   stopFadeStartFrame = 0;

    // A TSF note-off due at this frame of the block: the render splits there. -1 = none.
    int   pendingTsfOffAt   = -1;
    int   pendingTsfOffNote = -1;

    // The render pass runs the ramp and calls hardStop() at zero.
    void startStopFade(int fadeSamples, int atFrame = 0) {
        if (!isActive || stopFade.moving()) return;
        stopFadeStartFrame = atFrame;
        dropArmedNote();
        stopFade.start(1.0f, 0.0f, fadeSamples > 0 ? fadeSamples : 1);
    }

    // The onset inside the trigger block, as Voice::startDelayFrames.
    int   startDelayFrames = 0;

    // The armed note, held from the dispatch pass until the render pass fires it (see armNote()).
    struct ArmedNote {
        int   slot = -1, midiNote = 0, midiVelocity = 0, bank = 0, preset = 0;
        float noteVol = 1.0f, pan = 0.5f;
        int   envAtk = -1, envDec = -1, envSus = -1, envRel = -1;
        int   keys[4] = {};      // a chord: every key it plays, `midiNote` unused
        int   keyWait[4] = {};   // frames after the onset each key sounds (a strum)
        int   keyCount = 0;      // 0 = the single `midiNote`
    };
    bool      hasArmedNote = false;
    ArmedNote armed;
    // The fader a stolen note keeps when a VTR on the next step moves it.
    bool  faderHeld      = false;
    float faderHeldStart = 1.0f, faderHeldEnd = 1.0f;

    // An armed note will not sound after all (a stop or a note-off came first): the note it would have
    // cut carries on, through its own chain.
    void dropArmedNote() {
        if (hasArmedNote && cutting) chain = cutChain;
        hasArmedNote = false;
        cutting      = false;
    }

    // ── IAudioVoice ──
    bool active()     const override { return isActive; }
    int  getTrackId() const override { return _trackId; }

    void hardStop() override;

    // The armed note as a chord: `semis` from its key, `wait` frames after the onset each (a strum),
    // clamped to MIDI's range, two clamped onto one kept once. Returns how many keys it plays.
    int armChord(const int* semis, const int* wait, int count) {
        armed.keyCount = 0;
        for (int k = 0; k < count && k < 4; ++k) {
            const int key = std::max(0, std::min(127, armed.midiNote + semis[k]));
            int again = -1;
            for (int j = 0; j < armed.keyCount; ++j) if (armed.keys[j] == key) again = j;
            if (again >= 0) {
                armed.keyWait[again] = std::min(armed.keyWait[again], wait[k]);
                continue;
            }
            armed.keyWait[armed.keyCount] = wait[k];
            armed.keys[armed.keyCount++]  = key;
        }
        return std::max(1, armed.keyCount);
    }

    // Note-off for `key`, and for the chord keys when `key` is the note they sound with.
    void keysOff(tsf* h, int key);

    // Starts TSF's release; the render runs until silence, then calls hardStop().
    void noteOff() override;
    // At frame `atFrame` of this block — the audio thread's form.
    void noteOffAt(int atFrame);

    void setVolume(float v) override;

    void setPan(float pan) override;

    void retrigger(int /*startPoint*/) override {}

    void setMidiNote(int midiNote) override;

    // Pitch setters write state only — no TSF calls.
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
        if (depth < 0.01f) { vibratoActive = false; vibratoDepth = 0.0f; }
        else { vibratoSpeed = speed; vibratoDepth = depth; vibratoActive = true; }
    }
    // Unused: processAudioBlock renders SF channels itself.
    float render(float*, int) override { return 0.0f; }

    // Run on the rendered buffer; its values come from the ParamBus once per piece.
    InstrumentChain chain;
    // ⚠️ The note a new one cuts plays on under it for the steal's fade, and must not hear the new note's
    // filter, drive or crush: it finishes through this copy of the chain it had. `cutting` while it does.
    InstrumentChain cutChain;
    bool            cutting = false;

    // ── Audio thread only ──

    // Arms a note; noteVol = instrument × phrase. ⚠️ It does not sound: the render pass calls
    // `fireArmedNote` at the note's frame, or the previous note would be cut dead at the block edge.
    // ⚠️ False when the slot's handle is gone — nothing written; the caller must honour it.
    bool armNote(int slot, int midiNote, int midiVelocity,
                 float noteVol, float pan,
                 int bank, int preset, int trackId,
                 int envAtk, int envDec, int envSus, int envRel);

    // Fires into `h`, the handle loaded for this block, at frame `atFrame` of it; clears `hasArmedNote`
    // either way.
    void fireArmedNote(tsf* h, int atFrame);

    // Sounds every strummed key due at or before frame `atFrame` of this block.
    void fireStrumKeys(tsf* h, int atFrame);
    void strumBlockEnd(int numFrames) {
        for (int k = 0; k < strumCount; ++k) strumAt[k] = std::max(0, strumAt[k] - numFrames);
    }

    // Also re-centres TSF's wheel next block — a PBN can have left it at an extreme.
    void resetPitchState() {
        midiWheel        = 0.0f;
        pitchOffset      = 0.0f;
        pitchSlideTarget = 0.0f;
        pitchSlideRate   = 0.0f;
        pitchSliding     = false;
        vibratoPhase     = 0.0f;
        vibratoActive    = false;
        needsPitchReset  = true;
    }

    void resetTableState(int tblId, const int (&ticRates)[TABLE_LANES], int octave, int pitch,
                         const int (&startRows)[TABLE_LANES]) {
        tableId          = tblId;
        tableTranspose   = 0.0f;
        tableVolume      = 1.0f;
        carrySemitones   = 0.0f;
        carryVolume      = 1.0f;
        volGlide         = Ramp{};
        noteOctave       = octave;
        notePitch        = pitch;
        reset_table_lanes(lanes, ticRates, startRows, octave, pitch);
    }

    // Advances slide and vibrato by `numFrames` and writes the wheel; before each render.
    void applyPitchMod(float sampleRate, int numFrames);

    // The owning slot was unloaded.
    void detach() {
        isActive       = false;
        activeNote     = -1;
        chordKeyCount  = 0;
        strumCount     = 0;
        sfSlot         = -1;
        _trackId       = -1;
        noteVolume     = 1.0f;
        isReleasingOnly = false;
        tableId        = -1;
        startDelayFrames = 0;
        pendingTsfOffAt = -1;
        hasArmedNote   = false;
        cutting        = false;
    }
};

// The bank and number of preset `index`; false when f is null or index out of range.
bool tsf_get_preset_at(tsf* f, int index, int* bank, int* preset_number);

// Whether the last null `tsf_load` was the memory guard refusing ("too big") rather than a broken
// file. Reset before a load, read after; per thread.
void sf_memory_guard_reset();
bool sf_memory_guard_tripped();
