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
// ⚠️ It exists because a block of song rows LOOPS FOR EVER (scheduler.h `song_cell_plays`): a project
// can hold several unrelated sketches, and the range is what makes one of them exportable at all.
//
// The output is not a row: which EXPORT button opened the dialog picks stereo WAV or stems.
//
// A modal with the confirm dialog's full-canvas dim; a render takes the machine over while it runs.

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
     * ⚠️ AUTO IS A RULE, NOT A REMEMBERED NUMBER — a stored number would go stale when a row is added
     * to the part, and the export would stop short of what is on screen.
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
