#ifndef POCKETTRACKER_UI_MODULES_LOADING_STRIP_H
#define POCKETTRACKER_UI_MODULES_LOADING_STRIP_H

#include "ui/app_state.h"
#include "ui/canvas.h"
#include "ui/helpers.h"   // ROW_HEIGHT — the strip is two of the app's own rows
#include "ui/theme.h"

namespace pt::ui {

/** The height of the strip: TWO of the app's rows — the words on the first, the bar on the second. */
inline constexpr int LOADING_STRIP_H = ROW_HEIGHT * 2;

/**
 * The line a slow load puts across the top of the screen.
 *
 * ⚠️ Not a modal: it dims and covers nothing, and is not in `modal_backdrop_active`. It draws nothing
 * until `loading.shown`, raised only once a load has outlasted the delay (`AppState::LoadingState`).
 *
 * ⚠️⚠️ Drawn OUTSIDE `TrackerLayout::draw_frame`: the file browser and the sample editor — where every
 * load starts — return from the middle of the frame and would skip it.
 */
void draw_loading_strip(Canvas& c, const AppState::LoadingState& s, const Theme& t);

}  // namespace pt::ui

#endif  // POCKETTRACKER_UI_MODULES_LOADING_STRIP_H
