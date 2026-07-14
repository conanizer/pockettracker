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
#include "ui/settings_row_layout.h"
#include "ui/settings_store.h"
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

        // The shell has EIGHT of the thirteen; Android has all thirteen (in a debug build).
        state.caps = PlatformCaps::sdl(true);
        int shellRows = 0;
        for (int r = 0; r < SETTINGS_ROW_COUNT; ++r)
            if (settings_row_visible(static_cast<SettingsRow>(r), state.caps)) ++shellRows;
        eq(shellRows, 8, "SETTINGS[sdl]: eight rows — SCALING, KB, CURSOR, PREV, VIZ, THEME, TPL, TRACE");

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
        state.settingsDirty  = false;

        state.settingsCursorRow    = static_cast<int>(SettingsRow::NOTE_PREV);
        state.settingsCursorColumn = 1;
        const bool before = state.settings.notePreviewEnabled;
        dispatch.on_a_up();

        ok(state.settings.notePreviewEnabled != before, "SETTINGS: A+UP toggles NOTE PREV");
        ok(!state.project_dirty(),
           "⚠️ SETTINGS: …and does NOT dirty the PROJECT — a setting is not a song");
        ok(state.settingsDirty, "SETTINGS: …but it does mark settings.json as needing a write");
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

    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    std::printf("%s\n", failures == 0 ? "ALL GREEN" : "RED");
    return failures == 0 ? 0 : 1;
}
