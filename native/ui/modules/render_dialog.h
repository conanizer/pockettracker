#pragma once

// The RENDER dialog, raised by EXPORT → MIX or STEMS: which song rows go into the file, and how
// many times.
//   SONG START  the first song row
//   SONG END    the last, or AUTO — to the end of SONG START's section
//   REPEAT      times the range plays, or OFF (once)
//   RENDER      A fires it; the row shows the percentage
// A block of song rows loops for ever, so the range is what makes one sketch exportable. A modal
// with the confirm dialog's dim.

#include <string>

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/theme.h"

namespace pt::ui {

// ⚠️ A row's number is its identity — append, never insert.
enum class RenderRow {
    SONG_START = 0,
    SONG_END   = 1,
    REPEAT     = 2,
    RENDER     = 3,
    COUNT      = 4,
};

// OFF, then 2..16. No "1": that is OFF.
inline constexpr int RENDER_REPEAT_MAX = 16;

struct RenderDialogState {
    enum class Output { MIX, STEMS };

    bool   isOpen = false;
    Output output = Output::MIX;

    int cursorRow = 0;   // RenderRow

    // Set from the SONG cursor's section each time it opens.
    int startRow = 0;

    // The last song row, or −1 = AUTO. ⚠️ AUTO is a rule resolved at each render, never a stored
    // number — that would go stale when the part grows.
    int endRow = -1;

    // 1 = OFF.
    int repeat = 1;

    bool is_on(RenderRow row) const { return cursorRow == static_cast<int>(row); }
};

// The range handed to the renderer, AUTO resolved. The draw, the fire and SONG END's clamp all ask
// here, so they agree.
int render_dialog_end_row(const RenderDialogState& s, const songcore::Project& project);

void draw_render_dialog(Canvas& c, const RenderDialogState& s, const songcore::Project& project,
                        bool isRendering, float progress, const Theme& t);

}  // namespace pt::ui
