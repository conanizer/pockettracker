// The QWERTY keyboard: renaming and naming files, instruments, the project, themes, scales, grooves
// and samples. It sits over whatever screen raised it; the theme editor's SAVE raises it over the editor.

#include "ui/dispatch/dispatch_common.h"

#include "ui/std_filesystem.h"   // path_stem

#include <cstdio>
#include <string>

namespace pt::ui {

LayerResult InputDispatcher::qwerty_layer(Gesture g) {
    QwertyKeyboardState& k = s_.qwerty;
    switch (g) {
        case Gesture::DPAD_UP:    move_key_cursor_up(k);    break;
        case Gesture::DPAD_DOWN:  move_key_cursor_down(k);  break;
        case Gesture::DPAD_LEFT:  move_key_cursor_left(k);  break;
        case Gesture::DPAD_RIGHT: move_key_cursor_right(k); break;

        // A types the key under the cursor — except on the action row: ABORT (col 0) and APPLY (col 1).
        case Gesture::A:
            if (!k.is_on_action_row())   insert_current_key(k);
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
    return LayerResult::TAKEN;
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
    k.insertBefore  = s_.settings.insertBefore;   // read at OPEN, so flipping the setting cannot change
    s_.qwerty       = k;                 // what the buttons mean mid-word
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
                s_.fileBrowser.statusMessage = "RENAMED";
                s_.fileBrowser.statusSuccess = true;
            } else {
                s_.fileBrowser.statusMessage = "RENAME FAILED";
                s_.fileBrowser.statusSuccess = false;
            }
            break;
        }

        case QwertyContext::FOLDER_CREATE: {
            const std::string name = text.empty() ? "NewFolder" : text;
            if (!fs_.create_folder(k.contextExtra, name).empty()) {
                refresh_browser();
                s_.fileBrowser.statusMessage = "CREATED";
                s_.fileBrowser.statusSuccess = true;
            } else {
                s_.fileBrowser.statusMessage = "CREATE FAILED";
                s_.fileBrowser.statusSuccess = false;
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
                s_.statusMessage = "SAVED: " + name;
                s_.statusSuccess = true;
            } else {
                s_.statusMessage = "SAVE FAILED";
                s_.statusSuccess = false;
            }
            break;
        }

        case QwertyContext::THEME_SAVE:
            // See `save_theme_as`. ⚠️ `k.contextExtra`, not `s_.qwerty.contextExtra`: the live keyboard was
            // cleared at the top of this function.
            save_theme_as(k.contextExtra, text);
            break;

        case QwertyContext::SCALE_SAVE:
            // ⚠️ `k.contextExtra` — the live keyboard is already cleared.
            save_scale_as(k.contextExtra, text);
            break;

        case QwertyContext::GROOVE_SAVE:
            save_groove_as(k.contextExtra, text);
            break;

        case QwertyContext::SAMPLE_NAME: {
            // Renames BOTH the editor's sample and the INSTRUMENT holding it — one thing to the user. An
            // empty field keeps the current name.
            SampleEditorState& se = s_.sampleEditor;
            const std::string  name = text.empty() ? se.sampleName : text;
            se.sampleName = name;
            host_.edit_project().instruments[static_cast<size_t>(se.instrumentId)].name = name;
            mark_modified();
            break;
        }

        case QwertyContext::SAMPLE_SAVE: {
            // SAVE-AS DE-DUPLICATES (`SNARE.wav`, `SNARE_0001.wav`, …) — OVERWRITE is its own button, and a
            // silent replace would be a destructive act with no confirm.
            const std::string base = text.empty() ? "SAMPLE" : text;
            std::string       path = k.contextExtra + "/" + base + ".wav";
            for (int n = 1; fs_.file_exists(path); ++n) {
                char suffix[16];   // "_%04d" of an int can be 12 bytes (-Wformat-truncation)
                std::snprintf(suffix, sizeof(suffix), "_%04d", n);
                path = k.contextExtra + "/" + base + suffix + ".wav";
            }
            save_sample_to(path, /*adopt_name=*/true);
            break;
        }

        case QwertyContext::RESAMPLE:
            // Empty → auto Resample_NNNN; anything typed is the base name. The selection is still live
            // (the keyboard never touched it).
            resample_selection(text);
            break;
    }
}

}  // namespace pt::ui
