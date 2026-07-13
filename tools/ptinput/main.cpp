// ptinput — Phase-3 S3 conformance harness for the INPUT LAYER (host tool, no device/NDK).
//
// Proves the C++ port of the input layer is equivalent to the Kotlin original it replaces:
//   * native/ui/cursor.h          — the CursorContext system + the five button handlers
//   * native/ui/selection.h       — the L+B multi-tap CELL/ROW/SCREEN selection machine
//   * native/ui/clipboard.h       — copy / cut / paste / delete over a selection
//   * native/ui/fx_helper.h       — the effect-picker grid
//   * native/ui/modules/*         — each editor's cursor_context() and handle_input()
//
// It reads testdata/units/p3-input.txt (emitted by the JVM P3InputGoldenTest from the REAL Kotlin
// InputController + screen modules + ClipboardManager), and for every `<inputs> => <outputs>` line it
// re-parses the inputs, recomputes the RHS in C++, and byte-compares against the golden RHS.
//
// ── What an EDIT line proves, and why the third part is the one that matters ──────────────────────
//
// Each EDIT line carries the cursor CONTEXT, the RESOLVED ACTION, and the CELL AFTERWARDS. The first
// two alone are a weak test: a module that resolves A on a velocity to SET_VALUE(0x40) and then
// writes 0x40 into the *instrument* field satisfies both of them. Only the cell afterwards catches
// it — so the golden records the cell before and after, and this tool rebuilds it, applies, and
// compares the bytes.
//
// ── The clock ────────────────────────────────────────────────────────────────────────────────────
//
// SEL scripts spell a tap as F (inside the 500 ms multi-tap window) or S (outside it) rather than as
// a timestamp, because the JVM side cannot be handed a fake clock — InputController.handleSelectB
// reads System.currentTimeMillis() itself. The C++ port takes `now_ms` as a parameter precisely so
// that it CAN, and this tool advances a fake clock by 10 ms for F and 600 ms for S. What is under
// test is "tap fast" vs "tap slow", and that is what the encoding says.
//
// Build + run via the tools/ CMake project — this is the `p3-input` ctest, run by CI on every push:
//   cmake -S tools -B tools/build -DCMAKE_BUILD_TYPE=Release
//   cmake --build tools/build --config Release
//   ctest --test-dir tools/build -R p3-input --output-on-failure -C Release
//
// Exit code 0 = all green, 1 = any mismatch.  Linux-port plan §4.7 (Phase 3 S3, the dispatcher).

#include "../../native/ui/clipboard.h"
#include "../../native/ui/cursor.h"
#include "../../native/ui/fx_helper.h"
#include "../../native/ui/selection.h"
#include "../../native/ui/modules/chain_editor.h"
#include "../../native/ui/modules/groove_editor.h"
#include "../../native/ui/modules/phrase_editor.h"
#include "../../native/ui/modules/song_editor.h"
#include "../../native/ui/modules/table_editor.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace pt::ui;
using songcore::Chain;
using songcore::Groove;
using songcore::Note;
using songcore::Phrase;
using songcore::PhraseStep;
using songcore::Project;
using songcore::Table;
using songcore::TableRow;

// ─── small helpers (formatting MUST match P3InputGoldenTest.kt byte-for-byte) ─────────────────────

static bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> v;
    std::string             cur;
    for (char c : s) {
        if (c == sep) {
            v.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    v.push_back(cur);
    return v;
}

/** The whitespace-separated tokens of a line. */
static std::vector<std::string> tokens(const std::string& s) {
    std::vector<std::string> v;
    std::istringstream       in(s);
    std::string              t;
    while (in >> t) v.push_back(t);
    return v;
}

/** The value of `key=` among `toks`, or "" when absent. */
static std::string field(const std::vector<std::string>& toks, const std::string& key) {
    const std::string pre = key + "=";
    for (const std::string& t : toks)
        if (t.rfind(pre, 0) == 0) return t.substr(pre.size());
    return "";
}

static std::string hex2(int v) {
    char b[8];
    std::snprintf(b, sizeof(b), "%02X", v & 0xFF);
    return b;
}

static int from_hex(const std::string& s) { return static_cast<int>(std::strtol(s.c_str(), nullptr, 16)); }
static int from_dec(const std::string& s) { return static_cast<int>(std::strtol(s.c_str(), nullptr, 10)); }

// ─── cell encodings ──────────────────────────────────────────────────────────────────────────────

static std::string note_str(const Note& n) {
    return std::to_string(n.pitch) + "." + std::to_string(n.octave);
}
static Note parse_note(const std::string& s) {
    const std::vector<std::string> f = split(s, '.');
    return Note{from_dec(f[0]), from_dec(f[1])};
}

static std::string step_str(const PhraseStep& s) {
    return "step=" + note_str(s.note) + ":" + hex2(s.volume) + ":" + hex2(s.instrument) + ":" +
           hex2(s.fx1Type) + "/" + hex2(s.fx1Value) + ":" + hex2(s.fx2Type) + "/" +
           hex2(s.fx2Value) + ":" + hex2(s.fx3Type) + "/" + hex2(s.fx3Value);
}

static PhraseStep parse_step(const std::string& spec) {
    const std::vector<std::string> f = split(spec.substr(std::string("step=").size()), ':');
    PhraseStep                     s;
    s.note       = parse_note(f[0]);
    s.volume     = from_hex(f[1]);
    s.instrument = from_hex(f[2]);
    const std::vector<std::string> a = split(f[3], '/');
    const std::vector<std::string> b = split(f[4], '/');
    const std::vector<std::string> c = split(f[5], '/');
    s.fx1Type = from_hex(a[0]); s.fx1Value = from_hex(a[1]);
    s.fx2Type = from_hex(b[0]); s.fx2Value = from_hex(b[1]);
    s.fx3Type = from_hex(c[0]); s.fx3Value = from_hex(c[1]);
    return s;
}

static std::string chain_row_str(const Chain& c, int row) {
    return "crow=" + std::to_string(c.phraseRefs[static_cast<size_t>(row)]) + ":" +
           hex2(c.transposeValues[static_cast<size_t>(row)]);
}

static std::string table_row_str(const TableRow& r) {
    return "trow=" + hex2(r.transpose) + ":" + std::to_string(r.volume) + ":" + hex2(r.fx1Type) +
           "/" + hex2(r.fx1Value) + ":" + hex2(r.fx2Type) + "/" + hex2(r.fx2Value) + ":" +
           hex2(r.fx3Type) + "/" + hex2(r.fx3Value);
}

static TableRow parse_table_row(const std::string& spec) {
    const std::vector<std::string> f = split(spec.substr(std::string("trow=").size()), ':');
    TableRow                       r;
    r.transpose = from_hex(f[0]);
    r.volume    = from_dec(f[1]);
    const std::vector<std::string> a = split(f[2], '/');
    const std::vector<std::string> b = split(f[3], '/');
    const std::vector<std::string> c = split(f[4], '/');
    r.fx1Type = from_hex(a[0]); r.fx1Value = from_hex(a[1]);
    r.fx2Type = from_hex(b[0]); r.fx2Value = from_hex(b[1]);
    r.fx3Type = from_hex(c[0]); r.fx3Value = from_hex(c[1]);
    return r;
}

static std::string song_cell_str(int len, int ref) {
    return "scell=" + std::to_string(len) + ":" + std::to_string(ref);
}

// ─── context / action encodings ──────────────────────────────────────────────────────────────────

static const char* value_type_name(CursorValueType t) {
    switch (t) {
        case CursorValueType::HEX_BYTE:        return "HEX_BYTE";
        case CursorValueType::HEX_NIBBLE:      return "HEX_NIBBLE";
        case CursorValueType::SEMITONE_OFFSET: return "SEMITONE_OFFSET";
        case CursorValueType::NOTE:            return "NOTE";
        case CursorValueType::VOLUME:          return "VOLUME";
        case CursorValueType::GAIN:            return "GAIN";
        case CursorValueType::FREQ:            return "FREQ";
        case CursorValueType::PHRASE_REF:      return "PHRASE_REF";
        case CursorValueType::CHAIN_REF:       return "CHAIN_REF";
        case CursorValueType::INSTRUMENT_REF:  return "INSTRUMENT_REF";
        case CursorValueType::CHARACTER:       return "CHARACTER";
        case CursorValueType::TOGGLE_BINARY:   return "TOGGLE_BINARY";
        case CursorValueType::TOGGLE_TERNARY:  return "TOGGLE_TERNARY";
        case CursorValueType::EFFECT_TYPE:     return "EFFECT_TYPE";
        case CursorValueType::EFFECT_VALUE:    return "EFFECT_VALUE";
        case CursorValueType::EMPTY:           return "EMPTY";
        case CursorValueType::READ_ONLY:       return "READ_ONLY";
        case CursorValueType::NONE:            return "NONE";
    }
    return "?";
}

static std::string caps_str(const CursorContext& c) {
    std::string s;
    s += c.capabilities.canIncrement     ? '+' : '.';
    s += c.capabilities.canDecrement     ? '-' : '.';
    s += c.capabilities.canIncrementFast ? '>' : '.';
    s += c.capabilities.canDecrementFast ? '<' : '.';
    s += c.capabilities.canDelete        ? 'D' : '.';
    s += c.capabilities.canInsert        ? 'I' : '.';
    s += c.capabilities.canCreate        ? 'C' : '.';
    s += c.capabilities.isEmpty          ? 'E' : '.';
    return s;
}

static std::string ctx_str(const CursorContext& c) {
    return std::string("ctx=") + value_type_name(c.valueType) + "|" + caps_str(c) + "|" +
           std::to_string(c.currentValue) + "|" + std::to_string(c.minValue) + "|" +
           std::to_string(c.maxValue) + "|" + std::to_string(c.smallStep) + "|" +
           std::to_string(c.largeStep) + "|" + std::to_string(c.emptyValue) + "|" +
           std::to_string(c.fxSlot) + "|" +
           (c.defaultValue == NO_DEFAULT ? std::string("-") : std::to_string(c.defaultValue));
}

static std::string act_str(const InputAction& a) {
    switch (a.type) {
        case ActionType::NONE:           return "NONE";
        case ActionType::SET_VALUE:      return "SET:" + std::to_string(a.value);
        case ActionType::DELETE:         return "DELETE";
        case ActionType::INSERT_DEFAULT: return "INSERT";
        case ActionType::CREATE_NEW:     return "CREATE";
        case ActionType::NAVIGATE_UP:    return "NAV_UP";
        case ActionType::NAVIGATE_DOWN:  return "NAV_DOWN";
        case ActionType::NAVIGATE_LEFT:  return "NAV_LEFT";
        case ActionType::NAVIGATE_RIGHT: return "NAV_RIGHT";
        case ActionType::COPY:           return "COPY";
        case ActionType::CUT:            return "CUT";
        case ActionType::PASTE:          return "PASTE";
    }
    return "?";
}

/** The five buttons, resolved through the REAL generic handlers. */
static InputAction resolve(const std::string& btn, const CursorContext& c) {
    if (btn == "A")  return on_a(c);
    if (btn == "B")  return on_b(c);
    if (btn == "AL") return on_a_left(c);
    if (btn == "AR") return on_a_right(c);
    if (btn == "AB") return on_a_b(c);
    return InputAction::none();
}

// ─── EDIT ────────────────────────────────────────────────────────────────────────────────────────

static const PhraseEditorModule kPhrase{};
static const ChainEditorModule  kChain{};
static const SongEditorModule   kSong{};
static const TableModule        kTable{};
static const GrooveModule       kGroove{};

static std::string recompute_edit(const std::vector<std::string>& toks, std::string& err) {
    const std::string scr = field(toks, "scr");
    const int         col = from_dec(field(toks, "col"));
    const std::string btn = field(toks, "btn");

    if (scr == "PHRASE") {
        const int row = 3;  // the golden's fixed probe row
        Phrase    phrase(0);
        phrase.steps[static_cast<size_t>(row)] = parse_step("step=" + field(toks, "step"));

        PhraseEditorState st{phrase};
        st.cursorRow    = row;
        st.cursorColumn = col;
        const CursorContext ctx = kPhrase.cursor_context(st);
        const InputAction   act = resolve(btn, ctx);
        kPhrase.handle_input(phrase, row, col, act);
        return ctx_str(ctx) + " act=" + act_str(act) + " " +
               step_str(phrase.steps[static_cast<size_t>(row)]);
    }
    if (scr == "CHAIN") {
        const int row = 5;
        Chain     chain(0);
        const std::vector<std::string> f = split(field(toks, "crow"), ':');
        chain.phraseRefs[static_cast<size_t>(row)]      = from_dec(f[0]);
        chain.transposeValues[static_cast<size_t>(row)] = from_hex(f[1]);

        ChainEditorState st{chain};
        st.cursorRow    = row;
        st.cursorColumn = col;
        const CursorContext ctx = kChain.cursor_context(st);
        const InputAction   act = resolve(btn, ctx);
        kChain.handle_input(chain, row, col, act);
        return ctx_str(ctx) + " act=" + act_str(act) + " " + chain_row_str(chain, row);
    }
    if (scr == "SONG") {
        const int row = 9;
        // ⚠️ NOT `Project p;` — songcore's default Project has EMPTY pools (model.h: "filled by
        // factory"), because project_io needs a blank one to parse into. Kotlin's default ctor IS the
        // factory, so the two only agree through make_default_project().
        Project p = songcore::make_default_project();

        const std::vector<std::string> f   = split(field(toks, "scell"), ':');
        const int                      len = from_dec(f[0]);
        const int                      ref = from_dec(f[1]);

        // The column IS the track and it is 1-based; col 0 is the read-only gutter, and the golden
        // probes it too (the module must answer `none()` there rather than index track −1).
        const int trackIndex = (col - 1) < 0 ? 0 : ((col - 1) > 7 ? 7 : (col - 1));
        songcore::Track& track = p.tracks[static_cast<size_t>(trackIndex)];
        track.chainRefs.assign(static_cast<size_t>(len), -1);
        if (len > 9) track.chainRefs[9] = ref;

        SongEditorState st{p};
        st.cursorRow   = row;
        st.cursorTrack = col;
        const CursorContext ctx = kSong.cursor_context(st);
        const InputAction   act = resolve(btn, ctx);
        kSong.handle_input(p, row, col, act);

        const int n = static_cast<int>(track.chainRefs.size());
        return ctx_str(ctx) + " act=" + act_str(act) + " " +
               song_cell_str(n, (9 < n) ? track.chainRefs[9] : -1);
    }
    if (scr == "TABLE") {
        const int row = 4;
        Table     table(0);
        table.rows[static_cast<size_t>(row)] = parse_table_row("trow=" + field(toks, "trow"));

        TableState st{table};
        st.cursorRow    = row;
        st.cursorColumn = col;
        const CursorContext ctx = kTable.cursor_context(st);
        const InputAction   act = resolve(btn, ctx);
        kTable.handle_input(table, row, col, act);
        return ctx_str(ctx) + " act=" + act_str(act) + " " +
               table_row_str(table.rows[static_cast<size_t>(row)]);
    }
    if (scr == "GROOVE") {
        const int row = 2;
        Groove    groove(0);
        groove.steps[static_cast<size_t>(row)] = from_dec(field(toks, "grow"));

        GrooveState st{groove};
        st.cursorRow    = row;
        st.cursorColumn = col;
        const CursorContext ctx = kGroove.cursor_context(st);
        const InputAction   act = resolve(btn, ctx);
        kGroove.handle_input(groove, row, col, act);
        return ctx_str(ctx) + " act=" + act_str(act) + " grow=" +
               std::to_string(groove.steps[static_cast<size_t>(row)]);
    }
    err = "unknown screen " + scr;
    return "";
}

// ─── SEL ─────────────────────────────────────────────────────────────────────────────────────────

static std::string recompute_sel(const std::vector<std::string>& toks, std::string& err) {
    const std::vector<std::string> cur = split(field(toks, "cur"), ',');
    const std::vector<std::string> mx  = split(field(toks, "max"), ',');
    const int row    = from_dec(cur[0]);
    const int col    = from_dec(cur[1]);
    const int maxCol = from_dec(mx[0]);
    const int maxRow = from_dec(mx[1]);

    Selection sel;
    // The fake clock. F advances it 10 ms (inside the 500 ms window), S advances 600 (outside).
    // This is the whole reason handle_select_b takes `now_ms` instead of reading it.
    long long now = 1000;

    for (const std::string& op : split(field(toks, "script"), ',')) {
        if (op == "LB:F") {
            now += 10;
            sel.handle_select_b(now, row, col, maxCol, maxRow);
        } else if (op == "LB:S") {
            now += 600;
            sel.handle_select_b(now, row, col, maxCol, maxRow);
        } else if (op == "X") {
            sel.exit();
        } else if (op.rfind("E:", 0) == 0) {
            sel.expand(op.substr(2).c_str(), maxRow, maxCol);
        } else {
            err = "unknown op " + op;
            return "";
        }
    }

    const char* scopeName = "NONE";
    switch (sel.scope) {
        case SelectionScope::CELL:   scopeName = "CELL";   break;
        case SelectionScope::ROW:    scopeName = "ROW";    break;
        case SelectionScope::SCREEN: scopeName = "SCREEN"; break;
        case SelectionScope::NONE:   scopeName = "NONE";   break;
    }

    // Kotlin nulls start/end on exit and prints "-"; the C++ struct zeroes them and gates on `active`.
    const std::string startStr =
        sel.active ? std::to_string(sel.start.row) + "," + std::to_string(sel.start.column) : "-";
    const std::string endStr =
        sel.active ? std::to_string(sel.end.row) + "," + std::to_string(sel.end.column) : "-";
    std::string boundsStr = "-";
    if (sel.active) {
        const SelectionBounds b = sel.bounds();
        boundsStr = std::to_string(b.topLeftRow) + "," + std::to_string(b.topLeftColumn) + "-" +
                    std::to_string(b.bottomRightRow) + "," + std::to_string(b.bottomRightColumn);
    }
    const std::string info = sel.info().empty() ? "-" : sel.info();

    return std::string("scope=") + scopeName + " active=" + (sel.active ? "1" : "0") +
           " start=" + startStr + " end=" + endStr + " bounds=" + boundsStr + " info=" + info;
}

// ─── CLIP ────────────────────────────────────────────────────────────────────────────────────────
//
// The seeded grids: every row distinct, so a mis-anchored paste is visible rather than lucky. They
// must match P3InputGoldenTest's `seeded*()` builders exactly — a divergence here shows up as a
// mismatch in the FIRST dump row, which is what makes it easy to spot.

static Phrase seeded_phrase() {
    Phrase p(0);
    for (int i = 0; i < 16; ++i) {
        PhraseStep s;
        s.note       = Note{i % 12, 2 + i / 12};
        s.volume     = 0x10 + i;
        s.instrument = 0x20 + i;
        s.fx1Type    = songcore::FX_ARC;    s.fx1Value = 0x30 + i;
        s.fx2Type    = songcore::FX_PAN;    s.fx2Value = 0x40 + i;
        s.fx3Type    = songcore::FX_VOLUME; s.fx3Value = 0x50 + i;
        p.steps[static_cast<size_t>(i)] = s;
    }
    return p;
}

static Chain seeded_chain() {
    Chain c(0);
    for (int i = 0; i < 16; ++i) {
        c.phraseRefs[static_cast<size_t>(i)]      = (i % 4 == 3) ? -1 : (0x10 + i);
        c.transposeValues[static_cast<size_t>(i)] = 0x20 + i;
    }
    return c;
}

static Table seeded_table() {
    Table t(0);
    for (int i = 0; i < 16; ++i) {
        TableRow r;
        r.transpose = 0x10 + i;
        r.volume    = (i % 5 == 4) ? -1 : (0x20 + i);
        r.fx1Type   = songcore::FX_TBL; r.fx1Value = 0x30 + i;
        r.fx2Type   = songcore::FX_HOP; r.fx2Value = 0x40 + i;
        r.fx3Type   = songcore::FX_PIT; r.fx3Value = 0x50 + i;
        t.rows[static_cast<size_t>(i)] = r;
    }
    return t;
}

static Project seeded_project() {
    Project p = songcore::make_default_project();
    for (int t = 0; t < 8; ++t) {
        songcore::Track& track = p.tracks[static_cast<size_t>(t)];
        track.chainRefs.clear();
        for (int r = 0; r < 16; ++r) track.chainRefs.push_back((r % 3 == 2) ? -1 : (t * 16 + r));
    }
    return p;
}

static std::string row_label(int i) {
    char b[8];
    std::snprintf(b, sizeof(b), "  R%02d ", i);
    return b;
}

static std::string paste_str(const PasteResult& r) {
    switch (r.kind) {
        case PasteResult::Kind::NO_CLIPBOARD: return "NO_CLIPBOARD";
        case PasteResult::Kind::SUCCESS:      return "SUCCESS:" + std::to_string(r.itemsPasted);
        case PasteResult::Kind::WRONG_SCREEN: return "WRONG_SCREEN";
    }
    return "?";
}

static ScreenType screen_of(const std::string& s) {
    if (s == "PHRASE") return ScreenType::PHRASE;
    if (s == "CHAIN")  return ScreenType::CHAIN;
    if (s == "SONG")   return ScreenType::SONG;
    if (s == "TABLE")  return ScreenType::TABLE;
    return ScreenType::PHRASE;
}

/** A CLIP case: the headline RHS, plus the grid dump that follows it. */
struct ClipOut {
    std::string              rhs;
    std::vector<std::string> dump;
};

static ClipOut recompute_clip(const std::vector<std::string>& toks, std::string& err) {
    ClipOut           out;
    const std::string scr = field(toks, "scr");
    const std::string op  = field(toks, "op");

    // Every case builds its own clipboard and its own grid. Carrying one across cases would make the
    // tool's answer depend on the ORDER the golden happens to list them in, and a case that only
    // passed because the previous one left the right thing behind is not a case that proves anything.
    Clipboard gClip;

    auto ints = [](const std::string& csv) {
        std::vector<int> v;
        for (const std::string& s : split(csv, ',')) v.push_back(from_dec(s));
        return v;
    };

    if (op == "copy-paste" || op == "cut-paste") {
        const std::vector<int> src = ints(field(toks, "src"));
        const std::vector<int> dst = ints(field(toks, "dst"));
        gClip.clear();

        if (scr == "PHRASE") {
            Project p = songcore::make_default_project();
            p.phrases[0] = seeded_phrase();
            if (op == "copy-paste") gClip.copy_phrase_steps(p, 0, src[0], src[1], src[2], src[3]);
            else                    gClip.cut_phrase_steps(p, 0, src[0], src[1], src[2], src[3]);
            const PasteResult r = gClip.paste(p, ScreenType::PHRASE, 0, dst[0], dst[1]);
            out.rhs = paste_str(r) + " info=" + gClip.info();
            for (int i = 0; i < 16; ++i)
                out.dump.push_back(row_label(i) + step_str(p.phrases[0].steps[static_cast<size_t>(i)]));
        } else if (scr == "CHAIN") {
            Project p = songcore::make_default_project();
            p.chains[0] = seeded_chain();
            if (op == "copy-paste") gClip.copy_chain_rows(p, 0, src[0], src[1], src[2], src[3]);
            else                    gClip.cut_chain_rows(p, 0, src[0], src[1], src[2], src[3]);
            const PasteResult r = gClip.paste(p, ScreenType::CHAIN, 0, dst[0], dst[1]);
            out.rhs = paste_str(r) + " info=" + gClip.info();
            for (int i = 0; i < 16; ++i) out.dump.push_back(row_label(i) + chain_row_str(p.chains[0], i));
        } else if (scr == "SONG") {
            Project p = seeded_project();
            if (op == "copy-paste") gClip.copy_song_cells(p, src[0], src[1], src[2], src[3]);
            else                    gClip.cut_song_cells(p, src[0], src[1], src[2], src[3]);
            const PasteResult r = gClip.paste(p, ScreenType::SONG, 0, dst[0], dst[1]);
            out.rhs = paste_str(r) + " info=" + gClip.info();
            for (int i = 0; i < 16; ++i) {
                std::string cells;
                for (int t = 0; t < 8; ++t) {
                    const std::vector<int>& cr = p.tracks[static_cast<size_t>(t)].chainRefs;
                    if (t) cells += ",";
                    cells += std::to_string(i < static_cast<int>(cr.size()) ? cr[static_cast<size_t>(i)] : -1);
                }
                out.dump.push_back(row_label(i) + cells);
            }
        } else if (scr == "TABLE") {
            Project p = songcore::make_default_project();
            p.tables[0] = seeded_table();
            if (op == "copy-paste") gClip.copy_table_rows(p, 0, src[0], src[1], src[2], src[3]);
            else                    gClip.cut_table_rows(p, 0, src[0], src[1], src[2], src[3]);
            const PasteResult r = gClip.paste(p, ScreenType::TABLE, 0, dst[0], dst[1]);
            out.rhs = paste_str(r) + " info=" + gClip.info();
            for (int i = 0; i < 16; ++i)
                out.dump.push_back(row_label(i) + table_row_str(p.tables[0].rows[static_cast<size_t>(i)]));
        } else {
            err = "unknown clip screen " + scr;
        }
        return out;
    }

    if (op == "delete") {
        const std::vector<int> sel = ints(field(toks, "sel"));
        gClip.clear();
        int n = -1;
        if (scr == "PHRASE") {
            Project p = songcore::make_default_project();
            p.phrases[0] = seeded_phrase();
            n = gClip.delete_phrase_steps(p, 0, sel[0], sel[1], sel[2], sel[3]);
            out.rhs = "n=" + std::to_string(n);
            for (int i = 0; i < 16; ++i)
                out.dump.push_back(row_label(i) + step_str(p.phrases[0].steps[static_cast<size_t>(i)]));
        } else if (scr == "CHAIN") {
            Project p = songcore::make_default_project();
            p.chains[0] = seeded_chain();
            n = gClip.delete_chain_rows(p, 0, sel[0], sel[1], sel[2], sel[3]);
            out.rhs = "n=" + std::to_string(n);
            for (int i = 0; i < 16; ++i) out.dump.push_back(row_label(i) + chain_row_str(p.chains[0], i));
        } else if (scr == "SONG") {
            Project p = seeded_project();
            n = gClip.delete_song_cells(p, sel[0], sel[1], sel[2], sel[3]);
            out.rhs = "n=" + std::to_string(n);
            for (int i = 0; i < 16; ++i) {
                std::string cells;
                for (int t = 0; t < 8; ++t) {
                    const std::vector<int>& cr = p.tracks[static_cast<size_t>(t)].chainRefs;
                    if (t) cells += ",";
                    cells += std::to_string(i < static_cast<int>(cr.size()) ? cr[static_cast<size_t>(i)] : -1);
                }
                out.dump.push_back(row_label(i) + cells);
            }
        } else if (scr == "TABLE") {
            Project p = songcore::make_default_project();
            p.tables[0] = seeded_table();
            n = gClip.delete_table_rows(p, 0, sel[0], sel[1], sel[2], sel[3]);
            out.rhs = "n=" + std::to_string(n);
            for (int i = 0; i < 16; ++i)
                out.dump.push_back(row_label(i) + table_row_str(p.tables[0].rows[static_cast<size_t>(i)]));
        } else {
            err = "unknown clip screen " + scr;
        }
        return out;
    }

    if (op == "paste-foreign") {
        // A PHRASE clip aimed at CHAIN / SONG / TABLE. The copy is redone per line rather than left
        // over from the previous one — same reasoning as the fresh Clipboard above.
        Project p = songcore::make_default_project();
        p.phrases[0] = seeded_phrase();
        gClip.copy_phrase_steps(p, 0, 0, 1, 1, 2);
        const PasteResult r = gClip.paste(p, screen_of(scr), 0, 0, 1);
        out.rhs             = paste_str(r);
        return out;
    }

    if (op == "paste-empty") {
        gClip.clear();
        Project           p = songcore::make_default_project();
        const PasteResult r = gClip.paste(p, screen_of(scr), 0, 0, 1);
        out.rhs = paste_str(r) + " info=" + (gClip.info().empty() ? "-" : gClip.info());
        return out;
    }

    err = "unknown clip op " + op;
    return out;
}

// ─── FXH ─────────────────────────────────────────────────────────────────────────────────────────

static std::string recompute_fxh(const std::vector<std::string>& toks, std::string& err) {
    FxHelperState s     = fx_helper_opened_at(from_dec(field(toks, "open")));
    const std::string m = field(toks, "moves");

    if (m != "-") {
        for (const std::string& mv : split(m, ',')) {
            if (mv == "U")      fx_move_up(s);
            else if (mv == "D") fx_move_down(s);
            else if (mv == "L") fx_move_left(s);
            else if (mv == "R") fx_move_right(s);
            else {
                err = "unknown move " + mv;
                return "";
            }
        }
    }
    return "row=" + std::to_string(s.cursorRow) + " col=" + std::to_string(s.cursorCol) +
           " idx=" + std::to_string(s.cursor_index()) + " code=" + hex2(s.selected_effect_code());
}

// ─── main ────────────────────────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: ptinput <testdata/units/p3-input.txt>\n";
        return 2;
    }
    const std::string goldenPath = argv[1];

    std::string text;
    if (!read_file(goldenPath, text)) {
        std::cerr << "[FAIL] cannot read " << goldenPath
                  << " — run the JVM P3InputGoldenTest first to generate it.\n";
        return 1;
    }

    // Whole-file, because a CLIP case owns the indented `  R##` lines that follow it.
    std::vector<std::string> lines;
    {
        std::istringstream in(text);
        std::string        l;
        while (std::getline(in, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();  // tolerate CRLF
            lines.push_back(l);
        }
    }

    int                        total = 0, failures = 0;
    std::map<std::string, int> perKind;

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.empty() || line[0] == '#' || line[0] == ' ') continue;  // ' ' = a dump row

        const size_t sep = line.find(" => ");
        if (sep == std::string::npos) {
            std::cerr << "[FAIL] line " << (i + 1) << ": no ' => ' separator: " << line << "\n";
            ++failures;
            continue;
        }
        const std::string lhs       = line.substr(0, sep);
        const std::string rhsGolden = line.substr(sep + 4);

        const std::vector<std::string> toks = tokens(lhs);
        const std::string              kind = toks[0];

        std::string err;
        std::string rhsCpp;
        std::vector<std::string> dumpCpp;

        if (kind == "EDIT")      rhsCpp = recompute_edit(toks, err);
        else if (kind == "SEL")  rhsCpp = recompute_sel(toks, err);
        else if (kind == "FXH")  rhsCpp = recompute_fxh(toks, err);
        else if (kind == "CLIP") {
            const ClipOut o = recompute_clip(toks, err);
            rhsCpp          = o.rhs;
            dumpCpp         = o.dump;
        } else {
            err = "unknown kind " + kind;
        }

        ++total;
        ++perKind[kind];

        if (!err.empty()) {
            std::cerr << "[FAIL] line " << (i + 1) << " (" << kind << "): " << err << "\n";
            ++failures;
            continue;
        }
        if (rhsCpp != rhsGolden) {
            std::cerr << "[FAIL] line " << (i + 1) << " (" << kind << ") parity mismatch:\n"
                      << "    input : " << lhs << "\n"
                      << "    golden: " << rhsGolden << "\n"
                      << "    c++   : " << rhsCpp << "\n";
            ++failures;
            continue;
        }

        // The grid dump beneath a CLIP case, row by row — this is where a mis-anchored paste shows.
        for (size_t d = 0; d < dumpCpp.size(); ++d) {
            const size_t gi = i + 1 + d;
            const std::string golden = (gi < lines.size()) ? lines[gi] : std::string("<missing>");
            if (golden != dumpCpp[d]) {
                std::cerr << "[FAIL] line " << (gi + 1) << " (CLIP dump) mismatch:\n"
                          << "    case  : " << lhs << "\n"
                          << "    golden: " << golden << "\n"
                          << "    c++   : " << dumpCpp[d] << "\n";
                ++failures;
            }
        }
    }

    std::cout << "== Phase-3 S3 input-layer parity (C++ vs JVM golden) ==\n";
    for (const auto& kv : perKind) std::cout << "  " << kv.first << ": " << kv.second << " case(s)\n";
    std::cout << "checked " << total << " case(s)\n\n";
    std::cout << (failures == 0 ? "ALL GREEN" : "FAILURES: " + std::to_string(failures)) << "\n";
    return failures == 0 ? 0 : 1;
}
