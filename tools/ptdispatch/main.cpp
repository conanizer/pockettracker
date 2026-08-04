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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "audio-engine.h"
#include "songcore/automation.h"   // §47 — the AUS/AUF pairing, which is pure and has no other home
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
    // Likely unreachable — every overlay open writes it first — but the default is Android's
    // (MainActivity.kt:777), and "likely" is not a spec (parity audit, finding 8).
    ok(state.previousScreen == ScreenType::PROJECT,
       "BOOT: ...and previousScreen defaults to PROJECT, as Android's does");

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

    // ── 5b. SELECT on a CONTEXT screen pops back to the column's main screen ───────────────────────
    //
    // Kotlin `handleSelect`'s `else` arm — the ONE arm the 2026-07-27 handler-surface diff found
    // missing from the C++ `on_select`. A bare SELECT on GROOVE / SCALE / MODS / SETTINGS (context and
    // popup screens with no SELECT action of their own) navigates to the main screen of the column they
    // belong to. Nothing else in the app exercises this — ptinput sits below the dispatcher, and §5
    // above only ever pressed SELECT on EFFECTS.
    //
    // ⚠️ THE NEGATIVE CONTROL IS THE POINT. The arm is deliberately NOT `!is_main_row(currentScreen)`:
    // PROJECT, MIXER, EFFECTS and INST_POOL are ALSO non-main-row screens, and Kotlin cases them as
    // explicit no-ops. So SELECT on MIXER (off its master-EQ cell) and on INST_POOL (off its EQ column)
    // must NOT navigate — a `!is_main_row` implementation would send both to PHRASE, and these two
    // assertions are what turn red if anyone "simplifies" the switch into that.
    {
        state.eq       = EqEditorState{};   // §5 could have left one open on an early failure
        state.qwerty   = QwertyKeyboardState{};
        state.selection.exit();

        state.previousColumn = 3;
        state.currentScreen  = ScreenType::MODS;
        dispatch.on_select();
        eq(static_cast<int>(state.currentScreen), static_cast<int>(ScreenType::INSTRUMENT),
           "SELECT on MODS pops back to INSTRUMENT (its column's main screen)");

        state.previousColumn = 2;
        state.currentScreen  = ScreenType::GROOVE;
        dispatch.on_select();
        eq(static_cast<int>(state.currentScreen), static_cast<int>(ScreenType::PHRASE),
           "SELECT on GROOVE pops back to PHRASE");

        state.previousColumn = 2;
        state.currentScreen  = ScreenType::SCALE;
        dispatch.on_select();
        eq(static_cast<int>(state.currentScreen), static_cast<int>(ScreenType::PHRASE),
           "SELECT on SCALE pops back to PHRASE");

        state.previousColumn = 0;
        state.currentScreen  = ScreenType::SETTINGS;
        dispatch.on_select();
        eq(static_cast<int>(state.currentScreen), static_cast<int>(ScreenType::SONG),
           "SELECT on SETTINGS pops back to the column's main screen (col 0 = SONG)");

        // ⚠️ NEGATIVE CONTROLS — the non-main-row screens Kotlin cases as no-ops must stay put.
        state.previousColumn  = 2;
        state.currentScreen   = ScreenType::MIXER;
        state.mixerMasterRow  = 0;   // NOT the master-EQ cell, so open_sub_screen_at_cursor is false
        state.mixerCursorColumn = 0;
        dispatch.on_select();
        eq(static_cast<int>(state.currentScreen), static_cast<int>(ScreenType::MIXER),
           "SELECT on MIXER (off the EQ cell) does NOT navigate — the !is_main_row control");

        state.previousColumn  = 3;
        state.currentScreen   = ScreenType::INST_POOL;
        state.poolCursorColumn = 0;   // NOT the EQ column (4)
        dispatch.on_select();
        eq(static_cast<int>(state.currentScreen), static_cast<int>(ScreenType::INST_POOL),
           "SELECT on INST_POOL (off the EQ column) does NOT navigate — its own 'not the default jump'");

        // …and a main-row screen is inert too (TABLE is in MAIN_ROW_SCREENS, so Kotlin's else no-ops it).
        state.previousColumn = 0;
        state.currentScreen  = ScreenType::TABLE;
        dispatch.on_select();
        eq(static_cast<int>(state.currentScreen), static_cast<int>(ScreenType::TABLE),
           "SELECT on TABLE (a main-row screen) does NOT navigate");
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

    // ── 7b. FOLDER = REMEMBER seeds a SAMPLE load at the last folder (v0.9.4 D2a) ─────────────────
    //
    // Tests the CONSUMER, not just the store (§21 round-trips the setting): does open_file_browser
    // actually START at lastSampleFolder? Keyed off the requested dir being the samples dir, gated on
    // rememberFolder, with an is_directory fallback so a deleted folder does not strand the browser.
    {
        const std::string samples = fs_impl.samples_directory();
        const std::string kicks   = (tree.root / "Samples" / "Kicks").generic_string();

        // (a) REMEMBER on + a real remembered folder → the browser opens THERE, not at the samples root.
        state.settings.rememberFolder   = true;
        state.settings.lastSampleFolder = kicks;
        dispatch.open_file_browser(AppState::BrowserPurpose::LOAD_SOURCE, samples, sample_extensions());
        eqs(state.fileBrowser.currentDirectory, kicks,
            "D2a: REMEMBER opens a sample load at the remembered folder");

        // (b) the SAMPLE-EDITOR load is the other sample purpose — it must honour it too.
        dispatch.open_file_browser(AppState::BrowserPurpose::LOAD_SAMPLE_EDITOR, samples, {"wav"});
        eqs(state.fileBrowser.currentDirectory, kicks,
            "D2a: …and so does the sample-editor LOAD");

        // (c) a stale (deleted) remembered folder falls back to the samples root, not an empty listing.
        state.settings.lastSampleFolder = (tree.root / "Samples" / "Gone").generic_string();
        dispatch.open_file_browser(AppState::BrowserPurpose::LOAD_SOURCE, samples, sample_extensions());
        eqs(state.fileBrowser.currentDirectory, samples,
            "D2a: a vanished remembered folder falls back to the samples dir");

        // (d) REMEMBER off → the setting is inert even with a valid path.
        state.settings.rememberFolder   = false;
        state.settings.lastSampleFolder = kicks;
        dispatch.open_file_browser(AppState::BrowserPurpose::LOAD_SOURCE, samples, sample_extensions());
        eqs(state.fileBrowser.currentDirectory, samples,
            "D2a: REFRESH (remember off) ignores the remembered folder");

        // (e) it is keyed off the SAMPLES dir specifically — a preset/soundfont load is NOT seeded,
        //     even with REMEMBER on, so those keep their own directories.
        state.settings.rememberFolder = true;
        dispatch.open_file_browser(AppState::BrowserPurpose::LOAD_PRESET, fs_impl.instruments_directory(),
                                   {"pti"});
        eqs(state.fileBrowser.currentDirectory, fs_impl.instruments_directory(),
            "D2a: a PRESET load is not seeded from the sample folder");
        state.settings.rememberFolder   = false;   // leave the state as the blocks below found it
        state.settings.lastSampleFolder.clear();
    }

    // ── 7c. config.json redirects a LOAD browse — DEBUG only (v0.9.4 D2b) ─────────────────────────
    //
    // The user's hand-edited config.json sets where a load browse STARTS per category. Debug-gated (like
    // OVERLAY/TRACE), applied only where the folder exists. Tests the parse AND the consumer (browser_dir
    // + open_file_browser), not just the read.
    {
        const std::string samples = fs_impl.samples_directory();
        const std::string kicks   = (tree.root / "Samples" / "Kicks").generic_string();

        // Hand-write a config.json at the PocketTracker root: samples → Kicks, projects → a dead path.
        {
            std::ofstream f(tree.root / "config.json");
            f << "{ \"folders\": { \"samples\": \"" << kicks << "\", \"projects\": \"/no/such/dir\" } }";
        }

        FolderConfig cfg{};
        ok(load_folder_config(fs_impl, cfg), "D2b: config.json parses");
        ok(cfg.samples.has_value() && *cfg.samples == kicks, "D2b: the samples override is read");
        ok(!cfg.soundfonts.has_value(), "D2b: an absent key stays unset (→ default)");
        state.folderConfig = cfg;

        // DEBUG build: the override wins where it is a real directory, and flows through the browser.
        state.caps = PlatformCaps::sdl(true);   // debug = true
        eqs(dispatch.browser_dir(InputDispatcher::BrowserDir::SAMPLES), kicks,
            "D2b: on a debug build, browser_dir(SAMPLES) is the config override");
        dispatch.open_file_browser(AppState::BrowserPurpose::LOAD_SOURCE,
                                   dispatch.browser_dir(InputDispatcher::BrowserDir::SAMPLES),
                                   sample_extensions());
        eqs(state.fileBrowser.currentDirectory, kicks,
            "D2b: a sample LOAD opens at the config override");

        // An override that is not a real directory falls back — a typo costs nothing.
        eqs(dispatch.browser_dir(InputDispatcher::BrowserDir::PROJECTS), fs_impl.projects_directory(),
            "D2b: a dead-path override falls back to the default directory");

        // A category with no key keeps its default.
        eqs(dispatch.browser_dir(InputDispatcher::BrowserDir::SOUNDFONTS), fs_impl.soundfonts_directory(),
            "D2b: an unset category keeps its built-in default");

        // RELEASE build: the whole feature is gated off.
        state.caps = PlatformCaps::sdl(false);  // debug = false
        eqs(dispatch.browser_dir(InputDispatcher::BrowserDir::SAMPLES), samples,
            "D2b: on a release build the override is ignored (debug-gated)");

        // Restore the shared state so the blocks below are unaffected.
        state.folderConfig = FolderConfig{};
        state.caps         = PlatformCaps::sdl(true);
        std::error_code ec;
        fs::remove(tree.root / "config.json", ec);
    }

    // ── 7d. config.json is SEEDED when absent, so the feature is discoverable (v0.9.4 D2b) ─────────
    //
    // The app never used to create config.json, so a user had nothing to find or edit (the reported "can't
    // see the folders settings"). seed_folder_config_template writes a starter — every category at its
    // default path — ONCE, and never rewrites an existing (user-owned) file.
    {
        std::error_code ec;
        fs::remove(tree.root / "config.json", ec);   // start from no file

        ok(seed_folder_config_template(fs_impl), "D2b: seeds a template when config.json is absent");
        ok(fs_impl.file_exists(fs_impl.config_path()), "D2b: …and the file now exists on disk");

        // It parses back, and every category is present and pointed at its current default directory.
        FolderConfig seeded{};
        ok(load_folder_config(fs_impl, seeded), "D2b: the seeded template parses");
        ok(seeded.samples.has_value()     && *seeded.samples     == fs_impl.samples_directory(),
           "D2b: samples pre-filled with the default dir");
        ok(seeded.projects.has_value()    && *seeded.projects    == fs_impl.projects_directory(),
           "D2b: projects pre-filled with the default dir");
        ok(seeded.themes.has_value()      && *seeded.themes      == fs_impl.themes_directory(),
           "D2b: themes pre-filled with the default dir");

        // Second call is a NO-OP — never clobbers the user's file. Prove it by editing the file and
        // checking the edit survives a re-seed.
        {
            std::ofstream f(tree.root / "config.json", std::ios::trunc);
            f << "{ \"folders\": { \"samples\": \"/my/edited/path\" } }";
        }
        ok(!seed_folder_config_template(fs_impl), "D2b: a second seed does nothing (file present)");
        FolderConfig afterEdit{};
        load_folder_config(fs_impl, afterEdit);
        ok(afterEdit.samples.has_value() && *afterEdit.samples == "/my/edited/path",
           "D2b: …the user's hand-edit survives — the app never rewrites config.json");

        fs::remove(tree.root / "config.json", ec);
    }

    // ── 7e. modal_backdrop_active counts EVERY full-canvas modal (B4) ─────────────────────────────
    //
    // The shell extends the modal dim into the letterbox bars / the PORTRAIT2 bezel gap for exactly the
    // modals this predicate names. A full-canvas modal left OUT of it dims INSIDE the 4:3 frame but not
    // around it — the reported FX-overlay seam. Derived from state, so this pins that each such modal is
    // counted and a future one added to `draw_*` without a predicate line goes red here.
    {
        state.qwerty.isOpen   = false;
        state.fxHelper.isOpen = false;
        ok(!modal_backdrop_active(state), "B4: no full-canvas modal → no window scrim");

        state.fxHelper.isOpen = true;   // the phrase-screen FX picker — draw_fx_helper paints MODAL_BACKDROP
        ok(modal_backdrop_active(state),
           "B4: the FX-helper overlay extends the dim to the bars/bezel (the round-2 fix)");
        state.fxHelper.isOpen = false;

        state.qwerty.isOpen = true;
        ok(modal_backdrop_active(state), "B4: …and so does the qwerty overlay (unchanged)");
        state.qwerty.isOpen = false;
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

        // Row 0 col 2 — the TYPE row's source LOAD. A sampler browses samples; a SoundFont browses .sf2.
        state.instrumentCursorRow = 0; state.instrumentCursorColumn = 2;
        dispatch.on_button_a();
        eq(static_cast<int>(state.currentScreen), static_cast<int>(ScreenType::FILE_BROWSER),
           "INSTRUMENT: A on the TYPE row's LOAD opens the browser");
        eq(static_cast<int>(state.browserPurpose),
           static_cast<int>(AppState::BrowserPurpose::LOAD_SOURCE), "INSTRUMENT: …for a SOURCE");
        eqs(state.fileBrowser.currentDirectory, fs_impl.samples_directory(),
            "INSTRUMENT: …in the SAMPLES directory (the slot is a sampler)");
        dispatch.on_button_b();

        host.edit_project().instruments[5].instrumentType = songcore::InstrumentType::SOUNDFONT;
        state.instrumentCursorRow = 0; state.instrumentCursorColumn = 2;
        dispatch.on_button_a();
        eqs(state.fileBrowser.currentDirectory, fs_impl.soundfonts_directory(),
            "⚠️ INSTRUMENT: a SOUNDFONT slot browses SOUNDFONTS instead — same cell, different folder");
        dispatch.on_button_b();
        host.edit_project().instruments[5].instrumentType = songcore::InstrumentType::SAMPLER;

        // Row 5 col 3 — the INST PRESET row's LOAD (.pti): a different directory AND a different purpose
        // from the source LOAD on row 0. Two LOADs that used to sit on one row, now clearly separated.
        state.instrumentCursorRow = 5; state.instrumentCursorColumn = 3;
        dispatch.on_button_a();
        eq(static_cast<int>(state.browserPurpose),
           static_cast<int>(AppState::BrowserPurpose::LOAD_PRESET),
           "INSTRUMENT: the INST PRESET row's LOAD is a PRESET");
        eqs(state.fileBrowser.currentDirectory, fs_impl.instruments_directory(),
            "INSTRUMENT: …in the INSTRUMENTS directory");
        dispatch.on_button_b();

        // Row 5 col 2 — SAVE PRESET, which opens the keyboard rather than the browser.
        state.instrumentCursorRow = 5; state.instrumentCursorColumn = 2;
        dispatch.on_button_a();
        ok(state.qwerty.isOpen, "INSTRUMENT: the INST PRESET row's SAVE opens the KEYBOARD, not the browser");
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
           "INSTRUMENT: …and the SAVE button does NOT — it is a read-only cell with no A-combo to protect");

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

        state.instrumentCursorRow = 0; state.instrumentCursorColumn = 3;   // the EDIT cell (TYPE row)
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

        // ⚠️ A SoundFont has no single waveform to cut — it is a bank of them. The EDIT button is not
        // drawn on SF (the cursor caps at LOAD), but forced onto the cell the press is still CONSUMED
        // rather than falling through — it opens nothing.
        host.edit_project().instruments[7].instrumentType = songcore::InstrumentType::SOUNDFONT;
        state.instrumentCursorRow = 0; state.instrumentCursorColumn = 3;
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
        state.instrumentCursorRow = 0; state.instrumentCursorColumn = 3;
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
        state.instrumentCursorRow = 0; state.instrumentCursorColumn = 3;
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
        state.instrumentCursorRow = 0; state.instrumentCursorColumn = 3;
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

        // FOLDER (D2a) navigates between CURSOR and NOTE PREV — its VALUE is still 13, but its POSITION
        // is with the app toggles (SETTINGS_DISPLAY_ORDER), so the D-pad reaches it there, not after
        // RESUME. This is the reposition the user asked for, pinned so it cannot silently drift back.
        state.caps                 = PlatformCaps::android(true);
        state.settingsCursorRow    = static_cast<int>(SettingsRow::CURSOR);
        state.settingsCursorColumn = 1;
        dispatch.on_dpad_down();
        eq(state.settingsCursorRow, static_cast<int>(SettingsRow::FOLDER),
           "SETTINGS: DOWN from CURSOR lands on FOLDER (D2a: repositioned between CURSOR and NOTE PREV)");
        dispatch.on_dpad_down();
        eq(state.settingsCursorRow, static_cast<int>(SettingsRow::NOTE_PREV),
           "SETTINGS: …and DOWN from FOLDER lands on NOTE PREV");
        dispatch.on_dpad_up();
        eq(state.settingsCursorRow, static_cast<int>(SettingsRow::FOLDER),
           "SETTINGS: UP from NOTE PREV returns to FOLDER");

        // The shell has TEN of the fourteen; Android has all fourteen (in a debug build). FOLDER (row
        // 13, v0.9.4 D2a) is an app row visible on every platform, so it adds one to each count below.
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
        eq(shellRows, 10,
           "SETTINGS[sdl]: ten rows — SCALING, KB, CURSOR, PREV, VIZ, THEME, TPL, RESUME (S10), TRACE, FOLDER (D2a)");
        ok(settings_row_visible(SettingsRow::RESUME, state.caps),
           "SETTINGS[sdl]: …and RESUME is one of them — the row is BACK, because the autosave exists");

        state.caps = PlatformCaps::android(true);
        int androidRows = 0;
        for (int r = 0; r < SETTINGS_ROW_COUNT; ++r)
            if (settings_row_visible(static_cast<SettingsRow>(r), state.caps)) ++androidRows;
        eq(androidRows, 14, "SETTINGS[android+debug]: all fourteen (13 + FOLDER, D2a)");

        state.caps = PlatformCaps::android(false);
        int androidRel = 0;
        for (int r = 0; r < SETTINGS_ROW_COUNT; ++r)
            if (settings_row_visible(static_cast<SettingsRow>(r), state.caps)) ++androidRel;
        eq(androidRel, 12, "SETTINGS[android+release]: OVERLAY and TRACE drop out (BuildConfig.DEBUG); FOLDER stays");
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

        // ⚠️ THESE TWO NUMBERS MOVED IN B4.3 (7→8, 6→7), and that is the change being asserted rather
        // than a golden being repaired: the MIDI row was appended after SYSTEM, so the LAST row on each
        // platform is one further down. They are written as literals on purpose — deriving them from
        // `project_last_row()` would make the check agree with the map by construction and stop being a
        // check at all. Both are stated against the ROW NAME so the next reader can see which is which.
        dispatch.on_dpad_up();
        eq(state.projectCursorRow, 8, "PROJECT[sdl]: UP from row 0 WRAPS to EXIT (row 8, after MIDI)");

        state.caps = PlatformCaps::android(true);
        state.projectCursorRow = 0;
        dispatch.on_dpad_up();
        eq(state.projectCursorRow, 7, "PROJECT[android]: …wraps to MIDI (row 7) — there is no EXIT");

        // …and the row it displaced still sits where every caller expects it. SYSTEM keeping the number
        // 6 is the whole reason `p3-input`'s 4410 recorded PROJECT cases survived this increment.
        state.caps = PlatformCaps::sdl(true);
        state.projectCursorRow = static_cast<int>(ProjectRow::COMPACT);
        dispatch.on_dpad_down();
        eq(state.projectCursorRow, 6, "PROJECT: DOWN off COMPACT still lands on SYSTEM (row 6)");
        dispatch.on_dpad_down();
        eq(state.projectCursorRow, 7, "PROJECT: …and MIDI is the row below it");

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
        state.settings.rememberFolder    = true;                        // D2a — the FOLDER toggle
        state.settings.lastSampleFolder  = "/sdcard/PocketTracker/Samples/Kicks";  // SESSION-ONLY path
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
        // D2a — the TOGGLE persists, but the remembered PATH is SESSION-ONLY: it must NOT round-trip, so
        // the browser opens at the default folder after a restart (the user's call — like CURSOR).
        ok(back.rememberFolder, "settings.json: rememberFolder (the toggle) round-trips");
        eqs(back.lastSampleFolder, "",
            "settings.json: lastSampleFolder is SESSION-ONLY — it does NOT persist (resets to default)");
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
    // ── 23b. RESAMPLE — the SONG-selection render (needs a REAL engine, like §23) ────────────────
    // ═════════════════════════════════════════════════════════════════════════════════════════════
    //
    // ⚠️ RESAMPLE was the port's canonical DEAD SEAM (convergence-plan.md §9): the keyboard context, the
    // on_a_a arm and `resampled_directory()` were each half-built and CALLED FROM NOWHERE, so a
    // dead-seam grep re-found it while every conformance tool stayed green. This section is what turns
    // that green into a claim — it drives the whole gesture end to end on a real engine: select on SONG,
    // A,A, name it, APPLY, and check the WAV lands in Resampled/ AND becomes a fresh SAMPLER instrument.
    // Without it the feature is exactly as unproven as the day the plan flagged it.
    {
        auto engine = std::make_unique<AudioEngine>();   // ⚠️ HEAP — see §23
        engine->setDeviceSampleRate(44100);

        songcore::SongcoreHost rhost(engine.get(), 44100);

        AppState rstate;
        rstate.project = &rhost.edit_project();
        rstate.caps    = PlatformCaps::sdl(true);

        InputDispatcher rdispatch(rstate, rhost, fs_impl);

        // The §23 fixture: a C4 every fourth step on tracks 0 and 1 — audible, and two tracks so the
        // selection's track filter has something to include and something to leave out.
        songcore::Project& p = rhost.edit_project();
        p = songcore::make_default_project();
        p.name = "RESAMPLE TEST";
        for (int track = 0; track < 2; ++track) {
            p.tracks[track].chainRefs.assign(256, -1);
            p.tracks[track].chainRefs[0]  = track;
            p.chains[track].phraseRefs[0] = track;
            for (int step = 0; step < 4; ++step) {
                songcore::PhraseStep& s = p.phrases[track].steps[static_cast<size_t>(step * 4)];
                s.note       = songcore::Note::C4();
                s.instrument = 0;
            }
        }
        rhost.push_params();

        // A SONG selection over row 0, tracks 0 and 1 — the REAL gesture: L+B anchors a CELL at the
        // cursor, the D-pad drags its edge one column right.
        rstate.currentScreen = ScreenType::SONG;
        rstate.cursorRow = 0; rstate.cursorColumn = 1;
        rdispatch.set_now(1000);
        rdispatch.on_l_b();          // CELL selection at (0,1)
        rdispatch.on_dpad_right();   // widen to columns 1..2 = tracks 0,1
        ok(rstate.selection.active, "RESAMPLE: L+B on SONG starts a selection");
        eq(rstate.selection.bounds().topLeftColumn, 1,     "RESAMPLE: …anchored at track 0 (column 1)");
        eq(rstate.selection.bounds().bottomRightColumn, 2, "RESAMPLE: …the D-pad widened it to track 1");

        // A,A on a SONG selection opens the RESAMPLE keyboard — NOT the insert-a-chain path a bare A,A
        // takes without a selection (the arm that runs BEFORE the double-tap-position gate).
        rdispatch.on_a_a();
        ok(rstate.qwerty.isOpen, "RESAMPLE: A,A on a SONG selection opens the keyboard");
        ok(rstate.qwerty.context == QwertyContext::RESAMPLE, "RESAMPLE: …with the RESAMPLE context");
        eqs(rstate.qwerty.fieldLabel, "SAMPLE NAME:", "RESAMPLE: …labelled SAMPLE NAME");

        // Name it and APPLY (START). A typed name is used verbatim, so the file is deterministic.
        rstate.qwerty.text = "MYRESAMPLE"; rstate.qwerty.textCursor = 10;
        rdispatch.on_start();

        ok(!rstate.isRendering, "RESAMPLE: the render flag is down again afterwards");
        eqs(rstate.statusMessage, "RESAMPLED -> INST 00",
            "RESAMPLE: the sample lands in the first FREE slot (00) and says so");
        ok(rstate.statusSuccess, "RESAMPLE: …reported as success (green)");

        const fs::path wav = fs::path(fs_impl.resampled_directory()) / "MYRESAMPLE.wav";
        ok(fs::exists(wav), "RESAMPLE: the WAV is Resampled/MYRESAMPLE.wav (the typed name, verbatim)");
        ok(fs::exists(wav) && fs::file_size(wav) > 44 * 100,
           "RESAMPLE: …with real audio in it, not just a 44-byte header");

        // The slot is now a playable SAMPLER pointing at that WAV — Kotlin's createResampledInstrument.
        const songcore::Instrument& ins = p.instruments[0];
        ok(ins.instrumentType == songcore::InstrumentType::SAMPLER, "RESAMPLE: slot 00 is a SAMPLER");
        ok(ins.sampleFilePath.has_value() && fs::path(*ins.sampleFilePath) == wav,
           "RESAMPLE: …whose source IS the resampled WAV");
        ok(ins.root == songcore::Note::C4(), "RESAMPLE: …rooted at C-4");

        // The AUTO-NAME path: the selection is still live (APPLY never exits it), so A,A re-opens the
        // keyboard; clearing the field falls back to Resample_0001, de-duplicated. Slot 00 is taken now,
        // so this one claims the next free slot.
        rdispatch.on_a_a();
        ok(rstate.qwerty.isOpen && rstate.qwerty.context == QwertyContext::RESAMPLE,
           "RESAMPLE: the selection survives APPLY, so A,A opens the keyboard again");
        rstate.qwerty.text = ""; rstate.qwerty.textCursor = 0;
        rdispatch.on_start();
        ok(fs::exists(fs::path(fs_impl.resampled_directory()) / "Resample_0001.wav"),
           "RESAMPLE: an empty name auto-generates Resample_0001.wav");
        eqs(rstate.statusMessage, "RESAMPLED -> INST 01",
            "RESAMPLE: …into the NEXT free slot (00 is now the first resample)");
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
        // shell writes settings.json only `if (state.settingsDirty)` (shell/main.cpp) — and the ONLY
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
            // whole claim is that the path the SHELL takes on exit (shell/main.cpp) picks this edit up
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

    // ── 31. R+LEFT / R+RIGHT carry the lastEdited memory across the switch — and ONLY they do ────
    //        (§30 is P4f's RELOC block above — its header spells the number `§30`, which is how a
    //        sweep for `── N.` numbered this batch into a collision. Renumbered; the P4h commit
    //        messages for findings 2/4/7/5 say sections 30–33, which are these, 31–34.)
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

    // ── 32. NEW / LOAD reset the WHOLE editing context — every Kotlin-reset cursor goes home ─────
    //
    // TrackerController.resetEditingContext (:272–304) resets every secondary screen's cursor and
    // the three REMEMBER slots on top of the current*/lastEdited* set; the port reset a subset, so
    // a LOAD left INSTRUMENT / MIXER / EFFECTS / TABLE / GROOVE / MODS / PROJECT — and the REMEMBER
    // slots — pointing into the PREVIOUS song. Driven through the dispatcher's own NEW, the same
    // reset_editing_context funnel that LOAD and RECOVER run through.
    {
        state.caps          = PlatformCaps::sdl(true);
        state.currentScreen = ScreenType::PROJECT;
        state.confirm.close();
        state.projectCursorRow    = static_cast<int>(ProjectRow::PROJECT);
        state.projectCursorColumn = 3;   // NEW
        state.projectVersion = state.savedProjectVersion = 0;   // clean: no confirm in the way

        // Dirty EVERYTHING — first the Kotlin-reset set…
        state.instrumentCursorRow = 9; state.instrumentCursorColumn = 3;
        state.mixerCursorColumn   = 7;
        state.effectsCursorRow    = 5;
        state.tableCursorRow      = 12; state.tableCursorColumn = 6;
        state.grooveCursorRow     = 11;
        state.modCursorRow = 4; state.modCursorPair = 1; state.modCursorSide = 1;
        state.songCursorRow = 40; state.songCursorColumn = 5;
        state.chainCursorRow = 9; state.chainCursorColumn = 2;
        state.phraseCursorRow = 13; state.phraseCursorColumn = 7;
        // …then the three Kotlin deliberately LEAVES alone.
        state.mixerMasterRow    = 3;
        state.settingsCursorRow = 5; state.settingsCursorColumn = 1;
        state.poolCursorColumn  = 4;

        dispatch.on_button_a();   // NEW — clean project, so no question asked (§17)
        eqs(state.statusMessage, "NEW PROJECT", "RESET: the NEW actually ran");

        eq(state.instrumentCursorRow, 0,    "RESET: instrument cursor row went home");
        eq(state.instrumentCursorColumn, 1, "RESET: ...and its column");
        eq(state.mixerCursorColumn, 0,      "RESET: mixer column went home");
        eq(state.effectsCursorRow, 0,       "RESET: effects row went home");
        eq(state.tableCursorRow, 0,         "RESET: table cursor row went home");
        eq(state.tableCursorColumn, 1,      "RESET: ...and its column");
        eq(state.grooveCursorRow, 0,        "RESET: groove row went home");
        eq(state.modCursorRow, 0,           "RESET: mod row went home");
        eq(state.modCursorPair, 0,          "RESET: ...its pair");
        eq(state.modCursorSide, 0,          "RESET: ...and its side");
        eq(state.projectCursorRow, 0,       "RESET: PROJECT's own cursor row went home");
        eq(state.projectCursorColumn, 1,    "RESET: ...and its column");
        eq(state.songCursorRow, 0,          "RESET: the SONG remember slot went home (row)");
        eq(state.songCursorColumn, 1,       "RESET: ...and column");
        eq(state.chainCursorRow, 0,         "RESET: the CHAIN remember slot went home (row)");
        eq(state.chainCursorColumn, 1,      "RESET: ...and column");
        eq(state.phraseCursorRow, 0,        "RESET: the PHRASE remember slot went home (row)");
        eq(state.phraseCursorColumn, 1,     "RESET: ...and column");

        // ⚠️ The QUIRKS are part of the spec: Kotlin does NOT reset these three. Match exactly —
        // the pool's ROW is currentInstrument, which IS reset; its column is not.
        eq(state.mixerMasterRow, 3,    "RESET-NEG: mixerMasterRow is NOT reset (Kotlin quirk)");
        eq(state.settingsCursorRow, 5, "RESET-NEG: the SETTINGS cursor is NOT reset (Kotlin quirk)");
        eq(state.poolCursorColumn, 4,  "RESET-NEG: poolCursorColumn is NOT reset (Kotlin quirk)");
    }

    // ── 33. An EQ-only session still gets crash protection — the band edit ARMS the autosave ─────
    //
    // The EQ path deliberately bypasses mark_modified (its wholesale push_globals is oversized for a
    // 100 ms-repeat band dial) and ended in a bare projectVersion++ — dirty, but the autosave
    // debounce was never armed. On Android EVERY projectVersion bump re-keys the autosave
    // LaunchedEffect (MainActivity.kt:754), so a session whose only edits are EQ bands had crash
    // protection there and NONE here — P4d's "the write nobody armed", third body (parity audit,
    // finding 7).
    {
        state.caps          = PlatformCaps::sdl(true);
        state.currentScreen = ScreenType::MIXER;
        state.confirm.close();
        state.eq          = EqEditorState{};
        state.themeEditor = ThemeEditorState{};
        state.qwerty      = QwertyKeyboardState{};

        host.edit_project() = songcore::make_default_project();
        state.projectVersion = state.savedProjectVersion = 0;   // clean FIRST, so a stale pending
        autosave_clear(fs_impl);                                // deadline cannot write on set_now
        dispatch.set_now(1000000);

        state.mixerMasterRow = 1; state.mixerCursorColumn = 8;  // the master strip's EQ cell
        dispatch.on_button_a();
        ok(state.eq.isOpen, "EQ-ARM: A on the mixer's EQ cell opens the editor");

        dispatch.on_a_up();   // dial the band under the default cursor (BAND 1, TYPE)
        ok(state.project_dirty(), "EQ-ARM: a band edit marks the project dirty");
        dispatch.set_now(1002999);
        ok(!autosave_exists(fs_impl), "EQ-ARM: ...and respects the debounce (nothing at 2999 ms)");
        dispatch.set_now(1003000);
        ok(autosave_exists(fs_impl),
           "EQ-ARM: a session of ONLY EQ edits writes its crash autosave at 3000 ms");
        autosave_clear(fs_impl);
    }

    // ── 34. The status line auto-dismisses 5 s after it is set — and only on a CHANGE ────────────
    //
    // MainActivity.kt:734–747: two LaunchedEffects clear the status 5 s after it is set; the port
    // painted "SAVED" until the next message replaced it (parity audit, finding 5). The dismissal
    // is derived from the DATA — set_now watches the field for changes rather than trusting 22
    // call sites to stamp a deadline — so the window opens on the frame tick AFTER the action.
    // The +16 ms latches below make that explicit: the shell's loop calls set_now once per frame,
    // BEFORE it dispatches the frame's buttons.
    {
        state.caps          = PlatformCaps::sdl(true);
        state.currentScreen = ScreenType::PROJECT;
        state.confirm.close();
        state.eq          = EqEditorState{};
        state.themeEditor = ThemeEditorState{};
        state.qwerty      = QwertyKeyboardState{};
        state.projectCursorRow    = static_cast<int>(ProjectRow::PROJECT);
        state.projectCursorColumn = 3;   // NEW — sets "NEW PROJECT" with no disk involved
        state.statusMessage.clear();     // …and the line starts empty, whatever §31 left painted
        state.statusSuccess = true;

        // (a) set → hold through 4999 ms → gone at 5000 ms, with statusSuccess restored.
        state.projectVersion = state.savedProjectVersion = 0;   // clean: NEW asks nothing
        dispatch.set_now(2000000);
        dispatch.on_button_a();
        eqs(state.statusMessage, "NEW PROJECT", "STATUS: the action reported");
        dispatch.set_now(2000016);   // the next frame — the watcher opens the window from HERE
        dispatch.set_now(2005015);
        eqs(state.statusMessage, "NEW PROJECT", "STATUS: still painted 4999 ms into the window");
        state.statusSuccess = false;   // paint it red by hand: the dismissal must restore green
        dispatch.set_now(2005016);
        eqs(state.statusMessage, "", "STATUS: auto-dismissed 5 s after the frame loop noticed it");
        ok(state.statusSuccess, "STATUS: ...and statusSuccess resets to true (clearStatus parity)");

        // (b) after a dismissal, a fresh message opens a fresh window. ⚠️ NEW resets the PROJECT
        // cursor home (finding 4), so every press below re-aims at the NEW cell first — the first
        // draft did not, and its "re-raise" check passed on the pre-fix build only because the STALE
        // message was still painted. A pass has to be read, not counted.
        state.projectCursorRow    = static_cast<int>(ProjectRow::PROJECT);
        state.projectCursorColumn = 3;
        dispatch.set_now(2010000);
        dispatch.on_button_a();
        eqs(state.statusMessage, "NEW PROJECT", "STATUS: a later action re-raises the line");
        dispatch.set_now(2010016);
        dispatch.set_now(2015015);
        eqs(state.statusMessage, "NEW PROJECT", "STATUS: ...and its window is fresh (4999 ms in)");

        // (c) ⚠️ an IDENTICAL message inside the window does NOT extend it. Kotlin's effect is keyed
        // on the VALUE — an equal key does not restart the delay — and deriving the dismissal from
        // the field CHANGING reproduces exactly that.
        state.projectCursorRow    = static_cast<int>(ProjectRow::PROJECT);
        state.projectCursorColumn = 3;
        dispatch.on_button_a();      // the same text again, 4999 ms into the window
        eqs(state.statusMessage, "NEW PROJECT", "STATUS: (setup) the identical re-set really happened");
        dispatch.set_now(2015016);
        eqs(state.statusMessage, "",
            "STATUS: an identical re-set does NOT extend the window (LaunchedEffect key semantics)");
    }

    // ── 35. A,A on a PHRASE note cell advances to the next FREE instrument (v0.9.4 D1) ────────────
    //
    // Mirrors chain/song A,A ("insert the next unused ref"): the first A lays the last-used note +
    // instrument, a second A on the SAME cell keeps the note and re-points it at the next free slot.
    // "Free" is instrument_is_free — the resample-safe predicate — so an occupied sample slot is
    // skipped and a configured SoundFont would be too (not a bare sampleFilePath==null).
    {
        songcore::Project& p = host.edit_project();

        // Reset every overlay so on_a_a is not swallowed by a modal gate.
        state.confirm.close();
        state.eq          = EqEditorState{};
        state.themeEditor = ThemeEditorState{};
        state.qwerty      = QwertyKeyboardState{};
        state.selection   = {};
        state.settings.notePreviewEnabled = false;

        // Occupy instruments 6 and 7 so the next free slot after 5 is 8, not 6.
        p.instruments[6].sampleFilePath = "kick.wav";
        p.instruments[7].sampleFilePath = "snare.wav";
        ok(!songcore::instrument_is_free(p.instruments[6]), "D1: (setup) instrument 6 is occupied");

        state.currentScreen = ScreenType::PHRASE;
        state.currentPhrase = 0x20;
        state.cursorRow     = 4;
        state.cursorColumn  = 1;   // the NOTE column
        state.lastEditedNote       = songcore::note_from_midi(62);   // D-4, distinct from the default
        state.lastEditedInstrument = 5;
        state.lastEditedVolume     = 0x40;

        songcore::PhraseStep& step = p.phrases[0x20].steps[4];
        ok(step.note == songcore::Note::EMPTY(), "D1: (setup) the target note cell starts empty");

        dispatch.on_button_a();   // first A: lay the last-used note + instrument, arm A,A
        eq(step.instrument, 5, "D1: the first A inserts the last-used instrument (5)");
        ok(step.note == songcore::note_from_midi(62), "D1: ...and the last-used note (D-4)");

        dispatch.on_a_a();        // second A on the same cell: advance the instrument
        eq(step.instrument, 8, "D1: A,A advances to the next FREE instrument, skipping 6 and 7");
        ok(step.note == songcore::note_from_midi(62), "D1: ...and keeps the note the first A laid down");
        eq(state.lastEditedInstrument, 8, "D1: ...and lastEditedInstrument follows, as chain/song A,A do");
    }

    // ══ B4 ══ THE INSTRUMENT SCREEN'S ROW GEOMETRY, AND THE EXTERNAL LAYOUT ══════════════════════
    //
    // ⚠️ **THIS BLOCK EXISTS BECAUSE A NEGATIVE CONTROL DID NOT FIRE.** Before it, changing the
    // SAMPLER row table's row 3 from TRIPLE to DUAL — which moves where the cursor lands walking down
    // onto VOL/SLICE/PAN, and how far LEFT/RIGHT step there — left ALL SEVENTEEN ctest suites green.
    // ptinput byte-compares this screen's contexts and writes but is handed a row and column; it never
    // asks whether the cursor could REACH them. ptdispatch drove INSTRUMENT's buttons and its EQ row
    // and nothing else. So `ui/instrument_row_layout.h` — the one table the whole screen derives from,
    // and the file whose own comment says a wrong entry "strands the cursor on a spacer" — had no test
    // at all, on any type. That gap predates EXTERNAL; adding a third layout is what found it.
    //
    // Two claims, then: the two ORIGINAL layouts still step exactly as they did (the sampler's 16 rows
    // and the SoundFont's 15, spacers skipped, columns snapped), and EXTERNAL's 11 behave.
    {
        songcore::Project& p = host.edit_project();
        state.currentScreen     = ScreenType::INSTRUMENT;
        state.currentInstrument = 9;
        songcore::Instrument& ins = p.instruments[9];

        // Walk the whole map with the D-pad and collect the rows it stops on. A spacer in the list, a
        // row missing from it, or a wrong count all show up as one comparable token.
        const auto walk_rows = [&](songcore::InstrumentType type) {
            ins.instrumentType         = type;
            state.instrumentCursorRow    = 0;
            state.instrumentCursorColumn = 1;
            std::string out = "0";
            for (int i = 0; i < 20; ++i) {
                dispatch.on_dpad_down();
                if (state.instrumentCursorRow == 0) break;   // wrapped: one full lap
                out += "," + std::to_string(state.instrumentCursorRow);
            }
            return out;
        };

        eqs(walk_rows(songcore::InstrumentType::SAMPLER), "0,1,2,3,5,7,8,9,11,12,13,14,15",
            "INSTRUMENT rows, SAMPLER: 16 rows, spacers 4/6/10 stepped over, wraps at 15");
        eqs(walk_rows(songcore::InstrumentType::SOUNDFONT), "0,1,2,3,5,6,8,9,10,12,13,14",
            "INSTRUMENT rows, SOUNDFONT: 15 rows — PATCH at 6 is LIVE, spacers 4/7/11 are not");
        eqs(walk_rows(songcore::InstrumentType::EXTERNAL), "0,1,2,3,5,7,8,9,10,11",
            "INSTRUMENT rows, EXTERNAL: 12 rows — the patch, the preset row, then VOL/PAN and 4 CCs");

        // The COLUMN the cursor lands in after a vertical move, and how far LEFT/RIGHT reach. Walking
        // down the right-hand column must stay in it; a TRIPLE's third column (5) exists only on a
        // TRIPLE. This is the half the failed control was aimed at.
        const auto column_after = [&](songcore::InstrumentType type, int fromRow, int fromCol) {
            ins.instrumentType           = type;
            state.instrumentCursorRow    = fromRow;
            state.instrumentCursorColumn = fromCol;
            dispatch.on_dpad_down();
            return state.instrumentCursorColumn;
        };
        eq(column_after(songcore::InstrumentType::SAMPLER, 2, 5), 5,
           "INSTRUMENT columns, SAMPLER: TIC (row 2, col 5) steps down onto PAN (row 3, col 5)");
        eq(column_after(songcore::InstrumentType::SOUNDFONT, 2, 5), 3,
           "⚠️ INSTRUMENT columns, SOUNDFONT: row 3 is a DUAL — col 5 does not exist there, so a "
           "cursor coming down the right-hand side lands on the rightmost cell that DOES (3 = PAN)");
        eq(column_after(songcore::InstrumentType::SAMPLER, 7, 3), 3,
           "INSTRUMENT columns, SAMPLER: FILTER (7,3) steps down onto FREQ (8,3), right column kept");
        eq(column_after(songcore::InstrumentType::EXTERNAL, 2, 3), 3,
           "INSTRUMENT columns, EXTERNAL: BANK (2,3) steps down onto LEN (3,3) — both DUAL");
        eq(column_after(songcore::InstrumentType::EXTERNAL, 3, 3), 2,
           "INSTRUMENT columns, EXTERNAL: …and on down to the preset row, which SNAPS to LOAD (2)");

        // Row 0's cap is the number of BUTTONS drawn beside TYPE, and it differs on all three types.
        // A cursor past the cap sits on a cell that is not drawn — the exact bug the table prevents.
        const auto right_cap = [&](songcore::InstrumentType type) {
            ins.instrumentType           = type;
            state.instrumentCursorRow    = 0;
            state.instrumentCursorColumn = 1;
            for (int i = 0; i < 5; ++i) dispatch.on_dpad_right();
            return state.instrumentCursorColumn;
        };
        eq(right_cap(songcore::InstrumentType::SAMPLER), 3,
           "INSTRUMENT row 0, SAMPLER: RIGHT reaches EDIT (3) and stops");
        eq(right_cap(songcore::InstrumentType::SOUNDFONT), 2,
           "INSTRUMENT row 0, SOUNDFONT: …caps at LOAD (2) — no waveform to EDIT");
        eq(right_cap(songcore::InstrumentType::EXTERNAL), 1,
           "⚠️ INSTRUMENT row 0, EXTERNAL: …caps at TYPE (1) — no source to LOAD and none to EDIT");

        // ── The TYPE cell now CYCLES THREE WAYS, and A+UP and A+DOWN disagree ────────────────────
        //
        // ⚠️ The direction is the whole point of the change. As a two-way toggle both buttons meant
        // the same thing and the code ignored which had been pressed; with three types, a cell that
        // only walks forwards takes two presses to undo one.
        const auto type_of = [&] { return static_cast<int>(ins.instrumentType); };
        ins.instrumentType = songcore::InstrumentType::SAMPLER;
        ins.sampleFilePath.reset();
        ins.soundfontPath.reset();
        state.instrumentCursorRow = 0; state.instrumentCursorColumn = 1;

        dispatch.on_a_up();
        eq(type_of(), static_cast<int>(songcore::InstrumentType::SOUNDFONT),
           "TYPE cell: A+UP steps SAMPLER → SOUNDFONT");
        dispatch.on_a_up();
        eq(type_of(), static_cast<int>(songcore::InstrumentType::EXTERNAL),
           "⚠️ TYPE cell: …and on to EXTERNAL — the third type is REACHABLE, which is the whole of B4.1");
        dispatch.on_a_up();
        eq(type_of(), static_cast<int>(songcore::InstrumentType::SAMPLER),
           "TYPE cell: …and wraps back to SAMPLER");
        dispatch.on_a_down();
        eq(type_of(), static_cast<int>(songcore::InstrumentType::EXTERNAL),
           "⚠️ TYPE cell: A+DOWN steps the OTHER WAY (SAMPLER → EXTERNAL), not the same way as A+UP");

        // ── …and the direction survives the confirm dialog ───────────────────────────────────────
        //
        // ⚠️ A slot with a source loaded ASKS first (S7), and the answer arrives on a later frame with
        // the pressed button long gone. `confirm_accept` closes the box BEFORE running the arm, and
        // `close()` resets `arg` — so reading the direction after the close would silently turn every
        // confirmed A+DOWN into an A+UP. That is a wrong answer that still looks like the cell working,
        // which is why it is pinned here rather than trusted.
        ins.instrumentType  = songcore::InstrumentType::SAMPLER;
        ins.sampleFilePath  = std::string("kick.wav");
        state.instrumentCursorRow = 0; state.instrumentCursorColumn = 1;

        dispatch.on_a_down();
        eq(type_of(), static_cast<int>(songcore::InstrumentType::SAMPLER),
           "TYPE cell: A+DOWN on a LOADED slot changes nothing yet — it asks");
        ok(state.confirm.is_open() && state.confirm.kind == ConfirmDialogState::Kind::CHANGE_TYPE,
           "TYPE cell: …the CHANGE TYPE? dialog is up");
        dispatch.on_button_a();   // YES
        eq(type_of(), static_cast<int>(songcore::InstrumentType::EXTERNAL),
           "⚠️ TYPE cell: …and YES honours the DOWN it was asked about (→ EXTERNAL, not → SOUNDFONT)");
        ok(!ins.sampleFilePath.has_value(),
           "TYPE cell: …and the sample the question was about is freed");

        // ── EXTERNAL has no source, so neither of row 0's buttons does anything ──────────────────
        //
        // The cursor cannot reach columns 2 or 3 there (capped at 1, pinned above), so this is the
        // belt-and-braces half: put it there by hand and press A. A browser opening here would offer
        // to load a sample into a slot that has no sampler — and the load would flip the type back.
        state.instrumentCursorRow = 0; state.instrumentCursorColumn = 2;
        dispatch.on_button_a();
        eq(static_cast<int>(state.currentScreen), static_cast<int>(ScreenType::INSTRUMENT),
           "⚠️ EXTERNAL: A on row 0 col 2 opens NO browser — there is no source to load");

        // ── The EXTERNAL cells, through the generic five-button vocabulary ───────────────────────
        //
        // ⚠️ A VALUE CELL IS DIALLED WITH A+DPAD, NOT WITH A BARE A. `on_button_a` falls through to
        // `default: break;` on this screen — a plain A is for the BUTTONS (LOAD, SAVE, EDIT) and for
        // inserting on the sequencer screens. The first draft of this block pressed `on_button_a` on
        // every cell below and watched all nine assertions fail against an untouched instrument, which
        // is the harness being wrong about the gesture rather than the code being wrong about the cell.
        //
        // Every one of these is a byte that leaves over the cable (songcore/midi_out.h), so they are
        // asserted as the wire numbers, not as what the screen prints.
        ins.instrumentType = songcore::InstrumentType::EXTERNAL;
        ins.midiChannel = 0; ins.midiBank = -1; ins.midiProgram = -1; ins.midiLen = 0;
        ins.pan = 0x80;
        ins.midiCC[0] = songcore::MidiCcSlot{};

        state.instrumentCursorRow = 2; state.instrumentCursorColumn = 1;   // CHAN
        dispatch.on_a_up();
        eq(ins.midiChannel, 1, "EXTERNAL CHAN: A+UP steps 0 → 1 (stored 0-15; the screen shows 1-16)");
        dispatch.on_a_down();
        dispatch.on_a_down();
        eq(ins.midiChannel, 15, "⚠️ EXTERNAL CHAN: …and A+DOWN below 0 WRAPS to 15, not to 255");

        state.instrumentCursorRow = 3; state.instrumentCursorColumn = 1;   // PROG
        eq(ins.midiProgram, -1, "EXTERNAL PROG: (setup) starts at −1 = send nothing");
        dispatch.on_a_up();
        eq(ins.midiProgram, 0,
           "⚠️ EXTERNAL PROG: A+UP on an OFF cell INSERTS program 0 — 0 is a real program, −1 is silence");
        dispatch.on_a_up();
        eq(ins.midiProgram, 1, "EXTERNAL PROG: …and steps from there");
        dispatch.on_a_b();
        eq(ins.midiProgram, -1, "EXTERNAL PROG: A+B clears it back to OFF");

        state.instrumentCursorRow = 2; state.instrumentCursorColumn = 3;   // BANK, the 14-bit one
        dispatch.on_a_up();
        dispatch.on_a_right();
        eq(ins.midiBank, 128,
           "⚠️ EXTERNAL BANK: A+RIGHT steps a whole MSB (128), not 16 — the range is 14-bit");
        dispatch.on_a_right();
        dispatch.on_a_right();
        dispatch.on_a_right();
        dispatch.on_a_right();
        dispatch.on_a_right();
        dispatch.on_a_right();
        dispatch.on_a_right();
        eq(ins.midiBank, 1024,
           "⚠️ EXTERNAL BANK: …and it REACHES four digits — the first render drew 1024 as `00`, "
           "because a 2-hex-digit cell had silently masked a 14-bit number");

        state.instrumentCursorRow = 3; state.instrumentCursorColumn = 3;   // LEN
        dispatch.on_a_up();
        eq(ins.midiLen, 1, "EXTERNAL LEN: A+UP steps the gate length in ticks");
        dispatch.on_a_b();
        eq(ins.midiLen, 0,
           "⚠️ EXTERNAL LEN: A+B resets to 00, which is gate-to-next — a MODE, not an empty cell");

        state.instrumentCursorRow = 7; state.instrumentCursorColumn = 3;   // PAN
        dispatch.on_a_up();
        eq(ins.pan, 0x81, "EXTERNAL PAN: steps like any byte — it leaves as CC 10");

        state.instrumentCursorRow = INSTRUMENT_EXTERNAL_CC_ROW; state.instrumentCursorColumn = 1;
        dispatch.on_a_up();
        eq(ins.midiCC[0].cc, 0, "EXTERNAL CC A: A+UP on the OFF number cell inserts CC 0");
        state.instrumentCursorColumn = 3;
        dispatch.on_a_up();
        dispatch.on_a_right();
        eq(ins.midiCC[0].value, 16, "EXTERNAL CC A: …and its VALUE steps and fast-steps beside it");
        state.instrumentCursorRow = INSTRUMENT_EXTERNAL_CC_ROW + 3; state.instrumentCursorColumn = 1;
        dispatch.on_a_up();
        eq(ins.midiCC[3].cc, 0, "EXTERNAL CC D: the fourth slot is reachable — the block is 4 rows");
        eq(ins.midiCC[1].cc, -1, "EXTERNAL CC: …and the rows in between wrote their OWN slots");

        // EXTERNAL has no EQ row, and `instrument_eq_row` says so with −1 rather than a type test at
        // each call site. Row 12 does not exist on an 11-row map; nothing should open.
        state.instrumentCursorRow = 12; state.instrumentCursorColumn = 1;
        dispatch.on_button_a();   // a bare A, because the EQ cell is one that OPENS a sub-screen
        ok(!state.eq.isOpen, "⚠️ EXTERNAL: no EQ row — the sampler's row 12 opens nothing here");

        ins.instrumentType = songcore::InstrumentType::SAMPLER;
        ins.midiChannel = 0; ins.midiBank = -1; ins.midiProgram = -1; ins.midiLen = 0;
        ins.pan = 0x80;
        ins.midiCC = std::vector<songcore::MidiCcSlot>(songcore::MIDI_CC_SLOTS);
    }

    // ══ B4.3 ══ THE MIDI SCREEN — THE ROW THAT MAKES THE CABLE REACHABLE ════════════════════════
    //
    // ⚠️ **THIS BLOCK IS THE ONLY TEST THE SCREEN CAN HAVE, AND IT NEEDS A FAKE PORT TO EXIST AT ALL.**
    // MIDI out never existed in Kotlin, so there is no golden and none is possible; and the two rows
    // that matter most — OUTPUT and TEST — are the two whose real behaviour is "an OS call happened".
    // A stub `IMidiOut` is what turns those from unobservable into countable: how many opens, how many
    // closes, which bytes, in which order.
    //
    // ⭐ The claim it is really pointed at is the one the guardrails keep re-finding: **a setting that
    // round-trips is not a setting that is applied.** So every check below asks what the PORT did, not
    // what the struct holds — except the two that deliberately ask both, to show they agree.
    {
        struct FakeMidiOut : songcore::IMidiOut {
            std::vector<std::string> devices;
            std::vector<std::string> sent;      // each message as hex, in the order it left
            int  openIndex = -1;
            int  opens = 0, closes = 0;
            bool refuse = false;                // the "port is in use" case

            int         device_count() override { return static_cast<int>(devices.size()); }
            std::string device_name(int i) override { return devices[static_cast<size_t>(i)]; }
            bool        is_open() const override { return openIndex >= 0; }
            void        close() override { ++closes; openIndex = -1; }
            bool open(int i) override {
                ++opens;
                if (refuse || i < 0 || i >= static_cast<int>(devices.size())) return false;
                openIndex = i;
                return true;
            }
            void send(const uint8_t* d, int n) override {
                char        b[8];
                std::string s;
                for (int i = 0; i < n; ++i) {
                    std::snprintf(b, sizeof(b), "%02X", static_cast<unsigned>(d[i]));
                    s += b;
                }
                sent.push_back(s);
            }
        };

        FakeMidiOut port;
        port.devices = {"loopMIDI Port", "Microsoft GS Wavetable Synth"};

        state.caps          = PlatformCaps::sdl(true);
        state.confirm.close();
        state.eq            = EqEditorState{};
        state.themeEditor   = ThemeEditorState{};
        state.qwerty        = QwertyKeyboardState{};
        state.midiOut       = &port;
        state.settings.midiOutDevice = "OFF";
        state.settings.midiOffsetMs  = 0;
        // A CLEAN dirty flag to measure against: this AppState is shared with every block above, and
        // several of them left the song modified. Without this, "picking a cable does not dirty the
        // song" is a check that cannot fail — and one that cannot fail is not a check.
        state.projectVersion = state.savedProjectVersion = 100;

        songcore::Project& p = host.edit_project();

        // The gesture the user makes to reach this screen, as one call — and it MUST go through
        // PROJECT every time. Pressing A while already standing on MIDI is a different gesture
        // entirely (it is PANIC, or TEST, or nothing), so a "re-enter" that forgets to set the screen
        // back re-enumerates nothing and quietly tests the wrong thing.
        const auto enter_midi = [&] {
            state.currentScreen       = ScreenType::PROJECT;
            state.projectCursorRow    = static_cast<int>(ProjectRow::MIDI);
            state.projectCursorColumn = 1;
            dispatch.on_button_a();
        };

        // ── (a) The door: PROJECT > MIDI, and the enumeration that rides in with it ───────────────
        enter_midi();
        eq(static_cast<int>(state.currentScreen), static_cast<int>(ScreenType::MIDI),
           "MIDI: A on the PROJECT MIDI row opens the screen");
        eq(static_cast<int>(state.midiDeviceNames.size()), 3,
           "⚠️ MIDI: …and ENUMERATES on the way in — OFF plus the two ports (a list built at boot "
           "would be a list of the cables that were in at boot)");
        eqs(state.midiDeviceNames[0], "OFF", "MIDI: index 0 of the list is always OFF");
        eq(state.midiDeviceIndex, 0, "MIDI: nothing picked yet, so the row reads OFF");

        // ── (b) The cursor reaches every row, wraps, and never leaves column 1 ────────────────────
        state.midiCursorRow = 0;
        std::string walk = "0";
        for (int i = 0; i < 12; ++i) {
            dispatch.on_dpad_down();
            if (state.midiCursorRow == 0) break;
            walk += "," + std::to_string(state.midiCursorRow);
        }
        eqs(walk, "0,1,2,3,4,5,6,7",
            "MIDI rows: all eight are reachable and the eighth wraps to the first");

        state.midiCursorRow = static_cast<int>(MidiRow::OUTPUT);
        dispatch.on_dpad_right();
        dispatch.on_dpad_right();
        eq(state.midiCursorColumn, 1, "MIDI: every row but IN CH is ONE column — RIGHT does not move");
        dispatch.on_dpad_left();
        eq(state.midiCursorColumn, 1, "MIDI: …nor LEFT");

        // ── (c) OUTPUT: A+UP picks a port, and the PORT is what changes ──────────────────────────
        const int opensBefore = port.opens;
        dispatch.on_a_up();
        eq(state.midiDeviceIndex, 1, "MIDI OUTPUT: A+UP steps onto the first device");
        eqs(state.settings.midiOutDevice, "loopMIDI Port",
            "⚠️ MIDI OUTPUT: …and the SETTING stores the device NAME, never the index");
        eq(port.opens, opensBefore + 1, "⭐ MIDI OUTPUT: …and the PORT was actually OPENED");
        eq(port.openIndex, 0,
           "⚠️ MIDI OUTPUT: …at device index 0, not 1 — list index 1 is the FIRST device, because "
           "index 0 of the list is OFF and is not a device at all");
        ok(!state.project_dirty(),
           "MIDI OUTPUT: picking a cable does NOT dirty the SONG — it is settings.json's, not the .ptp's");

        // ── (d) ⭐ THE REPLUG, and the whole reason the setting is a NAME ─────────────────────────
        //
        // Reorder the port list the way an OS does when anything is plugged or unplugged, re-enter the
        // screen, and the SAME synth must still be selected — at a different index. Store an index
        // instead and this check is what goes red: it would silently name whatever took slot 1.
        port.devices = {"USB MIDI Interface", "loopMIDI Port", "Microsoft GS Wavetable Synth"};
        enter_midi();                                 // re-enter → re-enumerate
        eq(state.midiDeviceIndex, 2,
           "⭐ MIDI OUTPUT: after a REPLUG reorders the list, the same device is still selected — at "
           "its new index (this is what an index-based setting cannot do)");
        eqs(state.midiDeviceNames[static_cast<size_t>(state.midiDeviceIndex)], "loopMIDI Port",
            "MIDI OUTPUT: …and it is still the port the user chose");

        // A saved device that is simply GONE resolves to OFF, rather than to whoever took its place.
        port.devices = {"Microsoft GS Wavetable Synth"};
        enter_midi();
        eq(state.midiDeviceIndex, 0,
           "⚠️ MIDI OUTPUT: a saved device that is UNPLUGGED reads OFF — the row shows what is OPEN, "
           "never what was once wanted");
        eqs(state.settings.midiOutDevice, "loopMIDI Port",
            "MIDI OUTPUT: …and the CHOICE is kept, so plugging it back in restores it");

        // ── (e) The PANIC on a device swap — the note-offs the old cable is owed ─────────────────
        port.devices = {"loopMIDI Port", "Microsoft GS Wavetable Synth"};
        enter_midi();                                 // re-enter: the saved port is back at index 1
        // ⚠️ Re-entry ENUMERATES; it does not re-open. `refresh_midi_devices` is deliberately a pure
        // read — a screen entry that reopened the port would drop every sounding note every time the
        // user glanced at this page. So the open is asked for explicitly here, exactly as the shell's
        // boot does it.
        state.midiCursorRow = static_cast<int>(MidiRow::OUTPUT);
        dispatch.on_a_up();     // loopMIDI Port → Microsoft GS Wavetable Synth
        dispatch.on_a_down();   // …and back to the remembered choice, which OPENS it
        eq(port.openIndex, 0, "MIDI OUTPUT: (setup) the remembered port is open again");

        state.midiCursorRow = static_cast<int>(MidiRow::OUTPUT);
        const int closesBefore = port.closes;
        dispatch.on_a_up();                           // → "Microsoft GS Wavetable Synth"
        eq(port.closes, closesBefore + 1,
           "⚠️ MIDI OUTPUT: swapping the device CLOSES the old port first — `set_out` panics on a "
           "POINTER change and the pointer never changes here, so this file has to");
        eq(port.openIndex, 1, "MIDI OUTPUT: …and the new one is open");

        // ── (f) OFFSET, and the −1 that must not read as EMPTY ────────────────────────────────────
        state.midiCursorRow = static_cast<int>(MidiRow::OFFSET);
        dispatch.on_a_up();
        eq(state.settings.midiOffsetMs, 1, "MIDI OFFSET: A+UP steps it a millisecond");
        dispatch.on_a_right();
        eq(state.settings.midiOffsetMs, 11, "MIDI OFFSET: A+RIGHT jumps by ten, like TEMPO");

        // ⭐ THE NEGATIVE-CONTROL-SHAPED ONE. `cc::hex_byte`'s default empty_value is −1, and −1 is a
        // perfectly ordinary offset. Leave the default in and the context reports `isEmpty` at exactly
        // that value, A+DPAD goes dead, and the cell becomes one you can dial PAST but never away from.
        // Walking down through it is the only gesture that can tell the two apart.
        for (int i = 0; i < 13; ++i) dispatch.on_a_down();
        eq(state.settings.midiOffsetMs, -2,
           "⭐ MIDI OFFSET: A+DOWN walks THROUGH −1 to −2 — −1 is a value here, not an empty cell");
        for (int i = 0; i < 97; ++i) dispatch.on_a_down();
        eq(state.settings.midiOffsetMs, -99, "MIDI OFFSET: A+DOWN reaches the bottom of the range");
        dispatch.on_a_down();
        eq(state.settings.midiOffsetMs, 99,
           "⚠️ MIDI OFFSET: …and WRAPS to +99 rather than clamping — every hex_byte cell in the app "
           "wraps (TEMPO does too), and a range that is signed does not change that");
        state.settings.midiOffsetMs = 0;
        ok(!state.project_dirty(), "MIDI OFFSET: still not a change to the SONG");

        // ── (f2) SYNC — phase C, and the check that matters is the one about the HOST ────────────
        //
        // ⭐ **"A SETTING THAT ROUND-TRIPS IS NOT A SETTING THAT IS APPLIED — GREP THE CONSUMER, NOT
        // THE STORE."** `settings.midiSyncOut` going true is worth almost nothing on its own: it would
        // draw ON, save ON, load ON and send not one clock byte. The consumer here is
        // `SongcoreHost::midi_sync_out()`, and asserting THAT is what makes the row mean something.
        state.settings.midiSyncOut = false;
        host.set_midi_sync_out(false);
        state.midiCursorRow = static_cast<int>(MidiRow::SYNC);
        ok(!host.midi_sync_out(), "MIDI SYNC: (setup) the consumer starts off");
        dispatch.on_a_up();
        ok(state.settings.midiSyncOut, "MIDI SYNC: A+UP turns it on");
        ok(host.midi_sync_out(),
           "⭐ MIDI SYNC: …and the CONSUMER has it — the row reaches the thing that emits clock, not "
           "just the struct that remembers it");
        ok(!state.project_dirty(),
           "MIDI SYNC: not a change to the SONG — whether a drum machine is plugged into THIS desk is "
           "not something the .ptp can know");
        dispatch.on_a_down();
        ok(!state.settings.midiSyncOut, "MIDI SYNC: …and back off");
        ok(!host.midi_sync_out(), "MIDI SYNC: …the consumer follows both ways");

        // ── (g) PROG CHG — the one row here that IS the song's ───────────────────────────────────
        state.projectVersion = state.savedProjectVersion = 7;
        state.midiCursorRow  = static_cast<int>(MidiRow::PROG_CHG);
        ok(p.midiSendProgramChange, "MIDI PROG CHG: (setup) it defaults ON");
        dispatch.on_a_up();
        ok(!p.midiSendProgramChange, "MIDI PROG CHG: A+UP toggles it");
        ok(state.project_dirty(),
           "⚠️ MIDI PROG CHG: …and this one DOES dirty the song — it is a Project field and emits into "
           "the .ptp, unlike the two rows above it");
        dispatch.on_a_up();
        ok(p.midiSendProgramChange, "MIDI PROG CHG: …and back");

        // ── (h) TEST and PANIC — the two rows whose correct behaviour is otherwise SILENCE ────────
        port.sent.clear();
        state.midiCursorRow = static_cast<int>(MidiRow::TEST);
        dispatch.on_button_a();
        eq(static_cast<int>(port.sent.size()), 2, "MIDI TEST: A sends exactly two messages");
        // ⚠️ Read through a bounds-safe accessor, not `sent[0]`. A harness that SEGFAULTS on the very
        // failure it is checking for reports nothing at all — the count assertion above would have been
        // printed and then thrown away with the process. (It was: this is what the first run did.)
        const auto msg = [&](size_t i) { return i < port.sent.size() ? port.sent[i] : std::string("-"); };
        eqs(msg(0), "903C64", "MIDI TEST: …a note-on, C-4 (60) on channel 1 at velocity 100");
        eqs(msg(1), "803C00",
            "⚠️ MIDI TEST: …and its note-off in the same breath — a sustained TEST would be the one "
            "note in the app with no transport to stop it");
        eqs(state.midiStatusText, "TEST SENT",
            "⭐ MIDI TEST: …and it SAYS so — a handler whose success is silence cannot be told from one "
            "that never ran");

        // The negative control, and it is the point of the readout: with no port the SAME press must
        // reach the SAME row and report differently, not merely do nothing.
        port.close();
        port.sent.clear();
        dispatch.on_button_a();
        eq(static_cast<int>(port.sent.size()), 0, "MIDI TEST: with no port open, nothing is sent");
        eqs(state.midiStatusText, "NO PORT",
            "⭐ MIDI TEST: …and the screen distinguishes 'no cable' from 'it worked'");

        state.midiCursorRow = static_cast<int>(MidiRow::PANIC);
        dispatch.on_button_a();
        eqs(state.midiStatusText, "NO PORT", "MIDI PANIC: same distinction, same row shape");

        // ── (i) A port that refuses to open keeps the user's choice ──────────────────────────────
        // From a KNOWN cell, not from wherever the last section left the cursor: OUTPUT is a WRAPPING
        // cycle, so "step up from the current index" lands on OFF when the current index happens to be
        // the last device — and a check that reads OFF is measuring the wrap, not the refusal.
        port.refuse = true;
        state.settings.midiOutDevice = "OFF";
        enter_midi();
        eq(state.midiDeviceIndex, 0, "MIDI OUTPUT: (setup) starting from OFF");
        state.midiCursorRow = static_cast<int>(MidiRow::OUTPUT);
        dispatch.on_a_up();          // → the first real device, which will refuse
        ok(!port.is_open(), "MIDI OUTPUT: (setup) the port refused — another app holds it");
        eqs(state.midiStatusText, "PORT BUSY", "MIDI OUTPUT: a refused open says so");
        ok(state.settings.midiOutDevice != "OFF",
           "⚠️ MIDI OUTPUT: …and the CHOICE survives the failure — a busy port is a transient the user "
           "can fix and retry, not a reason to throw their pick away");
        port.refuse = false;

        // ── (j) The QUIT-AND-RELAUNCH channel: both settings must survive a round trip ───────────
        //
        // ⚠️ The blind channel S9's theme-by-name bug lived in. Neither the screen nor the dispatcher
        // can see it: the value is correct in memory, correct on the screen, and gone on the next launch.
        state.settings.midiOutDevice = "Microsoft GS Wavetable Synth";
        state.settings.midiOffsetMs  = -37;
        state.settings.midiSyncOut   = true;
        ok(save_settings(fs_impl, state.settings, state.theme), "MIDI: settings.json written");
        {
            SettingsValues back{};
            Theme          backTheme = theme_classic();
            ok(load_settings(fs_impl, back, backTheme), "MIDI: settings.json read back");
            eqs(back.midiOutDevice, "Microsoft GS Wavetable Synth",
                "⭐ MIDI: the device NAME survives a quit and relaunch");
            eq(back.midiOffsetMs, -37, "⭐ MIDI: …and so does the OFFSET, sign and all");
            ok(back.midiSyncOut, "⭐ MIDI: …and SYNC (phase C)");
            // ⚠️ The other half, and it is a DIFFERENT fact from the round trip: every settings.json
            // written before phase C has no `midi_sync_out` key at all. `load_settings` reads every key
            // as `get_bool(j, key, values.<field>)`, so an absent key yields whatever the struct was
            // initialised to — which makes the struct's default the operative value for every existing
            // file, and it must be OFF. (Stated rather than implied: this asserts the default, not the
            // parse; the parse of a PRESENT key is what the round trip above covers.)
            SettingsValues fresh{};
            ok(!fresh.midiSyncOut,
               "⭐ MIDI SYNC: the default is OFF, so a settings.json from before phase C stays OFF");
        }
        state.settings.midiSyncOut = false;

        // ── (k) boot_midi_port — the shell's one call, and the OFFSET it must not forget ─────────
        {
            AppState boot;
            boot.project  = &host.edit_project();
            boot.caps     = PlatformCaps::sdl(true);
            boot.midiOut  = &port;
            boot.settings.midiOutDevice = "Microsoft GS Wavetable Synth";
            boot.settings.midiOffsetMs  = -37;
            port.close();

            InputDispatcher bootDispatch(boot, host, fs_impl);
            bootDispatch.boot_midi_port();

            ok(port.is_open(), "MIDI boot: the port the settings NAME is opened at launch");
            eq(port.openIndex, 1, "MIDI boot: …and it is the right one");
            eq(host.midi_out().offset_ms(), -37,
               "⭐ MIDI boot: …and the OFFSET reached the CONSUMER — a value correct in settings.json, "
               "correct on the screen and never pushed is the exact bug this asks about");
        }

        // ── (l) B is the way out ─────────────────────────────────────────────────────────────────
        state.currentScreen = ScreenType::MIDI;
        state.midiReturnScreen = ScreenType::SONG;
        dispatch.on_button_b();
        eq(static_cast<int>(state.currentScreen), static_cast<int>(ScreenType::SONG),
           "MIDI: B returns to where the screen was opened from");

        // Leave the harness as it was found — later blocks share this AppState.
        state.midiOut = nullptr;
        host.set_midi_offset_ms(0);
        state.settings.midiOutDevice = "OFF";
        state.settings.midiOffsetMs  = 0;
    }

    // ══ E2 ══ THE MIDI **INPUT** PORT — OPENED AT BOOT, AND WIRED TO THE HOST'S QUEUE ═════════════
    //
    // ⚠️ **THE ROW THAT PICKS THIS IS E3's; THE OPEN IS ALREADY REAL, AND THAT IS THE POINT OF TESTING
    // IT NOW.** `settings.midiInDevice` could have been left round-tripping with no consumer until the
    // screen existed — which is the trap this repo has fallen into twice (`midiInputChannels` sat unread
    // from B1 to E1). It is read at boot instead, and these checks ask what the PORT did.
    //
    // ⭐⭐ And they ask one thing nothing else in the tree can: **that the SINK was wired.** An input port
    // that opens without one is a port that receives bytes and drops every single one — the purest form
    // of "a component whose correct behaviour is silence", indistinguishable from an unplugged cable
    // from every seat in the app. The fake port below can therefore DELIVER, and the assertion is that
    // the bytes arrived in the host.
    {
        struct FakeMidiIn : songcore::IMidiIn {
            std::vector<std::string> devices;
            songcore::IMidiInSink*   sink = nullptr;
            int  openIndex = -1;
            int  opens = 0, closes = 0, sinkSets = 0, sinkClears = 0;
            bool refuse = false;

            int         device_count() override { return static_cast<int>(devices.size()); }
            std::string device_name(int i) override { return devices[static_cast<size_t>(i)]; }
            bool        is_open() const override { return openIndex >= 0; }
            void        close() override { ++closes; openIndex = -1; }
            bool open(int i) override {
                ++opens;
                if (refuse || i < 0 || i >= static_cast<int>(devices.size())) return false;
                openIndex = i;
                return true;
            }
            void set_sink(songcore::IMidiInSink* s) override {
                if (s) ++sinkSets; else ++sinkClears;
                sink = s;
            }
            /** What a backend's callback does — the only way to prove the sink is more than a pointer. */
            void deliver(const uint8_t* d, int n) { if (sink) sink->on_bytes(d, n); }
        };

        FakeMidiIn in;
        in.devices = {"loopMIDI Port", "USB MIDI Keyboard"};

        AppState boot;
        boot.project = &host.edit_project();
        boot.caps    = PlatformCaps::sdl(true);
        boot.midiIn  = &in;
        boot.settings.midiInDevice = "USB MIDI Keyboard";

        InputDispatcher bootDispatch(boot, host, fs_impl);
        bootDispatch.boot_midi_in_port();

        eq(static_cast<int>(boot.midiInDeviceNames.size()), 3,
           "MIDI IN: boot ENUMERATES — OFF plus the two input ports (a separate list from OUTPUT's, "
           "because winmm, ALSA and MidiManager all enumerate the two directions separately)");
        ok(in.is_open(), "MIDI IN: the port the settings NAME is opened at launch");
        eq(in.openIndex, 1,
           "⚠️ MIDI IN: …at device index 1, not 2 — list index 0 is OFF and is not a device");
        ok(in.sink != nullptr, "⭐⭐ MIDI IN: …and the SINK was wired (a port with none drops every byte)");

        // ⭐ The sink is not merely non-null: it is the HOST's queue. Deliver as a backend's callback
        // does and the bytes must turn up in the host — the join E3's row will never re-check.
        const uint64_t before = host.midi_in_bytes();
        const uint8_t  wire[3] = {0x90, 0x3C, 0x40};
        in.deliver(wire, 3);
        host.poll();
        eq(static_cast<int>(host.midi_in_bytes() - before), 3,
           "⭐⭐ MIDI IN: bytes delivered on the backend's thread reach the HOST's queue and are drained");

        // A saved device that is GONE reads OFF and opens nothing — the OUTPUT row's rule, and the
        // reason both settings are NAMES: an index would silently open whatever took the slot.
        {
            FakeMidiIn gone;
            gone.devices = {"Some Other Interface"};
            AppState b2;
            b2.project = &host.edit_project();
            b2.caps    = PlatformCaps::sdl(true);
            b2.midiIn  = &gone;
            b2.settings.midiInDevice = "USB MIDI Keyboard";
            InputDispatcher d2(b2, host, fs_impl);
            d2.boot_midi_in_port();
            eq(b2.midiInDeviceIndex, 0, "MIDI IN: an unplugged saved device resolves to OFF");
            eq(gone.opens, 0, "⭐ MIDI IN: …and nothing is opened at all — not the port that took its place");
        }

        // A build with NO input backend (Linux and Android until E5) must boot, not crash, and say OFF.
        {
            AppState b3;
            b3.project = &host.edit_project();
            b3.caps    = PlatformCaps::sdl(true);
            b3.midiIn  = nullptr;
            b3.settings.midiInDevice = "USB MIDI Keyboard";
            InputDispatcher d3(b3, host, fs_impl);
            d3.boot_midi_in_port();
            eq(static_cast<int>(b3.midiInDeviceNames.size()), 1,
               "MIDI IN: with no backend the list is OFF alone");
            eq(b3.midiInDeviceIndex, 0, "MIDI IN: …and the row reads OFF rather than lying");
        }

        // The QUIT-AND-RELAUNCH channel, the one neither the screen nor the dispatcher can see.
        state.settings.midiInDevice = "USB MIDI Keyboard";
        ok(save_settings(fs_impl, state.settings, state.theme), "MIDI IN: settings.json written");
        {
            SettingsValues back{};
            Theme          backTheme = theme_classic();
            ok(load_settings(fs_impl, back, backTheme), "MIDI IN: settings.json read back");
            eqs(back.midiInDevice, "USB MIDI Keyboard",
                "⭐ MIDI IN: the device NAME survives a quit and relaunch");
            SettingsValues fresh{};
            eqs(fresh.midiInDevice, "OFF",
                "⭐ MIDI IN: and a settings.json from before phase E has no key at all, so it stays OFF "
                "— an app that opened whatever keyboard it found would hold a port nobody asked it to");
        }

        // ⚠️ The port must be unwired before the fakes leave scope: the host's queue outlives them here,
        // and in the app it is the other way round (app.cpp closes the port before `run` returns).
        in.set_sink(nullptr);
        in.close();
        state.settings.midiInDevice = "OFF";
        host.reset_midi_in();
    }

    // ══ E3 ══ THE **INPUT ROW** AND THE **PER-TRACK CHANNEL MAP** ═════════════════════════════════
    //
    // E2 opened an input port from a settings key nobody could edit and routed on a `.ptp` field nobody
    // could type. E3 is the two rows that end that — and the checks below are in two halves that ask
    // very different questions:
    //
    //   • INPUT is the OUTPUT row's twin, so its checks are OUTPUT's: the setting is a NAME, the port
    //     is really opened, the SINK goes with the open (E2's one-operation rule), and picking a cable
    //     does not dirty the SONG.
    //   • IN CH is not like any row this screen had. It is eight cells on one row, it edits a PROJECT
    //     field, and — the point of the whole increment — ⭐⭐ **the number in the cell has to reach the
    //     ROUTER.** "A setting that round-trips is not a setting that is applied": a map that draws and
    //     saves correctly while every key still lands on whatever track the old value named would look
    //     perfect from every seat in this file except the last one.
    {
        struct FakeMidiIn : songcore::IMidiIn {
            std::vector<std::string> devices;
            songcore::IMidiInSink*   sink = nullptr;
            int  openIndex = -1;
            int  opens = 0, closes = 0;

            int         device_count() override { return static_cast<int>(devices.size()); }
            std::string device_name(int i) override { return devices[static_cast<size_t>(i)]; }
            bool        is_open() const override { return openIndex >= 0; }
            void        close() override { ++closes; openIndex = -1; }
            bool open(int i) override {
                ++opens;
                if (i < 0 || i >= static_cast<int>(devices.size())) return false;
                openIndex = i;
                return true;
            }
            void set_sink(songcore::IMidiInSink* s) override { sink = s; }
            void deliver(const uint8_t* d, int n) { if (sink) sink->on_bytes(d, n); }
        };

        songcore::Project& p = host.edit_project();

        FakeMidiIn in;
        in.devices = {"loopMIDI Port", "USB MIDI Keyboard"};
        state.midiIn = &in;
        state.settings.midiInDevice = "OFF";
        host.reset_midi_in();

        const auto enter_midi = [&] {
            state.currentScreen       = ScreenType::PROJECT;
            state.projectCursorRow    = static_cast<int>(ProjectRow::MIDI);
            state.projectCursorColumn = 1;
            dispatch.on_button_a();
        };

        // ── (a) The INPUT row enumerates on the way in, alongside OUTPUT's list ───────────────────
        //
        // ⚠️ TWO LISTS, NOT ONE. A machine's inputs and outputs are different sets — this desk had two
        // outputs and ZERO inputs until loopMIDI was installed — so a screen that refreshed one of them
        // would show a stale INPUT row for as long as the user stayed on it.
        enter_midi();
        eq(static_cast<int>(state.midiInDeviceNames.size()), 3,
           "⭐ MIDI INPUT: entering the screen enumerates the INPUT ports too — OFF plus the two");
        eqs(state.midiInDeviceNames[0], "OFF", "MIDI INPUT: index 0 of that list is always OFF");
        eq(state.midiInDeviceIndex, 0, "MIDI INPUT: nothing picked yet, so the row reads OFF");

        // ── (b) A+UP picks a port — and the port OPENS, with its sink ────────────────────────────
        state.projectVersion = state.savedProjectVersion = 11;
        state.midiCursorRow  = static_cast<int>(MidiRow::INPUT);
        dispatch.on_a_up();
        eq(state.midiInDeviceIndex, 1, "MIDI INPUT: A+UP steps onto the first input device");
        eqs(state.settings.midiInDevice, "loopMIDI Port",
            "⚠️ MIDI INPUT: …and the SETTING stores the device NAME, never the index");
        ok(in.is_open(), "⭐ MIDI INPUT: …and the PORT was actually OPENED");
        eq(in.openIndex, 0,
           "⚠️ MIDI INPUT: …at device index 0, not 1 — list index 1 is the FIRST device");
        ok(in.sink != nullptr,
           "⭐⭐ MIDI INPUT: …and the SINK was wired by the same call (E2's rule: a port opened without "
           "one receives bytes and drops every single one, which looks exactly like an unplugged cable)");
        ok(!state.project_dirty(),
           "MIDI INPUT: picking a cable does NOT dirty the SONG — settings.json's, not the .ptp's");

        // ── (c) IN CH: eight cells on one row, and the cursor has to reach all of them ───────────
        state.midiCursorRow    = static_cast<int>(MidiRow::IN_MAP);
        state.midiCursorColumn = 1;
        std::string cols = "1";
        for (int i = 0; i < 12; ++i) {
            dispatch.on_dpad_right();
            cols += "," + std::to_string(state.midiCursorColumn);
        }
        eqs(cols, "1,2,3,4,5,6,7,8,8,8,8,8,8",
            "⭐ MIDI IN CH: RIGHT walks all eight track cells and then CLAMPS — columns clamp where "
            "rows wrap, which is this app's rule everywhere (cursor_move.h)");
        for (int i = 0; i < 9; ++i) dispatch.on_dpad_left();
        eq(state.midiCursorColumn, 1, "MIDI IN CH: …and LEFT walks back and clamps at the first");

        // ⚠️ …and stepping OFF the row must drop the column, or `cursor_context` would answer for a
        // cell the screen is not drawing.
        state.midiCursorColumn = 6;
        dispatch.on_dpad_down();
        eq(state.midiCursorColumn, 1,
           "⚠️ MIDI IN CH: leaving the row resets the column — the rows below it have only one");
        dispatch.on_dpad_up();
        eq(state.midiCursorRow, static_cast<int>(MidiRow::IN_MAP), "MIDI IN CH: (back on the row)");

        // ── (d) The three states of a cell: off, dialled, and cleared ────────────────────────────
        state.projectVersion = state.savedProjectVersion = 13;
        state.midiCursorColumn = 3;                       // track 3 → index 2
        p.midiInputChannels.assign(8, -1);
        eq(p.midiInputChannels[2], -1, "MIDI IN CH: (setup) track 3 listens to nothing");
        dispatch.on_a_up();
        eq(p.midiInputChannels[2], 0,
           "⚠️ MIDI IN CH: A+UP on an empty cell lands on channel 0 — STORED 0-15, and the screen is "
           "the only place the +1 happens (the instrument screen's CHAN row makes the same promise)");
        ok(state.project_dirty(),
           "⚠️ MIDI IN CH: …and it DIRTIES THE SONG — `midiInputChannels` is a Project field that emits "
           "into the .ptp, unlike the cable rows above");
        for (int i = 0; i < 4; ++i) dispatch.on_a_up();
        eq(p.midiInputChannels[2], 4, "MIDI IN CH: A+UP walks up the sixteen channels");
        dispatch.on_a_b();
        eq(p.midiInputChannels[2], -1,
           "⭐ MIDI IN CH: A+B clears the cell back to −1 — the project's ONE empty convention, and the "
           "only way to say 'this track listens to nothing'");

        // ⭐ THE CONTROL-SHAPED ONE, and it is the OFFSET row's lesson in a new cell. Channel 0 is a
        // REAL channel here (drawn `01`), so the empty value must be −1 and not 0: leave `hex_byte`'s
        // `empty_value` at its default and A+B on track 1's most likely setting does nothing at all.
        p.midiInputChannels[2] = 0;
        dispatch.on_a_b();
        eq(p.midiInputChannels[2], -1,
           "⭐ MIDI IN CH: …including from channel 0, which is a value and not an empty cell");
        dispatch.on_a_down();
        eq(p.midiInputChannels[2], -1,
           "⚠️ MIDI IN CH: A+DOWN on an EMPTY cell is inert — an empty cell is entered with A+UP "
           "(INSERT_DEFAULT) and with nothing else, which is cursor.h's rule for every deletable cell "
           "in the app, not something this row decides");

        // ── (e) ⭐⭐ THE CLAIM ONLY THIS INCREMENT CAN MAKE: THE CELL REACHES THE ROUTER ───────────
        //
        // Everything above is the row remembering a number. This is the row MEANING something: map a
        // track to a channel WITH THE D-PAD, then push real bytes through the backend the same way a
        // keyboard would, and ask which track the record came out on.
        //
        // ⚠️ It is deliberately driven end to end — `on_a_up` rather than a write to the vector —
        // because the question is whether the SCREEN's edit is the one the router reads. A test that
        // sets the field directly would pass with the whole row deleted.
        p.midiInputChannels.assign(8, -1);
        struct RouteSpy : songcore::IMidiInObserver {
            int lastTrack = -99, records = 0;
            void on_midi_in(const songcore::MidiInMessage&, const songcore::Event* ev, int n) override {
                records += (n > 0 ? n : 0);
                if (n > 0) lastTrack = ev[0].track;
            }
        } spy;
        host.set_midi_in_observer(&spy);
        // ⚠️ THE FALLBACK INSTRUMENT IS THE SHELL'S JOB and it is pushed every frame (app.cpp), so a
        // headless harness has to do it by hand — without it `input_instrument` answers −1, every record
        // is dropped as `noInstrument`, and the routing checks below would read as "the map does not
        // work" when the map is fine. E1's design note: a correctly configured keyboard must not be
        // silent on a stopped song, and this is the value that makes it so.
        host.set_midi_in_instrument(0);

        state.midiCursorRow    = static_cast<int>(MidiRow::IN_MAP);
        state.midiCursorColumn = 5;                       // track 5 → index 4
        for (int i = 0; i < 4; ++i) dispatch.on_a_up();   // → channel 3, drawn "04"
        eq(p.midiInputChannels[4], 3, "MIDI IN CH: (setup) track 5 now listens to channel 3");

        const uint8_t keyOn[3] = {0x93, 0x40, 0x64};      // note-on, CHANNEL 3, E-4, mf
        in.deliver(keyOn, 3);
        host.poll();
        eq(spy.records, 1, "⭐⭐ MIDI IN CH: a key on channel 3 produces exactly one bus record");
        eq(spy.lastTrack, 4,
           "⭐⭐ MIDI IN CH: …ON TRACK 5 — the number the D-PAD put in the cell is the number the ROUTER "
           "read. This is the whole increment: without it the row draws, saves and reloads perfectly "
           "while every key lands wherever the old map said");

        // The other half of the same claim, and it is a different fact: a channel NOBODY maps must
        // produce nothing. Without it, "routed to track 5" could just as well be "routes everything".
        spy.records = 0; spy.lastTrack = -99;
        const uint8_t strayOn[3] = {0x97, 0x40, 0x64};    // channel 7 — mapped by no track
        in.deliver(strayOn, 3);
        host.poll();
        eq(spy.records, 0,
           "⭐ MIDI IN CH: …and a key on an UNMAPPED channel produces no record at all — the map is a "
           "filter, not a default");
        ok(host.midi_in_router().unmapped() > 0,
           "MIDI IN CH: …counted as `unmapped`, so the console can say WHICH of the four silences it is");
        host.set_midi_in_observer(nullptr);

        // ── (f) The QUIT-AND-RELAUNCH channel — but for the .ptp, not settings.json ──────────────
        //
        // ⚠️ A DIFFERENT FILE FROM EVERY OTHER ROW ON THIS SCREEN. `midiInputChannels` is emitted
        // default-guarded (project_io.h), which is exactly the shape that stays invisible when it
        // breaks: the eight goldens keep matching byte for byte because they have no map, and a song
        // that HAS one silently loses it. So the map has to survive a serialize/parse of its own.
        p.midiInputChannels.assign(8, -1);
        p.midiInputChannels[0] = 9;
        p.midiInputChannels[7] = 0;
        {
            const std::string blob = songcore::serialize_project(p);
            ok(blob.find("midiInputChannels") != std::string::npos,
               "⭐ MIDI IN CH: a map that is not all-empty is EMITTED into the .ptp");
            const songcore::Project back = songcore::parse_project(songcore::json::parse(blob));
            eq(back.midiInputChannels[0], 9, "⭐ MIDI IN CH: …and track 1's channel comes back");
            eq(back.midiInputChannels[7], 0,
               "⭐ MIDI IN CH: …including channel 0, which a truthy default-guard would drop");
            eq(back.midiInputChannels[3], -1, "MIDI IN CH: …and an unmapped track stays unmapped");
        }
        {
            // The guard itself: an all-empty map writes NO key, which is what keeps the eight goldens
            // byte-identical. (`ptroundtrip` proves they are; this says WHY.)
            songcore::Project clean;
            ok(songcore::serialize_project(clean).find("midiInputChannels") == std::string::npos,
               "⚠️ MIDI IN CH: an all-empty map emits nothing — the default guard the goldens rest on");
        }

        // Leave the harness as it was found.
        p.midiInputChannels.assign(8, -1);
        in.set_sink(nullptr);
        in.close();
        state.midiIn = nullptr;
        state.settings.midiInDevice = "OFF";
        state.midiCursorColumn = 1;
        host.reset_midi_in();
    }

    // ══ E4 ══ THE **THRU VERDICT** — the one decision that reads BOTH device rows ══════════════════
    //
    // E4 injects a live key into the engine, and a key on a track whose instrument is EXTERNAL goes out
    // on the cable (MIDI thru). ⚠️ **On a LOOPBACK port that is an amplifying feedback loop** — key →
    // cable out → the same port → key — and a loopback is not exotic here: it is the only MIDI-in test
    // rig this project has. So the dispatcher compares the two OPEN ports and turns thru off when they
    // are the same device.
    //
    // ⭐ **THE CHECKS ARE HALF ABOUT THE ARMS NOBODY WOULD WRITE.** The ON→OFF transition is the easy
    // one and it is one line; what breaks in six months is the way BACK — changing either row away from
    // the loopback has to restore thru, from a function each row must remember to call. That is the
    // "derive it from the data, or write it once below the sites" rule, and these are the assertions
    // that hold it: ⚠️ the verdict is asserted after a change to the OUT row, the IN row, and both
    // being OFF, because a predicate that only ever answers "same" is also satisfied by two blanks.
    {
        struct FakeMidiOut2 : songcore::IMidiOut {
            std::vector<std::string> devices;
            int openIndex = -1;
            int         device_count() override { return static_cast<int>(devices.size()); }
            std::string device_name(int i) override { return devices[static_cast<size_t>(i)]; }
            bool        is_open() const override { return openIndex >= 0; }
            void        close() override { openIndex = -1; }
            bool open(int i) override {
                if (i < 0 || i >= static_cast<int>(devices.size())) return false;
                openIndex = i;
                return true;
            }
            void send(const uint8_t*, int) override {}
        };
        struct FakeMidiIn2 : songcore::IMidiIn {
            std::vector<std::string> devices;
            songcore::IMidiInSink*   sink = nullptr;
            int openIndex = -1;
            int         device_count() override { return static_cast<int>(devices.size()); }
            std::string device_name(int i) override { return devices[static_cast<size_t>(i)]; }
            bool        is_open() const override { return openIndex >= 0; }
            void        close() override { openIndex = -1; }
            bool open(int i) override {
                if (i < 0 || i >= static_cast<int>(devices.size())) return false;
                openIndex = i;
                return true;
            }
            void set_sink(songcore::IMidiInSink* s) override { sink = s; }
        };

        // ⚠️ The IN list's FIRST device shares a NAME with the OUT list's first — that is what a
        // loopback looks like from the app: winmm hands out "loopMIDI Port" in both directions. The
        // second entries differ, which is what a keyboard-plus-synth rig looks like.
        FakeMidiOut2 outPort;
        outPort.devices = {"loopMIDI Port", "Microsoft GS Wavetable Synth"};
        FakeMidiIn2 inPort;
        inPort.devices = {"loopMIDI Port", "USB MIDI Keyboard"};

        state.midiOut = &outPort;
        state.midiIn  = &inPort;
        state.settings.midiOutDevice = "OFF";
        state.settings.midiInDevice  = "OFF";

        const auto enter_midi = [&] {
            state.currentScreen       = ScreenType::PROJECT;
            state.projectCursorRow    = static_cast<int>(ProjectRow::MIDI);
            state.projectCursorColumn = 1;
            dispatch.on_button_a();
        };
        enter_midi();

        // ── (a) Two ports OFF is not a loopback ───────────────────────────────────────────────────
        //
        // ⚠️ Index 0 of BOTH lists is the string "OFF", so a verdict that compared names without asking
        // whether a port is OPEN would declare the commonest state in the app a feedback loop and
        // silently disable thru for every user who has never picked a cable. It costs one clause and
        // it is the arm most likely to be dropped in a rewrite.
        ok(host.midi_in_thru(), "⭐ MIDI THRU: two ports OFF is not a loopback — thru stays ON");

        // ── (b) Two DIFFERENT devices: the rig thru exists for ────────────────────────────────────
        state.midiCursorRow = static_cast<int>(MidiRow::OUTPUT);
        dispatch.on_a_up();                                   // OUT → loopMIDI Port
        state.midiCursorRow = static_cast<int>(MidiRow::INPUT);
        dispatch.on_a_up();
        dispatch.on_a_up();                                   // IN  → USB MIDI Keyboard
        eqs(state.settings.midiInDevice, "USB MIDI Keyboard", "MIDI THRU: (setup) a keyboard on IN");
        eqs(state.settings.midiOutDevice, "loopMIDI Port", "MIDI THRU: (setup) a synth on OUT");
        ok(host.midi_in_thru(),
           "⭐ MIDI THRU: a keyboard IN and a different port OUT — thru ON, which is the feature");

        // ── (c) ⭐⭐ THE SAME DEVICE ON BOTH ROWS: the loop, caught at the pick ─────────────────────
        state.midiCursorRow = static_cast<int>(MidiRow::INPUT);
        dispatch.on_a_down();                                 // IN → loopMIDI Port, same as OUT
        eqs(state.settings.midiInDevice, "loopMIDI Port", "MIDI THRU: (setup) IN is now the loopback");
        ok(!host.midi_in_thru(),
           "⭐⭐ MIDI THRU: IN and OUT are the SAME port — thru is turned OFF");
        eqs(state.midiStatusText, "THRU OFF: LOOP",
            "⭐ MIDI THRU: …and the screen SAYS so, where the pick was made");

        // ── (d) …and the way BACK, from the OTHER row ────────────────────────────────────────────
        //
        // ⭐ This is the assertion the design is actually about. The verdict lives in one function
        // called from both `apply_midi_device` and `apply_midi_in_device`; a version that only called
        // it from the INPUT row would pass every check above and leave thru dead for the rest of the
        // session the moment a user changed their OUTPUT port.
        state.midiCursorRow = static_cast<int>(MidiRow::OUTPUT);
        dispatch.on_a_up();                                   // OUT → the GS synth
        eqs(state.settings.midiOutDevice, "Microsoft GS Wavetable Synth",
            "MIDI THRU: (setup) OUT moved to a different device");
        ok(host.midi_in_thru(),
           "⭐⭐ MIDI THRU: changing the OUT row away from the loopback turns thru back ON");

        // ── (e) A closed OUT port cannot loop either ─────────────────────────────────────────────
        state.midiCursorRow = static_cast<int>(MidiRow::OUTPUT);
        dispatch.on_a_down();                                 // back to loopMIDI…
        ok(!host.midi_in_thru(), "MIDI THRU: (setup) back on the loopback, thru OFF again");
        dispatch.on_a_down();                                 // …and off entirely
        eqs(state.settings.midiOutDevice, "OFF", "MIDI THRU: (setup) OUT is OFF");
        ok(host.midi_in_thru(),
           "⭐ MIDI THRU: with the OUTPUT port closed there is nothing to loop — thru ON");

        // Leave the harness as it was found.
        inPort.set_sink(nullptr);
        inPort.close();
        outPort.close();
        state.midiIn  = nullptr;
        state.midiOut = nullptr;
        state.settings.midiInDevice  = "OFF";
        state.settings.midiOutDevice = "OFF";
        state.midiStatusText.clear();
        host.set_midi_in_thru(true);
        host.reset_midi_in();
    }

    // ── 43. THE MIDI SURFACES ARE GATED ON THE BUILD — and the DATA is not ───────────────────────
    //
    // A build with `PlatformCaps::midi` off cannot AUTHOR MIDI: no PROJECT > MIDI row (and so no way
    // to the MIDI screen at all), no EXTERNAL on the instrument TYPE cell, and no MIDI commands in the
    // FX column. It still DISPLAYS every one of them, because all three are persisted in a .ptp and a
    // build that drew them as something else would be lying about the file on disk — an EXTERNAL
    // instrument shown as a sampler is a track the engine correctly refuses to voice, with nothing on
    // screen to say why.
    //
    // ⚠️ EVERY CLAIM IS MADE TWICE, ONCE UNDER EACH CAP. A gate asserted only in the build that has it
    // off is indistinguishable from a feature that never worked: the `midi` half is the control, and it
    // is what proves the `no-midi` half is measuring the CAP rather than something that was always
    // true. The two halves must disagree on every line below or one of them is not running.
    {
        state.caps = PlatformCaps::sdl(true);
        auto& proj = host.edit_project();

        // ── (a) The PROJECT row, and the cursor walk over the hole it leaves ──────────────────────
        ok(project_row_visible(ProjectRow::MIDI, state.caps),
           "MIDI GATE [midi]: PROJECT draws the MIDI row");
        eq(project_row_count(state.caps), 9, "MIDI GATE [midi]: nine PROJECT rows");

        state.caps = PlatformCaps::sdl(false);
        ok(!project_row_visible(ProjectRow::MIDI, state.caps),
           "⭐ MIDI GATE [no-midi]: PROJECT does NOT draw the MIDI row");
        eq(project_row_count(state.caps), 8, "MIDI GATE [no-midi]: eight PROJECT rows");

        // ⚠️ EXIT KEEPS THE NUMBER 8. The row map is not compacted — the cursor, the tools and the
        // recorded PROJECT cases all speak in these values, so the walk SKIPS row 7 rather than
        // renumbering what follows it. A build that closed the gap would pass "MIDI is not drawn" and
        // silently move every row below it.
        eq(static_cast<int>(project_last_row(state.caps)), 8,
           "⭐ MIDI GATE [no-midi]: EXIT is still row 8 — the map is skipped, not renumbered");

        state.currentScreen       = ScreenType::PROJECT;
        state.projectCursorRow    = static_cast<int>(ProjectRow::SYSTEM);
        state.projectCursorColumn = 1;
        dispatch.on_dpad_down();
        eq(state.projectCursorRow, 8,
           "⭐ MIDI GATE [no-midi]: DOWN off SYSTEM jumps the hidden MIDI row and lands on EXIT");
        dispatch.on_dpad_up();
        eq(state.projectCursorRow, 6, "MIDI GATE [no-midi]: …and UP comes back to SYSTEM, not to MIDI");

        state.caps             = PlatformCaps::sdl(true);
        state.projectCursorRow = static_cast<int>(ProjectRow::SYSTEM);
        dispatch.on_dpad_down();
        eq(state.projectCursorRow, 7, "MIDI GATE [midi]: DOWN off SYSTEM lands on MIDI (the control)");

        // ── (b) A on the MIDI row cannot open the screen ──────────────────────────────────────────
        //
        // The row is the screen's only door, so this is the gate that matters. Driven with the cursor
        // parked on row 7 in a build that never draws it — which is exactly the stale-cursor case a
        // settings file written by a debug build produces.
        state.caps             = PlatformCaps::sdl(false);
        state.currentScreen    = ScreenType::PROJECT;
        state.projectCursorRow = static_cast<int>(ProjectRow::MIDI);
        dispatch.on_button_a();
        ok(state.currentScreen == ScreenType::PROJECT,
           "⭐⭐ MIDI GATE [no-midi]: A on the (undrawn) MIDI row does NOT open the MIDI screen");

        state.caps             = PlatformCaps::sdl(true);
        state.currentScreen    = ScreenType::PROJECT;
        state.projectCursorRow = static_cast<int>(ProjectRow::MIDI);
        dispatch.on_button_a();
        ok(state.currentScreen == ScreenType::MIDI,
           "MIDI GATE [midi]: …and with the cap on it DOES (the control — the gate is the cap)");
        dispatch.on_button_b();   // back to PROJECT

        // ── (c) The instrument TYPE cell stops one type short ─────────────────────────────────────
        // ⚠️ On an EMPTY slot. `request_instrument_type_toggle` opens a CONFIRM before changing the
        // type of an instrument that owns a source file — so on a loaded slot A+UP arms a dialog and
        // every press after it is swallowed by the modal rule, and the walk below would read as "the
        // type never changes" under BOTH caps. That is a check that cannot fail, which is worse than
        // one that does: it was the first draft here, and it "passed" the gate for the wrong reason.
        state.currentScreen           = ScreenType::INSTRUMENT;
        state.currentInstrument       = 7;
        state.instrumentCursorRow     = 0;
        state.instrumentCursorColumn  = 1;
        host.clear_instrument(7);
        ok(state.confirm.kind == ConfirmDialogState::Kind::NONE,
           "MIDI GATE: (setup) an empty slot — no confirm stands in the way");
        auto type_of = [&] {
            return host.project().instruments[7].instrumentType;
        };
        auto walk_types = [&](int steps) {
            std::string seen;
            for (int i = 0; i < steps; ++i) {
                dispatch.on_a_up();
                seen += songcore::instrument_type_name(type_of());
                seen += " ";
            }
            return seen;
        };

        state.caps = PlatformCaps::sdl(false);
        host.set_instrument_type(7, songcore::InstrumentType::SAMPLER);
        eqs(walk_types(4), "SOUNDFONT SAMPLER SOUNDFONT SAMPLER ",
            "⭐⭐ MIDI GATE [no-midi]: A+UP on TYPE cycles SAMPLER↔SOUNDFONT — EXTERNAL is unreachable");

        state.caps = PlatformCaps::sdl(true);
        host.set_instrument_type(7, songcore::InstrumentType::SAMPLER);
        eqs(walk_types(4), "SOUNDFONT EXTERNAL SAMPLER SOUNDFONT ",
            "MIDI GATE [midi]: …and with the cap on the cycle has three stops (the control)");

        // ⭐ DISPLAY IS NOT GATED. An instrument that is ALREADY external — off disk, or from a .pti —
        // keeps its type and keeps naming itself, and the TYPE gesture still has somewhere to go
        // rather than sitting on a value the modulo cannot leave.
        state.caps = PlatformCaps::sdl(false);
        host.set_instrument_type(7, songcore::InstrumentType::EXTERNAL);
        eqs(songcore::instrument_type_name(type_of()), "EXTERNAL",
            "⭐ MIDI GATE [no-midi]: an EXTERNAL instrument on disk KEEPS its type and its name");
        dispatch.on_a_up();
        eqs(songcore::instrument_type_name(type_of()), "SAMPLER",
            "⭐ MIDI GATE [no-midi]: …and A+UP walks it back INTO the reachable set, never stalls");

        // ── (d) The FX type column stops before the MIDI commands ─────────────────────────────────
        state.currentScreen = ScreenType::PHRASE;
        state.currentPhrase = 3;
        state.cursorRow     = 0;
        state.cursorColumn  = 4;                       // FX1 type
        proj.phrases[3].steps[0].fx1Type = songcore::FX_NONE;

        state.caps = PlatformCaps::sdl(false);
        eq(dispatch.visible_effect_type_count(), songcore::EFFECT_TYPE_COUNT_NO_MIDI,
           "⭐⭐ MIDI GATE [no-midi]: the reachable FX list stops before the MIDI commands");
        state.caps = PlatformCaps::sdl(true);
        eq(dispatch.visible_effect_type_count(), songcore::EFFECT_TYPE_COUNT,
           "MIDI GATE [midi]: …and reaches the last of them (the control)");

        // ⭐ …and a step that already HOLDS one still reads as itself. The bound clamps the CURSOR,
        // never the CELL: this value came off disk and this build did not author it.
        state.caps = PlatformCaps::sdl(false);
        proj.phrases[3].steps[0].fx1Type  = songcore::FX_MPG;
        proj.phrases[3].steps[0].fx1Value = 0x40;
        eqs(songcore::effect_name(proj.phrases[3].steps[0].fx1Type), "MPG",
            "⭐ MIDI GATE [no-midi]: an MPG cell on disk still reads MPG — display is not gated");

        // The picker A+UP opens is bounded by its own grid, and it must agree with the bound above —
        // two ways into one column, and a build where they disagree has a cell the picker cannot say.
        state.caps = PlatformCaps::sdl(false);
        proj.phrases[3].steps[0].fx1Type = songcore::FX_NONE;
        dispatch.on_a_up();
        ok(state.fxHelper.isOpen, "MIDI GATE [no-midi]: (setup) A+UP opens the FX picker");
        eq(state.fxHelper.grid.count, songcore::EFFECT_TYPE_COUNT_NO_MIDI,
           "⭐⭐ MIDI GATE [no-midi]: the picker holds the same effects the cell can be stepped to");
        // ⚠️ The row counts are PINS and they move whenever EFFECT_TYPES grows — 5/6 held while the
        // lists were 30/36, 6/7 holds at 32/38. The stable claim is the one below them: hiding the
        // MIDI six really does cost the picker a row, which is the whole reason the count is a
        // parameter. Both are kept — the pin makes an accidental re-shape loud, the difference makes
        // a deliberate one a one-line update.
        const int rowsNoMidi = state.fxHelper.grid.rows;
        eq(rowsNoMidi, 6, "MIDI GATE [no-midi]: …in six rows");
        state.fxHelper = FxHelperState{};

        state.caps = PlatformCaps::sdl(true);
        dispatch.on_a_up();
        eq(state.fxHelper.grid.count, songcore::EFFECT_TYPE_COUNT,
           "MIDI GATE [midi]: the picker holds every effect (the control)");
        const int rowsMidi = state.fxHelper.grid.rows;
        std::printf("       [info] FX picker rows: %d effects in %d rows / %d hidden-MIDI in %d\n",
                    songcore::EFFECT_TYPE_COUNT, rowsMidi,
                    songcore::EFFECT_TYPE_COUNT_NO_MIDI, rowsNoMidi);
        eq(rowsMidi, 7, "MIDI GATE [midi]: …in seven rows");
        eq(rowsMidi - rowsNoMidi, 1,
           "MIDI GATE: hiding the MIDI six costs the picker exactly one row");
        state.fxHelper = FxHelperState{};

        // Leave the harness as it was found.
        state.caps                = PlatformCaps::sdl(true);
        state.currentScreen       = ScreenType::PROJECT;
        state.projectCursorRow    = 0;
        state.projectCursorColumn = 1;
        host.set_instrument_type(7, songcore::InstrumentType::SAMPLER);
        proj.phrases[3].steps[0].fx1Type  = songcore::FX_NONE;
        proj.phrases[3].steps[0].fx1Value = 0;
    }

    // ── 44. THE PLAYBACK CURSOR IN PHRASE MODE ──────────────────────────────────────────────────
    //
    // ⚠️ THE FIELD THE SHELL READS IS NOT THE FIELD THE MODE FILLED.
    //
    // `PlaybackPosition` carries `row` and `phraseStep`, and its own header says `row` "doubles as the
    // phrase step in every mode". CHAIN and SONG fill BOTH. PHRASE filled only `row` — while the shell
    // reads `pos.phraseStep` into `state.playbackRow` (app.cpp). So the highlight on the PHRASE screen
    // sat on step 0 for the whole loop, and only in phrase playback: the same code path drew a moving
    // cursor under SONG, which is why it read as "it works sometimes".
    //
    // This is a SIDE-RECORD (event-schema SC-4): playheads carry no bus event, so all 36 golden traces
    // are green either way and ptplay is structurally blind to it. Nothing else in the ladder looks at
    // it at all — hence a check here, at the level the shell reads.
    {
        songcore::MidiRouter kr;
        songcore::Project    kp = songcore::make_default_project();
        kp.tempo = 120;
        songcore::Sequencer kseq(kr, kp, 44100);
        kseq.set_clock(0);
        kseq.playPhrase(0);

        const int64_t fps = songcore::frames_per_step(120, 44100);

        int distinctStep = 0, distinctRow = 0, lastStep = -1, lastRow = -1, matched = 0;
        for (int k = 0; k < 16; ++k) {
            kseq.set_clock(k * fps + fps / 2);          // mid-step: no boundary rounding to argue about
            const songcore::PlaybackPosition pos = kseq.getPlaybackPosition();
            if (pos.phraseStep != lastStep) { ++distinctStep; lastStep = pos.phraseStep; }
            if (pos.row        != lastRow)  { ++distinctRow;  lastRow  = pos.row; }
            if (pos.phraseStep == k) ++matched;
        }

        // ⚠️ `row` is the CONTROL, and it is what makes this measure the right thing: it moved before
        // the fix too, so a check that only asserted "something advances" passed on the broken build.
        eq(distinctRow, 16, "PHRASE CURSOR: (control) `row` advances one value per step");
        eq(distinctStep, 16, "⭐ PHRASE CURSOR: `phraseStep` — the field the SHELL reads — advances too");
        eq(matched, 16, "PHRASE CURSOR: …and it equals the step it is standing on, all 16 of them");
    }

    // ── 45. A LIVE EDIT IS HEARD ON THE NEXT PHRASE, NOT THREE PHRASES LATER ────────────────────
    //
    // ⚠️ §26 SAYS LIVE-EDIT RESCHEDULING HAS NO COVERAGE ON EITHER ENGINE. This is that coverage, and
    // it asks the question a user asks: **how many phrase repeats until I hear what I just typed?**
    //
    // The scheduler runs BUFFER_PHRASES (2) phrases ahead, so an untouched lookahead makes an edit
    // audible ~3 phrases later. `notify_data_changed()` exists to roll that back to the earliest
    // UNPLAYED phrase boundary; `mark_modified()` calls it whenever the transport is running. Neither
    // half was measured: §26 proved a rollback does not EAT A PENDING KIL, which is the opposite
    // question, and a harness that calls the host verb directly cannot tell "the rollback works" from
    // "the rollback works and no gesture asks for it".
    //
    // So the edit here is the REAL one — A+UP on the VOLUME cell of the PHRASE screen, through the
    // real dispatcher, over a real engine with the transport actually running — and the metric is the
    // AUDIO: which repeat first carries sound in the edited step's window.
    {
        auto engine = std::make_unique<AudioEngine>();   // ⚠️ HEAP — see §23
        engine->setDeviceSampleRate(44100);

        songcore::SongcoreHost lhost(engine.get(), 44100);
        AppState               lstate;
        lstate.project = &lhost.edit_project();
        lstate.caps    = PlatformCaps::sdl(true);
        InputDispatcher ld(lstate, lhost, fs_impl);

        const fs::path ltone = tree.root / "Samples" / "livetone.wav";
        {
            std::vector<float> pcm(44100 / 4);
            for (size_t i = 0; i < pcm.size(); ++i)
                pcm[i] = static_cast<float>(0.6 * std::sin(2.0 * 3.14159265358979 *
                                                           440.0 * static_cast<double>(i) / 44100.0));
            songcore::write_wav_mono(ltone.generic_string(), pcm, 44100);
        }

        songcore::Project& lp = lhost.edit_project();
        lp = songcore::make_default_project();
        lp.tempo = 240;                       // fast, so eight repeats fit in a short run
        lhost.load_sample(0, ltone.generic_string());
        lp.instruments[0].volume = 0xFF;

        // Step 0 sounds throughout — the positive control that the transport is running at all, so a
        // silent step 8 means "the edit was not heard" rather than "nothing was playing".
        lp.phrases[0].steps[0].note = songcore::Note::C4();
        lp.phrases[0].steps[0].instrument = 0;
        lp.phrases[0].steps[0].volume = 0x7F;
        // Step 8 already HAS a note, at volume 0 — inaudible. The gesture raises it. (An empty cell
        // would take A, a different handler; VOLUME is the column the report named.)
        lp.phrases[0].steps[8].note = songcore::Note::C4();
        lp.phrases[0].steps[8].instrument = 0;
        lp.phrases[0].steps[8].volume = 0x00;
        lhost.push_params();

        lhost.play_phrase(0);

        const int64_t fps       = songcore::frames_per_step(240, 44100);
        const int64_t perPhrase = fps * 16;
        constexpr int BLK       = 128;
        constexpr int REPEATS   = 8;
        const int     editAt    = 2;

        std::vector<float>  buf(BLK * 2);
        std::vector<double> step8(REPEATS, 0.0), step0(REPEATS, 0.0);
        bool                edited = false;

        for (int64_t f = 0; f < perPhrase * REPEATS; f += BLK) {
            lhost.poll();
            ld.set_now(static_cast<long long>(f * 1000 / 44100));

            if (!edited && f >= editAt * perPhrase + fps * 2) {
                edited = true;
                lstate.currentScreen = ScreenType::PHRASE;
                lstate.cursorRow     = 8;
                lstate.cursorColumn  = 2;                       // NOTE=0 INST=1 VOL=2
                for (int n = 0; n < 40; ++n) ld.on_a_up();      // walk the volume up to audible
            }

            engine->processLiveBlock(buf.data(), BLK, 2, 44100.0f);

            for (int i = 0; i < BLK; ++i) {
                const int64_t frame = f + i;
                const int     rep   = static_cast<int>(frame / perPhrase);
                if (rep < 0 || rep >= REPEATS) continue;
                const int64_t into = frame % perPhrase;
                const double  v    = std::fabs(static_cast<double>(buf[i * 2]));
                // ⚠️ The windows START one step AFTER the trigger, so a peak cannot inherit the
                // previous step's tail — E4's "discard one window, measure the next".
                if (into >= fps * 9 && into < fps * 11) step8[rep] = std::max(step8[rep], v);
                if (into >= fps * 1 && into < fps * 3)  step0[rep] = std::max(step0[rep], v);
            }
        }

        int quietRepeats = 0, heard = -1;
        for (int r = 0; r < REPEATS; ++r) if (step0[r] > 0.02) ++quietRepeats;
        for (int r = 0; r < REPEATS; ++r) if (step8[r] > 0.02) { heard = r; break; }

        // ⭐ The number beside the verdict, and `heard` is derived from the rendered AUDIO rather than
        // from what the gesture was supposed to do.
        std::printf("           live edit: made in repeat %d, first heard in repeat %d\n", editAt, heard);
        eq(quietRepeats, REPEATS, "LIVE EDIT: (control) step 0 sounded in every repeat — the transport ran");
        eq(lhost.project().phrases[0].steps[8].volume > 0x00, 1,
           "LIVE EDIT: (control) A+UP really moved the VOLUME cell it was standing on");
        eq(heard - editAt, 1,
           "⭐⭐ LIVE EDIT: the edit is audible on the NEXT phrase, not after the 2-phrase lookahead");
    }

    // ── 46. VTR / VMV MOVE THE MIXER FADERS, AND stop() PUTS THEM BACK ───────────────────────────
    //
    // Nothing else in this tree can see any of this. The traces stop at the bus, so ptplay proves only
    // that a CC was EMITTED — not that the fader moved, and certainly not that it moved back. And the
    // restore lives in the one channel the guardrails name as structurally invisible: STOP. So this is
    // rendered audio, with the level measured on both sides of every claim.
    //
    // ⚠️ THE WHOLE SECTION WOULD PASS BY CONSTRUCTION WITHOUT ITS CONTROL. "The level dropped after
    // step 4" is also what a sample running out, a voice being stolen, or an envelope closing looks
    // like. The control is the SAME project with the VTR cell cleared: if that also drops, the check is
    // measuring the tone and not the effect.
    {
        auto engine = std::make_unique<AudioEngine>();   // ⚠️ HEAP — see §23
        engine->setDeviceSampleRate(44100);
        songcore::SongcoreHost vhost(engine.get(), 44100);

        // A LOOPING tone, so the level is flat for as long as we care to measure and any drop we see is
        // something the engine did rather than the sample ending.
        const fs::path tone = tree.root / "Samples" / "vtrtone.wav";
        {
            std::vector<float> pcm(44100 / 2);
            for (size_t i = 0; i < pcm.size(); ++i) {
                const double t = static_cast<double>(i) / 44100.0;
                pcm[i] = static_cast<float>(0.6 * std::sin(2.0 * 3.14159265358979 * 440.0 * t));
            }
            songcore::write_wav_mono(tone.generic_string(), pcm, 44100);
        }

        const int64_t fps = songcore::frames_per_step(120, 44100);

        // One render, parameterised by what the phrase carries. Returns the RMS of a window WELL
        // before the effect step and one well after it, so a ramp or a declick cannot straddle either.
        // ⚠️ The windows deliberately start a step late — E4's "discard one window, measure the next".
        struct Levels { double before, after; };

        // Play the loaded project and measure. Split out of `render` because the STOP check below must
        // replay WITHOUT re-pushing params — see (c).
        auto play_and_measure = [&]() -> Levels {
            vhost.play_song(0);
            constexpr int      BLK = 256;
            std::vector<float> buf(static_cast<size_t>(BLK) * 2);
            double  sumB = 0.0, sumA = 0.0;
            int64_t nB = 0, nA = 0;
            for (int64_t f = 0; f < fps * 8; f += BLK) {
                vhost.poll();
                engine->processLiveBlock(buf.data(), BLK, 2, 44100.0f);
                for (int i = 0; i < BLK; ++i) {
                    const double  v     = buf[static_cast<size_t>(i) * 2];
                    const int64_t frame = f + i;
                    if (frame >= fps * 1 && frame < fps * 3) { sumB += v * v; ++nB; }
                    if (frame >= fps * 5 && frame < fps * 7) { sumA += v * v; ++nA; }
                }
            }
            vhost.stop();
            return Levels{ nB ? std::sqrt(sumB / static_cast<double>(nB)) : 0.0,
                           nA ? std::sqrt(sumA / static_cast<double>(nA)) : 0.0 };
        };

        auto render = [&](int fxType, int fxValue, int fxTrack, bool externalOnFxTrack) -> Levels {
            songcore::Project& p = vhost.edit_project();
            p = songcore::make_default_project();
            p.tempo        = 120;
            p.masterVolume = 0xFF;

            vhost.load_sample(0, tone.generic_string());
            p.instruments[0].loopMode = "fwd";   // ⚠️ a STRING; `= 1` assigns a char and does nothing
            p.instruments[0].volume   = 0xC0;

            // Track 0 SOUNDS, at a full authored fader — so a drop has somewhere to fall from.
            p.tracks[0].volume = 0xFF;
            p.tracks[0].chainRefs.assign(256, -1);
            p.tracks[0].chainRefs[0]  = 0;
            p.chains[0].phraseRefs[0] = 0;
            songcore::PhraseStep& n = p.phrases[0].steps[0];
            n.note = songcore::Note::C4();  n.instrument = 0;  n.volume = 0x7F;

            if (fxType != songcore::FX_NONE) {
                if (fxTrack == 0) {
                    p.phrases[0].steps[4].fx1Type  = fxType;
                    p.phrases[0].steps[4].fx1Value = fxValue;
                } else {
                    // The effect rides ANOTHER track, on its own chain and phrase. Used for the
                    // EXTERNAL case: that track makes no sound here, and must still move the master.
                    p.tracks[fxTrack].volume = 0xFF;
                    p.tracks[fxTrack].chainRefs.assign(256, -1);
                    p.tracks[fxTrack].chainRefs[0] = 1;
                    p.chains[1].phraseRefs[0]      = 1;
                    p.phrases[1].steps[4].fx1Type  = fxType;
                    p.phrases[1].steps[4].fx1Value = fxValue;
                    if (externalOnFxTrack) {
                        // The gate reads the instrument the TRACK is playing, so the phrase needs a
                        // note to name one.
                        p.instruments[1].instrumentType = songcore::InstrumentType::EXTERNAL;
                        songcore::PhraseStep& x = p.phrases[1].steps[0];
                        x.note = songcore::Note::C4();  x.instrument = 1;  x.volume = 0x7F;
                    }
                }
            }

            vhost.push_params();
            return play_and_measure();
        };

        // ── (a) the CONTROL, run FIRST so a broken harness cannot hide behind a passing claim ─────
        const Levels flat = render(songcore::FX_NONE, 0, 0, false);
        std::printf("       [info] VTR control (no effect): before %.5f → after %.5f (%.0f%%)\n",
                    flat.before, flat.after, flat.before > 0 ? 100.0 * flat.after / flat.before : 0.0);
        ok(flat.before > 0.02, "VTR: (control) the tone is actually sounding — the test can fail");
        ok(flat.after > flat.before * 0.90,
           "⚠️ VTR: (control) with NO effect the level is FLAT across the same two windows — a drop "
           "below would mean the checks under it measure the tone, not the fader");

        // ── (b) VTR 40 — a quarter of the authored FF ─────────────────────────────────────────────
        const Levels cut = render(songcore::FX_VTR, 0x40, 0, false);
        const double ratio = cut.before > 0 ? cut.after / cut.before : 0.0;
        std::printf("       [info] VTR 40 on a FF fader: before %.5f → after %.5f (%.1f%%, want ~25%%)\n",
                    cut.before, cut.after, 100.0 * ratio);
        ok(cut.before > 0.02, "VTR: the tone sounds before the effect step");
        ok(ratio > 0.18 && ratio < 0.32,
           "⭐ VTR: the track fader really moved to 0x40/0xFF — the RATIO is derived from the "
           "rendered audio, not from the byte that was typed");

        // ── (c) STOP PUTS THE AUTHORED FADER BACK — the blind channel ─────────────────────────────
        //
        // ⚠️⚠️ THE FIRST VERSION OF THIS CHECK WAS VACUOUS AND ONLY THE CONTROL SAID SO. It replayed
        // through `render`, which opens with `push_params()` — and push_mixer re-pushes both faders. So
        // the level came back whether stop() restored anything or not, and deleting the restore in
        // host.h left the whole section ALL GREEN.
        //
        // The fix is to replay WITHOUT pushing: `play_song` deliberately does not push params (host.h),
        // so the only thing that can put the fader back between these two plays is stop() itself. The
        // VTR cell is cleared through the project so the second play has no effect of its own.
        {
            songcore::Project& p = vhost.edit_project();
            p.phrases[0].steps[4].fx1Type  = songcore::FX_NONE;
            p.phrases[0].steps[4].fx1Value = 0x00;
            const Levels afterCut = play_and_measure();     // ⚠️ no push_params
            std::printf("       [info] VTR restore (no re-push): control %.5f → after a VTR song %.5f "
                        "(%.0f%%)\n", flat.before, afterCut.before,
                        flat.before > 0 ? 100.0 * afterCut.before / flat.before : 0.0);
            ok(afterCut.before > flat.before * 0.90,
               "⚠️⚠️ VTR: stop() RESTORED the authored fader — with no push_params between the two "
               "plays, nothing else can have");
            ok(afterCut.after > flat.before * 0.90,
               "VTR: …and it stayed restored for the whole of the next song");
        }

        // ── (d) VMV FROM AN EXTERNAL TRACK STILL MOVES THE MASTER ─────────────────────────────────
        //
        // The asymmetry claimed in event.h / engine_consumer.h: VTR is track-scoped and the external
        // routing gate is allowed to swallow it, while VMV rides TRACK_GLOBAL precisely so the gate
        // cannot. Track 1 plays an EXTERNAL instrument and carries the VMV; track 0 is what we hear.
        const Levels ext = render(songcore::FX_VMV, 0x40, 1, true);
        const double extRatio = ext.before > 0 ? ext.after / ext.before : 0.0;
        std::printf("       [info] VMV 40 from an EXTERNAL track: before %.5f → after %.5f (%.1f%%)\n",
                    ext.before, ext.after, 100.0 * extRatio);
        ok(ext.before > 0.02, "VMV: track 0 sounds while track 1 runs an external instrument");
        ok(extRatio > 0.18 && extRatio < 0.32,
           "⭐⭐ VMV: the MASTER fader moved from a track the engine consumer does not own — route it "
           "on the track lane instead of TRACK_GLOBAL and the external gate eats it silently");

        // …and the same restore, for the master this time — again with no push between the plays.
        {
            songcore::Project& p = vhost.edit_project();
            p.phrases[1].steps[4].fx1Type  = songcore::FX_NONE;
            p.phrases[1].steps[4].fx1Value = 0x00;
            const Levels afterVmv = play_and_measure();     // ⚠️ no push_params
            std::printf("       [info] VMV restore (no re-push): control %.5f → after a VMV song %.5f "
                        "(%.0f%%)\n", flat.before, afterVmv.before,
                        flat.before > 0 ? 100.0 * afterVmv.before / flat.before : 0.0);
            ok(afterVmv.before > flat.before * 0.90,
               "⚠️ VMV: stop() restored the authored MASTER fader too");
        }
    }

    // ── 47. AUS / AUF — THE PAIRING RULE, AND THE REGISTRY IT READS ──────────────────────────────
    //
    // Pairing is a pure function over a phrase (songcore/automation.h) and nothing else in this tree
    // can see it: it emits no events, so ptplay has nothing to compare; it makes no sound, so
    // ptrender has nothing to measure; and it is not a cell, so ptinput never runs it. Every claim
    // below is therefore hand-derived from the rule as written, and the two halves are each other's
    // control — a `find_ramps` that always returned nothing would pass every "inert" check on its
    // own, and does not survive being asked for the ramps that ARE there.
    {
        using songcore::AUTOMATABLE_PARAMS;
        using songcore::find_ramps;
        using songcore::RampSpec;

        auto fx = [](songcore::Phrase& ph, int step, int slot, int type, int value) {
            songcore::step_set_fx(ph.steps[static_cast<size_t>(step)], slot, type, value);
        };

        // A ramp as one printable token — every field that decides what the emitter will do with it.
        auto spec_str = [](const RampSpec& r) {
            char b[192];
            std::snprintf(b, sizeof b, "%s cc%d %s %d-%d %02X>%02X curve %02X slots %d/%d",
                          songcore::effect_name(r.fxCode).c_str(), static_cast<int>(r.ccId),
                          r.global ? "global" : "track", r.ausStep, r.aufStep,
                          r.startByte, r.destByte, r.curveByte, r.paramSlot, r.ausSlot);
            return std::string(b);
        };
        auto specs_str = [&](const std::vector<RampSpec>& v) {
            if (v.empty()) return std::string("(none)");
            std::string s;
            for (const RampSpec& r : v) { if (!s.empty()) s += " | "; s += spec_str(r); }
            return s;
        };

        // ── (a) the registry — read OUT of the table, never re-typed beside it ─────────────────────
        {
            std::string reg;
            int         globals = 0;
            for (const songcore::AutomatableParam& p : AUTOMATABLE_PARAMS) {
                reg += songcore::effect_name(p.fxCode) + "=" + std::to_string(static_cast<int>(p.ccId)) + " ";
                if (p.global) ++globals;
            }
            std::printf("       [info] automatable params: %s\n", reg.c_str());
            // The CC ids are the ones the PER-STEP effects already emit (scheduler.h STEP 2.3) — a
            // ramp is the same event more often, so a wrong id here is a ramp that moves nothing.
            eqs(reg, "VOL=7 PAN=10 REV=91 DEL=93 VTR=132 VMV=133 ",
                "AUTO: the registry names each parameter's existing CC id");
            eq(globals, 1, "AUTO: exactly one entry is global — the master fader belongs to no track");
            const songcore::AutomatableParam* vmv = songcore::automatable_param(songcore::FX_VMV);
            const bool vmvGlobal = vmv != nullptr && vmv->global;
            ok(vmvGlobal, "AUTO: ...and it is VMV — track-scoped, the external gate would eat it");
            // ⚠️ The gate asserted in BOTH directions: a lookup that returned a row for everything
            // would pass every check above.
            ok(songcore::automatable_param(songcore::FX_PSL) == nullptr,
               "AUTO: (control) an effect with no absolute CC path is NOT automatable");

            int typable = 0;
            for (const songcore::AutomatableParam& p : AUTOMATABLE_PARAMS)
                if (songcore::EFFECT_TYPES[songcore::effect_type_index(p.fxCode)] == p.fxCode) ++typable;
            eq(typable, songcore::AUTOMATABLE_PARAM_COUNT,
               "AUTO: every automatable effect is one the user can actually type");
        }

        // ── (b) the two new codes, and the lists that must never drift from them ───────────────────
        {
            eqs(songcore::effect_name(songcore::FX_AUS), "AUS", "AUS: has a name");
            eqs(songcore::effect_name(songcore::FX_AUF), "AUF", "AUF: has a name");
            ok(songcore::effect_type_index(songcore::FX_AUS) > 0 &&
                   songcore::effect_type_index(songcore::FX_AUF) > 0,
               "AUS/AUF: both are in EFFECT_TYPES — an effect missing from it is one nobody can type");
            eq(static_cast<int>(effect_descriptions().size()), songcore::EFFECT_TYPE_COUNT,
               "FX HELPER: one description per effect");

            // ⭐ The alignment, derived from BOTH lists rather than eyeballed: entry i must open with
            // the name of effect i. An insert that shifts one list and not the other is otherwise a
            // silent mislabel — the picker documents PAN while the cursor commits BCK.
            int         aligned = 0;
            std::string firstBad;
            for (int i = 0; i < songcore::EFFECT_TYPE_COUNT; ++i) {
                const std::string        want  = songcore::effect_name(songcore::EFFECT_TYPES[i]) + ":";
                const std::vector<std::string>& lines = effect_descriptions()[static_cast<size_t>(i)];
                if (!lines.empty() && lines[0].rfind(want, 0) == 0) { ++aligned; continue; }
                if (firstBad.empty())
                    firstBad = std::to_string(i) + " wants " + want +
                               ", reads " + (lines.empty() ? "(empty)" : lines[0]);
            }
            if (!firstBad.empty()) std::printf("       [info] first misaligned description: %s\n", firstBad.c_str());
            eq(aligned, songcore::EFFECT_TYPE_COUNT,
               "FX HELPER: every description opens with its own effect's name");
        }

        // ── (c) the curve — hand-derived, and the family is continuous through the middle ─────────
        {
            using songcore::automation_shape;
            using songcore::automation_value_byte;

            std::printf("       [info] curve 00/80/FF at t=0.5 over 00->FF: %d / %d / %d\n",
                        automation_value_byte(0x00, 0xFF, 0x00, 0.5),
                        automation_value_byte(0x00, 0xFF, 0x80, 0.5),
                        automation_value_byte(0x00, 0xFF, 0xFF, 0.5));

            eq(automation_value_byte(0x00, 0xFF, 0x80, 0.0), 0x00, "CURVE: linear starts AT the start byte");
            eq(automation_value_byte(0x00, 0xFF, 0x80, 1.0), 0xFF, "CURVE: ...and arrives AT the destination");
            eq(automation_value_byte(0x00, 0xFF, 0x80, 0.5), 128,  "CURVE: linear is half way at half time");
            eq(automation_value_byte(0x00, 0xFF, 0x00, 0.5), 32,
               "CURVE: ease-in is a cubic — 0.5^3 of the way at half time");
            eq(automation_value_byte(0x00, 0xFF, 0xFF, 0.5), 223,
               "CURVE: ease-out is its mirror — 1 - 0.5^3");
            eq(automation_value_byte(0xFF, 0x00, 0x80, 0.5), 128, "CURVE: a DESCENDING ramp is the same arithmetic");

            // ⚠️ Measured in the SHAPE, not the byte: either side of 0x80 differs from linear by less
            // than half a byte at t=0.5, so the quantised value cannot see the blend at all. The
            // metric has to match the claim, and the claim is about the curve.
            const double below = automation_shape(0x7F, 0.5);
            const double mid   = automation_shape(0x80, 0.5);
            const double above = automation_shape(0x81, 0.5);
            std::printf("       [info] shape at t=0.5, curve 7F/80/81: %.6f / %.6f / %.6f\n", below, mid, above);
            ok(mid == 0.5, "CURVE: 80 is EXACTLY linear, not merely close to it");
            ok(below < mid && mid < above,
               "⭐ CURVE: the family is monotonic in the curve byte — 7F leans towards ease-in, 81 "
               "towards ease-out, in both directions off the same anchor");

            // Every curve byte, not just the three anchors: a ramp that overshoots its destination or
            // starts somewhere other than where the author typed is a bug at any shape.
            int endsExact = 0, monotonic = 0;
            for (int c = 0; c <= 0xFF; ++c) {
                if (automation_shape(c, 0.0) == 0.0 && automation_shape(c, 1.0) == 1.0) ++endsExact;
                bool up = true;
                int  prev = -1;
                for (int k = 0; k <= 96; ++k) {
                    const int b = automation_value_byte(0x00, 0xFF, c, k / 96.0);
                    if (b < prev || b < 0 || b > 255) up = false;
                    prev = b;
                }
                if (up) ++monotonic;
            }
            eq(endsExact, 256, "CURVE: every shape hits 0 and 1 exactly at the ends");
            eq(monotonic, 256, "CURVE: ...and none of them backtracks or leaves 00-FF on the way");
        }

        // ── (d) THE PAIR OF FIXTURES THAT ARE EACH OTHER'S CONTROL ─────────────────────────────────
        //
        // One phrase, one cell apart. Without the pair there is nothing to find; with it there is
        // exactly one ramp. A `find_ramps` stuck at either answer fails one of these two.
        {
            songcore::Phrase quiet;
            fx(quiet, 0, 1, songcore::FX_VOLUME, 0x00);
            const std::vector<RampSpec> before = find_ramps(quiet);
            std::printf("       [info] pairing: VOL alone -> %zu ramp(s); + AUS/AUF -> ", before.size());
            eq(static_cast<int>(before.size()), 0,
               "PAIRING: (control) a VOL with no AUS beside it declares no ramp");

            songcore::Phrase ph = quiet;
            fx(ph, 0, 2, songcore::FX_AUS, 0x80);
            fx(ph, 8, 1, songcore::FX_AUF, 0xFF);
            const std::vector<RampSpec> after = find_ramps(ph);
            std::printf("%zu\n", after.size());
            eq(static_cast<int>(after.size()), 1, "PAIRING: the canonical pair declares exactly one ramp");
            if (after.size() == 1)
                eqs(spec_str(after[0]), "VOL cc7 track 0-8 00>FF curve 80 slots 1/2",
                    "⭐ PAIRING: VOL 00 + AUS 80 on step 0, AUF FF on step 8 — every field of it");
        }

        // ── (e) look-left: which effect a curve attaches to ───────────────────────────────────────
        {
            // Skips over a non-automatable effect to reach the nearest one that is.
            songcore::Phrase skip;
            fx(skip, 0, 1, songcore::FX_VOLUME, 0x20);
            fx(skip, 0, 2, songcore::FX_PSL,    0x40);
            fx(skip, 0, 3, songcore::FX_AUS,    0x00);
            fx(skip, 4, 1, songcore::FX_AUF,    0xF0);
            const std::vector<RampSpec> a = find_ramps(skip);
            std::printf("       [info] look-left over PSL: %s\n", specs_str(a).c_str());
            eqs(specs_str(a), "VOL cc7 track 0-4 20>F0 curve 00 slots 1/3",
                "LOOK-LEFT: a non-automatable effect in between is stepped over, not paired with");

            // NEAREST, not first: two automatable effects, the closer one wins.
            songcore::Phrase near;
            fx(near, 0, 1, songcore::FX_VOLUME, 0x20);
            fx(near, 0, 2, songcore::FX_PAN,    0x30);
            fx(near, 0, 3, songcore::FX_AUS,    0x80);
            fx(near, 4, 1, songcore::FX_AUF,    0xC0);
            const std::vector<RampSpec> b = find_ramps(near);
            std::printf("       [info] look-left with two candidates: %s\n", specs_str(b).c_str());
            eqs(specs_str(b), "PAN cc10 track 0-4 30>C0 curve 80 slots 2/3",
                "LOOK-LEFT: the NEAREST automatable effect to the left wins, and the ramp starts at ITS byte");

            // Nothing automatable to the left at all — the guard the design names.
            songcore::Phrase bare;
            fx(bare, 0, 1, songcore::FX_PSL, 0x40);
            fx(bare, 0, 2, songcore::FX_AUS, 0x80);
            fx(bare, 4, 1, songcore::FX_AUF, 0xFF);
            eqs(specs_str(find_ramps(bare)), "(none)",
               "LOOK-LEFT: an AUS with nothing automatable to its left is INERT — a curve alone names "
               "no parameter, and guessing one would move something nobody pointed at");

            // ⚠️ …and being inert means inert: it must not close or replace a ramp already open.
            songcore::Phrase survives;
            fx(survives, 0, 1, songcore::FX_VOLUME, 0x10);
            fx(survives, 0, 2, songcore::FX_AUS,    0x80);
            fx(survives, 2, 1, songcore::FX_PSL,    0x40);
            fx(survives, 2, 2, songcore::FX_AUS,    0x00);   // inert — PSL is not automatable
            fx(survives, 8, 1, songcore::FX_AUF,    0xFF);
            eqs(specs_str(find_ramps(survives)), "VOL cc7 track 0-8 10>FF curve 80 slots 1/2",
                "LOOK-LEFT: an inert AUS leaves an OPEN ramp alone — it does not silently take it over");
        }

        // ── (f) opening and closing: last AUS wins, first AUF closes ──────────────────────────────
        {
            songcore::Phrase reopen;
            fx(reopen, 0, 1, songcore::FX_VOLUME, 0x10);
            fx(reopen, 0, 2, songcore::FX_AUS,    0x80);
            fx(reopen, 2, 1, songcore::FX_PAN,    0x20);
            fx(reopen, 2, 2, songcore::FX_AUS,    0x00);
            fx(reopen, 8, 1, songcore::FX_AUF,    0xFF);
            eqs(specs_str(find_ramps(reopen)), "PAN cc10 track 2-8 20>FF curve 00 slots 1/2",
                "PAIRING: a second AUS REPLACES the open ramp — last-wins, as the FX slots already are");

            songcore::Phrase twoEnds;
            fx(twoEnds, 0, 1, songcore::FX_VOLUME, 0x00);
            fx(twoEnds, 0, 2, songcore::FX_AUS,    0x80);
            fx(twoEnds, 4, 1, songcore::FX_AUF,    0x40);
            fx(twoEnds, 8, 1, songcore::FX_AUF,    0xFF);
            eqs(specs_str(find_ramps(twoEnds)), "VOL cc7 track 0-4 00>40 curve 80 slots 1/2",
                "PAIRING: the FIRST AUF closes, and the second one has no open ramp to end");

            // Two complete ramps in one phrase, in the order they open.
            songcore::Phrase pairPair;
            fx(pairPair, 0,  1, songcore::FX_VOLUME, 0x00);
            fx(pairPair, 0,  2, songcore::FX_AUS,    0x80);
            fx(pairPair, 4,  1, songcore::FX_AUF,    0xFF);
            fx(pairPair, 8,  1, songcore::FX_PAN,    0xFF);
            fx(pairPair, 8,  2, songcore::FX_AUS,    0xFF);
            fx(pairPair, 12, 1, songcore::FX_AUF,    0x00);
            eqs(specs_str(find_ramps(pairPair)),
                "VOL cc7 track 0-4 00>FF curve 80 slots 1/2 | PAN cc10 track 8-12 FF>00 curve FF slots 1/2",
                "PAIRING: two ramps in one phrase come back in the order they open");
        }

        // ── (g) the spans that are not spans ──────────────────────────────────────────────────────
        {
            songcore::Phrase orphanEnd;
            fx(orphanEnd, 4, 1, songcore::FX_AUF, 0xFF);
            eqs(specs_str(find_ramps(orphanEnd)), "(none)", "PAIRING: an AUF with no open AUS is inert");

            songcore::Phrase orphanStart;
            fx(orphanStart, 0, 1, songcore::FX_VOLUME, 0x00);
            fx(orphanStart, 0, 2, songcore::FX_AUS,    0x80);
            eqs(specs_str(find_ramps(orphanStart)), "(none)",
                "PAIRING: an AUS the phrase never closes is inert — pairing does not cross into the "
                "next phrase, which the lookahead has not scheduled yet");

            // ⚠️ Slot order inside a step is not a time order, so a same-step AUF describes a span
            // with no duration. It is inert, and the AUS stays open for a real one.
            songcore::Phrase sameStep;
            fx(sameStep, 0, 1, songcore::FX_VOLUME, 0x00);
            fx(sameStep, 0, 2, songcore::FX_AUS,    0x80);
            fx(sameStep, 0, 3, songcore::FX_AUF,    0xFF);
            eqs(specs_str(find_ramps(sameStep)), "(none)",
                "PAIRING: an AUF on the AUS's OWN step is inert — a zero-length span has nothing to "
                "interpolate across");

            songcore::Phrase sameThenLater = sameStep;
            fx(sameThenLater, 6, 1, songcore::FX_AUF, 0xC0);
            eqs(specs_str(find_ramps(sameThenLater)), "VOL cc7 track 0-6 00>C0 curve 80 slots 1/2",
                "PAIRING: ...and the AUS it did not close is still open for the next real AUF");
        }

        // ── (h) entering a phrase part-way through, and the global lane ───────────────────────────
        {
            songcore::Phrase mid;
            fx(mid, 0, 1, songcore::FX_VOLUME, 0x00);
            fx(mid, 0, 2, songcore::FX_AUS,    0x80);
            fx(mid, 8, 1, songcore::FX_AUF,    0xFF);
            eqs(specs_str(find_ramps(mid, 0)), "VOL cc7 track 0-8 00>FF curve 80 slots 1/2",
                "START ROW: (control) from row 0 the ramp is there to be found");
            eqs(specs_str(find_ramps(mid, 4)), "(none)",
                "START ROW: a phrase entered BELOW its AUS runs no ramp — the step that opens it "
                "never plays, so neither does the fade");

            songcore::Phrase master;
            fx(master, 0, 1, songcore::FX_VMV, 0xFF);
            fx(master, 0, 2, songcore::FX_AUS, 0x80);
            fx(master, 8, 1, songcore::FX_AUF, 0x00);
            eqs(specs_str(find_ramps(master)), "VMV cc133 global 0-8 FF>00 curve 80 slots 1/2",
                "⭐ PAIRING: a master fade carries the GLOBAL lane out of the registry — on the track "
                "lane the external-routing gate would swallow it");
        }
    }

    // ── 48. AUS / AUF — THE EMISSION ────────────────────────────────────────────────────────────
    //
    // §47 proved which spans a phrase DECLARES. This is what the scheduler does with one. A ramp is the
    // parameter's own CC emitted per tic (`emit_ramp_ticks`, scheduler.h), so the entire fade rides in
    // the event stream and every value in it can be read back — which is the whole reason the dense-CC
    // architecture was chosen over an engine-side interpolator whose correctness would live in the
    // audio and nowhere else.
    //
    // Driven through the real Sequencer with a recorder on the bus. `playPhrase` schedules exactly one
    // pass and nothing polls afterwards, so the recording is one phrase's worth of events, not a loop's.
    //
    // ⚠️ Every count and every byte below is hand-derived from the RULE, in the comment beside it. The
    // emitter must not be able to certify itself by being asked what it did.
    {
        struct Rec : songcore::IMidiConsumer {
            struct Cc { int64_t frame; int track; int param; int valueByte; };
            std::vector<Cc>      ccs;
            std::vector<int64_t> notes;
            void consume(const songcore::Event& ev) override {
                if (ev.type == songcore::EV_NOTE_ON) { notes.push_back(ev.frame); return; }
                if (ev.type != songcore::EV_CC) return;
                float v = 0.0f;
                std::memcpy(&v, &ev.cc.valueBits, sizeof v);
                // ⭐ Back to the byte an author would have typed, derived from the float that was
                // really emitted — never from the byte the emitter believed it was sending.
                ccs.push_back({ev.frame, ev.track, ev.cc.param,
                               static_cast<int>(std::lround(v * 255.0f))});
            }
            void on_play(const std::string&, const std::string&, int64_t, int, int) override {}
            void on_stop() override {}
        };
        struct Run { std::vector<Rec::Cc> ccs; std::vector<int64_t> notes; bool mixerActive; };

        const int64_t fps = songcore::frames_per_step(120, 44100);
        const int64_t fpt = fps / songcore::TICS_PER_STEP;

        auto play = [&](auto&& build) -> Run {
            songcore::Project p = songcore::make_default_project();
            p.tempo = 120;
            build(p);
            Rec                 rec;
            songcore::MidiRouter r(&rec);
            songcore::Sequencer  s(r, p, 44100);
            s.set_clock(0);
            s.playPhrase(0);
            return Run{rec.ccs, rec.notes, s.mixer_vol_active()};
        };
        auto only = [](const std::vector<Rec::Cc>& v, int param) {
            std::vector<Rec::Cc> out;
            for (const Rec::Cc& c : v) if (c.param == param) out.push_back(c);
            return out;
        };
        auto value_at = [](const std::vector<Rec::Cc>& v, int64_t frame) {
            for (const Rec::Cc& c : v) if (c.frame == frame) return c.valueByte;
            return -1;
        };
        auto fx = [](songcore::Phrase& ph, int step, int slot, int type, int value) {
            songcore::step_set_fx(ph.steps[static_cast<size_t>(step)], slot, type, value);
        };

        // ── (a) THE CANONICAL RAMP, AND THE SAME PHRASE WITHOUT ITS AUS ───────────────────────────
        //
        // `VOL 00 · AUS 80` on step 0, `AUF FF` on step 8, and NO notes anywhere — so every CC_VOLUME
        // in the recording came from this one path and there is nothing else to subtract.
        //
        // Hand-derived: the span is 8 steps = 96 tic positions from step 0 tic 0 to step 8 tic 0. Tic 0
        // of the AUS step is the authored `VOL 00` writing itself; tics 1-95 are the ramp; step 8 tic 0
        // is the arrival. Linear across 00-FF moves the byte 255/96 = 2.66 per tic, so no two
        // consecutive tics round alike and the de-dup drops nothing — 1 + 95 + 1 = 97 events.
        const Run canon = play([&](songcore::Project& p) {
            fx(p.phrases[0], 0, 1, songcore::FX_VOLUME, 0x00);
            fx(p.phrases[0], 0, 2, songcore::FX_AUS,    0x80);
            fx(p.phrases[0], 8, 1, songcore::FX_AUF,    0xFF);
        });
        const std::vector<Rec::Cc> lin = only(canon.ccs, songcore::CC_VOLUME);

        // The control FIRST: the identical phrase with the AUS cell empty. One event — the `VOL 00`
        // the author typed — and no fade. A recorder that saw nothing, or a scheduler that emitted a
        // ramp for every VOL in the song, fails here rather than passing quietly under (a).
        const Run flat = play([&](songcore::Project& p) {
            fx(p.phrases[0], 0, 1, songcore::FX_VOLUME, 0x00);
            fx(p.phrases[0], 8, 1, songcore::FX_AUF,    0xFF);
        });
        std::printf("       [info] CC_VOLUME events: no AUS %zu, with AUS %zu\n",
                    only(flat.ccs, songcore::CC_VOLUME).size(), lin.size());
        eq(static_cast<int>(only(flat.ccs, songcore::CC_VOLUME).size()), 1,
           "RAMP: (control) VOL with no AUS beside it emits its ONE per-step event and nothing else");
        eq(static_cast<int>(lin.size()), 97,
           "⭐ RAMP: the authored write + 95 tics + the arrival — every tic of the span, none twice");

        if (lin.size() == 97) {
            eq(lin[0].valueByte, 0x00, "RAMP: the first event is the authored start value");
            eq(static_cast<int>(lin[0].frame), 0, "RAMP: …on the AUS step's own frame");
            // round(255 × 1/96) = 2.66 → 3, one tic into step 0.
            eq(lin[1].valueByte, 3, "RAMP: the ramp's first tic is one 96th of the way, rounded");
            eq(static_cast<int>(lin[1].frame), static_cast<int>(fpt), "RAMP: …one tic after it");
            // Global tic 48 is step 4 tic 0: t = 0.5, and linear at half time is half way.
            eq(value_at(lin, 4 * fps), 128, "RAMP: half way along the span, at half the distance");
            eq(lin[96].valueByte, 0xFF, "RAMP: it arrives at the destination byte the author typed");

            // ⭐ THE TIC GRID RESTARTS EVERY STEP — the property that makes the emitter groove-proof.
            // The arrival is on the AUF STEP's frame (8 × 5512), not 96 tics of 5512/12 from the start
            // (96 × 459), and the truncating division puts 32 frames between those two answers.
            std::printf("       [info] arrival at frame %lld; step 8 begins at %lld, 96 tics span %lld\n",
                        static_cast<long long>(lin[96].frame), static_cast<long long>(8 * fps),
                        static_cast<long long>(96 * fpt));
            eq(static_cast<int>(lin[96].frame), static_cast<int>(8 * fps),
               "⭐ RAMP: the arrival lands on the AUF STEP's frame, not on a tic grid extrapolated "
               "from the start of the span");

            // Nothing backtracks, and no step of the fade is bigger than the 2.66 bytes per tic the
            // arithmetic allows — a ramp that jumped would be audible as a step rather than a fade.
            int rises = 0, biggest = 0;
            for (size_t i = 1; i < lin.size(); ++i) {
                const int d = lin[i].valueByte - lin[i - 1].valueByte;
                if (d > 0) ++rises;
                if (d > biggest) biggest = d;
            }
            eq(rises, 96, "RAMP: every event moves the value UP — an ascending fade never backtracks");
            eq(biggest, 3, "RAMP: …and never by more than the 255/96 the span allows");
        }

        // ── (b) THE CURVE THE AUTHOR TYPED IS THE CURVE THE BUS CARRIES ───────────────────────────
        //
        // §47 checked `automation_shape` as arithmetic. This checks that the emitter reads AUS's byte
        // at all: three phrases differing in one cell, measured at the same frame, half way along.
        {
            auto curved = [&](int curve) {
                return play([&](songcore::Project& p) {
                    fx(p.phrases[0], 0, 1, songcore::FX_VOLUME, 0x00);
                    fx(p.phrases[0], 0, 2, songcore::FX_AUS,    curve);
                    fx(p.phrases[0], 8, 1, songcore::FX_AUF,    0xFF);
                });
            };
            const std::vector<Rec::Cc> in  = only(curved(0x00).ccs, songcore::CC_VOLUME);
            const std::vector<Rec::Cc> out = only(curved(0xFF).ccs, songcore::CC_VOLUME);
            std::printf("       [info] value at half time — ease-in %d, linear %d, ease-out %d\n",
                        value_at(in, 4 * fps), 128, value_at(out, 4 * fps));
            eq(value_at(in,  4 * fps), 32,  "CURVE ON THE BUS: ease-in is 0.5³ of the way at half time");
            eq(value_at(out, 4 * fps), 223, "CURVE ON THE BUS: ease-out is its mirror");

            // ⭐ THE DE-DUP, MEASURED WHERE IT ACTUALLY BITES. A cubic leaves its start slowly: tic n
            // holds round(255·(n/96)³), which is still 0 at n=12 (0.498) and first reaches 1 at n=13
            // (0.633). So after the authored `VOL 00` at frame 0 the ease-in ramp says NOTHING for
            // twelve tics, and its first word is one tic into step 1 — a dozen bus records, queue
            // slots and golden rows saved on every fade, and the ear cannot tell the difference
            // because there is none to tell.
            std::printf("       [info] ease-in: %zu events vs the linear's %zu; first RAMP event at "
                        "frame %lld (step 1 tic 1 = %lld)\n", in.size(), lin.size(),
                        in.size() < 2 ? -1LL : static_cast<long long>(in[1].frame),
                        static_cast<long long>(fps + fpt));
            ok(in.size() < lin.size(),
               "DE-DUP: a cubic's flat start collapses — the same span costs fewer events than linear");
            if (in.size() >= 2) {
                eq(static_cast<int>(in[0].frame), 0,
                   "DE-DUP: (control) the authored start value is still written, at frame 0");
                eq(static_cast<int>(in[1].frame), static_cast<int>(fps + fpt),
                   "DE-DUP: …and the first thing the ramp itself says is at tic 13, not tic 1");
                eq(in[1].valueByte, 1, "DE-DUP: …the first byte that is no longer the start value");
            }
        }

        // ── (c) ⭐⭐ A GROOVE MOVES THE FRAMES AND MUST NOT MOVE THE VALUES ────────────────────────
        //
        // The design decision this whole emitter turns on: `t` is measured in STEPS, not frames. So a
        // fade written across eight steps has covered exactly k/8 of its distance when step k plays,
        // whatever the groove did to the lengths in between — it arrives WITH the note it was written
        // under. Normalising over the span's total frames instead would need a pre-scan of durations
        // the walk has not reached, and would leave every intermediate value depending on the swing.
        //
        // ⚠️ The step boundaries are read out of the NOTE frames, not recomputed from the groove: a
        // check that derived the frames the same way the emitter does could only ever agree with it.
        {
            auto swung = [&](bool groove) {
                return play([&](songcore::Project& p) {
                    if (groove) {
                        p.grooves[0].steps[0] = 14;   // long-short, so no two steps are the same length
                        p.grooves[0].steps[1] = 10;
                    }
                    for (int s = 0; s < 16; ++s) {
                        p.phrases[0].steps[static_cast<size_t>(s)].note       = songcore::Note::C4();
                        p.phrases[0].steps[static_cast<size_t>(s)].instrument = 0;
                    }
                    fx(p.phrases[0], 0, 1, songcore::FX_VOLUME, 0x00);
                    fx(p.phrases[0], 0, 2, songcore::FX_AUS,    0x80);
                    fx(p.phrases[0], 8, 1, songcore::FX_AUF,    0xFF);
                });
            };
            const Run even = swung(false);
            const Run bent = swung(true);

            // The control that the groove is doing anything at all. Without it, "the values match" is
            // a claim about two identical runs.
            const bool gotNotes = even.notes.size() >= 9 && bent.notes.size() >= 9;
            ok(gotNotes, "GROOVE: (control) both takes scheduled a note on every step of the span");
            if (gotNotes) {
                const int64_t evenGap = even.notes[1] - even.notes[0];
                const int64_t bent1   = bent.notes[1] - bent.notes[0];
                const int64_t bent2   = bent.notes[2] - bent.notes[1];
                std::printf("       [info] step lengths — no groove %lld/%lld, groove %lld/%lld\n",
                            static_cast<long long>(evenGap),
                            static_cast<long long>(even.notes[2] - even.notes[1]),
                            static_cast<long long>(bent1), static_cast<long long>(bent2));
                ok(bent1 != bent2 && bent1 != evenGap,
                   "GROOVE: (control) the groove really warped the steps — they are no longer equal, "
                   "nor equal to the ungrooved length");

                // A parameter written on a note's own frame reaches the voice the note replaces, so the
                // emitter offsets a coinciding tic by one — the same +1 STEP 2.3 uses. Read at that
                // frame, hand-derived: round(255 · k/8) for k = 1..8.
                const int want[8] = {32, 64, 96, 128, 159, 191, 223, 255};
                std::string gotEven, gotBent;
                for (int k = 1; k <= 8; ++k) {
                    gotEven += std::to_string(value_at(only(even.ccs, songcore::CC_VOLUME),
                                                       even.notes[static_cast<size_t>(k)] + 1)) + " ";
                    gotBent += std::to_string(value_at(only(bent.ccs, songcore::CC_VOLUME),
                                                       bent.notes[static_cast<size_t>(k)] + 1)) + " ";
                }
                std::string wantStr;
                for (int k = 0; k < 8; ++k) wantStr += std::to_string(want[k]) + " ";
                std::printf("       [info] value at each step boundary — even %s| swung %s| want %s\n",
                            gotEven.c_str(), gotBent.c_str(), wantStr.c_str());
                eqs(gotEven, wantStr,
                    "GROOVE: (control) ungrooved, the fade is k/8 of the way at step k");
                eqs(gotBent, wantStr,
                    "⭐⭐ GROOVE: and SWUNG it is the same k/8 at every step — the groove moved the "
                    "frames and left the values exactly where the author wrote them");
            }
        }

        // ── (d) A MASTER FADE RIDES THE GLOBAL LANE ───────────────────────────────────────────────
        //
        // Same asymmetry §46 proved for the per-step VMV: track-scoped, EngineConsumer's external
        // routing gate is entitled to swallow it. The ramp reads the lane out of the registry rather
        // than assuming the carrying track's.
        {
            const Run master = play([&](songcore::Project& p) {
                fx(p.phrases[0], 0, 1, songcore::FX_VMV, 0xFF);
                fx(p.phrases[0], 0, 2, songcore::FX_AUS, 0x80);
                fx(p.phrases[0], 8, 1, songcore::FX_AUF, 0x00);
            });
            const std::vector<Rec::Cc> mv = only(master.ccs, songcore::CC_MASTER_VOL);
            int onGlobal = 0, falls = 0;
            for (size_t i = 0; i < mv.size(); ++i) {
                if (mv[i].track == songcore::TRACK_GLOBAL) ++onGlobal;
                if (i > 0 && mv[i].valueByte < mv[i - 1].valueByte) ++falls;
            }
            std::printf("       [info] master fade: %zu events, %d on TRACK_GLOBAL, last byte %d\n",
                        mv.size(), onGlobal, mv.empty() ? -1 : mv.back().valueByte);
            eq(static_cast<int>(mv.size()), 97, "MASTER FADE: the same 97 events, on the master CC");
            eq(onGlobal, static_cast<int>(mv.size()),
               "⭐ MASTER FADE: every one of them on TRACK_GLOBAL — on the track lane the external "
               "gate would eat the fade on exactly the tracks it is most often written under");
            eq(falls, 96, "MASTER FADE: a descending fade descends at every event");
            if (!mv.empty()) eq(mv.back().valueByte, 0x00, "MASTER FADE: …and arrives at silence");
        }

        // ── (e) THE FADER RESTORE SURVIVES A CHA THAT ATE THE START EFFECT ────────────────────────
        //
        // ⚠️ VTR/VMV REPLACE the mixer fader and hold, so the host puts the authored value back on
        // stop() — and the flag that asks it to was set only by the PER-STEP effect. Pairing reads the
        // AUTHORED step, so `CHA 01` (probability 0, target slot 1: always fails, always zeroes slot 1)
        // is a phrase where the ramp runs and the per-step VTR never happens. Without the emitter's own
        // arm that is a song which fades a fader down and leaves it there, on the next play too.
        {
            const Run eaten = play([&](songcore::Project& p) {
                fx(p.phrases[0], 0, 1, songcore::FX_VTR, 0x40);
                fx(p.phrases[0], 0, 2, songcore::FX_AUS, 0x80);
                fx(p.phrases[0], 0, 3, songcore::FX_CHA, 0x01);
                fx(p.phrases[0], 8, 1, songcore::FX_AUF, 0xFF);
            });
            const std::vector<Rec::Cc> tv = only(eaten.ccs, songcore::CC_TRACK_VOL);
            std::printf("       [info] CHA-eaten VTR: %zu track-fader events, first at frame %lld, "
                        "restore armed %s\n", tv.size(),
                        tv.empty() ? -1LL : static_cast<long long>(tv[0].frame),
                        eaten.mixerActive ? "yes" : "no");
            ok(!tv.empty(), "CHA+VTR: the ramp still runs — pairing reads the step the author wrote");
            ok(!tv.empty() && tv[0].frame != 0,
               "CHA+VTR: (control) the PER-STEP VTR really was eaten — nothing at the step's own frame");
            ok(eaten.mixerActive,
               "⭐ CHA+VTR: the fader restore is armed anyway, because the emitter arms it from the CC "
               "it sends rather than from an effect that may not have survived the step");

            // ⚠️ The gate in BOTH directions — otherwise "armed" is just a flag that is always true.
            ok(!canon.mixerActive,
               "CHA+VTR: (control) a VOL fade touches no fader and arms no restore");
        }

        // ── (f) AND IT IS AUDIBLE — THE FADE MEASURED IN THE RENDERED AUDIO ───────────────────────
        //
        // Everything above is the event stream. None of it says the engine did anything with it: 97
        // correct CCs landing on a parameter nothing applies is a fade nobody hears. So: a looping tone
        // at a full fader, `VTR FF · AUS 80` on step 0 and `AUF 00` on step 8, measured as RMS over the
        // middle half of each of the eight steps.
        //
        // ⭐ The want is DERIVED, not eyeballed. Over a window where the gain runs linearly from g1 to
        // g2 the RMS is A·√((g1² + g1g2 + g2²)/3) — the mean of a square over a linear ramp — so the
        // predicted ratio to the flat take is that expression, from the fade the author WROTE and not
        // from anything the emitter produced.
        //
        // ⚠️ WHAT THIS INSTRUMENT CANNOT SEE: a one-BLOCK apply lag. `processAudioBlock` snapshots the
        // fader once above its frame loop, so an apply arm writing only the member lands 256 frames (6
        // ms) late — 4.6% of a step, which moves these ratios by well under one part in a hundred and
        // is far inside the tolerance below. Both arms write the snapshot as well as the member
        // (audio-engine.cpp); nothing here would notice if one stopped.
        {
            auto engine = std::make_unique<AudioEngine>();   // ⚠️ HEAP — see §23
            engine->setDeviceSampleRate(44100);
            songcore::SongcoreHost ahost(engine.get(), 44100);

            const fs::path tone = tree.root / "Samples" / "austone.wav";
            {
                std::vector<float> pcm(44100 / 2);
                for (size_t i = 0; i < pcm.size(); ++i) {
                    const double t = static_cast<double>(i) / 44100.0;
                    pcm[i] = static_cast<float>(0.6 * std::sin(2.0 * 3.14159265358979 * 440.0 * t));
                }
                songcore::write_wav_mono(tone.generic_string(), pcm, 44100);
            }

            // Eight RMS readings, one per step of the span, over the middle half of each — far enough
            // from both boundaries that neither the note's attack nor a declick can straddle one.
            auto measure = [&](bool withAus) {
                songcore::Project& p = ahost.edit_project();
                p = songcore::make_default_project();
                p.tempo        = 120;
                p.masterVolume = 0xFF;
                ahost.load_sample(0, tone.generic_string());
                p.instruments[0].loopMode = "fwd";   // ⚠️ a STRING; `= 1` assigns a char and does nothing
                p.instruments[0].volume   = 0xFF;
                p.tracks[0].volume = 0xFF;
                p.tracks[0].chainRefs.assign(256, -1);
                p.tracks[0].chainRefs[0]  = 0;
                p.chains[0].phraseRefs[0] = 0;
                songcore::PhraseStep& n = p.phrases[0].steps[0];
                n.note = songcore::Note::C4();  n.instrument = 0;  n.volume = 0x7F;
                // The fader is written at the value it already holds, so the only thing that can move
                // the level is the fade itself.
                fx(p.phrases[0], 0, 1, songcore::FX_VTR, 0xFF);
                if (withAus) {
                    fx(p.phrases[0], 0, 2, songcore::FX_AUS, 0x80);
                    fx(p.phrases[0], 8, 1, songcore::FX_AUF, 0x00);
                }
                ahost.push_params();
                ahost.play_song(0);

                constexpr int       BLK = 256;
                std::vector<float>  buf(static_cast<size_t>(BLK) * 2);
                std::vector<double> sum(8, 0.0);
                std::vector<int64_t> cnt(8, 0);
                for (int64_t f = 0; f < fps * 8; f += BLK) {
                    ahost.poll();
                    engine->processLiveBlock(buf.data(), BLK, 2, 44100.0f);
                    for (int i = 0; i < BLK; ++i) {
                        const int64_t frame = f + i;
                        const int64_t k     = frame / fps;
                        if (k > 7) continue;
                        const int64_t off = frame - k * fps;
                        if (off < fps / 4 || off >= 3 * fps / 4) continue;
                        const double v = buf[static_cast<size_t>(i) * 2];
                        sum[static_cast<size_t>(k)] += v * v;
                        cnt[static_cast<size_t>(k)]++;
                    }
                }
                ahost.stop();
                std::vector<double> rms(8, 0.0);
                for (int k = 0; k < 8; ++k)
                    if (cnt[static_cast<size_t>(k)])
                        rms[static_cast<size_t>(k)] = std::sqrt(sum[static_cast<size_t>(k)] /
                                                     static_cast<double>(cnt[static_cast<size_t>(k)]));
                return rms;
            };

            const std::vector<double> level = measure(false);   // the control, FIRST
            const std::vector<double> fade  = measure(true);

            // The control: the same eight windows with no AUS must be FLAT. A tone that decays, a voice
            // that gets stolen or a sample that runs out looks exactly like a fade otherwise.
            double lowest = 1e9;
            for (int k = 0; k < 8; ++k) lowest = std::min(lowest, level[static_cast<size_t>(k)] / level[0]);
            std::printf("       [info] fade control (no AUS), 8 step windows:");
            for (int k = 0; k < 8; ++k) std::printf(" %.4f", level[static_cast<size_t>(k)]);
            std::printf("  (lowest %.0f%% of the first)\n", 100.0 * lowest);
            ok(level[0] > 0.02, "FADE: (control) the tone is sounding — the measurement can fail");
            ok(lowest > 0.95,
               "⚠️ FADE: (control) with no AUS the level is FLAT across all eight windows — a drop here "
               "would mean the fade below is measuring the tone");

            int    inBand = 0;
            double worst  = 0.0;
            std::string got, want;
            for (int k = 0; k < 8; ++k) {
                const double g1 = 1.0 - (k + 0.25) / 8.0;      // gain at the window's start
                const double g2 = 1.0 - (k + 0.75) / 8.0;      // …and at its end
                const double predicted = std::sqrt((g1 * g1 + g1 * g2 + g2 * g2) / 3.0);
                const double measured  = fade[static_cast<size_t>(k)] / level[static_cast<size_t>(k)];
                char b[32];
                std::snprintf(b, sizeof b, " %.3f", measured);  got  += b;
                std::snprintf(b, sizeof b, " %.3f", predicted); want += b;
                const double err = std::fabs(measured - predicted);
                if (err > worst) worst = err;
                if (err < 0.05) ++inBand;
            }
            std::printf("       [info] fade, per step —\n                got %s\n               want %s"
                        "  (worst error %.3f)\n", got.c_str(), want.c_str(), worst);
            eq(inBand, 8,
               "⭐⭐ FADE: the rendered level follows the authored curve through all eight steps — the "
               "ratio is derived from the AUDIO and the want from the ramp the author wrote");
            ok(fade[7] < fade[0] * 0.15,
               "FADE: …and by the last step of the span it has fallen to a sixteenth of where it began");
        }
    }

    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    std::printf("%s\n", failures == 0 ? "ALL GREEN" : "RED");
    return failures == 0 ? 0 : 1;
}
