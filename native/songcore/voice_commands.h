#ifndef POCKETTRACKER_SONGCORE_VOICE_COMMANDS_H
#define POCKETTRACKER_SONGCORE_VOICE_COMMANDS_H

// The step commands that move the sounding voice by a controller (event.h): what a step resolves
// and sends, what a retrigger re-applies, what reaches another PocketTracker. PAN and VOL are not
// here — they ride the note-on.

#include <cstdint>

#include "effect_codes.h"
#include "event.h"

namespace songcore {

// One controller a step can set, in the order a step sends them. LPF/HPF/BPF share a lane, so two on
// one row resolve last-wins (each sets type AND cutoff).
enum VoiceLane : int {
    LANE_REVERB_SEND, LANE_DELAY_SEND, LANE_FILTER_MODE, LANE_FILTER_CUT, LANE_FILTER_RES,
    LANE_DRIVE, LANE_CRUSH, LANE_FINE_TUNE,
    LANE_MACRO_TIMBRE, LANE_MACRO_COLOR,
    LANE_LOOP_SLIDE,
    VOICE_LANES
};

// A retrigger re-applies the lanes below this one. Not LPO: a slide adds to the last, and a carry
// holds one value, not the sum.
constexpr int CARRIED_LANES = LANE_LOOP_SLIDE;

struct VoiceCommand {
    int     fxCode;
    uint8_t ccId;
    int     lane;
    bool    noteCommand;   // travels to another PocketTracker as a note command (note_command.h)
};

inline constexpr VoiceCommand VOICE_COMMANDS[] = {
    { FX_RSEND, CC_REVERB_SEND, LANE_REVERB_SEND, true  },
    { FX_DSEND, CC_DELAY_SEND,  LANE_DELAY_SEND,  true  },
    { FX_LPF,   CC_FILTER_LP,   LANE_FILTER_MODE, true  },
    { FX_HPF,   CC_FILTER_HP,   LANE_FILTER_MODE, true  },
    { FX_BPF,   CC_FILTER_BP,   LANE_FILTER_MODE, true  },
    { FX_CUT,   CC_FILTER_CUT,  LANE_FILTER_CUT,  true  },
    { FX_RES,   CC_FILTER_RES,  LANE_FILTER_RES,  true  },
    { FX_DRV,   CC_DRIVE,       LANE_DRIVE,       true  },
    { FX_CRU,   CC_CRUSH,       LANE_CRUSH,       true  },
    { FX_FIN,   CC_FINE_TUNE,   LANE_FINE_TUNE,   false },   // goes out as the pitch wheel (midi_out.h)
    { FX_TMB,   CC_MACRO_TIMBRE, LANE_MACRO_TIMBRE, false },   // this engine's oscillators only
    { FX_CLR,   CC_MACRO_COLOR,  LANE_MACRO_COLOR,  false },
    { FX_LPO,   CC_LOOP_SLIDE,  LANE_LOOP_SLIDE,  true  },
};

inline constexpr const VoiceCommand* voice_command_for_fx(int fxCode) {
    for (const VoiceCommand& c : VOICE_COMMANDS)
        if (c.fxCode == fxCode) return &c;
    return nullptr;
}

inline constexpr const VoiceCommand* voice_command_for_cc(int ccId) {
    for (const VoiceCommand& c : VOICE_COMMANDS)
        if (c.ccId == ccId) return &c;
    return nullptr;
}

/** The lane a controller id moves, or -1. */
inline constexpr int voice_lane(int ccId) {
    const VoiceCommand* c = voice_command_for_cc(ccId);
    return c != nullptr ? c->lane : -1;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_VOICE_COMMANDS_H
