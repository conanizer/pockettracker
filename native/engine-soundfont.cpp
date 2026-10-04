// The SoundFont bank: parsing a preset into a tsf handle (in the background or not), the slot cache
// with its de-dup and LRU, the per-file preset index, and the per-instrument ADSR override.
#include "audio-engine.h"
#include "vendor/tsf/tsf.h"    // TSF API declarations only — TSF_IMPLEMENTATION lives in soundfont-voice.cpp
#include "byte_source.h"       // pt_fopen — the soundfont loader opens through it
#include "platform_memory.h"   // load_budget_bytes — refuses a load the device cannot hold
#include "load_progress.h"     // load_tick / load_cancelled — a slow load reports itself and can be stopped
#include <cstdio>
#include <cstdint>
#include <climits>   // INT_MAX — tsf_load_memory takes an int size
#include <vector>

namespace {
// The `tsf_stream` pair loadSoundfont hands to tsf_load — the same one tsf builds inside
// tsf_load_filename, which is unused here because the open must be pt_fopen's. `read` returns the
// byte count, `skip` returns 1 on success and 0 on error, per tsf.h.
//
// Declared `void*` rather than `FILE*`: tsf's own pair takes FILE* and is cast into the struct at
// the call site, which is a call through a mismatched function-pointer type. Taking void* and
// casting inside costs nothing and is the same shape the dr_libs callbacks use.
int sfStreamRead(void* f, void* ptr, unsigned int size) {
    return (int)std::fread(ptr, 1, size, (FILE*)f);
}
int sfStreamSkip(void* f, unsigned int count) {
    return std::fseek((FILE*)f, (long)count, SEEK_CUR) == 0;
}
}  // namespace

void AudioEngine::setSoundfontEnvelopeOverride(int instrumentId, int atk, int dec, int sus, int rel) {
    if (instrumentId < 0 || instrumentId >= 256) return;
    SfEnvOverride& o = sfEnvOverrides.edit(instrumentId);
    o.atk = atk; o.dec = dec; o.sus = sus; o.rel = rel;
    sfEnvOverrides.publish(instrumentId);
}

// ===================================
// SOUNDFONT BANK
// ===================================

void AudioEngine::freeSoundfontSlot(int slot) {
    if (slot < 0 || slot >= MAX_SOUNDFONTS) return;
    // The voices on this slot are detached by the audio thread's next block (the generation), and
    // the handle is closed only once the block that may have loaded it has ended.
    tsf* h = soundfonts[slot].handle.exchange(nullptr);
    soundfonts[slot].gen.fetch_add(1);
    if (h) {
        waitForAudioBlockBoundary();
        tsf_close(h);
    }
    soundfonts[slot].instrumentId = -1;
    soundfonts[slot].filePath.clear();
    soundfonts[slot].bank   = -1;
    soundfonts[slot].preset = -1;
}

// ⚠️ Everything tsf would otherwise allocate on the AUDIO thread, done here: it grows its voice array
// on a note that finds none free, and its channel array on the first call naming a new channel. So
// both are sized now — a voice pool no preset here comes near, and a channel per SoundFont voice.
// With a fixed pool tsf takes the voice furthest into its release instead of growing.
static void readyForAudio(tsf* h, int sampleRate) {
    tsf_set_output(h, TSF_STEREO_INTERLEAVED, sampleRate, 0.0f);
    tsf_set_max_voices(h, 128);
    tsf_channel_set_pan(h, SF_VOICE_COUNT - 1, 0.5f);   // creates channels 0..8, each at its default
}

/**
 * Turn a file plus a bank/preset into a parsed tsf handle. No slot is touched and no member is
 * written; the failure comes back through `failure` — this runs on the background worker as often
 * as on the calling thread.
 */
tsf* AudioEngine::parseSoundfont(const char* path, int bank, int preset, LoadFailure* failure) {
    *failure = LoadFailure::NONE;

    // ⭐ THE PREFERRED PATH: cut the one preset out of the file and parse only that — a complete,
    // ordinary SoundFont, typically under 1 % of the bank, so tsf never sees the rest. False means
    // "cannot be cut apart", never "broken", and falls through to the whole-bank parse.
    {
        std::vector<uint8_t> trimmed;
        sf_memory_guard_reset();
        if (pt::sf_build_trimmed_font(path, bank, preset, trimmed) &&
            trimmed.size() <= static_cast<size_t>(INT_MAX)) {
            tsf* small = tsf_load_memory(trimmed.data(), static_cast<int>(trimmed.size()));
            if (small) {
                readyForAudio(small, getSampleRate());
                return small;
            }
            // ⚠️ Asked before anything else, for the reason the whole-bank path below states: a cancel
            // and a parse failure unwind through the identical null, and falling through here would
            // answer a cancelled load by starting the very load the user just stopped.
            if (pt::load_cancelled()) {
                LOGD("🎹 Soundfont load cancelled: %s", path);
                *failure = LoadFailure::CANCELLED;
                return nullptr;
            }
            // A trimmed font that will not parse is a bug here, not a property of the file — but the
            // user's sound is worth more than the diagnosis, so fall through and load it whole.
            LOGE("❌ Trimmed soundfont failed to parse, loading whole bank: %s", path);
        }
    }

    // Parse into a single master handle; all tracks share it via MIDI channels (per-track clones
    // would cost 8× the file in RAM). `tsf_load` over pt_fopen's `FILE*` — sequential reads and
    // forward skips, so the file streams and peak RAM is the parsed soundfont alone.
    FILE* sf = pt_fopen(path, "rb");
    if (!sf) {
        LOGE("❌ Cannot open soundfont: %s", path);
        *failure = LoadFailure::PARSE;
        return nullptr;
    }
    tsf_stream sfStream = { sf, &sfStreamRead, &sfStreamSkip };
    // ⭐ The guard that makes a too-large font a MESSAGE instead of a kill (soundfont-voice.cpp).
    // Reset first: the flag separates "too big for this device" from "not a soundfont".
    sf_memory_guard_reset();
    tsf* loaded = tsf_load(&sfStream);
    std::fclose(sf);
    if (!loaded) {
        // ⚠️ Asked FIRST, and before the guard: a cancel is not a failure and must not be reported as
        // one. tsf unwinds through the identical null return either way (the abort reuses the decode
        // failure's own cleanup), so the reason is only knowable out here.
        if (pt::load_cancelled()) {
            LOGD("🎹 Soundfont load cancelled: %s", path);
            *failure = LoadFailure::CANCELLED;
        } else if (sf_memory_guard_tripped()) {
            LOGE("❌ Soundfont too large for this device (%lld MB free): %s",
                 (long long)(pt::available_memory_bytes() >> 20), path);
            *failure = LoadFailure::OUT_OF_MEMORY;
        } else {
            LOGE("❌ Failed to parse soundfont: %s", path);
            *failure = LoadFailure::PARSE;
        }
        return nullptr;
    }
    // Configured before publication, for the same reason the trimmed path is: a voice that sees the
    // handle must see it ready. `tsf_set_output` is not a read the audio thread can be racing,
    // because nothing else has the pointer yet.
    readyForAudio(loaded, getSampleRate());
    return loaded;
}

/**
 * Give a parsed handle a slot, evicting the least-recently-used one if every slot is taken.
 *
 * ⚠️ Slot-table work only, and only on the thread that owns it — never across a parse.
 */
int AudioEngine::installSoundfont(tsf* handle, int instrumentId, const char* path, int bank,
                                  int preset) {
    if (!handle) return -1;

    // Find a free slot; if none, evict the genuinely least-recently-used one (smallest use tick), not
    // the smallest instrumentId — that could evict the SoundFont playing right now.
    int slot = -1;
    for (int i = 0; i < MAX_SOUNDFONTS; i++) {
        if (soundfonts[i].handle == nullptr) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        uint64_t oldest = UINT64_MAX;
        slot = 0;
        for (int i = 0; i < MAX_SOUNDFONTS; i++) {
            uint64_t lu = soundfonts[i].lastUsed.load(std::memory_order_relaxed);
            if (lu < oldest) { oldest = lu; slot = i; }
        }
        freeSoundfontSlot(slot);
        LOGD("🎹 Evicted soundfont slot %d to make room for instrumentId %d", slot, instrumentId);
    }

    soundfonts[slot].instrumentId = instrumentId;
    soundfonts[slot].filePath     = path;
    soundfonts[slot].bank         = bank;
    soundfonts[slot].preset       = preset;
    soundfonts[slot].lastUsed.store(nextSfUseTick(), std::memory_order_relaxed);
    soundfonts[slot].handle.store(handle);   // last: the audio thread may use it from here on
    LOGD("🎹 Loaded soundfont slot %d: %s [%d:%d]", slot, path, bank, preset);
    return slot;
}

int AudioEngine::loadSoundfont(int instrumentId, const char* path, int bank, int preset) {
    if (!path) return -1;

    // ⚠️ Only one tsf parse at a time — see parseSoundfont. A synchronous load takes precedence over
    // a background one simply by waiting for it, which is at most one preset's worth of decode.
    waitForSoundfontLoad();

    // De-dup: this exact SOUND already loaded reuses its slot instead of a second copy. Multiple
    // instruments share one handle — they play on distinct MIDI channels (= tracks) and apply their
    // ADSR override per-note in fireArmedNote, so per-instrument state stays isolated. Frees stay
    // reference-guarded (setInstrumentType / clearAllSoundfonts).
    //
    // ⚠️ The bank and preset are part of the key, not just the path: a slot holds ONE preset cut out
    // of the file, so two instruments on the same .sf2 at different sounds must not share one.
    for (int i = 0; i < MAX_SOUNDFONTS; i++) {
        if (soundfonts[i].handle != nullptr && soundfonts[i].filePath == path &&
            soundfonts[i].bank == bank && soundfonts[i].preset == preset) {
            soundfonts[i].lastUsed.store(nextSfUseTick(), std::memory_order_relaxed);
            LOGD("🎹 Reusing soundfont slot %d (de-dup): %s [%d:%d]", i, path, bank, preset);
            return i;
        }
    }

    LoadFailure failure = LoadFailure::NONE;
    tsf* handle = parseSoundfont(path, bank, preset, &failure);
    if (!handle) { lastLoadFailure_ = failure; return -1; }

    const int slot = installSoundfont(handle, instrumentId, path, bank, preset);
    lastLoadFailure_ = LoadFailure::NONE;
    return slot;
}

// ─── the same load, off the drawing thread ──────────────────────────────────────────────────────

AudioEngine::SfRequest AudioEngine::requestSoundfontLoad(int instrumentId, const char* path, int bank,
                                                         int preset, int* readySlot) {
    if (readySlot) *readySlot = -1;
    if (!path) return SfRequest::BUSY;

    // A finished worker still holding its result blocks the next request. The caller polls, so it
    // will collect it and come back — refusing is what keeps "one result waiting at a time" true.
    if (sfLoadBusy.load(std::memory_order_acquire)) return SfRequest::BUSY;

    // The same de-dup the synchronous path does, and for the same reason — but here it also spares a
    // thread: walking back to a preset another instrument still holds costs nothing at all.
    for (int i = 0; i < MAX_SOUNDFONTS; i++) {
        if (soundfonts[i].handle != nullptr && soundfonts[i].filePath == path &&
            soundfonts[i].bank == bank && soundfonts[i].preset == preset) {
            soundfonts[i].lastUsed.store(nextSfUseTick(), std::memory_order_relaxed);
            if (readySlot) *readySlot = i;
            return SfRequest::READY;
        }
    }

    sfLoadInstrument = instrumentId;
    sfLoadPath       = path;
    sfLoadBank       = bank;
    sfLoadPreset     = preset;
    sfLoadHandle     = nullptr;
    sfLoadFailure    = LoadFailure::NONE;
    sfLoadDone.store(false, std::memory_order_relaxed);
    sfLoadBusy.store(true, std::memory_order_release);

    // ⚠️ The worker reads the request fields and writes the result fields, and `sfLoadDone` is the
    // fence between the two halves. Nothing else touches them while `sfLoadBusy` is set.
    sfLoadThread = std::thread([this]() {
        tsf* handle = parseSoundfont(sfLoadPath.c_str(), sfLoadBank, sfLoadPreset, &sfLoadFailure);
        sfLoadHandle = handle;
        sfLoadDone.store(true, std::memory_order_release);
    });
    return SfRequest::STARTED;
}

bool AudioEngine::collectSoundfontLoad(int* instrumentId, int* slot) {
    if (!sfLoadBusy.load(std::memory_order_acquire)) return false;
    if (!sfLoadDone.load(std::memory_order_acquire)) return false;

    if (sfLoadThread.joinable()) sfLoadThread.join();

    tsf* handle = sfLoadHandle;
    sfLoadHandle = nullptr;

    const int landed = handle ? installSoundfont(handle, sfLoadInstrument, sfLoadPath.c_str(),
                                                 sfLoadBank, sfLoadPreset)
                              : -1;
    lastLoadFailure_ = handle ? LoadFailure::NONE : sfLoadFailure;
    if (instrumentId) *instrumentId = sfLoadInstrument;
    if (slot) *slot = landed;

    // Released LAST: it is what lets the next request start, and the result must be fully read out
    // of the members before another one can overwrite them.
    sfLoadBusy.store(false, std::memory_order_release);
    return true;
}

bool AudioEngine::soundfontLoadPending() const {
    return sfLoadBusy.load(std::memory_order_acquire);
}

void AudioEngine::waitForSoundfontLoad() {
    if (!sfLoadBusy.load(std::memory_order_acquire)) return;
    if (sfLoadThread.joinable()) sfLoadThread.join();

    // ⚠️⚠️ THE RESULT IS KEPT, NOT THROWN AWAY: only the PARSE must be alone. The caller was told
    // the load was accepted and the PATCH row will not ask twice, so discarding it would leave the
    // instrument on its old sound for good. `sfLoadBusy` stays set and the next poll collects it.
}

void AudioEngine::discardSoundfontLoad() {
    if (!sfLoadBusy.load(std::memory_order_acquire)) return;
    if (sfLoadThread.joinable()) sfLoadThread.join();

    // ⚠️ Here the answer really is worthless: the project it was asked for is being torn down, so a
    // slot given to it would belong to a document that no longer exists.
    if (sfLoadHandle) {
        tsf_close(sfLoadHandle);
        sfLoadHandle = nullptr;
    }
    sfLoadDone.store(false, std::memory_order_relaxed);
    sfLoadBusy.store(false, std::memory_order_release);
}

bool AudioEngine::soundfontSlotHolds(int slot, const char* path, int bank, int preset) {
    if (slot < 0 || slot >= MAX_SOUNDFONTS || !path) return false;
    return soundfonts[slot].handle.load() != nullptr && soundfonts[slot].filePath == path &&
           soundfonts[slot].bank == bank && soundfonts[slot].preset == preset;
}

int AudioEngine::soundfontSlotCount() const { return MAX_SOUNDFONTS; }

bool AudioEngine::soundfontSlotSound(int slot, std::string& path, int& bank, int& preset) {
    if (slot < 0 || slot >= MAX_SOUNDFONTS) return false;
    if (!soundfonts[slot].handle.load()) return false;
    path   = soundfonts[slot].filePath;
    bank   = soundfonts[slot].bank;
    preset = soundfonts[slot].preset;
    return true;
}

void AudioEngine::unloadSoundfont(int slot) {
    if (slot < 0 || slot >= MAX_SOUNDFONTS) return;
    freeSoundfontSlot(slot);
    LOGD("🎹 Unloaded soundfont slot %d", slot);
}

void AudioEngine::clearAllSoundfonts() {
    // ⚠️ A load still in flight belongs to the project being thrown away. Waited for and discarded,
    // or it would land in a slot the new project has to clear all over again.
    discardSoundfontLoad();

    // Free EVERY slot on a project change (NEW / load); otherwise a slot is reclaimed only by LRU
    // eviction, and a loaded font's samples would stay resident.
    for (int s = 0; s < MAX_SOUNDFONTS; s++) freeSoundfontSlot(s);
    LOGD("🎹 Cleared all soundfont slots");
}

// ─── the FILE's preset list ─────────────────────────────────────────────────────────────────────
//
// A loaded slot holds one preset, so it can no longer say what else the file contains. These read the
// file's index instead — a few kilobytes even for a 200 MB bank, and no sample data at all, which is
// what makes browsing a font that is far too large to load work exactly like browsing a small one.

int AudioEngine::soundfontFileIndexSlot(const char* path) {
    for (size_t i = 0; i < sfFileIndexCache.size(); ++i) {
        if (sfFileIndexCache[i].path == path) return static_cast<int>(i);
    }
    std::vector<pt::SfPreset> presets;
    if (!pt::sf_read_preset_list(path, presets)) return -1;

    // Four files is more than the PATCH row can be walking at once; the oldest goes, and re-reading it
    // costs one small read.
    if (sfFileIndexCache.size() >= 4) sfFileIndexCache.erase(sfFileIndexCache.begin());
    sfFileIndexCache.push_back({ std::string(path), std::move(presets) });
    return static_cast<int>(sfFileIndexCache.size()) - 1;
}

int AudioEngine::getSoundfontFilePresetCount(const char* path) {
    if (!path) return 0;
    std::lock_guard<std::mutex> lock(sfFileIndexMutex);
    const int i = soundfontFileIndexSlot(path);
    return (i < 0) ? 0 : static_cast<int>(sfFileIndexCache[static_cast<size_t>(i)].presets.size());
}

bool AudioEngine::getSoundfontFilePresetAt(const char* path, int index, int* bank, int* presetNumber) {
    if (!path || index < 0) return false;
    std::lock_guard<std::mutex> lock(sfFileIndexMutex);
    const int i = soundfontFileIndexSlot(path);
    if (i < 0) return false;
    const std::vector<pt::SfPreset>& list = sfFileIndexCache[static_cast<size_t>(i)].presets;
    if (index >= static_cast<int>(list.size())) return false;
    if (bank) *bank = list[static_cast<size_t>(index)].bank;
    if (presetNumber) *presetNumber = list[static_cast<size_t>(index)].preset;
    return true;
}

std::string AudioEngine::getSoundfontFilePresetName(const char* path, int bank, int preset) {
    if (!path) return "---";
    std::lock_guard<std::mutex> lock(sfFileIndexMutex);
    const int i = soundfontFileIndexSlot(path);
    if (i < 0) return "---";
    for (const pt::SfPreset& p : sfFileIndexCache[static_cast<size_t>(i)].presets) {
        if (p.bank == bank && p.preset == preset) return p.name.empty() ? std::string("---") : p.name;
    }
    return "---";
}
