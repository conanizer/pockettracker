#include "ui/modules/scale_editor.h"

#include "songcore/scales.h"
#include "ui/helpers.h"

namespace pt::ui {

namespace {

/** The pitch class a degree row names, in the current key. */
inline int degree_pitch_class(int key, int degree) { return songcore::scale_mod12(key + degree); }

/**
 * A pitch class by name, with no octave — "C", "F#". The shared table pads a natural with a "-" so
 * that a phrase cell's "C-4" lines up with "C#4"; nothing follows the name here, so that pad would
 * read as part of it.
 */
inline std::string pitch_class_name(int pc) {
    std::string name = songcore::NOTE_NAMES[songcore::scale_mod12(pc)];
    if (name.size() == 2 && name[1] == '-') name.pop_back();
    return name;
}

}  // namespace

void ScaleModule::draw(Canvas& c, int x, int y, const ScaleState& s) const {
    const Theme&           t     = s.theme;
    const songcore::Scale& scale = s.scale;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    const int labelX = x + 10;
    const int noteX  = x + 10 + 40;
    const int valueX = x + 10 + 40 + 60;

    // ── Header ───────────────────────────────────────────────────────────────────────────────────
    const int headerY = y + TEXT_PADDING;
    c.draw_text("SCALE " + hex2(scale.id), labelX, headerY, t.textTitle, CHAR_SPACING, FONT_SCALE);

    // How many notes the scale has. It is the one number that says at a glance what you have built —
    // 7 is a mode, 5 a pentatonic, 12 no constraint at all — and it is derived, never stored.
    std::string lenText = std::to_string(songcore::scale_degree_count(scale));
    if (lenText.size() < 2) lenText = " " + lenText;
    c.draw_text("LEN:" + lenText, x + WIDTH - 130, headerY, t.textParam, CHAR_SPACING, FONT_SCALE);

    // ── Row 0: KEY ───────────────────────────────────────────────────────────────────────────────
    // ⚠️ It sits ABOVE the EN column header, not under it, and that is the whole reason for the gap:
    // the key belongs to the PROJECT and the twelve rows below belong to this SLOT. Drawn inside the
    // column, its value reads as the twelve rows' thirteenth "EN".
    const int keyY = y + ROW_HEIGHT + 14 + TEXT_PADDING;

    const bool keyCursor = (s.cursorRow == SCALE_KEY_ROW);
    c.draw_text("KEY", labelX, keyY, keyCursor ? t.textCursor : t.textParam, CHAR_SPACING,
                FONT_SCALE);
    // In the VALUE column, not beside its label: the label is three characters wide and the note
    // column starts inside it. Every parameter screen in the app puts a value here anyway.
    // Padded to two glyphs: the cursor's box is measured off the text it holds, and a bare "C" would
    // make it shrink and grow as the key cycles past the sharps.
    std::string keyName = pitch_class_name(s.key);
    if (keyName.size() < 2) keyName += " ";
    draw_cursor_cell(c, keyName, valueX, keyY, keyCursor, t.textValue, t);

    // ── Column header ────────────────────────────────────────────────────────────────────────────
    const int columnHeaderY = keyY + ROW_HEIGHT + 14;
    c.draw_text("EN", valueX, columnHeaderY, t.textCursor, CHAR_SPACING, FONT_SCALE);

    // ── Rows 1..12: the degrees ──────────────────────────────────────────────────────────────────
    const int dataStartY = columnHeaderY + ROW_HEIGHT;

    for (int degree = 0; degree < 12; ++degree) {
        const int  row      = degree + 1;
        const int  rowY     = dataStartY + (degree * ROW_HEIGHT);
        const bool isCursor = (row == s.cursorRow);
        const bool isOn     = scale.enabled[static_cast<size_t>(degree)] != 0;

        // ── What is sounding ─────────────────────────────────────────────────────────────────────
        // The same `>` every grid draws for a playback position, in the gap ahead of the EN cell.
        // Here it is not a position but a PITCH: the degree the note coming out of the speaker
        // landed on, which is how the quantizer becomes something you can watch. Play a chromatic
        // run under a five-note scale and the marker visibly skips the rows that are off.
        //
        // ⚠️ A marker on a row that is OFF is not a bug — it means the pitch sounding is not in the
        // scale on screen, because that track is under a different one or its instrument has
        // transposing disabled. Drawing it only on enabled rows would hide exactly that.
        if ((s.soundingMask >> degree_pitch_class(s.key, degree)) & 1u)
            draw_playhead(c, valueX - CHAR_W, rowY, t);

        // The row number is the DEGREE (0-B), the same gutter every grid draws; the note name beside
        // it is what that degree sounds like in the current key, and it moves when the key does.
        draw_cell(c, hex1(degree), labelX, rowY, /*is_cursor=*/false, /*is_selected=*/false,
                  /*is_empty=*/false, isCursor ? t.textCursor : t.textEmpty, t);

        // An out-of-scale note is drawn dim on its own row too, so the screen reads as the set of
        // notes you can play rather than as twelve switches.
        c.draw_text(pitch_class_name(degree_pitch_class(s.key, degree)), noteX, rowY,
                    isOn ? t.textValue : t.textEmpty, CHAR_SPACING, FONT_SCALE);

        draw_cell(c, isOn ? "ON" : "--", valueX, rowY, isCursor, /*is_selected=*/false,
                  /*is_empty=*/!isOn, t.textValue, t);
    }
}

CursorContext ScaleModule::cursor_context(const ScaleState& s) const {
    if (s.cursorRow == SCALE_KEY_ROW) return cc::index_cycle(s.key, 12);

    const int degree = scale_row_degree(s.cursorRow);
    if (degree < 0) return cc::none();

    // ⚠️ The root, and the last degree still on, are READ-ONLY rather than a toggle that refuses:
    // the cell says "you cannot do this" by not lighting, instead of swallowing a press silently.
    const bool isOn = s.scale.enabled[static_cast<size_t>(degree)] != 0;
    if (degree == 0) return cc::read_only();
    if (isOn && songcore::scale_degree_count(s.scale) <= 1) return cc::read_only();

    return cc::toggle_binary(isOn);
}

ScaleInputResult ScaleModule::handle_input(songcore::Scale& scale, int key, int cursor_row,
                                           const InputAction& action) const {
    ScaleInputResult r;
    if (action.type != ActionType::SET_VALUE) return r;

    if (cursor_row == SCALE_KEY_ROW) {
        const int k = songcore::scale_mod12(action.value);
        r.newKey    = k;
        r.modified  = (k != key);
        return r;
    }

    const int degree = scale_row_degree(cursor_row);
    if (degree <= 0) return r;  // no row, or the root, which cannot be turned off

    const int want = action.value != 0 ? 1 : 0;
    if (want == 0 && songcore::scale_degree_count(scale) <= 1) return r;

    r.modified = (scale.enabled[static_cast<size_t>(degree)] != want);
    scale.enabled[static_cast<size_t>(degree)] = want;
    return r;
}

}  // namespace pt::ui
