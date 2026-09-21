#pragma once

// ─── The RENDER dialog ───────────────────────────────────────────────────────────────────────────
//
// PROJECT → EXPORT → MIX or STEMS raises this instead of rendering on the spot: four rows that say
// WHICH rows of the song go into the file, and how many times over.
//
//   SONG START  the first song row of the render
//   SONG END    the last, or AUTO — follow SONG START's section to its end
//   REPEAT      how many times the range is played into the file, or OFF (once)
//   RENDER      A fires it; the row carries the percentage while it runs
//
// ⚠️ IT EXISTS BECAUSE A BLOCK LOOPS FOR EVER. A track now plays its block of song rows round and
// round rather than running on into what is below (scheduler.h `song_cell_plays`), so a project can
// hold several unrelated sketches that never meet. Rendering "the song" would glue them together with
// silence, and there was no way to ask for less — so the range is not a convenience here, it is what
// makes an isolated sketch exportable at all.
//
// The OUTPUT is not a row: which of the two EXPORT buttons opened the dialog is what picks stereo WAV
// or stems, exactly as it picked before. One panel, two outputs.
//
// A modal, with the same full-canvas dim as the confirm dialog — a render takes the machine over
// while it runs, and the panel is the only thing on screen that is live.

#include <string>

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/theme.h"

namespace pt::ui {

/** The four rows, top to bottom. ⚠️ A row's number is its identity — append, never insert. */
enum class RenderRow {
    SONG_START = 0,
    SONG_END   = 1,
    REPEAT     = 2,
    RENDER     = 3,
    COUNT      = 4,
};

/** OFF, then 2..16. ⚠️ There is no "1": that IS off, and two names for it is two ways to read it. */
inline constexpr int RENDER_REPEAT_MAX = 16;

struct RenderDialogState {
    enum class Output { MIX, STEMS };

    bool   isOpen = false;
    Output output = Output::MIX;

    int cursorRow = 0;   // RenderRow

    /** The first song row of the render. Set from the SONG cursor's section each time it opens. */
    int startRow = 0;

    /**
     * The last song row, or **−1 = AUTO**: resolve to the end of the section `startRow` is in, every
     * time the render fires.
     *
     * ⚠️ AUTO IS A RULE, NOT A REMEMBERED NUMBER. Left as a number it would go stale the moment a row
     * is added to the part, and the export would quietly stop one bar short of what is on screen.
     */
    int endRow = -1;

    /** How many times the range is played into the file. 1 = OFF. */
    int repeat = 1;

    bool is_on(RenderRow row) const { return cursorRow == static_cast<int>(row); }
};

/**
 * The row range this dialog will hand the renderer — AUTO resolved against the project as it stands.
 * ⭐ Asked here by the draw, by the fire and by SONG END's own editing clamp, so none of them can
 * disagree about what AUTO means.
 */
int render_dialog_end_row(const RenderDialogState& s, const songcore::Project& project);

void draw_render_dialog(Canvas& c, const RenderDialogState& s, const songcore::Project& project,
                        bool isRendering, float progress, const Theme& t);

}  // namespace pt::ui
