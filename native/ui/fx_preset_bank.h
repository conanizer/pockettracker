#pragma once

// The EFFECTS screen's two preset rows: their names, and the built-in cycle (effects/modules/*-presets.h).
// The project's `reverbPreset` / `delayPreset` are labels; an empty one is named by matching the cells.
// ⚠️ An `apply` must write every cell its `match` reads, or the `*` can never clear.

#include <string>

#include "effects/modules/delay-presets.h"
#include "effects/modules/reverb-presets.h"
#include "songcore/model.h"

namespace pt::ui {

// ── Reverb ───────────────────────────────────────────────────────────────────────────────────────

// −1 if none.
inline int reverb_bank_index_by_name(const std::string& name) {
    for (int i = 0; i < kReverbPresetCount; ++i)
        if (name == kReverbPresets[i].name) return i;
    return -1;
}

// The built-in the project's cells equal, or −1.
inline int reverb_bank_match(const songcore::Project& p) {
    return reverb_preset_match(p.reverbAlgo, p.reverbFeedback, p.reverbSize, p.reverbDamp,
                               p.reverbPreDelay, p.reverbWidth, p.reverbMod);
}

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

// The stored name, else the built-in the cells match, else empty.
inline std::string reverb_preset_display_name(const songcore::Project& p) {
    if (!p.reverbPreset.empty()) return p.reverbPreset;
    const int idx = reverb_bank_match(p);
    return idx >= 0 ? std::string(kReverbPresets[idx].name) : std::string();
}

// The `*`: the cells drifted from the built-in the row is named after (never for a file's name).
inline bool reverb_preset_differs_from_its_name(const songcore::Project& p) {
    const int idx = reverb_bank_index_by_name(p.reverbPreset);
    return idx >= 0 && reverb_bank_match(p) != idx;
}

// Where the cycle steps from: the named row, else the matching one, else 0.
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

// TIME, FDBK and REV stay.
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
