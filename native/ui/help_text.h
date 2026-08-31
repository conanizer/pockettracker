#pragma once

// ─── HELP ON SELECT — the text, and what the cursor is standing on ───────────────────────────────
//
// Tap SELECT and the visualizer strip becomes three lines explaining the cell under the cursor. This
// header is the whole of the WRITING half: a topic for every explainable place in the app, the three
// lines each one shows, and the lookup that turns a cursor position into a topic.
//
// It is PURE — no canvas, no theme, no drawing. That split is `fx_helper.h`'s, and for the same
// reason: the text can be checked, and the lookup driven, without linking a renderer.
//
// ── THE FOUR RULES THE TEXT OBEYS, AND WHY TWO OF THEM ARE COMPILER-CHECKED ──────────────────────
//
//  1. **Three lines, and the first one NAMES the thing** — "VOL: how loud this step is". The name is
//     what the reader is hunting for; the two lines under it are what it does.
//  2. ⚠️ **HELP_MAX_CHARS per line.** The strip is 620px wide, the mascot takes 64 of them plus its
//     margins, and a glyph advances CHAR_W. A longer line runs off the right edge, silently.
//  3. ⚠️ **NO APOSTROPHE AND NO SEMICOLON.** `font5x5.h` has neither glyph and draws a BLANK, so
//     "the sample's pitch" comes out as "THE SAMPLE S PITCH". The string is right, the width is
//     right, only the pixels are wrong. Stick to letters, digits, and `: = - ( ) . / , +`.
//  4. Say what the cell DOES, not why it is shaped that way.
//
// Rules 2 and 3 are a `static_assert` over the whole table below (`help_table_ok`), so a line that
// breaks either one fails the BUILD rather than reaching a device. Rules 1 and 4 need a reader.
//
// ⚠️ **A CELL WITH NO ENTRY FALLS BACK TO ITS SCREEN.** `help_topic` returns a SCREEN_* topic for
// anything not written up yet, so a half-finished table still says something useful everywhere — and
// a new screen is never silent by omission.
//
// ⚠️ Nothing here is persisted, so the enum's order is free — but it is APPENDED to anyway, because
// HELP_ENTRIES is indexed by the enum value and an insert would silently re-point every entry below.

#include <cstddef>

#include "ui/app_state.h"
#include "ui/instrument_row_layout.h"
#include "ui/screen.h"

namespace pt::ui {

/**
 * Characters that fit on one line beside the mascot.
 *
 * 620 (strip) − 3 (left margin) − 64 (mascot) − 7 (gutter) = 546px of text, and a glyph advances
 * CHAR_W = 17, so 32 fit with 2px to spare. ⚠️ Derived from the same numbers `modules/help_panel.cpp`
 * lays the panel out with — move the mascot or resize it there and this moves with it.
 */
inline constexpr int HELP_MAX_CHARS = 32;

/** Three lines, top to bottom. An unused line is "" and is simply not drawn. */
struct HelpEntry {
    const char* line1 = "";
    const char* line2 = "";
    const char* line3 = "";
};

/**
 * ⚠️ APPEND ONLY — the value indexes HELP_ENTRIES.
 *
 * SCREEN_* is the fallback for a cell with no entry of its own; everything after them is a cell.
 */
enum class HelpTopic {
    NONE = 0,

    // One per screen — the fallback, and what a screen with no per-cell text yet shows everywhere.
    SCREEN_SONG,
    SCREEN_CHAIN,
    SCREEN_PHRASE,
    SCREEN_INSTRUMENT,
    SCREEN_TABLE,
    SCREEN_PROJECT,
    SCREEN_GROOVE,
    SCREEN_SCALE,
    SCREEN_MODS,
    SCREEN_INST_POOL,
    SCREEN_MIXER,
    SCREEN_EFFECTS,
    SCREEN_FILE_BROWSER,
    SCREEN_SETTINGS,
    SCREEN_SAMPLE_EDITOR,
    SCREEN_MIDI,

    // SONG
    SONG_CELL,

    // CHAIN
    CHAIN_PHRASE,
    CHAIN_TRANSPOSE,

    // PHRASE
    PHRASE_NOTE,
    PHRASE_VOLUME,
    PHRASE_INSTRUMENT,
    PHRASE_FX_TYPE,
    PHRASE_FX_VALUE,

    // TABLE
    TABLE_TRANSPOSE,
    TABLE_VOLUME,
    TABLE_FX_TYPE,
    TABLE_FX_VALUE,

    // INSTRUMENT — on every type
    INST_TYPE,
    INST_SOURCE_LOAD,
    INST_SOURCE_EDIT,
    INST_NAME,
    INST_ROOT,
    INST_DETUNE,
    INST_TIC,
    INST_VOLUME,
    INST_PAN,
    INST_PRESET_SAVE,
    INST_PRESET_LOAD,
    INST_DRIVE,
    INST_FILTER,
    INST_CRUSH,
    INST_FILTER_FREQ,
    INST_DOWNSAMPLE,
    INST_FILTER_RES,
    INST_REVERB_SEND,
    INST_DELAY_SEND,
    INST_EQ,

    // INSTRUMENT — sampler only
    INST_SLICE,
    INST_LOOP_MODE,
    INST_SAMPLE_START,
    INST_LOOP_START,
    INST_SAMPLE_END,
    INST_LOOP_END,
    INST_REVERSE,

    // INSTRUMENT — SoundFont only
    INST_PATCH,

    // INSTRUMENT — external MIDI only
    INST_MIDI_CHANNEL,
    INST_MIDI_BANK,
    INST_MIDI_PROGRAM,
    INST_MIDI_LENGTH,
    INST_MIDI_CC_NUMBER,
    INST_MIDI_CC_VALUE,

    // Appended, never inserted — HELP_ENTRIES is indexed by these values.
    INST_TRANSPOSE,

    COUNT
};

/** Indexed by HelpTopic. ⚠️ One entry per member, in the enum order — `help_table_ok` says so. */
inline constexpr HelpEntry HELP_ENTRIES[] = {
    /* NONE */ {"", "", ""},

    // ── The screens ──────────────────────────────────────────────────────────────────────────────
    /* SCREEN_SONG */
    {"SONG: the arrangement", "Each column is a track. A cell", "holds a chain to play."},
    /* SCREEN_CHAIN */
    {"CHAIN: a run of phrases", "Played top to bottom by the", "song cell that points here."},
    /* SCREEN_PHRASE */
    {"PHRASE: 16 steps of notes", "The smallest pattern. Chains", "string them into a song."},
    /* SCREEN_INSTRUMENT */
    {"INSTRUMENT: one sound", "A sample, a SoundFont, or an", "external MIDI device."},
    /* SCREEN_TABLE */
    {"TABLE: per-tick commands", "Runs under a note while it", "sounds, one row per tick."},
    /* SCREEN_PROJECT */
    {"PROJECT: the whole song", "Name, tempo, saving and", "loading. And the way out."},
    /* SCREEN_GROOVE */
    {"GROOVE: swing and shuffle", "Ticks per step, row by row.", "Assign one with the GRV FX."},
    /* SCREEN_SCALE */
    {"SCALE: the notes allowed", "Notes off the scale are", "pulled onto the nearest one."},
    /* SCREEN_MODS */
    {"MODS: envelopes and LFOs", "Four per instrument. Each one", "moves a chosen parameter."},
    /* SCREEN_INST_POOL */
    {"INST.POOL: all the slots", "Every instrument in one list,", "with volume and sends."},
    /* SCREEN_MIXER */
    {"MIXER: levels and sends", "Eight track faders, a master", "fader, and the master chain."},
    /* SCREEN_EFFECTS */
    {"EFFECTS: the shared units", "One reverb and one delay for", "the whole song."},
    /* SCREEN_FILE_BROWSER */
    {"FILE BROWSER", "A picks. SELECT+A renames,", "SELECT+B deletes."},
    /* SCREEN_SETTINGS */
    {"SETTINGS: how the app acts", "Display, buttons, theme, and", "what happens after a crash."},
    /* SCREEN_SAMPLE_EDITOR */
    {"SAMPLE EDITOR", "Trim, chop and process the", "audio an instrument plays."},
    /* SCREEN_MIDI */
    {"MIDI: ports and sync", "Which device the app talks", "to, going in and going out."},

    // ── SONG ─────────────────────────────────────────────────────────────────────────────────────
    /* SONG_CELL */
    {"SONG CELL: a chain to play", "The column is the track, the", "row is the place in time."},

    // ── CHAIN ────────────────────────────────────────────────────────────────────────────────────
    /* CHAIN_PHRASE */
    {"PH: which phrase plays", "Chain rows run top to bottom.", "A on an empty row makes one."},
    /* CHAIN_TRANSPOSE */
    {"TSP: transpose the phrase", "Shifts every note in it.", "00 leaves the pitch alone."},

    // ── PHRASE ───────────────────────────────────────────────────────────────────────────────────
    /* PHRASE_NOTE */
    {"NOTE: the pitch of a step", "A+LEFT/RIGHT one semitone,", "A+UP/DOWN one octave."},
    /* PHRASE_VOLUME */
    {"VOL: how loud this step is", "00 is silent, FF is full.", "Empty keeps the last volume."},
    /* PHRASE_INSTRUMENT */
    {"INST: which sound to use", "Points at an instrument slot,", "00 to FF."},
    /* PHRASE_FX_TYPE */
    {"FX: a command on this step", "Hold A and press UP to open", "the picker and read them all."},
    /* PHRASE_FX_VALUE */
    {"FX VALUE: what it is set to", "The meaning comes from the FX", "to its left."},

    // ── TABLE ────────────────────────────────────────────────────────────────────────────────────
    /* TABLE_TRANSPOSE */
    {"N: transpose, per tick", "Shifts the pitch of the note", "the table is running under."},
    /* TABLE_VOLUME */
    {"V: volume, per tick", "00 is silent, FF is full.", "Empty leaves the volume be."},
    /* TABLE_FX_TYPE */
    {"FX: a command on this tick", "Hold A and press UP to open", "the picker and read them all."},
    /* TABLE_FX_VALUE */
    {"FX VALUE: what it is set to", "The meaning comes from the FX", "to its left."},

    // ── INSTRUMENT, on every type ────────────────────────────────────────────────────────────────
    /* INST_TYPE */
    {"TYPE: what kind of sound", "SAMPLER plays a file, SF2 a", "SoundFont, EXT a MIDI device."},
    /* INST_SOURCE_LOAD */
    {"LOAD: pick a source file", "Opens the browser for a", "sample or a SoundFont."},
    /* INST_SOURCE_EDIT */
    {"EDIT: open the sample editor", "Trim, chop and process the", "audio in this slot."},
    /* INST_NAME */
    {"NAME: what to call it", "A opens the keyboard. Shown", "here and in the pool."},
    /* INST_ROOT */
    {"ROOT: pitch of the recording", "The note that plays the file", "back at its original speed."},
    /* INST_DETUNE */
    {"DETUNE: fine pitch trim", "80 is centre. Below is flat,", "above is sharp."},
    /* INST_TIC */
    {"TIC: table speed", "Ticks per table row, for", "notes on this instrument."},
    /* INST_VOLUME */
    {"VOL: instrument volume", "00 is silent, FF is full.", "Applies to every note."},
    /* INST_PAN */
    {"PAN: left to right", "00 is hard left, 80 centre,", "FF hard right."},
    /* INST_PRESET_SAVE */
    {"SAVE: store these settings", "Writes a preset file you can", "load into any other slot."},
    /* INST_PRESET_LOAD */
    {"LOAD: recall a preset", "Replaces every setting in", "this slot."},
    /* INST_DRIVE */
    {"DRIVE: overdrive", "Pushes the level into", "distortion. 00 is clean."},
    /* INST_FILTER */
    {"FILTER: which filter", "OFF, LP cuts the top, HP cuts", "the bottom, BP keeps a band."},
    /* INST_CRUSH */
    {"CRUSH: bit depth reduction", "0 is off. Higher throws away", "bits and adds grit."},
    /* INST_FILTER_FREQ */
    {"FREQ: filter cutoff", "Where the filter acts.", "Needs a FILTER type set."},
    /* INST_DOWNSAMPLE */
    {"DWNSMPL: rate reduction", "0 is off. Higher drops the", "rate and dulls the top end."},
    /* INST_FILTER_RES */
    {"RES: filter resonance", "Peaks the sound right at the", "cutoff. Needs a FILTER type."},
    /* INST_REVERB_SEND */
    {"REV: reverb send", "How much of this instrument", "goes to the shared reverb."},
    /* INST_DELAY_SEND */
    {"DEL: delay send", "How much of this instrument", "goes to the shared delay."},
    /* INST_EQ */
    {"EQ: which EQ preset", "A preset slot, 00 to 7F.", "A opens the EQ editor."},

    // ── INSTRUMENT, sampler only ─────────────────────────────────────────────────────────────────
    /* INST_SLICE */
    {"SLICE: chop playback", "OFF, or one slice per note.", "Slices are made in EDIT."},
    /* INST_LOOP_MODE */
    {"LOOP: how the sample repeats", "OFF, FWD loops forward, PNG", "runs it back and forth."},
    /* INST_SAMPLE_START */
    {"START: where playback begins", "00 is the start of the file,", "FF is the end of it."},
    /* INST_LOOP_START */
    {"LOOP ST: where a loop begins", "The point playback jumps back", "to. Needs LOOP switched on."},
    /* INST_SAMPLE_END */
    {"END: where playback stops", "FF is the end of the file.", "Below that it cuts short."},
    /* INST_LOOP_END */
    {"LOOP END: where a loop ends", "The point playback jumps back", "from. Needs LOOP on."},
    /* INST_REVERSE */
    {"REVERSE: play backwards", "ON plays the sample from its", "end to its start."},

    // ── INSTRUMENT, SoundFont only ───────────────────────────────────────────────────────────────
    /* INST_PATCH */
    {"PATCH: which SoundFont voice", "A SoundFont holds many.", "This picks the one to play."},

    // ── INSTRUMENT, external MIDI only ───────────────────────────────────────────────────────────
    /* INST_MIDI_CHANNEL */
    {"CHAN: MIDI channel", "1 to 16. The channel this", "instrument sends on."},
    /* INST_MIDI_BANK */
    {"BANK: MIDI bank select", "Sent just before the program.", "Leave it off if unsure."},
    /* INST_MIDI_PROGRAM */
    {"PROG: MIDI program change", "Picks the patch on the far", "device. 00 to 7F."},
    /* INST_MIDI_LENGTH */
    {"LEN: note length in ticks", "00 holds the note until the", "next one on the track."},
    /* INST_MIDI_CC_NUMBER */
    {"CC: which controller", "The MIDI CC number this slot", "moves. 00 to 7F."},
    /* INST_MIDI_CC_VALUE */
    {"VAL: what to send", "The value for the CC to its", "left. 00 to 7F."},

    // ── Appended after the MIDI block, to match the enum ─────────────────────────────────────────
    /* INST_TRANSPOSE */
    {"TSP: scales and transpose", "OFF pins this instrument to the", "notes as written. ON follows."},
};

// ─── The compile-time check on the table ─────────────────────────────────────────────────────────
//
// ⚠️ Rules 2 and 3 above are silent at runtime — an over-long line simply vanishes off the right edge
// and an apostrophe simply draws as a space. Neither shows up as a crash, a log line or a wrong
// number, and neither is visible unless you happen to open the one screen it is on. So they are
// asserted HERE, where the failure is a compile error naming the file.

namespace detail {

/** Drawable by `font5x5.h` in a help line. ⚠️ `'` and `;` are deliberately absent — they draw blank. */
constexpr bool help_char_ok(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' ||
           c == ':' || c == '=' || c == '-' || c == '(' || c == ')' || c == '.' || c == '/' ||
           c == ',' || c == '+';
}

constexpr bool help_line_ok(const char* s) {
    int n = 0;
    for (; s[n] != '\0'; ++n) {
        if (!help_char_ok(s[n])) return false;
    }
    return n <= HELP_MAX_CHARS;
}

constexpr bool help_table_ok() {
    for (const HelpEntry& e : HELP_ENTRIES) {
        if (!help_line_ok(e.line1) || !help_line_ok(e.line2) || !help_line_ok(e.line3)) return false;
    }
    return true;
}

}  // namespace detail

static_assert(sizeof(HELP_ENTRIES) / sizeof(HELP_ENTRIES[0]) ==
                  static_cast<size_t>(HelpTopic::COUNT),
              "HELP_ENTRIES has one entry per HelpTopic, in the enum order");
static_assert(detail::help_table_ok(),
              "a help line is over HELP_MAX_CHARS, or holds a character font5x5 draws blank "
              "(an apostrophe or a semicolon)");

/** The three lines for `topic`. Out of range gives the empty entry rather than reading past the end. */
inline const HelpEntry& help_entry(HelpTopic topic) {
    const size_t i = static_cast<size_t>(topic);
    if (i >= static_cast<size_t>(HelpTopic::COUNT)) return HELP_ENTRIES[0];
    return HELP_ENTRIES[i];
}

// ─── The lookup ──────────────────────────────────────────────────────────────────────────────────

/** The screen-level fallback — what a cell with nothing written for it shows. */
inline HelpTopic help_screen_topic(ScreenType screen) {
    switch (screen) {
        case ScreenType::SONG:          return HelpTopic::SCREEN_SONG;
        case ScreenType::CHAIN:         return HelpTopic::SCREEN_CHAIN;
        case ScreenType::PHRASE:        return HelpTopic::SCREEN_PHRASE;
        case ScreenType::INSTRUMENT:    return HelpTopic::SCREEN_INSTRUMENT;
        case ScreenType::TABLE:         return HelpTopic::SCREEN_TABLE;
        case ScreenType::PROJECT:       return HelpTopic::SCREEN_PROJECT;
        case ScreenType::GROOVE:        return HelpTopic::SCREEN_GROOVE;
        case ScreenType::SCALE:         return HelpTopic::SCREEN_SCALE;
        case ScreenType::MODS:          return HelpTopic::SCREEN_MODS;
        case ScreenType::INST_POOL:     return HelpTopic::SCREEN_INST_POOL;
        case ScreenType::MIXER:         return HelpTopic::SCREEN_MIXER;
        case ScreenType::EFFECTS:       return HelpTopic::SCREEN_EFFECTS;
        case ScreenType::FILE_BROWSER:  return HelpTopic::SCREEN_FILE_BROWSER;
        case ScreenType::SETTINGS:      return HelpTopic::SCREEN_SETTINGS;
        case ScreenType::SAMPLE_EDITOR: return HelpTopic::SCREEN_SAMPLE_EDITOR;
        case ScreenType::MIDI:          return HelpTopic::SCREEN_MIDI;
    }
    return HelpTopic::NONE;
}

namespace detail {

/** PHRASE columns 1..9. Column 0 is the step number and `cursor_left_limit` never lets the cursor on it. */
inline HelpTopic phrase_cell_topic(int column) {
    switch (column) {
        case 1:  return HelpTopic::PHRASE_NOTE;
        case 2:  return HelpTopic::PHRASE_VOLUME;
        case 3:  return HelpTopic::PHRASE_INSTRUMENT;
        // The three FX slots are the same two cells three times over — a type and its value.
        case 4: case 6: case 8: return HelpTopic::PHRASE_FX_TYPE;
        case 5: case 7: case 9: return HelpTopic::PHRASE_FX_VALUE;
        default: return HelpTopic::NONE;
    }
}

/** TABLE columns 1..8 — one fewer than PHRASE, because a table row has no instrument cell. */
inline HelpTopic table_cell_topic(int column) {
    switch (column) {
        case 1:  return HelpTopic::TABLE_TRANSPOSE;
        case 2:  return HelpTopic::TABLE_VOLUME;
        case 3: case 5: case 7: return HelpTopic::TABLE_FX_TYPE;
        case 4: case 6: case 8: return HelpTopic::TABLE_FX_VALUE;
        default: return HelpTopic::NONE;
    }
}

/**
 * INSTRUMENT, external MIDI — the shortest of the three layouts.
 *
 * Rows 0/1/5 (TYPE, NAME, the preset buttons) are shared with the other two types and are answered by
 * the caller before this is reached; 4 and 6 are spacers.
 */
inline HelpTopic instrument_external_topic(int row, int column) {
    switch (row) {
        case 2: return column == 1 ? HelpTopic::INST_MIDI_CHANNEL : HelpTopic::INST_MIDI_BANK;
        case 3: return column == 1 ? HelpTopic::INST_MIDI_PROGRAM : HelpTopic::INST_MIDI_LENGTH;
        case 7: return column == 1 ? HelpTopic::INST_VOLUME : HelpTopic::INST_PAN;
        case 8: return column == 1 ? HelpTopic::INST_TRANSPOSE : HelpTopic::INST_TIC;
        default: break;
    }
    // The four CC rows are one pair repeated, exactly as `cursor_context` reads them.
    const int cc = row - INSTRUMENT_EXTERNAL_CC_ROW;
    if (cc >= 0 && cc < 4)
        return column == 1 ? HelpTopic::INST_MIDI_CC_NUMBER : HelpTopic::INST_MIDI_CC_VALUE;
    return HelpTopic::NONE;
}

/**
 * INSTRUMENT, sampler and SoundFont.
 *
 * ⚠️ `off` is the SAME one-row shift `cursor_context` applies, and for the same reason: the
 * SoundFont layout has a PATCH row at 6 that the sampler does not, so every row below it sits one
 * lower. Written the same way here so the two cannot disagree about which row DRIVE is on.
 */
inline HelpTopic instrument_sample_topic(bool sf, int row, int column) {
    const int off = sf ? 1 : 0;

    if (row == 2) {
        if (column == 1) return HelpTopic::INST_ROOT;
        if (column == 3) return HelpTopic::INST_DETUNE;
        if (column == 5) return HelpTopic::INST_TIC;
        return HelpTopic::NONE;
    }
    if (row == 3) {  // VOL + TSP + PAN, the same three on both types
        if (column == 1) return HelpTopic::INST_VOLUME;
        if (column == 3) return HelpTopic::INST_TRANSPOSE;
        if (column == 5) return HelpTopic::INST_PAN;
        return HelpTopic::NONE;
    }
    if (sf && row == 6) return column == 1 ? HelpTopic::INST_PATCH : HelpTopic::NONE;

    if (row == 7 + off) return column == 1 ? HelpTopic::INST_DRIVE : HelpTopic::INST_FILTER;
    if (row == 8 + off) return column == 1 ? HelpTopic::INST_CRUSH : HelpTopic::INST_FILTER_FREQ;
    if (row == 9 + off) return column == 1 ? HelpTopic::INST_DOWNSAMPLE : HelpTopic::INST_FILTER_RES;

    if (sf) {
        // The SoundFont tail is one parameter per row, so any column but the label is the value.
        if (row == 12) return HelpTopic::INST_REVERB_SEND;
        if (row == 13) return HelpTopic::INST_DELAY_SEND;
        if (row == 14) return HelpTopic::INST_EQ;
        return HelpTopic::NONE;
    }

    switch (row) {
        case 11: return column == 1 ? HelpTopic::INST_REVERB_SEND : HelpTopic::INST_DELAY_SEND;
        case 12: return column == 1 ? HelpTopic::INST_EQ : HelpTopic::INST_SLICE;
        case 13: return column == 1 ? HelpTopic::INST_LOOP_MODE : HelpTopic::INST_SAMPLE_START;
        case 14: return column == 1 ? HelpTopic::INST_LOOP_START : HelpTopic::INST_SAMPLE_END;
        case 15: return column == 1 ? HelpTopic::INST_LOOP_END : HelpTopic::INST_REVERSE;
        default: return HelpTopic::NONE;
    }
}

/** INSTRUMENT, all three types. Rows 0, 1 and 5 are shared; the tail is per type. */
inline HelpTopic instrument_topic(songcore::InstrumentType type, int row, int column) {
    if (row == 0) {
        // TYPE, then the two buttons beside it. On a SoundFont there is no EDIT (no single waveform
        // to edit) and on EXTERNAL neither button is drawn, so those columns are never reached.
        if (column == 1) return HelpTopic::INST_TYPE;
        if (column == 2) return HelpTopic::INST_SOURCE_LOAD;
        if (column == 3) return HelpTopic::INST_SOURCE_EDIT;
        return HelpTopic::NONE;
    }
    if (row == 1) return HelpTopic::INST_NAME;
    if (row == 5) {
        // The .pti preset buttons, on every layout: the cursor snaps to column 2 on entry.
        if (column == 2) return HelpTopic::INST_PRESET_SAVE;
        if (column == 3) return HelpTopic::INST_PRESET_LOAD;
        return HelpTopic::NONE;
    }

    switch (type) {
        case songcore::InstrumentType::EXTERNAL:  return instrument_external_topic(row, column);
        case songcore::InstrumentType::SOUNDFONT: return instrument_sample_topic(true, row, column);
        default:                                  return instrument_sample_topic(false, row, column);
    }
}

}  // namespace detail

/**
 * What the cursor is standing on, right now.
 *
 * ⚠️ **Never NONE.** A cell with no entry of its own falls back to its screen, so the panel always
 * has something to draw and a screen is never blank merely because its cells are unwritten.
 *
 * ⚠️ The per-screen arms read the SAME cursor fields the module's `cursor_context()` does — the
 * grid screens share `cursorRow`/`cursorColumn`, TABLE and INSTRUMENT carry their own. A screen with
 * no arm here falls through to its screen topic, which is what makes an unfinished table harmless.
 */
inline HelpTopic help_topic(const AppState& s) {
    HelpTopic cell = HelpTopic::NONE;

    switch (s.currentScreen) {
        case ScreenType::SONG:
            // Column 0 is the row-number gutter and the cursor never lands on it, so every reachable
            // column here is a track cell.
            cell = HelpTopic::SONG_CELL;
            break;
        case ScreenType::CHAIN:
            cell = (s.cursorColumn == 1) ? HelpTopic::CHAIN_PHRASE : HelpTopic::CHAIN_TRANSPOSE;
            break;
        case ScreenType::PHRASE:
            cell = detail::phrase_cell_topic(s.cursorColumn);
            break;
        case ScreenType::TABLE:
            cell = detail::table_cell_topic(s.tableCursorColumn);
            break;
        case ScreenType::INSTRUMENT:
            // ⚠️ Guarded, unlike the modules: `ptshot` and the tools build an AppState with no
            // project at all, and help is asked for on every frame it is up.
            if (s.project != nullptr) {
                const songcore::Instrument& ins =
                    s.project->instruments[static_cast<size_t>(s.currentInstrument)];
                cell = detail::instrument_topic(ins.instrumentType, s.instrumentCursorRow,
                                                s.instrumentCursorColumn);
            }
            break;
        default:
            break;
    }

    return (cell == HelpTopic::NONE) ? help_screen_topic(s.currentScreen) : cell;
}

}  // namespace pt::ui
