#pragma once

// ─── settings.json ───────────────────────────────────────────────────────────────────────────────
//
// SharedPreferences, as a file. The port plan's §4.5 line — "SharedPreferences → settings.json in
// CONFDIR (tiny key-value store)" — and the thing that makes the SETTINGS screen worth having: a
// setting that resets on every launch is a setting nobody will touch twice.
//
// It lives in pt-ui rather than in the shell for the same reason `ui/filesystem.h` does: it is
// portable (nlohmann + a FileSystem, no POSIX), so `ptdispatch` can round-trip it in a temp directory
// and prove that what was written is what comes back.
//
// ⚠️ ONLY THE ROWS THE SHELL ACTUALLY HAS ARE PERSISTED. The device rows (LAYOUT, OVERLAY, BTN SOUND,
// BTN VIBRO) are Android's, they are caps-gated off here, and writing zeroes for them would be
// inventing a value for a question this platform never asked. If Android ever converges onto this UI it
// brings its own answers with it — and its own keys.
//
// ⚠️ **RESUME MOVED OUT OF THAT LIST IN S10, AND THAT IS THE ONE LINE OF THIS FILE WORTH RE-READING.**
// The rule is "persist the rows this platform HAS", and S10 gave the shell a row it did not have — so
// the very same session that flips `PlatformCaps::sdl().autosave` on has to add the key here, or the
// setting silently resets to ASK on every single launch.
//
// That is not a hypothetical coupling: it is EXACTLY the bug S9 shipped and then found, one row later.
// S9's `theme` was stored by NAME, which was lossless until the layer above it (the theme editor) made
// a palette something you could invent — and **no tool in the ladder quits and relaunches the app**, so
// the only thing that can ever catch this class is a save → load round trip. `ptdispatch` §28 carries
// one for RESUME for precisely that reason, and its control fires.
//
// Missing file, missing key, unparseable value: the DEFAULT stays. A settings file is not a document,
// and losing one is not worth a dialog — it is worth the factory settings and a working app.

#include "ui/filesystem.h"
#include "ui/modules/settings_editor.h"
#include "ui/theme.h"

namespace pt::ui {

/**
 * Read `settings.json` into `values` and `theme`.
 *
 * ⚠️ `theme` carries its FULL PALETTE, not just its name — see the long note in the .cpp. Until S9 this
 * stored `theme.name` and rebuilt the palette from the four built-ins, which was lossless right up until
 * the THEME EDITOR made a palette something you can invent. A pre-S9 file (a `theme` string and no
 * `appTheme` object) still loads: the reader falls back to the by-name path.
 *
 * The visualizer is the theme's FIELD but the user's CHOICE, so it rides across a theme load rather than
 * coming from it — exactly as Android carries it across a theme change.
 *
 * Returns false when there was no file to read. That is not an error: it is the first launch.
 */
bool load_settings(FileSystem& fs, SettingsValues& values, Theme& theme);

/** Write `settings.json` unconditionally. */
bool save_settings(FileSystem& fs, const SettingsValues& values, const Theme& theme);

/** What `save_settings_if_changed` actually did — an ENUM, not a bool, for S10's reason: a caller that
 *  cannot tell "nothing needed writing" from "the write FAILED" reports a full SD card by saying
 *  nothing at all. That is the one answer you are reading the line to find out. */
enum class SettingsWrite { UNCHANGED, SAVED, FAILED };

/**
 * Write `settings.json` **only if its contents would differ** — the verb the shell calls on exit.
 *
 * ⚠️⚠️ THIS EXISTS INSTEAD OF A DIRTY FLAG, AND THAT IS THE WHOLE POINT.
 *
 * The shell used to write on exit `if (state.settingsDirty)`, and the ONLY thing that ever set that flag
 * was `apply_edit`'s SETTINGS arm. The THEME EDITOR has no CursorContext, so it never goes through
 * `apply_edit` — its A+DPAD arms mutate `theme` directly — and neither does LOAD THEME. So a session
 * whose only change was the palette armed nothing and **wrote nothing**, and eighteen dialled colours
 * were gone on the next launch. It was intermittent, too: nudge any SETTINGS row in the same sitting and
 * the flag was set, the write happened, and it carried the theme along with it.
 *
 * That is S9's headline bug in its second body. S9 fixed WHAT gets written (the palette, not its name)
 * and left WHETHER it gets written resting on every future mutation site remembering to say so — which is
 * the same "a rule you must remember at N call sites is a rule you will forget once" that Kotlin's modal
 * predicate warns about, and it was forgotten within one session of being written.
 *
 * So the question is answered from the DATA rather than from a flag: *do the bytes on disk already say
 * what memory says?* A new screen that touches the theme, a new key, a new SettingsValues field — all
 * covered, by construction, with nothing to remember. It costs one ~200-byte read and one serialize per
 * quit, and it keeps the property the flag was actually there for: a settings write happens on EXIT, never
 * per keystroke (holding A+UP fires an edit every 100 ms, and one file write per repeat is an SD card
 * hammered for a value that is still moving).
 */
SettingsWrite save_settings_if_changed(FileSystem& fs, const SettingsValues& values, const Theme& theme);

}  // namespace pt::ui
