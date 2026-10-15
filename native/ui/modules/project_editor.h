#pragma once

// PROJECT: TEMPO, TRANSPOSE, NAME, and the buttons that SAVE, LOAD, start fresh, EXPORT and COMPACT.
// Rows 3-6 are buttons (`read_only()`, plain A in the dispatcher); `handle_input` touches rows 0-2.
// ⚠️ NAME is edited in place: each of its characters is a cursor column (1..20), A+LEFT/RIGHT walks
// `allowed_chars()`, A+B writes a space; A opens the keyboard. It scrolls via `qwerty_text_window`.

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
    int cursorColumn = 1;   // 1..project_row_max_column(row); 0, the label, is unreachable

    // A live percentage beside EXPORT while rendering.
    bool  isRendering    = false;
    float renderProgress = 0.0f;

    // USED RAM: sample + SoundFont PCM the engine holds. Debug builds only.
    int64_t sampleRamBytes = 0;

    // FREE RAM, the machine's. 0 = unknown, and then the row is not drawn — "0.0 MB" would say
    // "no memory left".
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

    // The value column, from the module's left edge.
    static constexpr int VALUE_X = 210;

    // NAME's cells on screen at once. ⚠️ Held to the editor clip by static_asserts in layout.h —
    // move `VALUE_X` instead.
    static constexpr int NAME_VISIBLE_CHARS = 17;

    void draw(Canvas& c, int x, int y, const ProjectState& s) const;

    CursorContext cursor_context(const ProjectState& s) const;

    // Rows 0-2 only; every other row is a button the dispatcher fires.
    ProjectInputResult handle_input(songcore::Project& project, int cursor_row, int cursor_column,
                                    const InputAction& action) const;
};

}  // namespace pt::ui
