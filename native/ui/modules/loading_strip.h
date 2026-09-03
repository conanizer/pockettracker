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
 * ⚠️ **NOT A MODAL, AND DELIBERATELY SO.** A load dims nothing, covers nothing and answers no
 * question — the screen underneath stays readable, and the only thing the strip claims is that a
 * file is being opened and that B stops it. It is not in `modal_backdrop_active`.
 *
 * ⚠️ It draws NOTHING until `loading.shown`, which the dispatcher raises only once a load has already
 * outlasted the delay. See `AppState::LoadingState`: the app never predicts whether a file is big, it
 * finds out by waiting, and the overwhelming majority of loads finish before this is ever reached.
 *
 * ⚠️⚠️ **IT IS DRAWN OUTSIDE `TrackerLayout::draw_frame`, NOT AT THE END OF IT.** The file browser and
 * the sample editor return from the MIDDLE of the frame, and those two screens are where every load
 * the user starts begins — drawn inside, the strip is skipped by an early return that has nothing to
 * do with it, on exactly the screens it exists for.
 */
void draw_loading_strip(Canvas& c, const AppState::LoadingState& s, const Theme& t);

}  // namespace pt::ui

#endif  // POCKETTRACKER_UI_MODULES_LOADING_STRIP_H
