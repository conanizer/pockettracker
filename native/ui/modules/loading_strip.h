#ifndef POCKETTRACKER_UI_MODULES_LOADING_STRIP_H
#define POCKETTRACKER_UI_MODULES_LOADING_STRIP_H

#include "ui/app_state.h"
#include "ui/canvas.h"
#include "ui/helpers.h"
#include "ui/theme.h"

namespace pt::ui {

// Two of the app's rows.
inline constexpr int LOADING_STRIP_H = ROW_HEIGHT * 2;

// The line a slow load puts across the top of the screen. Not a modal; draws nothing until
// `loading.shown`. ⚠️ Drawn outside `TrackerLayout::draw_frame`: the file browser and the sample
// editor, where loads start, return mid-frame.
void draw_loading_strip(Canvas& c, const AppState::LoadingState& s, const Theme& t);

}  // namespace pt::ui

#endif  // POCKETTRACKER_UI_MODULES_LOADING_STRIP_H
