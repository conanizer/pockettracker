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

}  // namespace pt::ui
