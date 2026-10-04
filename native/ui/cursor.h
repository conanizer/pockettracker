#pragma once

// ─── The cursor-context system ───────────────────────────────────────────────────────────────────
//
// The input layer never asks "which screen am I on?". Each module answers `cursor_context(state)` —
// "a NOTE, empty, insertable, range 12..127, small step 1, large step 12" — and one generic handler
// turns presses into InputActions from that alone. Every screen gets increment / decrement / fast
// step / delete / insert for free; a new column is a new `case` in one function.

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

#include "songcore/effects.h"

namespace pt::ui {

/** Sentinel for `default_value`: this cell has no A+B "reset to default" target. */
inline constexpr int NO_DEFAULT = std::numeric_limits<int>::min();

/** What kind of value the cursor is on. Each steps differently — see `step_value`, where the wrapping
 *  and clamping sets part ways. */
enum class CursorValueType {
    // Numeric values that can be increased/decreased
    HEX_BYTE,         // 00-FF (most common: phrases, chains, instruments)
    HEX_NIBBLE,       // 0-F (single hex digit)
    SEMITONE_OFFSET,  // transpose values (centred at 0x80)

    // Musical values
    NOTE,
    VOLUME,

    // Continuous physical-unit values (clamp at bounds, no wrap)
    GAIN,  // EQ gain in dB (stored 0..240 = −12.0..+12.0 dB; one step = 0.1 dB)
    FREQ,  // EQ frequency (stored 0..255 = log 20Hz..20kHz; stepping is display-aware)

    // Reference types
    PHRASE_REF,
    CHAIN_REF,
    INSTRUMENT_REF,

    // Text editing
    CHARACTER,  // character from the allowed set (A-Z, 0-9, _, -, space)

    // Toggles
    TOGGLE_BINARY,
    TOGGLE_TERNARY,

    // Effects
    EFFECT_TYPE,   // an INDEX into songcore::EFFECT_TYPES, not an effect code
    EFFECT_VALUE,  // 00-FF

    // Special
    EMPTY,
    READ_ONLY,  // can't edit (step numbers)
    NONE        // no cursor / invalid position
};

/** Which button combinations do anything here. */
struct CursorCapabilities {
    bool canIncrement     = false;  // A+RIGHT
    bool canDecrement     = false;  // A+LEFT
    bool canIncrementFast = false;  // A+UP
    bool canDecrementFast = false;  // A+DOWN
    bool canDelete        = false;  // A+B
    bool canInsert        = false;  // A on an empty cell
    bool canCreate        = false;  // A+A
    bool isEmpty          = false;
};

/** What the cursor is on, and what can be done to it. Every module's `cursor_context()` returns one. */
struct CursorContext {
    CursorValueType    valueType = CursorValueType::NONE;
    CursorCapabilities capabilities{};
    int                currentValue = 0;
    int                minValue     = 0;
    int                maxValue     = 255;
    int                smallStep    = 1;   // A+RIGHT / A+LEFT
    int                largeStep    = 16;  // A+UP / A+DOWN
    int                emptyValue   = 0xFF;
    int                fxSlot       = 0;  // for effects: which FX slot (1, 2, 3)
    int                defaultValue = NO_DEFAULT;  // A+B resets a non-deletable value to this

    /**
     * NOTE cells only: which pitch classes may be typed — a 12-bit mask, bit 0 = pitch class
     * `scaleKey`. `0x0FFF` (chromatic) means no constraint, the default everywhere.
     * ⚠️ On the CONTEXT because stepping is where the scale is felt: A+RIGHT must move a whole degree;
     * correcting an out-of-scale step afterwards would land back where it started.
     * The input tests do not print these two, so their golden is unaffected.
     */
    unsigned           scaleMask = 0x0FFFu;
    int                scaleKey  = 0;

    bool is_editable() const {
        return valueType != CursorValueType::READ_ONLY && valueType != CursorValueType::NONE;
    }
};

// ─── Factory ─────────────────────────────────────────────────────────────────────────────────────
// The named constructors the modules build their contexts from.

namespace cc {

inline CursorContext read_only() {
    CursorContext c;
    c.valueType = CursorValueType::READ_ONLY;
    return c;
}

inline CursorContext none() {
    CursorContext c;
    c.valueType = CursorValueType::NONE;
    return c;
}

/** The base every hex-byte-shaped context delegates to. */
inline CursorContext hex_byte(int current, int min = 0, int max = 255, int empty_value = -1,
                              bool can_delete = false, bool can_insert = false,
                              bool can_create = false, int def = NO_DEFAULT) {
    const bool  is_empty = (current == empty_value);
    CursorContext c;
    c.valueType                   = CursorValueType::HEX_BYTE;
    c.capabilities.canIncrement     = !is_empty;
    c.capabilities.canDecrement     = !is_empty;
    c.capabilities.canIncrementFast = !is_empty;
    c.capabilities.canDecrementFast = !is_empty;
    c.capabilities.canDelete        = can_delete && !is_empty;
    c.capabilities.canInsert        = can_insert && is_empty;
    c.capabilities.canCreate        = can_create;
    c.capabilities.isEmpty          = is_empty;
    c.currentValue = current;
    c.minValue     = min;
    c.maxValue     = max;
    c.smallStep    = 1;
    c.largeStep    = 16;
    c.emptyValue   = empty_value;
    c.defaultValue = def;
    return c;
}

/** Phrase reference (00-FF, -1 = empty). An empty cell starts cycling from 0. */
inline CursorContext phrase_ref(int current, bool can_create = true) {
    CursorContext c = hex_byte(current == -1 ? 0 : current, 0, 255, /*empty_value=*/-1,
                               /*can_delete=*/true, /*can_insert=*/true, can_create);
    c.valueType     = CursorValueType::PHRASE_REF;
    return c;
}

/** Chain reference (00-FF, -1 = empty). */
inline CursorContext chain_ref(int current, bool can_create = true) {
    CursorContext c = hex_byte(current == -1 ? 0 : current, 0, 255, /*empty_value=*/-1,
                               /*can_delete=*/true, /*can_insert=*/true, can_create);
    c.valueType     = CursorValueType::CHAIN_REF;
    return c;
}

/** Transpose (00-FF, centred at 0x80). Small step 1 semitone, large step an octave. */
inline CursorContext transpose(int current, bool is_empty = false, int def = NO_DEFAULT) {
    CursorContext c;
    c.valueType                     = CursorValueType::SEMITONE_OFFSET;
    c.capabilities.canIncrement     = !is_empty;
    c.capabilities.canDecrement     = !is_empty;
    c.capabilities.canIncrementFast = !is_empty;
    c.capabilities.canDecrementFast = !is_empty;
    c.capabilities.isEmpty          = is_empty;
    c.currentValue = current;
    c.minValue     = 0;
    c.maxValue     = 255;
    c.smallStep    = 1;
    c.largeStep    = 12;
    c.emptyValue   = 0x80;  // display value only; 0x00 is no-transpose (two's-complement)
    c.defaultValue = is_empty ? NO_DEFAULT : def;
    return c;
}

/**
 * A musical note, C-0 (midi 12) to G-9 (midi 127), C-4 = midi 60. A on an empty note inserts C-4;
 * A+B deletes it. `scale_mask` / `scale_key` limit what can be typed; the default is chromatic, which
 * ROOT (a tuning reference, not a song note) relies on.
 */
inline CursorContext note(int current, bool is_empty = false, unsigned scale_mask = 0x0FFFu,
                          int scale_key = 0) {
    CursorContext c;
    c.valueType                     = CursorValueType::NOTE;
    c.capabilities.canIncrement     = !is_empty;
    c.capabilities.canDecrement     = !is_empty;
    c.capabilities.canIncrementFast = !is_empty;  // +12 = octave up
    c.capabilities.canDecrementFast = !is_empty;  // -12 = octave down
    c.capabilities.canDelete        = !is_empty;
    c.capabilities.canInsert        = is_empty;
    c.capabilities.isEmpty          = is_empty;
    c.currentValue = current;
    c.minValue     = 12;
    c.maxValue     = 127;
    c.smallStep    = 1;
    c.largeStep    = 12;
    c.emptyValue   = -1;
    c.scaleMask    = (scale_mask & 0x0FFFu) == 0 ? 0x0FFFu : (scale_mask & 0x0FFFu);
    c.scaleKey     = scale_key;
    return c;
}

/** Step velocity, the phrase V column (MIDI velocity 0x00-0x7F). */
inline CursorContext volume(int current) {
    CursorContext c = hex_byte(current, 0, 127, /*empty_value=*/-1, false, false, false,
                               /*def=*/0x7F);
    c.valueType     = CursorValueType::VOLUME;
    return c;
}

/** Instrument reference (00-7F). */
inline CursorContext instrument(int current) {
    CursorContext c = hex_byte(current, 0, 127);
    c.valueType     = CursorValueType::INSTRUMENT_REF;
    return c;
}

/**
 * A single hex digit, 0..F — CRUSH and DWNSMPL.
 * ⚠️ CLAMPS where a hex byte wraps: holding A+RIGHT on CRUSH stops at F instead of snapping to 0 and
 * undoing the destruction being dialled in. Large step 4.
 */
inline CursorContext hex_nibble(int current, int def = NO_DEFAULT) {
    CursorContext c;
    c.valueType                     = CursorValueType::HEX_NIBBLE;
    c.capabilities.canIncrement     = true;
    c.capabilities.canDecrement     = true;
    c.capabilities.canIncrementFast = true;
    c.capabilities.canDecrementFast = true;
    c.capabilities.isEmpty          = false;   // a nibble is never empty
    c.currentValue = current & 0x0F;
    c.minValue     = 0;
    c.maxValue     = 15;
    c.smallStep    = 1;
    c.largeStep    = 4;
    c.emptyValue   = -1;   // unused
    c.defaultValue = def;
    return c;
}

/**
 * An EQ band's GAIN — stored 0..240, read as −12.0..+12.0 dB; small step 0.1 dB, fast step 1.0 dB
 * (`largeStep` 10).
 * ⚠️ CLAMPS: wrapping +12 dB to −12 dB would turn one more dB of boost into a full cut.
 */
inline CursorContext gain_db(int current, int def = 120) {
    CursorContext c;
    c.valueType                     = CursorValueType::GAIN;
    c.capabilities.canIncrement     = true;
    c.capabilities.canDecrement     = true;
    c.capabilities.canIncrementFast = true;   // +1.0 dB
    c.capabilities.canDecrementFast = true;   // −1.0 dB
    c.currentValue = current;
    c.minValue     = 0;
    c.maxValue     = 240;
    c.smallStep    = 1;    // 0.1 dB
    c.largeStep    = 10;   // 1.0 dB
    c.emptyValue   = -1;
    c.defaultValue = def;
    return c;
}

/**
 * An EQ band's FREQUENCY — stored 0..255, log over 20 Hz..20 kHz; clamps like GAIN.
 * The context is a plain ±1 / ±16 stepper; the module steps display-aware (until the READOUT changes)
 * in `EqModule::step_freq_display_aware` — a property of the field, measured separately.
 */
inline CursorContext freq(int current, int def = NO_DEFAULT) {
    CursorContext c;
    c.valueType                     = CursorValueType::FREQ;
    c.capabilities.canIncrement     = true;
    c.capabilities.canDecrement     = true;
    c.capabilities.canIncrementFast = true;
    c.capabilities.canDecrementFast = true;
    c.currentValue = current;
    c.minValue     = 0;
    c.maxValue     = 255;
    c.smallStep    = 1;
    c.largeStep    = 16;
    c.emptyValue   = -1;
    c.defaultValue = def;
    return c;
}

/** An on/off flag — REVERSE. Wraps, so one button cycles it. */
inline CursorContext toggle_binary(bool current) {
    CursorContext c;
    c.valueType                 = CursorValueType::TOGGLE_BINARY;
    c.capabilities.canIncrement = true;
    c.capabilities.canDecrement = true;
    c.currentValue = current ? 1 : 0;
    c.minValue     = 0;
    c.maxValue     = 1;
    c.smallStep    = 1;
    c.largeStep    = 1;
    c.emptyValue   = -1;
    return c;
}

/**
 * An N-state cycle stored as a STRING in the model — SLICE (OFF/CUT/TRU), FILTER (off/lp/hp/bp), LOOP.
 * The context carries the INDEX; the module maps it back on write. An unknown string reads as index 0
 * (a junk filterType shows "off", not a blank). Any number of options, despite the name.
 */
inline CursorContext toggle_ternary(const std::string& current,
                                    const std::vector<std::string>& options) {
    int index = 0;
    for (size_t i = 0; i < options.size(); ++i)
        if (options[i] == current) { index = static_cast<int>(i); break; }

    CursorContext c;
    c.valueType                 = CursorValueType::TOGGLE_TERNARY;
    c.capabilities.canIncrement = true;
    c.capabilities.canDecrement = true;
    c.currentValue = index;
    c.minValue     = 0;
    c.maxValue     = static_cast<int>(options.size()) - 1;
    c.smallStep    = 1;
    c.largeStep    = 1;
    c.emptyValue   = -1;
    return c;
}

/**
 * A cycle through a list stored as an INDEX — the MODS screen's TYPE, DEST, OSC and TRIG rows. Uses
 * EFFECT_TYPE because it wraps with a large step of 1.
 */
inline CursorContext index_cycle(int current, int count) {
    CursorContext c;
    c.valueType                 = CursorValueType::EFFECT_TYPE;
    c.capabilities.canIncrement = true;
    c.capabilities.canDecrement = true;
    c.currentValue = current < 0 ? 0 : current;
    c.minValue     = 0;
    c.maxValue     = count - 1;
    c.smallStep    = 1;
    c.largeStep    = 1;
    return c;
}

/**
 * A cycle through an option LIST — SETTINGS' LAYOUT, OVERLAY and VISUALIZER rows.
 * ⚠️ Not `index_cycle`, though they behave identically: this is a HEX_BYTE with emptyValue −1, and
 * The input tests byte-compare the context — merging them turns the settings golden red.
 * `option_count` clamps at 0, so an empty list is a 0..0 cycle.
 */
inline CursorContext enum_cycle(int current, int option_count) {
    CursorContext c;
    c.valueType                 = CursorValueType::HEX_BYTE;
    c.capabilities.canIncrement = true;
    c.capabilities.canDecrement = true;
    c.currentValue = current < 0 ? 0 : current;
    c.minValue     = 0;
    c.maxValue     = option_count - 1 < 0 ? 0 : option_count - 1;
    c.smallStep    = 1;
    c.largeStep    = 1;
    c.emptyValue   = -1;
    return c;
}

/**
 * One character of an in-place name editor (PROJECT's NAME row, one column per character); A+LEFT/RIGHT
 * walks `allowed_chars()`. A+B writes a space — a character is never empty, so delete is always live.
 */
inline CursorContext character(char current) {
    CursorContext c;
    c.valueType                 = CursorValueType::CHARACTER;
    c.capabilities.canIncrement = true;
    c.capabilities.canDecrement = true;
    c.capabilities.canDelete    = true;
    c.currentValue = static_cast<int>(static_cast<unsigned char>(current));
    c.minValue     = 0;   // unused for CHARACTER
    c.maxValue     = 0;   // unused for CHARACTER
    c.smallStep    = 1;
    c.largeStep    = 1;   // no fast step — the set is short
    c.emptyValue   = static_cast<int>('_');
    return c;
}

/**
 * Effect type: the value is an INDEX into songcore::EFFECT_TYPES, not a code; the module converts on
 * write. A+B clears it unless already NONE (a valid stop on the cycle, not "empty").
 * `type_count` is how much of the list this build reaches (the MIDI tail may be hidden,
 * ui/platform_caps.h). ⚠️ It clamps the CURSOR, never the CELL: a step holding a hidden effect keeps and
 * draws it — the value came off disk.
 */
inline CursorContext effect_type(int current_type, int fx_slot,
                                 int type_count = songcore::EFFECT_TYPE_COUNT) {
    CursorContext c;
    c.valueType                 = CursorValueType::EFFECT_TYPE;
    c.capabilities.canIncrement = true;
    c.capabilities.canDecrement = true;
    c.capabilities.canDelete    = (current_type != songcore::FX_NONE);
    c.capabilities.isEmpty      = false;
    c.currentValue = songcore::effect_type_index(current_type);
    c.minValue     = 0;
    c.maxValue     = type_count - 1;
    c.smallStep    = 1;
    c.fxSlot       = fx_slot;
    return c;
}

/** Effect parameter byte. `max` comes from songcore::effect_value_max(type). */
inline CursorContext effect_value(int current, int fx_slot, int max = 255) {
    CursorContext c;
    c.valueType                     = CursorValueType::EFFECT_VALUE;
    c.capabilities.canIncrement     = true;
    c.capabilities.canDecrement     = true;
    c.capabilities.canIncrementFast = true;
    c.capabilities.canDecrementFast = true;
    c.currentValue = current;
    c.minValue     = 0;
    c.maxValue     = max;
    c.smallStep    = 1;
    c.largeStep    = 16;
    c.fxSlot       = fx_slot;
    return c;
}

}  // namespace cc

// ─── Actions ─────────────────────────────────────────────────────────────────────────────────────
// Only SET_VALUE carries a payload, so a tagged struct rather than a variant.

enum class ActionType {
    NONE,
    SET_VALUE,
    DELETE,
    INSERT_DEFAULT,
    CREATE_NEW,
    NAVIGATE_UP,
    NAVIGATE_DOWN,
    NAVIGATE_LEFT,
    NAVIGATE_RIGHT,
    COPY,
    CUT,
    PASTE
};

struct InputAction {
    ActionType type  = ActionType::NONE;
    int        value = 0;  // SET_VALUE only

    static InputAction none() { return {}; }
    static InputAction set_value(int v) { return InputAction{ActionType::SET_VALUE, v}; }
    static InputAction of(ActionType t) { return InputAction{t, 0}; }
};

// ─── Stepping ────────────────────────────────────────────────────────────────────────────────────

/** The character cycle A→Z→0→9→_→-→space, used by the in-place name editors. */
inline const std::string& allowed_chars() {
    static const std::string s = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_- ";
    return s;
}

/** Is this MIDI note one the context's scale allows? Bit 0 of the mask is pitch class `scaleKey`. */
inline bool note_in_scale_mask(const CursorContext& ctx, int midi) {
    if (midi < 0) return false;
    const int degree = ((midi - ctx.scaleKey) % 12 + 12) % 12;
    return (ctx.scaleMask >> degree) & 1u;
}

/**
 * The nearest allowed note at or above `midi`, wrapping down within the octave if the search runs off
 * the top. What A on an empty note cell inserts: C-4, unless C is not in the scale.
 */
inline int snap_note_to_scale(const CursorContext& ctx, int midi) {
    for (int d = 0; d < 12; ++d) {
        if (midi + d <= ctx.maxValue && note_in_scale_mask(ctx, midi + d)) return midi + d;
        if (d != 0 && midi - d >= ctx.minValue && note_in_scale_mask(ctx, midi - d)) return midi - d;
    }
    return midi;
}

/**
 * Apply a signed step, honouring the type's rule: discrete, enumerable types WRAP (past FF lands on
 * 00 — dialling on a four-button device); continuous physical units (GAIN, FREQ) CLAMP; NOTE clamps at
 * the MIDI ceiling, and so does anything not listed.
 */
inline int step_value(int current, int signed_step, const CursorContext& ctx) {
    switch (ctx.valueType) {
        case CursorValueType::CHARACTER: {
            const std::string& set  = allowed_chars();
            const int          size = static_cast<int>(set.size());
            const auto         pos  = set.find(static_cast<char>(current));
            if (pos == std::string::npos) {
                return signed_step >= 0 ? set.front() : set.back();
            }
            const int idx = (static_cast<int>(pos) + signed_step) % size;
            return set[static_cast<size_t>((idx + size) % size)];
        }

        case CursorValueType::PHRASE_REF:
        case CursorValueType::CHAIN_REF:
        case CursorValueType::HEX_BYTE:
        case CursorValueType::SEMITONE_OFFSET:
        case CursorValueType::VOLUME:
        case CursorValueType::EFFECT_TYPE:
        case CursorValueType::EFFECT_VALUE:
        case CursorValueType::INSTRUMENT_REF:
        case CursorValueType::TOGGLE_BINARY:
        case CursorValueType::TOGGLE_TERNARY: {
            // ⚠️ `std::max(1, …)`: an inverted range (a factory handed a zero count) would make both loops
            // spin forever, in the function every editable cell runs through.
            const int range = std::max(1, ctx.maxValue - ctx.minValue + 1);
            int       v     = current + signed_step;
            while (v > ctx.maxValue) v -= range;
            while (v < ctx.minValue) v += range;
            return v;
        }

        case CursorValueType::NOTE: {
            // A+LEFT/RIGHT walks ONE SCALE DEGREE (a semitone on chromatic).
            // ⚠️ A+UP/DOWN (12) is not walked: an octave is in every scale, while twelve degrees of a
            // pentatonic would be two octaves.
            const bool byDegree = (signed_step == ctx.smallStep || signed_step == -ctx.smallStep);
            if (!byDegree || ctx.scaleMask == 0x0FFFu)
                return std::min(ctx.maxValue, std::max(ctx.minValue, current + signed_step));

            const int dir = signed_step > 0 ? 1 : -1;
            for (int v = current + dir; v >= ctx.minValue && v <= ctx.maxValue; v += dir)
                if (note_in_scale_mask(ctx, v)) return v;
            return current;  // no further note in the scale: stay put
        }

        default: {
            const int v = current + signed_step;
            return v < ctx.minValue ? ctx.minValue : (v > ctx.maxValue ? ctx.maxValue : v);
        }
    }
}

// ─── Step → action ───────────────────────────────────────────────────────────────────────────────
// The generic editing vocabulary; it never mentions a screen. Named for the STEP, not the chord: today
// A+RIGHT → `increment`, A+LEFT → `decrement`, A+UP → `increment_fast`, A+DOWN → `decrement_fast`.

/**
 * What A on an empty NOTE cell inserts, as MIDI. ⚠️ Must name the same note as `Note::C4()` in
 * phrase_editor.cpp, which is what the INSERT_DEFAULT actually writes.
 */
inline constexpr int NOTE_INSERT_DEFAULT_MIDI = 60;  // C-4

inline InputAction increment(const CursorContext& c) {
    if (!c.is_editable()) return InputAction::none();
    if (c.capabilities.isEmpty && c.capabilities.canInsert) {
        // ⚠️ A scale without C-4 must not be handed one — no press could produce it or step back to
        // it. Spelled as a VALUE only when the scale moves it, so chromatic still emits INSERT_DEFAULT.
        if (c.valueType == CursorValueType::NOTE && c.scaleMask != 0x0FFFu) {
            const int snapped = snap_note_to_scale(c, NOTE_INSERT_DEFAULT_MIDI);
            if (snapped != NOTE_INSERT_DEFAULT_MIDI) return InputAction::set_value(snapped);
        }
        return InputAction::of(ActionType::INSERT_DEFAULT);
    }
    if (c.capabilities.canIncrement)
        return InputAction::set_value(step_value(c.currentValue, c.smallStep, c));
    return InputAction::none();
}

inline InputAction decrement(const CursorContext& c) {
    if (!c.is_editable() || c.capabilities.isEmpty) return InputAction::none();
    if (c.capabilities.canDecrement)
        return InputAction::set_value(step_value(c.currentValue, -c.smallStep, c));
    return InputAction::none();
}

/**
 * The fast pair falls back to the small step on a cell with none, so a short cycle (presets, SLICE,
 * a flag…) answers A+UP/DOWN like A+RIGHT/LEFT. Derived here so a new factory inherits it.
 */
inline InputAction increment_fast(const CursorContext& c) {
    if (!c.is_editable() || c.capabilities.isEmpty) return InputAction::none();
    if (c.capabilities.canIncrementFast)
        return InputAction::set_value(step_value(c.currentValue, c.largeStep, c));
    if (c.capabilities.canIncrement)
        return InputAction::set_value(step_value(c.currentValue, c.smallStep, c));
    return InputAction::none();
}

inline InputAction decrement_fast(const CursorContext& c) {
    if (!c.is_editable() || c.capabilities.isEmpty) return InputAction::none();
    if (c.capabilities.canDecrementFast)
        return InputAction::set_value(step_value(c.currentValue, -c.largeStep, c));
    if (c.capabilities.canDecrement)
        return InputAction::set_value(step_value(c.currentValue, -c.smallStep, c));
    return InputAction::none();
}

/** A+B: deletable cells clear to empty; others reset to `defaultValue` if they declare one. */
inline InputAction on_a_b(const CursorContext& c) {
    if (c.capabilities.canDelete) return InputAction::of(ActionType::DELETE);
    if (c.defaultValue != NO_DEFAULT && c.is_editable())
        return InputAction::set_value(c.defaultValue);
    return InputAction::none();
}

}  // namespace pt::ui
