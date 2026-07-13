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

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "songcore/host.h"
#include "ui/app_state.h"
#include "ui/cursor_move.h"
#include "ui/input_dispatcher.h"
#include "ui/navigation.h"
#include "ui/std_filesystem.h"

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

        // Any OTHER row: SELECT does nothing. (The two INP EQ rows open the EQ overlay on Android;
        // it does not exist here yet, so they must be no-ops rather than anything clever.)
        state.effectsCursorRow = EffectModule::ROW_REV_EQ;
        const int eqBefore = p.reverbInputEq;
        const bool syncBefore = p.delaySync;
        dispatch.on_select();
        eq(p.reverbInputEq, eqBefore, "SELECT on an INP EQ row is a no-op (no overlay yet)");
        ok(p.delaySync == syncBefore, "SELECT on an INP EQ row does not touch delay sync");
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
        state.cursorRemember = false;   // REFRESH: Android's default

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

        // EFFECTS is deliberately absent from the reset, on both platforms.
        state.currentScreen    = ScreenType::EFFECTS;
        state.effectsCursorRow = 5;
        go_to_screen(state, NavResult{ScreenType::MIXER, 2});
        go_to_screen(state, NavResult{ScreenType::EFFECTS, 2});
        eq(state.effectsCursorRow, 5, "REFRESH: EFFECTS' row PERSISTS (Kotlin does not reset it either)");

        // …and in REMEMBER mode nothing is reset at all.
        state.cursorRemember = true;
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

    state.cursorRemember = false;

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

    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    std::printf("%s\n", failures == 0 ? "ALL GREEN" : "RED");
    return failures == 0 ? 0 : 1;
}
