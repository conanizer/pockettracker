#ifndef POCKETTRACKER_SONGCORE_VOICE_CODES_H
#define POCKETTRACKER_SONGCORE_VOICE_CODES_H

// The names a voice's modulation and filter travel under, from the project to the engine. A leaf: the
// engine includes it too, so the two sides can never disagree on a number.

namespace songcore {

// ⚠️ Saved by NAME (model.h's MOD_TYPE_NAMES / MOD_DEST_NAMES): append, never insert. DRUM and TRIG
// only load — a file's become AHD and ADSR. ENV is a curved ADSR timed in seconds. VELOCITY is the
// note's velocity, held for the note.
enum class ModType { NONE, AHD, ADSR, LFO, DRUM, TRIG, TRACKING, SCALAR, ENV, VELOCITY };
// MODn_AMT, _RATE and _BOTH aim at slot n. MOD_AMT, MOD_RATE and MOD_BOTH aim at the NEXT slot; a
// file's are read as the MODn ones (project_io.h), the engine still answers them.
enum class ModDest {
    NONE, VOLUME, PAN, PITCH, FINE_PITCH, FILTER_CUTOFF, FILTER_RES,
    SAMPLE_START, MOD_AMT, MOD_RATE, MOD_BOTH,
    TIMBRE, COLOR,   // a MACRO instrument's two controls; nothing on any other type
    MOD1_AMT, MOD1_RATE, MOD1_BOTH, MOD2_AMT, MOD2_RATE, MOD2_BOTH,
    MOD3_AMT, MOD3_RATE, MOD3_BOTH, MOD4_AMT, MOD4_RATE, MOD4_BOTH
};

// A route into another slot: the slot (0-3, -1 = not such a route) and what it moves there.
enum class ModTargetPart { AMT, RATE, BOTH };
struct ModTarget { int slot = -1; ModTargetPart part = ModTargetPart::AMT; };

inline ModDest mod_target_dest(int slot, ModTargetPart part) {
    return static_cast<ModDest>(static_cast<int>(ModDest::MOD1_AMT) + slot * 3 + static_cast<int>(part));
}

// `d` set on slot `own`.
inline ModTarget mod_target(ModDest d, int own) {
    const int i = static_cast<int>(d);
    if (d == ModDest::MOD_AMT || d == ModDest::MOD_RATE || d == ModDest::MOD_BOTH)
        return {(own + 1) % 4, static_cast<ModTargetPart>(i - static_cast<int>(ModDest::MOD_AMT))};
    const int k = i - static_cast<int>(ModDest::MOD1_AMT);
    if (k < 0 || k >= 12) return {};
    return {k / 3, static_cast<ModTargetPart>(k % 3)};
}

// ⚠️ Saved as these numbers. An AHD or ADSR slot's `curve`:
constexpr int CURVE_LINEAR = 0;
constexpr int CURVE_FAST   = 1;   // falls 60 dB in its time
constexpr int CURVE_ANALOG = 2;   // a gentler bend
// The time scale of the ENV type: not a curve, never saved. Every other slot's scale is its curve.
constexpr int ENV_SCALE_LOG = -1;
// An LFO's `lfoTrigMode`: FREE and RETG differ only in the phase set at the note; HOLD freezes the
// value taken there; ONCE runs one cycle and holds its end.
constexpr int LFO_FREE = 0, LFO_RETG = 1, LFO_HOLD = 2, LFO_ONCE = 3;
// An LFO's `lfoPolarity`: swings −1..+1, or only 0..+1, or only 0..−1.
constexpr int LFO_BI = 0, LFO_UP = 1, LFO_DOWN = 2;
// The stateful `oscShape`s, after the eight drawn ones.
constexpr int LFO_RND = 8, LFO_DRNK = 9;

// One slot as the engine runs it, times in samples. NONE clears the slot.
struct EngineModSlot {
    ModType type  = ModType::NONE;
    ModDest dest  = ModDest::NONE;
    int     curve = CURVE_LINEAR;   // AHD and ADSR
    float amount  = 0.0f;           // 0-1
    int   attackSamples  = 0;
    int   holdSamples    = 0;       // AHD
    int   decaySamples   = 0;
    float sustainLevel   = 0.5f;    // ADSR and ENV, 0-1
    int   releaseSamples = 0;       // ADSR and ENV; 0 = instant
    float lfoHz          = 4.0f;
    int   oscShape       = 0;       // 0 TRI, 1 SIN, 2 RMP+, 3 RMP-, 4 EXP+, 5 EXP-, 6 SQU+, 7 SQU-, 8 RND, 9 DRNK
    int   lfoTrigMode    = LFO_RETG;
    int   lfoPolarity    = LFO_BI;
};

// The engine's filter. The project stores the name ("lp", "k35"…), not the number. LP to PEAK are taps
// of one state-variable filter; the app never offers PEAK, the tools' wave plugin does.
enum FilterType : int {
    FILTER_OFF, FILTER_LP, FILTER_HP, FILTER_BP, FILTER_NOTCH, FILTER_PEAK, FILTER_K35, FILTER_LADDER
};

}  // namespace songcore

#endif
