#pragma once
#include <cmath>
#include "mod-system.h"

// Forward declaration — tsf is defined in soundfont-voice.cpp (TSF_IMPLEMENTATION).
// note-queue.h declares SoundfontEntry and extern soundfonts[].

// ===================================
// SOUNDFONTVOICE — per-track state; rendering via shared soundfonts[sfSlot].handle
// ===================================
// Design: ONE tsf* instance per SoundfontEntry (per unique SF2 file).
// Each of the 8 tracks maps to a MIDI channel (0–7) on that shared instance.
// This eliminates the per-track tsf_load_memory() call that previously stalled the
// audio callback for hundreds of ms and used 8× the SF2 file size in RAM.
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

    // ── IAudioVoice ─────────────────────────────────────────────────────────
    bool active()     const override { return isActive; }
    int  getTrackId() const override { return _trackId; }

    // Called from JNI thread (stop button) or audio thread (kill note queue).
    void hardStop() override;

    void noteOff() override { hardStop(); }  // TSF handles its own release tail

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

    // ── Audio-thread-only methods (no lock needed) ──────────────────────────

    // Trigger a new note. Called from processAudioBlock (audio thread).
    // noteVol = instrument × phrase volume (from Kotlin).
    // trkVol  = current track mixer volume (fetched from trackVolumes[] at call site).
    // TSF channel volume = noteVol * trkVol so per-track mixing works on the shared handle.
    void triggerNote(int slot, int midiNote, int midiVelocity,
                     float noteVol, float trkVol, float pan,
                     int bank, int preset, int trackId);

    // Reset pitch state after a new note trigger.
    void resetPitchState() {
        pitchOffset      = 0.0f;
        pitchSlideTarget = 0.0f;
        pitchSlideRate   = 0.0f;
        pitchSliding     = false;
        vibratoPhase     = 0.0f;
        vibratoActive    = false;
        needsPitchReset  = false;
    }

    // Advance pitch LFO/slide and write MIDI pitch wheel to the shared handle.
    // Called once per audio block, BEFORE the per-slot render. Audio thread only.
    void applyPitchMod(float sampleRate, int numFrames);

    // Reset voice state when the owning slot is unloaded.
    void detach() {
        isActive    = false;
        activeNote  = -1;
        sfSlot      = -1;
        _trackId    = -1;
        noteVolume  = 1.0f;
        trackVolume = 1.0f;
    }
};

// Helper: get bank and preset_number for a preset at the given index.
// Defined in soundfont-voice.cpp where TSF_IMPLEMENTATION is active (full tsf struct visible).
// Returns true on success, false if f is null or index is out of range.
bool tsf_get_preset_at(tsf* f, int index, int* bank, int* preset_number);
