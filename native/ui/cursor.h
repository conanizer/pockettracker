#pragma once

// ─── The cursor-context system ───────────────────────────────────────────────────────────────────
//
// A 1:1 port of input/CursorContext.kt and the value-stepping half of core/logic/InputController.kt.
//
// This is the idea the whole input layer rests on, and it is worth restating: the app does NOT ask
// "which screen am I on?" to decide what a button does. Each module answers `cursor_context(state)`
// — "the cursor is on a NOTE, it is empty, it can be inserted, its range is 12..127, a small step is
// 1 and a large step is 12" — and one generic handler turns button presses into InputActions from
// that answer alone. Every screen therefore gets increment/decrement/fast-step/delete/insert for
// free, and a new column is a new `case` in one function rather than a new branch in the dispatcher.
//
// The selection machinery (CELL/ROW/SCREEN multi-tap, expand, clipboard) is the other half of
// InputController and lands with the dispatcher; only the parts a single editable cell needs are here.

#include <limits>
#include <string>
#include <vector>

#include "songcore/effects.h"

namespace pt::ui {

/** Sentinel for `default_value`: this cell has no A+B "reset to default" target. */
inline constexpr int NO_DEFAULT = std::numeric_limits<int>::min();

/**
 * What kind of value is the cursor on? Each type steps differently — see `step_value`, where the
 * wrapping set and the clamping set part ways.
 */
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
    bool canIncrement     = false;  // A
    bool canDecrement     = false;  // B
    bool canIncrementFast = false;  // A+RIGHT
    bool canDecrementFast = false;  // A+LEFT
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
    int                smallStep    = 1;   // A / B
    int                largeStep    = 16;  // A+LEFT / A+RIGHT
    int                emptyValue   = 0xFF;
    int                fxSlot       = 0;  // for effects: which FX slot (1, 2, 3)
    int                defaultValue = NO_DEFAULT;  // A+B resets a non-deletable value to this

    bool is_editable() const {
        return valueType != CursorValueType::READ_ONLY && valueType != CursorValueType::NONE;
    }
};

// ─── Factory ─────────────────────────────────────────────────────────────────────────────────────
// CursorContextFactory — the named constructors the modules build their contexts from.

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
    c.emptyValue   = 0x80;  // vestigial display value; 0x00 is no-transpose (two's-complement)
    c.defaultValue = is_empty ? NO_DEFAULT : def;
    return c;
}

/**
 * A musical note. Range C-0 (midi 12) to G-9 (midi 127), keeping C-4 = middle C = midi 60.
 * A on an empty note inserts C-4; A+B deletes it.
 */
inline CursorContext note(int current, bool is_empty = false) {
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
 * A single hex digit, 0..F — CRUSH and DWNSMPL on the INSTRUMENT screen.
 *
 * ⚠️ It CLAMPS, where a hex byte wraps: HEX_NIBBLE is not in `step_value`'s wrapping set, on either
 * side of the port. Holding A+UP on CRUSH stops at F rather than snapping back to 0 and quietly
 * undoing the destruction you were dialling in. Large step is 4 — a quarter of the range.
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
 * An N-state cycle stored as a STRING in the model — SLICE (OFF/CUT/TRU), FILTER (off/lp/hp/bp),
 * LOOP (off/fwd/png). The context carries the INDEX; the module maps it back to the string when it
 * writes. An unrecognised value reads as index 0, which is Kotlin's `indexOf(...).coerceAtLeast(0)`
 * and the reason a project with a junk filterType shows "off" rather than a blank cell.
 *
 * (Named "ternary" after its first user; it takes any number of options — `filterType` has four.)
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
 * A cycle through a list the model stores as an INDEX already — the MODS screen's TYPE, DEST, OSC and
 * TRIG rows. Kotlin builds these four inline as raw `CursorContext(valueType = EFFECT_TYPE, …)`
 * literals rather than through a factory; the EFFECT_TYPE type is reused purely because it is the one
 * that wraps with a large step of 1. Named here so the intent survives the port.
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
 * A cycle through an option LIST — SETTINGS' LAYOUT, OVERLAY and VISUALIZER rows. Kotlin's
 * `CursorContextFactory.enumCycle`.
 *
 * ⚠️ NOT the same context as `index_cycle` above, however interchangeable the two look. Kotlin's
 * enumCycle is a **HEX_BYTE with emptyValue = −1**; the MODS rows index_cycle serves are inline
 * EFFECT_TYPE literals that keep the struct's own emptyValue. The BEHAVIOUR is identical — both value
 * types wrap, neither carries a delete, and neither is ever empty — but the CONTEXT is not, and
 * ptinput byte-compares the context. Folding these two into one factory is a one-line "cleanup" that
 * turns the settings golden red.
 *
 * `optionCount` is clamped the way Kotlin clamps it (`coerceAtLeast(0)`), so an empty option list
 * yields a 0..0 cycle rather than a 0..−1 one.
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
 * One character of an in-place name editor — PROJECT's NAME row, where each of the 20 characters is
 * its own cursor column and A+UP/DOWN walks the allowed set (`allowed_chars()`).
 *
 * The first user of CursorValueType::CHARACTER in the port: the stepping has been here since S1 (it
 * came with the value type), but no ported screen had an in-place text cell until PROJECT — the
 * INSTRUMENT NAME row opens the QWERTY keyboard instead. A+B DELETES, which for a character means
 * writing a space; there is no "empty" character, so `isEmpty` stays false and the delete arm is
 * always live.
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
 * Effect type. The stored value is an INDEX into songcore::EFFECT_TYPES, not an effect code — A+UP
 * walks the list, and the module converts back to a code when it writes the step.
 * A+B clears the effect, but only when it is not already NONE (FX_NONE is a valid stop on the cycle,
 * not an "empty" cell).
 */
inline CursorContext effect_type(int current_type, int fx_slot) {
    CursorContext c;
    c.valueType                 = CursorValueType::EFFECT_TYPE;
    c.capabilities.canIncrement = true;
    c.capabilities.canDecrement = true;
    c.capabilities.canDelete    = (current_type != songcore::FX_NONE);
    c.capabilities.isEmpty      = false;
    c.currentValue = songcore::effect_type_index(current_type);
    c.minValue     = 0;
    c.maxValue     = songcore::EFFECT_TYPE_COUNT - 1;
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
// Kotlin's `sealed class InputAction`. Only SET_VALUE carries a payload, so a tagged struct is the
// honest C++ shape — a variant would buy nothing and cost every call site a `std::get`.

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

/**
 * Apply a signed step to the cursor value, honouring the value type's wrap/clamp rules.
 *
 * The split matters and is not arbitrary: the discrete, enumerable types WRAP (stepping past FF on a
 * hex byte lands back on 00, which is what makes A+UP a usable way to dial a value in on a
 * four-button device), while the continuous physical-unit types — GAIN in dB, FREQ in Hz — CLAMP,
 * because wrapping +12 dB round to −12 dB would be a trap rather than a convenience. NOTE clamps too
 * (its range is the MIDI ceiling), and so does anything not named here.
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
            const int range = ctx.maxValue - ctx.minValue + 1;
            int       v     = current + signed_step;
            while (v > ctx.maxValue) v -= range;
            while (v < ctx.minValue) v += range;
            return v;
        }

        default: {
            const int v = current + signed_step;
            return v < ctx.minValue ? ctx.minValue : (v > ctx.maxValue ? ctx.maxValue : v);
        }
    }
}

// ─── Button → action ─────────────────────────────────────────────────────────────────────────────
// The generic half of InputController: these five functions are the ENTIRE editing vocabulary, and
// they never mention a screen.

inline InputAction on_a(const CursorContext& c) {
    if (!c.is_editable()) return InputAction::none();
    if (c.capabilities.isEmpty && c.capabilities.canInsert)
        return InputAction::of(ActionType::INSERT_DEFAULT);
    if (c.capabilities.canIncrement)
        return InputAction::set_value(step_value(c.currentValue, c.smallStep, c));
    return InputAction::none();
}

inline InputAction on_b(const CursorContext& c) {
    if (!c.is_editable() || c.capabilities.isEmpty) return InputAction::none();
    if (c.capabilities.canDecrement)
        return InputAction::set_value(step_value(c.currentValue, -c.smallStep, c));
    return InputAction::none();
}

inline InputAction on_a_right(const CursorContext& c) {
    if (!c.is_editable() || c.capabilities.isEmpty) return InputAction::none();
    if (c.capabilities.canIncrementFast)
        return InputAction::set_value(step_value(c.currentValue, c.largeStep, c));
    return InputAction::none();
}

inline InputAction on_a_left(const CursorContext& c) {
    if (!c.is_editable() || c.capabilities.isEmpty) return InputAction::none();
    if (c.capabilities.canDecrementFast)
        return InputAction::set_value(step_value(c.currentValue, -c.largeStep, c));
    return InputAction::none();
}

/**
 * A+B. Deletable cells clear to empty; non-deletable cells that declare a default (PAN, DRIVE, VOL,
 * the sends…) reset to it, which is why `defaultValue` exists at all.
 */
inline InputAction on_a_b(const CursorContext& c) {
    if (c.capabilities.canDelete) return InputAction::of(ActionType::DELETE);
    if (c.defaultValue != NO_DEFAULT && c.is_editable())
        return InputAction::set_value(c.defaultValue);
    return InputAction::none();
}

}  // namespace pt::ui
