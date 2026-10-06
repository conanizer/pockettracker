// MIXER and EFFECTS: their buttons. Their MUTE and SOLO chords are shared with SONG (dispatch/chords.cpp).

#include "ui/dispatch/dispatch_common.h"

#include <algorithm>

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
        // The two input EQ cells open the EQ editor on the release.
        case Gesture::A:
            open_sub_screen_at_cursor(/*peek=*/false);
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
