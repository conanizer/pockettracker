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
//     right, only the pixels are wrong. Stick to letters, digits, and `: = - ( ) . / , +` — plus
//     the four arrows ← ↑ → ↓, the one thing above ASCII the font maps, each costing ONE column.
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
#include "ui/modules/effects_editor.h"
#include "ui/modules/modulation.h"
#include "ui/modules/sample_editor.h"
#include "ui/modules/scale_editor.h"
#include "ui/screen.h"
#include "ui/settings_row_layout.h"

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

    // PROJECT
    PROJECT_TEMPO,
    PROJECT_TRANSPOSE,
    PROJECT_NAME,
    PROJECT_SAVE,
    PROJECT_LOAD,
    PROJECT_NEW,
    PROJECT_EXPORT_MIX,
    PROJECT_EXPORT_STEMS,
    PROJECT_COMPACT_SEQ,
    PROJECT_COMPACT_INST,
    PROJECT_SYSTEM,
    PROJECT_MIDI,
    PROJECT_EXIT,

    // GROOVE — one editable column, so one entry
    GROOVE_TIC,

    // SCALE
    SCALE_NAME,
    SCALE_SAVE,
    SCALE_LOAD,
    SCALE_KEY,
    SCALE_DEGREE,

    // MODS — the row's meaning follows the slot's TYPE, so these are named after the parameter
    MOD_TYPE,
    MOD_DEST,
    MOD_AMOUNT,
    MOD_ATTACK,
    MOD_HOLD,
    MOD_DECAY,
    MOD_SUSTAIN,
    MOD_RELEASE,
    MOD_OSC,
    MOD_TRIG,
    MOD_FREQ,

    // INST.POOL — its four value columns ARE the instrument's own, so they reuse the INST_* entries
    POOL_SLOT,

    // MIXER
    MIXER_TRACK_VOL,
    MIXER_MASTER_VOL,
    MIXER_REVERB_RETURN,
    MIXER_DELAY_RETURN,
    MIXER_MASTER_EQ,
    MIXER_MASTER_FX,
    MIXER_LIMITER,

    // EFFECTS
    FX_MASTER_TYPE,
    FX_REVERB_SIZE,
    FX_REVERB_DAMP,
    FX_REVERB_EQ,
    FX_DELAY_TIME,
    FX_DELAY_FEEDBACK,
    FX_DELAY_TO_REVERB,
    FX_DELAY_EQ,

    // SETTINGS
    SET_LAYOUT,
    SET_SKIN,
    SET_SCALING,
    SET_OVERLAY,
    SET_OVERLAY_STRENGTH,
    SET_BTN_SOUND,
    SET_BTN_SOUND_VOL,
    SET_BTN_VIBRO,
    SET_BTN_VIBRO_POW,
    SET_ABXY,
    SET_KB_INSERT,
    SET_CURSOR,
    SET_NAV,
    SET_FOLDER,
    SET_NOTE_PREVIEW,
    SET_VISUALIZER,
    SET_THEME,
    SET_TEMPLATE_SAVE,
    SET_TEMPLATE_CLEAR,
    SET_RESUME,
    SET_TRACE,
    SET_ENGINE,

    // MIDI
    MIDI_OUTPUT,
    MIDI_INPUT,
    MIDI_OFFSET,
    MIDI_SYNC,
    MIDI_PROG_CHG,
    MIDI_IN_CHANNEL,
    MIDI_PANIC,
    MIDI_TEST,

    // ── The two IN-PLACE OVERLAYS ────────────────────────────────────────────────────────────────
    // Neither is a `ScreenType`: they stand in the editor's place and leave `currentScreen` alone,
    // which is why their screen-level topics sit here rather than beside the sixteen above.
    SCREEN_EQ,
    EQ_TYPE,
    EQ_FREQ,
    EQ_GAIN,
    EQ_Q,

    SCREEN_THEME,
    THEME_NAME,
    THEME_SAVE,
    THEME_LOAD,
    // ⚠️ THE TWENTY-TWO COLOUR ROWS ARE IN `theme_color_rows()` ORDER (ui/theme.h) and are looked up
    // BY POSITION — see THEME_COLOR_TOPICS below. Append here and there together, or a new colour row
    // falls back to the screen text, which is the harmless direction and the only one reachable.
    THEME_BACKGROUND,
    THEME_ROW_4TH,
    THEME_ROW_CURSOR,
    THEME_ROW_SELECT,
    THEME_TXT_TITLE,
    THEME_TXT_PARAM,
    THEME_TXT_VALUE,
    THEME_TXT_CURSOR,
    THEME_TXT_EMPTY,
    THEME_TXT_SELECT,
    THEME_TXT_PLAY,
    THEME_VIZ_BG,
    THEME_VIZ_LINE,
    THEME_VIZ_WAVE,
    THEME_MTR_BG,
    THEME_MTR_LOW,
    THEME_MTR_MID,
    THEME_MTR_HIGH,
    THEME_EQ_BG,
    THEME_EQ_FILL,
    THEME_EQ_BORDER,
    THEME_EQ_TXT,

    // ── The SAMPLE EDITOR ────────────────────────────────────────────────────────────────────────
    // The one full-screen module that shows help: its waveform panel is the same 620 wide as the
    // strip, and no cell lives there (help_panel.h). Its rows are sparse — 1, 2, 8, 10, 11, 13, 14,
    // 16, 18, 19 — and the twelve OP BUTTONS get an entry each because a six-letter verb on a button
    // is exactly the label that explains nothing.
    SE_ZOOM,
    SE_SOURCE,
    SE_RATE,
    SE_PITCH,
    SE_DURATION,
    SE_SNAP,
    SE_SEL_START,
    SE_SEL_END,
    SE_SLICE_METHOD,
    SE_SLICE_SENS,
    SE_SLICE_BY,
    SE_SLICE_INDEX,
    SE_SLICE_POS,
    SE_OP_CROP,
    SE_OP_COPY,
    SE_OP_CUT,
    SE_OP_DUPL,
    SE_OP_PASTE,
    SE_OP_DEL,
    SE_OP_NORM,
    SE_OP_FADE_IN,
    SE_OP_FADE_OUT,
    SE_OP_SILENCE,
    SE_OP_REVERSE,
    SE_OP_UNDO,
    SE_FX_TYPE,
    SE_FX_VALUE,
    SE_FX_APPLY,
    SE_NAME,
    SE_LOAD,
    SE_SAVE,
    SE_OVERWRITE,
    SE_CHOP,

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
    {"NOTE: the pitch of a step", "A+←/→ steps a semitone,", "A+↑/↓ a whole octave."},
    /* PHRASE_VOLUME */
    {"VOL: how loud this step is", "00 is silent, FF is full.", "Empty keeps the last volume."},
    /* PHRASE_INSTRUMENT */
    {"INST: which sound to use", "Points at an instrument slot,", "00 to FF."},
    /* PHRASE_FX_TYPE */
    {"FX: a command on this step", "A+↑/↓ opens the picker and", "shows what each one does."},
    /* PHRASE_FX_VALUE */
    {"FX VALUE: what it is set to", "The meaning comes from the FX", "to its left."},

    // ── TABLE ────────────────────────────────────────────────────────────────────────────────────
    /* TABLE_TRANSPOSE */
    {"N: transpose, per tick", "Shifts the pitch of the note", "the table is running under."},
    /* TABLE_VOLUME */
    {"V: volume, per tick", "00 is silent, FF is full.", "Empty leaves the volume be."},
    /* TABLE_FX_TYPE */
    {"FX: a command on this tick", "A+↑/↓ opens the picker and", "shows what each one does."},
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

    // ── PROJECT ──────────────────────────────────────────────────────────────────────────────────
    /* PROJECT_TEMPO */
    {"TEMPO: beats per minute", "20 to 999. A+↑/↓ steps by", "ten at a time."},
    /* PROJECT_TRANSPOSE */
    {"TSP: transpose the whole song", "Shifts every note. 00 leaves", "the pitch alone."},
    /* PROJECT_NAME */
    {"NAME: what the song is called", "A opens the keyboard. Each", "letter is its own cell."},
    /* PROJECT_SAVE */
    {"SAVE: write the song to disk", "Under the name on the NAME", "row above."},
    /* PROJECT_LOAD */
    {"LOAD: open another song", "Opens the file browser to", "pick one."},
    /* PROJECT_NEW */
    {"NEW: start an empty song", "Asks first. Starts from your", "template if you saved one."},
    /* PROJECT_EXPORT_MIX */
    {"MIX: render the song to WAV", "One file, with every track", "playing together."},
    /* PROJECT_EXPORT_STEMS */
    {"STEMS: render each track", "One WAV per track, so they", "can be mixed elsewhere."},
    /* PROJECT_COMPACT_SEQ */
    {"SEQ: clear unused patterns", "Empties every chain and", "phrase the song never plays."},
    /* PROJECT_COMPACT_INST */
    {"INST: clear unused sounds", "Empties instrument slots no", "phrase plays, and frees RAM."},
    /* PROJECT_SYSTEM */
    {"SETTINGS: how the app acts", "A opens the settings screen,", "B comes back here."},
    /* PROJECT_MIDI */
    {"MIDI: ports and sync", "A opens the MIDI screen,", "B comes back here."},
    /* PROJECT_EXIT */
    {"EXIT: leave PocketTracker", "Asks first. Save the song", "before you go."},

    // ── GROOVE ───────────────────────────────────────────────────────────────────────────────────
    /* GROOVE_TIC */
    {"TIC: how long a step lasts", "In ticks. A on -- adds a", "step, A+B takes it away."},

    // ── SCALE ────────────────────────────────────────────────────────────────────────────────────
    /* SCALE_NAME */
    {"NAME: the shape of the scale", "A+D-PAD walks the 38 built-in", "shapes."},
    /* SCALE_SAVE */
    {"SAVE: store this scale", "Writes a file you can load", "into any other project."},
    /* SCALE_LOAD */
    {"LOAD: recall a scale", "Replaces the twelve rows", "below it."},
    /* SCALE_KEY */
    {"KEY: the root of the song", "Every row below is named", "from here. All 16 share it."},
    /* SCALE_DEGREE */
    {"EN: is this note allowed", "ON keeps it. A note that is", "off is pulled to the nearest."},

    // ── MODS ─────────────────────────────────────────────────────────────────────────────────────
    /* MOD_TYPE */
    {"TYPE: the shape it moves in", "AHD and ADSR are envelopes,", "LFO repeats. NONE is off."},
    /* MOD_DEST */
    {"DEST: what it moves", "The parameter this slot", "changes while a note plays."},
    /* MOD_AMOUNT */
    {"AMT: how far it moves", "00 does nothing, FF is the", "full swing."},
    /* MOD_ATTACK */
    {"ATK: time up to the peak", "00 is instant. Higher fades", "in more slowly."},
    /* MOD_HOLD */
    {"HOLD: time spent at the peak", "Before the decay starts.", ""},
    /* MOD_DECAY */
    {"DEC: time to fall away", "How long the drop after the", "peak takes."},
    /* MOD_SUSTAIN */
    {"SUS: the level it settles at", "Held for as long as the note", "is on."},
    /* MOD_RELEASE */
    {"REL: time to fall at the end", "Starts when the note stops.", ""},
    /* MOD_OSC */
    {"OSC: the LFO shape", "Triangle, sine, ramps and", "squares, plus two random."},
    /* MOD_TRIG */
    {"TRIG: how the LFO starts", "RETG restarts per note, ONCE", "runs once, HOLD freezes it."},
    /* MOD_FREQ */
    {"FREQ: how fast the LFO runs", "Higher is faster.", ""},

    // ── INST.POOL ────────────────────────────────────────────────────────────────────────────────
    /* POOL_SLOT */
    {"SLOT: pick an instrument", "A on an empty slot loads a", "file. A+B clears the slot."},

    // ── MIXER ────────────────────────────────────────────────────────────────────────────────────
    /* MIXER_TRACK_VOL */
    {"TRACK VOLUME: one fader", "00 is silent, FF is full.", "One column per track."},
    /* MIXER_MASTER_VOL */
    {"MIX: the master volume", "Everything passes through it", "on the way out."},
    /* MIXER_REVERB_RETURN */
    {"REV: the reverb return", "How loud the shared reverb", "comes back into the mix."},
    /* MIXER_DELAY_RETURN */
    {"DEL: the delay return", "How loud the shared delay", "comes back into the mix."},
    /* MIXER_MASTER_EQ */
    {"EQ: the master EQ preset", "A slot, 00 to 7F, or --.", "A opens the EQ editor."},
    /* MIXER_MASTER_FX */
    {"OTT/DUST: master bus depth", "How hard it works. EFFECTS", "picks which of the two."},
    /* MIXER_LIMITER */
    {"LIM: limiter pre-gain", "Pushes the mix harder into", "the limiter. 00 is off."},

    // ── EFFECTS ──────────────────────────────────────────────────────────────────────────────────
    /* FX_MASTER_TYPE */
    {"MASTER: the bus effect", "OTT is a 3-band squeeze,", "DUST is a lo-fi chain."},
    /* FX_REVERB_SIZE */
    {"SIZE: how big the room is", "Higher makes the tail last", "longer."},
    /* FX_REVERB_DAMP */
    {"DAMP: how dark the tail is", "Higher takes more top end", "out of the reverb."},
    /* FX_REVERB_EQ */
    {"EQ: EQ on the reverb input", "A slot, or -- for none.", "A opens the EQ editor."},
    /* FX_DELAY_TIME */
    {"TIME: the gap between echoes", "B switches between a free", "value and note divisions."},
    /* FX_DELAY_FEEDBACK */
    {"FDBK: how many echoes", "Higher repeats for longer.", "Very high never stops."},
    /* FX_DELAY_TO_REVERB */
    {"REV: delay into the reverb", "Sends the echoes through the", "reverb as well."},
    /* FX_DELAY_EQ */
    {"EQ: EQ on the delay input", "A slot, or -- for none.", "A opens the EQ editor."},

    // ── SETTINGS ─────────────────────────────────────────────────────────────────────────────────
    /* SET_LAYOUT */
    {"LAYOUT: how the app fits", "Fullscreen, landscape, or", "portrait with buttons."},
    /* SET_SKIN */
    {"SKIN: the button artwork", "Which set of on-screen", "buttons the portrait uses."},
    /* SET_SCALING */
    {"SCALING: how pixels are drawn", "INT keeps them sharp,", "BILINEAR smooths them."},
    /* SET_OVERLAY */
    {"OVERLAY: a picture on top", "Laid over the screen for a", "scanline or LCD look."},
    /* SET_OVERLAY_STRENGTH */
    {"STR: how strong it is", "00 is invisible, FF is the", "full picture."},
    /* SET_BTN_SOUND */
    {"BTN SOUND: a click per press", "For the on-screen buttons.", ""},
    /* SET_BTN_SOUND_VOL */
    {"VOL: how loud the click is", "00 is silent, FF is full.", ""},
    /* SET_BTN_VIBRO */
    {"BTN VIBRO: a buzz per press", "For the on-screen buttons.", ""},
    /* SET_BTN_VIBRO_POW */
    {"POW: how hard it buzzes", "LO is a tick, HI is a click.", "There is nothing in between."},
    /* SET_ABXY */
    {"ABXY: where A and B are", "Match it to the labels", "printed on your own pad."},
    /* SET_KB_INSERT */
    {"KB INSERT: where a letter goes", "BEFORE the keyboard cursor,", "or AFTER it."},
    /* SET_CURSOR */
    {"CURSOR: coming back to a screen", "REMEMBER keeps where you were,", "REFRESH goes to the top."},
    /* SET_NAV */
    {"NAV: what B+arrows walk", "SONG steps through the", "arrangement, POOL by number."},
    /* SET_FOLDER */
    {"FOLDER: where a load opens", "REMEMBER returns to the last", "folder used, REFRESH resets."},
    /* SET_NOTE_PREVIEW */
    {"NOTE PREV: hear what you type", "Plays the note as you enter", "it in a phrase."},
    /* SET_VISUALIZER */
    {"VISUALIZER: the top strip", "A scope, a meter per track,", "or a spectrum."},
    /* SET_THEME */
    {"THEME: the colours", "A opens the theme editor,", "where every colour is a row."},
    /* SET_TEMPLATE_SAVE */
    {"SAVE: this song as the start", "Every NEW project begins", "from it."},
    /* SET_TEMPLATE_CLEAR */
    {"CLEAR: forget the template", "NEW goes back to an empty", "song."},
    /* SET_RESUME */
    {"RESUME: after a crash", "ASK offers the recovered", "work, AUTO just opens it."},
    /* SET_TRACE */
    {"TRACE: write a debug log", "Records what the sequencer", "does. Off unless asked for."},
    /* SET_ENGINE */
    {"ENG: which sequencer runs", "A developer switch. Leave it", "where it is."},

    // ── MIDI ─────────────────────────────────────────────────────────────────────────────────────
    /* MIDI_OUTPUT */
    {"OUTPUT: the cable out", "The device notes are sent", "to. OFF sends nothing."},
    /* MIDI_INPUT */
    {"INPUT: the cable in", "The device you play from.", "OFF listens to nothing."},
    /* MIDI_OFFSET */
    {"OFFSET: nudge the timing", "Minus sends earlier, plus", "later. In milliseconds."},
    /* MIDI_SYNC */
    {"SYNC: send a clock out", "24 pulses a beat, plus start", "and stop."},
    /* MIDI_PROG_CHG */
    {"PROG CHG: send patch changes", "Sends BANK and PROG from the", "instrument before a note."},
    /* MIDI_IN_CHANNEL */
    {"IN CH: what a track listens to", "One channel per track.", "-- ignores the input."},
    /* MIDI_PANIC */
    {"PANIC: silence everything", "A sends all notes off on", "every channel."},
    /* MIDI_TEST */
    {"TEST: prove the cable works", "A sends one C-4 on channel", "1 and says what happened."},

    // ── The EQ editor ────────────────────────────────────────────────────────────────────────────
    /* SCREEN_EQ */
    {"EQ: three bands of tone", "The yellow curve is what the", "three add up to."},
    /* EQ_TYPE */
    {"TYPE: what this band does", "Shelves lift or drop one end,", "BELL a spot, cuts remove it."},
    /* EQ_FREQ */
    {"FREQ: where the band sits", "The frequency it works on,", "20 Hz up to 20 kHz."},
    /* EQ_GAIN */
    {"GAIN: how much to lift or cut", "Centre is flat. Up to 12 dB", "each way."},
    /* EQ_Q */
    {"Q: how wide the band is", "Low is broad and gentle,", "high is narrow and sharp."},

    // ── The theme editor ─────────────────────────────────────────────────────────────────────────
    /* SCREEN_THEME */
    {"THEME: every colour, by row", "A+←/→ nudges a channel,", "A+↑/↓ moves it by 16."},
    /* THEME_NAME */
    {"THEME: the built-in palettes", "A+D-PAD walks them and", "replaces every colour below."},
    /* THEME_SAVE */
    {"SAVE: store this palette", "Writes a theme file you can", "load again or share."},
    /* THEME_LOAD */
    {"LOAD: recall a palette", "Replaces every colour row", "below."},
    /* THEME_BACKGROUND */
    {"BACKGROUND: behind it all", "The ground every screen is", "drawn on."},
    /* THEME_ROW_4TH */
    {"ROW 4TH: every fourth row", "The faint stripe that counts", "a grid off in fours."},
    /* THEME_ROW_CURSOR */
    {"ROW CURSOR: behind the cell", "The block under the cell you", "are standing on."},
    /* THEME_ROW_SELECT */
    {"ROW SELECT: behind a block", "The fill under a selection", "you have marked out."},
    /* THEME_TXT_TITLE */
    {"TXT TITLE: the headings", "Screen names, and the border", "around a pop-up box."},
    /* THEME_TXT_PARAM */
    {"TXT PARAM: the labels", "The name beside a value, and", "the column headers."},
    /* THEME_TXT_VALUE */
    {"TXT VALUE: the numbers", "Every value you can type or", "dial."},
    /* THEME_TXT_CURSOR */
    {"TXT CURSOR: ink on the cell", "The text inside the block you", "are standing on."},
    /* THEME_TXT_EMPTY */
    {"TXT EMPTY: the blanks", "The -- and --- a cell shows", "when nothing is set."},
    /* THEME_TXT_SELECT */
    {"TXT SELECT: ink in a block", "The text inside a selection.", ""},
    /* THEME_TXT_PLAY */
    {"TXT PLAY: the playhead", "The arrow marking where each", "track is playing."},
    /* THEME_VIZ_BG */
    {"VIZ BG: behind the top strip", "And the ground this help", "panel is drawn on."},
    /* THEME_VIZ_LINE */
    {"VIZ LINE: the centre line", "The rule across the middle", "of the scope."},
    /* THEME_VIZ_WAVE */
    {"VIZ WAVE: the waveform", "The scope trace, and the ink", "of this help panel."},
    /* THEME_MTR_BG */
    {"MTR BG: behind the meters", "And the fill of every pop-up", "box in the app."},
    /* THEME_MTR_LOW */
    {"MTR LOW: a quiet meter", "The bottom of a level bar on", "the mixer."},
    /* THEME_MTR_MID */
    {"MTR MID: a loud meter", "The middle of a level bar on", "the mixer."},
    /* THEME_MTR_HIGH */
    {"MTR HIGH: a meter near clip", "The top of a level bar on", "the mixer."},
    /* THEME_EQ_BG */
    {"EQ BG: behind the EQ curve", "The ground of the EQ editor", "panel."},
    /* THEME_EQ_FILL */
    {"EQ FILL: under the spectrum", "The block below the live", "spectrum in the EQ editor."},
    /* THEME_EQ_BORDER */
    {"EQ BORDER: the spectrum line", "The outline drawn on top of", "that spectrum."},
    /* THEME_EQ_TXT */
    {"EQ TXT: the EQ scale marks", "The frequency labels across", "the EQ panel."},

    // ── The sample editor ────────────────────────────────────────────────────────────────────────
    /* SE_ZOOM */
    {"ZOOM: how close the view is", "R+↑ zooms in, R+↓ out,", "from any cell on this screen."},
    /* SE_SOURCE */
    {"SOURCE: which side to edit", "LEFT, RIGHT or MONO. Only", "STEREO keeps both sides."},
    /* SE_RATE */
    {"RATE: how much detail to keep", "NORM and LOFI throw some", "away for good. HIGH keeps it."},
    /* SE_PITCH */
    {"PITCH: move the whole sample", "In semitones. It happens when", "you save, not before."},
    /* SE_DURATION */
    {"DURATION: the length to fit", "What SYNC on the EFFECT row", "stretches the sample to."},
    /* SE_SNAP */
    {"SNAP: stops edits clicking", "Puts a dragged edge where", "the wave crosses the middle."},
    /* SE_SEL_START */
    {"START: where editing begins", "A+←/→ drags it a little,", "A+↑/↓ a lot."},
    /* SE_SEL_END */
    {"END: where editing stops", "A+←/→ drags it a little,", "A+↑/↓ a lot."},
    /* SE_SLICE_METHOD */
    {"SLICE: how to cut it up", "TRANSIENT finds the hits,", "DIVIDE cuts equal parts."},
    /* SE_SLICE_SENS */
    {"SENS: how many hits to find", "Higher finds more of them,", "quiet ones included."},
    /* SE_SLICE_BY */
    {"BY: how many equal parts", "The sample is cut into this", "many, end to end."},
    /* SE_SLICE_INDEX */
    {"SLICE: which cut you are on", "A+D-PAD walks them. Under", "MANUAL, A cuts at the playhead."},
    /* SE_SLICE_POS */
    {"START: where this cut sits", "A+D-PAD drags it. A+B puts it", "back, or removes one you made."},
    /* SE_OP_CROP */
    {"CROP: keep only the selection", "Everything outside it is", "thrown away."},
    /* SE_OP_COPY */
    {"COPY: take the selection", "Puts it on the clipboard and", "changes nothing."},
    /* SE_OP_CUT */
    {"CUT: copy it, then remove it", "The sample gets shorter by", "the length you took."},
    /* SE_OP_DUPL */
    {"DUPL: repeat the selection", "Adds another copy of it on", "the end of the sample."},
    /* SE_OP_PASTE */
    {"PASTE: drop the clipboard in", "Inserts it at the start of", "the selection."},
    /* SE_OP_DEL */
    {"DEL: remove the selection", "The sample gets shorter by", "the length you cut."},
    /* SE_OP_NORM */
    {"NORM: as loud as it can go", "Lifts the selection until", "its loudest peak is full."},
    /* SE_OP_FADE_IN */
    {"FADE+: fade the selection in", "It rises from silence to", "full over its own length."},
    /* SE_OP_FADE_OUT */
    {"FADE-: fade the selection out", "It falls from full to", "silence over its length."},
    /* SE_OP_SILENCE */
    {"SLNC: empty the selection", "Wipes what is there and", "keeps the length."},
    /* SE_OP_REVERSE */
    {"REV: play it backwards", "Turns the selection around,", "end to start."},
    /* SE_OP_UNDO */
    {"UNDO: take back the last edit", "One step only, and it does", "not mean back to the file."},
    /* SE_FX_TYPE */
    {"EFFECT: bake one in for good", "OTT, DUST, DRIVE, EQ or the", "SYNC fit. APPLY does it."},
    /* SE_FX_VALUE */
    {"VALUE: what the effect uses", "An amount, or an EQ slot, or", "which way SYNC fits it."},
    /* SE_FX_APPLY */
    {"APPLY: do it, for good", "Bakes the effect into the", "audio. SYNC fits the length."},
    /* SE_NAME */
    {"NAME: what to call it", "A opens the keyboard. SAVE", "uses this for the file name."},
    /* SE_LOAD */
    {"LOAD: open another sample", "Into this same slot. Opens", "the file browser."},
    /* SE_SAVE */
    {"SAVE: write a new file", "Into the samples folder. It", "never replaces an old one."},
    /* SE_OVERWRITE */
    {"OVERWRITE: replace the file", "Writes back over the one", "this sample came from."},
    /* SE_CHOP */
    {"CHOP: every slice as a file", "Writes them into a Chops", "folder named after this one."},
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

/**
 * One of the four arrows, at `s`? They are the only non-ASCII text a help line may hold: `font5x5.h`
 * maps U+2190..U+2193 and nothing else above 127, and every other code point draws BLANK.
 *
 * ⚠️ Read one byte at a time so a truncated sequence at the end of the string stops at the
 * terminator rather than reading past it.
 */
constexpr bool help_arrow_at(const char* s) {
    if (static_cast<unsigned char>(s[0]) != 0xE2) return false;
    if (static_cast<unsigned char>(s[1]) != 0x86) return false;
    const auto tail = static_cast<unsigned char>(s[2]);
    return tail >= 0x90 && tail <= 0x93;
}

// ⚠️ The budget is CODE POINTS, not bytes: `Canvas::draw_text` advances one column per code point,
// so a three-byte arrow costs ONE of the HELP_MAX_CHARS.
constexpr bool help_line_ok(const char* s) {
    int n = 0;
    for (int i = 0; s[i] != '\0'; ++n) {
        if (help_arrow_at(s + i)) { i += 3; continue; }
        if (!help_char_ok(s[i])) return false;
        ++i;
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
              "(an apostrophe, a semicolon, or a code point that is not one of the four arrows)");

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

/**
 * PROJECT — two values, a name, and then rows that are BUTTONS.
 *
 * ⚠️ The button rows are read by COLUMN, and the column numbers are the DRAW order, not the reading
 * order: on the PROJECT row column 1 is SAVE and column 2 is LOAD, which is the order the row itself
 * lists them in (`project_editor.cpp`).
 */
inline HelpTopic project_cell_topic(int row, int column) {
    if (row < 0 || row >= PROJECT_ROW_COUNT) return HelpTopic::NONE;
    switch (static_cast<ProjectRow>(row)) {
        case ProjectRow::TEMPO:     return HelpTopic::PROJECT_TEMPO;
        case ProjectRow::TRANSPOSE: return HelpTopic::PROJECT_TRANSPOSE;
        // Every character of the name is its own cursor column, and they all say the same thing.
        case ProjectRow::NAME:      return HelpTopic::PROJECT_NAME;
        case ProjectRow::PROJECT:
            return column == 1   ? HelpTopic::PROJECT_SAVE
                   : column == 2 ? HelpTopic::PROJECT_LOAD
                                 : HelpTopic::PROJECT_NEW;
        case ProjectRow::EXPORT:
            return column == 1 ? HelpTopic::PROJECT_EXPORT_MIX : HelpTopic::PROJECT_EXPORT_STEMS;
        case ProjectRow::COMPACT:
            return column == 1 ? HelpTopic::PROJECT_COMPACT_SEQ : HelpTopic::PROJECT_COMPACT_INST;
        case ProjectRow::SYSTEM:    return HelpTopic::PROJECT_SYSTEM;
        case ProjectRow::MIDI:      return HelpTopic::PROJECT_MIDI;
        case ProjectRow::EXIT:      return HelpTopic::PROJECT_EXIT;
    }
    return HelpTopic::NONE;
}

/** SCALE — the NAME row is the only one with more than one cell; the twelve below it are degrees. */
inline HelpTopic scale_cell_topic(int row, int column) {
    if (row == SCALE_NAME_ROW) {
        if (column == SCALE_NAME_COL_SAVE) return HelpTopic::SCALE_SAVE;
        if (column == SCALE_NAME_COL_LOAD) return HelpTopic::SCALE_LOAD;
        return HelpTopic::SCALE_NAME;
    }
    if (row == SCALE_KEY_ROW) return HelpTopic::SCALE_KEY;
    return (scale_row_degree(row) >= 0) ? HelpTopic::SCALE_DEGREE : HelpTopic::NONE;
}

/**
 * MODS — the row's MEANING follows the slot's type, so this asks the same three questions the module
 * asks (`modulation.cpp`): LFO first, then the AHD-shaped pair, then ADSR.
 *
 * ⚠️ Row 4 is HOLD on an AHD, DEC on an ADSR and TRIG on an LFO. There is no "the row 4 parameter",
 * which is why this cannot be a plain table the way PHRASE's columns are.
 */
inline HelpTopic mod_cell_topic(songcore::ModType type, int row) {
    const bool lfo = (type == songcore::ModType::LFO);
    switch (row) {
        case 0: return HelpTopic::MOD_TYPE;
        case 1: return HelpTopic::MOD_DEST;
        case 2: return HelpTopic::MOD_AMOUNT;
        case 3: return lfo ? HelpTopic::MOD_OSC : HelpTopic::MOD_ATTACK;
        case 4:
            if (lfo) return HelpTopic::MOD_TRIG;
            return is_ahd_shaped(type) ? HelpTopic::MOD_HOLD : HelpTopic::MOD_DECAY;
        case 5:
            if (lfo) return HelpTopic::MOD_FREQ;
            return is_ahd_shaped(type) ? HelpTopic::MOD_DECAY : HelpTopic::MOD_SUSTAIN;
        case 6: return HelpTopic::MOD_RELEASE;
        default: return HelpTopic::NONE;
    }
}

/**
 * INST.POOL — its four value columns ARE the instrument's own fields, so they take the INSTRUMENT
 * screen's entries rather than a second set that could drift from them. Only the name column, whose
 * A and A+B belong to the pool alone, has text of its own.
 */
inline HelpTopic pool_cell_topic(int column) {
    switch (column) {
        case 1:  return HelpTopic::INST_VOLUME;
        case 2:  return HelpTopic::INST_REVERB_SEND;
        case 3:  return HelpTopic::INST_DELAY_SEND;
        case 4:  return HelpTopic::INST_EQ;
        default: return HelpTopic::POOL_SLOT;
    }
}

/** MIXER — not a grid: rows 2 and 3 exist only on the master strip (column 8). See `mixer.h`. */
inline HelpTopic mixer_cell_topic(int master_row, int column) {
    if (master_row == 0)
        return (column < 8) ? HelpTopic::MIXER_TRACK_VOL : HelpTopic::MIXER_MASTER_VOL;
    if (master_row == 1) {
        if (column == 0) return HelpTopic::MIXER_REVERB_RETURN;
        if (column == 1) return HelpTopic::MIXER_DELAY_RETURN;
        if (column == 8) return HelpTopic::MIXER_MASTER_EQ;
        return HelpTopic::NONE;
    }
    if (column != 8) return HelpTopic::NONE;
    if (master_row == 2) return HelpTopic::MIXER_MASTER_FX;
    if (master_row == 3) return HelpTopic::MIXER_LIMITER;
    return HelpTopic::NONE;
}

/** EFFECTS — eight editable rows, named by the module so the two cannot disagree about which is which. */
inline HelpTopic effects_cell_topic(int row) {
    switch (row) {
        case EffectModule::ROW_MASTER_TYPE: return HelpTopic::FX_MASTER_TYPE;
        case EffectModule::ROW_REV_SIZE:    return HelpTopic::FX_REVERB_SIZE;
        case EffectModule::ROW_REV_DAMP:    return HelpTopic::FX_REVERB_DAMP;
        case EffectModule::ROW_REV_EQ:      return HelpTopic::FX_REVERB_EQ;
        case EffectModule::ROW_DLY_TIME:    return HelpTopic::FX_DELAY_TIME;
        case EffectModule::ROW_DLY_FDBK:    return HelpTopic::FX_DELAY_FEEDBACK;
        case EffectModule::ROW_DLY_REV:     return HelpTopic::FX_DELAY_TO_REVERB;
        case EffectModule::ROW_DLY_EQ:      return HelpTopic::FX_DELAY_EQ;
        default:                            return HelpTopic::NONE;
    }
}

/**
 * SETTINGS — column 2 is the row's SECOND cell where it has one (the skin, STR, VOL, POW, ENG), and
 * on TEMPLATE it is the second BUTTON. Column 0 is the label and is unreachable, as on PROJECT.
 */
inline HelpTopic settings_cell_topic(int row, int column) {
    if (row < 0 || row >= SETTINGS_ROW_COUNT) return HelpTopic::NONE;
    const bool second = (column == 2);
    switch (static_cast<SettingsRow>(row)) {
        case SettingsRow::LAYOUT:     return second ? HelpTopic::SET_SKIN : HelpTopic::SET_LAYOUT;
        case SettingsRow::SCALING:    return HelpTopic::SET_SCALING;
        case SettingsRow::OVERLAY:
            return second ? HelpTopic::SET_OVERLAY_STRENGTH : HelpTopic::SET_OVERLAY;
        case SettingsRow::BTN_SOUND:
            return second ? HelpTopic::SET_BTN_SOUND_VOL : HelpTopic::SET_BTN_SOUND;
        case SettingsRow::BTN_VIBRO:
            return second ? HelpTopic::SET_BTN_VIBRO_POW : HelpTopic::SET_BTN_VIBRO;
        case SettingsRow::ABXY:       return HelpTopic::SET_ABXY;
        case SettingsRow::KB_INSERT:  return HelpTopic::SET_KB_INSERT;
        case SettingsRow::CURSOR:     return HelpTopic::SET_CURSOR;
        case SettingsRow::NAV:        return HelpTopic::SET_NAV;
        case SettingsRow::FOLDER:     return HelpTopic::SET_FOLDER;
        case SettingsRow::NOTE_PREV:  return HelpTopic::SET_NOTE_PREVIEW;
        case SettingsRow::VISUALIZER: return HelpTopic::SET_VISUALIZER;
        case SettingsRow::THEME:      return HelpTopic::SET_THEME;
        case SettingsRow::TEMPLATE:
            return second ? HelpTopic::SET_TEMPLATE_CLEAR : HelpTopic::SET_TEMPLATE_SAVE;
        case SettingsRow::RESUME:     return HelpTopic::SET_RESUME;
        case SettingsRow::TRACE:      return second ? HelpTopic::SET_ENGINE : HelpTopic::SET_TRACE;
    }
    return HelpTopic::NONE;
}

/** MIDI — one topic per row. IN CH is eight cells that all mean the same thing, one per track. */
inline HelpTopic midi_cell_topic(int row) {
    if (row < 0 || row >= MIDI_ROW_COUNT) return HelpTopic::NONE;
    switch (static_cast<MidiRow>(row)) {
        case MidiRow::OUTPUT:   return HelpTopic::MIDI_OUTPUT;
        case MidiRow::INPUT:    return HelpTopic::MIDI_INPUT;
        case MidiRow::OFFSET:   return HelpTopic::MIDI_OFFSET;
        case MidiRow::SYNC:     return HelpTopic::MIDI_SYNC;
        case MidiRow::PROG_CHG: return HelpTopic::MIDI_PROG_CHG;
        case MidiRow::IN_MAP:   return HelpTopic::MIDI_IN_CHANNEL;
        case MidiRow::PANIC:    return HelpTopic::MIDI_PANIC;
        case MidiRow::TEST:     return HelpTopic::MIDI_TEST;
    }
    return HelpTopic::NONE;
}

/** The EQ editor's cursor is one int over a 3×4 grid: band = row / 4, parameter = row % 4. */
inline HelpTopic eq_cell_topic(int cursor_row) {
    if (cursor_row < 0) return HelpTopic::NONE;
    switch (cursor_row % 4) {
        case 0:  return HelpTopic::EQ_TYPE;
        case 1:  return HelpTopic::EQ_FREQ;
        case 2:  return HelpTopic::EQ_GAIN;
        default: return HelpTopic::EQ_Q;
    }
}

/**
 * The theme editor's colour rows, IN `theme_color_rows()` ORDER (ui/theme.h) — its row N is this
 * array's entry N.
 *
 * ⚠️ A colour row added there and not here falls off the end and shows the screen text instead. That
 * is the same fallback every unwritten cell in this file gets, and it is the only direction the
 * mismatch can go: the lookup bounds itself on THIS array, so it can never read past either list.
 */
inline constexpr HelpTopic THEME_COLOR_TOPICS[] = {
    HelpTopic::THEME_BACKGROUND, HelpTopic::THEME_ROW_4TH,     HelpTopic::THEME_ROW_CURSOR,
    HelpTopic::THEME_ROW_SELECT, HelpTopic::THEME_TXT_TITLE,   HelpTopic::THEME_TXT_PARAM,
    HelpTopic::THEME_TXT_VALUE,  HelpTopic::THEME_TXT_CURSOR,  HelpTopic::THEME_TXT_EMPTY,
    HelpTopic::THEME_TXT_SELECT, HelpTopic::THEME_TXT_PLAY,    HelpTopic::THEME_VIZ_BG,
    HelpTopic::THEME_VIZ_LINE,   HelpTopic::THEME_VIZ_WAVE,    HelpTopic::THEME_MTR_BG,
    HelpTopic::THEME_MTR_LOW,    HelpTopic::THEME_MTR_MID,     HelpTopic::THEME_MTR_HIGH,
    HelpTopic::THEME_EQ_BG,      HelpTopic::THEME_EQ_FILL,     HelpTopic::THEME_EQ_BORDER,
    HelpTopic::THEME_EQ_TXT,
};

/**
 * The theme editor. Row 0 is the palette row — its three cells are the name, SAVE and LOAD — and
 * every row below it is one colour, whose three channels all say the same thing.
 */
inline HelpTopic theme_cell_topic(int row, int channel) {
    if (row == 0) {
        if (channel == 1) return HelpTopic::THEME_SAVE;
        if (channel == 2) return HelpTopic::THEME_LOAD;
        return HelpTopic::THEME_NAME;
    }
    const int index = row - 1;
    const int count = static_cast<int>(sizeof(THEME_COLOR_TOPICS) / sizeof(THEME_COLOR_TOPICS[0]));
    return (index >= 0 && index < count) ? THEME_COLOR_TOPICS[index] : HelpTopic::NONE;
}


/**
 * The sample editor. Its rows are SPARSE — 1, 2, 8, 10, 11, 13, 14, 16, 18, 19 — and two of the cells
 * change meaning under a mode, so both modes are asked for here rather than guessed:
 *
 *  · row 10 column 1 is SENS under TRANSIENT and BY under DIVIDE, and does not exist under the other
 *    two methods (`slice_has_parameter`);
 *  · row 19 column 3 is CHOP, which only exists when there are slices to chop.
 *
 * ⚠️ The two OP ROWS are indexed straight off the column, so the order here IS `ops_row1()` and
 * `ops_row2()` in sample_editor.cpp. Reorder a button there and the wrong text comes up under it —
 * which is why they are named in the same left-to-right order and nowhere else.
 */
inline HelpTopic sample_editor_cell_topic(int row, int column, int slice_method) {
    static constexpr HelpTopic OPS_1[] = {HelpTopic::SE_OP_CROP,  HelpTopic::SE_OP_COPY,
                                          HelpTopic::SE_OP_CUT,   HelpTopic::SE_OP_DUPL,
                                          HelpTopic::SE_OP_PASTE, HelpTopic::SE_OP_DEL};
    static constexpr HelpTopic OPS_2[] = {HelpTopic::SE_OP_NORM,    HelpTopic::SE_OP_FADE_IN,
                                          HelpTopic::SE_OP_FADE_OUT, HelpTopic::SE_OP_SILENCE,
                                          HelpTopic::SE_OP_REVERSE,  HelpTopic::SE_OP_UNDO};
    const bool op_col = (column >= 0 && column < 6);

    switch (row) {
        case 1:
            if (column == 0) return HelpTopic::SE_ZOOM;
            if (column == 1) return HelpTopic::SE_SOURCE;
            return (column == 2) ? HelpTopic::SE_RATE : HelpTopic::NONE;
        case 2:
            if (column == 0) return HelpTopic::SE_PITCH;
            if (column == 1) return HelpTopic::SE_DURATION;
            return (column == 2) ? HelpTopic::SE_SNAP : HelpTopic::NONE;

        // Rows 3..8 are all the SELECTION: the cursor only ever rests on 8, but the D-pad drags an
        // edge from any of them, and column is which edge.
        case 3: case 4: case 5: case 6: case 7: case 8:
            return (column == 1) ? HelpTopic::SE_SEL_END : HelpTopic::SE_SEL_START;

        case 10:
            if (column == 0) return HelpTopic::SE_SLICE_METHOD;
            if (column != 1) return HelpTopic::NONE;
            if (slice_method == SampleEditorModule::SLICE_TRANSIENT) return HelpTopic::SE_SLICE_SENS;
            if (slice_method == SampleEditorModule::SLICE_DIVIDE)    return HelpTopic::SE_SLICE_BY;
            return HelpTopic::NONE;
        case 11:
            return (column == 1) ? HelpTopic::SE_SLICE_POS : HelpTopic::SE_SLICE_INDEX;

        case 13: return op_col ? OPS_1[column] : HelpTopic::NONE;
        case 14: return op_col ? OPS_2[column] : HelpTopic::NONE;

        case 16:
            if (column == 0) return HelpTopic::SE_FX_TYPE;
            if (column == 1) return HelpTopic::SE_FX_VALUE;
            return (column == 2) ? HelpTopic::SE_FX_APPLY : HelpTopic::NONE;

        case 18: return HelpTopic::SE_NAME;
        case 19:
            if (column == 0) return HelpTopic::SE_LOAD;
            if (column == 1) return HelpTopic::SE_SAVE;
            if (column == 2) return HelpTopic::SE_OVERWRITE;
            return (column == 3) ? HelpTopic::SE_CHOP : HelpTopic::NONE;

        default: return HelpTopic::NONE;   // the title bar and the four spacer rows
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
    // ⚠️ THE TWO IN-PLACE OVERLAYS ARE ASKED FIRST, and they have to be: neither changes
    // `currentScreen`, so the screen underneath is still the answer to every question about where the
    // cursor is — and it is not on the canvas. Asking it would explain a cell nobody can see.
    // ⚠️ The EQ editor outranks the theme editor because it is the one that can be up over the SAMPLE
    // EDITOR (which is where it brings the strip back at all); the two can never be open together.
    if (s.eq.isOpen) {
        const HelpTopic eq = detail::eq_cell_topic(s.eq.cursorRow);
        return (eq == HelpTopic::NONE) ? HelpTopic::SCREEN_EQ : eq;
    }
    if (s.themeEditor.isOpen) {
        const HelpTopic th =
            detail::theme_cell_topic(s.themeEditor.cursorRow, s.themeEditor.cursorChannel);
        return (th == HelpTopic::NONE) ? HelpTopic::SCREEN_THEME : th;
    }

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
        case ScreenType::PROJECT:
            cell = detail::project_cell_topic(s.projectCursorRow, s.projectCursorColumn);
            break;
        case ScreenType::GROOVE:
            // One editable column, and the cursor is never anywhere else: the screen is 16 TIC cells.
            cell = HelpTopic::GROOVE_TIC;
            break;
        case ScreenType::SCALE:
            cell = detail::scale_cell_topic(s.scaleCursorRow, s.scaleCursorColumn);
            break;
        case ScreenType::MODS:
            // Guarded like INSTRUMENT above, and for the same reason: the tools build an AppState
            // with no project at all.
            if (s.project != nullptr) {
                const songcore::Instrument& ins =
                    s.project->instruments[static_cast<size_t>(s.currentInstrument)];
                // The cursor is (pair, side, row) here — the slot is which HALF of which pair.
                const size_t slot = static_cast<size_t>(s.modCursorPair * 2 + s.modCursorSide);
                if (slot < ins.modSlots.size())
                    cell = detail::mod_cell_topic(ins.modSlots[slot].type, s.modCursorRow);
            }
            break;
        case ScreenType::INST_POOL:
            cell = detail::pool_cell_topic(s.poolCursorColumn);
            break;
        case ScreenType::MIXER:
            cell = detail::mixer_cell_topic(s.mixerMasterRow, s.mixerCursorColumn);
            break;
        case ScreenType::EFFECTS:
            cell = detail::effects_cell_topic(s.effectsCursorRow);
            break;
        case ScreenType::SETTINGS:
            cell = detail::settings_cell_topic(s.settingsCursorRow, s.settingsCursorColumn);
            break;
        case ScreenType::SAMPLE_EDITOR:
            cell = detail::sample_editor_cell_topic(s.sampleEditor.cursorRow,
                                                    s.sampleEditor.cursorCol,
                                                    s.sampleEditor.sliceMethod);
            break;
        case ScreenType::MIDI:
            cell = detail::midi_cell_topic(s.midiCursorRow);
            break;
        default:
            break;
    }

    return (cell == HelpTopic::NONE) ? help_screen_topic(s.currentScreen) : cell;
}

}  // namespace pt::ui
