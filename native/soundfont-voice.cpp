// TinySoundFont — single-header SF2/SF3 renderer (MIT license)
// NOTE: TSF_IMPLEMENTATION must be defined in exactly one .cpp file

// ⚠️ THIS INCLUDE IS WHAT DECODES COMPRESSED SAMPLE DATA, AND IT MUST COME FIRST. tsf's Ogg paths
// (`tsf_decode_sf3_samples`, `tsf_decode_ogg`) sit behind stb_vorbis's include guard; without it
// tsf's `#else` arm converts the chunk as raw 16-bit PCM — the file loads and renders noise. tsf
// picks the decoder per shdr flag, not by extension, so a `.sf2` can need it too.
//
// Declarations only: stb_vorbis is its own C translation unit (CMakeLists.txt); extern "C"
// resolves against it, as in audio-decoders.cpp.
extern "C" {
#define STB_VORBIS_HEADER_ONLY
#include "vendor/stb_vorbis/stb_vorbis.c"
}

// ─── The memory guard on a soundfont load ────────────────────────────────────────────────────────
//
// ⚠️⚠️ WITHOUT THIS, A FONT TOO BIG FOR THE MACHINE KILLS THE APP AND NOTHING SAYS WHY: bionic's
// malloc never refuses (platform_memory.h), so tsf's null checks never fire and the kernel kills
// the process when the pages are written.
//
// ⭐ It MEASURES rather than estimates: an SF3 header does not state its decoded size, but asking
// the machine what is free when a large block is taken works for both formats. tsf documents
// TSF_MALLOC / TSF_REALLOC / TSF_FREE as override points, so a refusal lands on tsf's own null checks.
//
// ⚠️⚠️ Making the allocator say no makes tsf's never-taken error paths reachable — `tsf_decode_ogg`'s
// double free on a failed realloc is fixed in `tsf.h`, one of the THREE local changes to vendored
// tsf that must be re-applied on any update.
//
// ⚠️⚠️ THE QUANTITY MEASURED IS `old + new`, NOT `new` — a realloc holds both (see the block table).
// tsf's growth factor (1.5×, in tsf.h) is the other half of that number.
//
// ⚠️ Only allocations ≥ SF_GUARD_MIN_BYTES are checked: reading free memory costs a syscall, and
// only the sample buffers are large enough to matter.
//
// ⚠️ `available_memory_bytes()` returning 0 means UNKNOWN and must never refuse.
#include "common/platform_memory.h"
#include "common/load_progress.h"
#include "note-queue.h"   // MAX_SOUNDFONTS — how many big blocks can be resident at once

#include <cstdlib>
#include <mutex>

namespace {

/**
 * Set when the guard below refuses, so `loadSoundfont` can tell "too big" from "not a soundfont".
 *
 * ⚠️ **PER-THREAD.** A preset now loads on a worker while the main thread can be loading a project,
 * and the flag is read by whichever load just came back — sharing it would let one load's refusal be
 * reported as the other's, on a font that loaded fine.
 */
thread_local bool g_sfMemoryGuardTripped = false;

/** Below this, an allocation cannot plausibly exhaust the machine and is not worth a syscall. */
constexpr size_t SF_GUARD_MIN_BYTES = 4u * 1024 * 1024;

/**
 * Headroom left unclaimed so the app that just said LOAD FAILED can still draw it. An absolute
 * floor under a MEASURED figure — not a margin shaved off a predicted budget, which costs files.
 */
constexpr int64_t SF_GUARD_RESERVE_BYTES = 32ll * 1024 * 1024;

/**
 * The large blocks this guard has handed out.
 *
 * ⚠️⚠️ IT EXISTS BECAUSE A `realloc` COSTS `old + new`: while the buffer grows (e.g. 540 → 810 MB)
 * the old block is still alive, so a guard comparing only the new size approves the step that
 * kills the process during the copy.
 *
 * ⚠️ Sized for every RESIDENT font plus the two a load in flight holds. An overflow leaves a block
 * unrecorded and measured optimistically later — degrading toward no guard, never a false refusal.
 */
struct SfBigBlock {
    void*  ptr  = nullptr;
    size_t size = 0;
};
SfBigBlock g_sfBig[MAX_SOUNDFONTS + 2];

/**
 * ⚠️⚠️ THE TABLE IS SHARED, NOT PER-THREAD, so every touch takes this: a block allocated by the
 * worker loading a preset is FREED by the main thread when that slot is evicted. Taken only during
 * a load or a free.
 */
std::mutex g_sfBigMutex;

/** The size this guard handed out for `ptr`, or 0 when it is not one of ours (or was too small). */
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
    // Full: the block goes unrecorded, and a later realloc of it is measured without its old half.
}

/**
 * True when taking `size` right now — while still holding `alsoHeld` bytes across the call — would
 * leave the machine with nothing.
 */
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

/**
 * ⚠️ On refusal the ORIGINAL BLOCK IS LEFT ALIVE and untouched, because that is what `realloc`
 * promises on failure and what tsf's growth sites rely on: both do `oldres = res; res =
 * TSF_REALLOC(res, ...); if (!res) { TSF_FREE(oldres); ... }`. Freeing here as well would double-free.
 */
void* sf_guarded_realloc(void* ptr, size_t size) {
    const size_t held = sf_big_block_size(ptr);

    // ⚠️⚠️ A SHRINK IS NEVER REFUSED: tsf's one shrink (the trim ending `tsf_decode_sf3_samples`)
    // keeps the oversized buffer on failure and reports success, so a refusal there would report
    // FILE TOO BIG for a font that loaded. Only growth can exhaust the machine.
    if (size > held && sf_alloc_would_exhaust(size, held)) return nullptr;

    // The book-keeping moves BEFORE the realloc: after it `ptr` may already be freed, and a freed
    // pointer is not something to look up, even as a key. A refused realloc puts the entry back.
    sf_forget_big_block(ptr);
    void* out = std::realloc(ptr, size);
    if (out) sf_remember_big_block(out, size);
    else     sf_remember_big_block(ptr, held);
    return out;
}

// ─── Progress out of, and a stop into, the SF3 decode ────────────────────────────────────────────
//
// ⚠️ The allocator above is too coarse for this (a dozen calls over a whole decode), so tsf.h is
// patched to report once per sample header. ⭐ `tsf_decode_sf3_samples` also converts plain `.sf2`
// PCM, so the bar and the cancel work for both formats.
int sf_load_progress(int index, int count) {
    return pt::load_tick(count > 0 ? static_cast<float>(index) / static_cast<float>(count) : -1.0f)
               ? 1 : 0;
}

}  // namespace

void sf_memory_guard_reset()   { g_sfMemoryGuardTripped = false; }
bool sf_memory_guard_tripped() { return g_sfMemoryGuardTripped; }

#define TSF_MALLOC(size)       sf_guarded_malloc(size)
#define TSF_REALLOC(ptr, size) sf_guarded_realloc(ptr, size)
// ⚠️ FREE goes through the guard too — not to check anything, but so the block table above stays
// true. A free that did not un-record its block would leave a stale pointer that a later allocation
// can land on, and the next realloc of that address would be charged a size it does not have.
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

// ===================================
// SOUNDFONT INFRASTRUCTURE (TinySoundFont)
// ===================================
// Supports up to MAX_SOUNDFONTS simultaneously loaded soundfont files.
// tsf is NOT thread-safe, and only the audio thread calls into a loaded handle — see SoundfontEntry
// for how the UI frees one without a lock.

SoundfontEntry soundfonts[MAX_SOUNDFONTS];

/**
 * Above this release time TSF's own envelope is smooth enough to hear as a fade; below it the note
 * ends on OUR ramp instead (see `noteOff`).
 */
static constexpr float SF_RELEASE_RAMP_MAX_SECS = 0.1f;

/**
 * The longest release, in seconds, among the TSF voices sounding on `channel` — the time the note
 * would take to end if TSF were left to do it. 0 when nothing is sounding there.
 *
 * ⚠️ Read off the VOICE and not off its region: `tsf_preset_apply_overrides` rewrites a region's
 * envelope, so the region can already be carrying the next instrument's SF REL while this note is
 * still playing the one it captured at note-on.
 */
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

// ── SoundfontVoice method implementations ──────────────────────────────────

void SoundfontVoice::hardStop() {
    pendingTsfOffAt = -1;   // every voice on the channel is killed below
    int slot = sfSlot;
    if (slot >= 0 && slot < MAX_SOUNDFONTS) {
        tsf* h = soundfonts[slot].handle.load();
        if (h) {
            if (activeNote >= 0) tsf_channel_note_off(h, _trackId, activeNote);
            // ⚠️⚠️ AND THEN KILL WHAT THAT LEFT ALIVE, OR THE NEXT TAKE PLAYS IT. A note-off only
            // starts a release, and with `isActive` false below it is never rendered to its end —
            // TSF's voices sit frozen, and the steal path renders them again at the next trigger as
            // a step out of silence. Every call site has the channel at zero already or wants it gone.
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
    // Whether the ramp ran to its end or a hard stop overtook it, the counters go with the note —
    // a leftover total would scale the next note's first block.
    stopFadeRemaining = 0;
    stopFadeTotal     = 0;
    // ⚠️ A stop discards an armed note rather than letting it fire afterwards. A note fired into a
    // voice that has just been stopped is a note nothing will ever end: `isActive` is false, so the
    // render loop never reaches it again and never runs the silence detection that calls hardStop.
    hasArmedNote = false;
}

void SoundfontVoice::noteOff() { noteOffAt(0); }

void SoundfontVoice::noteOffAt(int atFrame) {
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
    //   ⚠️ …unless that release is SHORT, in which case the note ends on our own ramp — see below.
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
        // Set when the note is ending on OUR ramp, which keeps `activeNote` — the ramp's end calls
        // hardStop(), and that is what sends TSF the note-off.
        bool ownRamp = false;
        int slot = sfSlot;
        if (slot >= 0 && slot < MAX_SOUNDFONTS) {
            tsf* h = soundfonts[slot].handle.load();
            if (h && activeNote >= 0) {
                // ⚠️⚠️ A SHORT RELEASE IS RUN AS OUR OWN RAMP, BECAUSE TSF CANNOT FADE ONE: its
                // envelope is held flat per 64-sample chunk, so a release of a few ms lands as one
                // step — a crack on every key release and KIL. Below the threshold TSF holds sustain
                // while our ramp takes the samples down over the release time (never faster than the
                // declick fade). Above it, TSF's own release is left alone — a pad keeps its tail.
                const float rel = longest_release(h, _trackId);
                if (rel < SF_RELEASE_RAMP_MAX_SECS) {
                    const int want = static_cast<int>(rel * h->outSampleRate);
                    startStopFade(want > KILL_FADE_SAMPLES ? want : KILL_FADE_SAMPLES, atFrame);
                    ownRamp = true;
                } else if (atFrame > 0) {
                    // Sent by the render pass at that frame, between the two halves of the render.
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
    // Stored only: the note gain is applied to the rendered buffer each block, never to the channel.
    noteVolume = v;
}

void SoundfontVoice::setPan(float pan) {
    // The BASE too, exactly as the sampler's setPan does (sampler-voice.h). The per-block PAN
    // modulation in processAudioBlock recomputes the channel pan as base + mod, so a PAN effect that
    // moved only TSF's channel would be undone by the next modulated block.
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
    // ⚠️ Checked BEFORE any member is written, so a slot whose handle has gone leaves this voice
    // untouched rather than half-retargeted at a note that never sounds. The generation is read
    // first: a free between the two reads then makes the voice stale, never the other way round.
    const uint32_t gen = soundfonts[slot].gen.load();
    if (!soundfonts[slot].handle.load()) return false;

    sfSlot      = slot;
    sfGen       = gen;
    _trackId    = trackId;
    noteVolume  = noteVol;

    // ⚠️ activeNote is deliberately NOT moved to the new note here. Until the arm fires, the note this
    // channel is SOUNDING is still the old one, and activeNote is what hardStop() and noteOff() send
    // TSF's note_off for. Writing the new note now would aim those at a key nothing is holding.
    armed = ArmedNote{slot, midiNote, midiVelocity, bank, preset,
                      noteVol, pan, envAtk, envDec, envSus, envRel};
    // ⚠️ A second arm in the same sub-block REPLACES the first — two notes 5.8 ms apart on one
    // track, the second stealing the first.
    hasArmedNote = true;
    isActive     = true;   // the render pass skips an inactive voice, and it is the one that fires this
    // Clear any stale transport-stop ramp, exactly as Voice::trigger() clears a stale fade-out: a new
    // note starts at full level or it starts fading the moment it is heard.
    stopFadeRemaining = 0;
    stopFadeTotal     = 0;
    return true;
}

void SoundfontVoice::fireArmedNote(tsf* h) {
    hasArmedNote = false;
    pendingTsfOffAt = -1;   // the note it was for is killed below
    if (!h) return;
    const ArmedNote a = armed;

    // Hard-kill every TSF voice on this channel: a new note on the track is a steal, as on the
    // sampler; noteOff() and KIL keep the SF REL envelope, this does not.
    // ⚠️ The cut is UNFADED because TSF cannot fade (its envelope is flat per 64-sample block). The
    // steal is faded in the RENDER instead, over DECLICK_SAMPLES of the rendered samples — and that
    // fade must ALREADY have been applied when this runs; the caller's ordering is the contract.
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
    // ⚠️ THE CHANNEL VOLUME IS LEFT ALONE, at TSF's own unity. Both gains that would go through it —
    // the note's (volGain) and the track fader — are ramps applied to the rendered samples instead,
    // because a channel volume can only change at a render boundary.
    tsf_channel_set_bank_preset(h, _trackId, a.bank, a.preset);
    // This instrument's ADSR override, right before note_on: TSF captures the envelope into the
    // voice there, so each note keeps its own even on a shared handle. -1 fields keep the SF2 value.
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

    // Re-centre the wheel only when NO pitch source is active: the wheel persists in TSF, so a table
    // row back at 0 semitones must re-centre it explicitly. A static detune, a held slide offset and
    // FIN's fine tune (params.base[PARAM_PITCH]) all count as active — none has a slide or mod behind
    // it, so an early return would wipe it.
    if (!pitchSliding && !vibratoActive && pitchOffset == 0.0f &&
        modDestValues[PARAM_PITCH] == 0.0f && detuneSemitones == 0.0f &&
        params.base[PARAM_PITCH] == 0.0f) {
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
    // params.base[PARAM_PITCH]: FIN's fine tune — the same slot's other half, cleared by every trigger
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

// ── TSF internal-access helper ──────────────────────────────────────────────
// TSF_IMPLEMENTATION is defined above, so the full tsf struct is visible here; elsewhere tsf* is opaque.

bool tsf_get_preset_at(tsf* f, int index, int* bank, int* preset_number) {
    if (!f || index < 0 || index >= f->presetNum) return false;
    *bank          = f->presets[index].bank;
    *preset_number = f->presets[index].preset;
    return true;
}
