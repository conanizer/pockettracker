#pragma once

// ─── Reading a SoundFont's index without its samples, and writing a one-preset copy ──────────────
// Both seek past the samples (`sdta`) and read only the index (`pdta`, the "hydra": nine small tables,
// a few KB even for a 200 MB bank). No sample is ever decoded — Ogg ranges are copied as bytes.

#include <cstdint>
#include <string>
#include <vector>

namespace pt {

// One entry of the list the PATCH row cycles through.
struct SfPreset {
    int         bank   = 0;
    int         preset = 0;
    std::string name;
};

// `path`'s presets, reading no sample bytes; false when it is not a SoundFont. `out` is cleared first.
// ⚠️ In tsf's order (bank, preset, file order) — any other renumbers every patch already chosen.
bool sf_read_preset_list(const char* path, std::vector<SfPreset>& out);

// A complete SoundFont in memory holding only `bank`:`preset`, for `tsf_load_memory`. False means
// "load the whole bank instead", never "broken file".
// ⚠️ Compressed and uncompressed fonts are laid out differently (argued in the .cpp); a mistake is
// wrong sound, not an error.
bool sf_build_trimmed_font(const char* path, int bank, int preset, std::vector<uint8_t>& out);

}  // namespace pt
