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
}

}  // namespace pt::ui
