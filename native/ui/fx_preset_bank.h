#pragma once

// The EFFECTS screen's two preset rows: their names (effects/modules/*-presets.h). The project's
// `reverbPreset` / `delayPreset` are labels; an empty one is named by matching the built-ins' cells.

#include <string>

#include "effects/modules/delay-presets.h"
#include "effects/modules/reverb-presets.h"
#include "songcore/model.h"
#include "songcore/project_ops.h"

namespace pt::ui {

using songcore::delay_apply_bank;
using songcore::reverb_apply_bank;

// ── Reverb ───────────────────────────────────────────────────────────────────────────────────────

// The built-in the project's cells equal, or −1.
inline int reverb_bank_match(const songcore::Project& p) { return reverb_preset_match(p.reverb); }

// The stored name, else the built-in the cells match, else empty.
inline std::string reverb_preset_display_name(const songcore::Project& p) {
    if (!p.reverbPreset.empty()) return p.reverbPreset;
    const int idx = reverb_bank_match(p);
    return idx >= 0 ? std::string(kReverbPresets[idx].name) : std::string();
}

// ── Delay ────────────────────────────────────────────────────────────────────────────────────────

inline int delay_bank_match(const songcore::Project& p) { return delay_preset_match(p.delay); }

inline std::string delay_preset_display_name(const songcore::Project& p) {
    if (!p.delayPreset.empty()) return p.delayPreset;
    const int idx = delay_bank_match(p);
    return idx >= 0 ? std::string(kDelayPresets[idx].name) : std::string();
}

}  // namespace pt::ui
