#pragma once

// ─── Reading a SoundFont's index without its samples, and writing a one-preset copy ──────────────
//
// An SF2/SF3 file is a RIFF with two big pieces: `sdta`, the sample audio, and `pdta`, the "hydra" —
// nine small tables that between them say which samples every preset reaches. The hydra is 5–200 KB
// even for a 200 MB bank, and it comes *after* the samples in the file, so both functions here seek
// past `sdta` and read only the index.
//
// `sf_read_preset_list` answers "what is in this file?" for a few kilobytes of I/O.
// `sf_build_trimmed_font` answers "give me just this one preset" — it resolves the preset down to
// the set of sample headers it reaches, copies only those byte ranges, and emits a complete, valid
// SoundFont in memory that tsf loads through `tsf_load_memory` unmodified and unaware.
//
// ⚠️ **Nothing here parses a sample.** Compressed (SF3) sample ranges are copied as opaque bytes,
// so the expensive half — Vorbis decoding — is skipped for every preset that is left out. That is
// where the win is largest: a compressed bank decodes to roughly twenty times its file size, and it
// is the decoding, not the reading, that a small device cannot afford.

#include <cstdint>
#include <string>
#include <vector>

namespace pt {

/** One row of a SoundFont's preset list: what the INSTRUMENT screen's PATCH row cycles through. */
struct SfPreset {
    int         bank   = 0;
    int         preset = 0;
    std::string name;
};

/**
 * Read `path`'s preset list, reading no sample bytes.
 *
 * ⚠️ **The order is tsf's order** — by bank, then preset number, then file order — so index `i` here
 * is the same preset index a fully loaded font would report. A different order silently renumbers
 * every patch a user has already chosen.
 *
 * Returns false if the file is not a SoundFont or has no preset table. `out` is cleared first.
 */
bool sf_read_preset_list(const char* path, std::vector<SfPreset>& out);

/**
 * Build a complete SoundFont in memory containing only the preset at `bank`:`preset` from `path`.
 *
 * The result is a normal `.sf2`/`.sf3` byte-for-byte — RIFF header, all nine hydra chunks with their
 * terminal records, and an `smpl` chunk holding just the sample ranges this preset reaches. Hand it
 * to `tsf_load_memory`.
 *
 * Returns false when the preset is not in the file, when the file cannot be read, or when the layout
 * is one this cannot trim. A false answer means "load the whole bank instead", never "this file is
 * broken", and the caller does exactly that.
 *
 * ⚠️⚠️ **A COMPRESSED FONT IS LAID OUT DIFFERENTLY FROM AN UNCOMPRESSED ONE, AND THE `.cpp` IS WHERE
 * THAT IS ARGUED.** Both have to end with silence sitting exactly where a region's overrun reads: a
 * PCM sample gets it as bytes written after itself, an Ogg stream — which cannot be padded from the
 * inside — gets it as a silent sample HEADER placed after it in the table. Mixing the two rules, or
 * reordering the table, is wrong sound rather than an error.
 */
bool sf_build_trimmed_font(const char* path, int bank, int preset, std::vector<uint8_t>& out);

}  // namespace pt
