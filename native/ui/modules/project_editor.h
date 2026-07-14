#pragma once

// ─── PROJECT ─────────────────────────────────────────────────────────────────────────────────────
//
// The C++ twin of ui/modules/ProjectModule.kt. Two editable values (TEMPO, TRANSPOSE), one in-place
// text field (NAME), and then the screen the whole app has been missing: the one where a song is
// SAVED, LOADED, started fresh, EXPORTED and COMPACTED.
//
// A FORM, like INSTRUMENT and EFFECTS — but the first whose rows are mostly BUTTONS rather than
// cells. Rows 3-6 have no editable value at all: their cursor context is `read_only()` and the whole
// of their behaviour is what plain A does on them, which lives in the dispatcher. That is why
// `handle_input` below only ever touches rows 0-2, exactly as Kotlin's does.
//
// ⚠️ NAME is the port's first IN-PLACE character editor. Each of the 20 characters is its own cursor
// COLUMN (1..20), A+UP/DOWN walks `allowed_chars()`, and A+B writes a space. (A on the row opens the
// QWERTY keyboard instead — the deferred-A latch S6a built for the INSTRUMENT NAME cell.)
//
// ⚠️ Column 0 — the row LABEL — is unreachable on this screen. `getProjectCursorLeftColumn` coerces
// to at least 1 and the cursor starts there, so ProjectModule's four `cursorColumn == 0 -> readOnly()`
// arms are dead code in Kotlin. They are ported anyway: they cost a line, and a screen whose cursor
// can only be proven not to reach column 0 by reading a different file is one refactor away from
// being wrong.

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

    /** The debug-only USED RAM readout. Bytes of sample + SoundFont PCM the engine is holding. */
    int64_t sampleRamBytes = 0;

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
