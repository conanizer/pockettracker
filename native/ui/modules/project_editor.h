#pragma once

// ─── PROJECT ─────────────────────────────────────────────────────────────────────────────────────
//
// Two editable values (TEMPO, TRANSPOSE), an in-place text field (NAME), and the buttons that SAVE,
// LOAD, start fresh, EXPORT and COMPACT a song.
//
// A FORM whose rows are mostly BUTTONS: rows 3-6 are `read_only()` and their behaviour is what plain
// A does in the dispatcher, so `handle_input` only touches rows 0-2.
//
// ⚠️ NAME is an in-place character editor: each of its `PROJECT_NAME_MAX_CHARS` characters is a
// cursor COLUMN (1..20), A+LEFT/RIGHT walks `allowed_chars()`, A+B writes a space. A on the row opens
// the QWERTY keyboard instead. The field scrolls under the cursor with a "…" on the hidden side,
// through `qwerty_text_window`, as the keyboard's own text box does.
//
// ⚠️ Column 0 (the label) is unreachable — the cursor's left move stops at 1 — but its `read_only()`
// arms stay, so the screen does not depend on another file to be safe.

#include <cstdint>
#include <string>

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/platform_caps.h"
#include "ui/settings_row_layout.h"
#include "ui/theme.h"

namespace pt::ui {

struct ProjectState {
    const songcore::Project& project;

    int cursorRow    = 0;   // ProjectRow
    int cursorColumn = 1;   // 1..project_row_max_column(row); 0 (the label) is unreachable

    /** EXPORT draws a live percentage beside its buttons while a render is running. */
    bool  isRendering    = false;
    float renderProgress = 0.0f;

    /** USED RAM. Bytes of sample + SoundFont PCM the engine is holding. `caps.debug` builds only. */
    int64_t sampleRamBytes = 0;

    /**
     * FREE RAM — physical memory the machine still has, the denominator USED never had. 0 when the
     * platform cannot answer, and then the row is not drawn at all: a readout stuck on "0.0 MB"
     * reads as "you have no memory left", which is the one message it must never send by accident.
     */
    int64_t freeRamBytes = 0;

    PlatformCaps caps{};
    Theme        theme = theme_classic();
};

struct ProjectInputResult {
    bool modified = false;
};

class ProjectModule {
public:
    static constexpr int WIDTH  = 510;
    static constexpr int HEIGHT = 392;

    /** x of the value column, from the module's own left edge. Every row's value starts here. */
    static constexpr int VALUE_X = 210;

    /**
     * How many of NAME's cells are on screen at once; the field scrolls under the cursor.
     * ⚠️ This number belongs to the editor clip: two static_asserts in layout.h hold it to the most
     * cells that fit. Move `VALUE_X` instead of adjusting it.
     */
    static constexpr int NAME_VISIBLE_CHARS = 17;

    void draw(Canvas& c, int x, int y, const ProjectState& s) const;

    CursorContext cursor_context(const ProjectState& s) const;

    /**
     * Rows 0-2 only — TEMPO, TRANSPOSE and one character of NAME. Every other row is a button, and a
     * button is not an edit: SAVE / LOAD / NEW / MIX / STEMS / SEQ / INST / SETTINGS / EXIT all fire
     * from plain A in the dispatcher, which is the only place that can reach a filesystem or a
     * screen change.
     */
    ProjectInputResult handle_input(songcore::Project& project, int cursor_row, int cursor_column,
                                    const InputAction& action) const;
};

}  // namespace pt::ui
