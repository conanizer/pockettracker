#ifndef POCKETTRACKER_SONGCORE_VOICE_CODES_H
#define POCKETTRACKER_SONGCORE_VOICE_CODES_H

// The names a voice's modulation and filter travel under, from the project to the engine. A leaf: the
// engine includes it too, so the two sides can never disagree on a number.

namespace songcore {

// ⚠️ Saved by NAME (model.h's MOD_TYPE_NAMES / MOD_DEST_NAMES): append, never insert.
enum class ModType { NONE, AHD, ADSR, LFO, DRUM, TRIG, TRACKING, SCALAR };
// MOD_AMT, MOD_RATE and MOD_BOTH aim at the NEXT slot. TIMBRE and COLOR are a MACRO instrument's two
// controls; nothing on any other type.
enum class ModDest {
    NONE, VOLUME, PAN, PITCH, FINE_PITCH, FILTER_CUTOFF, FILTER_RES,
    SAMPLE_START, MOD_AMT, MOD_RATE, MOD_BOTH,
    TIMBRE, COLOR
};

// ⚠️ Saved as these numbers. An LFO's `lfoTrigMode`: FREE and RETG differ only in the phase set at the
// note; HOLD freezes the value taken there; ONCE runs one cycle and holds its end.
constexpr int LFO_FREE = 0, LFO_RETG = 1, LFO_HOLD = 2, LFO_ONCE = 3;
// The stateful `oscShape`s, after the eight drawn ones.
constexpr int LFO_RND = 8, LFO_DRNK = 9;

// One slot as the engine runs it, times in samples. NONE clears the slot.
struct EngineModSlot {
    ModType type  = ModType::NONE;
    ModDest dest  = ModDest::NONE;
    float amount  = 0.0f;           // 0-1
    int   attackSamples  = 0;
    int   holdSamples    = 0;       // AHD and DRUM
    int   decaySamples   = 0;
    float sustainLevel   = 0.5f;    // ADSR and TRIG, 0-1
    int   releaseSamples = 0;       // ADSR and TRIG; 0 = instant
    float lfoHz          = 4.0f;
    int   oscShape       = 0;       // 0 TRI, 1 SIN, 2 RMP+, 3 RMP-, 4 EXP+, 5 EXP-, 6 SQU+, 7 SQU-, 8 RND, 9 DRNK
    int   lfoTrigMode    = LFO_RETG;
};

// The engine's filter. The project stores the name ("lp"…), not the number. All are taps of one
// state-variable filter; the app never offers PEAK, the tools' wave plugin does.
enum FilterType : int { FILTER_OFF, FILTER_LP, FILTER_HP, FILTER_BP, FILTER_NOTCH, FILTER_PEAK };

}  // namespace songcore

#endif
