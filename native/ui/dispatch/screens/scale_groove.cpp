// The GROOVE and SCALE screens: their buttons, what A does on their action rows, and saving to a file.

#include "ui/dispatch/dispatch_common.h"

#include "ui/groove_io.h"        // .ptg — save_groove_file / load_groove_file / the factory seed
#include "ui/scale_io.h"         // .pts — save_scale_file / load_scale_file / the factory seed

#include <algorithm>
#include <string>

namespace pt::ui {

// ─── The buttons ─────────────────────────────────────────────────────────────────────────────────

GestureResult InputDispatcher::groove_screen(Gesture g) {
    switch (g) {
        // The panel's SAVE and LOAD cells. On the tick grid, A on the end-of-pattern marker lays a step
        // down at the default tick count. ⚠️ Only there: on an existing tick a bare A would step it.
        case Gesture::A:
            if (s_.grooveCursorColumn == GROOVE_COL_PANEL)
                groove_row_action();
            else if (cursor_context().capabilities.isEmpty)
                generic_input(pt::ui::increment);
            return GestureResult::TAKEN;

        case Gesture::B_LEFT:
        case Gesture::B_RIGHT:
            s_.currentGroove =
                step_wrapping(s_.currentGroove, (g == Gesture::B_LEFT) ? -1 : +1, songcore::POOL_GROOVES);
            return GestureResult::TAKEN;

        // The quantize, from any cell.
        case Gesture::B_UP:
        case Gesture::B_DOWN:
            s_.grooveQuantize =
                step_wrapping(s_.grooveQuantize, (g == Gesture::B_UP) ? +1 : -1, GROOVE_QUANTIZE_COUNT);
            return GestureResult::TAKEN;

        default:
            return GestureResult::PASS;
    }
}

GestureResult InputDispatcher::scale_screen(Gesture g) {
    switch (g) {
        case Gesture::A:
            scale_row_action();
            return GestureResult::TAKEN;

        case Gesture::B_LEFT:
        case Gesture::B_RIGHT:
            s_.currentScale =
                step_wrapping(s_.currentScale, (g == Gesture::B_LEFT) ? -1 : +1, songcore::POOL_SCALES);
            return GestureResult::TAKEN;

        default:
            return GestureResult::PASS;
    }
}

// ─── The SCALE screen's NAME row ─────────────────────────────────────────────────────────────────

void InputDispatcher::scale_row_action() {
    const songcore::Scale& scale =
        host_.project().scales[static_cast<size_t>(s_.currentScale)];

    switch (scale_name_action(s_.scaleCursorRow, s_.scaleCursorColumn)) {
        case ScaleNameAction::SAVE: {
            // Seeded with the sanitized display name — what the file will be called.
            const std::string seed = safe_file_name(songcore::scale_display_name(scale));
            open_qwerty(QwertyContext::SCALE_SAVE, seed.empty() ? "SCALE" : seed, "SAVE SCALE:",
                        fs_.scales_directory(), /*max_length=*/20, /*clear_on_first_b=*/true);
            break;
        }
        case ScaleNameAction::LOAD:
            // The browser replaces the screen; it starts at the built-in folder (no config.json key).
            open_file_browser(AppState::BrowserPurpose::LOAD_SCALE, fs_.scales_directory(),
                              {SCALE_FILE_EXT});
            break;
        case ScaleNameAction::NONE:
            break;
    }
}

void InputDispatcher::save_scale_as(const std::string& dir, const std::string& typed_text) {
    // Sanitized filename (empty → SCALE.pts), raw name inside.
    const std::string safe = safe_file_name(typed_text);
    const std::string file = (safe.empty() ? std::string("SCALE") : safe) + ".pts";

    songcore::Scale& slot = host_.edit_project().scales[static_cast<size_t>(s_.currentScale)];

    // The slot adopts the name it was saved under, which clears the `*`.
    const std::string want = !typed_text.empty()          ? typed_text
                           : !slot.name.empty()           ? slot.name
                                                          : songcore::scale_display_name(slot);
    if (slot.name != want) {
        slot.name = want;
        mark_modified();
    }

    const bool ok = save_scale_file(fs_, dir + "/" + file, slot);
    set_status(s_, ok ? "SCALE SAVED" : "SAVE FAILED", ok);
}

// ─── The GROOVE screen's SAVE / LOAD cells ───────────────────────────────────────────────────────

void InputDispatcher::groove_row_action() {
    const songcore::Groove& groove =
        host_.project().grooves[static_cast<size_t>(s_.currentGroove)];

    switch (groove_file_action(s_.groovePanelRow, s_.groovePanelColumn)) {
        case GrooveFileAction::SAVE: {
            // Seeded with the sanitized display name — what the file will be called.
            const std::string seed = safe_file_name(songcore::groove_display_name(groove));
            open_qwerty(QwertyContext::GROOVE_SAVE, seed.empty() ? "GROOVE" : seed, "SAVE GROOVE:",
                        fs_.grooves_directory(), /*max_length=*/20, /*clear_on_first_b=*/true);
            break;
        }
        case GrooveFileAction::LOAD:
            // The browser replaces the screen; it starts at the built-in folder (no config.json key).
            open_file_browser(AppState::BrowserPurpose::LOAD_GROOVE, fs_.grooves_directory(),
                              {GROOVE_FILE_EXT});
            break;
        case GrooveFileAction::NONE:
            break;
    }
}

void InputDispatcher::save_groove_as(const std::string& dir, const std::string& typed_text) {
    // As for scales (empty → GROOVE.ptg).
    const std::string safe = safe_file_name(typed_text);
    const std::string file = (safe.empty() ? std::string("GROOVE") : safe) + ".ptg";

    songcore::Groove& slot = host_.edit_project().grooves[static_cast<size_t>(s_.currentGroove)];

    const std::string want = !typed_text.empty()          ? typed_text
                           : !slot.name.empty()           ? slot.name
                                                          : songcore::groove_display_name(slot);
    if (slot.name != want) {
        slot.name = want;
        mark_modified();
    }

    const bool ok = save_groove_file(fs_, dir + "/" + file, slot);
    set_status(s_, ok ? "GROOVE SAVED" : "SAVE FAILED", ok);
}

// ─── The cursor and the edit ─────────────────────────────────────────────────────────────────────

/** The GROOVE screen's state, assembled once for both the cursor context and the edit. */
GrooveState InputDispatcher::groove_state(const Project& p) const {
    GrooveState gs{p.grooves[static_cast<size_t>(s_.currentGroove)]};
    gs.cursorRow    = s_.grooveCursorRow;
    gs.cursorColumn = s_.grooveCursorColumn;
    gs.panelRow     = s_.groovePanelRow;
    gs.panelColumn  = s_.groovePanelColumn;
    gs.quantize     = s_.grooveQuantize;
    return gs;
}

CursorContext InputDispatcher::groove_context() const {
    const Project& p = *s_.project;
    return groove_.cursor_context(groove_state(p));
}

bool InputDispatcher::groove_edit(const InputAction& action) {
    Project& p = host_.edit_project();
    const GrooveInputResult r = groove_.handle_input(
        p.grooves[static_cast<size_t>(s_.currentGroove)], groove_state(p), action);
    // ⚠️ The quantize pointer is not song data: it comes back separately, and moving it must
    // not dirty the project or arm an autosave.
    if (r.newQuantize >= 0) s_.grooveQuantize = r.newQuantize;
    if (r.rulerRefused) {
        set_status(s_, "A STEP WOULD PASS FF", false);
    }
    return r.modified;
}

CursorContext InputDispatcher::scale_context() const {
    const Project& p = *s_.project;
    ScaleState cs{p.scales[static_cast<size_t>(s_.currentScale)]};
    cs.key          = p.scaleKey;
    cs.cursorRow    = s_.scaleCursorRow;
    cs.cursorColumn = s_.scaleCursorColumn;
    return scale_.cursor_context(cs);
}

bool InputDispatcher::scale_edit(const InputAction& action) {
    Project& p = host_.edit_project();
    // ⚠️ The KEY row edits the PROJECT, not the scale handed in; the module returns the new key
    // rather than holding a Project.
    const ScaleInputResult r = scale_.handle_input(
        p.scales[static_cast<size_t>(s_.currentScale)], p.scaleKey, s_.scaleCursorRow,
        s_.scaleCursorColumn, action);
    if (r.newKey >= 0) p.scaleKey = r.newKey;
    return r.modified;
}

}  // namespace pt::ui
