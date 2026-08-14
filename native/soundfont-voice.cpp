// TinySoundFont — single-header SF2/SF3 renderer (MIT license)
// NOTE: TSF_IMPLEMENTATION must be defined in exactly one .cpp file

// ⚠️ **THIS INCLUDE IS WHAT DECODES COMPRESSED SAMPLE DATA, AND IT MUST COME FIRST.** A compressed
// soundfont's `smpl` chunk holds Ogg Vorbis frames, and both of tsf's paths for them —
// `tsf_decode_sf3_samples` and `tsf_decode_ogg` — sit behind `#ifdef STB_VORBIS_INCLUDE_STB_VORBIS_H`,
// which is stb_vorbis's own include guard. Without it those two functions are never compiled, and
// tsf's `#else` arm has NO FORMAT CHECK: it converts the chunk in place as raw 16-bit PCM, so the file
// loads and then renders noise.
//
// ⚠️ That is why this stays even though the browser no longer offers `.sf3` (see
// `ui::soundfont_extensions`, which records why it does not): tsf picks the decoder off each shdr's
// compression flag, not off the extension, so a file NAMED `.sf2` can carry Vorbis samples. Silent
// noise on such a file is a far worse outcome than the slow load the extension was withdrawn over.
//
// Declarations only. stb_vorbis is compiled as its own C translation unit (see CMakeLists.txt), so
// STB_VORBIS_HEADER_ONLY avoids a second copy of the implementation, and extern "C" makes these C++
// references resolve against the C-compiled symbols. Same shape as audio-decoders.cpp.
extern "C" {
#define STB_VORBIS_HEADER_ONLY
#include "vendor/stb_vorbis/stb_vorbis.c"
}

#define TSF_IMPLEMENTATION
#include "vendor/tsf/tsf.h"

#include "soundfont-voice.h"
#include "mods/modules/pitch-slide-module.h"  // advancePitchSlide (shared with sampler path)
#include "mods/modules/vibrato-module.h"      // advanceVibratoPhase (shared with sampler path)

// ===================================
// SOUNDFONT INFRASTRUCTURE (TinySoundFont)
// ===================================
// Supports up to MAX_SOUNDFONTS simultaneously loaded soundfont files.
// tsf is NOT thread-safe — each entry has its own mutex.
// The mutex is held by:
//   • audio thread   — armNote(), applyPitchMod(), tsf_render_float() (and, under that last one's
//                      lock, fireArmedNote())
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
    // ⚠️ A stop discards an armed note rather than letting it fire afterwards. A note fired into a
    // voice that has just been stopped is a note nothing will ever end: `isActive` is false, so the
    // render loop never reaches it again and never runs the silence detection that calls hardStop.
    hasArmedNote = false;
}

void SoundfontVoice::noteOff() {
    isReleasingOnly = true;
    // Same reason as hardStop's: an arm this block that a note-off in the SAME block supersedes (a
    // sampler note taking the track, a KIL) must not sound after the thing that ended it.
    hasArmedNote = false;

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
    // The BASE too, exactly as the sampler's setPan does (sampler-voice.h). The per-block PAN
    // modulation in processAudioBlock recomputes the channel pan as base + mod, so a PAN effect that
    // moved only TSF's channel would be undone by the next modulated block.
    params.setBase(PARAM_PAN, pan);
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

bool SoundfontVoice::armNote(int slot, int midiNote, int midiVelocity,
                             float noteVol, float trkVol, float pan,
                             int bank, int preset, int trackId,
                             int envAtk, int envDec, int envSus, int envRel) {
    // The handle must be read inside the lock (loadSoundfont eviction can tsf_close it concurrently).
    //
    // ⚠️ The lock is taken BEFORE any member is written, so a slot whose handle has gone leaves this
    // voice untouched rather than half-retargeted at a note that never sounds. The handle is only
    // READ here — the answer is a hint by the time fireArmedNote re-reads it under its own lock, and
    // that is exactly what it is for: the caller's eighty lines of setup are worth doing on it.
    {
        std::lock_guard<std::mutex> lock(soundfonts[slot].mutex);
        if (!soundfonts[slot].handle) return false;
    }

    sfSlot      = slot;
    _trackId    = trackId;
    noteVolume  = noteVol;
    trackVolume = trkVol;

    // ⚠️ activeNote is deliberately NOT moved to the new note here. Until the arm fires, the note this
    // channel is SOUNDING is still the old one, and activeNote is what hardStop() and noteOff() send
    // TSF's note_off for. Writing the new note now would aim those at a key nothing is holding.
    armed = ArmedNote{slot, midiNote, midiVelocity, bank, preset,
                      noteVol, trkVol, pan, envAtk, envDec, envSus, envRel};
    // ⚠️ A second arm in the same sub-block REPLACES the first, which is what the audible result was
    // when both fired: two notes 5.8 ms apart on one track, the second stealing the first.
    hasArmedNote = true;
    isActive     = true;   // the render pass skips an inactive voice, and it is the one that fires this
    return true;
}

void SoundfontVoice::fireArmedNote(tsf* h) {
    hasArmedNote = false;
    if (!h) return;
    const ArmedNote a = armed;

    // Hard-kill every TSF voice on this channel — no release-tail overlap. New note on the same track
    // = voice-steal, matching sampler behaviour; noteOff() / the KIL effect preserve the instrument's
    // SF REL envelope, this path does not.
    //
    // ⚠️ AND THE CUT IS DELIBERATELY UNFADED, BECAUSE TSF CANNOT FADE. Its amplitude envelope is
    // computed once per 64-sample render block and held flat across it, so `tsf_voice_endquick` — the
    // fastest release it has — steps the level down 74% at the first of those boundaries rather than
    // sliding it. Measured, it left a step a third of the peak: a quieter crack is still a crack.
    // The steal is faded in the RENDER instead, over DECLICK_SAMPLES of the already-rendered samples,
    // which is where the sampler pool has always faded its own (processAudioBlock's SF render loop).
    // ⚠️ That fade must ALREADY have been applied when this runs — the caller's ordering is the
    // contract, and cutting first would be the old bug back.
    //
    // tsf_voice_kill() is accessible here because TSF_IMPLEMENTATION is defined in this file.
    {
        struct tsf_voice* v = h->voices;
        struct tsf_voice* vEnd = v + h->voiceNum;
        for (; v != vEnd; v++) {
            if (v->playingPreset != -1 && v->playingChannel == _trackId) tsf_voice_kill(v);
        }
    }
    tsf_channel_set_pan(h, _trackId, a.pan);
    tsf_channel_set_volume(h, _trackId, a.noteVol * a.trkVol);
    tsf_channel_set_bank_preset(h, _trackId, a.bank, a.preset);
    // Apply THIS instrument's ADSR override atomically, under the slot mutex the caller holds, right
    // before note_on. TSF captures the envelope into the voice at note_on, so each note grabs its own
    // override even when instruments share a de-duplicated handle — the next trigger re-patches and
    // re-captures, and playing voices are immune. -1 fields keep the SF2 value.
    tsf_preset_apply_overrides(h, a.bank, a.preset, a.envAtk, a.envDec, a.envSus, a.envRel);
    tsf_channel_note_on(h, _trackId, a.midiNote, a.midiVelocity / 127.0f);
    activeNote = a.midiNote;
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
    // pitchOffset likewise: a finished slide (advancePitchSlide clears pitchSliding at the
    // target) must keep applying its held offset — a stopped PBN stays bent, like the sampler.
    if (!pitchSliding && !vibratoActive && pitchOffset == 0.0f &&
        modDestValues[PARAM_PITCH] == 0.0f && detuneSemitones == 0.0f) {
        tsf_channel_set_pitchrange(h, _trackId, PITCH_RANGE);
        tsf_channel_set_pitchwheel(h, _trackId, 8192);
        return;
    }

    // Advance pitch slide (PSL / PBN) + vibrato LFO (PVB / PVX) — the shared per-block
    // state machines, identical to the sampler path (mods/modules).
    advancePitchSlide(*this, numFrames);
    advanceVibratoPhase(*this, numFrames, sampleRate);

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
