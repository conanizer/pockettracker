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

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "songcore/host.h"
#include "ui/app_state.h"
#include "ui/cursor_move.h"
#include "ui/input_dispatcher.h"
#include "ui/navigation.h"

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

int main() {
    songcore::SongcoreHost host(nullptr, 44100);   // ⚠️ no engine, on purpose

    AppState state;
    state.project = &host.edit_project();
    InputDispatcher dispatch(state, host);

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

    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    std::printf("%s\n", failures == 0 ? "ALL GREEN" : "RED");
    return failures == 0 ? 0 : 1;
}
