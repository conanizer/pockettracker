// The FX HELPER: the effect list A+UP/DOWN raises on an FX-TYPE column, up while A is held; the release
// writes the highlighted effect. ⚠️ The cursor underneath still names the cell the commit writes.

#include "ui/dispatch/dispatch_common.h"
#include "ui/fx_columns.h"

namespace pt::ui {

namespace {

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
        case ScreenType::PHRASE: return is_fx_type_column(s_.cursorColumn, PHRASE_FX_COLUMN);
        case ScreenType::TABLE:  return is_fx_type_column(s_.tableCursorColumn, TABLE_FX_COLUMN);
        default:                 return false;
    }
}

int InputDispatcher::current_fx_type_code() const {
    int code = 0;
    if (on_fx_type_column())
        with_fx_row(s_, *s_.project, [&](const auto& r) { code = songcore::fx_slot(r, fx_slot_at_cursor()).type; });
    return code;
}

void InputDispatcher::apply_fx_type_change(int effect_code) {
    if (!on_fx_type_column() || effect_code == current_fx_type_code()) return;
    const int slot = fx_slot_at_cursor();
    with_fx_row(s_, host_.edit_project(), [&](auto& r) { songcore::fx_slot_ref(r, slot).type = effect_code; });
    mark_modified(/*table_touched=*/s_.currentScreen == ScreenType::TABLE);
    remember_fx_at_cursor();
}

// ─── A on an empty command cell ──────────────────────────────────────────────────────────────────

int InputDispatcher::fx_slot_at_cursor() const {
    if (s_.currentScreen == ScreenType::PHRASE) return fx_slot_of_column(s_.cursorColumn, PHRASE_FX_COLUMN);
    if (s_.currentScreen == ScreenType::TABLE)  return fx_slot_of_column(s_.tableCursorColumn, TABLE_FX_COLUMN);
    return 0;
}

void InputDispatcher::remember_fx_at_cursor() {
    const int slot = fx_slot_at_cursor();
    if (slot == 0) return;
    songcore::FxSlot f;
    with_fx_row(s_, *s_.project, [&](const auto& r) { f = songcore::fx_slot(r, slot); });
    if (f.type == songcore::FX_NONE) return;   // a cleared cell leaves the memory alone
    s_.lastEditedFxType  = f.type;
    s_.lastEditedFxValue = f.value;
}

bool InputDispatcher::lay_last_fx(bool peek) {
    if (!on_fx_type_column()) return false;
    if (s_.lastEditedFxType == songcore::FX_NONE || current_fx_type_code() != songcore::FX_NONE) return false;
    if (peek) return true;

    const int slot = fx_slot_at_cursor();
    with_fx_row(s_, host_.edit_project(), [&](auto& r) {
        songcore::FxSlot& f = songcore::fx_slot_ref(r, slot);
        f.type  = s_.lastEditedFxType;
        f.value = s_.lastEditedFxValue;
    });
    return true;
}

bool InputDispatcher::fx_insert_last(bool peek) {
    if (s_.selection.active || !lay_last_fx(peek)) return false;
    if (!peek) mark_modified(/*table_touched=*/s_.currentScreen == ScreenType::TABLE);
    return true;
}

bool InputDispatcher::cursor_cell_empty() const {
    if (cursor_context().capabilities.isEmpty) return true;
    // PHRASE's V and I play nothing without a note, and are drawn dimmed.
    if (s_.currentScreen == ScreenType::PHRASE && (s_.cursorColumn == 2 || s_.cursorColumn == 3))
        return !songcore::step_has_note(s_.project->phrases[static_cast<size_t>(s_.currentPhrase)]
                                            .steps[static_cast<size_t>(s_.cursorRow)]);
    const int slot = fx_slot_at_cursor();
    if (slot == 0) return false;
    int type = songcore::FX_NONE;
    with_fx_row(s_, *s_.project, [&](const auto& r) { type = songcore::fx_slot(r, slot).type; });
    return type == songcore::FX_NONE;
}

}  // namespace pt::ui
