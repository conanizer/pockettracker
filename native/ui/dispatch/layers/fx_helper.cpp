// The FX HELPER: the effect list A+UP/DOWN raises on an FX-TYPE column, up while A is held; the release
// writes the highlighted effect. ⚠️ The cursor underneath still names the cell the commit writes.

#include "ui/dispatch/dispatch_common.h"

namespace pt::ui {

namespace {

// A phrase step and a table row carry the same six FX fields; `slot` is 0-2.
template <class Row> auto& fx_type_of(Row& r, int slot) {
    return slot == 0 ? r.fx1Type : slot == 1 ? r.fx2Type : r.fx3Type;
}
template <class Row> auto& fx_value_of(Row& r, int slot) {
    return slot == 0 ? r.fx1Value : slot == 1 ? r.fx2Value : r.fx3Value;
}

// The phrase step or table row under the cursor, handed to `fn`; nothing on any other screen.
template <class P, class F> void with_fx_row(const AppState& s, P& p, F fn) {
    if (s.currentScreen == ScreenType::PHRASE)
        fn(p.phrases[static_cast<size_t>(s.currentPhrase)].steps[static_cast<size_t>(s.cursorRow)]);
    else if (s.currentScreen == ScreenType::TABLE)
        fn(p.tables[static_cast<size_t>(s.currentTable)].rows[static_cast<size_t>(s.tableCursorRow)]);
}

}  // namespace

GestureResult InputDispatcher::fx_helper_layer(Gesture g) {
    switch (g) {
        case Gesture::A_UP:    fx_move_up(s_.fxHelper); break;
        case Gesture::A_DOWN:  fx_move_down(s_.fxHelper); break;
        case Gesture::A_LEFT:  fx_move_left(s_.fxHelper); break;
        case Gesture::A_RIGHT: fx_move_right(s_.fxHelper); break;

        // Commits on RELEASE, so you can hold A, read the list, and let go on your choice.
        case Gesture::A_RELEASE:
            apply_fx_type_change(s_.fxHelper.selected_effect_code());
            s_.fxHelper = FxHelperState{};
            break;

        // The screen behind may have an audition ringing.
        case Gesture::STOP_PREVIEW: return GestureResult::PASS;

        default: break;
    }
    return GestureResult::TAKEN;
}

void InputDispatcher::open_fx_helper() {
    s_.fxHelper = fx_helper_opened_at(current_fx_type_code(),
                                      fx_layout_for(visible_effect_type_count()));
}

bool InputDispatcher::on_fx_type_column() const {
    switch (s_.currentScreen) {
        case ScreenType::PHRASE:
            return s_.cursorColumn == 4 || s_.cursorColumn == 6 || s_.cursorColumn == 8;
        case ScreenType::TABLE:
            return s_.tableCursorColumn == 3 || s_.tableCursorColumn == 5 ||
                   s_.tableCursorColumn == 7;
        default:
            return false;
    }
}

int InputDispatcher::current_fx_type_code() const {
    int code = 0;
    if (on_fx_type_column())
        with_fx_row(s_, *s_.project, [&](const auto& r) { code = fx_type_of(r, fx_slot_at_cursor()); });
    return code;
}

void InputDispatcher::apply_fx_type_change(int effect_code) {
    if (!on_fx_type_column()) return;
    const int slot = fx_slot_at_cursor();
    with_fx_row(s_, host_.edit_project(), [&](auto& r) { fx_type_of(r, slot) = effect_code; });
    mark_modified(/*table_touched=*/s_.currentScreen == ScreenType::TABLE);
    remember_fx_at_cursor();
}

// ─── A on an empty command cell ──────────────────────────────────────────────────────────────────

int InputDispatcher::fx_slot_at_cursor() const {
    if (s_.currentScreen == ScreenType::PHRASE && s_.cursorColumn >= 4) return (s_.cursorColumn - 4) / 2;
    if (s_.currentScreen == ScreenType::TABLE && s_.tableCursorColumn >= 3)
        return (s_.tableCursorColumn - 3) / 2;
    return -1;
}

void InputDispatcher::remember_fx_at_cursor() {
    const int slot = fx_slot_at_cursor();
    if (slot < 0) return;
    int type  = songcore::FX_NONE;
    int value = 0;
    with_fx_row(s_, *s_.project, [&](const auto& r) {
        type  = fx_type_of(r, slot);
        value = fx_value_of(r, slot);
    });
    if (type == songcore::FX_NONE) return;   // a cleared cell leaves the memory alone
    s_.lastEditedFxType  = type;
    s_.lastEditedFxValue = value;
}

bool InputDispatcher::fx_insert_last(bool peek) {
    if (!on_fx_type_column() || s_.selection.active) return false;
    if (s_.lastEditedFxType == songcore::FX_NONE || current_fx_type_code() != songcore::FX_NONE) return false;
    if (peek) return true;

    const int slot = fx_slot_at_cursor();
    with_fx_row(s_, host_.edit_project(), [&](auto& r) {
        fx_type_of(r, slot)  = s_.lastEditedFxType;
        fx_value_of(r, slot) = s_.lastEditedFxValue;
    });
    mark_modified(/*table_touched=*/s_.currentScreen == ScreenType::TABLE);
    return true;
}

}  // namespace pt::ui
