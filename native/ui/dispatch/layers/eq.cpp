// The EQ EDITOR: the three bands of one EQ slot, raised from any cell that names a slot — an
// instrument's, the master's, the two send inputs', the sample editor's EQ effect. It owns the buttons
// while it is up, but lets START through (the song plays while you dial) and SELECT (its cells have help).
// ⚠️ `currentScreen` still names the screen underneath, so nothing here may read the screen's cursor.

#include "ui/dispatch/dispatch_common.h"

#include <algorithm>

namespace pt::ui {

LayerResult InputDispatcher::eq_layer(Gesture g) {
    switch (g) {
        // UP/DOWN walk the band's parameters. ⚠️ LEFT/RIGHT change BAND keeping the PARAM, so one
        // parameter sweeps across all three bands.
        case Gesture::DPAD_UP:    eq_move_cursor(0, -1); break;
        case Gesture::DPAD_DOWN:  eq_move_cursor(0, +1); break;
        case Gesture::DPAD_LEFT:  eq_move_cursor(-1, 0); break;
        case Gesture::DPAD_RIGHT: eq_move_cursor(+1, 0); break;

        case Gesture::A_UP:    eq_edit(pt::ui::increment_fast); break;
        case Gesture::A_DOWN:  eq_edit(pt::ui::decrement_fast); break;
        case Gesture::A_LEFT:  eq_edit(pt::ui::decrement); break;
        case Gesture::A_RIGHT: eq_edit(pt::ui::increment); break;

        // A+B resets the param under the cursor: FREQ 0x80 (≈450 Hz), GAIN 120 (0 dB), Q 0x80. TYPE has
        // no default.
        case Gesture::A_B: eq_edit(pt::ui::on_a_b); break;

        // ⚠️ B closes on its RELEASE (`defer_b_to_release`), so a B+LEFT/RIGHT can still claim it.
        case Gesture::B: close_eq_editor(); break;

        case Gesture::B_LEFT:  eq_step_slot(-1); break;
        case Gesture::B_RIGHT: eq_step_slot(+1); break;

        // A plain press stops an audition only when the editor came from an instrument — the one case
        // a preview rings underneath.
        case Gesture::STOP_PREVIEW:
            if (s_.eq.caller.kind == EqCallerContext::Kind::INSTRUMENT) host_.stop_preview();
            break;

        case Gesture::START:
        case Gesture::SELECT:
            return LayerResult::PASS;

        // A plain A included: the editor's vocabulary is A+DPAD, A+B and B.
        default: break;
    }
    return LayerResult::TAKEN;
}

void InputDispatcher::eq_edit(InputAction (*fn)(const CursorContext&)) {
    EqState es{*s_.project};
    es.slotIndex = s_.eq.slotIndex;
    es.cursorRow = s_.eq.cursorRow;
    es.caller    = s_.eq.caller;

    const CursorContext ctx = eq_.cursor_context(es);
    const InputAction   act = fn(ctx);
    const EqInputResult r =
        eq_.handle_input(host_.edit_project(), s_.eq.slotIndex, s_.eq.cursorRow, act);

    if (r.eqBandChanged) {
        // Not `mark_modified()`, which would re-push the whole globals (all 128 EQ slots) on every
        // key-repeat when the screen behind is MIXER/EFFECTS. The band needs two calls;
        // apply_caller_eq_slot_change bumps `projectVersion`.
        push_eq_band_to_engine();
    }
}

void InputDispatcher::eq_step_slot(int delta) {
    // ⚠️ CLAMPS at 0 and 127 where every other B+LEFT/RIGHT wraps: wrapping would silently re-point
    // the cell at an unrelated curve.
    const int newSlot = std::min(127, std::max(0, s_.eq.slotIndex + delta));
    s_.eq.slotIndex   = newSlot;
    apply_caller_eq_slot_change(newSlot);
}

void InputDispatcher::open_eq_editor(int slot, EqCallerContext caller) {
    s_.eq           = EqEditorState{};
    s_.eq.isOpen    = true;
    s_.eq.slotIndex = std::min(127, std::max(0, slot));
    s_.eq.cursorRow = 0;   // BAND 1, TYPE — the top-left cell, every time
    s_.eq.caller    = caller;
}

void InputDispatcher::eq_move_cursor(int d_band, int d_param) {
    const int band  = std::min(2, std::max(0, s_.eq.cursor_band() + d_band));
    const int param = std::min(3, std::max(0, s_.eq.cursor_param() + d_param));
    s_.eq.cursorRow = band * 4 + param;
}

void InputDispatcher::apply_caller_eq_slot_change(int new_slot) {
    Project& p = host_.edit_project();

    // Five different project fields, one gesture: cycling the slot writes back to the cell that
    // RAISED the editor, which is what `EqCallerContext` records.
    switch (s_.eq.caller.kind) {
        case EqCallerContext::Kind::MASTER:
            p.masterEqSlot = new_slot;
            host_.set_master_eq_slot(new_slot);
            break;
        case EqCallerContext::Kind::REVERB_IN:
            p.reverbInputEq = new_slot;
            host_.set_reverb_input_eq(new_slot);
            break;
        case EqCallerContext::Kind::DELAY_IN:
            p.delayInputEq = new_slot;
            host_.set_delay_input_eq(new_slot);
            break;
        case EqCallerContext::Kind::INSTRUMENT: {
            const int id = s_.eq.caller.instrId;
            if (id >= 0 && id < static_cast<int>(p.instruments.size())) {
                p.instruments[static_cast<size_t>(id)].eqSlot = new_slot;
                host_.set_instrument_eq_slot(id, new_slot);
            }
            break;
        }
        case EqCallerContext::Kind::SAMPLE_EDITOR_FX:
            // No engine call: the sample editor's EQ is applied destructively on APPLY, reading the
            // bank then.
            s_.sampleEditor.fxValue = new_slot;
            break;
    }

    // Dirty AND armed: this path skips mark_modified (its push_globals is too heavy for a fast band
    // dial; the two calls below are the right-sized push), but must not skip the crash autosave.
    mark_dirty_and_arm_autosave();
}

void InputDispatcher::push_eq_band_to_engine() {
    const Project& p       = *s_.project;
    const int      slot    = s_.eq.slotIndex;
    const int      bandIdx = s_.eq.cursor_band();

    if (slot < 0 || slot >= static_cast<int>(p.eqPresets.size())) return;
    const songcore::EqPreset& preset = p.eqPresets[static_cast<size_t>(slot)];
    if (bandIdx < 0 || bandIdx >= static_cast<int>(preset.bands.size())) return;
    const songcore::EqBand& band = preset.bands[static_cast<size_t>(bandIdx)];

    // The BAND, into the engine's 128-slot bank.
    host_.set_eq_band(slot, bandIdx, band.type, band.freq, band.gain, band.q);

    // ⚠️ Then re-hand the caller the slot: `set_eq_band` writes the BANK, and only re-assigning the
    // slot makes the consumer recompile its coefficients. It goes through apply_caller_eq_slot_change
    // so an UNASSIGNED EQ (shown as slot 0) ADOPTS the slot in the project too — otherwise you would
    // hear the EQ while the cell read "--" and a reload discarded it.
    apply_caller_eq_slot_change(slot);
}

}  // namespace pt::ui
