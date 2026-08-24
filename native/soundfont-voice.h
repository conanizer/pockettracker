#pragma once
#include <cmath>
#include "mods/mod-system.h"
#include "effects/instrument-chain.h"
#include "table-lanes.h"

// Forward declaration — tsf is defined in soundfont-voice.cpp (TSF_IMPLEMENTATION).
// note-queue.h declares SoundfontEntry and extern soundfonts[].

// ===================================
// SOUNDFONTVOICE — per-track state; rendering via shared soundfonts[sfSlot].handle
// ===================================
// Design: ONE tsf* instance per SoundfontEntry (per unique SF2 file).
// Each of the 8 tracks maps to a MIDI channel (0–7) on that shared instance; the dedicated
// preview lane (track 8) uses channel 8, so SF previews never steal a song track's channel.
// One shared instance avoids a per-track tsf_load_memory() call that would otherwise stall the
// audio callback for hundreds of ms and use 8× the SF2 file size in RAM.
//
// Thread safety:
//   audio thread : armNote(), applyPitchMod()     — holds soundfonts[slot].mutex.
//   audio thread : tsf_render_float()             — holds soundfonts[slot].mutex.
//   audio thread : fireArmedNote()                — the CALLER's lock; it takes none of its own.
//   JNI thread   : hardStop(), setVolume(), etc.  — holds soundfonts[slot].mutex.
struct SoundfontVoice : public IAudioVoice {
    int   sfSlot      = -1;    // soundfonts[] index that owns this voice (-1 = unassigned)
    int   _trackId    = -1;    // Track index = MIDI channel on the shared tsf* handle
    bool  isActive    = false;
    int   activeNote  = -1;
    float noteVolume  = 1.0f;  // Note-only volume (instrument × phrase × V-effect)
    float trackVolume = 1.0f;  // Cached track volume; combined with noteVolume for TSF channel

    // Static per-instrument detune in semitones (fractional). Independent of PSL/PBN so it survives
    // pitch slides; folded into pitchMod every block. Set at note trigger, NOT cleared by resetPitchState.
    float detuneSemitones  = 0.0f;

    // Pitch effect state (PSL/PBN/PVB/PVX) — advanced each block, applied via MIDI pitch wheel
    float pitchOffset      = 0.0f;
    float pitchSlideTarget = 0.0f;
    float pitchSlideRate   = 0.0f;
    bool  pitchSliding     = false;
    float vibratoPhase     = 0.0f;
    float vibratoSpeed     = 0.0f;
    float vibratoDepth     = 0.0f;
    bool  vibratoActive    = false;
    // Written from JNI thread, read+cleared on audio thread — ARM64 bool write is atomic.
    bool  needsPitchReset  = false;

    // Table state — mirrors Voice table fields; populated from scheduleSoundfontNote.
    int   tableId          = -1;
    TableLane lanes[TABLE_LANES];   // one cursor and one rate per FX column — see table-lanes.h
    float tableTranspose   = 0.0f;  // current semitones from table row (for debug)
    float tableVolume      = 1.0f;  // current vol multiplier from table row (for debug)
    int   noteOctave       = 4;     // note octave (for TICFC/TICFE special modes)
    int   notePitch        = 0;     // note pitch  (for TICFE mode)

    // Release tail: true after noteOff() — keeps rendering while TSF decays to silence.
    bool  isReleasingOnly  = false;

    // Intra-block onset offset (same contract as Voice::startDelayFrames): the SF render
    // pass starts this channel's tsf render at this offset within the trigger block so a
    // mid-block targetFrame doesn't sound at the block start, then zeroes it.
    int   startDelayFrames = 0;

    // ── The armed note ──────────────────────────────────────────────────────────────────────────
    // Everything a trigger needs, held from the dispatch pass until the RENDER pass fires it. See
    // armNote() for why the note_on cannot happen where the note is scheduled.
    struct ArmedNote {
        int   slot = -1, midiNote = 0, midiVelocity = 0, bank = 0, preset = 0;
        float noteVol = 1.0f, trkVol = 1.0f, pan = 0.5f;
        int   envAtk = -1, envDec = -1, envSus = -1, envRel = -1;
    };
    bool      hasArmedNote = false;
    ArmedNote armed;

    // ── IAudioVoice ─────────────────────────────────────────────────────────
    bool active()     const override { return isActive; }
    int  getTrackId() const override { return _trackId; }

    // Called from JNI thread (stop button) or audio thread (kill note queue).
    void hardStop() override;

    // Soft note-off: tell TSF to start its internal release envelope, keep rendering until silence.
    // isActive stays true so the audio block keeps calling tsf_render_float_channel.
    // The render loop detects silence and calls hardStop() to clean up.
    void noteOff() override;

    void setVolume(float v) override;

    void setPan(float pan) override;

    void retrigger(int /*startPoint*/) override {}  // not applicable to SF

    void setMidiNote(int midiNote) override;

    // ── Pitch effect interface ───────────────────────────────────────────────
    // All pitch setters only write state fields — no TSF calls, safe from JNI thread.
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
    // ── end IAudioVoice ─────────────────────────────────────────────────────

    // render() is intentionally unused for SF voices.
    // Rendering is done per-slot in processAudioBlock (one tsf_render_float per active slot).
    float render(float*, int) override { return 0.0f; }

    // ── Effects — applied post-render to per-channel TSF buffer ─────────────
    // Copied from instrumentParams[] at note trigger; independent per SF track.
    InstrumentParams instrParams;
    // Per-track stereo effect chain (filter + future modules).
    InstrumentChain chain;

    // ── Audio-thread-only methods (no lock needed) ──────────────────────────

    // Arm a new note. Called from processAudioBlock's dispatch pass (audio thread).
    // noteVol = instrument × phrase volume.
    // trkVol  = current track mixer volume (fetched from trackVolumes[] at call site).
    // TSF channel volume = noteVol * trkVol so per-track mixing works on the shared handle.
    //
    // ⚠️⚠️ **IT ARMS; IT DOES NOT SOUND.** The note_on happens in `fireArmedNote`, which the SF render
    // pass calls at this note's exact intra-block frame — AFTER it has rendered the frames before it.
    // Sounding the note here instead is what used to make an SF note change CRACK: the render pass
    // starts a trigger block at `startDelayFrames` and leaves the head silent, so a note that had
    // already replaced the previous one at dispatch time cut it dead at the block boundary, at
    // whatever amplitude its waveform happened to be at. Nothing on the note's own timing changes —
    // `startDelayFrames` decided when it sounds before this split and decides it still.
    //
    // ⚠️ **RETURNS FALSE WHEN THE SLOT'S HANDLE IS GONE, AND THE CALLER MUST HONOUR IT.** Only a
    // function holding the slot mutex can answer that; anything the caller checked beforehand is a
    // hint that may already be stale. On false NOTHING has been written — the voice is left exactly
    // as it was, still sounding whatever it was sounding, which is the point: the eighty lines of
    // chain/envelope/mod setup that follow a trigger belong to a note that is actually going to play.
    bool armNote(int slot, int midiNote, int midiVelocity,
                 float noteVol, float trkVol, float pan,
                 int bank, int preset, int trackId,
                 int envAtk, int envDec, int envSus, int envRel);

    // Fire the armed note into `h`. ⚠️ The caller must already hold `soundfonts[sfSlot].mutex` and
    // must have read `h` from the handle under it — this is called from inside the render pass's lock.
    // Clears `hasArmedNote` either way.
    void fireArmedNote(tsf* h);

    // Reset pitch state after a new note trigger.
    // needsPitchReset=true so applyPitchMod() resets the TSF pitch wheel to center on the
    // next audio block — required when a previous note left the wheel at an extreme value (PBN).
    void resetPitchState() {
        pitchOffset      = 0.0f;
        pitchSlideTarget = 0.0f;
        pitchSlideRate   = 0.0f;
        pitchSliding     = false;
        vibratoPhase     = 0.0f;
        vibratoActive    = false;
        needsPitchReset  = true;
    }

    // Reset table state for a new note.
    void resetTableState(int tblId, const int (&ticRates)[TABLE_LANES], int octave, int pitch,
                         const int (&startRows)[TABLE_LANES]) {
        tableId          = tblId;
        tableTranspose   = 0.0f;
        tableVolume      = 1.0f;
        noteOctave       = octave;
        notePitch        = pitch;
        reset_table_lanes(lanes, ticRates, startRows, octave, pitch);
    }

    // Advance pitch LFO/slide and write MIDI pitch wheel to the shared handle.
    // Called once per audio block, BEFORE the per-slot render. Audio thread only.
    void applyPitchMod(float sampleRate, int numFrames);

    // Reset voice state when the owning slot is unloaded.
    void detach() {
        isActive       = false;
        activeNote     = -1;
        sfSlot         = -1;
        _trackId       = -1;
        noteVolume     = 1.0f;
        trackVolume    = 1.0f;
        isReleasingOnly = false;
        tableId        = -1;
        startDelayFrames = 0;
        hasArmedNote   = false;  // the slot it was armed against is the one being unloaded
    }
};

// Helper: get bank and preset_number for a preset at the given index.
// Defined in soundfont-voice.cpp where TSF_IMPLEMENTATION is active (full tsf struct visible).
// Returns true on success, false if f is null or index is out of range.
bool tsf_get_preset_at(tsf* f, int index, int* bank, int* preset_number);

// ─── Why the last soundfont load returned null ───────────────────────────────────────────────────
//
// `tsf_load` reports failure the same way whatever went wrong: a null return. A file that is not a
// soundfont and one that is simply too big for the machine are indistinguishable to the caller, and
// the two want opposite messages on screen — "this file is broken" against "this device cannot hold
// it". The allocator guard (soundfont-voice.cpp) is the only thing that can tell them apart, because
// it is what refused.
//
// Reset before a load, read after. ⚠️ Not thread-safe by design and does not need to be: soundfont
// loads happen on one thread, and the flag is read immediately after the load that set it.
void sf_memory_guard_reset();
bool sf_memory_guard_tripped();
