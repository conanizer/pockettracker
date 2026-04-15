// TinySoundFont — single-header SF2/SF3 renderer (MIT license)
// NOTE: TSF_IMPLEMENTATION must be defined in exactly one .cpp file
#define TSF_IMPLEMENTATION
#include "tsf.h"

#include "soundfont-voice.h"

// ===================================
// SOUNDFONT INFRASTRUCTURE (TinySoundFont)
// ===================================
// Supports up to MAX_SOUNDFONTS simultaneously loaded SF2/SF3 files.
// tsf is NOT thread-safe — each entry has its own mutex.
// The mutex is held by:
//   • audio thread   — triggerNote(), applyPitchMod(), tsf_render_float()
//   • JNI/main thread — hardStop(), setVolume(), setPan(), unloadSoundfont()

SoundfontEntry soundfonts[MAX_SOUNDFONTS];

// ── SoundfontVoice method implementations ──────────────────────────────────

void SoundfontVoice::hardStop() {
    if (sfSlot >= 0 && sfSlot < MAX_SOUNDFONTS) {
        std::lock_guard<std::mutex> lock(soundfonts[sfSlot].mutex);
        tsf* h = soundfonts[sfSlot].handle;
        if (h && activeNote >= 0) tsf_channel_note_off(h, _trackId, activeNote);
    }
    activeNote = -1;
    isActive   = false;
}

void SoundfontVoice::setVolume(float v) {
    noteVolume = v;
    if (sfSlot >= 0 && sfSlot < MAX_SOUNDFONTS) {
        std::lock_guard<std::mutex> lock(soundfonts[sfSlot].mutex);
        tsf* h = soundfonts[sfSlot].handle;
        if (h) tsf_channel_set_volume(h, _trackId, v * trackVolume);
    }
}

void SoundfontVoice::setPan(float pan) {
    if (sfSlot >= 0 && sfSlot < MAX_SOUNDFONTS) {
        std::lock_guard<std::mutex> lock(soundfonts[sfSlot].mutex);
        tsf* h = soundfonts[sfSlot].handle;
        if (h) tsf_channel_set_pan(h, _trackId, pan);
    }
}

void SoundfontVoice::setMidiNote(int midiNote) {
    if (sfSlot < 0 || sfSlot >= MAX_SOUNDFONTS) return;
    std::lock_guard<std::mutex> lock(soundfonts[sfSlot].mutex);
    tsf* h = soundfonts[sfSlot].handle;
    if (!h) return;
    if (activeNote >= 0) tsf_channel_note_off(h, _trackId, activeNote);
    tsf_channel_note_on(h, _trackId, midiNote, noteVolume);
    activeNote = midiNote;
}

void SoundfontVoice::triggerNote(int slot, int midiNote, int midiVelocity,
                                 float noteVol, float trkVol, float pan,
                                 int bank, int preset, int trackId) {
    sfSlot      = slot;
    _trackId    = trackId;
    noteVolume  = noteVol;
    trackVolume = trkVol;
    tsf* h = soundfonts[slot].handle;
    if (!h) return;
    if (activeNote >= 0) tsf_channel_note_off(h, _trackId, activeNote);
    tsf_channel_set_pan(h, _trackId, pan);
    tsf_channel_set_volume(h, _trackId, noteVol * trkVol);
    tsf_channel_set_bank_preset(h, _trackId, bank, preset);
    tsf_channel_note_on(h, _trackId, midiNote, midiVelocity / 127.0f);
    activeNote = midiNote;
    isActive   = true;
}

void SoundfontVoice::applyPitchMod(float sampleRate, int numFrames) {
    if (sfSlot < 0 || sfSlot >= MAX_SOUNDFONTS) return;
    tsf* h = soundfonts[sfSlot].handle;
    if (!h) return;

    constexpr float PITCH_RANGE = 48.0f;
    if (needsPitchReset) {
        tsf_channel_set_pitchrange(h, _trackId, PITCH_RANGE);
        tsf_channel_set_pitchwheel(h, _trackId, 8192);
        needsPitchReset = false;
    }

    if (!pitchSliding && !vibratoActive) return;

    // Advance pitch slide (PSL / PBN)
    if (pitchSliding) {
        float delta      = pitchSlideTarget - pitchOffset;
        float totalDelta = pitchSlideRate * numFrames;
        if (fabsf(totalDelta) >= fabsf(delta)) {
            pitchOffset = pitchSlideTarget;
            if (fabsf(pitchSlideTarget) < 100.0f) pitchSliding = false;
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

    float pitchMod = pitchOffset;
    if (vibratoActive) pitchMod += sinf(vibratoPhase) * vibratoDepth;

    float clamped    = fmaxf(-PITCH_RANGE, fminf(PITCH_RANGE, pitchMod));
    int   pitchWheel = (int)(8192.0f + clamped / PITCH_RANGE * 8191.0f);
    if (pitchWheel < 0) pitchWheel = 0;
    if (pitchWheel > 16383) pitchWheel = 16383;

    tsf_channel_set_pitchrange(h, _trackId, PITCH_RANGE);
    tsf_channel_set_pitchwheel(h, _trackId, pitchWheel);
}
