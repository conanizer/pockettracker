// ─── ptmapper — the COMBO MATRIX, which every other tool starts one layer below ──────────────────
//
// `ui/button_mapper.h`'s `handle_button` answers one question — "which of the ~30 named dispatcher
// handlers does this press mean?" — and its own comment says **the order of these checks IS the
// specification**. Until convergence C0.1 nothing measured that order, on either side of the port:
//
//   • `ptdispatch` calls `d.on_l_b_a()` and friends DIRECTLY. Every assertion it makes begins after
//     the matrix has already decided; swap two arms below and it stays all green.
//   • `ptinput` is lower still — it drives the modules' `handle_input`, two layers down.
//   • Kotlin's `InputMapper.handleButtonAction` has no test either (testdata/README.md §3 lists it as
//     uncovered on BOTH sides), which is why this file cannot be a conformance tool.
//
// ⚠️ **SO THIS IS ptdispatch'S KIND OF TOOL, NOT ptplay'S: hand-written assertions, no golden.** It
// encodes what the author believed after reading `ButtonHandlers.kt:402`, and that is a weaker claim
// than a byte-compare. Each section below names the Kotlin lines it was transcribed from. A
// disagreement between this file and the code is an open question, not a bug report.
//
// What it CAN prove, and what the matrix has never had: that the order does not change by accident.
// C1 hands this same matrix to Android, and a reordering there would present as "a mis-press" —
// the bug class the modal rule in input_dispatcher.h already warns about, in the one block of the
// input chain that had no coverage at all.
//
// ── ⚠️ THE MODS ARE DERIVED, NEVER HAND-WRITTEN, AND THAT IS THE WHOLE FIDELITY ARGUMENT ─────────
//
// `ButtonMods` is a snapshot of what was held AT THE INSTANT of the event — and `SdlInput::press`
// sets the held flag BEFORE it snapshots, so **the snapshot includes the button being pressed**. A
// plain A press therefore arrives with `mods.a == true`, which is exactly what makes the
// `on_stop_preview` guard (`!m.a`) skip it and what forces the SELECT/START arms to be checked ahead
// of the no-modifier guard.
//
// A test that hand-wrote `mods.a = false` on an A press would be testing a gesture the app can never
// produce, and would "pass" while proving nothing — §26's shape from the guardrails. So `Rig` below
// keeps a held-set and derives the mods from it, transcribed from `SdlInput::press`/`release`
// (shell/sdl-input.cpp): set the flag, then snapshot. The gestures here are the ones a user's thumbs
// can actually make.

#include <cstdio>
#include <string>
#include <vector>

#include "ui/button_mapper.h"
#include "ui/buttons.h"

using namespace pt::ui;

static int checks = 0, failures = 0;

static void eqs(const std::string& got, const std::string& want, const std::string& what) {
    ++checks;
    if (got != want) {
        ++failures;
        std::printf("  [FAIL] %s\n           got  %s\n           want %s\n", what.c_str(),
                    got.c_str(), want.c_str());
    }
}

// ─── The recording dispatcher ────────────────────────────────────────────────────────────────────
//
// Every handler `handle_button` can call, recording its own name instead of doing anything. This is
// what the template parameter on `handle_button` bought: the shipped build instantiates it with the
// real `InputDispatcher` and generates identical code, and this instantiates it with a stub that can
// be asked what happened.
//
// `deferA` / `deferB` stand in for the dispatcher's `defer_a_to_release()` / `defer_b_to_release()` —
// which really do depend on which CELL the cursor is on (an INSTRUMENT NAME, an EQ cell). Whether
// those predicates pick the right cells is ptdispatch's question and ptinput's; whether the MATRIX
// does the right thing given the answer is this file's, and the two are cleanly separable.
struct Rec {
    std::vector<std::string> log;
    bool                     deferA = false;
    bool                     deferB = false;

#define PT_REC(name) void name() { log.push_back(#name); }
    PT_REC(on_dpad_up)   PT_REC(on_dpad_down)  PT_REC(on_dpad_left)  PT_REC(on_dpad_right)
    PT_REC(on_a_up)      PT_REC(on_a_down)     PT_REC(on_a_left)     PT_REC(on_a_right)
    PT_REC(on_a_b)       PT_REC(on_a_a)        PT_REC(on_a_released)
    PT_REC(on_b_left)    PT_REC(on_b_right)    PT_REC(on_b_up)       PT_REC(on_b_down)
    PT_REC(on_r_up)      PT_REC(on_r_down)     PT_REC(on_r_left)     PT_REC(on_r_right)
    PT_REC(on_l_b)       PT_REC(on_l_a)        PT_REC(on_l_r)        PT_REC(on_l_b_a)
    PT_REC(on_select_a)  PT_REC(on_select_b)   PT_REC(on_select_r)
    PT_REC(on_button_a)  PT_REC(on_button_b)   PT_REC(on_select)     PT_REC(on_start)
    PT_REC(on_stop_preview)
#undef PT_REC

    bool defer_a_to_release() const { return deferA; }
    bool defer_b_to_release() const { return deferB; }
};

// ─── The rig: a held-set, driven exactly as SdlInput drives it ───────────────────────────────────
struct Rig {
    Rec         d;
    MapperState ms;
    bool        held[static_cast<size_t>(Button::COUNT)] = {false};

    static size_t idx(Button b) { return static_cast<size_t>(b); }

    /** `SdlInput::mods_now` — the five modifier flags, read off the held-set. */
    ButtonMods mods() const {
        ButtonMods m;
        m.a      = held[idx(Button::A)];
        m.b      = held[idx(Button::B)];
        m.l      = held[idx(Button::L_SHIFT)];
        m.r      = held[idx(Button::R_SHIFT)];
        m.select = held[idx(Button::SELECT)];
        return m;
    }

    /**
     * ⚠️ The default clock is 5000, NOT 0, and that is load-bearing rather than arbitrary — see
     * §8's BOOT WINDOW case. `SDL_GetTicks64()` counts from `SDL_Init`, and `MapperState.lastAPress`
     * starts at 0, so at `now < 300` the double-tap test `now - lastAPress < 300` is satisfied by a
     * FIRST press. A rig defaulting to 0 would put every A case in this file inside that window and
     * quietly measure the boot edge instead of the gesture it names.
     */
    static constexpr uint64_t DEFAULT_NOW = 5000;

    /** `SdlInput::press` — de-dup a held button, set the flag, THEN snapshot. Order is the point. */
    void press(Button b, uint64_t now = DEFAULT_NOW) {
        if (held[idx(b)]) return;
        held[idx(b)] = true;
        handle_button(ButtonEvent{b, ButtonAction::PRESSED, mods()}, d, ms, now);
    }

    /** `SdlInput::release` — clear the flag, THEN snapshot. */
    void release(Button b, uint64_t now = DEFAULT_NOW) {
        if (!held[idx(b)]) return;
        held[idx(b)] = false;
        handle_button(ButtonEvent{b, ButtonAction::RELEASED, mods()}, d, ms, now);
    }

    /**
     * `SdlInput::tick`'s synthetic repeat: a PRESSED for a button that is already down, carrying the
     * modifiers as they stand NOW. It deliberately bypasses the press() de-dup, because the repeat is
     * generated rather than received — which is how holding A+UP keeps editing.
     */
    void repeat(Button b, uint64_t now = DEFAULT_NOW) {
        handle_button(ButtonEvent{b, ButtonAction::PRESSED, mods()}, d, ms, now);
    }

    /** What fired since the last `take()`, comma-joined; clears the log. */
    std::string take() {
        std::string out;
        for (const std::string& s : d.log) {
            if (!out.empty()) out += ",";
            out += s;
        }
        d.log.clear();
        return out;
    }
};

/** One gesture from a clean rig, so no case can inherit another's latch or double-tap window. */
#define CASE(rig) Rig rig; (void)rig

int main() {
    std::printf("ptmapper - the combo matrix (ui/button_mapper.h), hand-written assertions\n\n");

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // §1  The plain presses  (ButtonHandlers.kt:402 tail — the no-modifier guard and the two
    //     explicit SELECT/START arms ahead of it)
    // ═════════════════════════════════════════════════════════════════════════════════════════════
    std::printf("[1] plain presses\n");
    {
        CASE(r); r.press(Button::DPAD_UP);
        eqs(r.take(), "on_stop_preview,on_dpad_up", "plain UP silences the audition, then moves");
    }
    {
        CASE(r); r.press(Button::DPAD_DOWN);
        eqs(r.take(), "on_stop_preview,on_dpad_down", "plain DOWN");
    }
    {
        CASE(r); r.press(Button::DPAD_LEFT);
        eqs(r.take(), "on_stop_preview,on_dpad_left", "plain LEFT");
    }
    {
        CASE(r); r.press(Button::DPAD_RIGHT);
        eqs(r.take(), "on_stop_preview,on_dpad_right", "plain RIGHT");
    }
    {
        // ⚠️ NO on_stop_preview: A's own press has already set mods.a, and the guard is `!m.a`.
        // An edit should stay audible — see the matrix's comment on that line.
        CASE(r); r.press(Button::A);
        eqs(r.take(), "on_button_a", "plain A does NOT silence the audition (its own press sets m.a)");
    }
    {
        CASE(r); r.press(Button::B);
        eqs(r.take(), "on_stop_preview,on_button_b", "plain B silences, then acts");
    }
    {
        // SELECT is checked EXPLICITLY ahead of the no-modifier guard, because its own press sets
        // m.select and a guard rejecting any modifier would swallow it.
        CASE(r); r.press(Button::SELECT);
        eqs(r.take(), "on_stop_preview,on_select", "plain SELECT survives its own modifier flag");
    }
    {
        // START is exempt from the silence — it STARTS playback.
        CASE(r); r.press(Button::START);
        eqs(r.take(), "on_start", "plain START is exempt from on_stop_preview");
    }
    {
        CASE(r); r.press(Button::L_SHIFT);
        eqs(r.take(), "on_stop_preview", "L alone is a modifier: it silences and does nothing else");
    }
    {
        CASE(r); r.press(Button::R_SHIFT);
        eqs(r.take(), "on_stop_preview", "R alone likewise");
    }

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // §2  A + … — the core editing gesture  (ButtonHandlers.kt:470-505)
    // ═════════════════════════════════════════════════════════════════════════════════════════════
    std::printf("[2] A + ...\n");
    {
        CASE(r); r.press(Button::A); r.take();
        r.press(Button::DPAD_UP);
        eqs(r.take(), "on_a_up", "A+UP edits (+1) and stays audible");
    }
    {
        CASE(r); r.press(Button::A); r.take();
        r.press(Button::DPAD_DOWN);
        eqs(r.take(), "on_a_down", "A+DOWN edits (-1)");
    }
    {
        CASE(r); r.press(Button::A); r.take();
        r.press(Button::DPAD_RIGHT);
        eqs(r.take(), "on_a_right", "A+RIGHT edits fast (+16)");
    }
    {
        CASE(r); r.press(Button::A); r.take();
        r.press(Button::DPAD_LEFT);
        eqs(r.take(), "on_a_left", "A+LEFT edits fast (-16)");
    }
    {
        // ⚠️ ORDER: A+B must be resolved BEFORE the plain-B arm, or a delete would read as an insert.
        CASE(r); r.press(Button::A); r.take();
        r.press(Button::B);
        eqs(r.take(), "on_a_b", "A+B clears - resolved ahead of the plain-B arm");
    }
    {
        // The repeat fires with the modifiers as they stand NOW, which is what lets a UP already
        // repeating turn into an edit when A comes down on top of it (SdlInput::tick's comment).
        CASE(r); r.press(Button::DPAD_UP); r.take();
        r.press(Button::A); r.take();
        r.repeat(Button::DPAD_UP);
        eqs(r.take(), "on_a_up", "a UP repeat becomes an EDIT once A is held");
    }

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // §3  B + DPAD — which item am I looking at  (ButtonHandlers.kt:513-540)
    // ═════════════════════════════════════════════════════════════════════════════════════════════
    std::printf("[3] B + DPAD\n");
    {
        CASE(r); r.press(Button::B); r.take();
        r.press(Button::DPAD_LEFT);
        eqs(r.take(), "on_stop_preview,on_b_left", "B+LEFT walks to the previous item");
    }
    {
        CASE(r); r.press(Button::B); r.take();
        r.press(Button::DPAD_RIGHT);
        eqs(r.take(), "on_stop_preview,on_b_right", "B+RIGHT walks to the next");
    }
    {
        CASE(r); r.press(Button::B); r.take();
        r.press(Button::DPAD_UP);
        eqs(r.take(), "on_stop_preview,on_b_up", "B+UP pages the song");
    }
    {
        CASE(r); r.press(Button::B); r.take();
        r.press(Button::DPAD_DOWN);
        eqs(r.take(), "on_stop_preview,on_b_down", "B+DOWN pages the song");
    }
    {
        // A wins over B: the A+DPAD block is tested first, and B+DPAD explicitly requires !m.a.
        CASE(r); r.press(Button::A); r.take(); r.press(Button::B); r.take();
        r.press(Button::DPAD_UP);
        eqs(r.take(), "on_a_up", "A+B+UP is an EDIT, not an item walk - A's block is tested first");
    }

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // §4  SELECT + … — the browser's file-management chords  (ButtonHandlers.kt:621)
    // ═════════════════════════════════════════════════════════════════════════════════════════════
    std::printf("[4] SELECT + ...\n");
    {
        CASE(r); r.press(Button::SELECT); r.take();
        r.press(Button::A);
        eqs(r.take(), "on_select_a", "SELECT+A renames");
    }
    {
        CASE(r); r.press(Button::SELECT); r.take();
        r.press(Button::B);
        eqs(r.take(), "on_stop_preview,on_select_b", "SELECT+B deletes");
    }
    {
        // ⚠️ THE `(!m.r || e.button == R_SHIFT)` SUBTLETY, and the reason it is not a flat `!m.r`:
        // R's OWN press has already set m.r by the time the event is snapshotted, so a flat guard
        // would reject the very chord it is trying to match. This case is that guard's only witness.
        CASE(r); r.press(Button::SELECT); r.take();
        r.press(Button::R_SHIFT);
        eqs(r.take(), "on_stop_preview,on_select_r", "SELECT+R makes a folder - R may be held when R IS the button");
    }
    {
        // …and the other half of the same guard: with R ALREADY down, SELECT+A is NOT a rename.
        CASE(r); r.press(Button::R_SHIFT); r.take(); r.press(Button::SELECT); r.take();
        r.press(Button::A);
        eqs(r.take(), "", "R held first: SELECT+A is consumed by the R block, not a rename");
    }

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // §5  L + … , and the ORDER that makes clone beat paste  (ButtonHandlers.kt:545-620)
    // ═════════════════════════════════════════════════════════════════════════════════════════════
    std::printf("[5] L + ...\n");
    {
        CASE(r); r.press(Button::L_SHIFT); r.take();
        r.press(Button::B);
        eqs(r.take(), "on_stop_preview,on_l_b", "L+B enters selection");
    }
    {
        CASE(r); r.press(Button::L_SHIFT); r.take();
        r.press(Button::A);
        eqs(r.take(), "on_l_a", "L+A cuts or pastes");
    }
    {
        // ⚠️⚠️ THE ORDERING CASE THIS FILE EXISTS FOR. L+B+A must be tested BEFORE the L+... block
        // or the A would fall into `case Button::A: d.on_l_a()` and PASTE where the user asked to
        // CLONE. Nothing else in the tree can see this: ptdispatch calls on_l_b_a() directly.
        CASE(r); r.press(Button::L_SHIFT); r.take(); r.press(Button::B); r.take();
        r.press(Button::A);
        eqs(r.take(), "on_l_b_a", "L+B+A CLONES - the arm is ahead of L+A, which would have pasted");
    }
    {
        // Consumed, and SILENTLY: START is exempt from on_stop_preview, so a reserved START chord
        // fires nothing at all. (This assertion said "on_stop_preview" when it was first written —
        // transcribed from the section's theme rather than from the guard three lines above it.
        // ptdispatch's header warns about exactly this; it took one run to prove the warning.)
        CASE(r); r.press(Button::L_SHIFT); r.take();
        r.press(Button::START);
        eqs(r.take(), "", "L+START is reserved and fires NOTHING - START never silences");
    }
    {
        CASE(r); r.press(Button::L_SHIFT); r.take();
        r.press(Button::DPAD_UP);
        eqs(r.take(), "on_stop_preview", "L+DPAD is the browser's; no screen handles it yet");
    }

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // §6  L+R — leave selection mode, and the chords it consumes
    // ═════════════════════════════════════════════════════════════════════════════════════════════
    std::printf("[6] L+R\n");
    {
        CASE(r); r.press(Button::L_SHIFT); r.take();
        r.press(Button::R_SHIFT);
        eqs(r.take(), "on_stop_preview,on_l_r", "L then R deselects (R IS the button)");
    }
    {
        CASE(r); r.press(Button::R_SHIFT); r.take();
        r.press(Button::L_SHIFT);
        eqs(r.take(), "on_stop_preview,on_l_r", "R then L deselects too - the chord is symmetric");
    }
    {
        CASE(r); r.press(Button::L_SHIFT); r.take(); r.press(Button::R_SHIFT); r.take();
        r.press(Button::A);
        eqs(r.take(), "", "L+R+A is consumed, NOT a clone or a paste");
    }
    {
        CASE(r); r.press(Button::L_SHIFT); r.take(); r.press(Button::R_SHIFT); r.take();
        r.press(Button::B);
        eqs(r.take(), "on_stop_preview", "L+R+B is consumed (the silence still fires - it is above the block)");
    }
    {
        CASE(r); r.press(Button::L_SHIFT); r.take(); r.press(Button::R_SHIFT); r.take();
        r.press(Button::SELECT);
        eqs(r.take(), "on_stop_preview", "L+R+SELECT is consumed");
    }

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // §7  R + DPAD — screen navigation, and the edits it must NOT let through
    // ═════════════════════════════════════════════════════════════════════════════════════════════
    std::printf("[7] R + DPAD\n");
    {
        CASE(r); r.press(Button::R_SHIFT); r.take();
        r.press(Button::DPAD_UP);
        eqs(r.take(), "on_stop_preview,on_r_up", "R+UP changes screen");
    }
    {
        CASE(r); r.press(Button::R_SHIFT); r.take();
        r.press(Button::DPAD_DOWN);
        eqs(r.take(), "on_stop_preview,on_r_down", "R+DOWN");
    }
    {
        CASE(r); r.press(Button::R_SHIFT); r.take();
        r.press(Button::DPAD_LEFT);
        eqs(r.take(), "on_stop_preview,on_r_left", "R+LEFT");
    }
    {
        CASE(r); r.press(Button::R_SHIFT); r.take();
        r.press(Button::DPAD_RIGHT);
        eqs(r.take(), "on_stop_preview,on_r_right", "R+RIGHT");
    }
    {
        // ⚠️ R+A is CONSUMED. Holding R to change screens must not also fire an edit underneath.
        CASE(r); r.press(Button::R_SHIFT); r.take();
        r.press(Button::A);
        eqs(r.take(), "", "R+A is consumed - no edit fires under a screen change");
    }
    {
        CASE(r); r.press(Button::R_SHIFT); r.take();
        r.press(Button::B);
        eqs(r.take(), "on_stop_preview", "R+B is consumed");
    }
    {
        CASE(r); r.press(Button::R_SHIFT); r.take();
        r.press(Button::START);
        eqs(r.take(), "", "R+START is consumed (START is exempt from the silence)");
    }

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // §8  The A,A double-tap  (ButtonHandlers.kt:688-695) — a function of the CLOCK
    // ═════════════════════════════════════════════════════════════════════════════════════════════
    std::printf("[8] the A,A double-tap (300 ms)\n");
    {
        CASE(r);
        r.press(Button::A, 1000);   r.release(Button::A, 1010);  r.take();
        r.press(Button::A, 1200);   // 200 ms later: inside the window
        eqs(r.take(), "on_a_a", "A,A within 300 ms inserts the next UNUSED item");
    }
    {
        CASE(r);
        r.press(Button::A, 1000);   r.release(Button::A, 1010);  r.take();
        r.press(Button::A, 1400);   // 400 ms later: outside it
        eqs(r.take(), "on_button_a", "A,A after 300 ms is two plain As");
    }
    {
        // The boundary itself: `now - lastAPress < 300` is STRICT, so exactly 300 is a MISS.
        CASE(r);
        r.press(Button::A, 1000);   r.release(Button::A, 1010);  r.take();
        r.press(Button::A, 1300);
        eqs(r.take(), "on_button_a", "exactly 300 ms is OUTSIDE the window (the < is strict)");
    }
    {
        // ⚠️ A TRIPLE tap is not two double-taps: the second A clears lastAPress precisely so the
        // third cannot read as the second half of another one.
        CASE(r);
        r.press(Button::A, 1000);  r.release(Button::A, 1010);
        r.press(Button::A, 1100);  r.release(Button::A, 1110);  r.take();
        r.press(Button::A, 1200);
        eqs(r.take(), "on_button_a", "a TRIPLE tap does not read as two double-taps");
    }
    {
        // ⚠️⚠️ **A KNOWN DIVERGENCE FROM KOTLIN, PINNED HERE RATHER THAN FIXED.** This asserts what
        // the code DOES today, and it is not what the app it was ported from does.
        //
        //   `MapperState.lastAPress` starts at 0, and the test is `now - lastAPress < 300`.
        //   Kotlin reads `System.currentTimeMillis()` (ButtonHandlers.kt:687) — absolute epoch ms,
        //   ~1.7e12 — so `now - 0` is astronomically over the window and a FIRST A press is always
        //   `onButtonA`. The shell passes `SDL_GetTicks64()`, which counts from `SDL_Init`, so for
        //   the first 300 ms `now - 0 < 300` holds and the first A press fires `on_a_a` instead:
        //   INSERT THE NEXT UNUSED chain/phrase, where Kotlin inserts the LAST-EDITED one.
        //
        // Pre-dates convergence C0.1 — it arrived with the clock substitution in Phase 3 S1 and the
        // transplant carried it across unchanged (deliberately: that move is provably verbatim, and
        // a behaviour change hidden inside it would have cost exactly that proof).
        //
        // Reachability is narrow but NOT theoretical: the press must land within 300 ms of
        // `SDL_Init`, and P4b established that a launcher can inject phantom input at boot.
        //
        // ⭐ **This case is deliberately red-on-fix.** Correct the clock and it fails, pointing here
        // — which is the intended way to retire it, not a regression.
        CASE(r);
        r.press(Button::A, 100);
        eqs(r.take(), "on_a_a",
            "DIVERGENCE: an A press inside the first 300 ms after SDL_Init reads as a double-tap "
            "(Kotlin's absolute clock cannot)");
    }

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // §9  The DEFERRED-A latch  (ButtonHandlers.kt:683 / :426)
    // ═════════════════════════════════════════════════════════════════════════════════════════════
    std::printf("[9] the deferred-A latch\n");
    {
        // On a cell whose A OPENS something: nothing on the press, the open on the release.
        CASE(r); r.d.deferA = true;
        r.press(Button::A, 1000);
        eqs(r.take(), "", "a deferred A fires NOTHING on the press");
        r.release(Button::A, 1010);
        eqs(r.take(), "on_button_a,on_a_released", "...and opens on the RELEASE");
    }
    {
        // ⚠️ The cancel: the gesture turned out to be a combo, so the held-open must NOT fire.
        // Without this, holding A on INSTRUMENT NAME and pressing B would open the keyboard and land
        // the reset on top of it.
        CASE(r); r.d.deferA = true;
        r.press(Button::A, 1000); r.take();
        r.press(Button::B, 1005);
        eqs(r.take(), "on_a_b", "A+B on a deferred cell is a CLEAR");
        r.release(Button::A, 1010);
        eqs(r.take(), "on_a_released", "...and the deferred open is CANCELLED - only the FX commit fires");
    }
    {
        CASE(r); r.d.deferA = true;
        r.press(Button::A, 1000); r.take();
        r.press(Button::DPAD_UP, 1005); r.take();
        r.release(Button::A, 1010);
        eqs(r.take(), "on_a_released", "A+UP cancels the deferred open too");
    }
    {
        // ⚠️ A deferred cell is never a double-tap cell, and `lastAPress` is CLEARED on the defer so
        // the NEXT A press cannot read as the second half of a double-tap that never happened.
        CASE(r); r.d.deferA = true;
        r.press(Button::A, 1000); r.release(Button::A, 1010); r.take();
        r.d.deferA = false;                    // the cursor moved to an ordinary cell
        r.press(Button::A, 1100);              // 100 ms later - inside the window, had it been armed
        eqs(r.take(), "on_button_a", "a deferred A does not arm a double-tap for the next press");
    }
    {
        // The FX helper commits on the release of A whether or not anything was deferred.
        CASE(r);
        r.press(Button::A, 1000); r.take();
        r.release(Button::A, 1010);
        eqs(r.take(), "on_a_released", "an undeferred A release still commits the FX helper");
    }

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // §10  The DEFERRED-B latch — the EQ editor  (ButtonHandlers.kt:707 / :435)
    // ═════════════════════════════════════════════════════════════════════════════════════════════
    std::printf("[10] the deferred-B latch (the EQ editor)\n");
    {
        CASE(r); r.d.deferB = true;
        r.press(Button::B, 1000);
        eqs(r.take(), "on_stop_preview", "a deferred B fires no CLOSE on the press");
        r.release(Button::B, 1010);
        eqs(r.take(), "on_button_b", "...and closes on the RELEASE");
    }
    {
        // ⚠️ THE BUG THE LATCH EXISTS FOR: B is both the CLOSE and the modifier of the slot cycle.
        // Fire the close on B's own press and B+RIGHT is unreachable - you would be back on the mixer
        // before RIGHT ever arrived.
        CASE(r); r.d.deferB = true;
        r.press(Button::B, 1000); r.take();
        r.press(Button::DPAD_RIGHT, 1005);
        eqs(r.take(), "on_stop_preview,on_b_right", "B+RIGHT cycles the EQ slot");
        r.release(Button::B, 1010);
        eqs(r.take(), "", "...and the editor does NOT close when B comes up");
    }
    {
        CASE(r); r.d.deferB = true;
        r.press(Button::B, 1000); r.take();
        r.press(Button::DPAD_LEFT, 1005); r.take();
        r.release(Button::B, 1010);
        eqs(r.take(), "", "B+LEFT cancels the deferred close too");
    }
    {
        // The two latches are independent: a deferred B must not swallow a deferred A, or vice versa.
        CASE(r); r.d.deferA = true; r.d.deferB = true;
        r.press(Button::A, 1000); r.take();
        r.release(Button::A, 1010);
        eqs(r.take(), "on_button_a,on_a_released", "the A latch is unaffected by a live B latch");
    }

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // §11  Releases fire nothing else — every RELEASE that is not A or B is inert
    // ═════════════════════════════════════════════════════════════════════════════════════════════
    std::printf("[11] inert releases\n");
    {
        CASE(r); r.press(Button::DPAD_UP); r.take();
        r.release(Button::DPAD_UP);
        eqs(r.take(), "", "releasing a DPAD does nothing");
    }
    {
        CASE(r); r.press(Button::START); r.take();
        r.release(Button::START);
        eqs(r.take(), "", "releasing START does nothing");
    }
    {
        CASE(r); r.press(Button::SELECT); r.take();
        r.release(Button::SELECT);
        eqs(r.take(), "", "releasing SELECT does nothing");
    }
    {
        // B's release is inert unless the latch is set - the mirror of A's, which always commits FX.
        CASE(r); r.press(Button::B); r.take();
        r.release(Button::B);
        eqs(r.take(), "", "an undeferred B release is inert");
    }

    std::printf("\n%d checks, %d failures\n", checks, failures);
    if (failures == 0) std::printf("ptmapper: OK\n");
    return failures == 0 ? 0 : 1;
}
