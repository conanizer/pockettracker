#pragma once
#include <cmath>
#include "mods/mod-system.h"
#include "effects/instrument-chain.h"

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
//   audio thread : triggerNote(), applyPitchMod() — sequential, no lock needed.
//   audio thread : tsf_render_float()             — holds soundfonts[slot].mutex.
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
    int   tableRow         = 0;
    int   lastProcessedRow = -1;
    int   tableTicRate     = 6;
    int   tableTicCounter  = 0;
    float tic200HzAccum    = 0.0f;
    float tableFrameAccum  = 0.0f;  // Frame accumulator for standard tempo-locked tic mode (01-FB)
    float tableTranspose   = 0.0f;  // current semitones from table row (for debug)
    float tableVolume      = 1.0f;  // current vol multiplier from table row (for debug)
    int   hopRepeatCount   = 0;
    int   hopTargetRow     = -1;
    int   noteOctave       = 4;     // note octave (for TICFC/TICFE special modes)
    int   notePitch        = 0;     // note pitch  (for TICFE mode)

    // Release tail: true after noteOff() — keeps rendering while TSF decays to silence.
    bool  isReleasingOnly  = false;

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
        if (depth < 0.01f) { vibratoActive = false; vibratoDepth = 0.0f; }
        else { vibratoSpeed = speed; vibratoDepth = depth; vibratoActive = true; }
    }
    void clearPitchMod() override {
        pitchOffset    = 0.0f;
        pitchSliding   = false;
        pitchSlideRate = 0.0f;
        vibratoActive  = false;
        vibratoDepth   = 0.0f;
        needsPitchReset = true;  // audio thread resets pitch wheel on next block
    }
    void setInitialPitchOffset(float semitones) override { pitchOffset = semitones; }
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

    // Trigger a new note. Called from processAudioBlock (audio thread).
    // noteVol = instrument × phrase volume (from Kotlin).
    // trkVol  = current track mixer volume (fetched from trackVolumes[] at call site).
    // TSF channel volume = noteVol * trkVol so per-track mixing works on the shared handle.
    void triggerNote(int slot, int midiNote, int midiVelocity,
                     float noteVol, float trkVol, float pan,
                     int bank, int preset, int trackId,
                     int envAtk, int envDec, int envSus, int envRel);

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
    void resetTableState(int tblId, int ticRate, int octave, int pitch, int startRow) {
        tableId          = tblId;
        tableRow         = (startRow >= 0) ? startRow % 16 : 0;
        lastProcessedRow = -1;
        tableTicRate     = ticRate;
        tableTicCounter  = 0;
        tic200HzAccum    = 0.0f;
        tableFrameAccum  = 0.0f;
        tableTranspose   = 0.0f;
        tableVolume      = 1.0f;
        hopRepeatCount   = 0;
        hopTargetRow     = -1;
        noteOctave       = octave;
        notePitch        = pitch;
        // TICFC/TICFE special modes: pin starting row to note octave/pitch
        if (ticRate == 0xFC) tableRow = std::min(octave, 15);
        else if (ticRate == 0xFE) tableRow = pitch;
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
    }
};

// Helper: get bank and preset_number for a preset at the given index.
// Defined in soundfont-voice.cpp where TSF_IMPLEMENTATION is active (full tsf struct visible).
// Returns true on success, false if f is null or index is out of range.
bool tsf_get_preset_at(tsf* f, int index, int* bank, int* preset_number);
