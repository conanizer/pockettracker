// TinySoundFont — single-header SF2/SF3 renderer (MIT license)
// NOTE: TSF_IMPLEMENTATION must be defined in exactly one .cpp file

// ⚠️ **THIS INCLUDE IS WHAT DECODES COMPRESSED SAMPLE DATA, AND IT MUST COME FIRST.** A compressed
// soundfont's `smpl` chunk holds Ogg Vorbis frames, and both of tsf's paths for them —
// `tsf_decode_sf3_samples` and `tsf_decode_ogg` — sit behind `#ifdef STB_VORBIS_INCLUDE_STB_VORBIS_H`,
// which is stb_vorbis's own include guard. Without it those two functions are never compiled, and
// tsf's `#else` arm has NO FORMAT CHECK: it converts the chunk in place as raw 16-bit PCM, so the file
// loads and then renders noise.
//
// ⚠️ It is needed for more than the `.sf3` the browser offers: tsf picks the decoder off each shdr's
// compression flag, not off the extension, so a file NAMED `.sf2` can carry Vorbis samples. Removing
// this include to follow whatever the extension list currently says would make such a file load and
// render noise, silently.
//
// Declarations only. stb_vorbis is compiled as its own C translation unit (see CMakeLists.txt), so
// STB_VORBIS_HEADER_ONLY avoids a second copy of the implementation, and extern "C" makes these C++
// references resolve against the C-compiled symbols. Same shape as audio-decoders.cpp.
extern "C" {
#define STB_VORBIS_HEADER_ONLY
#include "vendor/stb_vorbis/stb_vorbis.c"
}

// ─── The memory guard on a soundfont load ────────────────────────────────────────────────────────
//
// ⚠️⚠️ **WITHOUT THIS, A FONT TOO BIG FOR THE MACHINE KILLS THE APP AND NOTHING SAYS WHY.** tsf
// null-checks its allocations and unwinds to a null return, but the null never arrives: measured on
// a shipping device, bionic GRANTED a 256 GB request on a 7.36 GB machine. `malloc` never refuses,
// the request succeeds against untouched address space, and the kernel kills the process the moment
// the pages are written.
//
// ⭐ **So this does not ESTIMATE anything — it MEASURES.** There is nothing in an SF3 header that
// states its decoded size (tsf only learns it by decoding), so a prediction was never available for
// the format that needs one most. Asking the machine how much is actually free, at the moment a
// large block is about to be taken, needs no header and is exact for both formats at once.
//
// ⭐ tsf documents `TSF_MALLOC`/`TSF_REALLOC`/`TSF_FREE` as override points (tsf.h:13), so the guard
// itself needs no edit to the vendored file, and the refusal lands on tsf's own null checks.
//
// ⚠️⚠️ **BUT ONE OF THOSE UNWIND PATHS WAS WRONG, AND MAKING THE ALLOCATOR SAY NO IS WHAT EXPOSED
// IT.** `tsf_decode_ogg` freed the CALLER's buffer and returned 0; the caller freed it again — a
// double free, reachable only when a realloc fails, which under overcommit it never did. It is fixed
// in `tsf.h` and is the **THIRD** local change to vendored tsf, all three of which must be re-applied
// on any update. ⭐ The lesson to keep: **a dormant error path is not a working one, and a guard that
// makes a never-taken branch reachable inherits every bug in it.**
//
// ⚠️⚠️ **AND THE QUANTITY IT MEASURES IS `old + new`, NOT `new`** — see the block table below. A
// guard that measured only the requested size approved the step that killed the process, because a
// `realloc` holds both blocks at once. That, and tsf's growth factor (1.5x rather than 2x, in
// tsf.h), are what a font the machine can very nearly hold turns on.
//
// ⚠️ Only allocations at or above `SF_GUARD_MIN_BYTES` are checked. `/proc/meminfo` costs ~50 us to
// read and tsf makes thousands of small allocations for its preset and region tables; the ones that
// can exhaust a machine are the sample buffers, and those are enormous. A small allocation cannot be
// the one that kills us, so measuring before it is cost with no answer attached.
//
// ⚠️ **`available_memory_bytes()` returning 0 means UNKNOWN and must never refuse.** A platform that
// cannot answer has to keep loading exactly as it does today, or the guard turns into a total outage
// on whatever port reads it wrong.
#include "platform_memory.h"
#include "load_progress.h"
#include "note-queue.h"   // MAX_SOUNDFONTS — how many big blocks can be resident at once

#include <cstdlib>

namespace {

/** Set when the guard below refuses, so `loadSoundfont` can tell "too big" from "not a soundfont". */
bool g_sfMemoryGuardTripped = false;

/** Below this, an allocation cannot plausibly exhaust the machine and is not worth a syscall. */
constexpr size_t SF_GUARD_MIN_BYTES = 4u * 1024 * 1024;

/**
 * Headroom left unclaimed so that refusing a font leaves a machine that still works. This is NOT the
 * "reserve fraction" the budget policy rejects — that one shrinks a *predicted* budget and costs the
 * user files. This is an absolute floor under a *measured* one, and it exists so the app that just
 * said LOAD FAILED can still draw the message.
 */
constexpr int64_t SF_GUARD_RESERVE_BYTES = 32ll * 1024 * 1024;

/**
 * The large blocks this guard has handed out.
 *
 * ⚠️⚠️ **IT EXISTS BECAUSE A `realloc` COSTS `old + new`, NOT `new`, AND A GUARD THAT MEASURES THE
 * WRONG QUANTITY READS GREEN RIGHT UP TO THE CRASH.** Measured on a 43 MB `.sf3`: the sample buffer
 * climbs 160 → 240 → 360 → 540 → 810 MB, and while the last step is being served the 540 MB block is
 * still alive and still owned, because that is exactly what `realloc` promises. The machine is asked
 * for 1.35 GB at that instant. Comparing only the 810 MB against what is free approves the step on
 * any machine with about 850 MB — and the kill arrives during the copy, with the guard having said
 * yes. The growth factor in `tsf.h` is the other half of the same number.
 *
 * ⚠️ Sized for every font that can be RESIDENT plus the two a load in flight holds — a loaded font
 * keeps its trimmed sample buffer for its whole life, so those entries stay occupied. An overflowing
 * table simply leaves a block unrecorded, and a later realloc of it is measured the old, optimistic
 * way: the table degrades toward today's behaviour rather than toward a spurious refusal.
 */
struct SfBigBlock {
    void*  ptr  = nullptr;
    size_t size = 0;
};
SfBigBlock g_sfBig[MAX_SOUNDFONTS + 2];

/** The size this guard handed out for `ptr`, or 0 when it is not one of ours (or was too small). */
size_t sf_big_block_size(const void* ptr) {
    if (!ptr) return 0;
    for (const SfBigBlock& b : g_sfBig)
        if (b.ptr == ptr) return b.size;
    return 0;
}

void sf_forget_big_block(const void* ptr) {
    if (!ptr) return;
    for (SfBigBlock& b : g_sfBig)
        if (b.ptr == ptr) { b.ptr = nullptr; b.size = 0; return; }
}

void sf_remember_big_block(void* ptr, size_t size) {
    if (!ptr || size < SF_GUARD_MIN_BYTES) return;
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

    // ⚠️⚠️ **A SHRINK IS NEVER REFUSED, AND THAT IS NOT LENIENCY.** The one shrink tsf performs is the
    // trim at the end of `tsf_decode_sf3_samples`, whose failure arm quietly keeps the oversized
    // buffer and reports SUCCESS — so a refusal there would trip `g_sfMemoryGuardTripped` on a font
    // that had in fact loaded, and the app would say FILE TOO BIG about a soundfont sitting in a
    // playable slot. The guard is about GROWTH; a request no larger than what is already held cannot
    // be the allocation that exhausts the machine.
    if (size > held && sf_alloc_would_exhaust(size, held)) return nullptr;

    void* out = std::realloc(ptr, size);
    if (out) {
        sf_forget_big_block(ptr);
        sf_remember_big_block(out, size);
    }
    return out;
}

// ─── Progress out of, and a stop into, the SF3 decode ────────────────────────────────────────────
//
// ⚠️ **THE ALLOCATOR ABOVE IS NOT A USABLE HOOK FOR THIS, AND THAT IS WHY tsf.h IS PATCHED.** Through
// the decode of a 43 MB `.sf3`, `TSF_MALLOC`/`TSF_REALLOC` fire about a DOZEN times — the sample
// buffer only allocates when it grows — and the last of those steps covers half the run. A
// percentage built on that is a staircase, and a cancel read there lands seconds after the press.
// One report per sample header is the granularity both actually need.
//
// ⭐ **AN .sf2 REPORTS TOO, WHICH IS NOT OBVIOUS FROM THE FUNCTION'S NAME.** `tsf_decode_sf3_samples`
// runs for BOTH formats — tsf calls it whenever `tsf_load_samples` left the float buffer unbuilt,
// which is every plain `smpl` chunk — and its raw-PCM `else` arm is what converts an uncompressed
// font. So the report is one per SAMPLE HEADER for either format, and the bar and the cancel work on
// an `.sf2` as well. It rarely matters: measured, a 106 MB `.sf2` loads in 0.26 s and is over long
// before the box's delay. It matters on a slow card, and it costs nothing.
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
