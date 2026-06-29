// TinySoundFont — single-header SF2/SF3 renderer (MIT license)
// NOTE: TSF_IMPLEMENTATION must be defined in exactly one .cpp file
#define TSF_IMPLEMENTATION
#include "vendor/tsf/tsf.h"

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

// NOTE on the `int slot = sfSlot;` snapshots below: detach() (JNI thread, SF2 eviction) sets
// sfSlot = -1 at any moment. Checking the member and then re-reading it to index soundfonts[]
// is a TOCTOU race — soundfonts[-1] is out of bounds and yields a garbage tsf*. Always copy
// to a local once, validate the local, and index with the local only.

void SoundfontVoice::hardStop() {
    int slot = sfSlot;
    if (slot >= 0 && slot < MAX_SOUNDFONTS) {
        std::lock_guard<std::mutex> lock(soundfonts[slot].mutex);
        tsf* h = soundfonts[slot].handle;
        if (h && activeNote >= 0) tsf_channel_note_off(h, _trackId, activeNote);
    }
    activeNote      = -1;
    isActive        = false;
    isReleasingOnly = false;
}

void SoundfontVoice::noteOff() {
    isReleasingOnly = true;

    // Decide whether to defer tsf_channel_note_off based on active ADSR/TRIG VOL mods.
    //
    // ADSR path (ADSR/TRIG VOL mod active):
    //   Do NOT send note_off yet — TSF must keep generating audio at sustain so the
    //   ADSR channel-volume fade is audible. The rendering loop sends note_off via
    //   hardStop() once all ADSR/TRIG VOL mods reach stage 5 (done).
    //
    // TSF REL path (no ADSR/TRIG VOL mod):
    //   Send note_off now so TSF's own release envelope (configured via the SF REL
    //   parameter) plays out. Silence detection in the render loop fires hardStop().
    bool hasActiveAdsrVolMod = false;
    for (int m = 0; m < 4; m++) {
        const VoiceModSlot& mod = voiceMods[m];
        if (mod.dest == 1 && (mod.type == 2 || mod.type == 5)
                && mod.stage >= 1 && mod.stage <= 3) {
            hasActiveAdsrVolMod = true;
            break;
        }
    }

    if (hasActiveAdsrVolMod) {
        // Keep activeNote so hardStop() can send the deferred note_off to TSF.
        for (int m = 0; m < 4; m++) {
            VoiceModSlot& mod = voiceMods[m];
            if (mod.dest == 1 && (mod.type == 2 || mod.type == 5)
                    && mod.stage >= 1 && mod.stage <= 3) {
                mod.stage        = 4;
                mod.stageCounter = 0;
            }
        }
    } else {
        int slot = sfSlot;
        if (slot >= 0 && slot < MAX_SOUNDFONTS) {
            std::lock_guard<std::mutex> lock(soundfonts[slot].mutex);
            tsf* h = soundfonts[slot].handle;
            if (h && activeNote >= 0) tsf_channel_note_off(h, _trackId, activeNote);
        }
        activeNote = -1;
    }
}

void SoundfontVoice::setVolume(float v) {
    noteVolume = v;
    int slot = sfSlot;
    if (slot >= 0 && slot < MAX_SOUNDFONTS) {
        std::lock_guard<std::mutex> lock(soundfonts[slot].mutex);
        tsf* h = soundfonts[slot].handle;
        if (h) tsf_channel_set_volume(h, _trackId, v * trackVolume);
    }
}

void SoundfontVoice::setPan(float pan) {
    int slot = sfSlot;
    if (slot >= 0 && slot < MAX_SOUNDFONTS) {
        std::lock_guard<std::mutex> lock(soundfonts[slot].mutex);
        tsf* h = soundfonts[slot].handle;
        if (h) tsf_channel_set_pan(h, _trackId, pan);
    }
}

void SoundfontVoice::setMidiNote(int midiNote) {
    int slot = sfSlot;
    if (slot < 0 || slot >= MAX_SOUNDFONTS) return;
    std::lock_guard<std::mutex> lock(soundfonts[slot].mutex);
    tsf* h = soundfonts[slot].handle;
    if (!h) return;
    if (activeNote >= 0) tsf_channel_note_off(h, _trackId, activeNote);
    tsf_channel_note_on(h, _trackId, midiNote, noteVolume);
    activeNote = midiNote;
}

void SoundfontVoice::triggerNote(int slot, int midiNote, int midiVelocity,
                                 float noteVol, float trkVol, float pan,
                                 int bank, int preset, int trackId,
                                 int envAtk, int envDec, int envSus, int envRel) {
    sfSlot      = slot;
    _trackId    = trackId;
    noteVolume  = noteVol;
    trackVolume = trkVol;
    // Hold the slot mutex for the whole trigger: the handle must be read inside the lock
    // (loadSoundfont eviction can tsf_close it concurrently), and the channel setup below
    // mutates TSF state that must not interleave with a close.
    std::lock_guard<std::mutex> lock(soundfonts[slot].mutex);
    tsf* h = soundfonts[slot].handle;
    if (!h) return;
    // Hard-kill all TSF voices on this channel — no release-tail overlap.
    // New note on same track = voice-steal (immediate cut), matching sampler behavior.
    // noteOff() / KIL effect preserves TSF release; this path does not.
    // tsf_voice_kill() is accessible here because TSF_IMPLEMENTATION is defined in this file.
    {
        struct tsf_voice* v = h->voices;
        struct tsf_voice* vEnd = v + h->voiceNum;
        for (; v != vEnd; v++) {
            if (v->playingPreset != -1 && v->playingChannel == _trackId) tsf_voice_kill(v);
        }
    }
    tsf_channel_set_pan(h, _trackId, pan);
    tsf_channel_set_volume(h, _trackId, noteVol * trkVol);
    tsf_channel_set_bank_preset(h, _trackId, bank, preset);
    // Apply THIS instrument's ADSR override atomically, under the slot mutex we already hold, right
    // before note_on. TSF captures the envelope into the voice at note_on, so each note grabs its own
    // override even when instruments share a de-duplicated handle — the next trigger re-patches and
    // re-captures, and playing voices are immune. -1 fields keep the SF2 value.
    tsf_preset_apply_overrides(h, bank, preset, envAtk, envDec, envSus, envRel);
    tsf_channel_note_on(h, _trackId, midiNote, midiVelocity / 127.0f);
    activeNote = midiNote;
    isActive   = true;
}

void SoundfontVoice::applyPitchMod(float sampleRate, int numFrames) {
    int slot = sfSlot;
    if (slot < 0 || slot >= MAX_SOUNDFONTS) return;
    // Slot mutex held for the whole function: handle read + every pitch-wheel call below must
    // not interleave with loadSoundfont's eviction tsf_close. The function is short and the
    // lock is uncontended except during an actual SF2 load.
    std::lock_guard<std::mutex> lock(soundfonts[slot].mutex);
    tsf* h = soundfonts[slot].handle;
    if (!h) return;

    constexpr float PITCH_RANGE = 48.0f;
    if (needsPitchReset) {
        tsf_channel_set_pitchrange(h, _trackId, PITCH_RANGE);
        tsf_channel_set_pitchwheel(h, _trackId, 8192);
        needsPitchReset = false;
    }

    // If no pitch mod is active, reset pitch wheel to center and return.
    // Must NOT skip this when pitch was non-zero last block (e.g. table row returned to
    // 0 semitones after a +2 row): the wheel is persistent in TSF and must be explicitly
    // re-centered, or the previous pitch offset sticks (unlike the sampler which
    // recalculates playbackRate from modDestValues every sample).
    // detuneSemitones counts as active pitch: a static instrument detune has no slide/vibrato/mod,
    // so without this it would be wiped to center here and never reach the pitch wheel below.
    if (!pitchSliding && !vibratoActive && modDestValues[PARAM_PITCH] == 0.0f && detuneSemitones == 0.0f) {
        tsf_channel_set_pitchrange(h, _trackId, PITCH_RANGE);
        tsf_channel_set_pitchwheel(h, _trackId, 8192);
        return;
    }

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

    // detuneSemitones: static instrument detune (fractional, persists across slides)
    // pitchOffset: PSL/PBN pitch slide state (semitones, advanced above)
    // modDestValues[PARAM_PITCH]: accumulated from LFO/AHD routes targeting PITCH
    float pitchMod = detuneSemitones + pitchOffset + modDestValues[PARAM_PITCH];
    if (vibratoActive) pitchMod += sinf(vibratoPhase) * vibratoDepth;

    float clamped    = fmaxf(-PITCH_RANGE, fminf(PITCH_RANGE, pitchMod));
    int   pitchWheel = (int)(8192.0f + clamped / PITCH_RANGE * 8191.0f);
    if (pitchWheel < 0) pitchWheel = 0;
    if (pitchWheel > 16383) pitchWheel = 16383;

    tsf_channel_set_pitchrange(h, _trackId, PITCH_RANGE);
    tsf_channel_set_pitchwheel(h, _trackId, pitchWheel);
}

// ── TSF internal-access helper ──────────────────────────────────────────────
// TSF_IMPLEMENTATION is defined above, so the full tsf struct is visible here.
// jni-bridge.cpp uses the forward-declared opaque tsf*, so it can't access members directly.

bool tsf_get_preset_at(tsf* f, int index, int* bank, int* preset_number) {
    if (!f || index < 0 || index >= f->presetNum) return false;
    *bank          = f->presets[index].bank;
    *preset_number = f->presets[index].preset;
    return true;
}
