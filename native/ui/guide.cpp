#include "ui/guide.h"

#include <array>
#include <cstring>
#include <iterator>

#include "ui/app_state.h"
#include "ui/canvas.h"
#include "ui/clipboard.h"
#include "ui/help_text.h"
#include "ui/helpers.h"
#include "ui/navigation.h"
#include "ui/overlays.h"
#include "ui/song_pointer.h"
#include "ui/value_hint.h"

namespace pt::ui {

namespace {

using Lines = std::array<std::string, 2>;

Lines lines_of(const char* const pair[2]) { return {pair[0], pair[1]}; }

bool starts_with(const char* line, const char* prefix) {
    return std::strncmp(line, prefix, std::strlen(prefix)) == 0;
}

// "R+→ opens this chain": a control that only moves between screens. The R page lists those itself.
bool moves_between_screens(const char* line) {
    return starts_with(line, "R+") && detail::help_arrow_at(line + 2);
}

// Adds to `out` the controls of `e` that open with `chord` ("B+"), until it holds `limit`.
void add_chord_lines(Lines& out, size_t& count, size_t limit, const HelpEntry& e, const char* chord) {
    for (const char* key : e.keys) {
        if (count >= limit) return;
        if (key == nullptr || !starts_with(key, chord) || moves_between_screens(key)) continue;
        if (count > 0 && out[0] == key) continue;   // the cell and its screen may say the same
        out[count++] = key;
    }
}

// The cell's own, then its screen's.
Lines chord_lines(const HelpEntry& cell, const HelpEntry& screen, const char* chord, size_t limit = 2) {
    Lines  out;
    size_t count = 0;
    add_chord_lines(out, count, limit, cell, chord);
    add_chord_lines(out, count, limit, screen, chord);
    return out;
}

// Four of these fit on a line with their arrows.
const char* short_screen_name(ScreenType screen) {
    switch (screen) {
        case ScreenType::INSTRUMENT: return "INST";
        case ScreenType::INST_POOL:  return "POOL";
        default:                     return screen_label(screen);
    }
}

// Where R+D-PAD goes from here: "←CHAIN →INST ↑GROOVE ↓MIXER". A direction that leads nowhere is left
// out; empty on a popup and under a layer, where none does. Asked of the functions that move.
std::string screen_moves(const AppState& s) {
    if (top_overlay(s) != nullptr) return {};
    const NavState from = nav_state_of(s);
    const struct {
        const char* arrow;
        NavResult   to;
        bool        gated;   // NAV = SONG lets R+RIGHT in only through a filled cell
    } moves[] = {
        {"←", navigate_left(from), false},
        {"→", navigate_right(from), true},
        {"↑", navigate_up(from), false},
        {"↓", navigate_down(from), false},
    };
    std::string out;
    for (const auto& m : moves) {
        if (m.to.screen == s.currentScreen) continue;
        if (!out.empty()) out += ' ';
        out += m.arrow;
        if (m.gated && !song_relative_entry_allowed(s, m.to.screen))
            out += m.to.screen == ScreenType::CHAIN ? "no chain" : "no phrase";
        else
            out += short_screen_name(m.to.screen);
    }
    return out;
}

// "2x3 from PHRASE": what L+A would paste, and the screen it was copied on.
std::string clipboard_words(const Clipboard* clip) {
    if (clip == nullptr || !clip->has_data()) return "Empty";
    const char* from = "";
    switch (clip->type()) {
        case ClipboardType::PHRASE_STEPS: from = "PHRASE"; break;
        case ClipboardType::CHAIN_ROWS:   from = "CHAIN";  break;
        case ClipboardType::SONG_CELLS:   from = "SONG";   break;
        case ClipboardType::TABLE_ROWS:   from = "TABLE";  break;
    }
    return std::to_string(clip->width()) + "x" + std::to_string(clip->height()) + " from " + from;
}

const GuideScreenLines* l_lines_for(HelpTopic screen) {
    for (const GuideScreenLines& row : GUIDE_L)
        if (row.screen == screen) return &row;
    return nullptr;
}

}  // namespace

bool guide_up(const AppState& s) { return s.theme.visualizerType == VisualizerType::GUIDE; }

GuideLines guide_lines(const Canvas& c, const AppState& s) {
    const HelpTopic  screenTopic = help_screen(s);
    const HelpEntry& cell        = help_entry(help_topic(s));
    const HelpEntry& screen      = help_entry(screenTopic);
    // LIVE changes what START does on SONG, and nowhere else.
    const bool live = s.liveMode && s.currentScreen == ScreenType::SONG && top_overlay(s) == nullptr;

    // Nothing held: how to fill an empty cell, else the two controls the entry puts first.
    std::string title = live ? GUIDE_LIVE_TITLE : cell.line1;
    Lines       keys;
    if (live) {
        keys = lines_of(GUIDE_LIVE);
    } else {
        const bool         empty = s.cellUnderCursor && s.cellUnderCursor().capabilities.isEmpty;
        const char* const* idle  = empty && cell.guideEmpty[0] != nullptr ? cell.guideEmpty
                                   : cell.guide[0] != nullptr              ? cell.guide
                                                                           : cell.keys;
        keys = {idle[0] != nullptr ? idle[0] : "", idle[1] != nullptr ? idle[1] : ""};
    }

    const ValueHint hint = held_value_hint(c, s);
    if (s.selection.active) {
        const SelectionBounds b = s.selection.bounds();
        title = "SELECTION: " + std::to_string(b.width()) + "x" + std::to_string(b.height());
        keys  = lines_of(GUIDE_SELECTION);
    } else if (s.lHeld && s.rHeld) {
        // L+R clears one thing at a time; a screen with nothing to clear says nothing new.
        const Lines both = chord_lines(cell, screen, "L+R");
        if (!both[0].empty()) keys = both;
    } else if (s.rHeld) {
        const std::string moves = screen_moves(s);
        const Lines       chord = chord_lines(cell, screen, "R+", moves.empty() ? 2 : 1);
        if (!moves.empty()) {
            title = GUIDE_R_TITLE;
            keys  = {moves, live ? GUIDE_LIVE_R : chord[0]};
        } else if (!chord[0].empty()) {
            keys = chord;
        } else if (top_overlay(s) == nullptr) {
            keys = lines_of(GUIDE_R_NOWHERE);
        }
    } else if (s.lHeld) {
        const GuideScreenLines* row = l_lines_for(screenTopic);
        if (row == nullptr) {
            keys = lines_of(GUIDE_L_ELSEWHERE);
        } else {
            if (row->clipboard) title = "CLIPBOARD: " + clipboard_words(s.clipboard);
            keys = lines_of(live ? GUIDE_LIVE_L : row->line);
        }
    } else if (s.bHeld) {
        const Lines chord = chord_lines(cell, screen, "B+");
        if (!chord[0].empty()) keys = chord;
    } else if (!hint.head.empty()) {
        keys = {hint.head, hint.detail};
    }

    const Theme& t = s.theme;
    GuideLines   g;
    g.line[0] = s.statusMessage.empty()
                    ? GuideLines::Line{title, t.textTitle}
                    : GuideLines::Line{status_text(c, s.statusMessage, s.statusNameAt),
                                       s.statusSuccess ? t.vizWave : Argb{0xFFFF0000}};
    g.line[1] = {keys[0], t.textValue, t.vizWave};
    g.line[2] = {keys[1], t.textValue, t.vizWave};
    for (GuideLines::Line& line : g.line) line.text = Canvas::clip_text(line.text, HELP_MAX_CHARS);
    return g;
}

}  // namespace pt::ui
