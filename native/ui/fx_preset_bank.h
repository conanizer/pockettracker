#pragma once

// ─── The reverb's and the delay's preset NAMES ───────────────────────────────────────────────────
//
// What the EFFECTS screen's two preset rows show and cycle: the SCALE screen's name row, for a send
// (songcore/scale_bank.h). The built-in rows are compiled in (effects/modules/*-presets.h); a saved
// file is ui/fx_preset_io.h.
// ⚠️ The project's `reverbPreset` / `delayPreset` are LABELS. An empty one is named by matching the
// cells, so a project that never touched the row stores no name.
// ⚠️⚠️ An `apply` must write every cell its `match` reads, or the `*` can never clear.

#include <string>

#include "effects/modules/delay-presets.h"
#include "effects/modules/reverb-presets.h"
#include "songcore/model.h"

namespace pt::ui {

// ── Reverb ───────────────────────────────────────────────────────────────────────────────────────

/** The built-in reverb preset with this exact name, or −1. */
inline int reverb_bank_index_by_name(const std::string& name) {
    for (int i = 0; i < kReverbPresetCount; ++i)
        if (name == kReverbPresets[i].name) return i;
    return -1;
}

/** The built-in preset the project's reverb cells are, or −1. */
inline int reverb_bank_match(const songcore::Project& p) {
    return reverb_preset_match(p.reverbAlgo, p.reverbFeedback, p.reverbSize, p.reverbDamp,
                               p.reverbPreDelay, p.reverbWidth, p.reverbMod);
}

/** Write built-in row `index` into the project, with its name. */
inline void reverb_apply_bank(songcore::Project& p, int index) {
    if (index < 0 || index >= kReverbPresetCount) return;
    const ReverbPreset& r = kReverbPresets[index];
    p.reverbAlgo     = r.algo;
    p.reverbFeedback = r.decay;
    p.reverbSize     = r.room;
    p.reverbDamp     = r.damp;
    p.reverbPreDelay = r.pre;
    p.reverbWidth    = r.width;
    p.reverbMod      = r.mod;
    p.reverbPreset   = r.name;
}

/** The name to SHOW: the stored one, else the built-in the cells match, else empty (hand-set). */
inline std::string reverb_preset_display_name(const songcore::Project& p) {
    if (!p.reverbPreset.empty()) return p.reverbPreset;
    const int idx = reverb_bank_match(p);
    return idx >= 0 ? std::string(kReverbPresets[idx].name) : std::string();
}

/** Have the cells drifted from the built-in the row is named after? (the `*`). False for a name the
 *  built-ins do not know — a file's. */
inline bool reverb_preset_differs_from_its_name(const songcore::Project& p) {
    const int idx = reverb_bank_index_by_name(p.reverbPreset);
    return idx >= 0 && reverb_bank_match(p) != idx;
}

/** The row the cycle steps FROM: the one the name claims, else the one the cells match, else 0. */
inline int reverb_bank_cycle_index(const songcore::Project& p) {
    int idx = reverb_bank_index_by_name(p.reverbPreset);
    if (idx < 0) idx = reverb_bank_match(p);
    return idx < 0 ? 0 : idx;
}

// ── Delay ────────────────────────────────────────────────────────────────────────────────────────

inline int delay_bank_index_by_name(const std::string& name) {
    for (int i = 0; i < kDelayPresetCount; ++i)
        if (name == kDelayPresets[i].name) return i;
    return -1;
}

inline int delay_bank_match(const songcore::Project& p) {
    return delay_preset_match(p.delayPong, p.delayTone, p.delayWobble);
}

/** Write built-in row `index` into the project, with its name. ⚠️ TIME, FDBK and REV stay. */
inline void delay_apply_bank(songcore::Project& p, int index) {
    if (index < 0 || index >= kDelayPresetCount) return;
    const DelayPreset& d = kDelayPresets[index];
    p.delayPong   = d.pong;
    p.delayTone   = d.tone;
    p.delayWobble = d.wobble;
    p.delayPreset = d.name;
}

inline std::string delay_preset_display_name(const songcore::Project& p) {
    if (!p.delayPreset.empty()) return p.delayPreset;
    const int idx = delay_bank_match(p);
    return idx >= 0 ? std::string(kDelayPresets[idx].name) : std::string();
}

inline bool delay_preset_differs_from_its_name(const songcore::Project& p) {
    const int idx = delay_bank_index_by_name(p.delayPreset);
    return idx >= 0 && delay_bank_match(p) != idx;
}

inline int delay_bank_cycle_index(const songcore::Project& p) {
    int idx = delay_bank_index_by_name(p.delayPreset);
    if (idx < 0) idx = delay_bank_match(p);
    return idx < 0 ? 0 : idx;
}

}  // namespace pt::ui
