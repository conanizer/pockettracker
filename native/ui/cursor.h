#pragma once

// Each module describes the cell under its cursor ("a NOTE, empty, range 12..127, steps 1 and 12"),
// and the generic handlers below turn presses into edits from that alone, on every screen.

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

#include "songcore/effects.h"

namespace pt::ui {

// `defaultValue` for a cell with no A+B reset.
inline constexpr int NO_DEFAULT = std::numeric_limits<int>::min();

// What kind of value the cursor is on; `step_value` says which wrap and which clamp.
enum class CursorValueType {
    HEX_BYTE,         // 00-FF
    HEX_NIBBLE,       // 0-F
    SEMITONE_OFFSET,  // transpose, centred at 0x80

    NOTE,
    VOLUME,

    GAIN,  // EQ gain: 0..240 = −12.0..+12.0 dB
    FREQ,  // EQ frequency: 0..255, log 20 Hz..20 kHz

    PHRASE_REF,
    CHAIN_REF,
    INSTRUMENT_REF,

    CHARACTER,  // from `allowed_chars()`

    TOGGLE_BINARY,
    TOGGLE_TERNARY,

    EFFECT_TYPE,   // an index into songcore::EFFECT_TYPES, not an effect code
    EFFECT_VALUE,  // 00-FF

    EMPTY,
    READ_ONLY,  // step numbers
    NONE        // no cursor
};

// Which button combinations do anything here.
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

// What the cursor is on, and what can be done to it.
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

    // NOTE cells: the pitch classes that may be typed, bit 0 = `scaleKey`; 0x0FFF is chromatic.
    // On the context because stepping must move a whole degree.
    unsigned           scaleMask = 0x0FFFu;
    int                scaleKey  = 0;

    bool is_editable() const {
        return valueType != CursorValueType::READ_ONLY && valueType != CursorValueType::NONE;
    }
};

// ─── The named contexts the modules build from ───────────────────────────────────────────────────

namespace cc {

// A+LEFT/RIGHT, and with `fast` A+UP/DOWN too.
inline void allow_steps(CursorCapabilities& k, bool on, bool fast = true) {
    k.canIncrement     = k.canDecrement     = on;
    k.canIncrementFast = k.canDecrementFast = on && fast;
}

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

// The base every hex-byte-shaped context delegates to.
inline CursorContext hex_byte(int current, int min = 0, int max = 255, int empty_value = -1,
                              bool can_delete = false, bool can_insert = false,
                              bool can_create = false, int def = NO_DEFAULT) {
    const bool  is_empty = (current == empty_value);
    CursorContext c;
    c.valueType                   = CursorValueType::HEX_BYTE;
    allow_steps(c.capabilities, !is_empty);
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

// A plain 00..FF byte with an A+B reset value.
inline CursorContext plain_byte(int current, int def) {
    return hex_byte(current, 0, 255, -1, false, false, false, def);
}

// An EQ slot, −1 = none: A inserts slot 0, A+B clears.
inline CursorContext eq_slot(int slot) {
    return hex_byte(slot < 0 ? -1 : slot, 0, 127, /*empty_value=*/-1, /*can_delete=*/true,
                    /*can_insert=*/true);
}

// Phrase reference (-1 = empty); an empty cell cycles from 0.
inline CursorContext phrase_ref(int current, bool can_create = true) {
    CursorContext c = hex_byte(current == -1 ? 0 : current, 0, 255, /*empty_value=*/-1,
                               /*can_delete=*/true, /*can_insert=*/true, can_create);
    c.valueType     = CursorValueType::PHRASE_REF;
    return c;
}

// Chain reference (-1 = empty).
inline CursorContext chain_ref(int current, bool can_create = true) {
    CursorContext c = hex_byte(current == -1 ? 0 : current, 0, 255, /*empty_value=*/-1,
                               /*can_delete=*/true, /*can_insert=*/true, can_create);
    c.valueType     = CursorValueType::CHAIN_REF;
    return c;
}

// Transpose: a semitone, or an octave.
inline CursorContext transpose(int current, bool is_empty = false, int def = NO_DEFAULT) {
    CursorContext c;
    c.valueType                     = CursorValueType::SEMITONE_OFFSET;
    allow_steps(c.capabilities, !is_empty);
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

// A note, C-0 (midi 12) to G-9 (127). `scale_mask`/`scale_key` limit what can be typed; ROOT relies on
// the chromatic default.
inline CursorContext note(int current, bool is_empty = false, unsigned scale_mask = 0x0FFFu,
                          int scale_key = 0) {
    CursorContext c;
    c.valueType                     = CursorValueType::NOTE;
    allow_steps(c.capabilities, !is_empty);
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

// The phrase's V column, 00-7F.
inline CursorContext volume(int current) {
    CursorContext c = hex_byte(current, 0, 127, /*empty_value=*/-1, false, false, false,
                               /*def=*/0x7F);
    c.valueType     = CursorValueType::VOLUME;
    return c;
}

// Instrument reference, 00-7F.
inline CursorContext instrument(int current) {
    CursorContext c = hex_byte(current, 0, 127);
    c.valueType     = CursorValueType::INSTRUMENT_REF;
    return c;
}

// One hex digit (CRUSH, DWNSMPL). Clamps rather than wraps: a held A+RIGHT stops at F.
inline CursorContext hex_nibble(int current, int def = NO_DEFAULT) {
    CursorContext c;
    c.valueType                     = CursorValueType::HEX_NIBBLE;
    allow_steps(c.capabilities, true);
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

// An EQ band's gain. Clamps: a wrap would turn one more dB of boost into a full cut.
inline CursorContext gain_db(int current, int def = 120) {
    CursorContext c;
    c.valueType                     = CursorValueType::GAIN;
    allow_steps(c.capabilities, true);
    c.currentValue = current;
    c.minValue     = 0;
    c.maxValue     = 240;
    c.smallStep    = 1;    // 0.1 dB
    c.largeStep    = 10;   // 1.0 dB
    c.emptyValue   = -1;
    c.defaultValue = def;
    return c;
}

// An EQ band's frequency; clamps. The EQ module steps it until the readout changes.
inline CursorContext freq(int current, int def = NO_DEFAULT) {
    CursorContext c;
    c.valueType                     = CursorValueType::FREQ;
    allow_steps(c.capabilities, true);
    c.currentValue = current;
    c.minValue     = 0;
    c.maxValue     = 255;
    c.smallStep    = 1;
    c.largeStep    = 16;
    c.emptyValue   = -1;
    c.defaultValue = def;
    return c;
}

// An on/off flag; wraps, so one button cycles it.
inline CursorContext toggle_binary(bool current) {
    CursorContext c;
    c.valueType                 = CursorValueType::TOGGLE_BINARY;
    allow_steps(c.capabilities, true, /*fast=*/false);
    c.currentValue = current ? 1 : 0;
    c.minValue     = 0;
    c.maxValue     = 1;
    c.smallStep    = 1;
    c.largeStep    = 1;
    c.emptyValue   = -1;
    return c;
}

// A cycle stored as a string in the model (SLICE, FILTER, LOOP); the context carries the index, and an
// unknown string reads as 0. Any number of options, despite the name.
inline CursorContext toggle_ternary(const std::string& current,
                                    const std::vector<std::string>& options) {
    int index = 0;
    for (size_t i = 0; i < options.size(); ++i)
        if (options[i] == current) { index = static_cast<int>(i); break; }

    CursorContext c;
    c.valueType                 = CursorValueType::TOGGLE_TERNARY;
    allow_steps(c.capabilities, true, /*fast=*/false);
    c.currentValue = index;
    c.minValue     = 0;
    c.maxValue     = static_cast<int>(options.size()) - 1;
    c.smallStep    = 1;
    c.largeStep    = 1;
    c.emptyValue   = -1;
    return c;
}

// A cycle through a list stored as an index (MODS' TYPE, DEST, OSC, TRIG). EFFECT_TYPE for its wrap.
inline CursorContext index_cycle(int current, int count) {
    CursorContext c;
    c.valueType                 = CursorValueType::EFFECT_TYPE;
    allow_steps(c.capabilities, true, /*fast=*/false);
    c.currentValue = current < 0 ? 0 : current;
    c.minValue     = 0;
    c.maxValue     = count - 1;
    c.smallStep    = 1;
    c.largeStep    = 1;
    return c;
}

// A cycle through an option list (SETTINGS' LAYOUT, OVERLAY, VISUALIZER). Behaves as `index_cycle`,
// but the input golden pins this shape. An empty list is a 0..0 cycle.
inline CursorContext enum_cycle(int current, int option_count) {
    CursorContext c;
    c.valueType                 = CursorValueType::HEX_BYTE;
    allow_steps(c.capabilities, true, /*fast=*/false);
    c.currentValue = current < 0 ? 0 : current;
    c.minValue     = 0;
    c.maxValue     = option_count - 1 < 0 ? 0 : option_count - 1;
    c.smallStep    = 1;
    c.largeStep    = 1;
    c.emptyValue   = -1;
    return c;
}

// One character of an in-place name; A+LEFT/RIGHT walks `allowed_chars()`, A+B writes a space.
inline CursorContext character(char current) {
    CursorContext c;
    c.valueType                 = CursorValueType::CHARACTER;
    allow_steps(c.capabilities, true, /*fast=*/false);
    c.capabilities.canDelete    = true;
    c.currentValue = static_cast<int>(static_cast<unsigned char>(current));
    c.minValue     = 0;   // unused
    c.maxValue     = 0;   // unused
    c.smallStep    = 1;
    c.largeStep    = 1;
    c.emptyValue   = static_cast<int>('_');
    return c;
}

// Effect type, as an index into EFFECT_TYPES (the module converts on write). `type_count` is what this
// build reaches; it bounds the stepping, never the cell — a hidden effect read off disk stays.
inline CursorContext effect_type(int current_type, int fx_slot,
                                 int type_count = songcore::EFFECT_TYPE_COUNT) {
    CursorContext c;
    c.valueType                 = CursorValueType::EFFECT_TYPE;
    allow_steps(c.capabilities, true, /*fast=*/false);
    c.capabilities.canDelete    = (current_type != songcore::FX_NONE);
    c.capabilities.isEmpty      = false;
    c.currentValue = songcore::effect_type_index(current_type);
    c.minValue     = 0;
    c.maxValue     = type_count - 1;
    c.smallStep    = 1;
    c.fxSlot       = fx_slot;
    return c;
}

// Effect value; `max` from songcore::effect_value_max(type).
inline CursorContext effect_value(int current, int fx_slot, int max = 255) {
    CursorContext c;
    c.valueType                     = CursorValueType::EFFECT_VALUE;
    allow_steps(c.capabilities, true);
    c.currentValue = current;
    c.minValue     = 0;
    c.maxValue     = max;
    c.smallStep    = 1;
    c.largeStep    = 16;
    c.defaultValue = 0;
    c.fxSlot       = fx_slot;
    return c;
}

}  // namespace cc

// ─── Actions ─────────────────────────────────────────────────────────────────────────────────────

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

// A byte that can be −1: SET writes it (clamped to 0..max), DELETE clears it, INSERT lands on 0.
// False for any other action.
inline bool apply_optional_byte(int& field, int max, const InputAction& a) {
    switch (a.type) {
        case ActionType::SET_VALUE:      field = a.value < 0 ? 0 : (a.value > max ? max : a.value); return true;
        case ActionType::DELETE:         field = -1; return true;
        case ActionType::INSERT_DEFAULT: field = 0;  return true;
        default:                         return false;
    }
}

// ─── Stepping ────────────────────────────────────────────────────────────────────────────────────

// The in-place name editors' character cycle.
inline const std::string& allowed_chars() {
    static const std::string s = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_- ";
    return s;
}

// Does the context's scale allow this note?
inline bool note_in_scale_mask(const CursorContext& ctx, int midi) {
    if (midi < 0) return false;
    const int degree = ((midi - ctx.scaleKey) % 12 + 12) % 12;
    return (ctx.scaleMask >> degree) & 1u;
}

// The nearest allowed note to `midi`, upward first.
inline int snap_note_to_scale(const CursorContext& ctx, int midi) {
    for (int d = 0; d < 12; ++d) {
        if (midi + d <= ctx.maxValue && note_in_scale_mask(ctx, midi + d)) return midi + d;
        if (d != 0 && midi - d >= ctx.minValue && note_in_scale_mask(ctx, midi - d)) return midi - d;
    }
    return midi;
}

// Apply a signed step: enumerable types wrap, NOTE and the physical units (GAIN, FREQ) clamp.
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
            // ⚠️ max(1, …): an inverted range would spin these loops forever.
            const int range = std::max(1, ctx.maxValue - ctx.minValue + 1);
            int       v     = current + signed_step;
            while (v > ctx.maxValue) v -= range;
            while (v < ctx.minValue) v += range;
            return v;
        }

        case CursorValueType::NOTE: {
            // A+LEFT/RIGHT walk one scale degree; A+UP/DOWN stay an octave.
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

// ─── Step → action: the generic handlers, named for the step (A+RIGHT is `increment`, …) ─────────

// ⚠️ The note INSERT_DEFAULT writes (`Note::C4()` in phrase_editor.cpp).
inline constexpr int NOTE_INSERT_DEFAULT_MIDI = 60;  // C-4

inline InputAction increment(const CursorContext& c) {
    if (!c.is_editable()) return InputAction::none();
    if (c.capabilities.isEmpty && c.capabilities.canInsert) {
        // A scale without C-4 gets its nearest note instead.
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

// The fast pair falls back to the small step on a cell with no fast step (a short cycle).
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

// A+B: clear a deletable cell, else reset to `defaultValue` if it has one.
inline InputAction on_a_b(const CursorContext& c) {
    if (c.capabilities.canDelete) return InputAction::of(ActionType::DELETE);
    if (c.defaultValue != NO_DEFAULT && c.is_editable())
        return InputAction::set_value(c.defaultValue);
    return InputAction::none();
}

}  // namespace pt::ui
