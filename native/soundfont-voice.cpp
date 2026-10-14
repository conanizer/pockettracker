// TinySoundFont (MIT); TSF_IMPLEMENTATION in this file only.

// ⚠️ Must come first: without stb_vorbis's guard tsf decodes Ogg samples as raw PCM — the file loads
// and plays noise. The flag is per sample, so a `.sf2` can need it too. Declarations only; stb_vorbis
// is its own C unit.
extern "C" {
#define STB_VORBIS_HEADER_ONLY
#include "vendor/stb_vorbis/stb_vorbis.c"
}

// ─── The memory guard on a soundfont load ────────────────────────────────────────────────────────
// ⚠️ Without it a font too big for the machine kills the app silently: Android's malloc never
// refuses, so the kernel kills the process when the pages are written. It measures free memory when
// a large block is taken (TSF_MALLOC / REALLOC / FREE), so a refusal lands on tsf's own null checks.
// ⚠️ That makes tsf's untaken error paths reachable — its double free in `tsf_decode_ogg` is one of
// the THREE local changes to vendored tsf to re-apply on any update.
// ⚠️ A realloc costs `old + new`, not `new`. Free memory reading 0 means unknown and never refuses.
#include "common/platform_memory.h"
#include "common/load_progress.h"
#include "note-queue.h"   // MAX_SOUNDFONTS — how many big blocks can be resident at once

#include <cstdlib>
#include <mutex>

namespace {

// Set when the guard refuses, so `loadSoundfont` can tell "too big" from "not a soundfont".
// ⚠️ Per thread: the worker and the main thread can load at once.
thread_local bool g_sfMemoryGuardTripped = false;

// Smaller allocations are not worth a syscall.
constexpr size_t SF_GUARD_MIN_BYTES = 4u * 1024 * 1024;

// Left free so the app can still draw LOAD FAILED.
constexpr int64_t SF_GUARD_RESERVE_BYTES = 32ll * 1024 * 1024;

// The large blocks handed out — a growing realloc holds the old one alive during the copy.
// Sized for every resident font plus a load's two; an overflow only weakens the guard.
struct SfBigBlock {
    void*  ptr  = nullptr;
    size_t size = 0;
};
SfBigBlock g_sfBig[MAX_SOUNDFONTS + 2];

// ⚠️ Shared: the worker allocates a block the main thread later frees.
std::mutex g_sfBigMutex;

// The recorded size of `ptr`; 0 when not ours.
size_t sf_big_block_size(const void* ptr) {
    if (!ptr) return 0;
    std::lock_guard<std::mutex> lock(g_sfBigMutex);
    for (const SfBigBlock& b : g_sfBig)
        if (b.ptr == ptr) return b.size;
    return 0;
}

void sf_forget_big_block(const void* ptr) {
    if (!ptr) return;
    std::lock_guard<std::mutex> lock(g_sfBigMutex);
    for (SfBigBlock& b : g_sfBig)
        if (b.ptr == ptr) { b.ptr = nullptr; b.size = 0; return; }
}

void sf_remember_big_block(void* ptr, size_t size) {
    if (!ptr || size < SF_GUARD_MIN_BYTES) return;
    std::lock_guard<std::mutex> lock(g_sfBigMutex);
    for (SfBigBlock& b : g_sfBig)
        if (b.ptr == nullptr) { b.ptr = ptr; b.size = size; return; }
    // Full: the block goes unrecorded.
}

// True when taking `size` while still holding `alsoHeld` would leave the machine with nothing.
bool sf_alloc_would_exhaust(size_t size, size_t alsoHeld) {
    if (size < SF_GUARD_MIN_BYTES) return false;
    const int64_t available = pt::available_memory_bytes();
    if (available <= 0) return false;              // unknown is not "empty"
    const int64_t needed = static_cast<int64_t>(size) + static_cast<int64_t>(alsoHeld);
    const bool exhausted = needed > available - SF_GUARD_RESERVE_BYTES;
    if (exhausted) g_sfMemoryGuardTripped = true;
    return exhausted;
}

void* sf_guarded_malloc(size_t size) {
    if (sf_alloc_would_exhaust(size, 0)) return nullptr;
    void* out = std::malloc(size);
    sf_remember_big_block(out, size);
    return out;
}

void sf_guarded_free(void* ptr) {
    sf_forget_big_block(ptr);
    std::free(ptr);
}

// ⚠️ On refusal the original block stays alive, as realloc promises — tsf frees it itself.
void* sf_guarded_realloc(void* ptr, size_t size) {
    const size_t held = sf_big_block_size(ptr);

    // ⚠️ A shrink is never refused: tsf would report FILE TOO BIG for a font that loaded.
    if (size > held && sf_alloc_would_exhaust(size, held)) return nullptr;

    // Un-record before the realloc frees `ptr`; a refusal puts the entry back.
    sf_forget_big_block(ptr);
    void* out = std::realloc(ptr, size);
    if (out) sf_remember_big_block(out, size);
    else     sf_remember_big_block(ptr, held);
    return out;
}

// ─── Progress out of, and a stop into, the decode ───────────────────────────────────────────────
// tsf.h is patched to report once per sample header; it covers `.sf2` PCM as well as SF3.
int sf_load_progress(int index, int count) {
    return pt::load_tick(count > 0 ? static_cast<float>(index) / static_cast<float>(count) : -1.0f)
               ? 1 : 0;
}

}  // namespace

void sf_memory_guard_reset()   { g_sfMemoryGuardTripped = false; }
bool sf_memory_guard_tripped() { return g_sfMemoryGuardTripped; }

#define TSF_MALLOC(size)       sf_guarded_malloc(size)
#define TSF_REALLOC(ptr, size) sf_guarded_realloc(ptr, size)
// FREE goes through the guard too, so the block table holds no stale pointer.
#define TSF_FREE(ptr)          sf_guarded_free(ptr)
#define TSF_PROGRESS(i, n)     sf_load_progress((i), (n))

#define TSF_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"   // vendored, not ours to fix
#endif
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wnull-pointer-subtraction"
#endif
#include "vendor/tsf/tsf.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "soundfont-voice.h"
#include "mods/modules/pitch-slide-module.h"  // advancePitchSlide (shared with sampler path)
#include "mods/modules/vibrato-module.h"      // advanceVibratoPhase (shared with sampler path)

// tsf is not thread-safe: only the audio thread calls into a loaded handle (see SoundfontEntry).

SoundfontEntry soundfonts[MAX_SOUNDFONTS];

// Below this release time the note ends on our ramp, not TSF's envelope (see `noteOff`).
static constexpr float SF_RELEASE_RAMP_MAX_SECS = 0.1f;

// The longest release (s) among TSF's voices on `channel`; 0 when silent. ⚠️ Read off the voice, not
// its region — the region may already carry the next instrument's SF REL.
static float longest_release(tsf* h, int channel) {
    float longest = 0.0f;
    struct tsf_voice* v    = h->voices;
    struct tsf_voice* vEnd = v + h->voiceNum;
    for (; v != vEnd; v++) {
        if (v->playingPreset == -1 || v->playingChannel != channel) continue;
        if (v->ampenv.parameters.release > longest) longest = v->ampenv.parameters.release;
    }
    return longest;
}


void SoundfontVoice::hardStop() {
    pendingTsfOffAt = -1;   // every voice on the channel is killed below
    int slot = sfSlot;
    if (slot >= 0 && slot < MAX_SOUNDFONTS) {
        tsf* h = soundfonts[slot].handle.load();
        if (h) {
            if (activeNote >= 0) tsf_channel_note_off(h, _trackId, activeNote);
            // ⚠️ Then kill what that left releasing: an inactive voice is never rendered, so its TSF
            // voices would sit frozen and play out at the next trigger.
            struct tsf_voice* v    = h->voices;
            struct tsf_voice* vEnd = v + h->voiceNum;
            for (; v != vEnd; v++) {
                if (v->playingPreset != -1 && v->playingChannel == _trackId) tsf_voice_kill(v);
            }
        }
    }
    activeNote      = -1;
    isActive        = false;
    isReleasingOnly = false;
    // The ramp's counters go with the note, or they scale the next note's first block.
    stopFadeRemaining = 0;
    stopFadeTotal     = 0;
    // ⚠️ A stop discards an armed note: fired into a stopped voice, nothing would ever end it.
    hasArmedNote = false;
}

void SoundfontVoice::noteOff() { noteOffAt(0); }

void SoundfontVoice::noteOffAt(int atFrame) {
    isReleasingOnly = true;
    // As in hardStop: an arm this block must not sound after the note-off that superseded it.
    hasArmedNote = false;

    // With an ADSR/TRIG on VOL, TSF holds sustain while our envelope fades; hardStop() sends the
    // note-off when they finish. Otherwise TSF's own release (SF REL) plays out — unless it is short.
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
        // activeNote stays for hardStop()'s deferred note-off.
        for (int m = 0; m < 4; m++) {
            VoiceModSlot& mod = voiceMods[m];
            if (mod.dest == 1 && (mod.type == 2 || mod.type == 5)
                    && mod.stage >= 1 && mod.stage <= 3) {
                mod.stage        = 4;
                mod.stageCounter = 0;
            }
        }
    } else {
        // Ending on our ramp: its end calls hardStop(), which sends the note-off.
        bool ownRamp = false;
        int slot = sfSlot;
        if (slot >= 0 && slot < MAX_SOUNDFONTS) {
            tsf* h = soundfonts[slot].handle.load();
            if (h && activeNote >= 0) {
                // ⚠️ TSF steps its envelope per 64 samples, so a release of a few ms lands as one
                // crack. A short one runs as our ramp (never faster than the declick) while TSF
                // holds sustain; a long one is TSF's own.
                const float rel = longest_release(h, _trackId);
                if (rel < SF_RELEASE_RAMP_MAX_SECS) {
                    const int want = static_cast<int>(rel * h->outSampleRate);
                    startStopFade(want > KILL_FADE_SAMPLES ? want : KILL_FADE_SAMPLES, atFrame);
                    ownRamp = true;
                } else if (atFrame > 0) {
                    // Sent by the render pass at that frame.
                    pendingTsfOffAt   = atFrame;
                    pendingTsfOffNote = activeNote;
                } else {
                    tsf_channel_note_off(h, _trackId, activeNote);
                }
            }
        }
        if (!ownRamp) activeNote = -1;
    }
}

void SoundfontVoice::setVolume(float v) {
    // Applied to the rendered buffer each block, never to the channel.
    noteVolume = v;
}

void SoundfontVoice::setPan(float pan) {
    // The base too, as the sampler's setPan does: each block recomputes the pan as base + mod.
    params.setBase(PARAM_PAN, pan);
    panNow = pan;
    panGlideLeft = 0;
    int slot = sfSlot;
    if (slot >= 0 && slot < MAX_SOUNDFONTS) {
        tsf* h = soundfonts[slot].handle.load();
        if (h) tsf_channel_set_pan(h, _trackId, pan);
    }
}

void SoundfontVoice::setMidiNote(int midiNote) {
    int slot = sfSlot;
    if (slot < 0 || slot >= MAX_SOUNDFONTS) return;
    tsf* h = soundfonts[slot].handle.load();
    if (!h) return;
    if (activeNote >= 0) tsf_channel_note_off(h, _trackId, activeNote);
    tsf_channel_note_on(h, _trackId, midiNote, noteVolume);
    activeNote = midiNote;
}

bool SoundfontVoice::armNote(int slot, int midiNote, int midiVelocity,
                             float noteVol, float pan,
                             int bank, int preset, int trackId,
                             int envAtk, int envDec, int envSus, int envRel) {
    // ⚠️ Checked before any member is written. The generation is read first, so a free between the
    // two reads makes the voice stale, never the reverse.
    const uint32_t gen = soundfonts[slot].gen.load();
    if (!soundfonts[slot].handle.load()) return false;

    sfSlot      = slot;
    sfGen       = gen;
    _trackId    = trackId;
    noteVolume  = noteVol;

    // ⚠️ activeNote stays the old note until the arm fires — it is what hardStop()/noteOff() release.
    armed = ArmedNote{slot, midiNote, midiVelocity, bank, preset,
                      noteVol, pan, envAtk, envDec, envSus, envRel};
    // A second arm in the same sub-block replaces the first (the second note steals it).
    hasArmedNote = true;
    isActive     = true;   // the render pass skips an inactive voice, and it is the one that fires this
    // A stale stop ramp would fade the new note as it starts.
    stopFadeRemaining = 0;
    stopFadeTotal     = 0;
    return true;
}

void SoundfontVoice::fireArmedNote(tsf* h) {
    hasArmedNote = false;
    pendingTsfOffAt = -1;   // the note it was for is killed below
    if (!h) return;
    const ArmedNote a = armed;

    // A new note steals the channel: every TSF voice on it is cut. ⚠️ Unfaded — the render fades the
    // steal over DECLICK_SAMPLES, and the caller must have done that before this runs.
    {
        struct tsf_voice* v = h->voices;
        struct tsf_voice* vEnd = v + h->voiceNum;
        for (; v != vEnd; v++) {
            if (v->playingPreset != -1 && v->playingChannel == _trackId) tsf_voice_kill(v);
        }
    }
    tsf_channel_set_pan(h, _trackId, a.pan);
    panNow = a.pan;
    panGlideLeft = 0;
    // The channel volume stays at unity: note gain and fader are ramped on the rendered samples.
    tsf_channel_set_bank_preset(h, _trackId, a.bank, a.preset);
    // The instrument's ADSR override, just before note_on, which captures it per voice. -1 = the file's.
    tsf_preset_apply_overrides(h, a.bank, a.preset, a.envAtk, a.envDec, a.envSus, a.envRel);
    tsf_channel_note_on(h, _trackId, a.midiNote, a.midiVelocity / 127.0f);
    activeNote = a.midiNote;
    isActive   = true;
}

void SoundfontVoice::applyPitchMod(float sampleRate, int numFrames) {
    int slot = sfSlot;
    if (slot < 0 || slot >= MAX_SOUNDFONTS) return;
    tsf* h = soundfonts[slot].handle.load();
    if (!h) return;

    constexpr float PITCH_RANGE = 48.0f;
    if (needsPitchReset) {
        tsf_channel_set_pitchrange(h, _trackId, PITCH_RANGE);
        tsf_channel_set_pitchwheel(h, _trackId, 8192);
        needsPitchReset = false;
    }

    // The wheel persists in TSF, so with no pitch source at all it is re-centred explicitly. Detune, a
    // held slide offset and FIN (params.base[PARAM_PITCH]) count as sources.
    if (!pitchSliding && !vibratoActive && pitchOffset == 0.0f &&
        modDestValues[PARAM_PITCH] == 0.0f && detuneSemitones == 0.0f &&
        params.base[PARAM_PITCH] == 0.0f) {
        tsf_channel_set_pitchrange(h, _trackId, PITCH_RANGE);
        tsf_channel_set_pitchwheel(h, _trackId, 8192);
        return;
    }

    // PSL/PBN and PVB/PVX — the same per-block state machines as the sampler's.
    advancePitchSlide(*this, numFrames);
    advanceVibratoPhase(*this, numFrames, sampleRate);

    // Detune + slide + mod routes + FIN (cleared by every trigger), in semitones.
    float pitchMod = detuneSemitones + pitchOffset + modDestValues[PARAM_PITCH]
                   + params.base[PARAM_PITCH];
    if (vibratoActive) pitchMod += sinf(vibratoPhase) * vibratoDepth;

    float clamped    = fmaxf(-PITCH_RANGE, fminf(PITCH_RANGE, pitchMod));
    int   pitchWheel = (int)(8192.0f + clamped / PITCH_RANGE * 8191.0f);
    if (pitchWheel < 0) pitchWheel = 0;
    if (pitchWheel > 16383) pitchWheel = 16383;

    tsf_channel_set_pitchrange(h, _trackId, PITCH_RANGE);
    tsf_channel_set_pitchwheel(h, _trackId, pitchWheel);
}

// Needs the full tsf struct, visible only in this file.

bool tsf_get_preset_at(tsf* f, int index, int* bank, int* preset_number) {
    if (!f || index < 0 || index >= f->presetNum) return false;
    *bank          = f->presets[index].bank;
    *preset_number = f->presets[index].preset;
    return true;
}
