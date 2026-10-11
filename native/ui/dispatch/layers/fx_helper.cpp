// The FX HELPER: the effect list raised by A+UP/DOWN on an FX-TYPE column. It lives only while A is
// held — A+DPAD walks the list and releasing A writes the highlighted effect into the cell and closes.
// ⚠️ `currentScreen` and the cursor still name the cell underneath; the commit writes through them.

#include "ui/dispatch/dispatch_common.h"

namespace pt::ui {

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
    const Project& p = *s_.project;
    int            code = 0;

    if (s_.currentScreen == ScreenType::PHRASE) {
        const songcore::PhraseStep& step =
            p.phrases[static_cast<size_t>(s_.currentPhrase)].steps[static_cast<size_t>(s_.cursorRow)];
        switch (s_.cursorColumn) {
            case 4: code = step.fx1Type; break;
            case 6: code = step.fx2Type; break;
            case 8: code = step.fx3Type; break;
            default: break;
        }
    } else if (s_.currentScreen == ScreenType::TABLE) {
        const songcore::TableRow& row =
            p.tables[static_cast<size_t>(s_.currentTable)].rows[static_cast<size_t>(s_.tableCursorRow)];
        switch (s_.tableCursorColumn) {
            case 3: code = row.fx1Type; break;
            case 5: code = row.fx2Type; break;
            case 7: code = row.fx3Type; break;
            default: break;
        }
    }
    return code;
}

void InputDispatcher::apply_fx_type_change(int effect_code) {
    Project& p = host_.edit_project();

    if (s_.currentScreen == ScreenType::PHRASE) {
        songcore::PhraseStep& step =
            p.phrases[static_cast<size_t>(s_.currentPhrase)].steps[static_cast<size_t>(s_.cursorRow)];
        switch (s_.cursorColumn) {
            case 4: step.fx1Type = effect_code; break;
            case 6: step.fx2Type = effect_code; break;
            case 8: step.fx3Type = effect_code; break;
            default: return;
        }
        mark_modified();
    } else if (s_.currentScreen == ScreenType::TABLE) {
        songcore::TableRow& row =
            p.tables[static_cast<size_t>(s_.currentTable)].rows[static_cast<size_t>(s_.tableCursorRow)];
        switch (s_.tableCursorColumn) {
            case 3: row.fx1Type = effect_code; break;
            case 5: row.fx2Type = effect_code; break;
            case 7: row.fx3Type = effect_code; break;
            default: return;
        }
        mark_modified(/*table_touched=*/true);
    }
    remember_fx_at_cursor();
}

// ─── A on an empty command cell ──────────────────────────────────────────────────────────────────

namespace {

// A phrase step and a table row carry the same six FX fields; `slot` is 0-2.
template <class Row> auto& fx_type_of(Row& r, int slot) {
    return slot == 0 ? r.fx1Type : slot == 1 ? r.fx2Type : r.fx3Type;
}
template <class Row> auto& fx_value_of(Row& r, int slot) {
    return slot == 0 ? r.fx1Value : slot == 1 ? r.fx2Value : r.fx3Value;
}

}  // namespace

int InputDispatcher::fx_slot_at_cursor() const {
    if (s_.currentScreen == ScreenType::PHRASE && s_.cursorColumn >= 4) return (s_.cursorColumn - 4) / 2;
    if (s_.currentScreen == ScreenType::TABLE && s_.tableCursorColumn >= 3)
        return (s_.tableCursorColumn - 3) / 2;
    return -1;
}

void InputDispatcher::remember_fx_at_cursor() {
    const int slot = fx_slot_at_cursor();
    if (slot < 0) return;
    const Project& p = *s_.project;
    int type  = songcore::FX_NONE;
    int value = 0;
    if (s_.currentScreen == ScreenType::PHRASE) {
        const songcore::PhraseStep& step =
            p.phrases[static_cast<size_t>(s_.currentPhrase)].steps[static_cast<size_t>(s_.cursorRow)];
        type  = fx_type_of(step, slot);
        value = fx_value_of(step, slot);
    } else {
        const songcore::TableRow& row =
            p.tables[static_cast<size_t>(s_.currentTable)].rows[static_cast<size_t>(s_.tableCursorRow)];
        type  = fx_type_of(row, slot);
        value = fx_value_of(row, slot);
    }
    if (type == songcore::FX_NONE) return;   // a cleared cell leaves the memory alone
    s_.lastEditedFxType  = type;
    s_.lastEditedFxValue = value;
}

bool InputDispatcher::fx_insert_last(bool peek) {
    if (!on_fx_type_column() || s_.selection.active) return false;
    if (s_.lastEditedFxType == songcore::FX_NONE || current_fx_type_code() != songcore::FX_NONE) return false;
    if (peek) return true;

    Project&  p    = host_.edit_project();
    const int slot = fx_slot_at_cursor();
    if (s_.currentScreen == ScreenType::PHRASE) {
        songcore::PhraseStep& step =
            p.phrases[static_cast<size_t>(s_.currentPhrase)].steps[static_cast<size_t>(s_.cursorRow)];
        fx_type_of(step, slot)  = s_.lastEditedFxType;
        fx_value_of(step, slot) = s_.lastEditedFxValue;
        mark_modified();
    } else {
        songcore::TableRow& row =
            p.tables[static_cast<size_t>(s_.currentTable)].rows[static_cast<size_t>(s_.tableCursorRow)];
        fx_type_of(row, slot)  = s_.lastEditedFxType;
        fx_value_of(row, slot) = s_.lastEditedFxValue;
        mark_modified(/*table_touched=*/true);
    }
    return true;
}

}  // namespace pt::ui
