// The SoundFont bank: parsing a preset into a tsf handle (in the background or not), the slot cache
// with its de-dup and LRU, the per-file preset index, and the per-instrument ADSR override.
#include "audio-engine.h"
#include "vendor/tsf/tsf.h"    // declarations only
#include "common/byte_source.h"       // pt_fopen
#include "common/platform_memory.h"   // load_budget_bytes
#include "common/load_progress.h"     // load_tick / load_cancelled
#include <cstdio>
#include <cstdint>
#include <climits>   // INT_MAX — tsf_load_memory takes an int size
#include <vector>

namespace {
// tsf_load's stream over a pt_fopen'd FILE: `read` returns bytes, `skip` 1 or 0. Taking `void*`
// keeps the function-pointer type exact.
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


void AudioEngine::freeSoundfontSlot(int slot) {
    if (slot < 0 || slot >= MAX_SOUNDFONTS) return;
    // The next block detaches its voices (the generation); the handle closes once that block ends.
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

// ⚠️ Sizes tsf's voice and channel arrays now — otherwise tsf grows them on the audio thread. With a
// full pool it steals the voice furthest into its release.
static void readyForAudio(tsf* h, int sampleRate) {
    tsf_set_output(h, TSF_STEREO_INTERLEAVED, sampleRate, 0.0f);
    tsf_set_max_voices(h, 128);
    tsf_channel_set_pan(h, SF_VOICE_COUNT - 1, 0.5f);   // creates channels 0..8
}

// File + bank/preset → a parsed handle. Touches no slot and no member — it runs on the worker too.
tsf* AudioEngine::parseSoundfont(const char* path, int bank, int preset, LoadFailure* failure) {
    *failure = LoadFailure::NONE;

    // First try the one preset cut out of the file; if it cannot be cut, the whole bank.
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
            // ⚠️ First: a cancel returns the same null, and falling through would restart the load.
            if (pt::load_cancelled()) {
                LOGD("🎹 Soundfont load cancelled: %s", path);
                *failure = LoadFailure::CANCELLED;
                return nullptr;
            }
            // A trimmed font that will not parse is our bug; load it whole rather than fail.
            LOGE("❌ Trimmed soundfont failed to parse, loading whole bank: %s", path);
        }
    }

    // Streamed through pt_fopen: peak RAM is the parsed font alone.
    FILE* sf = pt_fopen(path, "rb");
    if (!sf) {
        LOGE("❌ Cannot open soundfont: %s", path);
        *failure = LoadFailure::PARSE;
        return nullptr;
    }
    tsf_stream sfStream = { sf, &sfStreamRead, &sfStreamSkip };
    // The memory guard's flag (soundfont-voice.cpp) tells "too big" from "not a soundfont".
    sf_memory_guard_reset();
    tsf* loaded = tsf_load(&sfStream);
    std::fclose(sf);
    if (!loaded) {
        // ⚠️ Before the guard: a cancel returns the same null and must not read as a failure.
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
    // Configured before anyone else has the pointer.
    readyForAudio(loaded, getSampleRate());
    return loaded;
}

// A slot for a parsed handle, evicting the least recently used. ⚠️ Owner thread only, never across a parse.
int AudioEngine::installSoundfont(tsf* handle, int instrumentId, const char* path, int bank,
                                  int preset) {
    if (!handle) return -1;

    // A free slot, else the smallest use tick.
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

    // ⚠️ One tsf parse at a time: wait for a background one (at most one preset's decode).
    waitForSoundfontLoad();

    // The same sound already loaded shares its slot; each instrument applies its ADSR per note.
    // ⚠️ Keyed by path, bank AND preset.
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

    // An uncollected result blocks the next request; the caller polls and comes back.
    if (sfLoadBusy.load(std::memory_order_acquire)) return SfRequest::BUSY;

    // The synchronous path's de-dup.
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

    // ⚠️ While `sfLoadBusy` is set only the worker touches the request/result fields; `sfLoadDone`
    // fences the two halves.
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

    // Last: it lets the next request overwrite the result.
    sfLoadBusy.store(false, std::memory_order_release);
    return true;
}

bool AudioEngine::soundfontLoadPending() const {
    return sfLoadBusy.load(std::memory_order_acquire);
}

void AudioEngine::waitForSoundfontLoad() {
    if (!sfLoadBusy.load(std::memory_order_acquire)) return;
    if (sfLoadThread.joinable()) sfLoadThread.join();

    // ⚠️ The result is kept for the next poll — the PATCH row will not ask twice.
}

void AudioEngine::discardSoundfontLoad() {
    if (!sfLoadBusy.load(std::memory_order_acquire)) return;
    if (sfLoadThread.joinable()) sfLoadThread.join();

    // Here the result is discarded: its project is being torn down.
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
    // A load in flight belongs to the old project: waited for and discarded.
    discardSoundfontLoad();

    // Every slot, or the old project's fonts stay resident until evicted.
    for (int s = 0; s < MAX_SOUNDFONTS; s++) freeSoundfontSlot(s);
    LOGD("🎹 Cleared all soundfont slots");
}

// ─── the FILE's preset list ─────────────────────────────────────────────────────────────────────
// Read from the file's index — a slot holds one preset, and a font too large to load can still be listed.

int AudioEngine::soundfontFileIndexSlot(const char* path) {
    for (size_t i = 0; i < sfFileIndexCache.size(); ++i) {
        if (sfFileIndexCache[i].path == path) return static_cast<int>(i);
    }
    std::vector<pt::SfPreset> presets;
    if (!pt::sf_read_preset_list(path, presets)) return -1;

    // Four files; the oldest goes.
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
