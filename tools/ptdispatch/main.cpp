// ─── ptdispatch — the DISPATCHER, which ptinput is structurally blind to ─────────────────────────
//
// ptinput proves each MODULE matches Kotlin: given a cursor, the context, the action and the cell it
// writes are byte-identical to the Kotlin original's. Nothing in it proves the modules are WIRED UP.
// Whether the MIXER's cursor can reach its cells at all, whether SELECT toggles delay sync, whether
// START on the mixer plays the song or the phrase — none of that is a module, it is the join between
// InputDispatcher, cursor_move and navigation, and ptinput cannot see any of it. (Measured, not
// argued: revert S5's START fix and ptinput stays ALL GREEN while this goes red and names it.)
//
// ⚠️ **THIS IS NOT A CONFORMANCE TOOL, AND THE DIFFERENCE MATTERS.** The other nine byte-compare
// against a golden RECORDED FROM THE KOTLIN CODE THEY REPLACE; they encode what Kotlin *does*. These
// are hand-written assertions: they encode what the author BELIEVED Kotlin does, having read it. That
// is a weaker claim, and it can be confidently wrong — while writing this file the BFS below was
// asserted to find 12 mixer cells; the real answer is 14, and the code was right where the assertion
// was not. So: transcribe from the Kotlin source (each block below names the function it came from),
// never from memory, and treat a disagreement between this file and the code as an open question
// rather than a bug report.
//
// It exists as a standing test rather than the throwaway S1–S4 used because the condition S3 named for
// building one has been met — "worth opening only if the dispatcher starts regressing". It has, twice:
// S5 found that START on MIXER/EFFECTS played the current phrase where Android plays the song (an S3
// bug, whose own comment said otherwise), and that entering INSTRUMENT / MODS / INST.POOL did not
// refresh their cursors (an S4 bug). Both are dispatcher-level; neither was visible to any of the nine.
// The properly-recorded golden S3 describes — driven from Kotlin's AppInputDispatcher, which is
// entangled with ~60 Compose state refs — is still worth a session of its own, and this does not
// replace it.
//
// The engine is NULL throughout. SongcoreHost null-checks it everywhere, so the whole editing path
// runs with no audio device, no window and no SDL — which is itself standing proof that pt-ui has not
// grown a platform dependency.
//
// ⚠️ The FILE SYSTEM, since S6a, is NOT null — and cannot be. An engine is optional because a document
// edit does not need one (S4's harness proved exactly that); a file browser without a filesystem is
// not a degraded browser, it is an empty box, and every assertion about listing, sorting, renaming or
// deleting would pass vacuously. So this builds a REAL directory tree in the system temp dir, drives
// the real browser over it, and removes it on the way out. The one thing it must never do is touch the
// user's own PocketTracker folder.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "audio-engine.h"
#include "songcore/host.h"
#include "songcore/wav_writer.h"   // the cue-point round trip (S6b)
#include "ui/app_state.h"
#include "ui/cursor_move.h"
#include "ui/input_dispatcher.h"
#include "ui/navigation.h"
#include "ui/platform_caps.h"
#include "ui/canvas.h"           // §27(a) — the pixel check RENDERS
#include "ui/layout.h"           // §27(a) — …through the same TrackerLayout the shell draws through
#include "ui/lifecycle.h"        // §28   — the crash-recovery autosave (S10)
#include "ui/settings_row_layout.h"
#include "ui/settings_store.h"
#include "ui/std_filesystem.h"
#include "ui/theme_io.h"         // §27 — the .ptt round trip

using namespace pt::ui;

static int checks = 0, failures = 0;

static void ok(bool cond, const std::string& what) {
    ++checks;
    if (!cond) {
        ++failures;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

static void eq(int got, int want, const std::string& what) {
    ++checks;
    if (got != want) {
        ++failures;
        std::printf("  [FAIL] %s: got %d, want %d\n", what.c_str(), got, want);
    }
}

/** The mixer cursor as one printable token, for readable failures. */
static std::string mix_pos(const AppState& s) {
    return "(" + std::to_string(s.mixerMasterRow) + "," + std::to_string(s.mixerCursorColumn) + ")";
}

/** The names in the browser's list, joined — the whole listing as one comparable token. */
static std::string listing(const FileBrowserState& b) {
    std::string out;
    for (const BrowserItem& it : b.items) {
        if (!out.empty()) out += ",";
        out += it.displayName;
    }
    return out;
}

static void eqs(const std::string& got, const std::string& want, const std::string& what) {
    ++checks;
    if (got != want) {
        ++failures;
        std::printf("  [FAIL] %s:\n           got  %s\n           want %s\n", what.c_str(),
                    got.c_str(), want.c_str());
    }
}

/** How many rows the browser's anchor..cursor range currently covers — asked through its own predicate. */
static int selected_count(const AppState& s) {
    int n = 0;
    for (int i = 0; i < static_cast<int>(s.fileBrowser.items.size()); ++i)
        if (s.fileBrowser.is_selected(i)) ++n;
    return n;
}

namespace fs = std::filesystem;

/**
 * A throwaway directory tree, built fresh each run so the assertions below can be exact rather than
 * "whatever happens to be on this machine".
 *
 * ⚠️ The MTIMES are set explicitly and two of them are EQUAL, which is the point of the fixture rather
 * than an incidental detail: equal keys are what make the sort's STABILITY observable. `sort_items` is
 * a `std::stable_sort` over a list `build_item_list` already put in name order — so two files written
 * in the same second must come out in NAME order under a DATE sort. A plain `std::sort` would order
 * them arbitrarily and differently per toolchain, and only a tie can see the difference.
 */
struct TempTree {
    fs::path root;

    TempTree() {
        root = fs::temp_directory_path() / "ptdispatch-s6a";
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root / "Samples" / "Kicks", ec);
        fs::create_directories(root / "Instruments", ec);
        fs::create_directories(root / "Soundfonts", ec);

        // Names deliberately out of every natural order, so NAME/SIZE/DATE each pick a different one.
        write(root / "Samples" / "zeta.wav", 3000);
        write(root / "Samples" / "alpha.wav", 1000);
        write(root / "Samples" / "mid.wav", 2000);
        write(root / "Samples" / "notes.txt", 10);      // filtered OUT by the sample extension set
        write(root / "Samples" / ".hidden.wav", 10);    // filtered OUT because it is hidden

        using namespace std::chrono;
        const auto now = fs::file_time_type::clock::now();
        std::error_code ec2;
        fs::last_write_time(root / "Samples" / "zeta.wav",  now - hours(3), ec2);   // oldest
        fs::last_write_time(root / "Samples" / "alpha.wav", now - hours(1), ec2);   // ⚠️ same as mid
        fs::last_write_time(root / "Samples" / "mid.wav",   now - hours(1), ec2);   // ⚠️ same as alpha
    }

    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    static void write(const fs::path& p, size_t bytes) {
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f << std::string(bytes, 'x');
    }
};

int main() {
    songcore::SongcoreHost host(nullptr, 44100);   // ⚠️ no engine, on purpose

    TempTree      tree;
    StdFileSystem fs_impl(tree.root.generic_string());

    AppState state;

    // ── 0. The freshly-constructed state boots on SONG, as Android does ──────────────────────────
    //
    // TrackerController.kt:41 — `var currentScreen = ScreenType.SONG`, and no startup navigation
    // overrides it. The port said PHRASE in two places (the app_state.h default and a main.cpp
    // re-assignment), both S1 relics from when PHRASE was the only screen that existed. The check
    // reads the DEFAULT, before anything below drives a navigation — exactly the assertion that did
    // not exist while both sites quietly agreed with each other.
    ok(state.currentScreen == ScreenType::SONG, "BOOT: a fresh AppState starts on SONG, as Android does");

    state.project = &host.edit_project();
    InputDispatcher dispatch(state, host, fs_impl);

    // ── 1. The MIXER's cursor is a SHAPE — can it reach every editable cell, and is it a trap? ────
    //
    // Rows 2 and 3 exist only in column 8. The interesting question is not "does DOWN work" but
    // "is every cell reachable, and can the cursor get stuck anywhere it should not". A BFS over the
    // four D-pad moves answers both, from the entry state the REFRESH reset guarantees: (0,0).
    {
        std::set<std::pair<int, int>> seen;
        std::vector<std::pair<int, int>> frontier{{0, 0}};
        seen.insert({0, 0});

        while (!frontier.empty()) {
            const auto here = frontier.back();
            frontier.pop_back();
            for (int dir = 0; dir < 4; ++dir) {
                state.currentScreen     = ScreenType::MIXER;
                state.mixerMasterRow    = here.first;
                state.mixerCursorColumn = here.second;
                switch (dir) {
                    case 0: move_cursor_up(state); break;
                    case 1: move_cursor_down(state); break;
                    case 2: move_cursor_left(state); break;
                    case 3: move_cursor_right(state); break;
                    default: break;
                }
                const std::pair<int, int> next{state.mixerMasterRow, state.mixerCursorColumn};
                if (seen.insert(next).second) frontier.push_back(next);
            }
        }

        // The 14 cells the mixer actually has: 8 track volumes + master MIX along row 0, the REV and
        // DEL sends on row 1, and the master strip's EQ / depth / LIM at (1,8), (2,8), (3,8).
        std::set<std::pair<int, int>> expected;
        for (int c = 0; c <= 8; ++c) expected.insert({0, c});
        expected.insert({1, 0});
        expected.insert({1, 1});
        expected.insert({1, 8});
        expected.insert({2, 8});
        expected.insert({3, 8});

        eq(static_cast<int>(seen.size()), 14, "MIXER: reachable cursor states");
        ok(seen == expected, "MIXER: BFS reaches exactly the 14 real cells and no phantom state");

        // Every reachable state must be EDITABLE (or the cursor can land on nothing) — the flip side
        // of the module answering none() outside them.
        for (const auto& cell : seen) {
            state.mixerMasterRow    = cell.first;
            state.mixerCursorColumn = cell.second;
            MixerState ms{*state.project};
            ms.mixerMasterRow = cell.first;
            ms.cursorColumn   = cell.second;
            MixerModule mixer;
            ok(mixer.cursor_context(ms).is_editable(),
               "MIXER: cell (" + std::to_string(cell.first) + "," + std::to_string(cell.second) +
                   ") is reachable AND editable");
        }
    }

    // ── 2. The named MIXER moves, spelled out (a BFS proves reachability, not the RIGHT topology) ──
    {
        state.currentScreen = ScreenType::MIXER;
        auto at = [&](int row, int col) {
            state.mixerMasterRow    = row;
            state.mixerCursorColumn = col;
        };

        // DOWN off any track → REV (not "the send under that track": there isn't one).
        at(0, 5); move_cursor_down(state);
        ok(mix_pos(state) == "(1,0)", "MIXER: DOWN from track 5 → REV, got " + mix_pos(state));

        // …but column 8 continues DOWN the master strip instead.
        at(0, 8); move_cursor_down(state);
        ok(mix_pos(state) == "(1,8)", "MIXER: DOWN from MIX → EQ, got " + mix_pos(state));
        move_cursor_down(state);
        ok(mix_pos(state) == "(2,8)", "MIXER: DOWN from EQ → depth, got " + mix_pos(state));
        move_cursor_down(state);
        ok(mix_pos(state) == "(3,8)", "MIXER: DOWN from depth → LIM, got " + mix_pos(state));
        move_cursor_down(state);
        ok(mix_pos(state) == "(3,8)", "MIXER: DOWN from LIM stays (no wrap), got " + mix_pos(state));

        // UP out of a send lands on the FIRST track, not on the track above.
        at(1, 1); move_cursor_up(state);
        ok(mix_pos(state) == "(0,0)", "MIXER: UP from DEL → track 0, got " + mix_pos(state));

        // The whole master strip exits LEFT onto DEL, from any of its rows.
        for (int row = 1; row <= 3; ++row) {
            at(row, 8);
            move_cursor_left(state);
            ok(mix_pos(state) == "(1,1)",
               "MIXER: LEFT from master row " + std::to_string(row) + " → DEL, got " + mix_pos(state));
        }

        // Row 0 is the app's only WRAPPING column.
        at(0, 0); move_cursor_left(state);
        ok(mix_pos(state) == "(0,8)", "MIXER: LEFT from track 0 wraps → master, got " + mix_pos(state));
        at(0, 8); move_cursor_right(state);
        ok(mix_pos(state) == "(0,0)", "MIXER: RIGHT from master wraps → track 0, got " + mix_pos(state));

        // REV is the left edge of row 1; the send row does not wrap.
        at(1, 0); move_cursor_left(state);
        ok(mix_pos(state) == "(1,0)", "MIXER: LEFT from REV stays, got " + mix_pos(state));
        at(1, 0); move_cursor_right(state);
        ok(mix_pos(state) == "(1,1)", "MIXER: RIGHT from REV → DEL, got " + mix_pos(state));
        at(1, 1); move_cursor_right(state);
        ok(mix_pos(state) == "(1,8)", "MIXER: RIGHT from DEL → master EQ, got " + mix_pos(state));
        at(0, 0); move_cursor_up(state);
        ok(mix_pos(state) == "(0,0)", "MIXER: UP from a track stays, got " + mix_pos(state));
    }

    // ── 3. EFFECTS: eight rows, clamped at both ends ──────────────────────────────────────────────
    {
        state.currentScreen    = ScreenType::EFFECTS;
        state.effectsCursorRow = 0;
        move_cursor_up(state);
        eq(state.effectsCursorRow, 0, "EFFECTS: UP at row 0 clamps (does not wrap to 7)");

        for (int i = 0; i < 20; ++i) move_cursor_down(state);
        eq(state.effectsCursorRow, 7, "EFFECTS: DOWN clamps at row 7 (does not wrap to 0)");

        for (int i = 0; i < 20; ++i) move_cursor_up(state);
        eq(state.effectsCursorRow, 0, "EFFECTS: UP walks back to row 0");
    }

    // ── 4. The DISPATCHER is wired: does A on a mixer cell edit the RIGHT project field? ──────────
    //
    // ptinput proves the module writes the right field given a cursor. This proves the DISPATCHER
    // hands it the cursor it thinks it has — the join ptinput cannot see.
    {
        songcore::Project& p = host.edit_project();

        state.currentScreen = ScreenType::MIXER;
        state.mixerMasterRow = 0;
        state.mixerCursorColumn = 3;
        p.tracks[3].volume = 0x40;
        dispatch.on_a_up();
        eq(p.tracks[3].volume, 0x41, "DISPATCH: A+UP on MIXER track 3 increments track 3's volume");
        eq(p.tracks[4].volume, 0xFF, "DISPATCH: …and leaves track 4 alone");

        state.mixerMasterRow = 1;
        state.mixerCursorColumn = 0;
        p.reverbWet = 0x10;
        dispatch.on_a_up();
        eq(p.reverbWet, 0x11, "DISPATCH: A+UP on REV increments reverbWet");

        // The master EQ: empty → A INSERTS slot 0 (its canInsert is live, unlike the instrument EQ's).
        state.mixerMasterRow = 1;
        state.mixerCursorColumn = 8;
        p.masterEqSlot = -1;
        dispatch.on_a_up();
        eq(p.masterEqSlot, 0, "DISPATCH: A+UP on an unassigned master EQ inserts slot 0");
        dispatch.on_a_b();
        eq(p.masterEqSlot, -1, "DISPATCH: A+B deletes it back to -1 (bypass)");

        // Row 2 follows masterBusFx.
        state.mixerMasterRow = 2;
        p.masterBusFx = 1;
        p.ottDepth = 0x11;
        p.dustDepth = 0x22;
        dispatch.on_a_up();
        eq(p.dustDepth, 0x23, "DISPATCH: with DUST selected, A+UP on the depth row edits dustDepth");
        eq(p.ottDepth, 0x11, "DISPATCH: …and leaves ottDepth untouched");

        // EFFECTS.
        state.currentScreen = ScreenType::EFFECTS;
        state.effectsCursorRow = EffectModule::ROW_DLY_REV;
        p.delayReverbSend = 0x30;
        p.delayFeedback = 0x60;
        dispatch.on_a_up();
        eq(p.delayReverbSend, 0x31, "DISPATCH: A+UP on EFFECTS REV increments delayReverbSend");
        eq(p.delayFeedback, 0x60, "DISPATCH: …and not delayFeedback");
    }

    // ── 5. SELECT on EFFECTS' TIME row toggles DELAY SYNC (and re-clamps into the subdivisions) ───
    {
        songcore::Project& p = host.edit_project();
        state.currentScreen    = ScreenType::EFFECTS;
        state.effectsCursorRow = EffectModule::ROW_DLY_TIME;

        p.delaySync = false;
        p.delayTime = 0xF0;              // a free-running time, far outside the 12 subdivisions
        dispatch.on_select();
        ok(p.delaySync, "SELECT on TIME turns delay sync ON");
        eq(p.delayTime, 11, "SELECT: …and clamps a 0xF0 free time into the 12 subdivisions");

        dispatch.on_select();
        ok(!p.delaySync, "SELECT on TIME turns it back OFF");
        eq(p.delayTime, 11, "SELECT: …leaving the value where it was (11 is a valid free time too)");

        // ⚠️ S5 ASSERTED THE OPPOSITE HERE, and was right to: "SELECT on an INP EQ row is a no-op (no
        // overlay yet)". S8 built the overlay, so the no-op became an OPEN — and the two `eq()` checks
        // S5 wrote still PASSED (opening the editor touches neither `reverbInputEq` nor `delaySync`),
        // while the editor it left open silently swallowed the D-pad, A, B and R+DPAD of every section
        // BELOW this one. Twelve browser assertions went red for a reason that was nowhere near the
        // browser.
        //
        // Worth keeping as a note rather than just fixing: an assertion that encodes "X does not exist
        // yet" is a TIME BOMB with no owner. It cannot fail when X arrives — it passes, and something
        // three hundred lines away breaks instead.
        state.effectsCursorRow = EffectModule::ROW_REV_EQ;
        const bool syncBefore  = p.delaySync;
        p.reverbInputEq        = 9;
        dispatch.on_select();
        ok(state.eq.isOpen, "SELECT on EFFECTS' REV EQ row OPENS the EQ editor");
        eq(state.eq.slotIndex, 9, "…on the slot that row was pointing at");
        ok(state.eq.caller.kind == EqCallerContext::Kind::REVERB_IN, "…with the REVERB_IN caller");
        eq(p.reverbInputEq, 9, "…and merely OPENING it writes nothing into the project");
        ok(p.delaySync == syncBefore, "…nor touches delay sync");
        dispatch.on_select();
        ok(!state.eq.isOpen, "SELECT again CLOSES it");
    }

    // ── 6. START is not the transport on every screen — and on these two it plays the SONG ────────
    //
    // The S3 code dropped MIXER/EFFECTS into its `default` arm and played the current PHRASE, while the
    // comment above it said they start the song at row 0. With no engine the transport cannot actually
    // run, so what is checked is WHICH verb was called — via the sequencer's own mode.
    {
        state.currentScreen = ScreenType::MIXER;
        state.currentPhrase = 7;
        dispatch.on_start();
        ok(host.sequencer().is_playing(), "START on MIXER starts the transport");
        eq(static_cast<int>(host.sequencer().playback_mode()), static_cast<int>(songcore::PlaybackMode::SONG),
           "START on MIXER plays the SONG (not the current phrase)");
        dispatch.on_start();
        ok(!host.sequencer().is_playing(), "START again stops it");

        state.currentScreen = ScreenType::EFFECTS;
        dispatch.on_start();
        eq(static_cast<int>(host.sequencer().playback_mode()), static_cast<int>(songcore::PlaybackMode::SONG),
           "START on EFFECTS plays the SONG");
        dispatch.on_start();

        // …while PHRASE still plays the phrase, and GROOVE (the `default` arm) still does too.
        state.currentScreen = ScreenType::PHRASE;
        dispatch.on_start();
        eq(static_cast<int>(host.sequencer().playback_mode()), static_cast<int>(songcore::PlaybackMode::PHRASE),
           "START on PHRASE still plays the PHRASE");
        dispatch.on_start();

        state.currentScreen = ScreenType::GROOVE;
        dispatch.on_start();
        eq(static_cast<int>(host.sequencer().playback_mode()), static_cast<int>(songcore::PlaybackMode::PHRASE),
           "START on GROOVE still plays the PHRASE (the default arm)");
        dispatch.on_start();
    }

    // ── 7. go_to_screen's REFRESH resets — including the three S4 left out ────────────────────────
    {
        state.settings.cursorRemember = false;   // REFRESH: Android's default

        state.currentScreen         = ScreenType::MIXER;
        state.mixerMasterRow        = 3;
        state.mixerCursorColumn     = 8;
        state.instrumentCursorRow   = 9;
        state.instrumentCursorColumn = 3;
        state.modCursorRow          = 4;
        state.modCursorPair         = 1;
        state.modCursorSide         = 1;
        state.poolCursorColumn      = 4;

        go_to_screen(state, NavResult{ScreenType::PHRASE, 2});
        go_to_screen(state, NavResult{ScreenType::MIXER, 2});
        eq(state.mixerMasterRow, 0, "REFRESH: re-entering MIXER resets its row");
        eq(state.mixerCursorColumn, 0, "REFRESH: …and its column");

        go_to_screen(state, NavResult{ScreenType::INSTRUMENT, 3});
        eq(state.instrumentCursorRow, 0, "REFRESH: entering INSTRUMENT resets its row (S4 missed this)");
        eq(state.instrumentCursorColumn, 1, "REFRESH: …and its column");

        state.modCursorRow = 4; state.modCursorPair = 1; state.modCursorSide = 1;
        go_to_screen(state, NavResult{ScreenType::MODS, 3});
        eq(state.modCursorRow, 0, "REFRESH: entering MODS resets its row (S4 missed this)");
        eq(state.modCursorPair, 0, "REFRESH: …its pair");
        eq(state.modCursorSide, 0, "REFRESH: …and its side");

        state.poolCursorColumn = 4;
        state.currentInstrument = 42;
        go_to_screen(state, NavResult{ScreenType::INST_POOL, 3});
        eq(state.poolCursorColumn, 0, "REFRESH: entering INST_POOL resets its column (S4 missed this)");
        eq(state.currentInstrument, 42,
           "REFRESH: …but NOT currentInstrument — that IS the pool's row, and it must persist");

        // ⚠️ TABLE-follow runs through the Kotlin SETTER's semantics, not a bare assignment: Android's
        // `currentTable = currentInstrument` fires the currentTable setter (TrackerController.kt:124–129),
        // which also mirrors lastEditedTable (and clamps to the pool). Assign the field bare and
        // lastEditedTable trails one navigation behind whatever consumes it (parity audit, finding 6).
        state.currentTable    = 7;
        state.lastEditedTable = 7;
        go_to_screen(state, NavResult{ScreenType::TABLE, 4});
        eq(state.currentTable, 42, "TABLE-follow: entering TABLE syncs currentTable to the instrument");
        eq(state.lastEditedTable, 42,
           "TABLE-follow: …and MIRRORS lastEditedTable, as the Kotlin setter does");

        // EFFECTS is deliberately absent from the reset, on both platforms.
        state.currentScreen    = ScreenType::EFFECTS;
        state.effectsCursorRow = 5;
        go_to_screen(state, NavResult{ScreenType::MIXER, 2});
        go_to_screen(state, NavResult{ScreenType::EFFECTS, 2});
        eq(state.effectsCursorRow, 5, "REFRESH: EFFECTS' row PERSISTS (Kotlin does not reset it either)");

        // …and in REMEMBER mode nothing is reset at all.
        state.settings.cursorRemember = true;
        state.currentScreen = ScreenType::MIXER;
        state.mixerMasterRow = 2;
        state.mixerCursorColumn = 8;
        go_to_screen(state, NavResult{ScreenType::PHRASE, 2});
        go_to_screen(state, NavResult{ScreenType::MIXER, 2});
        eq(state.mixerMasterRow, 2, "REMEMBER: MIXER's row survives a round trip");
        eq(state.mixerCursorColumn, 8, "REMEMBER: …and its column");
    }

    // ══ S6a ══ THE FILE BROWSER ══════════════════════════════════════════════════════════════════
    //
    // Everything below is a JOIN, which is why it lives here and not in ptinput: the browser has no
    // cursor context and no `handle_input`, so ptinput has nothing to record from it. What can go wrong
    // is the wiring — the filter, the sort order, which button does what, and whether the modal above it
    // swallows the press.

    state.settings.cursorRemember = false;

    // ── 5. The listing: what is IN it, and in what order ────────────────────────────────────────
    {
        dispatch.open_file_browser(AppState::BrowserPurpose::LOAD_SOURCE, fs_impl.samples_directory(),
                                   sample_extensions());

        eq(static_cast<int>(state.currentScreen), static_cast<int>(ScreenType::FILE_BROWSER),
           "browser: opening it switches the screen");

        // "..", then the folders, then the files that pass the filter. `notes.txt` is not a sample and
        // `.hidden.wav` is hidden — both must be absent, and absence is exactly what a filter bug
        // produces silently.
        eqs(listing(state.fileBrowser), "..,[Kicks],alpha,mid,zeta",
            "browser: NAME_ASC — parent, folders, then filtered files");

        // ⚠️ THE STABILITY CHECK. alpha and mid have the SAME mtime; zeta is older. Under DATE_ASC the
        // old one leads and the tie must fall back on the NAME order build_item_list left behind. A
        // std::sort here would be free to emit "mid,alpha" and would do so unpredictably per toolchain.
        dispatch.on_r_up();   // NAME_ASC → NAME_DESC
        eqs(listing(state.fileBrowser), "..,[Kicks],zeta,mid,alpha", "browser: R+UP → NAME_DESC");

        dispatch.on_r_up();   // → SIZE_ASC
        eqs(listing(state.fileBrowser), "..,[Kicks],alpha,mid,zeta",
            "browser: R+UP → SIZE_ASC (1000 < 2000 < 3000)");

        dispatch.on_r_up();   // → SIZE_DESC
        eqs(listing(state.fileBrowser), "..,[Kicks],zeta,mid,alpha", "browser: R+UP → SIZE_DESC");

        dispatch.on_r_up();   // → wraps round to DATE_DESC
        eq(static_cast<int>(state.fileBrowser.sortMode), static_cast<int>(FileSortMode::DATE_DESC),
           "browser: the six sort modes WRAP (SIZE_DESC → DATE_DESC)");

        // ⚠️ **THE STABLE-SORT CHECK, and it is the reason the fixture sets mtimes by hand.** alpha and
        // mid share an mtime; zeta is older. Under DATE_DESC the two newest come first — and their TIE
        // must fall back on NAME order, because `rebuild_items` re-lists (name-ordered) before every
        // sort and `sort_items` is a stable_sort over that. Two things could break it and neither would
        // be visible without a tie: a plain std::sort (arbitrary, and different per toolchain), or
        // re-sorting the on-screen list instead of rebuilding it (which would tie-break on whatever the
        // PREVIOUS mode left — so NAME→SIZE_DESC→DATE_DESC would differ from NAME→DATE_DESC).
        eqs(listing(state.fileBrowser), "..,[Kicks],alpha,mid,zeta",
            "⚠️ browser: DATE_DESC — the newest lead, and the alpha/mid TIE holds its NAME order");

        dispatch.on_r_up();   // → DATE_ASC
        eqs(listing(state.fileBrowser), "..,[Kicks],zeta,alpha,mid",
            "⚠️ browser: DATE_ASC — the oldest leads, and the tie AGAIN falls back on name order");

        dispatch.on_r_up();   // six steps in all: back where we started
        eq(static_cast<int>(state.fileBrowser.sortMode), static_cast<int>(FileSortMode::NAME_ASC),
           "browser: six R+UPs is a full cycle");

        dispatch.on_r_down();
        eq(static_cast<int>(state.fileBrowser.sortMode), static_cast<int>(FileSortMode::DATE_ASC),
           "browser: R+DOWN cycles the other way");
        dispatch.on_r_up();   // …and back to NAME_ASC, which the blocks below assume
    }

    // ── 6. The cursor: it WRAPS on a step and CLAMPS on a page ──────────────────────────────────
    {
        dispatch.open_file_browser(AppState::BrowserPurpose::LOAD_SOURCE, fs_impl.samples_directory(),
                                   sample_extensions());
        eq(state.fileBrowser.cursor, 0, "browser: opens on the first row");

        dispatch.on_dpad_up();
        eq(state.fileBrowser.cursor, 4, "browser: UP off the top WRAPS to the last row");
        dispatch.on_dpad_down();
        eq(state.fileBrowser.cursor, 0, "browser: …and DOWN off the bottom wraps back");

        // ⚠️ The page jump CLAMPS where the single step wraps. A page that wrapped would fling you from
        // the top of a 400-file directory to the bottom on one tap.
        dispatch.on_dpad_left();
        eq(state.fileBrowser.cursor, 0, "browser: LEFT (page) CLAMPS at the top, it does not wrap");
        dispatch.on_dpad_right();
        eq(state.fileBrowser.cursor, 4, "browser: RIGHT (page) clamps at the last row");
    }

    // ── 7. Navigation: into a folder, and back out with ".." ────────────────────────────────────
    {
        dispatch.open_file_browser(AppState::BrowserPurpose::LOAD_SOURCE, fs_impl.samples_directory(),
                                   sample_extensions());
        dispatch.on_dpad_down();   // onto [Kicks]
        eq(state.fileBrowser.cursor, 1, "browser: cursor on the folder");

        dispatch.on_button_a();    // into it
        eqs(listing(state.fileBrowser), "..", "browser: A on a folder descends into it (Kicks is empty)");
        eq(static_cast<int>(state.currentScreen), static_cast<int>(ScreenType::FILE_BROWSER),
           "browser: …and descending is NOT a load — the browser stays up");

        dispatch.on_r_left();      // R+LEFT = up a directory (what the bottom bar advertises)
        eqs(listing(state.fileBrowser), "..,[Kicks],alpha,mid,zeta", "browser: R+LEFT goes up a level");

        dispatch.on_button_a();    // A on ".." — the same thing, by cursor
        eq(state.fileBrowser.cursor, 0, "browser: A on '..' goes up (cursor resets)");
        ok(state.fileBrowser.currentDirectory == tree.root.generic_string(),
           "browser: …and lands in the parent directory");
    }

    // ── 8. B leaves; the DELETE confirm needs TWO presses ───────────────────────────────────────
    {
        state.currentScreen = ScreenType::INSTRUMENT;
        dispatch.open_file_browser(AppState::BrowserPurpose::LOAD_SOURCE, fs_impl.samples_directory(),
                                   sample_extensions());
        eq(static_cast<int>(state.previousScreen), static_cast<int>(ScreenType::INSTRUMENT),
           "browser: it remembers where it was opened from");

        dispatch.on_button_b();
        eq(static_cast<int>(state.currentScreen), static_cast<int>(ScreenType::INSTRUMENT),
           "browser: B returns to the screen it was opened from");

        dispatch.open_file_browser(AppState::BrowserPurpose::LOAD_SOURCE, fs_impl.samples_directory(),
                                   sample_extensions());
        dispatch.on_dpad_down(); dispatch.on_dpad_down();   // onto "alpha"

        // ⚠️ SELECT+B ARMS the confirm; it must never delete on the press itself.
        dispatch.on_select_b();
        eq(static_cast<int>(state.fileBrowser.mode), static_cast<int>(BrowserMode::DELETE),
           "browser: SELECT+B arms the DELETE confirm");
        ok(fs_impl.file_exists(fs_impl.samples_directory() + "/alpha.wav"),
           "browser: …and the file is STILL THERE — arming is not deleting");

        dispatch.on_button_b();   // B = NO
        eq(static_cast<int>(state.fileBrowser.mode), static_cast<int>(BrowserMode::NORMAL),
           "browser: B cancels the confirm…");
        eq(static_cast<int>(state.currentScreen), static_cast<int>(ScreenType::FILE_BROWSER),
           "browser: …and does NOT also leave the browser (that is what makes SELECT+B safe)");
        ok(fs_impl.file_exists(fs_impl.samples_directory() + "/alpha.wav"), "browser: the file survives");

        dispatch.on_select_b();
        dispatch.on_button_a();   // A = YES
        ok(!fs_impl.file_exists(fs_impl.samples_directory() + "/alpha.wav"),
           "browser: SELECT+B then A actually deletes");
        eqs(listing(state.fileBrowser), "..,[Kicks],mid,zeta", "browser: …and the listing refreshes");

        TempTree::write(fs::path(fs_impl.samples_directory()) / "alpha.wav", 1000);   // put it back
    }

    // ══ S6a ══ THE QWERTY KEYBOARD ═══════════════════════════════════════════════════════════════
    //
    // ⚠️ THE MODAL RULE: while the keyboard is open it OWNS every button, and it can be open ON TOP of
    // the browser. A D-pad press there must move the KEY cursor and leave the FILE cursor alone — which
    // is the single thing most likely to be got wrong, and the reason the guards are ordered
    // keyboard-then-browser in every handler.
    {
        dispatch.open_file_browser(AppState::BrowserPurpose::LOAD_SOURCE, fs_impl.samples_directory(),
                                   sample_extensions());
        dispatch.on_dpad_down(); dispatch.on_dpad_down();   // onto "alpha"
        const int fileCursor = state.fileBrowser.cursor;

        dispatch.on_select_a();
        ok(state.qwerty.isOpen, "keyboard: SELECT+A on a file opens it");
        eqs(state.qwerty.text, "alpha", "keyboard: …pre-filled with the name, WITHOUT the extension");
        eqs(state.qwerty.fieldLabel, "SAMPLE NAME:", "keyboard: …and a .wav gets the SAMPLE label");

        // The D-pad now belongs to the keyboard.
        dispatch.on_dpad_down();
        eq(state.qwerty.keyCursorRow, 1, "keyboard: D-pad moves the KEY cursor");
        eq(state.fileBrowser.cursor, fileCursor,
           "⚠️ keyboard: …and the FILE cursor underneath does NOT move (the modal rule)");

        // Q W E R T Y / A S D F… — row 1 col 0 is 'A'.
        eq(static_cast<int>(state.qwerty.current_key()), static_cast<int>('A'),
           "keyboard: row 1 col 0 is 'A'");

        dispatch.on_button_a();   // type it
        eqs(state.qwerty.text, "alphaA", "keyboard: A types the key under the cursor");
        dispatch.on_button_b();   // backspace
        eqs(state.qwerty.text, "alpha", "keyboard: B backspaces (insertBefore = true)");

        // R+DOWN switches to the symbol layout; R+UP switches back.
        dispatch.on_r_down();
        eq(state.qwerty.layout, 1, "keyboard: R+DOWN → the 123 layout");
        eq(static_cast<int>(state.qwerty.current_key()), static_cast<int>('!'),
           "keyboard: …and row 1 col 0 is now '!'");
        dispatch.on_r_up();
        eq(state.qwerty.layout, 0, "keyboard: R+UP → back to ABC");

        // SELECT aborts. The file must NOT have been renamed.
        dispatch.on_select();
        ok(!state.qwerty.isOpen, "keyboard: SELECT aborts");
        ok(fs_impl.file_exists(fs_impl.samples_directory() + "/alpha.wav"),
           "keyboard: …and an abort renames NOTHING");
    }

    // ── 10. APPLY actually renames, and keeps the extension ─────────────────────────────────────
    {
        dispatch.open_file_browser(AppState::BrowserPurpose::LOAD_SOURCE, fs_impl.samples_directory(),
                                   sample_extensions());
        dispatch.on_dpad_down(); dispatch.on_dpad_down();   // "alpha"
        dispatch.on_select_a();

        // Clear the field and type "KICK" — the four keys are on three different rows, so this also
        // exercises the row clamp.
        for (int i = 0; i < 8; ++i) dispatch.on_button_b();
        eqs(state.qwerty.text, "", "keyboard: B past the start of the text stops at empty");

        // K is row 1 col 7; I is row 0 col 7; C is row 2 col 2; K again.
        state.qwerty.keyCursorRow = 1; state.qwerty.keyCursorCol = 7; dispatch.on_button_a();
        state.qwerty.keyCursorRow = 0; state.qwerty.keyCursorCol = 7; dispatch.on_button_a();
        state.qwerty.keyCursorRow = 2; state.qwerty.keyCursorCol = 2; dispatch.on_button_a();
        state.qwerty.keyCursorRow = 1; state.qwerty.keyCursorCol = 7; dispatch.on_button_a();
        eqs(state.qwerty.text, "KICK", "keyboard: typed KICK");

        dispatch.on_start();   // START = APPLY
        ok(!state.qwerty.isOpen, "keyboard: START applies and closes");
        ok(fs_impl.file_exists(fs_impl.samples_directory() + "/KICK.wav"),
           "keyboard: FILE_RENAME renamed it — and KEPT THE .wav EXTENSION");
        ok(!fs_impl.file_exists(fs_impl.samples_directory() + "/alpha.wav"),
           "keyboard: …and the old name is gone");
        eqs(listing(state.fileBrowser), "..,[Kicks],KICK,mid,zeta",
            "keyboard: …and the browser re-listed itself");
    }

    // ── 11. SELECT+R creates a folder ───────────────────────────────────────────────────────────
    {
        dispatch.on_select_r();
        ok(state.qwerty.isOpen, "keyboard: SELECT+R opens it for a new folder");
        eqs(state.qwerty.text, "NEW FOLDER", "keyboard: …pre-filled with NEW FOLDER");

        dispatch.on_start();
        // ⚠️ create_folder sanitises: a space is not in [A-Za-z0-9_-], so it becomes '_'.
        ok(fs_impl.is_directory(fs_impl.samples_directory() + "/NEW_FOLDER"),
           "keyboard: FOLDER_CREATE made it, with the space SANITISED to an underscore");
        eqs(listing(state.fileBrowser), "..,[Kicks],[NEW_FOLDER],KICK,mid,zeta",
            "keyboard: …and it appears among the FOLDERS, not the files");

        std::error_code ec;
        fs::remove_all(fs::path(fs_impl.samples_directory()) / "NEW_FOLDER", ec);
    }

    // ── 12. The file clipboard: L+B selects, B copies, L+A pastes ───────────────────────────────
    {
        dispatch.open_file_browser(AppState::BrowserPurpose::LOAD_SOURCE, fs_impl.samples_directory(),
                                   sample_extensions());
        dispatch.set_now(1000);
        dispatch.on_dpad_down(); dispatch.on_dpad_down();   // "KICK"

        dispatch.on_l_b();
        ok(state.fileBrowser.selectionMode, "browser: L+B enters selection");
        eq(state.fileBrowser.selectionAnchor, 2, "browser: …anchored at the cursor");

        dispatch.on_dpad_down();   // widen onto "mid"
        eq(selected_count(state), 2,
           "browser: the D-pad widens the selection (2 files)");

        dispatch.on_button_b();    // B = COPY
        ok(!state.fileBrowser.selectionMode, "browser: B copies and leaves selection");
        eq(static_cast<int>(state.fileBrowser.fileClipboard.size()), 2, "browser: 2 files on the clipboard");
        ok(!state.fileBrowser.fileClipboardIsCut, "browser: …as a COPY, not a cut");

        // Into Kicks/, and paste.
        dispatch.on_dpad_up(); dispatch.on_dpad_up();   // back to [Kicks] (cursor 3 → 2 → 1)
        dispatch.on_button_a();
        dispatch.on_l_a();
        eqs(listing(state.fileBrowser), "..,KICK,mid", "browser: L+A pasted both files into Kicks/");

        // ⚠️ A COPY clipboard SURVIVES the paste — the same files can go into several folders. Pasting
        // again into the same folder must de-duplicate rather than overwrite.
        eq(static_cast<int>(state.fileBrowser.fileClipboard.size()), 2,
           "browser: a COPY clipboard survives its paste");
        dispatch.on_l_a();
        eqs(listing(state.fileBrowser), "..,KICK,KICK_2,mid,mid_2",
            "⚠️ browser: pasting again DE-DUPLICATES (_2) — it never overwrites");
    }

    // ── 13. START auditions; SELECT alone does nothing ──────────────────────────────────────────
    {
        // With a null engine `preview_file` returns false and the browser says so. What is asserted
        // here is the ROUTING — that START on the browser reached the audition at all instead of
        // falling through to the transport and starting playback.
        dispatch.open_file_browser(AppState::BrowserPurpose::LOAD_SOURCE, fs_impl.samples_directory(),
                                   sample_extensions());
        dispatch.on_dpad_down();   // [Kicks] — a folder has nothing to audition
        dispatch.on_start();
        ok(!host.is_playing(), "⚠️ browser: START does NOT start the transport (it auditions)");
        eqs(state.fileBrowser.statusMessage, "",
            "browser: START on a FOLDER is a no-op — no failure message either");

        dispatch.on_dpad_down();   // onto a .wav
        dispatch.on_start();
        ok(!host.is_playing(), "browser: …still not the transport on a file");
        eqs(state.fileBrowser.statusMessage, "PREVIEW FAILED",
            "browser: START on a file DID reach the audition (it fails with no engine, and says so)");
    }

    // ══ S6a ══ THE INSTRUMENT SCREEN'S BUTTONS, WHICH S4 DREW AND COULD NOT PRESS ════════════════
    {
        state.currentScreen = ScreenType::INSTRUMENT;
        state.currentInstrument = 5;

        // Row 5 col 2 — the SOURCE row's LOAD. A sampler browses samples; a SoundFont browses .sf2.
        state.instrumentCursorRow = 5; state.instrumentCursorColumn = 2;
        dispatch.on_button_a();
        eq(static_cast<int>(state.currentScreen), static_cast<int>(ScreenType::FILE_BROWSER),
           "INSTRUMENT: A on the SOURCE row's LOAD opens the browser");
        eq(static_cast<int>(state.browserPurpose),
           static_cast<int>(AppState::BrowserPurpose::LOAD_SOURCE), "INSTRUMENT: …for a SOURCE");
        eqs(state.fileBrowser.currentDirectory, fs_impl.samples_directory(),
            "INSTRUMENT: …in the SAMPLES directory (the slot is a sampler)");
        dispatch.on_button_b();

        host.edit_project().instruments[5].instrumentType = songcore::InstrumentType::SOUNDFONT;
        state.instrumentCursorRow = 5; state.instrumentCursorColumn = 2;
        dispatch.on_button_a();
        eqs(state.fileBrowser.currentDirectory, fs_impl.soundfonts_directory(),
            "⚠️ INSTRUMENT: a SOUNDFONT slot browses SOUNDFONTS instead — same cell, different folder");
        dispatch.on_button_b();
        host.edit_project().instruments[5].instrumentType = songcore::InstrumentType::SAMPLER;

        // Row 0 col 2 — LOAD PRESET (.pti), which is a different directory AND a different purpose.
        state.instrumentCursorRow = 0; state.instrumentCursorColumn = 2;
        dispatch.on_button_a();
        eq(static_cast<int>(state.browserPurpose),
           static_cast<int>(AppState::BrowserPurpose::LOAD_PRESET), "INSTRUMENT: row 0 LOAD is a PRESET");
        eqs(state.fileBrowser.currentDirectory, fs_impl.instruments_directory(),
            "INSTRUMENT: …in the INSTRUMENTS directory");
        dispatch.on_button_b();

        // Row 0 col 3 — SAVE PRESET, which opens the keyboard rather than the browser.
        state.instrumentCursorRow = 0; state.instrumentCursorColumn = 3;
        dispatch.on_button_a();
        ok(state.qwerty.isOpen, "INSTRUMENT: row 0 SAVE opens the KEYBOARD, not the browser");
        eqs(state.qwerty.fieldLabel, "SAVE PRESET:", "INSTRUMENT: …labelled SAVE PRESET");
        dispatch.on_start();
        ok(fs_impl.file_exists(fs_impl.instruments_directory() + "/INST05.pti"),
           "INSTRUMENT: …and APPLY wrote the .pti");

        // ⚠️ The NAME row is the one cell whose A is DEFERRED TO RELEASE.
        state.instrumentCursorRow = 1; state.instrumentCursorColumn = 1;
        ok(dispatch.defer_a_to_release(),
           "⚠️ INSTRUMENT: the NAME row DEFERS its A to release (so A+B can reset the cell instead)");
        state.instrumentCursorRow = 5; state.instrumentCursorColumn = 2;
        ok(!dispatch.defer_a_to_release(),
           "INSTRUMENT: …and the LOAD button does NOT — it is a read-only cell with no A-combo to protect");

        state.instrumentCursorRow = 1; state.instrumentCursorColumn = 1;
        dispatch.on_button_a();
        ok(state.qwerty.isOpen, "INSTRUMENT: A on NAME opens the keyboard");
        eqs(state.qwerty.text, "",
            "⚠️ INSTRUMENT: …EMPTY, not 'INST05' — a default name is a placeholder, not text to delete");
        dispatch.on_select();
    }

    // ══ S6b ══ THE SAMPLE EDITOR ════════════════════════════════════════════════════════════════
    //
    // ptinput proves the MODULE: given a cursor, its context, its action and the cell it writes match
    // Kotlin's. It cannot see any of what follows — whether the cursor can REACH those cells, whether
    // INSTRUMENT's EDIT button opens the editor at all, whether B guards an unsaved sample, or whether
    // the WAV that comes out of a CHOP can be read back in.

    // ── 1. The WAV round trip: cue points OUT, cue points IN ────────────────────────────────────
    //
    // ⚠️ This is the assertion that makes S6b's cue-point work REAL rather than merely compiled. S6a
    // shipped the load path with the cue chunk deliberately unread ("a sliced WAV loaded on Linux plays
    // whole") because there was no writer to pair a reader with. There is one now — and a writer whose
    // output nothing can read back is not half a feature, it is a feature that silently loses data.
    //
    // ptdispatch is the only tool with a real filesystem, so this is its natural home.
    {
        const std::string wav = tree.root.string() + "/roundtrip.wav";

        std::vector<float> left(1000), right(1000);
        for (size_t i = 0; i < left.size(); ++i) {
            left[i]  = std::sin(static_cast<float>(i) * 0.05f);
            right[i] = -left[i];
        }
        const std::vector<int> cues = {100, 250, 700};

        ok(songcore::write_wav(wav, left, right, 44100, cues, /*channels=*/2),
           "wav: a stereo WAV with three cue points was written");

        const std::vector<int> read = songcore::read_cue_points(wav);
        eq(static_cast<int>(read.size()), 3, "wav: …and three cue points came back");
        if (read.size() == 3) {
            eq(read[0], 100, "wav: cue 0 round-tripped");
            eq(read[1], 250, "wav: cue 1 round-tripped");
            eq(read[2], 700, "wav: cue 2 round-tripped");
        }

        // A WAV with NO cue chunk must read back as empty rather than as garbage — that is the path
        // every ordinary sample in the world takes through the loader.
        const std::string plain = tree.root.string() + "/nocues.wav";
        ok(songcore::write_wav(plain, left, right, 44100, {}, /*channels=*/1),
           "wav: a mono WAV with no cue chunk was written");
        eq(static_cast<int>(songcore::read_cue_points(plain).size()), 0,
           "wav: …and it reads back with NO markers (not garbage)");

        // ⚠️ Frame 0 is EXCLUDED on the way back in: it is the sample's own start, not a boundary
        // WITHIN it, and letting it through would give every sliced file a zero-length slice 0.
        const std::string zero = tree.root.string() + "/zerocue.wav";
        songcore::write_wav(zero, left, right, 44100, {0, 500}, /*channels=*/1);
        const std::vector<int> zread = songcore::read_cue_points(zero);
        eq(static_cast<int>(zread.size()), 1, "⚠️ wav: a cue at frame 0 is DROPPED on read");
        if (zread.size() == 1) eq(zread[0], 500, "wav: …and the real boundary survives");
    }

    // ── 2. INSTRUMENT's EDIT button opens it — and a SoundFont's does not ───────────────────────
    {
        state.currentScreen     = ScreenType::INSTRUMENT;
        state.currentInstrument = 7;
        host.edit_project().instruments[7].instrumentType = songcore::InstrumentType::SAMPLER;

        state.instrumentCursorRow = 5; state.instrumentCursorColumn = 3;   // the EDIT cell
        dispatch.on_button_a();
        eq(static_cast<int>(state.currentScreen), static_cast<int>(ScreenType::SAMPLE_EDITOR),
           "SE: A on INSTRUMENT's EDIT opens the sample editor");
        eq(state.sampleEditor.instrumentId, 7, "SE: …on the instrument under the cursor");
        eq(state.sampleEditor.cursorRow, 1, "SE: …with the cursor on row 1 (ZOOM), not on row 0");
        eq(static_cast<int>(state.previousScreen), static_cast<int>(ScreenType::INSTRUMENT),
           "SE: …and B will return to INSTRUMENT");

        dispatch.on_button_b();   // nothing modified → straight out
        eq(static_cast<int>(state.currentScreen), static_cast<int>(ScreenType::INSTRUMENT),
           "SE: B on an UNMODIFIED sample leaves at once (no confirm)");

        // ⚠️ A SoundFont has no single waveform to cut — it is a bank of them. The press is CONSUMED
        // (the row is shared, so the button is drawn on both) but it opens nothing.
        host.edit_project().instruments[7].instrumentType = songcore::InstrumentType::SOUNDFONT;
        state.instrumentCursorRow = 5; state.instrumentCursorColumn = 3;
        dispatch.on_button_a();
        eq(static_cast<int>(state.currentScreen), static_cast<int>(ScreenType::INSTRUMENT),
           "⚠️ SE: EDIT on a SOUNDFONT slot opens NOTHING — there is no one waveform to edit");
        host.edit_project().instruments[7].instrumentType = songcore::InstrumentType::SAMPLER;
    }

    // ── 3. ⚠️ THE CRASH. An empty slot, four presses, and Android dies. ─────────────────────────
    //
    // With no sample loaded, `totalFrames` and `selectionEnd` are both 0 — and Kotlin's nudge arms are
    // `coerceIn(0, selectionEnd - 1)` and `coerceIn(selectionStart + 1, maxFrame)`, i.e. coerceIn(0, -1)
    // and coerceIn(1, 0). Both have min > max, which `coerceIn` REQUIRES not to be: it throws
    // IllegalArgumentException and the app is gone. Nothing on the way into the editor checks that the
    // slot has any audio in it, so EDIT → DOWN → DOWN → A+RIGHT is all it takes.
    //
    // (In C++ it would be worse than a crash — `std::clamp` with lo > hi is undefined behaviour.)
    // Fixed on BOTH platforms, per §4's zone-B rule.
    {
        state.currentScreen = ScreenType::INSTRUMENT;
        state.currentInstrument = 9;
        state.instrumentCursorRow = 5; state.instrumentCursorColumn = 3;
        dispatch.on_button_a();          // into the editor, on a slot with no sample at all

        eq(state.sampleEditor.totalFrames, 0, "SE: the empty slot really is empty (no engine, no audio)");

        dispatch.on_dpad_down();         // row 1 → 2
        dispatch.on_dpad_down();         // row 2 → 8  (the SELECTION row — the map skips 3..7)
        eq(state.sampleEditor.cursorRow, 8, "SE: DOWN, DOWN reaches row 8 — the waveform is not five rows");

        // Each of these four is the crash on Android. What is asserted is simply that we come BACK.
        dispatch.on_a_right();
        dispatch.on_a_left();
        dispatch.on_a_up();
        dispatch.on_a_down();
        eq(static_cast<int>(state.sampleEditor.selectionStart), 0,
           "⚠️ SE: A+DPAD on an EMPTY sample is a NO-OP, not a crash (selection start)");
        eq(static_cast<int>(state.sampleEditor.selectionEnd), 0,
           "⚠️ SE: …and not a crash on the END edge either");

        state.sampleEditor.cursorCol = 1;   // the END column — Kotlin's *other* throwing arm
        dispatch.on_a_right();
        dispatch.on_a_left();
        eq(static_cast<int>(state.sampleEditor.selectionEnd), 0,
           "⚠️ SE: …nor on column 1, which is the arm that throws coerceIn(1, 0)");

        dispatch.on_button_b();
    }

    // ── 4. The SPARSE ROW MAP, walked through the real cursor ───────────────────────────────────
    //
    // ptinput's SEROW lines prove the map is Kotlin's. They do NOT prove the D-pad consults it — that is
    // `cursor_move.h`, and it is a different file.
    {
        state.currentScreen = ScreenType::INSTRUMENT;
        state.currentInstrument = 7;
        state.instrumentCursorRow = 5; state.instrumentCursorColumn = 3;
        dispatch.on_button_a();

        SampleEditorState& se = state.sampleEditor;
        se.sliceMethod = SampleEditorModule::SLICE_OFF;

        // DOWN from the top, with slicing OFF. Row 11 (the slice detail) is NOT DRAWN, so it must not be
        // reachable — the cursor has to step over it.
        const int wantOff[] = {2, 8, 10, 13, 14, 16, 18, 19, 1};
        int       rowIdx    = 0;
        for (const int want : wantOff) {
            dispatch.on_dpad_down();
            eq(se.cursorRow, want,
               "SE: DOWN #" + std::to_string(++rowIdx) + " with slicing OFF");
        }
        ok(se.cursorRow == 1, "SE: …and row 19 WRAPS back to row 1");

        // With slicing ON, row 11 exists and DOWN from 10 must land on it.
        se.sliceMethod = SampleEditorModule::SLICE_DIVIDE;
        se.cursorRow   = 10;
        dispatch.on_dpad_down();
        eq(se.cursorRow, 11, "⚠️ SE: with slicing ON, DOWN from 10 reaches row 11 (it exists now)");
        dispatch.on_dpad_up();
        eq(se.cursorRow, 10, "SE: …and UP goes back");

        // ⚠️ The COLUMN clamps on the way. NAME (18) has ONE column; the op rows have six. Carry a live
        // column across and the cursor lands where nothing is drawn — S2's "the cursor vanishes" bug.
        se.cursorRow = 13; se.cursorCol = 5;   // DEL, the rightmost op
        dispatch.on_dpad_down();               // → 14, which also has 6
        eq(se.cursorCol, 5, "SE: column 5 survives 13 → 14 (both op rows have six)");
        dispatch.on_dpad_down();               // → 16, which has 3
        eq(se.cursorCol, 2, "⚠️ SE: …but CLAMPS to 2 on the FX row, which has three columns");
        dispatch.on_dpad_down();               // → 18 (NAME), which has ONE
        eq(se.cursorCol, 0, "⚠️ SE: …and to 0 on NAME, which is one cell wide");

        // ⚠️ The two OP rows WRAP; everything else clamps. They are a ring of buttons, not a range.
        se.cursorRow = 13; se.cursorCol = 5;
        dispatch.on_dpad_right();
        eq(se.cursorCol, 0, "⚠️ SE: RIGHT off the last op WRAPS to the first (a ring of buttons)");
        dispatch.on_dpad_left();
        eq(se.cursorCol, 5, "⚠️ SE: …and LEFT wraps back the other way");

        se.cursorRow = 1; se.cursorCol = 2;    // the view row: three columns, and it CLAMPS
        dispatch.on_dpad_right();
        eq(se.cursorCol, 2, "SE: RIGHT off the last column of row 1 CLAMPS — it does not wrap");

        // CHOP (row 19, col 3) exists only when there ARE slices.
        se.sliceMethod = SampleEditorModule::SLICE_OFF;
        se.cursorRow = 19; se.cursorCol = 3;
        dispatch.on_dpad_right();
        eq(SampleEditorModule::max_col_for_row(19, SampleEditorModule::SLICE_OFF), 2,
           "⚠️ SE: with slicing OFF, row 19's last column is OVERWRITE — there is no CHOP");
    }

    // ── 5. The unsaved-changes guard ────────────────────────────────────────────────────────────
    //
    // The editor's edits live in the ENGINE's buffer, not in the project — so leaving without saving is
    // the one gesture in the app that can silently destroy work.
    {
        SampleEditorState& se = state.sampleEditor;
        se.isModified = true;

        dispatch.on_button_b();
        ok(se.showConfirmClose, "⚠️ SE: B on a MODIFIED sample arms the confirm instead of leaving");
        eq(static_cast<int>(state.currentScreen), static_cast<int>(ScreenType::SAMPLE_EDITOR),
           "SE: …and stays on the editor");

        dispatch.on_button_b();   // B = NO
        ok(!se.showConfirmClose, "SE: B again is NO — the dialog closes");
        eq(static_cast<int>(state.currentScreen), static_cast<int>(ScreenType::SAMPLE_EDITOR),
           "SE: …and you are still in the editor, with the edit intact");

        dispatch.on_button_b();   // arm it again
        dispatch.on_button_a();   // A = YES
        eq(static_cast<int>(state.currentScreen), static_cast<int>(ScreenType::INSTRUMENT),
           "SE: A on the confirm DISCARDS and leaves");
    }

    // ── 6. START auditions, SELECT names, and neither is the transport ──────────────────────────
    {
        state.currentScreen = ScreenType::INSTRUMENT;
        state.instrumentCursorRow = 5; state.instrumentCursorColumn = 3;
        dispatch.on_button_a();

        dispatch.set_now(5000);
        dispatch.on_start();
        ok(!host.is_playing(),
           "⚠️ SE: START AUDITIONS the sample — it does not start the transport");

        // ⚠️ SELECT on the NAME row opens the keyboard. It is the alias for the A that would otherwise
        // be deferred to release.
        state.sampleEditor.cursorRow = 18;
        dispatch.on_select();
        ok(state.qwerty.isOpen, "SE: SELECT on the NAME row opens the keyboard");
        eqs(state.qwerty.fieldLabel, "SAMPLE NAME:", "SE: …labelled SAMPLE NAME");

        // And APPLY renames BOTH the editor's sample and the instrument holding it — they are one thing
        // to the user, and a pool showing "INST07" for a slot just named "SNARE" is the app disagreeing
        // with itself.
        state.qwerty.text = "SNARE";
        dispatch.on_start();
        eqs(state.sampleEditor.sampleName, "SNARE", "SE: …and APPLY renames the sample");
        eqs(host.project().instruments[7].name, "SNARE",
            "⚠️ SE: …AND the instrument holding it, which is the same thing to the user");

        // SELECT on a row that is not NAME does nothing at all (the EQ editor it would open on the FX
        // row is not ported — as no EQ cell in the app is yet).
        state.sampleEditor.cursorRow = 13;
        dispatch.on_select();
        ok(!state.qwerty.isOpen, "SE: SELECT on an OP row does nothing");
    }

    // ── 7. The slice arithmetic that CHOP and SAVE both depend on ───────────────────────────────
    //
    // `compute_slice_cue_points` is what goes into the WAV, and `current_slices` is CHOP's work list.
    // Neither is reachable from a module, so ptinput cannot see either.
    {
        SampleEditorState& se = state.sampleEditor;
        se.totalFrames = 96000;

        // DIVIDE by 4 → three internal boundaries at 24000 / 48000 / 72000. Not four: the sample's own
        // start and end are not boundaries WITHIN it.
        se.sliceMethod   = SampleEditorModule::SLICE_DIVIDE;
        se.sliceDivisions = 4;
        se.cursorRow = 19; se.cursorCol = 3;   // CHOP — with a null engine it writes nothing, but the
        dispatch.on_button_a();                //        arithmetic below is what it would have used.

        // TRANSIENT: the markers ARE the boundaries, minus any at 0 or past the end.
        se.sliceMethod      = SampleEditorModule::SLICE_TRANSIENT;
        se.transientMarkers = {0, 12000, 48000, 96000, 200000};
        int64_t a = 0, b = 0;
        se.sliceIndex = 0;
        se.slice_bounds(0, a, b);
        eq(static_cast<int>(a), 0, "SE: transient slice 0 starts at the sample's own start");
        eq(static_cast<int>(b), 0, "SE: …and ends at the first marker (which is 0 here)");
        se.slice_bounds(2, a, b);
        eq(static_cast<int>(a), 12000, "SE: slice 2 runs from marker 1…");
        eq(static_cast<int>(b), 48000, "SE: …to marker 2");
        se.slice_bounds(5, a, b);   // past the last marker
        eq(static_cast<int>(b), 96000,
           "⚠️ SE: the LAST slice ends at the sample, not at a marker that does not exist");

        dispatch.on_button_b();
        if (state.sampleEditor.showConfirmClose) dispatch.on_button_a();
    }

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // PROJECT + SETTINGS (S7)
    // ═════════════════════════════════════════════════════════════════════════════════════════════
    //
    // Everything below is invisible to `ptinput`, and most of it is invisible to it BY CONSTRUCTION
    // rather than by omission:
    //
    //   • The SETTINGS row map is CAPS-FILTERED. Android has no such thing, so there is no Kotlin to
    //     record a golden FROM — the visible-row walk can only be checked here.
    //   • The EXIT row does not exist on Android either.
    //   • ptinput proves each module matches Kotlin GIVEN a cursor. Nothing in it proves the cursor
    //     can reach the cell, that A on SAVE writes a file, or that a modal owns the buttons.

    // ── 12. The SETTINGS row map: can the cursor reach every visible row, and ONLY those? ────────
    //
    // ⚠️ THE CONTROL THAT MATTERS. Kotlin hides its two debug rows with a SINGLE substitution
    // (`if (!DEBUG && prev == 12) prev = 11`), which is correct there only because no two hidden rows
    // are ever adjacent. On the shell, rows 2, 3 and 4 (OVERLAY, BTN SOUND, BTN VIBRO) vanish
    // TOGETHER — so a one-level hop off row 1 lands on row 3, which is not there either, and the
    // cursor disappears onto a row nothing draws. The walk has to LOOP.
    {
        state.currentScreen = ScreenType::SETTINGS;

        for (const bool android : {false, true}) {
            state.caps = android ? PlatformCaps::android(/*debug=*/true)
                                 : PlatformCaps::sdl(/*debug=*/true);
            const char* who = android ? "android" : "sdl";

            // Walk DOWN from the first visible row, all the way round, and collect what we land on.
            state.settingsCursorRow = settings_first_visible_row(state.caps);
            std::set<int> seen;
            for (int i = 0; i < SETTINGS_ROW_COUNT * 2; ++i) {
                seen.insert(state.settingsCursorRow);
                ok(settings_row_visible(static_cast<SettingsRow>(state.settingsCursorRow), state.caps),
                   std::string("SETTINGS[") + who + "]: DOWN never lands on a hidden row");
                dispatch.on_dpad_down();
            }

            int expected = 0;
            for (int r = 0; r < SETTINGS_ROW_COUNT; ++r)
                if (settings_row_visible(static_cast<SettingsRow>(r), state.caps)) ++expected;

            eq(static_cast<int>(seen.size()), expected,
               std::string("SETTINGS[") + who + "]: DOWN reaches every visible row (and wraps)");

            // …and UP, which is the direction Kotlin's substitution gets wrong first.
            state.settingsCursorRow = settings_first_visible_row(state.caps);
            std::set<int> seenUp;
            for (int i = 0; i < SETTINGS_ROW_COUNT * 2; ++i) {
                seenUp.insert(state.settingsCursorRow);
                ok(settings_row_visible(static_cast<SettingsRow>(state.settingsCursorRow), state.caps),
                   std::string("SETTINGS[") + who + "]: UP never lands on a hidden row");
                dispatch.on_dpad_up();
            }
            eq(static_cast<int>(seenUp.size()), expected,
               std::string("SETTINGS[") + who + "]: UP reaches every visible row (and wraps)");
        }

        // The shell has NINE of the thirteen; Android has all thirteen (in a debug build).
        //
        // ⚠️ **EIGHT until S10, and this assertion is what noticed.** RESUME (row 11) was caps-gated OFF
        // while there was no autosave for it to configure; S10 built one, `PlatformCaps::sdl().autosave`
        // went true, and this line went red on the next run naming the exact delta (got 9, want 8).
        // That is the check working, not the check breaking — a row map that can change under the port
        // without a single test noticing is the thing worth being afraid of.
        state.caps = PlatformCaps::sdl(true);
        int shellRows = 0;
        for (int r = 0; r < SETTINGS_ROW_COUNT; ++r)
            if (settings_row_visible(static_cast<SettingsRow>(r), state.caps)) ++shellRows;
        eq(shellRows, 9,
           "SETTINGS[sdl]: nine rows — SCALING, KB, CURSOR, PREV, VIZ, THEME, TPL, RESUME (S10), TRACE");
        ok(settings_row_visible(SettingsRow::RESUME, state.caps),
           "SETTINGS[sdl]: …and RESUME is one of them — the row is BACK, because the autosave exists");

        state.caps = PlatformCaps::android(true);
        int androidRows = 0;
        for (int r = 0; r < SETTINGS_ROW_COUNT; ++r)
            if (settings_row_visible(static_cast<SettingsRow>(r), state.caps)) ++androidRows;
        eq(androidRows, 13, "SETTINGS[android+debug]: all thirteen — this is what ptinput compares against");

        state.caps = PlatformCaps::android(false);
        int androidRel = 0;
        for (int r = 0; r < SETTINGS_ROW_COUNT; ++r)
            if (settings_row_visible(static_cast<SettingsRow>(r), state.caps)) ++androidRel;
        eq(androidRel, 11, "SETTINGS[android+release]: OVERLAY and TRACE drop out (BuildConfig.DEBUG)");
    }

    // ── 12b. B LEAVES SETTINGS — and lands on the screen it was entered FROM ─────────────────────
    //
    // Reported from the device (Phase 4): SETTINGS could only be left with R+DPAD. Every other
    // full-screen destination in the app answers B, and the port simply had no arm — Kotlin's
    // `handleButtonB` opens with one (AppInputDispatcher.kt:2057) and the transcription missed it.
    //
    // ⚠️ ptinput is structurally blind to this and always will be: B here is not an EDIT. It resolves no
    // cursor context, produces no action and writes no cell — the three things every one of its 22,929
    // cases compares. It is a screen change, which is the join between the dispatcher and navigation, and
    // that join is the whole reason this file exists.
    {
        state.caps = PlatformCaps::sdl(true);

        // Entered the way a user enters it: PROJECT → SYSTEM → A.
        state.currentScreen       = ScreenType::PROJECT;
        state.projectCursorRow    = static_cast<int>(ProjectRow::SYSTEM);
        state.projectCursorColumn = 1;
        dispatch.on_button_a();
        ok(state.currentScreen == ScreenType::SETTINGS,
           "SETTINGS/B(control): PROJECT → SYSTEM → A opens SETTINGS — so the B below acts on the real screen");

        // ⚠️ THE CHECK THAT CAN TELL THE TWO FIELDS APART, and without it this section proves nothing:
        // `previousScreen` is poisoned to a screen SETTINGS was never entered from. It is the FILE
        // BROWSER's and the SAMPLE EDITOR's return target, and raising either FROM settings (LOAD THEME
        // does exactly that) moves it — so a B arm riding on `previousScreen`, which is the obvious way
        // to write this, passes every test that does not poison it and strands the user on a screen they
        // never came from. That is why Android carries a second field, and why the port now does.
        state.previousScreen = ScreenType::SAMPLE_EDITOR;

        dispatch.on_button_b();
        ok(state.currentScreen == ScreenType::PROJECT,
           "⚠️ SETTINGS/B: B returns to where SETTINGS was opened from — settingsReturnScreen, NOT previousScreen");
        eq(state.projectCursorRow, static_cast<int>(ProjectRow::SYSTEM),
           "SETTINGS/B: …with PROJECT's cursor still on SYSTEM (go_to_screen resets neither PROJECT nor SETTINGS)");

        // The modal rule, and the reason this arm sits BELOW the modals rather than at the top of B:
        // SETTINGS' own A raises the THEME EDITOR. While it is up B must close IT — close the SCREEN
        // instead and the editor is yanked out from under the user, still flagged open. It is opened here
        // through the REAL gesture (A on row 9) rather than by poking `themeEditor.isOpen`: the modal's
        // own entry path is part of what is under test, and `open_theme_editor` is private for good reason.
        state.currentScreen     = ScreenType::SETTINGS;
        state.settingsCursorRow = static_cast<int>(SettingsRow::THEME);
        dispatch.on_button_a();
        ok(state.themeEditor.isOpen,
           "SETTINGS/B(control): A on the THEME row raises the theme editor — so the B below has a modal to own it");

        dispatch.on_button_b();
        ok(state.currentScreen == ScreenType::SETTINGS,
           "⚠️ SETTINGS/B: a modal over SETTINGS still owns B — the theme editor closes, the screen stays");
        ok(!state.themeEditor.isOpen, "SETTINGS/B: …and it is the editor that closed");

        dispatch.on_button_b();   // now that the modal is gone, B means leave
        ok(state.currentScreen == ScreenType::PROJECT, "SETTINGS/B: …and the next B leaves");
    }

    // ── 13. The cursor cannot be LOST on entry — the guard Kotlin cannot need ────────────────────
    //
    // The default row is 0 (LAYOUT), which the SHELL does not draw. Without the bounds check in
    // go_to_screen, the very first entry into SETTINGS would put the cursor on an invisible row.
    {
        state.caps            = PlatformCaps::sdl(true);
        state.currentScreen   = ScreenType::PHRASE;
        state.settingsCursorRow = 0;   // LAYOUT — hidden here

        NavResult nav;
        nav.screen = ScreenType::SETTINGS;
        nav.column = state.previousColumn;
        go_to_screen(state, nav);

        ok(settings_row_visible(static_cast<SettingsRow>(state.settingsCursorRow), state.caps),
           "⚠️ SETTINGS: entering with the cursor on a row this platform HIDES snaps it to a visible one");
        eq(state.settingsCursorRow, static_cast<int>(SettingsRow::SCALING),
           "SETTINGS: …specifically to SCALING, the shell's first visible row");
    }

    // ── 14. TRACE's second column is caps-dependent — the one row whose WIDTH differs ────────────
    {
        state.currentScreen      = ScreenType::SETTINGS;
        state.settingsCursorRow  = static_cast<int>(SettingsRow::TRACE);

        state.caps = PlatformCaps::android(true);
        state.settingsCursorColumn = 1;
        dispatch.on_dpad_right();
        eq(state.settingsCursorColumn, 2, "SETTINGS[android]: RIGHT on TRACE reaches ENG (column 2)");

        state.caps = PlatformCaps::sdl(true);
        state.settingsCursorColumn = 1;
        dispatch.on_dpad_right();
        eq(state.settingsCursorColumn, 1,
           "⚠️ SETTINGS[sdl]: RIGHT on TRACE does NOT move — there is no second sequencer to select");

        // …and TEMPLATE's second column is there on both.
        state.settingsCursorRow    = static_cast<int>(SettingsRow::TEMPLATE);
        state.settingsCursorColumn = 1;
        dispatch.on_dpad_right();
        eq(state.settingsCursorColumn, 2, "SETTINGS[sdl]: RIGHT on TEMPLATE reaches CLEAR");
        dispatch.on_dpad_left();
        eq(state.settingsCursorColumn, 1, "SETTINGS: LEFT SNAPS back to column 1 (it does not step)");
    }

    // ── 15. PROJECT's cursor: rows wrap, and every row change snaps the column back to 1 ─────────
    {
        state.caps               = PlatformCaps::sdl(true);
        state.currentScreen      = ScreenType::PROJECT;
        state.projectCursorRow   = 0;
        state.projectCursorColumn = 1;

        dispatch.on_dpad_up();
        eq(state.projectCursorRow, 7, "PROJECT[sdl]: UP from row 0 WRAPS to EXIT (row 7)");

        state.caps = PlatformCaps::android(true);
        state.projectCursorRow = 0;
        dispatch.on_dpad_up();
        eq(state.projectCursorRow, 6, "PROJECT[android]: …wraps to SYSTEM (row 6) — there is no EXIT");

        // The NAME row is 20 columns wide; every other row is 1, 2 or 3. Carry a column across and it
        // would land nowhere — which is why the row change resets it.
        state.caps = PlatformCaps::sdl(true);
        state.projectCursorRow = static_cast<int>(ProjectRow::NAME);
        state.projectCursorColumn = 1;
        for (int i = 0; i < 30; ++i) dispatch.on_dpad_right();
        eq(state.projectCursorColumn, 20, "PROJECT: RIGHT walks NAME to column 20 and stops (20 chars)");

        dispatch.on_dpad_down();
        eq(state.projectCursorRow, static_cast<int>(ProjectRow::PROJECT),
           "PROJECT: DOWN off NAME lands on the PROJECT row");
        eq(state.projectCursorColumn, 1,
           "⚠️ PROJECT: …and the column SNAPS back to 1 — column 20 does not exist on a 3-button row");

        for (int i = 0; i < 5; ++i) dispatch.on_dpad_right();
        eq(state.projectCursorColumn, 3, "PROJECT: RIGHT stops at NEW (column 3)");

        for (int i = 0; i < 5; ++i) dispatch.on_dpad_left();
        eq(state.projectCursorColumn, 1,
           "PROJECT: LEFT stops at column 1 — column 0 is the LABEL and is not a cell");

        // ⚠️ THE DEFERRED-A LATCH, which is what lets THREE gestures live on one cell. On the NAME row,
        // plain A opens the KEYBOARD, A+UP walks that character through the alphabet, and A+B blanks it.
        // Fire the open on the PRESS and the last two become unreachable — so the mapper holds the press
        // until A is RELEASED, and cancels it if any A-combo fires in between (S6a built this for
        // INSTRUMENT's NAME cell; PROJECT's is the sharper case, since its 20 characters are 20 columns).
        state.projectCursorRow = static_cast<int>(ProjectRow::NAME);
        ok(dispatch.defer_a_to_release(),
           "⚠️ PROJECT: A on the NAME row is DEFERRED to the release — A+UP and A+B share the cell");

        state.projectCursorRow = static_cast<int>(ProjectRow::PROJECT);
        ok(!dispatch.defer_a_to_release(),
           "PROJECT: …but SAVE/LOAD/NEW are NOT deferred — read-only cells with no A+DPAD to protect");
        state.projectCursorRow = static_cast<int>(ProjectRow::EXPORT);
        ok(!dispatch.defer_a_to_release(), "PROJECT: …nor MIX/STEMS");
    }

    // ── 16. THE MODAL RULE: a confirm owns every button but A and B ──────────────────────────────
    //
    // ⚠️ THIS IS THE ASSERTION THAT MAKES THE 28 GUARDS SAFE, not the code shape. Kotlin's own comment
    // on this predicate warns that "every new show*Dialog-style modal state MUST be added" to it — a
    // rule you must remember at 28 call sites is a rule you will forget once, and the symptom is a
    // button that does the wrong thing exactly once, which nobody reports because it reads as a
    // mis-press. So: raise a dialog, press EVERYTHING, and assert that nothing moved.
    {
        state.caps                = PlatformCaps::sdl(true);
        state.currentScreen       = ScreenType::PROJECT;
        state.projectCursorRow    = static_cast<int>(ProjectRow::COMPACT);
        state.projectCursorColumn = 1;   // SEQ

        dispatch.on_button_a();
        ok(state.confirm.is_open(), "CONFIRM: A on COMPACT/SEQ raises a dialog rather than compacting");
        eq(static_cast<int>(state.confirm.kind), static_cast<int>(ConfirmDialogState::Kind::CLEAN_SEQ),
           "CONFIRM: …and it is the CLEAN SEQ one");

        const int rowBefore    = state.projectCursorRow;
        const int colBefore    = state.projectCursorColumn;
        const auto screenBefore = state.currentScreen;

        // Every button except A and B. Not one of them may do anything.
        dispatch.on_dpad_up();    dispatch.on_dpad_down();
        dispatch.on_dpad_left();  dispatch.on_dpad_right();
        dispatch.on_a_up();       dispatch.on_a_down();
        dispatch.on_a_left();     dispatch.on_a_right();
        dispatch.on_a_b();        dispatch.on_a_a();      dispatch.on_a_released();
        dispatch.on_b_left();     dispatch.on_b_right();
        dispatch.on_b_up();       dispatch.on_b_down();
        dispatch.on_r_up();       dispatch.on_r_down();
        dispatch.on_r_left();     dispatch.on_r_right();
        dispatch.on_l_b();        dispatch.on_l_a();      dispatch.on_l_r();  dispatch.on_l_b_a();
        dispatch.on_select();     dispatch.on_select_a(); dispatch.on_select_b(); dispatch.on_select_r();
        dispatch.on_start();

        ok(state.confirm.is_open(), "⚠️ MODAL RULE: 28 other buttons leave the dialog OPEN");
        eq(state.projectCursorRow, rowBefore, "MODAL RULE: …the cursor row did not move");
        eq(state.projectCursorColumn, colBefore, "MODAL RULE: …nor the column");
        eq(static_cast<int>(state.currentScreen), static_cast<int>(screenBefore),
           "MODAL RULE: …and R+DPAD did not change screen out from under it");

        dispatch.on_button_b();
        ok(!state.confirm.is_open(), "CONFIRM: B is NO — it closes without doing the thing");
    }

    // ── 17. NEW asks only when there is something to lose ────────────────────────────────────────
    {
        state.caps                = PlatformCaps::sdl(true);
        state.currentScreen       = ScreenType::PROJECT;
        state.projectCursorRow    = static_cast<int>(ProjectRow::PROJECT);
        state.projectCursorColumn = 3;   // NEW

        // Clean: no question asked.
        state.projectVersion = state.savedProjectVersion = 0;
        host.edit_project().tempo = 155;
        dispatch.on_button_a();
        ok(!state.confirm.is_open(), "NEW: a CLEAN project is replaced with no question asked");
        eq(host.project().tempo, 128, "NEW: …and the document really is factory-fresh again");
        eq(host.project().version, 1, "⚠️ NEW: version = 1, not 0 — 0 means a PRE-VERSIONING file");

        // Dirty: it asks.
        state.currentScreen       = ScreenType::PHRASE;
        state.cursorRow = 0; state.cursorColumn = 2;   // a phrase VELOCITY cell
        dispatch.on_a_up();
        ok(state.project_dirty(), "DIRTY: an edit on PHRASE marks the project dirty");

        state.currentScreen       = ScreenType::PROJECT;
        state.projectCursorRow    = static_cast<int>(ProjectRow::PROJECT);
        state.projectCursorColumn = 3;
        dispatch.on_button_a();
        ok(state.confirm.is_open(), "NEW: a DIRTY project asks first");
        eq(static_cast<int>(state.confirm.kind),
           static_cast<int>(ConfirmDialogState::Kind::NEW_PROJECT), "NEW: …with the NEW PROJECT? dialog");

        dispatch.on_button_a();   // YES
        ok(!state.project_dirty(), "NEW: confirming it leaves the document clean");
    }

    // ── 18. EXIT — the shell's row, which Android has no counterpart for ─────────────────────────
    {
        state.caps                = PlatformCaps::sdl(true);
        state.currentScreen       = ScreenType::PROJECT;
        state.projectCursorRow    = static_cast<int>(ProjectRow::EXIT);
        state.projectCursorColumn = 1;
        state.shouldQuit          = false;

        state.projectVersion = state.savedProjectVersion = 0;   // clean
        dispatch.on_button_a();
        ok(state.shouldQuit, "EXIT: a clean project quits outright");

        state.shouldQuit = false;
        state.projectVersion = 5; state.savedProjectVersion = 0;   // dirty
        dispatch.on_button_a();
        ok(!state.shouldQuit, "⚠️ EXIT: a DIRTY project does NOT quit…");
        eq(static_cast<int>(state.confirm.kind), static_cast<int>(ConfirmDialogState::Kind::EXIT),
           "EXIT: …it asks — there is no autosave to make a lost song survivable");
        dispatch.on_button_b();   // NO
        ok(!state.shouldQuit, "EXIT: B cancels, and the app stays up");

        dispatch.on_button_a();   // ask again
        dispatch.on_button_a();   // YES
        ok(state.shouldQuit, "EXIT: A confirms, and the frame loop is told to leave");

        // …and on Android there is no such row to press.
        state.caps             = PlatformCaps::android(true);
        state.shouldQuit       = false;
        state.projectCursorRow = static_cast<int>(ProjectRow::EXIT);   // out of range there
        dispatch.on_button_a();
        ok(!state.shouldQuit, "EXIT[android]: the row does not exist, and A on it does nothing");
    }

    // ── 19. SAVE / LOAD, on a real disk ──────────────────────────────────────────────────────────
    //
    // ⚠️ This is the session's Definition of Done: "the shell still cannot save a project". It can now,
    // and this is what says so — a real .ptp, written and read back, with NO engine in the process.
    {
        state.caps          = PlatformCaps::sdl(true);
        state.currentScreen = ScreenType::PROJECT;
        state.confirm.close();

        songcore::Project& p = host.edit_project();
        p.name  = "SAVE TEST";     // ⚠️ a SPACE — the filename must be sanitized, the NAME must not be
        p.tempo = 141;
        p.transpose = 0x0C;
        state.projectVersion = 9; state.savedProjectVersion = 0;

        state.projectCursorRow    = static_cast<int>(ProjectRow::PROJECT);
        state.projectCursorColumn = 1;   // SAVE
        dispatch.on_button_a();

        eqs(state.statusMessage, "SAVED", "SAVE: reports back through the status line…");
        ok(state.statusSuccess, "SAVE: …as a success");
        ok(!state.project_dirty(), "SAVE: …and the document is no longer dirty");

        const fs::path written = tree.root / "Projects" / "SAVE_TEST.ptp";
        ok(fs::exists(written), "⚠️ SAVE: the file is SAVE_TEST.ptp — the space is sanitized OUT of the "
                                "filename (and left alone in the project's name)");

        // ⚠️ AN ANDROID BUG, FOUND BY PORTING. An EMPTY name sanitizes to an empty filename, so the
        // save used to write "<Projects>/.ptp" — a DOTFILE, which the browser skips. The save reported
        // SAVED, went green, and the file was invisible to the app forever. Reachable: A+B every
        // character on the NAME row. Fixed on both platforms (FileController.saveProject).
        p.name = "";
        dispatch.on_button_a();
        ok(fs::exists(tree.root / "Projects" / "UNTITLED.ptp"),
           "⚠️ SAVE: an EMPTY name falls back to UNTITLED.ptp — never to '.ptp', which no browser lists");
        ok(!fs::exists(tree.root / "Projects" / ".ptp"), "SAVE: …and no dotfile is left behind");
        p.name = "SAVE TEST";

        // Now break the document, and LOAD it back.
        p.tempo = 90; p.transpose = 0; p.name = "SCRIBBLE";
        ok(host.load_project_file(written.generic_string(), tree.root.generic_string()),
           "LOAD: the .ptp parses back");
        eq(host.project().tempo, 141, "LOAD: …tempo restored");
        eq(host.project().transpose, 0x0C, "LOAD: …transpose restored");
        eqs(host.project().name, "SAVE TEST", "LOAD: …and the NAME kept its space");
    }

    // ── 20. COMPACT — the surgery, and the transitive table walk ─────────────────────────────────
    {
        state.caps          = PlatformCaps::sdl(true);
        state.currentScreen = ScreenType::PROJECT;
        state.confirm.close();

        songcore::Project& p = host.edit_project();
        p = songcore::make_default_project();

        // A song that reaches: chain 5 → phrase 9 → instrument 3, and (via a TBL in phrase 9)
        // table 40 — which itself carries a GRV pointing at groove 7 and a TBL at table 41.
        p.tracks[0].chainRefs.assign(256, -1);
        p.tracks[0].chainRefs[0] = 5;
        p.chains[5].phraseRefs[0] = 9;
        p.phrases[9].steps[0].note       = songcore::Note::C4();
        p.phrases[9].steps[0].instrument = 3;
        p.phrases[9].steps[0].fx1Type    = songcore::FX_TBL;
        p.phrases[9].steps[0].fx1Value   = 40;
        p.tables[40].rows[0].fx1Type  = songcore::FX_GRV;
        p.tables[40].rows[0].fx1Value = 7;
        p.tables[40].rows[1].fx1Type  = songcore::FX_TBL;
        p.tables[40].rows[1].fx1Value = 41;
        p.tables[41].rows[0].transpose = 0x42;   // …so we can see whether it survived
        p.grooves[7].steps[0] = 9;

        // …and a lot that it does not.
        p.chains[6].phraseRefs[0]  = 200;
        p.phrases[200].steps[0].note = songcore::Note::C4();
        p.instruments[99].name = "ORPHAN";
        p.tables[99].rows[0].transpose = 0x11;
        p.grooves[99].steps[0] = 3;

        state.projectCursorRow    = static_cast<int>(ProjectRow::COMPACT);
        state.projectCursorColumn = 2;   // INST
        dispatch.on_button_a();
        ok(state.confirm.is_open(), "COMPACT: A on INST asks first");
        dispatch.on_button_a();          // YES

        eqs(host.project().instruments[3].name, "INST03", "COMPACT INST: the USED instrument survives");
        eqs(host.project().instruments[99].name, "INST63",   // ⚠️ HEX: slot 99 = 0x63
            "COMPACT INST: the orphan is back to factory");
        eq(host.project().instruments[99].sampleId, -1,
           "⚠️ COMPACT: a cleaned slot has sampleId = -1 (the FIELD default) — where a slot in a FRESH "
           "project has sampleId = i. The two do not even serialize alike, and that is Kotlin's.");

        eq(host.project().tables[41].rows[0].transpose, 0x42,
           "⚠️ COMPACT: table 41 SURVIVES — it is reached only from INSIDE table 40, and the walk is "
           "TRANSITIVE");
        eq(host.project().grooves[7].steps[0], 9,
           "⚠️ COMPACT: groove 7 survives — reached only from a table's own GRV row");
        eq(host.project().tables[99].rows[0].transpose, 0x00, "COMPACT: the orphan table is wiped");
        eq(host.project().grooves[99].steps[0], -1, "COMPACT: the orphan groove is wiped");
        eq(host.project().grooves[0].steps[0], -1, "COMPACT: groove 0 is always kept");

        // SEQ leaves the instruments alone and takes the arrangement.
        state.projectCursorColumn = 1;   // SEQ
        dispatch.on_button_a();
        dispatch.on_button_a();          // YES
        eq(static_cast<int>(host.project().chains[5].phraseRefs[0]), 9,
           "COMPACT SEQ: the used chain survives");
        eq(static_cast<int>(host.project().chains[6].phraseRefs[0]), -1,
           "COMPACT SEQ: the unused chain is wiped");
        ok(host.project().phrases[200].steps[0].note == songcore::Note::EMPTY(),
           "COMPACT SEQ: …and so is the phrase only IT referenced");
    }

    // ── 21. The song TEMPLATE, and settings.json ─────────────────────────────────────────────────
    {
        state.caps          = PlatformCaps::sdl(true);
        state.currentScreen = ScreenType::SETTINGS;
        state.confirm.close();

        host.edit_project().tempo = 174;

        state.settingsCursorRow    = static_cast<int>(SettingsRow::TEMPLATE);
        state.settingsCursorColumn = 1;   // SAVE
        dispatch.on_button_a();
        eqs(state.statusMessage, "TEMPLATE SAVED", "TEMPLATE: A on SAVE writes it");
        ok(fs::exists(fs::path(fs_impl.template_project_path())), "TEMPLATE: …and the file is there");

        state.settingsCursorColumn = 2;   // CLEAR
        dispatch.on_button_a();
        eqs(state.statusMessage, "TEMPLATE CLEARED", "TEMPLATE: A on CLEAR deletes it");
        ok(!fs::exists(fs::path(fs_impl.template_project_path())), "TEMPLATE: …and it is really gone");

        dispatch.on_button_a();   // …again, on nothing
        ok(state.statusSuccess,
           "⚠️ TEMPLATE: clearing an ABSENT template SUCCEEDS — it is a no-op, not a failure (Kotlin's)");

        // settings.json — the round trip. A setting that resets on every launch is a setting nobody
        // will touch twice.
        state.settings.cursorRemember    = true;
        state.settings.insertBefore      = false;
        state.settings.notePreviewEnabled = false;
        state.settings.scalingBilinear   = true;
        state.theme = theme_amber();
        state.theme.visualizerType = VisualizerType::SPECTRUM_PEAKS;
        ok(save_settings(fs_impl, state.settings, state.theme), "settings.json: written");

        SettingsValues back{};
        Theme          backTheme = theme_classic();
        ok(load_settings(fs_impl, back, backTheme), "settings.json: read back");
        ok(back.cursorRemember, "settings.json: cursorRemember round-trips");
        ok(!back.insertBefore, "settings.json: insertBefore round-trips");
        ok(!back.notePreviewEnabled, "settings.json: notePreview round-trips");
        ok(back.scalingBilinear, "settings.json: scaling round-trips");
        eqs(backTheme.name, "AMBER", "settings.json: the theme round-trips BY NAME");
        eq(static_cast<int>(backTheme.visualizerType), static_cast<int>(VisualizerType::SPECTRUM_PEAKS),
           "⚠️ settings.json: …and the VISUALIZER rides across the theme swap, as Android carries it");

        // A file that is not there is not an error: it is the first launch.
        StdFileSystem   emptyFs((tree.root / "nowhere").generic_string());
        SettingsValues  fresh{};
        Theme           freshTheme = theme_classic();
        ok(!load_settings(emptyFs, fresh, freshTheme),
           "settings.json: a missing file reads as FALSE (first launch), and the defaults stand");
        ok(fresh.insertBefore, "settings.json: …with insertBefore still at its factory default");
    }

    // ── 22. The A+DPAD edit path on SETTINGS does NOT dirty the project ──────────────────────────
    //
    // Turning the visualizer on is not a change to the song, and it must not put a "you have unsaved
    // work" question in front of the next NEW or EXIT.
    {
        state.caps          = PlatformCaps::sdl(true);
        state.currentScreen = ScreenType::SETTINGS;
        state.projectVersion = state.savedProjectVersion = 3;

        state.settingsCursorRow    = static_cast<int>(SettingsRow::NOTE_PREV);
        state.settingsCursorColumn = 1;
        const bool before = state.settings.notePreviewEnabled;
        dispatch.on_a_up();

        ok(state.settings.notePreviewEnabled != before, "SETTINGS: A+UP toggles NOTE PREV");
        ok(!state.project_dirty(),
           "⚠️ SETTINGS: …and does NOT dirty the PROJECT — a setting is not a song");

        // ⚠️⚠️ THIS LINE USED TO ASSERT A `settingsDirty` FLAG, AND THAT ASSERTION IS PART OF WHY THE
        // THEME BUG LIVED. It pinned the one mutation path that armed the write CORRECTLY and had no
        // counterpart for the one that did not (the theme editor — §27(c)) — a permanently green check
        // sitting directly on top of the hole, reading as coverage. The flag is gone now; what survives
        // is the claim it was always standing in for: after this edit, an exit WRITES.
        ok(save_settings_if_changed(fs_impl, state.settings, state.theme) == SettingsWrite::SAVED,
           "SETTINGS: …and an exit writes settings.json for it");
    }

    // ── 23. EXPORT — the one section that needs a REAL engine ────────────────────────────────────
    //
    // ⚠️ Every check above runs with a NULL engine, and S4 earned the right to say that: a document
    // edit does not need an audio device. A RENDER does. A render with no engine is not a degraded
    // render — it is silence, and every assertion about it would pass vacuously. (S6a made exactly
    // this argument about the FILESYSTEM, and for exactly this reason.) So this block builds its own
    // engine, its own host and its own dispatcher, and drives the PROJECT screen's EXPORT buttons.
    //
    // ⚠️ HEAP. AudioEngine's DSP scratch, spectrum rings and 256-slot table pool are members, and they
    // blow a 1 MB stack instantly if it is constructed as a local (S6b).
    //
    // What this proves that `ptrender` does not: ptrender calls `render_song_to_wav` directly. Nothing
    // in it goes through the PROJECT screen — through the cursor, the A button, `export_song`, the
    // stems PLAN, the folder creation and the `_0001` naming. That whole path is new in S7, and it is
    // the path a user actually presses.
    {
        auto engine = std::make_unique<AudioEngine>();
        engine->setDeviceSampleRate(44100);

        songcore::SongcoreHost rhost(engine.get(), 44100);

        AppState rstate;
        rstate.project = &rhost.edit_project();
        rstate.caps    = PlatformCaps::sdl(true);

        InputDispatcher rdispatch(rstate, rhost, fs_impl);

        // A minimal but AUDIBLE song: two tracks, so the stems plan yields two track stems, and one
        // instrument with a reverb send, so it yields a reverb return as well.
        songcore::Project& p = rhost.edit_project();
        p = songcore::make_default_project();
        p.name = "EXPORT TEST";
        p.instruments[0].reverbSend = 0x40;
        for (int track = 0; track < 2; ++track) {
            p.tracks[track].chainRefs.assign(256, -1);
            p.tracks[track].chainRefs[0] = track;
            p.chains[track].phraseRefs[0] = track;
            for (int step = 0; step < 4; ++step) {
                songcore::PhraseStep& s = p.phrases[track].steps[static_cast<size_t>(step * 4)];
                s.note       = songcore::Note::C4();
                s.instrument = 0;
            }
        }
        rhost.push_params();

        // The plan, before the render — it is what decides how many files there will be.
        const std::vector<songcore::StemPass> plan = songcore::stems_plan(p);
        eq(static_cast<int>(plan.size()), 3,
           "STEMS: two active tracks + the reverb return (instrument 0 feeds it) = three passes");
        eqs(plan[0].suffix, "_1", "⚠️ STEMS: track stems are numbered SEQUENTIALLY (_1.._N), not by track id");
        eqs(plan[2].suffix, "_reverb", "STEMS: …and the send returns come last");

        // MIX, through the button.
        rstate.currentScreen       = ScreenType::PROJECT;
        rstate.projectCursorRow    = static_cast<int>(ProjectRow::EXPORT);
        rstate.projectCursorColumn = 1;
        rdispatch.on_button_a();

        eqs(rstate.statusMessage, "EXPORTED!", "EXPORT: A on MIX renders and reports back");
        ok(!rstate.isRendering, "EXPORT: …and the render flag is down again afterwards");

        const fs::path mix = tree.root / "Renders" / "EXPORT_TEST_0001.wav";
        ok(fs::exists(mix), "EXPORT: the WAV is Renders/EXPORT_TEST_0001.wav (name sanitized, counter added)");
        ok(fs::exists(mix) && fs::file_size(mix) > 44 * 100,
           "EXPORT: …and it has real audio in it, not just a 44-byte header");

        // A second MIX must not overwrite the first — the counter walks.
        rdispatch.on_button_a();
        ok(fs::exists(tree.root / "Renders" / "EXPORT_TEST_0002.wav"),
           "EXPORT: a second render counts up rather than overwriting");

        // STEMS, through the button.
        rstate.projectCursorColumn = 2;
        rdispatch.on_button_a();
        eqs(rstate.statusMessage, "STEMS EXPORTED!", "STEMS: A on STEMS renders the set");

        const fs::path stemDir = tree.root / "Renders" / "EXPORT_TEST";
        ok(fs::exists(stemDir / "EXPORT_TEST_1.wav"),      "STEMS: track 1 is written…");
        ok(fs::exists(stemDir / "EXPORT_TEST_2.wav"),      "STEMS: …track 2…");
        ok(fs::exists(stemDir / "EXPORT_TEST_reverb.wav"), "STEMS: …and the reverb return");
        int stemCount = 0;
        for (const auto& e : fs::directory_iterator(stemDir)) { (void)e; ++stemCount; }
        eq(stemCount, 3, "STEMS: exactly three files — the plan and the disk agree");

        // An EMPTY song has nothing to export, and says so rather than writing a 0-second WAV.
        p = songcore::make_default_project();
        p.name = "EMPTY";
        rdispatch.on_button_a();
        eqs(rstate.statusMessage, "SONG IS EMPTY", "EXPORT: an empty song is refused, not rendered");
        ok(!rstate.statusSuccess, "EXPORT: …and it is reported as a failure (red), not a success");
    }

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // ── 24. THE EQ EDITOR (S8) — the overlay, and everything ptinput is blind to ─────────────────
    // ═════════════════════════════════════════════════════════════════════════════════════════════
    //
    // ptinput proves the MODULE matches Kotlin: given a slot and a cursor row, the context, the action
    // and the resulting band are byte-identical, over 2,000 cases. Nothing in it proves any of the
    // following, and every one of them is a place a real bug can live:
    //
    //   · that the cursor can REACH the editor at all — five different cells raise it
    //   · that it opens on the RIGHT SLOT, remembering WHICH cell asked
    //   · that B+LEFT/RIGHT writes the new slot back into that cell's own project field (five fields)
    //   · that B closes it, SELECT closes it, and R+DPAD cannot navigate out from under it
    //   · that the deferred-A / deferred-B latches answer correctly
    //   · and — the one that needs a real engine — that a band dialled here reaches the AUDIO
    {
        songcore::SongcoreHost ehost(nullptr, 44100);
        AppState               estate;
        estate.project = &ehost.edit_project();
        estate.caps    = PlatformCaps::sdl(true);
        InputDispatcher ed(estate, ehost, fs_impl);

        songcore::Project& p = ehost.edit_project();
        p = songcore::make_default_project();

        // ── 24a. All five callers can raise it, on the right slot ────────────────────────────────

        // MIXER's master EQ. ⚠️ Start it UNASSIGNED (−1), which is the state a fresh project is in.
        estate.currentScreen     = ScreenType::MIXER;
        estate.mixerMasterRow    = 1;
        estate.mixerCursorColumn = 8;
        p.masterEqSlot           = -1;
        ok(ed.defer_a_to_release(), "EQ: the master EQ cell DEFERS its A to release (A+DPAD dials the slot)");
        ed.on_button_a();
        ok(estate.eq.isOpen, "EQ: A on MIXER's master EQ cell opens the editor");
        ok(estate.eq.caller.kind == EqCallerContext::Kind::MASTER, "EQ: …with the MASTER caller");
        eq(estate.eq.slotIndex, 0, "⚠️ EQ: an UNASSIGNED EQ (−1) opens on slot 0 — −1 is a bypass, not a slot");
        eq(estate.eq.cursorRow, 0, "EQ: …and the cursor starts on BAND 1 / TYPE");
        ok(!ed.defer_a_to_release(),
           "⚠️ EQ: with the editor OPEN, no cell 'opens a sub-screen' any more — the modal owns A");
        ok(ed.defer_b_to_release(), "EQ: …and B is deferred, because it is both the close and the slot cycle");
        ed.on_button_b();
        ok(!estate.eq.isOpen, "EQ: B closes it");
        ok(!ed.defer_b_to_release(), "EQ: …and B stops being deferred the moment it is closed");

        // ⚠️ AND THIS IS WHAT THE DEFER LATCH IS PROTECTING. On the very same cell, A+DPAD must still dial
        // the slot NUMBER — the plain value the cell has held since S5. If A fired its open on the PRESS,
        // the editor would be up before the D-pad ever arrived and this gesture would be unreachable.
        p.masterEqSlot = 4;
        ed.on_a_up();
        eq(p.masterEqSlot, 5, "⚠️ EQ: A+DPAD on the EQ cell still DIALS THE SLOT — the whole point of the defer");
        ok(!estate.eq.isOpen, "EQ: …and does NOT open the editor");

        // EFFECTS' two input EQs.
        estate.currentScreen    = ScreenType::EFFECTS;
        estate.effectsCursorRow = EffectModule::ROW_DLY_EQ;
        p.delayInputEq          = 33;
        ed.on_button_a();
        ok(estate.eq.isOpen && estate.eq.caller.kind == EqCallerContext::Kind::DELAY_IN,
           "EQ: A on EFFECTS' DLY EQ row opens it with the DELAY_IN caller");
        eq(estate.eq.slotIndex, 33, "EQ: …on the slot that row held");
        ed.on_select();
        ok(!estate.eq.isOpen, "EQ: SELECT closes it too (B is deferred, so SELECT is the instant way out)");

        // INSTRUMENT — and the row depends on the instrument TYPE (12 on a sampler, 14 on a SoundFont).
        estate.currentScreen           = ScreenType::INSTRUMENT;
        estate.currentInstrument       = 5;
        p.instruments[5].eqSlot        = 77;
        estate.instrumentCursorColumn  = 1;
        estate.instrumentCursorRow     = 12;
        ed.on_button_a();
        ok(estate.eq.isOpen && estate.eq.caller.kind == EqCallerContext::Kind::INSTRUMENT,
           "EQ: A on INSTRUMENT's EQ row (12, a SAMPLER) opens it with the INSTRUMENT caller");
        eq(estate.eq.caller.instrId, 5, "EQ: …carrying WHICH instrument, captured at open time");
        eq(estate.eq.slotIndex, 77, "EQ: …on that instrument's own slot");
        ed.on_button_b();

        p.instruments[5].instrumentType = songcore::InstrumentType::SOUNDFONT;
        estate.instrumentCursorRow      = 12;
        ed.on_button_a();
        ok(!estate.eq.isOpen, "⚠️ EQ: row 12 is NOT the EQ row on a SOUNDFONT — its map is shifted");
        estate.instrumentCursorRow = 14;
        ed.on_button_a();
        ok(estate.eq.isOpen, "EQ: …row 14 is, and it opens there");
        ed.on_button_b();
        p.instruments[5].instrumentType = songcore::InstrumentType::SAMPLER;

        // INST.POOL, column 4.
        estate.currentScreen    = ScreenType::INST_POOL;
        estate.poolCursorColumn = 4;
        ed.on_button_a();
        ok(estate.eq.isOpen && estate.eq.caller.kind == EqCallerContext::Kind::INSTRUMENT,
           "EQ: A on the pool's EQ column opens it for the instrument under the cursor");
        eq(estate.eq.slotIndex, 77, "EQ: …on the same slot the INSTRUMENT screen showed");

        // ── 24b. ⚠️ THE ANDROID BUG: B+UP must not page the pool out from under the overlay ──────
        //
        // `handleBUp`/`handleBDown` are the only two handlers in the Kotlin dispatcher that never got an
        // EQ guard — and INST.POOL is the one screen that can BOTH raise the editor and respond to them.
        // Hold B (which does not close the editor: the deferred-B latch is holding it), press UP, and
        // Android pages `currentInstrument` sixteen slots while the editor stays open on the instrument
        // it was raised for. Close it and you are looking at a different instrument.
        {
            const int instBefore = estate.currentInstrument;
            ed.on_b_up();
            ed.on_b_down();
            eq(estate.currentInstrument, instBefore,
               "⚠️ EQ: B+UP/B+DOWN do NOT page the pool underneath the open editor (the Android bug)");
            ok(estate.eq.isOpen, "EQ: …and the editor is still up");
        }

        // ── 24c. The D-pad is the editor's, and it is a 3×4 grid, not a flat list of twelve ──────
        eq(estate.eq.cursorRow, 0, "EQ: the cursor opened on BAND 1 / TYPE");
        ed.on_dpad_up();
        eq(estate.eq.cursorRow, 0, "EQ: UP at the top of a band CLAMPS — it does not wrap to Q");
        ed.on_dpad_left();
        eq(estate.eq.cursorRow, 0, "EQ: LEFT at band 1 CLAMPS");
        ed.on_dpad_down(); ed.on_dpad_down();
        eq(estate.eq.cursorRow, 2, "EQ: DOWN twice → BAND 1 / GAIN");
        ed.on_dpad_right();
        eq(estate.eq.cursorRow, 6, "⚠️ EQ: RIGHT changes BAND and KEEPS the param (band 2's GAIN, row 6)");
        ed.on_dpad_right(); ed.on_dpad_right();
        eq(estate.eq.cursorRow, 10, "EQ: …and RIGHT clamps at band 3 (row 10, still GAIN)");
        ed.on_dpad_down(); ed.on_dpad_down();
        eq(estate.eq.cursorRow, 11, "EQ: DOWN clamps at Q (row 11), the last param of the last band");

        // ── 24d. R+DPAD is SWALLOWED — there is no cell in the 5×5 grid to navigate from ─────────
        {
            const auto screenBefore = estate.currentScreen;
            ed.on_r_up(); ed.on_r_down(); ed.on_r_left(); ed.on_r_right();
            eq(static_cast<int>(estate.currentScreen), static_cast<int>(screenBefore),
               "EQ: R+DPAD cannot navigate out from under the overlay");
            ok(estate.eq.isOpen, "EQ: …and it is still open");
        }

        // ── 24e. ⚠️ THE MODAL RULE: everything outside the editor's vocabulary is INERT ──────────
        //
        // The editor owns the D-pad, A, A+DPAD, A+B, B, B+DPAD and SELECT. Every OTHER button must do
        // nothing to the screen underneath — which is the claim S8 adds explicit `eq_open()` guards for
        // in on_a_a / on_l_a / on_l_b / on_l_b_a / on_l_r, where Kotlin has none and merely gets away
        // with it because those five are screen-gated to screens the editor cannot be raised from.
        {
            const int  instBefore  = estate.currentInstrument;
            const int  colBefore   = estate.poolCursorColumn;
            const auto screenBefore = estate.currentScreen;

            ed.on_a_a();      ed.on_a_released();
            ed.on_l_b();      ed.on_l_a();   ed.on_l_r();  ed.on_l_b_a();
            ed.on_select_a(); ed.on_select_b(); ed.on_select_r();

            ok(estate.eq.isOpen,        "MODAL RULE: the 9 out-of-vocabulary buttons leave the editor OPEN");
            ok(!estate.selection.active, "MODAL RULE: …L+B did not start a selection on the screen behind");
            eq(estate.currentInstrument, instBefore, "MODAL RULE: …nothing moved the instrument");
            eq(estate.poolCursorColumn,  colBefore,  "MODAL RULE: …nor the pool cursor");
            eq(static_cast<int>(estate.currentScreen), static_cast<int>(screenBefore),
               "MODAL RULE: …nor the screen");
        }
        ed.on_button_b();

        // ── 24f. B+LEFT/RIGHT cycles the SLOT — and writes it back to the cell that opened it ────
        //
        // Five different project fields, one gesture. This is what the caller tag exists for, and it is
        // the single thing ptinput is most structurally blind to: the module never learns who asked.
        {
            estate.currentScreen     = ScreenType::MIXER;
            estate.mixerMasterRow    = 1;
            estate.mixerCursorColumn = 8;
            p.masterEqSlot           = 10;
            ed.on_button_a();
            ed.on_b_right();
            eq(estate.eq.slotIndex, 11, "EQ SLOT: B+RIGHT steps the slot");
            eq(p.masterEqSlot, 11, "⚠️ EQ SLOT: …and writes it back into `masterEqSlot`, the cell that asked");
            ed.on_b_left(); ed.on_b_left();
            eq(p.masterEqSlot, 9, "EQ SLOT: B+LEFT steps back down");

            // ⚠️ It CLAMPS at both ends, where every other B+LEFT/RIGHT in the app WRAPS. The EQ bank is
            // an index you are pointing a bus at, not a ring you scroll: wrapping 127 → 0 would silently
            // re-point the master bus at a completely different curve.
            estate.eq.slotIndex = 0;
            ed.on_b_left();
            eq(estate.eq.slotIndex, 0, "⚠️ EQ SLOT: it CLAMPS at 0 — it does NOT wrap to 127");
            estate.eq.slotIndex = 127;
            ed.on_b_right();
            eq(estate.eq.slotIndex, 127, "⚠️ EQ SLOT: …and clamps at 127");
            eq(p.masterEqSlot, 127, "EQ SLOT: …still writing through to the project");
            ed.on_button_b();
        }

        // Each of the other four callers writes its OWN field, and only its own.
        {
            p.masterEqSlot = 1; p.reverbInputEq = 1; p.delayInputEq = 1; p.instruments[5].eqSlot = 1;

            estate.currentScreen    = ScreenType::EFFECTS;
            estate.effectsCursorRow = EffectModule::ROW_REV_EQ;
            ed.on_button_a(); ed.on_b_right(); ed.on_button_b();
            eq(p.reverbInputEq, 2, "EQ SLOT: the REV EQ cell writes `reverbInputEq`…");
            eq(p.delayInputEq, 1,  "EQ SLOT: …and not `delayInputEq`");
            eq(p.masterEqSlot, 1,  "EQ SLOT: …and not `masterEqSlot`");

            estate.effectsCursorRow = EffectModule::ROW_DLY_EQ;
            ed.on_button_a(); ed.on_b_right(); ed.on_button_b();
            eq(p.delayInputEq, 2,  "EQ SLOT: the DLY EQ cell writes `delayInputEq`…");
            eq(p.reverbInputEq, 2, "EQ SLOT: …and leaves the reverb's alone");

            estate.currentScreen          = ScreenType::INSTRUMENT;
            estate.instrumentCursorRow    = 12;
            estate.instrumentCursorColumn = 1;
            ed.on_button_a(); ed.on_b_right(); ed.on_button_b();
            eq(p.instruments[5].eqSlot, 2, "EQ SLOT: the instrument's EQ cell writes THAT instrument's eqSlot");
            eq(p.instruments[4].eqSlot, -1, "EQ SLOT: …and not its neighbour's");
            eq(p.masterEqSlot, 1, "EQ SLOT: …and not the master's");
        }

        // ── 24g. ⚠️ THE SECOND ANDROID BUG: a band edit must ADOPT the slot ──────────────────────
        //
        // Open the editor on an UNASSIGNED EQ. It shows slot 0 (−1 is a bypass value, not a slot). Now
        // dial a band. Kotlin told the ENGINE "use slot 0" and left `masterEqSlot` at −1 — so the EQ was
        // audible, the mixer cell still read "--", and the next save-and-reload silently threw it away
        // (the load path faithfully re-pushes the −1 the project still held). The project and the engine
        // must never disagree about which slot is live.
        {
            p.masterEqSlot           = -1;
            estate.currentScreen     = ScreenType::MIXER;
            estate.mixerMasterRow    = 1;
            estate.mixerCursorColumn = 8;
            ed.on_button_a();
            eq(estate.eq.slotIndex, 0, "EQ ADOPT: the editor opens on slot 0 over an unassigned EQ…");
            eq(p.masterEqSlot, -1, "EQ ADOPT: …and merely OPENING it assigns nothing (you might be looking)");

            ed.on_a_up();   // BAND 1 / TYPE: OFF → LOSHELF
            eq(p.eqPresets[0].bands[0].type, 1, "EQ ADOPT: A+UP dials the band…");
            eq(p.masterEqSlot, 0,
               "⚠️ EQ ADOPT: …and EDITING it ADOPTS slot 0 into the project (the Android bug)");
            ok(estate.project_dirty(), "EQ ADOPT: …and the song is now dirty");
            ed.on_button_b();
        }
    }

    // ── 25. …and the one EQ claim that needs a REAL ENGINE: does a band reach the AUDIO? ─────────
    //
    // ⚠️ THIS IS THE ASSERTION THE WHOLE SESSION RESTS ON, and no other tool in the ladder can make it.
    // ptinput sees the PROJECT change. Nothing in it — or in ptplay, or ptvoice — can see whether the
    // engine was ever TOLD. And the EQ's push is a two-call sequence where the obvious half is the
    // useless one: `setEqBand` writes the 128-slot BANK, but the master bus compiles its own coefficients
    // and never reads the bank again. Push only the band and the audio does not change by one sample.
    //
    // So: render the same song twice through the same engine — once flat, once with a savage HICUT at
    // 20 Hz dialled in THROUGH THE EDITOR — and measure. If the second render is not dramatically
    // quieter, the band never reached the audio.
    {
        auto engine = std::make_unique<AudioEngine>();   // ⚠️ HEAP — see §23
        engine->setDeviceSampleRate(44100);

        songcore::SongcoreHost qhost(engine.get(), 44100);
        AppState               qstate;
        qstate.project = &qhost.edit_project();
        qstate.caps    = PlatformCaps::sdl(true);
        InputDispatcher qd(qstate, qhost, fs_impl);

        songcore::Project& p = qhost.edit_project();
        p = songcore::make_default_project();
        p.name = "EQ AUDIO";
        p.tracks[0].chainRefs.assign(256, -1);
        p.tracks[0].chainRefs[0]  = 0;
        p.chains[0].phraseRefs[0] = 0;
        for (int step = 0; step < 4; ++step) {
            songcore::PhraseStep& s = p.phrases[0].steps[static_cast<size_t>(step * 4)];
            s.note       = songcore::Note::C4();
            s.instrument = 0;
        }

        // ⚠️ THE INSTRUMENT NEEDS A SOURCE, or the render is SILENCE and both audio checks below pass
        // vacuously against 0.0 — which is exactly the trap S6a named about the filesystem and S6b about
        // the engine. So the fixture is SYNTHESIZED rather than borrowed: a 1 kHz sine, decaying, mono at
        // the render's own rate. A formula is the better fixture (S6b's argument for the golden media),
        // and it keeps ptdispatch self-contained — it is the one tool with no /testdata argument.
        //
        // 1 kHz matters: it must sit WELL ABOVE the 20 Hz corner the HICUT is dialled to, so that a
        // working low-pass has to gut it. A sine at 30 Hz would survive the filter and the check would
        // be measuring nothing.
        {
            const int          rate = 44100;
            std::vector<float> tone(static_cast<size_t>(rate));   // 1 second
            for (size_t i = 0; i < tone.size(); ++i) {
                const double t   = static_cast<double>(i) / rate;
                const double env = std::exp(-4.0 * t);
                tone[i] = static_cast<float>(0.7 * env * std::sin(2.0 * 3.14159265358979 * 1000.0 * t));
            }
            const fs::path wav = tree.root / "Samples" / "eqtone.wav";
            ok(songcore::write_wav_mono(wav.generic_string(), tone, rate),
               "EQ AUDIO: the fixture tone is written");
            ok(qhost.load_sample(0, wav.generic_string()),
               "EQ AUDIO: …and loads into instrument 0, so the render has something to filter");
        }

        qhost.push_params();

        const auto render_rms = [&](const char* name) -> double {
            p.name = name;
            qstate.currentScreen       = ScreenType::PROJECT;
            qstate.projectCursorRow    = static_cast<int>(ProjectRow::EXPORT);
            qstate.projectCursorColumn = 1;   // MIX
            qd.on_button_a();

            const fs::path wav = tree.root / "Renders" / (std::string(name) + "_0001.wav");
            std::ifstream  f(wav, std::ios::binary);
            if (!f) return -1.0;
            f.seekg(44);   // past the canonical header
            double   sum = 0.0;
            long long n  = 0;
            int16_t  s16 = 0;
            while (f.read(reinterpret_cast<char*>(&s16), sizeof(s16))) {
                const double v = static_cast<double>(s16) / 32768.0;
                sum += v * v;
                ++n;
            }
            return n > 0 ? std::sqrt(sum / static_cast<double>(n)) : -1.0;
        };

        const double flat = render_rms("EQFLAT");
        ok(flat > 0.001, "EQ AUDIO: the un-EQ'd render has real audio in it (RMS > 0.001)");

        // Now dial a HICUT at the very bottom of the sweep, through the editor, exactly as a user would.
        qstate.currentScreen     = ScreenType::MIXER;
        qstate.mixerMasterRow    = 1;
        qstate.mixerCursorColumn = 8;
        p.masterEqSlot           = -1;
        qd.on_button_a();
        ok(qstate.eq.isOpen, "EQ AUDIO: the editor is open on the master bus");

        // BAND 1 / TYPE → 5 (HICUT). Five A+UPs from OFF.
        for (int i = 0; i < 5; ++i) qd.on_a_up();
        eq(p.eqPresets[0].bands[0].type, 5, "EQ AUDIO: BAND 1 TYPE is HICUT");

        // BAND 1 / FREQ → 0 (20 Hz). A+B resets it to 0x80, then A+LEFT (−16) eight times gets to 0.
        qd.on_dpad_down();
        for (int i = 0; i < 10; ++i) qd.on_a_left();
        eq(p.eqPresets[0].bands[0].freq, 0, "EQ AUDIO: …at 20 Hz, so it cuts essentially everything");

        qd.on_button_b();
        eq(p.masterEqSlot, 0, "EQ AUDIO: the band edit adopted slot 0 into the project");

        const double cut = render_rms("EQCUT");
        ok(cut >= 0.0, "EQ AUDIO: the EQ'd render was written");

        // ⚠️ THE CLAIM. A 20 Hz low-pass on the master bus must gut a C-4 sample. If the two renders are
        // the same loudness, the band was written into the bank and NOBODY WAS TOLD.
        ok(cut < flat * 0.5,
           "⚠️ EQ AUDIO: a HICUT at 20 Hz makes the render at least 6 dB quieter — the band REACHED THE "
           "AUDIO (remove the caller re-push in push_eq_band_to_engine and this is the check that dies)");
        std::printf("       [info] master-bus RMS: flat %.5f → HICUT@20Hz %.5f (%.1f%% of flat)\n",
                    flat, cut, flat > 0 ? 100.0 * cut / flat : 0.0);
    }

    // ── 26. A LIVE EDIT DURING PLAYBACK MUST NOT EAT A PENDING KIL ──────────────────────────────
    //
    // ⚠️ THIS SECTION EXISTS BECAUSE OF A BUG REPORT NOBODY COULD REPRODUCE, AND IT CLOSES A HOLE THAT
    // WAS ALWAYS THERE.
    //
    // After S8 shipped, editing an EQ band on device while the song played was reported to make a KIL'd
    // note ring forever. It could not be reproduced — a harness drove the real engine through 24
    // configurations of it (instrument EQ slot × edit cadence × KIL placement across a phrase boundary ×
    // both branches of Voice::noteOff) and every one killed correctly. The report may have been a stale
    // build. What the hunt DID establish is worth a standing test on its own:
    //
    //   **LIVE-EDIT RESCHEDULING HAS NO COVERAGE AT ALL, ON EITHER ENGINE.** `notify_data_changed()` is
    //   an event-schema SC-4 exclusion: the 36 golden traces are recorded from a sequencer that is never
    //   edited mid-flight, so ptplay cannot see it, and neither can anything else in the ladder. Yet it
    //   does the single most dangerous thing in the scheduler — it rolls the lookahead back to a
    //   checkpoint and calls `clearScheduledNotesFrom()`, which wipes the note queue, the param queue AND
    //   THE KILL QUEUE from that frame on. A KIL is a *pending kill-queue entry*. If a rollback ever
    //   reaches back far enough to swallow one belonging to a note that has already STARTED, nothing
    //   re-emits it (the step that carried it is in the past) and the voice rings until the heat death of
    //   the phrase.
    //
    // So this drives the REAL dispatcher's EQ editor — the real A+UP, at the real key-repeat cadence —
    // over a real engine with the transport actually RUNNING, and listens to what comes out.
    //
    // ⚠️ It covers the SHELL's path, which edits the project in place. Android's additionally
    // re-serializes and re-parses the whole project on every edit (`PlaybackController.notifyDataChanged`
    // → `songcorePushProject`) before rolling back; that half is NOT tested here, and it is the half the
    // report came from. Stated rather than glossed.
    {
        auto engine = std::make_unique<AudioEngine>();   // ⚠️ HEAP — see §23
        engine->setDeviceSampleRate(44100);

        songcore::SongcoreHost khost(engine.get(), 44100);
        AppState               kstate;
        kstate.project = &khost.edit_project();
        kstate.caps    = PlatformCaps::sdl(true);
        InputDispatcher kd(kstate, khost, fs_impl);

        // A LOOPING tone: without its KIL this voice rings forever, which is the only way "the note kept
        // playing" is measurable rather than a matter of a sample running out on its own.
        const fs::path tone = tree.root / "Samples" / "kiltone.wav";
        {
            std::vector<float> pcm(44100 / 2);
            for (size_t i = 0; i < pcm.size(); ++i) {
                const double t = static_cast<double>(i) / 44100.0;
                pcm[i] = static_cast<float>(0.6 * std::sin(2.0 * 3.14159265358979 * 440.0 * t));
            }
            songcore::write_wav_mono(tone.generic_string(), pcm, 44100);
        }

        songcore::Project& p = khost.edit_project();
        p = songcore::make_default_project();
        p.tempo        = 120;
        p.masterEqSlot = 0;
        p.eqPresets[0].bands[1].type = 3;    // the BELL the edit dials
        p.eqPresets[4].bands[0].type = 3;    // the INSTRUMENT's own EQ — the report said it mattered

        khost.load_sample(0, tone.generic_string());
        p.instruments[0].loopMode = "fwd";   // ⚠️ a STRING; `= 1` silently assigns a char and does nothing
        p.instruments[0].eqSlot   = 4;
        p.instruments[0].volume   = 0xC0;

        // Chain rows 1..3 are an EMPTY phrase, so there are three phrases of guaranteed silence to
        // measure in. ⚠️ Without them the phrase loops straight back onto its own step 0 and RE-TRIGGERS
        // the note — which reads as "the kill never fired" no matter what the kill did. The first version
        // of the repro harness had exactly that confound and reported the bug everywhere.
        p.tracks[0].chainRefs.assign(256, -1);
        p.tracks[0].chainRefs[0]  = 0;
        p.chains[0].phraseRefs[0] = 0;
        p.chains[0].phraseRefs[1] = 1;
        p.chains[0].phraseRefs[2] = 1;
        p.chains[0].phraseRefs[3] = 1;

        songcore::PhraseStep& n = p.phrases[0].steps[0];
        n.note = songcore::Note::C4();  n.instrument = 0;  n.volume = 0x7F;
        p.phrases[0].steps[4].fx1Type  = songcore::FX_KILL;
        p.phrases[0].steps[4].fx1Value = 0x00;

        khost.push_params();

        const int64_t fps       = songcore::frames_per_step(120, 44100);
        const int64_t killFrame = fps * 4;

        khost.play_song(0);

        // The EQ editor, opened the way a user opens it: A on the MIXER's master EQ cell.
        kstate.currentScreen     = ScreenType::MIXER;
        kstate.mixerMasterRow    = 1;
        kstate.mixerCursorColumn = 8;
        kd.on_button_a();
        ok(kstate.eq.isOpen, "LIVE EDIT: the EQ editor is open over the mixer, mid-playback");

        // BAND 2 / FREQ, which is the cell the report named. ⚠️ RIGHT changes BAND and DOWN changes
        // PARAM — the cursor is one int over a 3×4 grid, so this is row 1*4 + 1 = 5. (The "was it really
        // dialled" assertion below caught the first version of this walking onto BAND 1 instead, which is
        // exactly what such a guard is for: a test that edits the wrong cell still goes green on the
        // thing it was actually checking.)
        kd.on_dpad_right();
        kd.on_dpad_down();
        eq(kstate.eq.cursorRow, 5, "LIVE EDIT: …with the cursor on BAND 2 / FREQ");

        constexpr int      BLK = 256;
        std::vector<float> buf(BLK * 2);
        double  sumPre = 0.0, sumPost = 0.0;
        int64_t nPre = 0, nPost = 0;

        for (int64_t f = 0; f < killFrame + fps * 6; f += BLK) {
            khost.poll();

            // ⚠️ The EDIT, through the REAL dispatcher, at the REAL key-repeat rate — every ~100 ms while
            // the note is sounding and the kill is still pending. Each one calls notify_data_changed(),
            // and each one is therefore a chance to roll the pending kill off the end of the world.
            if (f > fps / 2 && f < killFrame && (f / BLK) % 17 == 0) kd.on_a_up();

            engine->processLiveBlock(buf.data(), BLK, 2, 44100.0f);

            for (int i = 0; i < BLK; ++i) {
                const double  v     = buf[static_cast<size_t>(i) * 2];
                const int64_t frame = f + i;
                if (frame < killFrame - 44100 / 50)      { sumPre  += v * v; ++nPre;  }
                else if (frame > killFrame + 44100 / 20) { sumPost += v * v; ++nPost; }
            }
        }

        const double pre  = nPre  ? std::sqrt(sumPre  / static_cast<double>(nPre))  : 0.0;
        const double post = nPost ? std::sqrt(sumPost / static_cast<double>(nPost)) : 0.0;

        ok(pre > 0.02, "LIVE EDIT: the note is actually sounding before the KIL (the test can fail)");
        ok(kstate.eq.isOpen, "LIVE EDIT: …and the editor stayed open across every repeat");
        ok(p.eqPresets[0].bands[1].freq != 0x80, "LIVE EDIT: …and the band really was dialled");

        // ⚠️ THE CLAIM. Editing an EQ while the transport runs must not cost the note its KIL.
        ok(post < pre * 0.10,
           "⚠️ LIVE EDIT: a KIL'd note is STILL KILLED after an EQ is dialled mid-playback (make "
           "notify_data_changed roll back to frame 0 and this is the check that dies)");
        std::printf("       [info] KIL'd note RMS: before %.5f → after %.5f (%.1f%% — silence is the pass)\n",
                    pre, post, pre > 0 ? 100.0 * post / pre : 0.0);
    }

    // ── 27. THE THEME EDITOR (S9) ───────────────────────────────────────────────────────────────
    //
    // The join ptinput is structurally blind to. ptinput proves `theme_adjust_color` and
    // `theme_cycle_builtin` match Kotlin GIVEN a row and a channel; nothing in it proves the cursor can
    // reach the row, that A on SAVE raises a keyboard, that the file that lands can be read back — or
    // the two claims below, which no golden of any kind could make.
    {
        songcore::SongcoreHost thost(nullptr, 44100);
        AppState               tstate;
        tstate.project = &thost.edit_project();
        tstate.caps    = PlatformCaps::sdl(true);
        InputDispatcher td(tstate, thost, fs_impl);

        // ⚠️ EVERYTHING BELOW GOES THROUGH THE PUBLIC BUTTON HANDLERS, never the dispatcher's own verbs
        // — `open_theme_editor()`, `save_theme_as()` and the rest are private, and they are private on
        // purpose. A test that reaches past the buttons proves the plumbing works when called correctly
        // and says nothing about whether any button calls it. The join is exactly what this tool is for,
        // so the only key that opens this editor here is the same one a user presses: A on SETTINGS row 9.
        const auto open_editor = [&] {
            tstate.currentScreen     = ScreenType::SETTINGS;
            tstate.settingsCursorRow = static_cast<int>(SettingsRow::THEME);
            td.on_button_a();
        };
        // The keyboard's APPLY is START (`on_start` → `qwerty_apply`). Typing the name key by key is
        // ptinput's job (384 KBD cases); here the text is set and the REAL apply path is pressed.
        const auto type_and_apply = [&](const std::string& text) {
            tstate.qwerty.text = text;
            td.on_start();
        };

        // ── Opening it: SETTINGS row 9, the row S7 drew an arrow on and left inert ───────────────
        ok(!tstate.themeEditor.isOpen, "THEME: closed to begin with");
        open_editor();
        ok(tstate.themeEditor.isOpen, "THEME: A on SETTINGS' THEME row opens the editor");
        eq(tstate.themeEditor.cursorRow, 0, "THEME: …at row 0 (the THEME row)");
        eq(tstate.themeEditor.cursorChannel, 0, "THEME: …channel 0");

        // ── The cursor: BOTH axes WRAP. The only cursor in the app that does ─────────────────────
        td.on_dpad_up();
        eq(tstate.themeEditor.cursorRow, ThemeEditorModule::MAX_ROW,
           "THEME: UP from row 0 WRAPS to row 17 (a colour list is a ring)");
        td.on_dpad_down();
        eq(tstate.themeEditor.cursorRow, 0, "THEME: …and DOWN wraps back to 0");

        td.on_dpad_left();
        eq(tstate.themeEditor.cursorChannel, 2, "THEME: LEFT from channel 0 WRAPS to B");
        td.on_dpad_right();
        eq(tstate.themeEditor.cursorChannel, 0, "THEME: …and RIGHT wraps back to R");

        // Every one of the 18 rows must be reachable by walking DOWN, and land back where it started.
        {
            bool allSeen = true;
            for (int i = 0; i < ThemeEditorModule::MAX_ROW + 1; ++i) {
                if (tstate.themeEditor.cursorRow != i) allSeen = false;
                td.on_dpad_down();
            }
            ok(allSeen, "THEME: DOWN walks all 18 rows in order…");
            eq(tstate.themeEditor.cursorRow, 0, "THEME: …and returns to row 0");
        }

        // ── The edit. A+UP/DOWN are ±1; A+RIGHT/LEFT are ±0x10 ──────────────────────────────────
        tstate.themeEditor.cursorRow     = 1;   // BACKGROUND
        tstate.themeEditor.cursorChannel = 0;   // R
        tstate.theme = theme_classic();

        td.on_a_up();
        eq(static_cast<int>((tstate.theme.background >> 16) & 0xFF), 0x0B,
           "THEME: A+UP nudges the cursor's channel by +1");
        td.on_a_right();
        eq(static_cast<int>((tstate.theme.background >> 16) & 0xFF), 0x1B,
           "THEME: A+RIGHT nudges it by +0x10");
        td.on_a_left();
        td.on_a_down();
        eq(tstate.theme.background, 0xFF0A0A0Au, "THEME: …and A+LEFT / A+DOWN put it back exactly");

        // ⚠️ On the THEME row the SAME four buttons mean something else entirely: UP/DOWN cycle the
        // built-in palette, and LEFT/RIGHT do NOTHING. Get that wrong and A+LEFT on row 0 would nudge
        // the red channel of a colour the cursor is not even on.
        tstate.themeEditor.cursorRow = 0;
        tstate.theme = theme_classic();
        td.on_a_down();
        ok(tstate.theme.name == "AMBER", "THEME: A+DOWN on the THEME row steps to the NEXT built-in");
        td.on_a_up();
        ok(tstate.theme.name == "CLASSIC", "THEME: A+UP steps back to the PREVIOUS one");
        const Theme before = tstate.theme;
        td.on_a_left();
        td.on_a_right();
        ok(tstate.theme.background == before.background && tstate.theme.name == before.name,
           "THEME: A+LEFT / A+RIGHT on the THEME row do NOTHING (no coarse step for a palette)");

        // ── THE MODAL RULE. It owns every button but START ───────────────────────────────────────
        //
        // ⚠️ This is the check S8 wishes it had had. Kotlin's `handleBUp`/`handleBDown` had NO modal
        // guard at all — and S8 documented that, ported the guard, pinned it with a control on the C++…
        // and never actually wrote the fix into the Kotlin. A control that only tests the port cannot
        // notice that the original was left broken. So: press everything, and assert nothing moved.
        open_editor();
        tstate.themeEditor.cursorRow = 5;
        {
            const int  row    = tstate.settingsCursorRow;
            const auto screen = tstate.currentScreen;
            const int  inst   = tstate.currentInstrument;

            td.on_b_up();     td.on_b_down();
            td.on_b_left();   td.on_b_right();
            td.on_r_up();     td.on_r_down();
            td.on_r_left();   td.on_r_right();
            td.on_l_a();      td.on_l_b();      td.on_l_r();
            td.on_a_a();      td.on_l_b_a();

            ok(tstate.themeEditor.isOpen, "THEME/MODAL: the editor survives every other button");
            eq(tstate.themeEditor.cursorRow, 5, "THEME/MODAL: …its cursor did not move");
            eq(tstate.settingsCursorRow, row, "THEME/MODAL: …SETTINGS' cursor underneath did not move");
            eq(tstate.currentInstrument, inst, "THEME/MODAL: …and B+UP did not page the instrument");
            ok(tstate.currentScreen == screen, "THEME/MODAL: …and R+DPAD did not navigate out from under it");
        }

        // B closes it. SELECT closes it.
        td.on_button_b();
        ok(!tstate.themeEditor.isOpen, "THEME: B closes the editor");
        open_editor();
        td.on_select();
        ok(!tstate.themeEditor.isOpen, "THEME: SELECT closes it too");

        // ── SAVE: A on the THEME row / column 1 raises the KEYBOARD — ON TOP of the editor ───────
        //
        // ⚠️ The editor stays OPEN under the keyboard (LOAD, by contrast, closes it), which is why every
        // handler tests `qwerty_open()` BEFORE `theme_open()`. If that order were reversed, a D-pad press
        // meant for the keyboard would walk the colour list behind it.
        open_editor();
        tstate.theme = theme_classic();
        tstate.theme.vizWave = 0xFF123456;          // a palette worth keeping
        tstate.themeEditor.cursorRow     = 0;
        tstate.themeEditor.cursorChannel = 1;       // SAVE
        td.on_button_a();
        ok(tstate.qwerty.isOpen, "THEME/SAVE: A on SAVE raises the QWERTY keyboard");
        ok(tstate.themeEditor.isOpen, "THEME/SAVE: …and the editor stays OPEN underneath it");
        {
            const int rowBefore = tstate.themeEditor.cursorRow;
            td.on_dpad_down();
            eq(tstate.themeEditor.cursorRow, rowBefore,
               "THEME/SAVE: …so a D-pad press moves the KEY cursor, not the colour list (THE MODAL RULE)");
        }

        // Type a name and APPLY it (START). The file must land, and it must be a real `.ptt`.
        type_and_apply("SUNSET");
        ok(!tstate.qwerty.isOpen, "THEME/SAVE: START applies the name and closes the keyboard");
        ok(tstate.themeEditor.isOpen, "THEME/SAVE: …returning to the editor");

        const std::string pttPath = fs_impl.themes_directory() + "/SUNSET.ptt";
        ok(fs_impl.file_exists(pttPath), "THEME/SAVE: <Themes>/SUNSET.ptt was written");
        ok(tstate.statusSuccess && tstate.statusMessage == "THEME SAVED",
           "THEME/SAVE: …and it REPORTS it (Kotlin discards writeFile's Boolean — S9 fixed that)");

        // ⚠️ The FILENAME is sanitized but the NAME INSIDE the file is RAW. Two names, one typed string.
        tstate.themeEditor.cursorRow     = 0;
        tstate.themeEditor.cursorChannel = 1;
        td.on_button_a();
        type_and_apply("My Theme!");
        ok(fs_impl.file_exists(fs_impl.themes_directory() + "/My_Theme_.ptt"),
           "THEME/SAVE: the FILENAME is sanitized ('My Theme!' → My_Theme_.ptt)");
        {
            Theme back;
            ok(load_theme_file(fs_impl, fs_impl.themes_directory() + "/My_Theme_.ptt", back),
               "THEME/SAVE: …the file parses back");
            ok(back.name == "My Theme!",
               "THEME/SAVE: …and the NAME INSIDE it is the RAW typed text, punctuation and all");
        }

        // ⚠️ An EMPTY name must not write a DOTFILE. This is S7's bug (`<Projects>/.ptp`, invisible to the
        // browser forever) in the one save path that was always guarded against it. Pinned so it stays so.
        tstate.themeEditor.cursorRow     = 0;
        tstate.themeEditor.cursorChannel = 1;
        td.on_button_a();
        type_and_apply("");
        ok(fs_impl.file_exists(fs_impl.themes_directory() + "/THEME.ptt"),
           "THEME/SAVE: an EMPTY name falls back to THEME.ptt — never the dotfile `.ptt`");
        ok(!fs_impl.file_exists(fs_impl.themes_directory() + "/.ptt"),
           "THEME/SAVE: …and no dotfile was written");

        // ── LOAD: A on column 2 raises the BROWSER — and CLOSES the editor ───────────────────────
        open_editor();
        tstate.themeEditor.cursorRow     = 0;
        tstate.themeEditor.cursorChannel = 2;   // LOAD
        td.on_button_a();
        ok(tstate.currentScreen == ScreenType::FILE_BROWSER, "THEME/LOAD: A on LOAD opens the browser");
        ok(!tstate.themeEditor.isOpen,
           "THEME/LOAD: …and CLOSES the editor (the browser is a SCREEN, not an overlay — leaving it "
           "open would strand a modal on a screen it was never raised from)");

        // ⚠️ AND NOW THE WHOLE ROUND TRIP, THROUGH THE BROWSER'S OWN A BUTTON — not through
        // `load_theme_file`. That is the difference between "the parser works" and "the button works":
        // it is `browser_confirm`'s LOAD_THEME arm that has to re-open the editor and put the palette
        // into the app's one live Theme, and nothing else in the tree exercises it.
        tstate.theme = theme_mono();                       // wreck the live palette first
        {
            // Park the browser's cursor on SUNSET.ptt and press A.
            FileBrowserState& fb = tstate.fileBrowser;
            int idx = -1;
            for (size_t k = 0; k < fb.items.size(); ++k)
                if (fb.items[k].displayName == "SUNSET") idx = static_cast<int>(k);
            ok(idx >= 0, "THEME/LOAD: the browser lists SUNSET.ptt (filtered to *.ptt)");
            if (idx >= 0) {
                fb.cursor = idx;
                td.on_button_a();

                ok(tstate.themeEditor.isOpen,
                   "⚠️ THEME/LOAD: A on the file RE-OPENS the editor (browser_confirm's LOAD_THEME arm)");
                ok(tstate.currentScreen == ScreenType::SETTINGS,
                   "THEME/LOAD: …and the browser is gone");
                eq(static_cast<int>(tstate.theme.vizWave), static_cast<int>(0xFF123456),
                   "⚠️ THEME/LOAD: …and the palette in the FILE is now the app's LIVE theme");
                ok(tstate.theme.name == "SUNSET", "THEME/LOAD: …name and all");
            }
        }

        // ⚠️ THE VISUALIZER DOES NOT COME FROM THE FILE. The palette belongs to the theme; the visualizer
        // belongs to the USER. Loading a friend's palette must not switch your scope to their spectrum.
        {
            Theme withViz = theme_classic();
            withViz.name           = "VIZTEST";
            withViz.visualizerType = VisualizerType::SPECTRUM_PEAKS;
            const std::string vp = fs_impl.themes_directory() + "/VIZTEST.ptt";
            ok(save_theme_file(fs_impl, vp, withViz), "THEME/VIZ: a theme with a non-default viz saves");

            Theme mine = theme_classic();
            mine.visualizerType = VisualizerType::OCTA;     // the user's choice
            ok(load_theme_file(fs_impl, vp, mine), "THEME/VIZ: …and loads");
            ok(mine.name == "VIZTEST", "THEME/VIZ: …bringing its NAME");
            ok(mine.visualizerType == VisualizerType::OCTA,
               "THEME/VIZ: …but NOT its visualizer — the user's OCTA survives the load");
        }

        // ═══════════════════════════════════════════════════════════════════════════════════════
        // ⚠️⚠️ THE TWO CHECKS THAT EARN THIS SECTION, AND NEITHER IS A GOLDEN
        // ═══════════════════════════════════════════════════════════════════════════════════════

        // ── (a) THE PIXEL. Does a colour you dial actually REACH THE SCREEN? ─────────────────────
        //
        // Every tool in the ladder compares a VALUE. ptinput compares the theme struct after a nudge;
        // this section, up to here, compares the theme struct after a gesture. NOT ONE OF THEM LOOKS AT
        // A PIXEL — so a theme that is edited correctly, saved correctly, reloaded correctly and then
        // never handed to a module would pass every check above and every case in ptinput, and the user
        // would watch the hex digits change while the screen stayed exactly as it was.
        //
        // That is not a hypothetical shape. It is S4's `push_project_params` (edited correctly, never
        // pushed — 84.4% of a render wrong) and S8's `setEqBand` (bank written, nobody told) in their
        // third disguise, and the guardrail says to ask what the existing tools structurally cannot
        // observe. They cannot observe pixels. So: RENDER, and read one back.
        {
            // ⚠️ COUNT the pixels of one unmistakable colour rather than sampling a coordinate. A
            // coordinate is a second, silent assumption about the layout — the first attempt at this
            // check sampled (300, 10), which is inside the OSCILLOSCOPE panel and is therefore painted
            // with `vizBackground`, not `background`. It reported "the theme does not reach the canvas"
            // when the theme reached the canvas perfectly well. A colour census has no geometry in it at
            // all, so it cannot be wrong about where to look.
            const auto count_of = [](const Canvas& c, Argb want) {
                int n = 0;
                for (int i = 0; i < DESIGN_W * DESIGN_H; ++i)
                    if (c.pixels()[static_cast<size_t>(i)] == want) ++n;
                return n;
            };
            constexpr Argb MAGENTA = 0xFFFF00FF;   // in no built-in palette, by construction

            Canvas        canvas;
            TrackerLayout layout;

            AppState vstate;
            vstate.project       = &thost.edit_project();
            vstate.caps          = PlatformCaps::sdl(true);
            vstate.currentScreen = ScreenType::PHRASE;   // a MODULE, not just the page fill
            vstate.theme         = theme_classic();

            layout.draw(canvas, vstate);
            eq(count_of(canvas, MAGENTA), 0, "THEME/PIXEL: no magenta on screen under CLASSIC");

            // Dial ROW CURSOR (row 3) to magenta through the REAL dispatcher, opened with the REAL
            // button. R and B saturate UP at 0xFF, G floors DOWN at 0x00 — which also exercises the
            // clamp at both ends on the way, since 0x33 ± 0x10 × n runs past both rails.
            AppState estate2;
            estate2.project           = &thost.edit_project();
            estate2.caps              = PlatformCaps::sdl(true);
            estate2.currentScreen     = ScreenType::SETTINGS;
            estate2.settingsCursorRow = static_cast<int>(SettingsRow::THEME);
            estate2.theme             = theme_classic();
            InputDispatcher ed2(estate2, thost, fs_impl);

            ed2.on_button_a();                             // A on SETTINGS row 9 → the editor opens
            ok(estate2.themeEditor.isOpen, "THEME/PIXEL: the editor opened from the real button");
            estate2.themeEditor.cursorRow = 3;             // ROW CURSOR

            estate2.themeEditor.cursorChannel = 0;         // R: 0x33 → 0xFF (clamps)
            for (int i = 0; i < 13; ++i) ed2.on_a_right();
            estate2.themeEditor.cursorChannel = 1;         // G: 0x33 → 0x00 (clamps)
            for (int i = 0; i < 4; ++i) ed2.on_a_left();
            estate2.themeEditor.cursorChannel = 2;         // B: 0x33 → 0xFF (clamps)
            for (int i = 0; i < 13; ++i) ed2.on_a_right();

            eq(static_cast<int>(estate2.theme.rowCursor), static_cast<int>(MAGENTA),
               "THEME/PIXEL: ROW CURSOR dialled to FFFF00FF through A+DPAD (and the clamp held at both rails)");

            vstate.theme = estate2.theme;                  // the app's ONE live Theme
            layout.draw(canvas, vstate);

            const int magenta = count_of(canvas, MAGENTA);
            ok(magenta > 1000,
               "⚠️ THEME/PIXEL: the dialled colour REACHES THE CANVAS — the PHRASE editor's cursor row "
               "is now magenta (stop passing the theme into TrackerLayout::draw and this is the ONLY "
               "check in the entire tree that dies)");
            std::printf("       [info] magenta pixels after dialling ROW CURSOR: %d (a 510x21 band)\n",
                        magenta);
        }

        // ── (b) THE RELAUNCH. Does the palette SURVIVE A QUIT? ───────────────────────────────────
        //
        // ⚠️⚠️ THIS IS THE S9 HEADLINE, AND IT IS A BUG THE PORT SHIPPED WITH UNTIL THIS SESSION.
        //
        // `settings_store` stored the theme as `j["theme"] = theme.name`, and rebuilt it on load with
        // `theme_by_name()`. That was CORRECT when S7 wrote it — the four built-ins were the entire
        // palette set, so a name WAS a palette and the derivation was lossless. The THEME EDITOR ends
        // that: a palette is now an arbitrary eighteen colours that exist nowhere else, and storing its
        // name threw every one of them away, silently, on every quit, with the app coming back up in
        // CLASSIC as though nothing had happened.
        //
        // NOTHING IN THE LADDER COULD SEE IT. ptinput compares the cell an edit lands in. Every check
        // above compares the state after a gesture. **Not one of them quits and relaunches the app** —
        // and that is the only place this bug lives. Same shape, again: an assumption that was true when
        // it was made, invalidated by the layer built on top of it, in a channel nothing was pointed at.
        {
            SettingsValues sv;
            sv.scalingBilinear = true;

            Theme dialled = theme_amber();
            dialled.name           = "SUNSET";
            dialled.background     = 0xFF102030;   // none of these are AMBER's, and none are CLASSIC's
            dialled.vizWave        = 0xFF123456;
            dialled.meterHigh      = 0xFFABCDEF;
            dialled.meterBorder    = 0xFF00FF00;   // the colour with NO editor row — persisted anyway
            dialled.visualizerType = VisualizerType::SPECTRUM;

            ok(save_settings(fs_impl, sv, dialled), "THEME/QUIT: settings.json written");

            // …the app exits, and comes back. `back` starts as a FRESH default, exactly as boot does.
            SettingsValues sv2;
            Theme          back = theme_classic();
            ok(load_settings(fs_impl, sv2, back), "THEME/QUIT: …and read back on the next launch");

            ok(back.name == "SUNSET", "THEME/QUIT: the theme's NAME survived");
            eq(static_cast<int>(back.background), static_cast<int>(0xFF102030),
               "⚠️ THEME/QUIT: …and so did a colour that belongs to NO built-in "
               "(store the theme by NAME and this is the check that dies)");
            eq(static_cast<int>(back.vizWave), static_cast<int>(0xFF123456),
               "THEME/QUIT: …and another");
            eq(static_cast<int>(back.meterHigh), static_cast<int>(0xFFABCDEF),
               "THEME/QUIT: …and another");
            eq(static_cast<int>(back.meterBorder), static_cast<int>(0xFF00FF00),
               "THEME/QUIT: …including meterBorder, which has no editor row but is still a field");
            ok(back.visualizerType == VisualizerType::SPECTRUM,
               "THEME/QUIT: …and the visualizer, which is stored beside the palette, not in it");

            // ⚠️ A PRE-S9 settings.json — a `theme` string and no `appTheme` object — must still load.
            // The reader falls back to the by-name path rather than booting into a blank palette.
            ok(fs_impl.write_file(fs_impl.settings_path(),
                                  "{\"theme\": \"BLUE\", \"visualizer\": 2}\n"),
               "THEME/QUIT: a pre-S9 settings.json (no appTheme object)…");
            SettingsValues sv3;
            Theme          old = theme_classic();
            ok(load_settings(fs_impl, sv3, old), "THEME/QUIT: …still loads");
            ok(old.name == "BLUE" && old.textTitle == theme_blue().textTitle,
               "THEME/QUIT: …and falls back to rebuilding the palette from its NAME");
            ok(old.visualizerType == VisualizerType::OCTA,
               "THEME/QUIT: …with the visualizer it stored");
        }

        // ── (c) THE ARM. Does the app ever CALL save_settings? ───────────────────────────────────
        //
        // ⚠️⚠️ (b) ABOVE CANNOT FAIL ON THIS, AND THAT IS THE POINT OF WRITING IT SEPARATELY.
        //
        // (b) hand-builds a Theme and calls `save_settings` ITSELF. So it proves the SERIALIZER
        // round-trips and says exactly nothing about whether any button press ever reaches it. The
        // shell writes settings.json only `if (state.settingsDirty)` (linux/main.cpp) — and the ONLY
        // thing in the entire tree that ever set that flag was `apply_edit`'s SETTINGS arm
        // (input_dispatcher.cpp). The THEME EDITOR does not go through `apply_edit`: it has no
        // CursorContext, so its four A+DPAD arms call `theme_adjust_color` / `theme_cycle_builtin` on
        // `s_.theme` DIRECTLY and armed nothing. Same for LOAD THEME's `load_theme_file`.
        //
        // So a session whose ONLY change was the palette wrote NOTHING, and the colours were gone on
        // the next launch — which is the exact bug (b) exists to prevent, one layer up. S9 fixed WHAT
        // gets written and left WHETHER it gets written unasserted.
        //
        // ⚠️ AND IT IS INTERMITTENT, which is why a device session can miss it: touch ANY SETTINGS row
        // in the same sitting and the flag is set, the exit write happens, and it carries the dialled
        // palette with it. Change ONLY the palette and it vanishes. "It works now" is not a fix.
        {
            // A settings.json holding a KNOWN palette, the way a real launch finds one.
            SettingsValues onDisk;
            Theme          stored = theme_classic();
            ok(save_settings(fs_impl, onDisk, stored), "THEME/ARM: a settings.json to start from");

            // ── the app BOOTS ────────────────────────────────────────────────────────────────────
            songcore::SongcoreHost h2(nullptr, 44100);
            AppState               s2;
            s2.project = &h2.edit_project();
            s2.caps    = PlatformCaps::sdl(true);
            ok(load_settings(fs_impl, s2.settings, s2.theme), "THEME/ARM: …read back at boot");
            InputDispatcher d2(s2, h2, fs_impl);

            // ── the user opens the editor and dials ONE colour. NOTHING ELSE. ────────────────────
            // That "nothing else" is the whole fixture: any SETTINGS row touched here would arm the
            // flag through the arm that already works, and the check would pass on the broken build.
            s2.currentScreen     = ScreenType::SETTINGS;
            s2.settingsCursorRow = static_cast<int>(SettingsRow::THEME);
            d2.on_button_a();
            ok(s2.themeEditor.isOpen, "THEME/ARM: A on SETTINGS' THEME row opens the editor");

            s2.themeEditor.cursorRow     = 1;   // BACKGROUND
            s2.themeEditor.cursorChannel = 0;   // R
            const unsigned before = s2.theme.background;
            d2.on_a_up();
            ok(s2.theme.background != before,
               "THEME/ARM: A+UP dialled the palette in memory (the fixture is live)");

            // ── the app QUITS — through THE SHELL'S OWN EXIT VERB ────────────────────────────────
            //
            // ⚠️⚠️ CALLING `save_settings` DIRECTLY HERE WOULD MAKE THIS CHECK (b) ALL OVER AGAIN — it
            // would prove the serializer round-trips and pass on a build that never writes at all. The
            // whole claim is that the path the SHELL takes on exit (linux/main.cpp) picks this edit up
            // without anything having flagged it. So the exit verb is what gets called, and nothing
            // else. Verified RED against the dirty-flag build that preceded it, where this was
            // `if (s2.settingsDirty) save_settings(...)` and the flag was never set: 1 failure of 627,
            // `got 0xFF0A0A0A, want 0xFF0B0A0A` — the dialled red still sitting in memory only.
            ok(save_settings_if_changed(fs_impl, s2.settings, s2.theme) == SettingsWrite::SAVED,
               "⚠️ THEME/ARM: the EXIT decides on its own that this session needs writing "
               "(nothing armed a flag — the editor never goes through apply_edit)");

            // ── …and COMES BACK ─────────────────────────────────────────────────────────────────
            SettingsValues sv4;
            Theme          back2 = theme_classic();
            load_settings(fs_impl, sv4, back2);
            eq(static_cast<int>(back2.background), static_cast<int>(s2.theme.background),
               "⚠️⚠️ THEME/ARM: a palette dialled in the EDITOR and nothing else SURVIVES THE QUIT "
               "(the editor mutates s_.theme directly — if nothing arms the write, this is the check "
               "that dies, while every other check in §27 stays green)");
        }
    }

    // ── 28. THE LIFECYCLE — the autosave, the kill, and the recovery (S10) ──────────────────────────
    //
    // ⚠️ **NOT ONE THING IN THIS SECTION IS A CELL, AND THAT IS WHY IT IS ALL HERE.** ptinput's whole
    // vocabulary is (context, action, resulting cell): it can say what A+UP on the RESUME row writes into
    // `autosaveResumeAuto` — and it does, in the 3,040 SETTINGS cases it has recorded from Kotlin since
    // S7, because the row's NUMBER never changed even while the shell hid it. It cannot say anything at
    // all about a FILE that appears three seconds after the last keystroke, is deleted by a save, and is
    // read back by a process that has not started yet.
    //
    // The guardrail says to ask what the existing tools structurally cannot observe. They cannot observe
    // TIME (the debounce is a deadline), they cannot observe the FILESYSTEM as a consequence (only as a
    // fixture), and — as S9 proved the hard way — **not one of them quits and relaunches the app.** All
    // three of those are the subject here.
    {
        songcore::SongcoreHost lhost(nullptr, 44100);   // no engine: a document edit never needed one
        AppState               lstate;
        lstate.project = &lhost.edit_project();
        lstate.caps    = PlatformCaps::sdl(true);
        InputDispatcher ld(lstate, lhost, fs_impl);
        ld.set_media_base_dir(fs_impl.samples_directory());

        const std::string autosavePath = fs_impl.autosave_file_path();
        const auto file_there = [&] { return fs_impl.file_exists(autosavePath); };

        // ⚠️ EVERY EDIT BELOW IS A REAL BUTTON. `mark_modified` is private and stays private: a test that
        // reaches past the buttons proves the debounce works when armed and says nothing about whether
        // anything arms it. So the document is dirtied the way a user dirties it — A+UP on a phrase cell.
        const auto edit = [&] {
            lstate.currentScreen = ScreenType::PHRASE;
            lstate.cursorRow     = 0;
            lstate.cursorColumn  = 1;   // the NOTE column
            ld.on_a_up();
        };
        const auto press_project = [&](ProjectRow row, int col) {
            lstate.currentScreen        = ScreenType::PROJECT;
            lstate.projectCursorRow     = static_cast<int>(row);
            lstate.projectCursorColumn  = col;
            ld.on_button_a();
        };

        autosave_clear(fs_impl);   // a clean slate — §27 left settings.json behind, not an autosave
        ok(!file_there(), "LIFE: no autosave to begin with");

        // ═══ (a) THE DEBOUNCE ════════════════════════════════════════════════════════════════════
        //
        // 3 s after the LAST edit, not the first. Kotlin gets this from Compose — a
        // LaunchedEffect(projectVersion) is CANCELLED and restarted whenever its key changes, so the
        // delay(3000) inside it never completes while the edits keep coming. Here it is a deadline, and
        // a deadline can be got wrong in exactly one interesting way: arm it only when idle.
        ld.set_now(0);
        edit();
        ok(lstate.project_dirty(), "LIFE/DEBOUNCE: an edit makes the document dirty");

        ld.set_now(2999);
        ok(!file_there(), "LIFE/DEBOUNCE: …nothing is written at 2999 ms");
        ld.set_now(3000);
        ok(file_there(), "LIFE/DEBOUNCE: …and the autosave lands at 3000 ms");

        // ⚠️ **THE COALESCE, and it is the assertion this whole sub-section exists for.** Edit at t=0 and
        // again at t=2000: the deadline must MOVE to 5000, not stay at 3000. Get this wrong — arm only if
        // not already armed — and a held A+UP (an edit every 100 ms, the key-repeat interval) writes ~440
        // KB of JSON to an SD card ten times a second for a value the user is still moving.
        autosave_clear(fs_impl);
        ld.set_now(10000);
        edit();                       // deadline → 13000
        ld.set_now(12000);
        edit();                       // …RE-ARMED → 15000
        ld.set_now(13000);
        ok(!file_there(),
           "⚠️ LIFE/DEBOUNCE: a second edit RE-ARMS the deadline — nothing at 13000 ms (arm-if-idle "
           "would have written here, mid-burst)");
        ld.set_now(14999);
        ok(!file_there(), "LIFE/DEBOUNCE: …still nothing at 14999 ms");
        ld.set_now(15000);
        ok(file_there(), "LIFE/DEBOUNCE: …and ONE write lands at 15000, 3 s after the LAST edit");

        // ⚠️ **A SAVE INSIDE THE WINDOW MUST NOT BE UNDONE BY IT.** The save makes the document clean and
        // deletes the file — and nothing cancels the pending deadline, because a save is not an edit and
        // does not go through mark_modified. Without run_due_autosave's re-check of project_dirty(), the
        // deadline would then fire and PUT THE FILE BACK: a crash-recovery autosave for a project that is
        // safely on disk, and a phantom RECOVER WORK? on the next launch. Kotlin carries the identical
        // second check for the identical reason.
        ld.set_now(20000);
        edit();                                        // dirty; deadline → 23000
        lhost.edit_project().name = "SAVETEST";
        ld.set_now(21000);
        press_project(ProjectRow::PROJECT, 1);         // SAVE — clears the file, aligns the versions
        ok(!lstate.project_dirty(), "LIFE/DEBOUNCE: a SAVE makes the document clean…");
        ok(!file_there(), "LIFE/DEBOUNCE: …and deletes the autosave");
        ld.set_now(23000);                             // the deadline the save did not cancel, firing
        ok(!file_there(),
           "⚠️ LIFE/DEBOUNCE: …and the deadline that was still pending does NOT put it back (drop the "
           "re-check of project_dirty() and this is the ONLY check that dies)");

        // ═══ (b) THE CLEAN POINTS — the deletions are as load-bearing as the writes ═══════════════
        //
        // "An autosave exists" has to mean "the last session ended badly and there is work in it". Every
        // clean transition therefore erases it, and a clean transition that forgets to leaves the user
        // being asked to recover a song they already saved — which is how a safety prompt teaches people
        // to dismiss it without reading.
        const auto dirty_with_autosave = [&](long long t) {
            ld.set_now(t);
            edit();
            ld.set_now(t + 3000);
            ok(file_there(), "LIFE/CLEAN: (setup) there is an autosave to erase");
        };

        dirty_with_autosave(30000);
        press_project(ProjectRow::PROJECT, 3);   // NEW — the project is dirty, so this ARMS the confirm
        ok(lstate.confirm.kind == ConfirmDialogState::Kind::NEW_PROJECT,
           "LIFE/CLEAN: NEW on a dirty project asks first");
        ld.on_button_a();                        // A = yes
        ok(!file_there(), "LIFE/CLEAN: NEW erases the autosave (nothing left to recover)");
        ok(!lstate.project_dirty(), "LIFE/CLEAN: …and the blank document is clean");

        // ⚠️ …and NEW must also disarm the PENDING deadline, or it fires 3 s later and writes the blank
        // document straight back out — an autosave whose contents are "nothing", offered as a recovery.
        ld.set_now(40000);
        ok(!file_there(), "⚠️ LIFE/CLEAN: …and no pending deadline re-creates it afterwards");

        dirty_with_autosave(50000);
        press_project(ProjectRow::PROJECT, 1);   // SAVE
        ok(!file_there(), "LIFE/CLEAN: SAVE erases it (the work is in a real file the user named)");

        // ═══ (c) THE CONFIRMED EXIT IS THE APP'S ONE *CLEAN* DEATH ═══════════════════════════════
        //
        // ⚠️ The design decision of the session, and it is not the obvious one. Now that an autosave
        // exists, "EXIT can stop asking — the work is safe either way" is exactly the wrong conclusion:
        // it would remove the only way to deliberately throw a session away, and make quitting silently
        // preserve a document the user thought they were discarding. So EXIT still ASKS (S7), and its YES
        // is the one exit that DELETES the autosave rather than writing one. Every other way out of the
        // process — SIGTERM, a flat battery, F10 — never asked, so it keeps the work.
        dirty_with_autosave(60000);
        press_project(ProjectRow::EXIT, 1);
        ok(lstate.confirm.kind == ConfirmDialogState::Kind::EXIT,
           "LIFE/EXIT: EXIT on a dirty project still ASKS (the autosave did not make the question moot)");
        ok(!lstate.shouldQuit, "LIFE/EXIT: …and has not quit yet");

        // B = no. The dialog closes, and NOTHING ELSE HAPPENS — the autosave is still there, because the
        // user is still working. ⚠️ Five of the six confirms have a NO that is a pure close; this checks
        // that EXIT's still is, now that RECOVER's is not.
        ld.on_button_b();
        ok(!lstate.confirm.is_open(), "LIFE/EXIT: B closes it");
        ok(!lstate.shouldQuit, "LIFE/EXIT: …without quitting");
        ok(file_there(),
           "⚠️ LIFE/EXIT: …and B on EXIT is a PURE CANCEL — it must not touch the autosave (only "
           "RECOVER's NO does)");

        press_project(ProjectRow::EXIT, 1);
        ld.on_button_a();                        // A = yes, quit
        ok(lstate.shouldQuit, "LIFE/EXIT: A quits");
        ok(!file_there(),
           "⚠️ LIFE/EXIT: …and a CONFIRMED exit erases the autosave — the user was shown their unsaved "
           "work and chose to leave it, and the next launch must not offer it back");
        lstate.shouldQuit = false;

        // ═══ (d) THE KILL — what the frame loop's exit path does ═════════════════════════════════
        //
        // ⚠️ This is the SIGTERM path, and it runs HERE rather than in a signal handler for a reason
        // worth repeating: writing a .ptp is ~440 KB of JSON through malloc, <filesystem> and ofstream,
        // and not one of those is async-signal-safe. A SIGTERM arriving while the main thread happens to
        // be inside malloc would deadlock the handler on the heap lock, the app would hang instead of
        // saving, and the launcher's SIGKILL would land a second later — the autosave failing in exactly
        // the case it exists for. SDL already solved it: its SIGINT/SIGTERM handler only sets a flag, and
        // the event pump turns it into SDL_QUIT. So a kill arrives at the shell as an ordinary event, the
        // loop ends, and `flush_autosave()` runs on the main thread. That call is what this drives.
        autosave_clear(fs_impl);
        ld.set_now(70000);
        edit();
        lhost.edit_project().name = "KILLED";
        ld.flush_autosave();                     // ← the launcher took the process away
        ok(file_there(), "⚠️ LIFE/KILL: the flush writes the autosave BEFORE the 3 s deadline was due");

        // ⚠️ And it is a NO-OP on a clean document, which is what keeps the file's meaning intact. A
        // flush that always wrote would leave an autosave after every single quit, and the next launch
        // would ask RECOVER WORK? every single time — about nothing.
        {
            AppState        cstate;
            cstate.project = &lhost.edit_project();
            cstate.caps    = PlatformCaps::sdl(true);
            InputDispatcher cd(cstate, lhost, fs_impl);
            autosave_clear(fs_impl);
            cd.flush_autosave();
            ok(!file_there(),
               "⚠️ LIFE/KILL: …but a CLEAN document flushes NOTHING (or 'an autosave exists' would stop "
               "meaning 'the last session ended badly')");
        }

        // The file that landed is a real .ptp with the edit in it — not merely a file that exists.
        {
            songcore::SongcoreHost back(nullptr, 44100);
            ld.set_now(80000);
            edit();
            lhost.edit_project().name = "ROUNDTRIP";
            ld.flush_autosave();
            ok(back.load_project_file(autosavePath, fs_impl.samples_directory()),
               "LIFE/KILL: the flushed autosave parses back as a .ptp");
            ok(back.project().name == "ROUNDTRIP",
               "⚠️ LIFE/KILL: …and it is the LIVE document that was written, edits and all");
        }

        // ═══ (e) THE RELAUNCH — boot recovery, and the four ways it can go ═══════════════════════
        //
        // ⚠️ The only section in the whole tool that models a SECOND PROCESS. Everything above drives one
        // app; this one has to end it and start another, because that is the only place these bugs live.
        const auto boot = [&](bool resumeAuto) {
            auto st = std::make_unique<AppState>();
            st->project              = &lhost.edit_project();
            st->caps                 = PlatformCaps::sdl(true);
            st->settings.autosaveResumeAuto = resumeAuto;
            return st;
        };

        // Write an autosave the way a killed session would, then relaunch onto it.
        const auto crash_with = [&](const std::string& name) {
            lstate.currentScreen = ScreenType::PHRASE;
            lhost.edit_project().name = name;
            ld.set_now(90000);
            edit();
            ld.flush_autosave();
            ok(file_there(), "LIFE/BOOT: (setup) a killed session left an autosave");
        };

        // — no autosave: boot_recovery says nothing at all —
        {
            autosave_clear(fs_impl);
            auto            st = boot(/*resumeAuto=*/false);
            InputDispatcher bd(*st, lhost, fs_impl);
            bd.set_media_base_dir(fs_impl.samples_directory());
            ok(bd.boot_recovery() == InputDispatcher::BootRecovery::NONE,
               "LIFE/BOOT: a clean last session finds nothing (the overwhelmingly common case)");
            ok(!st->confirm.is_open(), "LIFE/BOOT: …and raises no prompt");
        }

        // — ASK, and A = recover —
        {
            crash_with("CRASHED");
            lhost.new_project();                       // …the process died; a fresh one boots blank
            auto            st = boot(/*resumeAuto=*/false);
            InputDispatcher bd(*st, lhost, fs_impl);
            bd.set_media_base_dir(fs_impl.samples_directory());

            ok(bd.boot_recovery() == InputDispatcher::BootRecovery::ASKED, "LIFE/BOOT[ASK]: …is ASKED about");
            ok(st->confirm.kind == ConfirmDialogState::Kind::RECOVER,
               "LIFE/BOOT[ASK]: an autosave raises RECOVER WORK? — the one dialog nobody's button opened");

            bd.on_button_a();
            ok(!st->confirm.is_open(), "LIFE/BOOT[ASK]: A closes it");
            ok(lhost.project().name == "CRASHED",
               "⚠️ LIFE/BOOT[ASK]: …and the killed session's document is BACK");
            ok(st->project_dirty(),
               "⚠️ LIFE/BOOT[ASK]: …and it is DIRTY — recovered work is not STORED work, and marking it "
               "clean would tell the user the song is safe while its only copy is the crash file");
            ok(file_there(),
               "⚠️ LIFE/BOOT[ASK]: …and the autosave STAYS: the recovered document is still the only copy, "
               "and deleting it now is the one deletion in the app that can destroy real work");
        }

        // — ASK, and B = discard. The ONE confirm whose NO is an ACTION —
        {
            crash_with("DISCARD_ME");
            lhost.new_project();
            auto            st = boot(/*resumeAuto=*/false);
            InputDispatcher bd(*st, lhost, fs_impl);
            bd.set_media_base_dir(fs_impl.samples_directory());

            (void)bd.boot_recovery();
            bd.on_button_b();
            ok(!st->confirm.is_open(), "LIFE/BOOT[ASK]: B closes it");
            ok(!file_there(),
               "⚠️ LIFE/BOOT[ASK]: …and B DELETES the autosave. Every other confirm's NO is a pure close; "
               "leave the file here and the same prompt returns on every launch, forever");
            ok(lhost.project().name != "DISCARD_ME",
               "LIFE/BOOT[ASK]: …and the document was NOT loaded");
        }

        // — AUTO: no prompt at all —
        {
            crash_with("SILENT");
            lhost.new_project();
            auto            st = boot(/*resumeAuto=*/true);
            InputDispatcher bd(*st, lhost, fs_impl);
            bd.set_media_base_dir(fs_impl.samples_directory());

            ok(bd.boot_recovery() == InputDispatcher::BootRecovery::RESTORED,
               "LIFE/BOOT[AUTO]: …is RESTORED outright");
            ok(!st->confirm.is_open(),
               "⚠️ LIFE/BOOT[AUTO]: NO prompt — right on a handheld whose launcher kills the port every "
               "time the user opens a menu, where asking on every return is noise, not a safeguard");
            ok(lhost.project().name == "SILENT", "LIFE/BOOT[AUTO]: …the document came back anyway");
            ok(st->project_dirty(), "LIFE/BOOT[AUTO]: …and it is dirty, exactly as the ASK path leaves it");
        }

        // ⚠️ — A CORRUPT AUTOSAVE IS DROPPED, NOT LOOPED ON. Under BOTH resume modes —
        //
        // Kotlin guards its AUTO arm for precisely this ("A corrupt autosave is dropped so AUTO can't loop
        // on it") and **its ASK arm does not** — a recoverFromAutosave() that fails there leaves the file,
        // so the prompt comes back on every launch and can NEVER succeed. S10 found that asymmetry by
        // porting it, and fixed it on both platforms. Here, both arms drop it.
        for (const bool autoResume : {false, true}) {
            const char* who = autoResume ? "AUTO" : "ASK";
            ok(fs_impl.write_file(autosavePath, "{ this is not json"),
               std::string("LIFE/BOOT[") + who + "]: (setup) a truncated autosave — what a kill mid-write leaves");

            lhost.new_project();
            auto            st = boot(autoResume);
            InputDispatcher bd(*st, lhost, fs_impl);
            bd.set_media_base_dir(fs_impl.samples_directory());

            const InputDispatcher::BootRecovery outcome = bd.boot_recovery();
            if (!autoResume) {
                ok(outcome == InputDispatcher::BootRecovery::ASKED,
                   "LIFE/BOOT[ASK]: a corrupt autosave still raises the prompt (nothing has read it yet)");
                ok(st->confirm.kind == ConfirmDialogState::Kind::RECOVER, "LIFE/BOOT[ASK]: …RECOVER WORK?");
                bd.on_button_a();   // …and the recovery fails
                ok(!st->statusSuccess && st->statusMessage == "RECOVER FAILED",
                   "LIFE/BOOT[ASK]: …A on it fails, and SAYS so");
            } else {
                // ⚠️ DROPPED, not RESTORED — and the distinction is the reason this returns an enum
                // rather than a bool. With a bool the shell printed "restored silently" for a file it had
                // just thrown away unread, which is a boot diagnostic that lies about the one thing you
                // are reading it to find out.
                ok(outcome == InputDispatcher::BootRecovery::DROPPED,
                   "⚠️ LIFE/BOOT[AUTO]: a corrupt autosave reports DROPPED, never RESTORED");
            }
            ok(!file_there(),
               std::string("⚠️ LIFE/BOOT[") + who +
                   "]: …and the unreadable file is DROPPED, not offered again — a prompt that can never "
                   "succeed would otherwise return on every single launch");
        }

        // ═══ (f) THE ROUND TRIP — does RESUME itself survive a quit? ═════════════════════════════
        //
        // ⚠️⚠️ **THIS IS S9's HEADLINE BUG'S EXACT SHAPE, ONE ROW LATER, AND IT IS WHY THE CHECK EXISTS
        // BEFORE THE BUG DOES.** S9 shipped a `settings_store` that persisted the theme by NAME — correct
        // when written, silently lossy the moment the theme editor was built on top of it — and NOTHING IN
        // THE LADDER COULD SEE IT, because no tool quits and relaunches the app. S10 flips
        // `PlatformCaps::sdl().autosave` on, which gives the shell a settings row it did not have; the
        // very same session therefore has to add the key to settings.json, or RESUME resets to ASK on
        // every launch and not one of the 22,929 ptinput cases notices.
        //
        // The lesson from S9 was to point a check at the channel nothing is pointed at. This is that check,
        // written the same day as the feature rather than one session later.
        {
            SettingsValues sv;
            sv.autosaveResumeAuto = true;      // the user picks AUTO…
            sv.notePreviewEnabled = false;
            Theme th = theme_classic();
            ok(save_settings(fs_impl, sv, th), "LIFE/RESUME: settings.json written with RESUME=AUTO");

            SettingsValues back;                // …the app exits, and comes back to factory defaults
            Theme          bth = theme_classic();
            ok(!back.autosaveResumeAuto, "LIFE/RESUME: (a fresh SettingsValues defaults to ASK)");
            ok(load_settings(fs_impl, back, bth), "LIFE/RESUME: …and reads settings.json on the next launch");
            ok(back.autosaveResumeAuto,
               "⚠️ LIFE/RESUME: RESUME=AUTO SURVIVED THE QUIT — drop the key from settings_store and this "
               "is the only check in the entire tree that dies, exactly as S9's theme did");
            ok(!back.notePreviewEnabled, "LIFE/RESUME: …and it did not clobber the row beside it");

            // An OLDER settings.json — written before S10, with no such key — must still load, and must
            // default to ASK. A prompt an upgrading user can say no to is the safe answer; a silent restore
            // they never asked for is not.
            ok(fs_impl.write_file(fs_impl.settings_path(), "{\"notePreview\": true}\n"),
               "LIFE/RESUME: a pre-S10 settings.json (no autosaveResumeAuto key)…");
            SettingsValues old;
            old.autosaveResumeAuto = true;   // …poisoned, so a missing key CANNOT pass by accident
            Theme oth = theme_classic();
            ok(load_settings(fs_impl, old, oth), "LIFE/RESUME: …still loads");
            ok(old.autosaveResumeAuto,
               "LIFE/RESUME: …leaving the value it was handed alone (a missing key is not a false)");
        }

        autosave_clear(fs_impl);   // leave the temp tree as we found it
    }

    // ── 29. THE TRANSPORT — what the ENGINE is still holding after STOP ──────────────────────────
    //
    // ⚠️ ELEVEN TOOLS, AND NOT ONE OF THEM EVER ASKED WHAT STOP LEAVES BEHIND. Every one of them is
    // blind to it by construction, and the blindness has the same shape as S4's `push_params` bug:
    //   • ptplay and the seven goldens read the EVENT BUS — and the bus stops correctly. `seq_.stop()`
    //     does call `router_.t_stop()`, so the trace ends exactly where it should. The trace is not the
    //     audio, and the queue the audio drains from is BELOW the router.
    //   • ptrender renders — and `prepare_render` calls `stopAll()` + `clearScheduledNotes()` itself
    //     (render.h:89). So the render path was already correct and could never expose it.
    //   • ptdispatch (this file) ran §23/§24 through a real engine — but only ever RENDERED. Nothing
    //     had driven `processLiveBlock` and then pressed stop.
    // Not an event, not a note, and the render path was already right. The one channel nothing pointed at.
    //
    // What it hid (Phase 4, reported from the device, 2026-07-17): `SongcoreHost::stop()` stops the
    // SCHEDULER and never touches the ENGINE. On Android that is invisible, because the host is only
    // ever reached through `PlaybackController.stop()`, which does the engine-side cleanup itself and
    // whose own comment says it is "shared and runs for both engines" (PlaybackController.kt:415-430).
    // The SDL shell calls `host_.stop()` DIRECTLY — there is no PlaybackController under it — so the
    // BUFFER_PHRASES=2 lookahead (≈4 s at the default tempo) went on playing after the button, and the
    // next START, seeing `isPlaying_ == false`, scheduled a SECOND stream on top of the stale one.
    //
    // ⚠️ Both checks below pass by DOING NOTHING (silence, and "not louder"), which S10 named as the
    // trap: a test whose pass is nothing happening cannot tell a fix from a misfire. Two things answer
    // that. Each has a POSITIVE control inside it — the transport is proven AUDIBLE before it is asked
    // to go quiet. And the real control is that both were RUN AGAINST THE BROKEN BUILD FIRST and went
    // red, naming it: the bug is its own control, and it was free.
    {
        // ⚠️ THE FIXTURE MUST BE SHORTER THAN ONE STEP (0.125 s at 120 BPM) or every note runs into the
        // next, the voice never goes idle, and §29b's energy stops being a count of what FIRED. It is
        // synthesized rather than borrowed, as §24's is: ptdispatch is the one tool with no /testdata.
        const int      rate     = 44100;
        const fs::path tonePath = tree.root / "Samples" / "stoptone.wav";
        {
            std::vector<float> tone(static_cast<size_t>(rate / 12));   // ≈83 ms
            for (size_t i = 0; i < tone.size(); ++i) {
                const double t   = static_cast<double>(i) / rate;
                const double env = std::exp(-60.0 * t);
                tone[i] = static_cast<float>(0.7 * env * std::sin(2.0 * 3.14159265358979 * 1000.0 * t));
            }
            ok(songcore::write_wav_mono(tonePath.generic_string(), tone, rate),
               "STOP: the fixture tone is written");
        }

        struct Rig {
            std::unique_ptr<AudioEngine>            engine;
            std::unique_ptr<songcore::SongcoreHost> host;
            AppState                                state;
            std::unique_ptr<InputDispatcher>        dispatch;
        };

        // A whole rig per case: playback is a pure function of the project, and two cases sharing an
        // engine would let the first one's state decide the second one's verdict (S6b's argument for
        // rendering a DIFFERENT project between the two determinism renders).
        const auto make_rig = [&]() {
            auto r    = std::make_unique<Rig>();
            r->engine = std::make_unique<AudioEngine>();   // ⚠️ HEAP — see §23
            r->engine->setDeviceSampleRate(rate);
            r->host = std::make_unique<songcore::SongcoreHost>(r->engine.get(), rate);

            songcore::Project& p = r->host->edit_project();
            p      = songcore::make_default_project();
            p.name = "STOP TEST";
            for (int step = 0; step < 16; ++step) {   // a note on EVERY step — plenty still queued at the stop
                songcore::PhraseStep& s = p.phrases[0].steps[static_cast<size_t>(step)];
                s.note       = songcore::Note::C4();
                s.instrument = 0;
            }
            ok(r->host->load_sample(0, tonePath.generic_string()),
               "STOP: …and loads into instrument 0, so the phrase is AUDIBLE rather than vacuously silent");
            r->host->push_params();

            r->state.project       = &p;
            r->state.caps          = PlatformCaps::sdl(true);
            r->state.currentScreen = ScreenType::PHRASE;   // ⚠️ where START IS the transport (§S4)
            r->state.currentPhrase = 0;
            r->dispatch = std::make_unique<InputDispatcher>(r->state, *r->host, fs_impl);
            return r;
        };

        // The shell's frame loop in miniature: the audio callback drains the queues, the poll refills
        // them. It calls `processLiveBlock` — the SDL callback's own entry point (sdl-audio-engine.cpp:33)
        // — and NOT renderOffline, which is the one path that already cleaned up after itself. It polls
        // unconditionally, as the shell does (`poll()` no-ops while stopped).
        constexpr int      BLOCK = 512;                       // ≈11.6 ms
        std::vector<float> buf(BLOCK * 2);
        struct Pumped { double peak = 0.0; double energy = 0.0; };
        const auto pump = [&](Rig& r, int blocks) {
            Pumped out;
            for (int b = 0; b < blocks; ++b) {
                r.host->poll();
                r.engine->processLiveBlock(buf.data(), BLOCK, 2, static_cast<float>(rate));
                for (int i = 0; i < BLOCK * 2; ++i) {
                    const double v = static_cast<double>(buf[i]);
                    out.peak = std::max(out.peak, std::abs(v));
                    out.energy += v * v;
                }
            }
            return out;
        };

        // ── 29a. STOP SILENCES THE ENGINE ────────────────────────────────────────────────────────
        {
            auto r = make_rig();

            r->dispatch->on_start();   // the real gesture, not host->stop() — the button is what regressed
            ok(r->host->is_playing(), "STOP: START on PHRASE starts the transport");

            const Pumped playing = pump(*r, 43);   // ≈0.5 s
            ok(playing.peak > 0.01,
               "⚠️ STOP(control): …and it is genuinely AUDIBLE. Without this the silence below would pass "
               "on a rig that never made a sound at all");

            r->dispatch->on_start();   // a second press = STOP
            ok(!r->host->is_playing(), "STOP: a second START stops the transport");

            pump(*r, 2);   // let the block the stop landed in finish
            eq(r->engine->getActiveVoiceCount(), 0,
               "⚠️ STOP: no voice is left ringing — this is stopAll(), and Voice::stop() is instant "
               "(isActive = false), so there is no fade to wait out");

            // ≈4.5 s — deliberately LONGER than the BUFFER_PHRASES=2 lookahead (2 phrases × 16 steps ×
            // 5512 frames ≈ 4.0 s), so a queue that was never cleared has time to play itself out and
            // be caught, rather than still being pending when the window closes.
            const Pumped after = pump(*r, 388);
            ok(after.peak < 0.001,
               "⚠️ STOP: THE ENGINE IS SILENT AFTER STOP — clearScheduledNotes(). Without it the 2-phrase "
               "lookahead keeps playing for ~4 s after the button, which is the device's "
               "'START won't stop it' verbatim");
        }

        // ── 29b. …AND A RESTART PLAYS ONE SCHEDULE, NOT TWO ──────────────────────────────────────
        //
        // The second face of the same bug, and worth pinning directly rather than leaving as a corollary
        // of 29a: the device report guessed "voice stealing or something", and this is the check that
        // answers it. Layering is a STALE QUEUE. Nothing is wrong with the voice allocator.
        {
            auto clean = make_rig();
            clean->dispatch->on_start();
            const double base = pump(*clean, 86).energy;   // ≈1 s of exactly ONE schedule
            ok(base > 0.0, "STOP/RESTART(control): the reference run made sound");

            auto r = make_rig();
            r->dispatch->on_start();
            // ⚠️ THE CLOCK MUST ADVANCE BEFORE THE STOP, or this test passes by construction: playPhrase
            // latches `playbackStartFrame_ = getCurrentFrame()`, so a stop-and-restart with no audio in
            // between re-schedules onto the SAME frames and the stale copy hides inside the new one. A
            // quarter second offsets them, which is also what a human hand does.
            pump(*r, 22);
            r->dispatch->on_start();   // stop…
            r->dispatch->on_start();   // …and start again
            const double again = pump(*r, 86).energy;

            // With the queue cleared the restart is bit-for-bit the reference run's work; the tolerance is
            // for DSP state carried across the 0.25 s (limiter/OTT envelopes), not for a second schedule —
            // that one lands at ≈2×, nowhere near the line.
            ok(again < base * 1.3,
               "⚠️ STOP/RESTART: a restart plays ONE schedule, not two. A stale queue is what layers "
               "playback — NOT voice stealing");
        }
    }

    // ── §30 — an Android project's dead sample paths re-root onto THIS install's app root ─────────────
    //
    // The bug the user hit bringing phone projects up on the handheld: a .ptp authored on Android stores
    // sample paths ABSOLUTE under /storage/emulated/0/Documents/PocketTracker/…, which on a handheld point
    // nowhere — so every instrument loads silent. resolve_media_path now re-roots the app-root-relative
    // tail onto SongcoreHost::set_app_root(). This drives the REAL host.load_media — the exact call the
    // browser's LOAD makes — and reads its loaded/failed count.
    //
    // ⚠️ THE CONTROL IS THE OLD BEHAVIOUR ITSELF (b): a host never told its app root runs the code that
    // shipped before this change, and the SAME path fails. So the control fires red on the pre-fix logic —
    // this is not a check whose "pass" is nothing happening.
    {
        namespace sc = songcore;
        const std::string root = tree.root.generic_string();

        auto write_tone = [](const fs::path& at) {
            fs::create_directories(at.parent_path());
            std::vector<float> pcm(4410);   // 0.1 s — enough for the decoder to accept it
            for (size_t i = 0; i < pcm.size(); ++i)
                pcm[i] = static_cast<float>(0.5 * std::sin(2.0 * 3.14159265358979 * 440.0 *
                                                           (static_cast<double>(i) / 44100.0)));
            sc::write_wav_mono(at.generic_string(), pcm, 44100);
        };
        auto proj_with = [](const std::string& samplePath) {
            sc::Project p = sc::make_default_project();
            p.instruments[0].id             = 0;
            p.instruments[0].instrumentType = sc::InstrumentType::SAMPLER;
            p.instruments[0].sampleFilePath = samplePath;
            return p;
        };

        write_tone(tree.root / "Samples" / "reloc.wav");                       // where THIS install keeps it
        const std::string androidPath =
            "/storage/emulated/0/Documents/PocketTracker/Samples/reloc.wav";   // the phone's dead path

        write_tone(tree.root / "decoys" / "Samples" / "keep.wav");   // (c)'s valid path, inside the tree
        const std::string base    = fs_impl.samples_directory();
        const std::string decoy   = (tree.root / "decoys" / "Samples" / "keep.wav").generic_string();

        auto engine = std::make_unique<AudioEngine>();   // ⚠️ HEAP — see §23; ONE engine for all four
        engine->setDeviceSampleRate(44100);

        // (a) THE FIX, through the REAL host wiring: set_app_root → load_media re-roots and loads.
        {
            sc::SongcoreHost host(engine.get(), 44100);
            host.set_app_root(root);
            host.edit_project() = proj_with(androidPath);
            auto r = host.load_media(base);
            ok(r.loaded == 1 && r.failed == 0,
               "RELOC: an Android absolute sample path re-roots onto this app root and loads");
        }

        // (b) THE CONTROL — the pre-fix behaviour IS load_project_media with an EMPTY app root; on the same
        //     input it fails. Called directly on the same engine (it clears samples first), so nothing but
        //     app_root differs from (a).
        {
            sc::Project p = proj_with(androidPath);
            sc::Routing rt;
            auto r = sc::load_project_media(*engine, p, base, /*app_root=*/"", rt);
            ok(r.loaded == 0 && r.failed == 1,
               "RELOC control: with no app root the SAME path fails — the bug, reproduced on demand");
        }

        // (c) A VALID absolute path is used as-authored, NOT rewritten. The decoy contains "/Samples/" (so
        //     the tail extractor WOULD fire) but the file exists THERE, and its re-root target
        //     <root>/Samples/keep.wav does not — so loaded==1 proves the existence check let the real path
        //     win. A re-root-always bug fails here.
        {
            sc::Project p = proj_with(decoy);
            sc::Routing rt;
            auto r = sc::load_project_media(*engine, p, base, root, rt);
            ok(r.loaded == 1, "RELOC: an existing absolute path is used as-authored, never re-rooted");
        }

        // (d) A path under NO app sub-tree cannot be recovered — the user's own stated limitation, pinned.
        {
            sc::Project p = proj_with("/nowhere/at/all/ghost.wav");
            sc::Routing rt;
            auto r = sc::load_project_media(*engine, p, base, root, rt);
            ok(r.loaded == 0 && r.failed == 1,
               "RELOC: a path under no app sub-tree is left as-authored (fails, as the user accepted)");
        }

        // (e) CASE DRIFT — the P4f-device reality, and it ONLY reproduces on a case-SENSITIVE filesystem.
        //     Android storage is case-INsensitive, the SD card is not, so a project's "Samples/Breaks/…" is
        //     dead against the card's real "Samples/breaks/…". On a case-INsensitive host (this Windows dev
        //     box, macOS) "Breaks"=="breaks" so there is nothing to resolve — skip rather than assert what
        //     cannot be true here. The CI's Linux runners and the device DO exercise the walk.
        bool caseSensitiveFs = false;
        {
            const fs::path probe = tree.root / "casetest.tmp";
            { std::ofstream(probe.string()) << "x"; }
            caseSensitiveFs = !fs::exists(tree.root / "CASETEST.TMP");
            std::error_code pec; fs::remove(probe, pec);
        }
        if (caseSensitiveFs) {
            write_tone(tree.root / "Samples" / "breaks" / "amen.wav");   // on disk: LOWERCASE folder
            const std::string capital = (tree.root / "Samples" / "Breaks" / "amen.wav").generic_string();
            ok(!sc::path_exists(capital),
               "RELOC/case control: the exact capitalised path really is dead (case-sensitive host)");
            sc::Project p = proj_with("/storage/emulated/0/Documents/PocketTracker/Samples/Breaks/amen.wav");
            sc::Routing rt;
            auto r = sc::load_project_media(*engine, p, base, root, rt);
            ok(r.loaded == 1 && r.failed == 0,
               "RELOC/case: a wrong-case Android folder loads via case-insensitive resolution");
        }
    }

    // ══ P4h ══ THE PARITY-AUDIT PINS — the joins the audit found no assertion on ═════════════════
    //
    // docs/internal/port-parity-audit.md (P4g). Each block below ran RED against the pre-fix build
    // before its fix landed — two of the audit's three reported symptoms lived precisely where no
    // assertion existed, so the pins are the point, not the ceremony.

    // ── 30. R+LEFT / R+RIGHT carry the lastEdited memory across the switch — and ONLY they do ────
    //
    // AppInputDispatcher.syncLastEditedOnScreenSwitch (:2760), called from handleRLeft/handleRRight
    // alone, and only when the screen actually changes. TWO halves: CAPTURE the ref under the
    // departing screen's cursor into lastEdited*, APPLY lastEdited* to the arriving screen's
    // current*. Without it every horizontal screen change lands on slot 00 — "every screen starts
    // from 00", the audit's second reported symptom.
    {
        state.caps                    = PlatformCaps::sdl(true);
        state.settings.cursorRemember = false;
        state.confirm.close();
        state.eq          = EqEditorState{};
        state.themeEditor = ThemeEditorState{};
        state.qwerty      = QwertyKeyboardState{};

        songcore::Project& p = host.edit_project();
        p = songcore::make_default_project();

        // SONG → CHAIN: the chain ref under the cursor is captured AND applied in one gesture.
        // Column 3 is tracks[2] — on SONG the cursor column IS the track, 1-based.
        p.tracks[2].chainRefs.assign(256, -1);
        p.tracks[2].chainRefs[7] = 0x04;
        state.currentScreen  = ScreenType::SONG;
        state.previousColumn = 0;
        state.cursorRow = 7; state.cursorColumn = 3;
        state.currentChain = 0; state.lastEditedChain = 0;
        dispatch.on_r_right();
        ok(state.currentScreen == ScreenType::CHAIN, "SYNC: SONG x R+RIGHT lands on CHAIN");
        eq(state.currentChain, 0x04, "SYNC: ...deep-linked to the chain under the SONG cursor");
        eq(state.lastEditedChain, 0x04, "SYNC: ...and lastEditedChain remembers it");

        // CHAIN → PHRASE: same shape, one screen over.
        p.chains[0x04].phraseRefs[2] = 0x09;
        state.cursorRow = 2; state.cursorColumn = 1;
        state.currentPhrase = 0; state.lastEditedPhrase = 0;
        dispatch.on_r_right();
        ok(state.currentScreen == ScreenType::PHRASE, "SYNC: CHAIN x R+RIGHT lands on PHRASE");
        eq(state.currentPhrase, 0x09, "SYNC: ...deep-linked to the phrase under the CHAIN cursor");

        // PHRASE → INSTRUMENT: the capture asks the CELL, through the module's own cursor_context —
        // a noted step under the NOTE column is non-empty, so its instrument is captured.
        p.phrases[0x09].steps[5].note       = songcore::Note::C4();
        p.phrases[0x09].steps[5].instrument = 0x05;
        state.cursorRow = 5; state.cursorColumn = 1;
        state.currentInstrument = 0; state.lastEditedInstrument = 0;
        dispatch.on_r_right();
        ok(state.currentScreen == ScreenType::INSTRUMENT, "SYNC: PHRASE x R+RIGHT lands on INSTRUMENT");
        eq(state.currentInstrument, 0x05, "SYNC: ...on the instrument of the step under the cursor");

        // …and an EMPTY cell captures NOTHING — the arriving screen shows the lastEdited memory,
        // not a scavenged value. (Row 6 is untouched; the memory is parked at 0x22.)
        dispatch.on_r_left();                       // back to PHRASE (apply: currentPhrase = 0x09 again)
        eq(state.currentPhrase, 0x09, "SYNC: R+LEFT back into PHRASE re-applies the phrase memory");
        state.cursorRow = 6; state.cursorColumn = 1;
        state.lastEditedInstrument = 0x22;
        dispatch.on_r_right();
        eq(state.currentInstrument, 0x22, "SYNC: an empty PHRASE cell captures nothing - the memory wins");

        // ⚠️ THE CLAMP TRAP: Kotlin's apply runs through the currentInstrument SETTER
        // (TrackerController.kt:167-172), which coerces to the pool and mirrors the clamped value
        // back. A noted step whose instrument is -1 lands on 00 — never on "slot -1".
        dispatch.on_r_left();
        p.phrases[0x09].steps[8].note       = songcore::Note::C4();
        p.phrases[0x09].steps[8].instrument = -1;
        state.cursorRow = 8; state.cursorColumn = 1;
        dispatch.on_r_right();
        eq(state.currentInstrument, 0, "SYNC: a noted step with instrument -1 clamps to 00 on apply");
        eq(state.lastEditedInstrument, 0, "SYNC: ...and the setter's mirror writes the clamp back");

        // ⚠️ THE SCOPE TRAP, pinned from both sides: R+UP/R+DOWN do NOT sync (Kotlin's
        // handleRUp/handleRDown do only cursor save/restore + selection exit — :2695/:2716).
        // "Fixing" the vertical moves too would diverge the other way.
        state.currentScreen  = ScreenType::CHAIN;
        state.previousColumn = 1;
        p.chains[0x04].phraseRefs[3] = 0x0B;
        state.cursorRow = 3; state.cursorColumn = 1;
        state.currentChain = 0x04; state.lastEditedChain = 0x04;
        state.currentPhrase = 0x09; state.lastEditedPhrase = 0x02;   // memory != current, on purpose
        dispatch.on_r_down();                       // CHAIN → MIXER
        ok(state.currentScreen == ScreenType::MIXER, "SYNC-NEG: R+DOWN reaches MIXER");
        eq(state.lastEditedPhrase, 0x02, "SYNC-NEG: R+DOWN captured NOTHING from the chain cursor");
        dispatch.on_r_up();                         // MIXER → CHAIN
        ok(state.currentScreen == ScreenType::CHAIN, "SYNC-NEG: R+UP returns to CHAIN");
        eq(state.currentChain, 0x04, "SYNC-NEG: ...and applied NOTHING - currentChain is untouched");

        // The SIZE guard is load-bearing, not defensive: a track's chainRefs vector may be SHORTER
        // than the 256-row screen (the model's default is EMPTY, as Kotlin's mutableListOf() is).
        // Leaving SONG over such a row must neither crash nor capture.
        state.currentScreen  = ScreenType::SONG;
        state.previousColumn = 0;
        state.cursorRow = 5; state.cursorColumn = 1;   // tracks[0].chainRefs was never assigned
        state.lastEditedChain = 0x02;
        dispatch.on_r_right();
        eq(state.currentChain, 0x02, "SYNC: an out-of-range SONG row captures nothing (size guard)");
    }

    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    std::printf("%s\n", failures == 0 ? "ALL GREEN" : "RED");
    return failures == 0 ? 0 : 1;
}
