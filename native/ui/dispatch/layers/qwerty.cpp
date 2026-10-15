// The QWERTY keyboard, over whatever raised it (the theme editor's SAVE raises it over the editor).

#include "ui/dispatch/dispatch_common.h"

#include "ui/std_filesystem.h"   // path_stem

#include <string>

namespace pt::ui {

GestureResult InputDispatcher::qwerty_layer(Gesture g) {
    QwertyKeyboardState& k = s_.qwerty;
    switch (g) {
        case Gesture::DPAD_UP:    move_key_cursor_up(k);    break;
        case Gesture::DPAD_DOWN:  move_key_cursor_down(k);  break;
        case Gesture::DPAD_LEFT:  move_key_cursor_left(k);  break;
        case Gesture::DPAD_RIGHT: move_key_cursor_right(k); break;

        // A types the key — except the layout key and the action row (ABORT col 0, APPLY col 1).
        case Gesture::A:
            if (k.is_on_layout_key())         k.layout = 1 - k.layout;
            else if (!k.is_on_action_row())   insert_current_key(k);
            else if (k.keyCursorCol == 0) qwerty_cancel();
            else                          qwerty_apply();
            break;

        case Gesture::B:      delete_char(k);  break;
        case Gesture::START:  qwerty_apply();  break;
        // B backspaces, so SELECT is the quick way to abandon a rename.
        case Gesture::SELECT: qwerty_cancel(); break;

        // R+UP/DOWN switch the layout; R+LEFT/RIGHT move the text cursor.
        case Gesture::R_UP:    k.layout = 0; clamp_col(k); break;
        case Gesture::R_DOWN:  k.layout = 1; clamp_col(k); break;
        case Gesture::R_LEFT:  move_text_cursor_left(k);  break;
        case Gesture::R_RIGHT: move_text_cursor_right(k); break;

        default: break;
    }
    return GestureResult::TAKEN;
}

void InputDispatcher::open_qwerty(QwertyContext context, const std::string& initial_text,
                                  const std::string& field_label, const std::string& context_extra,
                                  int max_length, bool clear_on_first_b) {
    QwertyKeyboardState k{};
    k.isOpen        = true;
    k.text          = initial_text.substr(0, static_cast<size_t>(max_length));
    k.maxLength     = max_length;
    k.textCursor    = static_cast<int>(k.text.size());
    k.fieldLabel    = field_label;
    k.contextExtra  = context_extra;
    k.context       = context;
    k.clearOnFirstB = clear_on_first_b;
    k.insertBefore  = s_.settings.insertBefore;   // read at open, so it cannot change mid-word
    s_.qwerty       = k;
}

void InputDispatcher::qwerty_apply() {
    const QwertyKeyboardState k    = s_.qwerty;   // by value: every arm below closes the keyboard
    const std::string         text = trimmed_text(k);
    s_.qwerty = QwertyKeyboardState{};

    switch (k.context) {
        case QwertyContext::FILE_RENAME: {
            // An empty field means "leave it alone", not "name it nothing".
            const std::string name = text.empty() ? path_stem(k.contextExtra) : text;
            if (fs_.rename_file(k.contextExtra, name)) {
                refresh_browser();
                set_status(s_.fileBrowser, "RENAMED", true);
            } else {
                set_status(s_.fileBrowser, "RENAME FAILED", false);
            }
            break;
        }

        case QwertyContext::FOLDER_CREATE: {
            const std::string name = text.empty() ? "NewFolder" : text;
            if (!fs_.create_folder(k.contextExtra, name).empty()) {
                refresh_browser();
                set_status(s_.fileBrowser, "CREATED", true);
            } else {
                set_status(s_.fileBrowser, "CREATE FAILED", false);
            }
            break;
        }

        case QwertyContext::INSTRUMENT_NAME: {
            Instrument& ins = host_.edit_project().instruments[static_cast<size_t>(s_.currentInstrument)];
            // A cleared name reverts to "INSTxx": an unnamed instrument must still be identifiable.
            ins.name = text.empty() ? songcore::default_instrument_name(ins.id) : text;
            mark_modified();
            break;
        }

        case QwertyContext::PROJECT_NAME:
            // An empty name is allowed; SAVE supplies a fallback filename (save_project).
            host_.edit_project().name = text;
            mark_modified();
            break;

        case QwertyContext::INSTRUMENT_SAVE: {
            const std::string name = text.empty() ? "PRESET" : text;
            const std::string path = k.contextExtra + "/" + name + ".pti";
            if (save_instrument_preset(host_, fs_, s_.currentInstrument, path)) {
                set_status(s_, "SAVED: " + name, true);
            } else {
                set_status(s_, "SAVE FAILED", false);
            }
            break;
        }

        case QwertyContext::THEME_SAVE:
            // ⚠️ k.contextExtra, not s_.qwerty's: the live keyboard is already cleared.
            save_theme_as(k.contextExtra, text);
            break;

        case QwertyContext::SCALE_SAVE:
            save_scale_as(k.contextExtra, text);
            break;

        case QwertyContext::GROOVE_SAVE:
            save_groove_as(k.contextExtra, text);
            break;

        case QwertyContext::REVERB_SAVE:
            save_reverb_preset_as(k.contextExtra, text);
            break;

        case QwertyContext::DELAY_SAVE:
            save_delay_preset_as(k.contextExtra, text);
            break;

        case QwertyContext::SAMPLE_NAME: {
            // Renames the editor's sample and its instrument — one thing to the user. Empty keeps the name.
            SampleEditorState& se = s_.sampleEditor;
            const std::string  name = text.empty() ? se.sampleName : text;
            se.sampleName = name;
            host_.edit_project().instruments[static_cast<size_t>(se.instrumentId)].name = name;
            mark_modified();
            break;
        }

        case QwertyContext::SAMPLE_SAVE: {
            // SAVE-AS never replaces (SNARE, SNARE_0001, …): OVERWRITE is its own button.
            const std::string stem = free_wav_stem(fs_, k.contextExtra, text.empty() ? "SAMPLE" : text);
            save_sample_to(k.contextExtra + "/" + stem + ".wav", /*adopt_name=*/true);
            break;
        }

        case QwertyContext::RESAMPLE:
            // Empty → auto Resample_NNNN. The selection is still live.
            resample_selection(text);
            break;
    }
}

}  // namespace pt::ui
