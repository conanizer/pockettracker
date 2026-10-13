// MIXER and EFFECTS: their buttons. Their MUTE and SOLO chords are shared with SONG (dispatch/chords.cpp).

#include "ui/dispatch/dispatch_common.h"

#include "ui/fx_preset_bank.h"
#include "ui/fx_preset_io.h"     // .ptr / .ptd

#include <algorithm>
#include <string>

namespace pt::ui {

// A on the master EQ cell opens the EQ editor on the release; nothing else here answers a bare A.
GestureResult InputDispatcher::mixer_screen(Gesture g) {
    switch (g) {
        case Gesture::A:
            open_sub_screen_at_cursor(/*peek=*/false);
            return GestureResult::TAKEN;
        default:
            return GestureResult::PASS;
    }
}

GestureResult InputDispatcher::effects_screen(Gesture g) {
    switch (g) {
        // A preset line's SAVE and LOAD; the two input EQ cells open the EQ editor on the release.
        case Gesture::A:
            if (!effects_preset_action()) open_sub_screen_at_cursor(/*peek=*/false);
            return GestureResult::TAKEN;

        // ⚠️ The TIME row: B toggles DELAY SYNC (milliseconds ↔ note divisions). A gesture of its own,
        // because the cell's value means different things on either side (0x40 a length; 4 a 1/16 note).
        // B is otherwise free on this screen, so no release latch is needed.
        // delayTime is re-clamped into 0..B on the way IN: a free time of 0xF0 would index past the names.
        case Gesture::B: {
            if (s_.effectsCursorRow != EffectModule::ROW_DLY_TIME) return GestureResult::PASS;
            songcore::Project& p = host_.edit_project();
            p.delaySync = !p.delaySync;
            if (p.delaySync) p.delayTime = std::min(std::max(p.delayTime, 0), 11);
            mark_modified();
            return GestureResult::TAKEN;
        }

        default:
            return GestureResult::PASS;
    }
}

// ─── EFFECTS: a preset line's SAVE and LOAD ──────────────────────────────────────────────────────

bool InputDispatcher::effects_preset_action() {
    const songcore::Project& p = host_.project();

    // SAVE is seeded with the sanitized name the row SHOWS (a built-in that was never cycled to stores
    // none), so what you see is what the file will be called. LOAD starts at the built-in folder:
    // config.json's `folders` has no key for either.
    switch (s_.effectsCursorRow) {
        case EffectModule::ROW_REV_SAVE: {
            const std::string seed = sanitize_fx_preset_filename(reverb_preset_display_name(p));
            open_qwerty(QwertyContext::REVERB_SAVE, seed.empty() ? "REVERB" : seed, "SAVE REVERB:",
                        fs_.reverbs_directory(), FX_PRESET_NAME_MAX, /*clear_on_first_b=*/true);
            return true;
        }
        case EffectModule::ROW_REV_LOAD:
            open_file_browser(AppState::BrowserPurpose::LOAD_REVERB, fs_.reverbs_directory(),
                              {REVERB_FILE_EXT});
            return true;
        case EffectModule::ROW_DLY_SAVE: {
            const std::string seed = sanitize_fx_preset_filename(delay_preset_display_name(p));
            open_qwerty(QwertyContext::DELAY_SAVE, seed.empty() ? "DELAY" : seed, "SAVE DELAY:",
                        fs_.delays_directory(), FX_PRESET_NAME_MAX, /*clear_on_first_b=*/true);
            return true;
        }
        case EffectModule::ROW_DLY_LOAD:
            open_file_browser(AppState::BrowserPurpose::LOAD_DELAY, fs_.delays_directory(),
                              {DELAY_FILE_EXT});
            return true;
        default:
            return false;
    }
}

void InputDispatcher::save_reverb_preset_as(const std::string& dir, const std::string& typed_text) {
    // Sanitized FILENAME (survives FAT32), raw name in the file and on the row. An empty field keeps
    // the shown name and falls back to "REVERB" — never the dotfile `.ptr`.
    const std::string shown = reverb_preset_display_name(host_.project());
    const std::string want  = !typed_text.empty() ? typed_text : !shown.empty() ? shown : "REVERB";
    const std::string safe  = sanitize_fx_preset_filename(want);

    // The row adopts the name it was saved under — the only thing on screen naming the file.
    if (host_.project().reverbPreset != want) {
        host_.edit_project().reverbPreset = want;
        mark_modified();
    }

    const bool ok = save_reverb_file(fs_, dir + "/" + safe + ".ptr", reverb_cells_of(host_.project()));
    s_.statusMessage = ok ? "REVERB SAVED" : "SAVE FAILED";
    s_.statusSuccess = ok;
}

void InputDispatcher::save_delay_preset_as(const std::string& dir, const std::string& typed_text) {
    // As the reverb's.
    const std::string shown = delay_preset_display_name(host_.project());
    const std::string want  = !typed_text.empty() ? typed_text : !shown.empty() ? shown : "DELAY";
    const std::string safe  = sanitize_fx_preset_filename(want);

    if (host_.project().delayPreset != want) {
        host_.edit_project().delayPreset = want;
        mark_modified();
    }

    const bool ok = save_delay_file(fs_, dir + "/" + safe + ".ptd", delay_cells_of(host_.project()));
    s_.statusMessage = ok ? "DELAY SAVED" : "SAVE FAILED";
    s_.statusSuccess = ok;
}

// ─── The cursor and the edit ─────────────────────────────────────────────────────────────────────

CursorContext InputDispatcher::mixer_context() const {
    const Project& p = *s_.project;
    MixerState ms{p};
    ms.cursorColumn   = s_.mixerCursorColumn;
    ms.mixerMasterRow = s_.mixerMasterRow;
    return mixer_.cursor_context(ms);
}

// MIXER and EFFECTS take the whole PROJECT: their fields are scattered across it.
bool InputDispatcher::mixer_edit(const InputAction& action) {
    Project& p = host_.edit_project();
    return mixer_.handle_input(p, s_.mixerMasterRow, s_.mixerCursorColumn, action).modified;
}

songcore::MapTarget InputDispatcher::mixer_knob() const {
    const Project& p = *s_.project;
    MixerState ms{p};
    ms.cursorColumn   = s_.mixerCursorColumn;
    ms.mixerMasterRow = s_.mixerMasterRow;
    return mixer_.map_target(ms);
}

CursorContext InputDispatcher::effects_context() const {
    const Project& p = *s_.project;
    EffectState es{p};
    es.cursorRow = s_.effectsCursorRow;
    return effects_.cursor_context(es);
}

bool InputDispatcher::effects_edit(const InputAction& action) {
    Project& p = host_.edit_project();
    return effects_.handle_input(p, s_.effectsCursorRow, action).modified;
}

songcore::MapTarget InputDispatcher::effects_knob() const {
    const Project& p = *s_.project;
    EffectState es{p};
    es.cursorRow = s_.effectsCursorRow;
    return effects_.map_target(es);
}

}  // namespace pt::ui
