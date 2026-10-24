#pragma once

// Help on SELECT: a topic for every cell, its text, and the cursor → topic lookup. No drawing.
//
// The text's rules:
//  1. The first line names the thing — "VOL: how loud this step is"; the rest say what it does.
//  2. At most HELP_MAX_CHARS per line.
//  3. No apostrophe or semicolon (`font5x5.h` draws them blank). Letters, digits, `: = - ( ) . / , +`
//     and the four arrows ← ↑ → ↓.
//  4. Say what the cell does, not why.
//  5. Whole sentences in everyday words: "how loud the reverb is in the mix", not "the return".
// 2 and 3 are a `static_assert` over the table; 1, 4 and 5 need a reader.
// A cell with no entry falls back to its SCREEN_* topic. ⚠️ Append to the enum: it indexes HELP_ENTRIES.

#include <cstddef>

#include "ui/app_state.h"
#include "ui/instrument_row_layout.h"
#include "ui/mixer_cell_layout.h"
#include "ui/modules/effects_editor.h"
#include "ui/modules/groove_editor.h"
#include "ui/modules/midi_map_editor.h"
#include "ui/modules/modulation.h"
#include "ui/modules/sample_editor.h"
#include "ui/modules/scale_editor.h"
#include "ui/screen.h"
#include "ui/settings_row_layout.h"

namespace pt::ui {

// Characters beside the mascot: (620 − 3 − 64 − 7) px at CHAR_W = 17. ⚠️ `modules/help_panel.cpp`
// lays out with the same numbers.
inline constexpr int HELP_MAX_CHARS = 32;

// Title characters beside the large mascot in the full overlay (pinned in `modules/help_overlay.h`).
inline constexpr int HELP_TITLE_MAX_CHARS = 26;

inline constexpr int HELP_BODY_LINES = 8;   // the full overlay's room for body…
inline constexpr int HELP_KEY_LINES = 9;    // …and for controls, one gesture per line

// Three summary lines, then an optional body and controls. The compact panel shows the summary; the
// full overlay the title, the body (or `line2`/`line3` without one), then the controls.
// The body replaces `line2`/`line3` there, so a gesture is not shown twice. A null line is a blank row.
struct HelpEntry {
    const char* line1 = "";
    const char* line2 = "";
    const char* line3 = "";
    const char* body[HELP_BODY_LINES] = {};
    const char* keys[HELP_KEY_LINES]  = {};
};

// ⚠️ Append only — the value indexes HELP_ENTRIES.
enum class HelpTopic {
    NONE = 0,

    // One per screen — the fallback for a cell with no entry of its own.
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
    SCREEN_MIDI_MAP,

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

    // INST.POOL — its value columns reuse the INST_* entries
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

    // MIDI
    MIDI_OUTPUT,
    MIDI_INPUT,
    MIDI_OFFSET,
    MIDI_SYNC,
    MIDI_PROG_CHG,
    MIDI_KEYS,
    MIDI_PANIC,
    MIDI_TEST,
    MIDI_CTL_CH,
    MIDI_MAPPING,

    // MIDI MAPPING — one topic per column of a mapping row, plus the row that adds one
    MAP_CC,
    MAP_MIN,
    MAP_MAX,
    MAP_DEST,
    MAP_SCOPE,
    MAP_ADD,

    // ── The two in-place overlays (not a `ScreenType`, so their screen topics sit here) ──────────
    SCREEN_EQ,
    EQ_TYPE,
    EQ_FREQ,
    EQ_GAIN,
    EQ_Q,

    SCREEN_THEME,
    THEME_NAME,
    THEME_SAVE,
    THEME_LOAD,
    THEME_SCHEME,
    THEME_ROLL,
    // ⚠️ The colour rows are in `theme_color_rows()` order and looked up by position
    // (THEME_COLOR_TOPICS). Append here and there together.
    THEME_BACKGROUND,
    THEME_ROW_4TH,
    THEME_ROW_CURSOR,
    THEME_ROW_SELECT,
    THEME_TXT_TITLE,
    THEME_TXT_PARAM,
    THEME_TXT_VALUE,
    THEME_TXT_EMPTY,
    THEME_TXT_PLAY,
    THEME_VIZ_BG,
    THEME_VIZ_LINE,
    THEME_VIZ_WAVE,
    THEME_MTR_BG,
    THEME_MTR_LOW,
    THEME_MTR_MID,
    THEME_MTR_HIGH,
    THEME_EQ_FILL,
    THEME_EQ_BORDER,
    THEME_EQ_TXT,
    THEME_EQ_BG,

    // ── The sample editor — the one full-screen module with help (its waveform panel is the strip) ──
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

    // ── Appended, because the value indexes HELP_ENTRIES ──────────────────────────────────────────
    SET_METRONOME,
    SET_METRONOME_VOL,
    PROJECT_TAP,
    FX_DELAY_TYPE,
    FX_DELAY_TONE,
    FX_DELAY_WOBBLE,
    FX_DELAY_PONG,
    FX_REVERB_TYPE,
    FX_REVERB_PRE,
    FX_REVERB_WIDE,
    FX_REVERB_MOD,
    FX_REVERB_DECAY,
    SE_BIT,
    FX_REVERB_ALGO,
    SET_HELP,
    GROOVE_NAME,
    GROOVE_SAVE,
    GROOVE_LOAD,
    GROOVE_QNT,
    GROOVE_SWG,

    MIDI_VELOCITY,
    SET_AUDIO_OUT,
    GROOVE_PPQ,
    FX_REVERB_SAVE,
    FX_REVERB_LOAD,
    FX_DELAY_SAVE,
    FX_DELAY_LOAD,
    SE_STRETCH_BPM,
    SE_STRETCH_PITCH,
    SE_STRETCH_GRAIN,
    SE_STRETCH_PREVIEW,
    SE_STRETCH_APPLY,
    INST_MIDI_BEND,
    MIDI_SYNC_IN,

    COUNT
};

// Indexed by HelpTopic: one entry per member, in the enum order (asserted below).
inline constexpr HelpEntry HELP_ENTRIES[] = {
    /* NONE */ {"", "", ""},

    // ── The screens ──────────────────────────────────────────────────────────────────────────────
    /* SCREEN_SONG */
    {"SONG: the arrangement", "Each column is a track, and", "each cell is a chain it plays.",
     {"Eight tracks side by side. Each",
      "track plays the chains in its",
      "column from top to bottom. An",
      "empty cell ends a group of",
      "chains: the track jumps back to",
      "the first one and plays the",
      "group again."},
     {"START plays from the cursor row",
      "R+→ opens the chain you are on",
      "B+↑/↓ jumps 16 rows",
      "B+←/→ switches SONG and LIVE",
      "R+B mutes, R+A solos a track",
      "L+R brings every track back",
      "L+B marks cells, B copies them",
      "L+A pastes"}},
    /* SCREEN_CHAIN */
    {"CHAIN: a list of phrases", "Up to 16 phrases, played from", "top to bottom.",
     {"A chain plays up to 16 phrases,",
      "one after another, from top to",
      "bottom. TSP plays a phrase",
      "higher or lower without making a",
      "copy of it."},
     {"START plays this chain",
      "R+START plays the whole song",
      "R+→ opens the phrase you are on",
      "R+← goes back to the song",
      "B+D-PAD goes to another chain",
      "L+B marks cells, B copies them",
      "L+A pastes"}},
    /* SCREEN_PHRASE */
    {"PHRASE: 16 steps of notes", "The smallest pattern. A chain", "plays phrases one after another.",
     {"Each step can hold a note, its",
      "volume and instrument, and three",
      "FX commands."},
     {"START plays this phrase",
      "R+START plays the whole song",
      "R+→ opens the instrument",
      "R+← goes back to the chain",
      "B+D-PAD goes to another phrase",
      "L+B marks cells, B copies them",
      "L+A pastes",
      "L+B+A copies it to a new phrase",
      "L+D-PAD fills the rows below"}},
    /* SCREEN_INSTRUMENT */
    {"INSTRUMENT: one sound", "A sample, a SoundFont, or an", "external MIDI device.",
     {"One of the 128 instruments of",
      "the song. It plays a sample or a",
      "SoundFont and shapes the sound:",
      "pitch, volume, filter, drive,",
      "and how much goes to the reverb",
      "and delay."},
     {"START plays the phrase",
      "R+START plays the whole song",
      "A+START plays the instrument",
      "B+←/→ goes to another instrument",
      "B+↑/↓ jumps 16 instruments",
      "R+↑ opens its MODS",
      "R+← goes back to the phrase"}},
    /* SCREEN_TABLE */
    {"TABLE: a pattern inside a note", "Plays along with each note of", "its instrument, row by row.",
     {"A short sequence that plays",
      "along with every note of its",
      "instrument, one row at a time.",
      "Each row can change the pitch",
      "and volume and run commands.",
      "Instrument 05 uses table 05",
      "unless a TBL command says",
      "otherwise."},
     {"START plays the phrase",
      "R+START plays the whole song",
      "A+START plays the table",
      "B+←/→ goes to another table",
      "B+↑/↓ jumps 16 tables",
      "L+B marks cells, B copies them",
      "L+A pastes"}},
    /* SCREEN_PROJECT */
    {"PROJECT: the whole song", "Name, tempo, save and load,", "export, and leaving the app.",
     {"The name, tempo and transpose of",
      "the song, plus saving, loading,",
      "exporting and leaving the app."},
     {"A presses the button you are on",
      "START plays the song from 00",
      "R+↓ goes back where you were"}},
    /* SCREEN_GROOVE */
    {"GROOVE: swing and shuffle", "Sets how long each step lasts,", "in ticks. GRV picks a groove.",
     {"A list of up to 16 step lengths,",
      "in ticks. Each phrase step takes",
      "the next one, and the list",
      "starts again at the end. 0C is",
      "an even step. The GRV command",
      "picks a groove for a track."},
     {"A on -- adds a step of 0C",
      "A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B ends the list on this row",
      "B+←/→ goes to another groove",
      "START plays the phrase",
      "R+↓ goes back to the phrase"}},
    /* SCREEN_SCALE */
    {"SCALE: the notes allowed", "A note outside the scale moves", "to the nearest one inside.",
     {"The song has 16 scales. A track",
      "that uses this one plays only",
      "the notes switched on here. Any",
      "other note moves to the nearest",
      "one that is on. The SCA and SCG",
      "commands choose a scale for a",
      "track."},
     {"A+D-PAD changes the cell",
      "A on SAVE or LOAD presses it",
      "B+←/→ goes to another scale",
      "START plays the phrase",
      "R+↓ goes to GROOVE"}},
    /* SCREEN_MODS */
    {"MODS: envelopes and LFOs", "Four per instrument. Each one", "moves a setting as a note plays.",
     {"This instrument has four mods,",
      "shown two at a time. Each one",
      "moves one setting of the sound",
      "while a note plays - or moves",
      "another mod."},
     {"↑/↓ moves through rows and pairs",
      "←/→ switches between the two",
      "A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B resets the value",
      "B+←/→ goes to another instrument",
      "B+↑/↓ jumps 16 instruments",
      "START plays the phrase",
      "A+START plays the instrument",
      "R+↓ goes back to the instrument"}},
    /* SCREEN_INST_POOL */
    {"INST.POOL: all instruments", "Every instrument in one list,", "with volume, effects and EQ.",
     {"All 128 instruments in one list,",
      "with the volume, REV, DEL and EQ",
      "of each. Use it to balance them",
      "without opening each one."},
     {"↑/↓ picks an instrument",
      "B+↑/↓ jumps 16 instruments",
      "A on an empty name loads a file",
      "A+B on a name empties it",
      "START plays the phrase",
      "A+START plays the instrument",
      "R+→ opens it on INSTRUMENT",
      "R+← goes to the phrase"}},
    /* SCREEN_MIXER */
    {"MIXER: how loud everything is", "Track volumes, effect levels", "and the master output.",
     {"A volume for each of the eight",
      "tracks, how loud the reverb and",
      "delay are, and the master",
      "output: its volume, EQ, the OTT",
      "or DUST amount and the limiter."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B resets the value",
      "R+B mutes, R+A solos a fader",
      "L+R brings everything back",
      "START plays the song from 00",
      "R+↓ goes to EFFECTS"}},
    /* SCREEN_EFFECTS */
    {"EFFECTS: reverb and delay", "One reverb and one delay that", "every instrument can use.",
     {"The one reverb and one delay",
      "that all instruments share, and",
      "the effect on the whole mix: OTT",
      "or DUST."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B resets the value",
      "A on an EQ row opens the editor",
      "A on SAVE or LOAD: a preset file",
      "A on TIME switches note sync",
      "START plays the song from 00",
      "R+↑ goes to the MIXER"}},
    /* SCREEN_FILE_BROWSER */
    {"FILE BROWSER", "A picks. SELECT+A renames,", "SELECT+B deletes.",
     {"Here you pick the file to load -",
      "a sample, SoundFont, preset,",
      "song or theme. It shows only",
      "files of that kind. You can also",
      "copy, move, rename and delete",
      "files here."},
     {"↑/↓ moves, ←/→ jumps a page",
      "A opens a folder or picks a file",
      "START plays the file you are on",
      "R+← goes up, R+↑/↓ sorts",
      "L+B marks files, B copies them",
      "L+A cuts them, or pastes here",
      "SELECT+A renames what you are on",
      "SELECT+B deletes it or all marks",
      "SELECT+R makes a new folder"}},
    /* SCREEN_SETTINGS */
    {"SETTINGS: how the app works", "Display, buttons, theme, and", "what happens after a crash.",
     {"These belong to the app, not to",
      "a song, so they are the same for",
      "every song."},
     {"A+D-PAD changes a value",
      "A presses THEME or TEMPLATE",
      "START plays the song from 00",
      "B goes back"}},
    /* SCREEN_SAMPLE_EDITOR */
    {"SAMPLE EDITOR", "Trim, slice and change the", "sample an instrument plays.",
     {"Edit the sample of this",
      "instrument: trim it, cut it into",
      "slices, fit it to the tempo or",
      "add effects to it. The file on",
      "disk does not change until SAVE",
      "or OVERWRITE."},
     {"START plays the selection",
      "START again stops it",
      "R+↑/↓ zooms in and out",
      "A presses a button",
      "B leaves, and asks if unsaved"}},
    /* SCREEN_MIDI */
    {"MIDI: devices and clock", "The MIDI devices the app sends", "to and listens to.",
     {"Choose the MIDI devices the app",
      "sends to and listens to, the",
      "clock it sends and follows, the",
      "timing, and how a MIDI keyboard",
      "plays."},
     {"A+D-PAD changes a value",
      "A on PANIC or TEST sends it",
      "START plays the song from 00",
      "B goes back"}},
    /* SCREEN_MIDI_MAP */
    {"MAPPING: knobs to controls", "A knob on your controller", "moves a value in the song.",
     {"Each line is one knob: the CC",
      "number it sends, the current",
      "value, the range it moves",
      "across, and the control it",
      "moves. The knob must send on the",
      "channel set on the CTL CH row.",
      "Holding R on a value and turning",
      "a knob also adds a line."},
     {"A+D-PAD changes a value",
      "A on the last line adds one",
      "A+B removes a line",
      "A+B on MIN or MAX resets it",
      "START plays the song from 00",
      "B goes back"}},

    // ── SONG ─────────────────────────────────────────────────────────────────────────────────────
    /* SONG_CELL */
    {"SONG CELL: a chain to play", "The column is the track, the", "row is when it plays.",
     {"The track plays this chain at",
      "this point in the song. An empty",
      "cell ends the group of chains",
      "above it: the track jumps back",
      "to the first one and plays the",
      "group again."},
     {"A on -- puts the last chain used",
      "Tap A twice for a new chain",
      "A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B clears the cell",
      "R+→ opens this chain",
      "L+B+A clones chain and phrases",
      "START plays from this row",
      "R+B mutes, R+A solos the track"}},

    // ── CHAIN ────────────────────────────────────────────────────────────────────────────────────
    /* CHAIN_PHRASE */
    {"PH: which phrase plays", "The rows play top to bottom.", "Tap A twice for a new phrase.",
     {"The rows of a chain play one",
      "after another, top to bottom."},
     {"A on -- puts the last phrase",
      "Tap A twice for a new phrase",
      "A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B clears the row",
      "R+→ opens this phrase",
      "L+B+A copies it to a new phrase",
      "START plays this chain"}},
    /* CHAIN_TRANSPOSE */
    {"TSP: transpose the phrase", "Moves every note up or down.", "00 leaves the pitch alone.",
     {"Counted in semitones. The phrase",
      "itself is not changed. 00 is no",
      "change, 0C is one octave up, F4",
      "is one octave down. A row with",
      "no phrase has no TSP."},
     {"A+←/→ changes by a semitone",
      "A+↑/↓ changes by an octave",
      "A+B sets it back to 00"}},

    // ── PHRASE ───────────────────────────────────────────────────────────────────────────────────
    /* PHRASE_NOTE */
    {"NOTE: the pitch of a step", "A+←/→ changes it by a semitone,", "A+↑/↓ by an octave.",
     {"With a scale on, only notes of",
      "that scale can be typed. If the",
      "instrument has SLICE on, the",
      "note picks a slice: C-4 is the",
      "first, and each semitone up is",
      "the next."},
     {"A on --- puts the last note",
      "Tap A twice for a new instrument",
      "A+←/→ one note, A+↑/↓ an octave",
      "A+B clears the note",
      "A+B on --- puts OFF: lets go",
      "of the note that is sounding",
      "Hold A to hear it (NOTE PREV)",
      "R+→ opens its instrument",
      "L+D-PAD fills the rows below"}},
    /* PHRASE_VOLUME */
    {"VOL: how loud this step is", "How hard the note is played,", "from 00 silent to 7F full.",
     {"From 00 silent to 7F full. The",
      "instrument VOL and the mixer",
      "faders also apply."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 7F",
      "L+D-PAD fills the rows below",
      "START plays the phrase"}},
    /* PHRASE_INSTRUMENT */
    {"INST: which sound to use", "The number of the instrument", "that plays the note, 00 to 7F.",
     {"The instrument that plays this",
      "note, 00 to 7F. You set up each",
      "instrument on the INSTRUMENT",
      "screen."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "R+→ opens this instrument",
      "L+D-PAD fills the rows below",
      "START plays the phrase"}},
    /* PHRASE_FX_TYPE */
    {"FX: a command on this step", "A+↑/↓ opens the list and", "shows what each one does.",
     {"A three-letter command, such as",
      "a slide, a repeat or a filter",
      "move. The number to its right",
      "says how much."},
     {"A+←/→ steps through commands",
      "A+↑/↓ opens the command list",
      "Keep A held and move to browse",
      "Let go of A to pick one",
      "A on --- repeats the last one",
      "A+B clears the command",
      "L+D-PAD fills the rows below"}},
    /* PHRASE_FX_VALUE */
    {"FX VALUE: what it is set to", "What it does depends on the", "command to its left.",
     {"What the number does depends on",
      "the command to its left. The",
      "command list explains each one."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 00",
      "L+D-PAD fills the rows below",
      "START plays the phrase"}},

    // ── TABLE ────────────────────────────────────────────────────────────────────────────────────
    /* TABLE_TRANSPOSE */
    {"N: pitch on this row", "Moves the note up or down", "while the table is on this row.",
     {"Counted in semitones from the",
      "note the table plays under. 00",
      "is no change, 0C is one octave",
      "up, F4 is one octave down."},
     {"A+←/→ changes by a semitone",
      "A+↑/↓ changes by an octave",
      "A+B sets it back to 00",
      "A+START plays the table"}},
    /* TABLE_VOLUME */
    {"V: volume on this row", "00 is silent, FF is full.", "-- leaves the volume as it is.",
     {"The volume when the table",
      "reaches this row, from 00 silent",
      "to FF full. -- leaves it as it",
      "is."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to --",
      "A+START plays the table"}},
    /* TABLE_FX_TYPE */
    {"FX: a command on this row", "A+↑/↓ opens the list and", "shows what each one does.",
     {"It runs when the table reaches",
      "this row. For example, HOP jumps",
      "to another row, and TIC sets how",
      "fast its column moves."},
     {"A+←/→ steps through commands",
      "A+↑/↓ opens the command list",
      "Keep A held and move to browse",
      "Let go of A to pick one",
      "A on --- repeats the last one",
      "A+B clears the command"}},
    /* TABLE_FX_VALUE */
    {"FX VALUE: what it is set to", "What it does depends on the", "command to its left.",
     {"What the number does depends on",
      "the command to its left. The",
      "command list explains each one."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 00",
      "A+START plays the table"}},

    // ── INSTRUMENT, on every type ────────────────────────────────────────────────────────────────
    /* INST_TYPE */
    {"TYPE: what kind of sound", "SAMPLER plays a file, SOUNDFONT", "a bank, EXTERNAL a MIDI device.",
     {"SAMPLER plays an audio file.",
      "SOUNDFONT plays one sound from a",
      "SoundFont bank. EXTERNAL sends",
      "the notes to a MIDI device. If",
      "the instrument already has a",
      "sound, it asks before changing."},
     {"A+D-PAD changes the type",
      "A+START plays the instrument"}},
    /* INST_SOURCE_LOAD */
    {"LOAD: choose the sound file", "Opens the file browser for a", "sample or a SoundFont.",
     {"Opens the file browser to pick",
      "the sample or SoundFont this",
      "instrument plays. The instrument",
      "takes the name of the file,",
      "unless you named it yourself."},
     {"A opens the file browser",
      "START there lets you hear it",
      "A there loads it, B goes back"}},
    /* INST_SOURCE_EDIT */
    {"EDIT: open the sample editor", "Trim, slice and change the", "sample of this instrument.",
     {"Trim the sample, cut it into",
      "slices, change its pitch or",
      "rate, and apply effects to it."},
     {"A opens the sample editor",
      "B comes back, asks if unsaved"}},
    /* INST_NAME */
    {"NAME: what to call it", "A opens the keyboard. The name", "shows here and in INST.POOL.",
     {"The name shows here and in",
      "INST.POOL. An empty instrument",
      "takes the name of the file you",
      "load into it."},
     {"A opens the keyboard",
      "START on the keyboard applies",
      "SELECT on the keyboard cancels"}},
    /* INST_ROOT */
    {"ROOT: pitch of the recording", "The note that plays the file", "back at its original speed.",
     {"When you play this note, the",
      "sample plays at its normal speed",
      "and pitch. If notes sound an",
      "octave too high or low, check",
      "ROOT."},
     {"A+←/→ changes by a semitone",
      "A+↑/↓ changes by an octave",
      "A+B sets it back to C-4",
      "A+START plays the instrument"}},
    /* INST_DETUNE */
    {"DETUNE: fine tuning", "80 is in tune. Lower is flatter,", "higher is sharper.",
     {"80 is in tune. The first digit",
      "changes the pitch in whole",
      "semitones, the second in steps",
      "of 1/16 of a semitone."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 80",
      "A+START plays the instrument"}},
    /* INST_TIC */
    {"TIC: how fast the table runs", "Ticks per table row. Lower", "runs the table faster.",
     {"How many ticks the table stays",
      "on each row, for notes of this",
      "instrument. An even step is 12",
      "ticks, so 06 plays two table",
      "rows per step. Lower runs the",
      "table faster."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 06"}},
    /* INST_VOLUME */
    {"VOL: instrument volume", "00 is silent, FF is full.", "Applies to every note.",
     {"The volume of every note this",
      "instrument plays, from 00 silent",
      "to FF full. The VOL of each step",
      "and the mixer faders also apply."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to FF"}},
    /* INST_PAN */
    {"PAN: left to right", "00 is fully left, 80 the middle,", "FF fully right.",
     {"Where the sound sits between the",
      "speakers: 00 is fully left, 80",
      "is the middle, FF is fully",
      "right."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 80"}},
    /* INST_PRESET_SAVE */
    {"SAVE: keep these settings", "Writes a preset file you can", "load into any instrument.",
     {"Saves all settings of this",
      "instrument to a preset file.",
      "LOAD can then put them into any",
      "instrument of any song."},
     {"A opens the keyboard to name it",
      "START on the keyboard saves",
      "SELECT on the keyboard cancels"}},
    /* INST_PRESET_LOAD */
    {"LOAD: open a preset", "Replaces all the settings of", "this instrument.",
     {"Opens the file browser on your",
      "saved presets. Loading one",
      "replaces all the settings of",
      "this instrument."},
     {"A opens the file browser",
      "A there loads it, B goes back"}},
    /* INST_DRIVE */
    {"DRIVE: overdrive", "Adds distortion. 00 is clean,", "higher values crunch more.",
     {"Pushes the sound into soft",
      "distortion. 00 is clean, low",
      "values make it warmer, high",
      "values make it crunchy."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 00"}},
    /* INST_FILTER */
    {"FILTER: the filter type", "LP keeps the lows, HP the highs,", "BP the middle. OFF: no filter.",
     {"LP keeps the low sounds and cuts",
      "the high ones. HP keeps the",
      "highs. BP keeps a band in the",
      "middle. FREQ sets where it cuts,",
      "and RES adds a peak there."},
     {"A+D-PAD picks the type"}},
    /* INST_CRUSH */
    {"CRUSH: fewer bits, more grit", "0 is off. Higher values make", "the sound rougher.",
     {"Lowers the bit depth, so the",
      "sound turns rough and noisy,",
      "like old digital gear. 0 is off,",
      "F is the harshest."},
     {"A+←/→ changes by 1, A+↑/↓ by 4",
      "A+B sets it back to 0"}},
    /* INST_FILTER_FREQ */
    {"FREQ: filter cutoff", "Where the filter starts to cut.", "Works when FILTER is not OFF.",
     {"The frequency where the filter",
      "starts to cut, from low (00) to",
      "high (FF). Does nothing while",
      "FILTER is OFF."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 00"}},
    /* INST_DOWNSAMPLE */
    {"DWNSMPL: lower sample rate", "0 is off. Higher makes it", "duller and grittier.",
     {"Lowers the sample rate for a",
      "dull, gritty sound, like an old",
      "sampler. 0 is off, F is the",
      "roughest."},
     {"A+←/→ changes by 1, A+↑/↓ by 4",
      "A+B sets it back to 0"}},
    /* INST_FILTER_RES */
    {"RES: filter resonance", "Boosts the sound right at FREQ.", "Works when FILTER is not OFF.",
     {"Boosts the sound right at FREQ.",
      "Low values are smooth, high",
      "values ring and whistle. Does",
      "nothing while FILTER is OFF."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 00"}},
    /* INST_REVERB_SEND */
    {"REV: how much reverb", "How much of this instrument", "goes into the reverb.",
     {"How much of this instrument goes",
      "into the reverb, from 00 (none)",
      "to FF (all). A opens the reverb",
      "settings, the same ones as on",
      "EFFECTS."},
     {"A opens the reverb, B closes it",
      "A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 00"}},
    /* INST_DELAY_SEND */
    {"DEL: how much delay", "How much of this instrument", "goes into the delay.",
     {"How much of this instrument goes",
      "into the delay, from 00 (none)",
      "to FF (all). A opens the delay",
      "settings, the same ones as on",
      "EFFECTS."},
     {"A opens the delay, B closes it",
      "A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 00"}},
    /* INST_EQ */
    {"EQ: which EQ to use", "One of 128 EQ slots, 00 to 7F.", "A opens the EQ editor.",
     {"The song has 128 EQ slots, 00 to",
      "7F, and many instruments can use",
      "the same one. -- is no EQ."},
     {"A opens the EQ editor",
      "A+←/→ picks the EQ slot",
      "A+B sets it back to --"}},

    // ── INSTRUMENT, sampler only ─────────────────────────────────────────────────────────────────
    /* INST_SLICE */
    {"SLICE: play slices by note", "Each note plays one slice.", "Slices are made in EDIT.",
     {"When the sample has slices (you",
      "make them in EDIT), each note",
      "plays one slice. CUT stops at",
      "the end of the slice, TRU plays",
      "on to the end of the sample. OFF",
      "ignores the slices."},
     {"A+D-PAD picks the mode"}},
    /* INST_LOOP_MODE */
    {"LOOP: how the sample repeats", "OFF plays once, FWD repeats,", "PNG goes back and forth.",
     {"OFF plays the sample once. FWD",
      "repeats from LOOP ST to LOOP",
      "END, PNG goes back and forth",
      "between them."},
     {"A+D-PAD picks the mode"}},
    /* INST_SAMPLE_START */
    {"START: where playback begins", "00 is the start of the file,", "FF is the end of it.",
     {"Where the sample starts to play,",
      "from 00 at the very start of the",
      "file to FF at the very end."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 00",
      "A+START plays the instrument"}},
    /* INST_LOOP_START */
    {"LOOP ST: where a loop begins", "Playback jumps back here at", "LOOP END. Needs LOOP on.",
     {"Each time playback reaches LOOP",
      "END, it jumps back here. Works",
      "only when LOOP is FWD or PNG."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 00"}},
    /* INST_SAMPLE_END */
    {"END: where playback stops", "Below FF it stops early. Set it", "below START to play to the end.",
     {"FF plays to the end of the file.",
      "With a loop on, the part after",
      "LOOP END is the release tail."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to FF"}},
    /* INST_LOOP_END */
    {"LOOP END: where a loop ends", "At this point playback jumps", "back to LOOP ST. Needs LOOP on.",
     {"FF loops right to the end of the",
      "file. Set it below END to keep a",
      "tail for the release."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to FF"}},
    /* INST_REVERSE */
    {"REVERSE: play backwards", "ON plays the sample from its", "end to its start.",
     {"ON plays the sample from its",
      "end back to its start."},
     {"A+D-PAD turns it on or off"}},

    // ── INSTRUMENT, SoundFont only ───────────────────────────────────────────────────────────────
    /* INST_PATCH */
    {"PATCH: which SoundFont voice", "A SoundFont holds many.", "This picks the one to play.",
     {"A SoundFont holds many voices -",
      "piano, strings, drums. This",
      "picks the one this instrument",
      "plays."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+START plays the voice"}},

    // ── INSTRUMENT, external MIDI only ───────────────────────────────────────────────────────────
    /* INST_MIDI_CHANNEL */
    {"CHAN: MIDI channel", "1 to 16. The channel this", "instrument sends on.",
     {"The channel this instrument",
      "sends its notes on, 1 to 16. Set",
      "your MIDI device to the same",
      "channel."},
     {"A+←/→ changes by 1",
      "A+B sets it back to 1"}},
    /* INST_MIDI_BANK */
    {"BANK: MIDI bank select", "Sent just before the program.", "Leave it off if unsure.",
     {"Sent before PROG to choose a",
      "sound bank on your MIDI device.",
      "---- sends no bank."},
     {"A+→ on ---- turns it on",
      "A+←/→ changes by 1, A+↑/↓ by 128",
      "A+B sets it back to ----"}},
    /* INST_MIDI_PROGRAM */
    {"PROG: MIDI program change", "Picks the sound on your MIDI", "device, 00 to 7F.",
     {"Picks the sound (patch) on your",
      "MIDI device, 00 to 7F. -- sends",
      "no program change."},
     {"A+→ on -- turns it on",
      "A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to --"}},
    /* INST_MIDI_LENGTH */
    {"LEN: note length in ticks", "00 holds the note until the", "next one on the track.",
     {"How long each note is held, in",
      "ticks - an even step is 12. 00",
      "holds it until the next note on",
      "the track."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 00"}},
    /* INST_MIDI_CC_NUMBER */
    {"CC: which controller", "The MIDI controller number to", "send, 00 to 7F.",
     {"The controller number, 00 to 7F.",
      "It is sent with every note,",
      "together with the VAL beside it.",
      "-- sends nothing."},
     {"A+→ on -- turns it on",
      "A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to --"}},
    /* INST_MIDI_CC_VALUE */
    {"VAL: what to send", "The value for the CC to its", "left. 00 to 7F.",
     {"Sent to the controller on its",
      "left with every note, 00 to 7F.",
      "-- sends nothing."},
     {"A+→ on -- turns it on",
      "A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to --"}},

    // ── Appended after the MIDI block, to match the enum ─────────────────────────────────────────
    /* INST_TRANSPOSE */
    {"TSP: follow scale and transpose", "ON: scales and transpose move", "its notes. OFF: they stay put.",
     {"ON lets the scale, the chain TSP",
      "and the song TRANSPOSE change",
      "the notes of this instrument.",
      "OFF keeps its notes exactly as",
      "written, which suits drums."},
     {"A+D-PAD turns it on or off"}},

    // ── PROJECT ──────────────────────────────────────────────────────────────────────────────────
    /* PROJECT_TEMPO */
    {"TEMPO: beats per minute", "From 20 to 999. A+↑/↓ changes", "it by ten.",
     {"How fast the song plays, from 20",
      "to 999 BPM. TAP, to its right,",
      "lets you set it by tapping."},
     {"A+←/→ changes by 1",
      "A+↑/↓ changes by 10",
      "START plays the song"}},
    /* PROJECT_TRANSPOSE */
    {"TRANSPOSE: the whole song", "Moves every note up or down.", "00 leaves the pitch alone.",
     {"Counted in semitones. 00 is no",
      "change, 0C is one octave up, F4",
      "is one octave down. Instruments",
      "with TSP OFF are not moved."},
     {"A+←/→ changes by a semitone",
      "A+↑/↓ changes by an octave"}},
    /* PROJECT_NAME */
    {"NAME: what the song is called", "A opens the keyboard to type", "a new one.",
     {"SAVE uses it as the file name,",
      "and so do the WAV files you",
      "export."},
     {"A opens the keyboard"}},
    /* PROJECT_SAVE */
    {"SAVE: write the song to disk", "It uses the name on the NAME", "row above.",
     {"Saves into the Projects folder,",
      "under the NAME above. A song",
      "saved earlier with the same name",
      "is replaced."},
     {"A saves"}},
    /* PROJECT_LOAD */
    {"LOAD: open another song", "Opens the file browser to", "pick one.",
     {"Opens the file browser on your",
      "songs. Loading one replaces the",
      "song that is open, so save it",
      "first if it has changes."},
     {"A opens the file browser",
      "A there loads it, B goes back"}},
    /* PROJECT_NEW */
    {"NEW: start an empty song", "Asks first. Starts from your", "template if you saved one.",
     {"If you saved a TEMPLATE in",
      "SETTINGS, the new song starts",
      "from it. If the open song has",
      "unsaved changes, it asks first."},
     {"A starts a new song"}},
    /* PROJECT_EXPORT_MIX */
    {"MIX: render to one WAV", "Opens the render panel for one", "WAV with all tracks mixed.",
     {"Opens the render panel for one",
      "stereo WAV in the Renders",
      "folder. Choose which song rows",
      "to play and how many times."},
     {"A opens it, A on RENDER starts",
      "A+D-PAD changes a value",
      "R+↑/↓ picks another song part",
      "B closes the panel"}},
    /* PROJECT_EXPORT_STEMS */
    {"STEMS: render each track", "Opens the render panel for one", "WAV per track.",
     {"Opens the render panel for one",
      "stereo WAV per track, plus one",
      "each for the reverb and the",
      "delay. They go in a folder named",
      "after the song, inside Renders."},
     {"A opens it, A on RENDER starts",
      "A+D-PAD changes a value",
      "R+↑/↓ picks another song part",
      "B closes the panel"}},
    /* PROJECT_COMPACT_SEQ */
    {"SEQ: clear unused patterns", "Empties every chain and", "phrase the song never plays.",
     {"Empties every chain and phrase",
      "the song never plays. It cannot",
      "be undone, so save first."},
     {"A asks, then cleans"}},
    /* PROJECT_COMPACT_INST */
    {"INST: clear unused sounds", "Empties instruments no phrase", "uses, and frees memory.",
     {"Empties every instrument that no",
      "phrase uses, and frees the",
      "memory they took. It cannot be",
      "undone, so save first."},
     {"A asks, then cleans"}},
    /* PROJECT_SYSTEM */
    {"SETTINGS: how the app works", "A opens the settings screen,", "B comes back here.",
     {"Display, buttons, help, theme",
      "and what happens after a crash."},
     {"A opens SETTINGS",
      "B there comes back here"}},
    /* PROJECT_MIDI */
    {"MIDI: devices and clock", "A opens the MIDI screen,", "B comes back here.",
     {"The MIDI devices the app sends",
      "to and listens to, and the",
      "clock."},
     {"A opens the MIDI screen",
      "B there comes back here"}},
    /* PROJECT_EXIT */
    {"EXIT: leave PocketTracker", "Asks first. Save the song", "before you go.",
     {"If the song has unsaved changes,",
      "it asks first."},
     {"A leaves the app"}},

    // ── GROOVE ───────────────────────────────────────────────────────────────────────────────────
    /* GROOVE_TIC */
    {"TIC: how long a step lasts", "In ticks. A on -- adds a step,", "A+B ends the list there.",
     {"Counted in ticks. At PPQ 48, 0C",
      "is an even step. A bigger number",
      "plays the next step later, a",
      "smaller one sooner. 00 skips the",
      "step. The list ends at the first",
      "--."},
     {"A on -- adds an even step",
      "A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B ends the list on this row",
      "START plays the phrase"}},

    // ── SCALE ────────────────────────────────────────────────────────────────────────────────────
    /* SCALE_NAME */
    {"NAME: which scale this is", "A+D-PAD steps through the 38", "built-in scales.",
     {"Steps through the 38 built-in",
      "scales. Choosing one replaces",
      "all twelve notes below, so save",
      "a scale you built yourself",
      "first."},
     {"A+D-PAD picks a built-in scale",
      "A+B goes back to CHROMATIC"}},
    /* SCALE_SAVE */
    {"SAVE: keep this scale", "Writes a file you can load", "into any other project.",
     {"Saves this scale under a name in",
      "the Scales folder. LOAD can then",
      "open it in any song."},
     {"A opens the keyboard to name it",
      "START on the keyboard saves",
      "SELECT on the keyboard cancels"}},
    /* SCALE_LOAD */
    {"LOAD: open a scale", "Replaces the twelve notes", "below it.",
     {"Opens the file browser on your",
      "saved scales. Loading one",
      "replaces the twelve notes of",
      "this scale."},
     {"A opens the file browser",
      "A there loads it, B goes back"}},
    /* SCALE_KEY */
    {"KEY: the key of the song", "The note all 16 scales start", "from. The rows are named by it.",
     {"The note the scales are built",
      "on. It belongs to the song, so",
      "changing it moves all 16 scales",
      "and renames the rows below."},
     {"A+D-PAD changes the key"}},
    /* SCALE_DEGREE */
    {"EN: is this note in the scale", "ON: yes. A note that is off", "moves to the nearest one on.",
     {"ON keeps this note in the scale.",
      "A note that is off (--) moves to",
      "the nearest note that is on. The",
      "key note is always on, and the",
      "last note left cannot be",
      "switched off."},
     {"A+D-PAD turns it on or off"}},

    // ── MODS ─────────────────────────────────────────────────────────────────────────────────────
    /* MOD_TYPE */
    {"TYPE: what kind of mod", "AHD, ADSR, DRUM, TRIG rise and", "fall, LFO swings. --- is off.",
     {"--- is off. AHD and DRUM rise,",
      "hold, then fall away. ADSR and",
      "TRIG rise, then stay at SUS",
      "until the note ends. LFO swings",
      "up and down for as long as the",
      "note plays."},
     {"A+D-PAD picks the type"}},
    /* MOD_DEST */
    {"DEST: what it moves", "The setting this mod changes", "while a note plays.",
     {"Volume, pan, pitch, fine pitch,",
      "filter cutoff, resonance or",
      "sample start. MOD A, MOD R and",
      "MOD B change the amount, the",
      "speed or both of the next mod."},
     {"A+D-PAD picks the target"}},
    /* MOD_AMOUNT */
    {"AMT: how far it moves", "00 does nothing, FF is the", "full swing.",
     {"How strongly the mod moves its",
      "setting. 00 does nothing, FF is",
      "the full swing."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to FF"}},
    /* MOD_ATTACK */
    {"ATK: time up to the peak", "00 is instant. Higher rises", "more slowly.",
     {"How long the mod takes to rise",
      "to its peak. 00 jumps straight",
      "there, higher rises more slowly."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 00"}},
    /* MOD_HOLD */
    {"HOLD: time spent at the peak", "Before the decay starts.", "",
     {"How long it stays at the peak.",
      "Then DEC starts."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 00"}},
    /* MOD_DECAY */
    {"DEC: time to fall away", "How long the drop after the", "peak takes.",
     {"How long it takes to fall after",
      "the peak. AHD and DRUM fall all",
      "the way, ADSR and TRIG fall to",
      "the SUS level."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 00"}},
    /* MOD_SUSTAIN */
    {"SUS: the level it settles at", "Where it stays for as long as", "the note is held.",
     {"The level it stays at after the",
      "fall, for as long as the note is",
      "held."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 80"}},
    /* MOD_RELEASE */
    {"REL: time to fall at the end", "Starts when the note stops.", "",
     {"How long it takes to fall once",
      "the note ends. An OFF in the",
      "note column ends a note."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 00"}},
    /* MOD_OSC */
    {"OSC: the shape of the LFO", "Triangle, sine, ramps and", "squares, plus two random.",
     {"Triangle, sine, ramps, curves",
      "and squares, each up or down.",
      "RND jumps to random values, DRK",
      "wanders between them."},
     {"A+D-PAD picks the shape"}},
    /* MOD_TRIG */
    {"TRIG: how the LFO starts", "RETG restarts on each note,", "FREE never restarts.",
     {"RETG starts the wave over on",
      "every note. FREE runs on and",
      "never restarts. ONCE starts over",
      "on each note, plays one cycle",
      "and stops. HOLD takes one value",
      "when the note starts and keeps",
      "it."},
     {"A+D-PAD picks the mode"}},
    /* MOD_FREQ */
    {"FREQ: how fast the LFO runs", "Higher is faster.", "",
     {"How fast the LFO swings. Higher",
      "is faster."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 40"}},

    // ── INST.POOL ────────────────────────────────────────────────────────────────────────────────
    /* POOL_SLOT */
    {"SLOT: pick an instrument", "A on an empty slot loads a", "file. A+B empties the slot.",
     {"The name of the instrument. An",
      "empty one can load a sample or",
      "SoundFont right here. To change",
      "one that has a sound, open it on",
      "INSTRUMENT."},
     {"A on an empty slot loads a file",
      "A+B empties the slot, no undo",
      "R+→ opens it on INSTRUMENT",
      "A+START plays it"}},

    // ── MIXER ────────────────────────────────────────────────────────────────────────────────────
    /* MIXER_TRACK_VOL */
    {"TRACK VOLUME: one fader", "00 is silent, FF is full.", "One column per track.",
     {"The volume of this track, from",
      "00 silent to FF full. A VTR",
      "command in a phrase can change",
      "it while the song plays."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to FF",
      "R+B mutes, R+A solos the track"}},
    /* MIXER_MASTER_VOL */
    {"MIX: the master volume", "Sets the volume of the whole", "mix, effects included.",
     {"The last volume before the",
      "speakers. It turns down every",
      "track and the reverb and delay",
      "too. A VMV command in a phrase",
      "can change it."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to FF"}},
    /* MIXER_REVERB_RETURN */
    {"REV: reverb level in the mix", "How loud the reverb is.", "R+B mutes it, R+A solos it.",
     {"How loud the reverb is in the",
      "mix. What goes into it is set by",
      "the REV of each instrument."},
     {"A opens the reverb, B closes it",
      "A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 80",
      "R+B mutes, R+A solos it"}},
    /* MIXER_DELAY_RETURN */
    {"DEL: delay level in the mix", "How loud the delay is.", "R+B mutes it, R+A solos it.",
     {"How loud the delay is in the",
      "mix. What goes into it is set by",
      "the DEL of each instrument."},
     {"A opens the delay, B closes it",
      "A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 80",
      "R+B mutes, R+A solos it"}},
    /* MIXER_MASTER_EQ */
    {"EQ: EQ on the whole mix", "One of 128 EQ slots, or --.", "A opens the EQ editor.",
     {"One of the 128 EQ slots of the",
      "song, used on the whole mix. --",
      "is no EQ."},
     {"A opens the EQ editor",
      "A+←/→ picks the EQ slot",
      "A+B sets it back to --"}},
    /* MIXER_MASTER_FX */
    {"OTT/DUST: amount on the mix", "How strong the master effect", "is. EFFECTS picks which one.",
     {"How strongly the master effect",
      "works on the whole mix. 00 is",
      "off. The EFFECTS screen picks",
      "OTT or DUST."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 00"}},
    /* MIXER_LIMITER */
    {"LIM: push into the limiter", "Makes the mix louder and", "flatter. 00 adds nothing.",
     {"Pushes the whole mix harder into",
      "the limiter at the very end, for",
      "a louder and flatter sound. 00",
      "adds no push."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 00"}},

    // ── EFFECTS ──────────────────────────────────────────────────────────────────────────────────
    /* FX_MASTER_TYPE */
    {"MASTER FX: effect on the mix", "OTT makes it loud and bright,", "DUST makes it sound old.",
     {"OTT compresses the whole mix in",
      "three bands for a loud, bright",
      "sound. DUST makes it sound old",
      "and worn. The MIXER sets how",
      "strong it is."},
     {"A+D-PAD picks OTT or DUST"}},
    /* FX_REVERB_SIZE */
    {"SIZE: how big the room is", "Bigger spreads the echoes out.", "PLATE and FOIL ignore it.",
     {"Bigger spreads the echoes out,",
      "but does not make the reverb",
      "ring longer. On OLD it also",
      "slows the movement from MOD.",
      "PLATE and FOIL have no room, so",
      "they ignore it."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 60"}},
    /* FX_REVERB_DAMP */
    {"DAMP: how bright the tail is", "Lower makes the reverb darker.", "FF keeps all the highs.",
     {"00 is dark, FF keeps the highs."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 80"}},
    /* FX_REVERB_EQ */
    {"INP EQ: tone into the reverb", "Shapes the sound going in.", "A opens the EQ editor.",
     {"One of the 128 EQ slots, used on",
      "the sound before it enters the",
      "reverb. -- is no EQ."},
     {"A opens the EQ editor",
      "A+←/→ picks the EQ slot",
      "A+B sets it back to --"}},
    /* FX_DELAY_TIME */
    {"TIME: the gap between echoes", "A free value or a note length.", "A switches between them.",
     {"In free mode it runs from 00 to",
      "FF. In note sync it is a note",
      "length from 1/1 to 1/32 - plain,",
      "triplet or dotted - and follows",
      "the TEMPO."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A switches free and note sync",
      "A+B sets free time back to 40"}},
    /* FX_DELAY_FEEDBACK */
    {"FDBK: how many echoes", "Higher repeats for longer.", "Very high never stops.",
     {"How much of each echo comes back",
      "as the next one. Higher repeats",
      "for longer. Near FF they pile up",
      "and get loud."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 60"}},
    /* FX_DELAY_TO_REVERB */
    {"REV: delay into the reverb", "Sends the echoes through the", "reverb as well.",
     {"How much of the echoes goes into",
      "the reverb, so they melt into a",
      "room. 00 sends none."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 00"}},
    /* FX_DELAY_EQ */
    {"INP EQ: tone into the delay", "Shapes the sound going in.", "A opens the EQ editor.",
     {"One of the 128 EQ slots, used on",
      "the sound before it enters the",
      "delay. -- is no EQ."},
     {"A opens the EQ editor",
      "A+←/→ picks the EQ slot",
      "A+B sets it back to --"}},

    // ── SETTINGS ─────────────────────────────────────────────────────────────────────────────────
    /* SET_LAYOUT */
    {"LAYOUT: how the app fits", "Fullscreen, landscape, or", "portrait with buttons.",
     {"Full screen, or portrait with",
      "buttons drawn under the tracker."},
     {"A+D-PAD picks the layout"}},
    /* SET_SKIN */
    {"SKIN: the button artwork", "How the on-screen buttons", "look in portrait.",
     {"How the on-screen buttons look",
      "in portrait."},
     {"A+D-PAD picks a skin"}},
    /* SET_SCALING */
    {"SCALING: how pixels are drawn", "INT keeps them sharp,", "BILINEAR smooths them.",
     {"INT keeps every pixel sharp as",
      "the picture is stretched to your",
      "screen, BILINEAR smooths them."},
     {"A+D-PAD picks INT or BILINEAR"}},
    /* SET_OVERLAY */
    {"OVERLAY: a picture on top", "Laid over the screen to look", "like an old TV or LCD.",
     {"A picture laid over the screen",
      "for the look of an old TV or",
      "LCD. STR beside it sets how",
      "strong it is."},
     {"A+D-PAD picks the picture"}},
    /* SET_OVERLAY_STRENGTH */
    {"STR: how strong it is", "00 is invisible, FF is the", "full picture.",
     {"How strong the overlay is, from",
      "00 (invisible) to FF (full)."},
     {"A+←/→ changes by 1, A+↑/↓ by 16"}},
    /* SET_BTN_SOUND */
    {"BTN SOUND: a click per press", "For the on-screen buttons.", "",
     {"A click when you press an",
      "on-screen button. VOL beside it",
      "sets how loud."},
     {"A+D-PAD turns it on or off"}},
    /* SET_BTN_SOUND_VOL */
    {"VOL: how loud the click is", "00 is silent, FF is full.", "",
     {"From 00 silent to FF full."},
     {"A+←/→ changes by 1, A+↑/↓ by 16"}},
    /* SET_BTN_VIBRO */
    {"BTN VIBRO: a buzz per press", "For the on-screen buttons.", "",
     {"A short buzz when you press an",
      "on-screen button. POW beside it",
      "sets how strong."},
     {"A+D-PAD turns it on or off"}},
    /* SET_BTN_VIBRO_POW */
    {"POW: how hard it buzzes", "LO is a tick, HI is a click.", "There is nothing in between.",
     {"LO is a light tick, HI a firm",
      "click."},
     {"A+D-PAD picks LO or HI"}},
    /* SET_ABXY */
    {"ABXY: where A and B are", "Match it to the labels", "printed on your own pad.",
     {"AUTO trusts the controller. Pick",
      "NINTENDO if A and B work the",
      "wrong way round, or XBOX if A is",
      "the bottom button."},
     {"A+D-PAD picks the layout"}},
    /* SET_KB_INSERT */
    {"KB INSERT: where a letter goes", "BEFORE the keyboard cursor,", "or AFTER it.",
     {"When you type a name, a new",
      "letter goes BEFORE or AFTER the",
      "text cursor."},
     {"A+D-PAD picks BEFORE or AFTER"}},
    /* SET_CURSOR */
    {"CURSOR: coming back to a screen", "REMEMBER keeps where you were,", "REFRESH goes to the top.",
     {"REMEMBER puts the cursor back",
      "where you left it on a screen.",
      "REFRESH starts at the top."},
     {"A+D-PAD picks the mode"}},
    /* SET_NAV */
    {"NAV: where B+D-PAD takes you", "SONG follows the song, POOL", "goes by number.",
     {"On CHAIN and PHRASE, B+D-PAD",
      "goes to another chain or phrase.",
      "SONG follows the order of the",
      "song. POOL goes through every",
      "number, 00 to FF."},
     {"A+D-PAD picks SONG or POOL"}},
    /* SET_FOLDER */
    {"FOLDER: where loading starts", "REMEMBER opens the last folder,", "REFRESH the default one.",
     {"When you load a sample, REMEMBER",
      "opens the folder you used last",
      "time. REFRESH always starts in",
      "the default folder."},
     {"A+D-PAD picks the mode"}},
    /* SET_NOTE_PREVIEW */
    {"NOTE PREV: hear what you type", "A phrase note plays for as", "long as you hold A on it.",
     {"ON plays a phrase note for as",
      "long as you hold A on it - while",
      "typing it, changing it, or just",
      "holding it."},
     {"A+D-PAD turns it on or off"}},
    /* SET_VISUALIZER */
    {"VISUALIZER: the top strip", "A wave, a wave per track, or", "a spectrum.",
     {"SCOPE shows the sound as a wave,",
      "FLAT shows nothing. OCTA and",
      "OCTA.F show a small wave for",
      "each track. SPECT and SPCT.P",
      "show the spectrum."},
     {"A+D-PAD picks the mode"}},
    /* SET_THEME */
    {"THEME: the colours", "A opens the theme editor,", "where every colour is a row.",
     {"The theme editor shows every",
      "colour of the app as a row you",
      "can change."},
     {"A opens the theme editor",
      "B there comes back here"}},
    /* SET_TEMPLATE_SAVE */
    {"SAVE: start new songs from this", "Every NEW song begins as a", "copy of this one.",
     {"Every NEW song will begin as a",
      "copy of the song open now - its",
      "instruments, tempo and",
      "everything else."},
     {"A saves the template"}},
    /* SET_TEMPLATE_CLEAR */
    {"CLEAR: forget the template", "NEW goes back to an empty", "song.",
     {"NEW starts from an empty song",
      "again."},
     {"A clears the template"}},
    /* SET_RESUME */
    {"RESUME: after a crash", "ASK offers the recovered", "work, AUTO just opens it.",
     {"What happens to unsaved work if",
      "the app was closed without",
      "saving. ASK offers it back at",
      "the next start, AUTO just opens",
      "it."},
     {"A+D-PAD picks ASK or AUTO"}},
    /* SET_TRACE */
    {"TRACE: write a debug log", "Records what the sequencer", "does. Off unless asked for.",
     {"Writes down what the sequencer",
      "does, to help find a bug. Leave",
      "it off unless you are asked to",
      "turn it on."},
     {"A+D-PAD turns it on or off"}},

    // ── MIDI ─────────────────────────────────────────────────────────────────────────────────────
    /* MIDI_OUTPUT */
    {"OUTPUT: the device to send to", "Gets the notes and the clock.", "AUTO finds one by itself.",
     {"The device that gets the notes",
      "of EXTERNAL instruments, and the",
      "clock. AUTO picks the first",
      "device it finds, and picks it",
      "again when you plug it back in.",
      "OFF sends nothing."},
     {"A+D-PAD picks a device"}},
    /* MIDI_INPUT */
    {"INPUT: the device to play from", "Your keyboard or controller.", "AUTO finds one by itself.",
     {"The keyboard or controller you",
      "play the app from. AUTO picks",
      "the first device it finds, and",
      "picks it again when you plug it",
      "back in. OFF listens to nothing."},
     {"A+D-PAD picks a device"}},
    /* MIDI_OFFSET */
    {"OFFSET: nudge the timing", "Minus sends earlier, plus", "later. In milliseconds.",
     {"Sends everything earlier or",
      "later, from -99 to 99 ms, so an",
      "outside synth plays in time with",
      "the app. AUTO works it out from",
      "the sound delay of this device.",
      "You can also set it by ear while",
      "the song plays."},
     {"A+←/→ changes by 1 ms",
      "A+↑/↓ changes by 10 ms",
      "A+B goes back to AUTO",
      "START plays the song"}},
    /* MIDI_SYNC */
    {"SYNC OUT: send the tempo out", "Other gear can follow your", "tempo, start and stop.",
     {"Sends a MIDI clock - 24 pulses a",
      "beat - with start and stop, so",
      "other gear can play along at the",
      "same tempo."},
     {"A+D-PAD turns it on or off"}},
    /* MIDI_PROG_CHG */
    {"PROG CHG: send patch changes", "Sends BANK and PROG from the", "instrument before a note.",
     {"ON sends the BANK and PROG of",
      "each EXTERNAL instrument before",
      "its notes, so your MIDI device",
      "switches to the right sound.",
      "Saved with the song."},
     {"A+D-PAD turns it on or off"}},
    /* MIDI_KEYS */
    {"KEYS: how a keyboard plays", "MONO plays one note at a time,", "POLY lets you play chords.",
     {"A MIDI keyboard plays the",
      "instrument under the PHRASE",
      "cursor, or the last one used, on",
      "the SONG cursor track. MONO",
      "plays one note at a time. POLY",
      "2-8 spreads a chord over that",
      "many tracks, from the cursor",
      "track to the right."},
     {"A+←/→ picks MONO or POLY 2-8"}},
    /* MIDI_PANIC */
    {"PANIC: silence everything", "A sends all notes off on", "every channel.",
     {"Stops the notes on every channel",
      "- for a note that is stuck on an",
      "outside synth."},
     {"A sends it"}},
    /* MIDI_TEST */
    {"TEST: check the connection", "A sends one C-4 on channel", "1 and says what happened.",
     {"Sends one short C-4 on channel 1",
      "and says whether it went out -",
      "a quick check of the cable."},
     {"A sends the test note"}},
    /* MIDI_CTL_CH */
    {"CTL CH: the channel for knobs", "Where mapped knobs are read", "from. ALL means any channel.",
     {"A mapped knob only moves its",
      "control, and does not also play",
      "a track. Other knobs work as",
      "before, so ALL changes nothing",
      "until you map one.",
      "",
      "Pick one channel if two devices",
      "send the same knob numbers."},
     {"A+←/→ picks ALL or 01 to 16"}},
    /* MIDI_MAPPING */
    {"MAPPING: knobs to controls", "A opens the list of knobs", "and what each one moves.",
     {"Every mapped knob and what it",
      "moves. Add, change or remove",
      "them here. Saved with the song.",
      "",
      "Or put the cursor on a value on",
      "MIXER, EFFECTS or INSTRUMENT,",
      "hold R alone and turn a knob.",
      "Needs a device on the INPUT row."},
     {"A opens the list"}},

    // ── The MIDI mapping list ────────────────────────────────────────────────────────────────────
    /* MAP_CC */
    {"CC: which knob", "The controller number the", "knob sends. 00 to 7F.",
     {"The number your knob sends. Most",
      "controllers let you set it, and",
      "most print it on the knob."},
     {"A+←/→ changes by 1",
      "A+↑/↓ changes by 16",
      "A+B removes this line"}},
    /* MAP_MIN */
    {"MIN: the low end", "Where the knob puts the", "control turned all the way down.",
     {"The value the control takes when",
      "the knob is turned all the way",
      "down. Put MIN above MAX and the",
      "knob works backwards."},
     {"A+←/→ changes by 1",
      "A+↑/↓ changes by 16",
      "A+B sets it back to the lowest"}},
    /* MAP_MAX */
    {"MAX: the high end", "Where the knob puts the", "control turned all the way up.",
     {"The value the control takes when",
      "the knob is turned all the way",
      "up. Put MAX below MIN and the",
      "knob works backwards."},
     {"A+←/→ changes by 1",
      "A+↑/↓ changes by 16",
      "A+B sets it back to the highest"}},
    /* MAP_DEST */
    {"DEST: what the knob moves", "A group, then a control", "inside it.",
     {"Two cells: the group - track,",
      "master, reverb, delay or",
      "instrument - and the control in",
      "it. A new choice also sets its",
      "own MIN and MAX.",
      "",
      "A+↑/↓ shows them all at once -",
      "keep A held, look, and let go."},
     {"A+←/→ steps through the list",
      "A+↑/↓ opens the full list",
      "A+B removes this line"}},
    /* MAP_SCOPE */
    {"WHICH ONE: track or instrument", "Which track or which", "instrument this line moves.",
     {"For a track fader or an",
      "instrument control, this says",
      "which track or instrument. It",
      "stays fixed, so moving the",
      "cursor later does not change it."},
     {"A+←/→ changes by 1",
      "A+↑/↓ changes by 16",
      "A+B removes this line"}},
    /* MAP_ADD */
    {"ADD: a new knob", "A adds a line, then set", "the CC and what it moves.",
     {"Adds a line to the list. It",
      "starts on track 1 volume across",
      "the whole range - set the CC",
      "number and the control from",
      "there. 128 lines at most."},
     {"A adds a line"}},

    // ── The EQ editor ────────────────────────────────────────────────────────────────────────────
    /* SCREEN_EQ */
    {"EQ: three bands of tone", "The curve shows the three", "bands added together.",
     {"Three bands that shape the sound",
      "of whatever opened this editor.",
      "The curve shows all three",
      "together, drawn over the live",
      "sound. Many places can share one",
      "EQ slot."},
     {"←/→ picks a band, ↑/↓ a setting",
      "A+←/→ small step, A+↑/↓ large",
      "A+B resets the setting",
      "B+←/→ picks another EQ slot",
      "START plays, B closes"}},
    /* EQ_TYPE */
    {"TYPE: what this band does", "SHELF lifts or lowers one end,", "BELL one spot, CUT removes it.",
     {"LOSHELF lifts or lowers",
      "everything below FREQ, HISHELF",
      "everything above it. BELL works",
      "only around FREQ. LOWCUT removes",
      "the lows, HICUT the highs. OFF",
      "turns the band off."},
     {"A+D-PAD picks the type"}},
    /* EQ_FREQ */
    {"FREQ: where the band sits", "The frequency it works on,", "20 Hz up to 20 kHz.",
     {"The frequency the band works on,",
      "from 20 Hz (deep) to 20 kHz",
      "(high). Each small step changes",
      "the number shown."},
     {"A+←/→ small step, A+↑/↓ large",
      "A+B sets it back to the middle"}},
    /* EQ_GAIN */
    {"GAIN: how much to lift or cut", "0.0 changes nothing. Up to", "12 dB up or down.",
     {"How much the band lifts or",
      "lowers the sound, up to 12 dB",
      "each way. 0.0 changes nothing."},
     {"A+←/→ changes by 0.1 dB",
      "A+↑/↓ changes by 1 dB",
      "A+B sets it back to 0.0"}},
    /* EQ_Q */
    {"Q: how wide the band is", "Low is broad and gentle,", "high is narrow and sharp.",
     {"Low is broad and gentle, high is",
      "narrow and sharp."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 80"}},

    // ── The theme editor ─────────────────────────────────────────────────────────────────────────
    /* SCREEN_THEME */
    {"THEME: all the colours", "Each row is one colour, mixed", "from red, green and blue.",
     {"Each row is one colour of the",
      "app, mixed from red, green and",
      "blue. Changes show at once and",
      "stay when you close the editor."},
     {"↑/↓ picks a row, ←/→ R, G or B",
      "A+←/→ changes by 1, A+↑/↓ by 16",
      "START plays, B closes"}},
    /* THEME_NAME */
    {"THEME: the built-in palettes", "A+D-PAD steps through them and", "sets every colour below.",
     {"Steps through the built-in",
      "palettes. Choosing one replaces",
      "every colour below."},
     {"A+D-PAD picks a palette"}},
    /* THEME_SAVE */
    {"SAVE: keep this palette", "Writes a theme file you can", "load again or share.",
     {"Saves this palette under a name",
      "in the Themes folder, to load",
      "again or share."},
     {"A opens the keyboard to name it",
      "START on the keyboard saves",
      "SELECT on the keyboard cancels"}},
    /* THEME_LOAD */
    {"LOAD: open a palette", "Replaces every colour row", "below.",
     {"Opens the file browser on your",
      "saved themes. Loading one",
      "replaces every colour."},
     {"A opens the file browser",
      "A there loads it, B goes back"}},
    /* THEME_SCHEME */
    {"SCHEME: colours that match", "The rule ROLL follows to pick", "colours that go together.",
     {"ALL takes any colour. The others",
      "start from one colour and add",
      "its own shades, its neighbours,",
      "its opposite, or three or four",
      "colours spread evenly around the",
      "colour wheel."},
     {"A+D-PAD picks the scheme"}},
    /* THEME_ROLL */
    {"ROLL: a whole new palette", "A makes a random one. L+A keeps", "a row, R+A rolls just that row.",
     {"Makes a random palette that",
      "follows the SCHEME beside it,",
      "keeping every row you have held.",
      "A held row shows a star and does",
      "not change."},
     {"A makes a new palette",
      "L+A on a colour row keeps it",
      "R+A on a colour row re-rolls it"}},
    /* THEME_BACKGROUND */
    {"BACKGROUND: behind everything", "The main background, and the", "text on the cursor block.",
     {},
     {"←/→ picks R, G or B",
      "A+←/→ changes by 1",
      "A+↑/↓ changes by 16"}},
    /* THEME_ROW_4TH */
    {"ROW 4TH: every fourth row", "Marks every fourth row, and", "the text on marked cells.",
     {},
     {"←/→ picks R, G or B",
      "A+←/→ changes by 1",
      "A+↑/↓ changes by 16"}},
    /* THEME_ROW_CURSOR */
    {"ROW CURSOR: where you are", "The block behind the cursor,", "and the marks showing where.",
     {},
     {"←/→ picks R, G or B",
      "A+←/→ changes by 1",
      "A+↑/↓ changes by 16"}},
    /* THEME_ROW_SELECT */
    {"ROW SELECT: marked cells", "The colour behind the cells", "you have marked to copy.",
     {},
     {"←/→ picks R, G or B",
      "A+←/→ changes by 1",
      "A+↑/↓ changes by 16"}},
    /* THEME_TXT_TITLE */
    {"TXT TITLE: the headings", "Screen names, and the border", "around a pop-up box.",
     {},
     {"←/→ picks R, G or B",
      "A+←/→ changes by 1",
      "A+↑/↓ changes by 16"}},
    /* THEME_TXT_PARAM */
    {"TXT PARAM: the labels", "The name beside a value, and", "the column headers.",
     {},
     {"←/→ picks R, G or B",
      "A+←/→ changes by 1",
      "A+↑/↓ changes by 16"}},
    /* THEME_TXT_VALUE */
    {"TXT VALUE: the numbers", "Every value you can type or", "change.",
     {},
     {"←/→ picks R, G or B",
      "A+←/→ changes by 1",
      "A+↑/↓ changes by 16"}},
    /* THEME_TXT_EMPTY */
    {"TXT EMPTY: the blanks", "The -- and --- a cell shows", "when nothing is set.",
     {},
     {"←/→ picks R, G or B",
      "A+←/→ changes by 1",
      "A+↑/↓ changes by 16"}},
    /* THEME_TXT_PLAY */
    {"TXT PLAY: the playhead", "The arrow marking where each", "track is playing.",
     {},
     {"←/→ picks R, G or B",
      "A+←/→ changes by 1",
      "A+↑/↓ changes by 16"}},
    /* THEME_VIZ_BG */
    {"VIZ BG: behind the top strip", "Also the background of this", "help panel.",
     {},
     {"←/→ picks R, G or B",
      "A+←/→ changes by 1",
      "A+↑/↓ changes by 16"}},
    /* THEME_VIZ_LINE */
    {"VIZ LINE: the centre line", "The line across the middle", "of the wave.",
     {},
     {"←/→ picks R, G or B",
      "A+←/→ changes by 1",
      "A+↑/↓ changes by 16"}},
    /* THEME_VIZ_WAVE */
    {"VIZ WAVE: the waveform", "The wave in the top strip,", "and the text of this help.",
     {},
     {"←/→ picks R, G or B",
      "A+←/→ changes by 1",
      "A+↑/↓ changes by 16"}},
    /* THEME_MTR_BG */
    {"MTR BG: behind the meters", "Also the inside of every", "pop-up box in the app.",
     {},
     {"←/→ picks R, G or B",
      "A+←/→ changes by 1",
      "A+↑/↓ changes by 16"}},
    /* THEME_MTR_LOW */
    {"MTR LOW: a quiet meter", "The bottom of a level bar on", "the mixer.",
     {},
     {"←/→ picks R, G or B",
      "A+←/→ changes by 1",
      "A+↑/↓ changes by 16"}},
    /* THEME_MTR_MID */
    {"MTR MID: a loud meter", "The middle of a level bar on", "the mixer.",
     {},
     {"←/→ picks R, G or B",
      "A+←/→ changes by 1",
      "A+↑/↓ changes by 16"}},
    /* THEME_MTR_HIGH */
    {"MTR HIGH: a very loud meter", "The top of a level bar on", "the mixer.",
     {},
     {"←/→ picks R, G or B",
      "A+←/→ changes by 1",
      "A+↑/↓ changes by 16"}},
    /* THEME_EQ_FILL */
    {"EQ FILL: under the spectrum", "The area under the live", "spectrum in the EQ editor.",
     {},
     {"←/→ picks R, G or B",
      "A+←/→ changes by 1",
      "A+↑/↓ changes by 16"}},
    /* THEME_EQ_BORDER */
    {"EQ BORDER: the fixed lines", "The spectrum outline, and the", "0 dB line behind the curve.",
     {},
     {"←/→ picks R, G or B",
      "A+←/→ changes by 1",
      "A+↑/↓ changes by 16"}},
    /* THEME_EQ_TXT */
    {"EQ TXT: the EQ labels", "The frequency numbers across", "the EQ panel.",
     {},
     {"←/→ picks R, G or B",
      "A+←/→ changes by 1",
      "A+↑/↓ changes by 16"}},
    /* THEME_EQ_BG */
    {"EQ BG: behind the EQ panel", "The background of the panel", "in the EQ editor.",
     {"RANDOMIZE makes it the same as",
      "BACKGROUND or VIZ BG."},
     {"←/→ picks R, G or B",
      "A+←/→ changes by 1",
      "A+↑/↓ changes by 16"}},

    // ── The sample editor ────────────────────────────────────────────────────────────────────────
    /* SE_ZOOM */
    {"ZOOM: how close the view is", "R+↑ zooms in, R+↓ out,", "from any cell on this screen.",
     {"From 1x to 16x. The closer you",
      "zoom, the finer an edge or a",
      "slice moves."},
     {"A+D-PAD zooms",
      "R+↑/↓ zooms from any row"}},
    /* SE_SOURCE */
    {"SOURCE: which channel to use", "LEFT, RIGHT or MONO. Only", "STEREO keeps both sides.",
     {"The left channel, the right one,",
      "both (STEREO), or the two mixed",
      "together (MONO). SAVE writes",
      "what you pick here. A mono file",
      "is always MONO."},
     {"A+D-PAD picks the source"}},
    /* SE_RATE */
    {"RATE: how much detail to keep", "HIGH is the full rate, NORM", "half of it, LOFI a quarter.",
     {"HIGH keeps the full sample rate.",
      "NORM halves it and LOFI quarters",
      "it, for a duller, grittier",
      "sound."},
     {"A+D-PAD picks the rate"}},
    /* SE_PITCH */
    {"PITCH: tune the whole sample", "In semitones. It is written", "into the file when you SAVE.",
     {"Moves the pitch up or down, from",
      "-24 to +24 semitones. START",
      "plays it with the change, and",
      "SAVE writes it into the file. A",
      "TSTRETCH also uses it, and keeps",
      "the length it fits to."},
     {"A+←/→ changes by a semitone",
      "A+↑/↓ changes by 16",
      "A+B goes back to +0"}},
    /* SE_DURATION */
    {"DURATION: the length to fit", "What SYNC on the EFFECT row", "stretches the sample to.",
     {"The length SYNC fits the sample",
      "to, from 4 BAR down to 1/32, at",
      "the song TEMPO."},
     {"A+D-PAD picks the length"}},
    /* SE_SNAP */
    {"SNAP: stops edits clicking", "Puts a dragged edge where", "the wave crosses the middle."},
    /* SE_SEL_START */
    {"START: start of the selection", "A+←/→ drags it a little,", "A+↑/↓ a lot.",
     {"The buttons below work on the",
      "selection, the part between",
      "START and END. An edge lands",
      "where the wave is quiet, so a",
      "cut does not click."},
     {"A+←/→ drags it a little",
      "A+↑/↓ drags it a lot",
      "A+B puts it at the very start"}},
    /* SE_SEL_END */
    {"END: end of the selection", "A+←/→ drags it a little,", "A+↑/↓ a lot.",
     {"The buttons below work on the",
      "selection, the part between",
      "START and END. An edge lands",
      "where the wave is quiet, so a",
      "cut does not click."},
     {"A+←/→ drags it a little",
      "A+↑/↓ drags it a lot",
      "A+B puts it at the very end"}},
    /* SE_SLICE_METHOD */
    {"SLICE: how to cut it up", "TRANSIENT finds the hits,", "DIVIDE cuts equal parts.",
     {"TRANSIENT finds the hits in the",
      "sound, DIVIDE cuts it into equal",
      "parts, MANUAL lets you place the",
      "cuts yourself. OFF shows the",
      "marks the file came with."},
     {"A+D-PAD picks the method"}},
    /* SE_SLICE_SENS */
    {"SENS: how many hits to find", "Higher finds more of them,", "quiet ones included.",
     {"Each hit found starts a new",
      "slice. Higher finds more, quiet",
      "ones too."},
     {"A+←/→ changes by 1, A+↑/↓ by 16"}},
    /* SE_SLICE_BY */
    {"BY: how many equal parts", "The sample is cut into this", "many, end to end.",
     {"The sample is cut into this many",
      "slices, end to end."},
     {"A+←/→ changes by 1, A+↑/↓ by 16"}},
    /* SE_SLICE_INDEX */
    {"SLICE: the slice you are on", "A+D-PAD steps through them.", "In MANUAL, A cuts as it plays.",
     {"Stepping to a slice selects it,",
      "so START plays just that slice.",
      "In MANUAL, play the sample and",
      "tap A on every hit to cut it",
      "there."},
     {"A+D-PAD steps through slices",
      "A cuts at the playhead (MANUAL)"}},
    /* SE_SLICE_POS */
    {"START: where this slice starts", "A+D-PAD drags it. A+B puts it", "back, or removes one you made.",
     {"Drag a found hit or an equal cut",
      "to where it sounds right."},
     {"A+←/→ drags it a little",
      "A+↑/↓ drags it a lot",
      "A+B puts it back where it was",
      "A+B under MANUAL deletes it"}},
    /* SE_OP_CROP */
    {"CROP: keep only the selection", "Everything outside it is", "thrown away.",
     {"The rest of the sample is thrown",
      "away."},
     {"A crops, UNDO takes it back"}},
    /* SE_OP_COPY */
    {"COPY: copy the selection", "Puts it on the clipboard and", "changes nothing.",
     {"PASTE can then put it in. The",
      "sample does not change."},
     {"A copies"}},
    /* SE_OP_CUT */
    {"CUT: copy it, then remove it", "The sample gets shorter by", "the length you took.",
     {"PASTE can then put it back in.",
      "The sample gets shorter."},
     {"A cuts, UNDO takes it back"}},
    /* SE_OP_DUPL */
    {"DUPL: repeat the selection", "Adds another copy of it on", "the end of the sample.",
     {"The copy goes on the end of the",
      "sample."},
     {"A adds it, UNDO takes it back"}},
    /* SE_OP_PASTE */
    {"PASTE: put the copy in", "Inserts it at the start of", "the selection.",
     {"What you copied or cut goes in",
      "at the START of the selection.",
      "The sample gets longer."},
     {"A pastes, UNDO takes it back"}},
    /* SE_OP_DEL */
    {"DEL: remove the selection", "The sample gets shorter by", "the length you cut.",
     {"Nothing is copied. The sample",
      "gets shorter."},
     {"A deletes, UNDO takes it back"}},
    /* SE_OP_NORM */
    {"NORM: as loud as it can go", "Lifts the selection until", "its loudest peak is full.",
     {"Turns the selection up until its",
      "loudest peak is at full level."},
     {"A does it, UNDO takes it back"}},
    /* SE_OP_FADE_IN */
    {"FADE+: fade the selection in", "It rises from silence to", "full over its own length.",
     {"It rises from silence to full",
      "across its length."},
     {"A does it, UNDO takes it back"}},
    /* SE_OP_FADE_OUT */
    {"FADE-: fade the selection out", "It falls from full to", "silence over its length.",
     {"It falls from full to silence",
      "across its length."},
     {"A does it, UNDO takes it back"}},
    /* SE_OP_SILENCE */
    {"SLNC: empty the selection", "Wipes what is there and", "keeps the length.",
     {"Makes the selection silent. The",
      "sample keeps its length."},
     {"A does it, UNDO takes it back"}},
    /* SE_OP_REVERSE */
    {"REV: play it backwards", "Turns the selection around,", "end to start.",
     {"Turns the selection around, end",
      "to start."},
     {"A does it, UNDO takes it back"}},
    /* SE_OP_UNDO */
    {"UNDO: take back the last edit", "Only one step back. It does", "not go back to the saved file.",
     {"Undoes only the last edit. The",
      "sample still counts as changed,",
      "so leaving the editor asks",
      "first."},
     {"A undoes the last edit"}},
    /* SE_FX_TYPE */
    {"EFFECT: change the sound", "OTT, DUST, DRIVE, EQ, or SYNC", "to fit the tempo. Then APPLY.",
     {"OTT, DUST, DRIVE, an EQ slot, or",
      "SYNC, which fits the length to",
      "the tempo. APPLY writes it into",
      "the sample."},
     {"A+D-PAD picks the effect",
      "START plays it with the effect"}},
    /* SE_FX_VALUE */
    {"VALUE: what the effect uses", "An amount, or an EQ slot, or", "which way SYNC fits it.",
     {"How much of the effect. For EQ",
      "it is the EQ slot. For SYNC,",
      "RPITCH changes speed and pitch",
      "together, TSTRETCH only the",
      "length."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A on an EQ slot opens the editor",
      "A+B on an EQ slot clears it",
      "A on TSTRETCH opens its settings",
      "A+B on TSTRETCH resets them"}},
    /* SE_FX_APPLY */
    {"APPLY: write it into the sample", "The effect becomes part of", "the sound. UNDO takes it back.",
     {"Writes the effect into the",
      "sample. SYNC fits the sample to",
      "DURATION at the song TEMPO, or",
      "at the NEW BPM set for TSTRETCH."},
     {"A applies, UNDO takes it back"}},
    /* SE_NAME */
    {"NAME: what to call it", "A opens the keyboard. SAVE", "uses this for the file name.",
     {"SAVE uses it for the file name."},
     {"A opens the keyboard",
      "START on the keyboard applies",
      "SELECT on the keyboard cancels"}},
    /* SE_LOAD */
    {"LOAD: open another sample", "It replaces this sample in the", "same instrument.",
     {"Opens the file browser. The new",
      "sample replaces this one in the",
      "same instrument."},
     {"A opens the file browser",
      "A there loads it, B goes back"}},
    /* SE_SAVE */
    {"SAVE: write a new file", "Into the Samples folder. It", "never replaces an old file.",
     {"Saves a new WAV in the Samples",
      "folder, named after NAME. If",
      "that name is taken, it offers",
      "another - it never replaces a",
      "file."},
     {"A saves, or asks for a new name"}},
    /* SE_OVERWRITE */
    {"OVERWRITE: replace the file", "Writes over the file this", "sample came from.",
     {"Writes over the file this sample",
      "came from, straight away. The",
      "old file is gone for good."},
     {"A overwrites, with no undo"}},
    /* SE_CHOP */
    {"CHOP: every slice as a file", "Writes them into a Chops", "folder named after the sample.",
     {"Saves each slice as its own WAV,",
      "in a folder named after this",
      "sample inside Samples/Chops."},
     {"A chops"}},
    /* SET_METRONOME */
    {"METRONOME: a click on the beat", "One click every four steps,", "while playing. Never exported.",
     {"One click every four steps while",
      "the song plays. You hear it, but",
      "it is never in an export. VOL",
      "sets how loud."},
     {"A+D-PAD turns it on or off"}},
    /* SET_METRONOME_VOL */
    {"VOL: how loud the click is", "00 is silent, FF is full.", "",
     {"From 00 silent to FF full."},
     {"A+←/→ changes by 1, A+↑/↓ by 16"}},
    /* PROJECT_TAP */
    {"TAP: set the tempo by feel", "Press A in time, at least twice.", "A pause starts a new count.",
     {"Tap A in time with the beat you",
      "want and TEMPO follows. Each",
      "tap makes it more exact. Wait",
      "three seconds to start over."},
     {"A taps, at least twice"}},
    /* FX_DELAY_TYPE */
    {"PRESET: a ready-made delay", "A+D-PAD steps through them.", "SAVE and LOAD are beside it.",
     {"Seven to start from, from a",
      "clean echo to dub and endless",
      "repeats. A preset sets every",
      "cell below except INP EQ. A",
      "star means you have changed it",
      "since."},
     {"A+D-PAD picks a built-in preset",
      "A+B goes back to DIGITAL"}},
    /* FX_DELAY_TONE */
    {"TONE: how bright repeats are", "Lower makes each echo darker", "than the one before it.",
     {"Lower makes each echo darker",
      "than the one before it. FF",
      "leaves them untouched."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to FF"}},
    /* FX_DELAY_WOBBLE */
    {"WOBL: tape speed wobble", "Makes the echoes drift in", "pitch. 00 holds them steady.",
     {"How far the echoes drift in",
      "pitch, like a tape running",
      "unevenly. 00 holds them steady."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 00"}},
    /* FX_DELAY_PONG */
    {"PONG: echoes bounce", "Repeats alternate left and", "right, ignoring the pan.",
     {"ON makes the echoes alternate",
      "left and right. The delay",
      "places them, so the pan of the",
      "instrument no longer matters.",
      "OFF keeps an echo on the side",
      "its instrument sits on."},
     {"A+D-PAD turns it on or off"}},
    /* FX_REVERB_TYPE */
    {"PRESET: a ready-made reverb", "A+D-PAD steps through them.", "SAVE and LOAD are beside it.",
     {"Seven to start from, from small",
      "rooms to long halls and plates.",
      "A preset sets every cell below",
      "except INP EQ. A star means you",
      "have changed it since."},
     {"A+D-PAD picks a built-in preset",
      "A+B goes back to DEFAULT"}},
    /* FX_REVERB_PRE */
    {"PRE: a gap before the tail", "The reverb starts late, so the", "sound stays in front of it.",
     {"A wait of up to 150 ms before",
      "the reverb starts, so the sound",
      "stays clear in front of it. 00",
      "starts it at once."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 00"}},
    /* FX_REVERB_WIDE */
    {"WIDE: how far it spreads", "00 is mono, 80 is normal,", "FF pushes it to the sides.",
     {"00 is mono, 80 leaves it as the",
      "reverb makes it, FF pushes it",
      "out to the sides. The level",
      "stays the same."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 80"}},
    /* FX_REVERB_MOD */
    {"MOD: movement in the tail", "The tail drifts in pitch. 00 is", "still. PLATE and FOIL ignore it.",
     {"Makes the tail drift in pitch.",
      "Higher drifts further. 00 holds",
      "it still, which sounds bright",
      "and metallic. PLATE and FOIL",
      "ignore it."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 10"}},
    /* FX_REVERB_DECAY */
    {"DCAY: how long it rings", "Higher rings longer. On OLD,", "FF never stops.",
     {"Higher rings longer, and the",
      "level stays where it is. On OLD,",
      "FF is a tail that never stops."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B sets it back to 60"}},
    /* SE_BIT */
    {"BIT: bits per sample", "The file depth, or lower for", "grit. SAVE writes at this depth.",
     {"It starts at the bit depth of",
      "the file and offers that depth",
      "and lower. Lower depths make the",
      "sound rougher. SAVE, OVERWRITE",
      "and CHOP write the file at the",
      "depth shown."},
     {"A+D-PAD picks the depth"}},
    /* FX_REVERB_ALGO */
    {"ALGO: which reverb", "OLD, or a Dragonfly HALL, ROOM,", "PLATE or FOIL.",
     {"OLD is the original reverb of",
      "the app. HALL is a large space,",
      "ROOM a small one. PLATE and FOIL",
      "are metal plates with no room,",
      "so they ignore SIZE and MOD."},
     {"A+D-PAD picks a reverb"}},
    /* SET_HELP */
    {"HELP: what SELECT shows", "SHORT uses the top strip, FULL", "a big page. OFF shows nothing.",
     {"SHORT puts three lines in the",
      "strip at the top. FULL opens a",
      "page like this one, and any",
      "button closes it. OFF shows",
      "nothing."},
     {"A+D-PAD picks OFF, SHORT, FULL"}},
    /* GROOVE_NAME */
    {"NAME: which groove this is", "A+D-PAD steps through the", "built-in grooves.",
     {"Steps through the built-in",
      "grooves. Choosing one replaces",
      "all the ticks beside it, so save",
      "a groove you built yourself",
      "first. A star means the ticks no",
      "longer match the name."},
     {"A+D-PAD picks a built-in groove",
      "A+B goes back to STRAIGHT"}},
    /* GROOVE_SAVE */
    {"SAVE: keep this groove", "Writes a file you can load", "into any other project.",
     {"Saves this groove under a name",
      "in the Grooves folder. LOAD can",
      "then open it in any song."},
     {"A opens the keyboard to name it",
      "START on the keyboard saves",
      "SELECT on the keyboard cancels"}},
    /* GROOVE_LOAD */
    {"LOAD: open a groove", "Replaces the ticks beside it.", "",
     {"Opens the file browser on your",
      "saved grooves. Loading one",
      "replaces all the ticks of this",
      "groove, and every track using it",
      "hears the change."},
     {"A loads the groove you are on",
      "B goes back without loading"}},
    /* GROOVE_QNT */
    {"QNT: keep the bar in place", "When on, a longer step makes", "its partner shorter.",
     {"OFF changes one step on its own.",
      "When set to a fraction, changing",
      "a step moves its partner the",
      "other way by the same amount, so",
      "the bar line does not shift."},
     {"A+D-PAD picks OFF or a fraction",
      "B+↑/↓ sets it from any cell",
      "A+D-PAD on a tick moves the pair"}},
    /* GROOVE_SWG */
    {"SWG: how much swing", "Shown, not set. 50 is even,", "66.7 is a triplet feel.",
     {"How long the first half of a",
      "group of steps lasts, as a share",
      "of the whole group - for the",
      "group under the TIC cursor. QNT",
      "sets how many steps a group has."},
     {"Nothing - it only shows a value"}},

    /* MIDI_VELOCITY */
    {"VELOCITY: key strength", "ON: a harder key plays", "louder. OFF: every key is full.",
     {"ON follows how hard each key is",
      "pressed. OFF plays every key at",
      "full strength, whatever the",
      "keyboard sends."},
     {"A+D-PAD turns it on or off"}},
    /* SET_AUDIO_OUT */
    {"AUDIO OUT: where sound goes", "SYSTEM, or an ASIO driver for", "less delay on an interface.",
     {"SYSTEM plays through Windows.",
      "An ASIO driver plays straight",
      "to your audio interface, with",
      "less delay. Its buffer size is",
      "set in the driver panel."},
     {"A+D-PAD picks the output"}},
    /* GROOVE_PPQ */
    {"PPQ: how fine the ticks are", "How many ticks make a quarter", "note: 24, 48 or 96.",
     {"How finely the TIC column",
      "counts. At 48 an even step is",
      "0C, at 24 it is 06, at 96 it is",
      "18. A change rewrites the ticks",
      "so the groove sounds the same -",
      "going down rounds them. Only",
      "grooves use it: FX commands",
      "always count 12 ticks to a step."},
     {"A+←/→ picks 24, 48 or 96",
      "A+B goes back to 48"}},
    /* FX_REVERB_SAVE */
    {"SAVE: keep this reverb", "Writes a file you can load", "into any other project.",
     {"Saves this reverb under a name",
      "in the Reverbs folder, so LOAD",
      "can open it in any song. The",
      "file holds every cell here",
      "except INP EQ."},
     {"A opens the keyboard to name it",
      "START on the keyboard saves",
      "SELECT on the keyboard cancels"}},
    /* FX_REVERB_LOAD */
    {"LOAD: open a reverb", "Replaces the reverb cells", "below it.",
     {"Opens the file browser on your",
      "saved reverbs. Loading one",
      "replaces every reverb cell",
      "except INP EQ."},
     {"A opens the file browser",
      "A there loads it, B goes back"}},
    /* FX_DELAY_SAVE */
    {"SAVE: keep this delay", "Writes a file you can load", "into any other project.",
     {"Saves this delay under a name in",
      "the Delays folder, so LOAD can",
      "open it in any song. The file",
      "holds every cell here except INP",
      "EQ."},
     {"A opens the keyboard to name it",
      "START on the keyboard saves",
      "SELECT on the keyboard cancels"}},
    /* FX_DELAY_LOAD */
    {"LOAD: open a delay", "Replaces the delay cells", "below it.",
     {"Opens the file browser on your",
      "saved delays. Loading one",
      "replaces every delay cell except",
      "INP EQ."},
     {"A opens the file browser",
      "A there loads it, B goes back"}},
    /* SE_STRETCH_BPM */
    {"NEW BPM: the tempo to fit to", "Starts as the song TEMPO.", "A+B puts it back there.",
     {"TSTRETCH fits the sample to",
      "DURATION at this tempo. It",
      "starts as the song TEMPO."},
     {"A+←/→ changes by 1, A+↑/↓ by 10",
      "A+B goes back to the song TEMPO"}},
    /* SE_STRETCH_PITCH */
    {"PITCH: stretch it at a new pitch", "In semitones. The length", "still fits DURATION.",
     {"Moves the pitch up or down, from",
      "-24 to +24 semitones, while the",
      "length still fits DURATION at",
      "the NEW BPM above. It is the",
      "same PITCH as on the editor."},
     {"A+←/→ changes by a semitone",
      "A+↑/↓ changes by 16",
      "A+B goes back to +0"}},
    /* SE_STRETCH_GRAIN */
    {"GRAIN: the size of each piece", "In milliseconds. Short is", "gritty, long is smooth.",
     {"The stretch cuts the sample into",
      "pieces this long, in",
      "milliseconds, and blends each",
      "into the next. Short is gritty,",
      "long is smoother but can echo."},
     {"A+←/→ changes by 1, A+↑/↓ by 16",
      "A+B goes back to 28"}},
    /* SE_STRETCH_PREVIEW */
    {"PREVIEW: hear it first", "Plays the stretched sample.", "Nothing is changed yet.",
     {"Plays the sample as these",
      "settings would stretch it.",
      "Nothing is changed yet."},
     {"A or START plays it",
      "A or START again stops it"}},
    /* SE_STRETCH_APPLY */
    {"APPLY: stretch it for good", "Stretches the sample and", "closes the box.",
     {"Stretches the sample and closes",
      "the box. APPLY on the EFFECT row",
      "does the same with these",
      "settings."},
     {"A applies, UNDO takes it back"}},
    /* INST_MIDI_BEND */
    {"BEND: pitch bend range", "Semitones of a full bend, 01", "to 48. Set the device the same.",
     {"PSL, PBN, PVB, PVX, FIN and MPB",
      "reach the device as pitch bend,",
      "scaled to this range. The app",
      "tells the device the range",
      "before the first bend. If the",
      "device ignores that, set the",
      "same number on it."},
     {"A+←/→ changes by 1, A+↑/↓ by 12",
      "A+B sets it back to 12"}},
    /* MIDI_SYNC_IN */
    {"SYNC IN: follow a clock", "Another device sets the tempo", "and when the song plays.",
     {"TEMPO: the song plays at the",
      "tempo of the other device, on",
      "its beat. You still start and",
      "stop it here. FULL: the other",
      "device also starts and stops the",
      "song, from the place it starts",
      "at. The TEMPO of the song is not",
      "changed."},
     {"A+D-PAD picks OFF, TEMPO or FULL"}},
};

// ─── Rules 2 and 3, as compile errors — at runtime they fail silently ────────────────────────────

namespace detail {

// Drawable by `font5x5.h`; `'` and `;` draw blank.
constexpr bool help_char_ok(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' ||
           c == ':' || c == '=' || c == '-' || c == '(' || c == ')' || c == '.' || c == '/' ||
           c == ',' || c == '+';
}

// One of the four arrows (U+2190..U+2193) at `s` — the only non-ASCII the font draws. Read a byte at
// a time, so a truncated sequence stops at the terminator.
constexpr bool help_arrow_at(const char* s) {
    if (static_cast<unsigned char>(s[0]) != 0xE2) return false;
    if (static_cast<unsigned char>(s[1]) != 0x86) return false;
    const auto tail = static_cast<unsigned char>(s[2]);
    return tail >= 0x90 && tail <= 0x93;
}

// The budget is code points, not bytes: an arrow is one column.
constexpr bool help_line_ok(const char* s) {
    if (s == nullptr) return true;   // an unwritten body line
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
        for (const char* b : e.body)
            if (!help_line_ok(b)) return false;
        for (const char* k : e.keys)
            if (!help_line_ok(k)) return false;
    }
    return true;
}

}  // namespace detail

// The title: `line1` up to its first colon, or all of it ("VOL: how loud…" → VOL).
// Counted in bytes, safe because a title never holds an arrow (`help_titles_ok`).
constexpr int help_title_length(const char* line1) {
    int n = 0;
    while (line1[n] != '\0' && line1[n] != ':') ++n;
    return n;
}

namespace detail {

constexpr bool help_titles_ok() {
    for (const HelpEntry& e : HELP_ENTRIES) {
        const int n = help_title_length(e.line1);
        if (n > HELP_TITLE_MAX_CHARS) return false;
        for (int i = 0; i < n; ++i)
            if (static_cast<unsigned char>(e.line1[i]) >= 0x80) return false;
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
static_assert(detail::help_titles_ok(),
              "a help title (line1 up to its colon) is over HELP_TITLE_MAX_CHARS, or holds an arrow");

// Out of range gives the empty entry.
inline const HelpEntry& help_entry(HelpTopic topic) {
    const size_t i = static_cast<size_t>(topic);
    if (i >= static_cast<size_t>(HelpTopic::COUNT)) return HELP_ENTRIES[0];
    return HELP_ENTRIES[i];
}

// ─── The lookup ──────────────────────────────────────────────────────────────────────────────────

// The screen-level fallback.
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
        case ScreenType::MIDI_MAP:      return HelpTopic::SCREEN_MIDI_MAP;
    }
    return HelpTopic::NONE;
}

namespace detail {

// PHRASE columns 1..9 (column 0, the step number, is unreachable).
inline HelpTopic phrase_cell_topic(int column) {
    switch (column) {
        case 1:  return HelpTopic::PHRASE_NOTE;
        case 2:  return HelpTopic::PHRASE_VOLUME;
        case 3:  return HelpTopic::PHRASE_INSTRUMENT;
        case 4: case 6: case 8: return HelpTopic::PHRASE_FX_TYPE;
        case 5: case 7: case 9: return HelpTopic::PHRASE_FX_VALUE;
        default: return HelpTopic::NONE;
    }
}

// TABLE columns 1..8 — no instrument cell.
inline HelpTopic table_cell_topic(int column) {
    switch (column) {
        case 1:  return HelpTopic::TABLE_TRANSPOSE;
        case 2:  return HelpTopic::TABLE_VOLUME;
        case 3: case 5: case 7: return HelpTopic::TABLE_FX_TYPE;
        case 4: case 6: case 8: return HelpTopic::TABLE_FX_VALUE;
        default: return HelpTopic::NONE;
    }
}

// INSTRUMENT, every type: the cell the type's table puts at (row, column).
inline HelpTopic instrument_topic(songcore::InstrumentType type, int row, int column) {
    using Cell = InstrumentCell;
    switch (instrument_cell_at(type, row, column)) {
        case Cell::NONE:         return HelpTopic::NONE;
        case Cell::TYPE:         return HelpTopic::INST_TYPE;
        case Cell::SOURCE_LOAD:  return HelpTopic::INST_SOURCE_LOAD;
        case Cell::SOURCE_EDIT:  return HelpTopic::INST_SOURCE_EDIT;
        case Cell::NAME:         return HelpTopic::INST_NAME;
        case Cell::PRESET_SAVE:  return HelpTopic::INST_PRESET_SAVE;
        case Cell::PRESET_LOAD:  return HelpTopic::INST_PRESET_LOAD;
        case Cell::VOL:          return HelpTopic::INST_VOLUME;
        case Cell::PAN:          return HelpTopic::INST_PAN;
        case Cell::TSP:          return HelpTopic::INST_TRANSPOSE;
        case Cell::TIC:          return HelpTopic::INST_TIC;
        case Cell::ROOT:         return HelpTopic::INST_ROOT;
        case Cell::DETUNE:       return HelpTopic::INST_DETUNE;
        case Cell::DRIVE:        return HelpTopic::INST_DRIVE;
        case Cell::FILTER:       return HelpTopic::INST_FILTER;
        case Cell::CRUSH:        return HelpTopic::INST_CRUSH;
        case Cell::FREQ:         return HelpTopic::INST_FILTER_FREQ;
        case Cell::DOWNSAMPLE:   return HelpTopic::INST_DOWNSAMPLE;
        case Cell::RES:          return HelpTopic::INST_FILTER_RES;
        case Cell::REV:          return HelpTopic::INST_REVERB_SEND;
        case Cell::DEL:          return HelpTopic::INST_DELAY_SEND;
        case Cell::EQ:           return HelpTopic::INST_EQ;
        case Cell::PATCH:        return HelpTopic::INST_PATCH;
        case Cell::SLICE:        return HelpTopic::INST_SLICE;
        case Cell::LOOP:         return HelpTopic::INST_LOOP_MODE;
        case Cell::START:        return HelpTopic::INST_SAMPLE_START;
        case Cell::LOOP_START:   return HelpTopic::INST_LOOP_START;
        case Cell::END:          return HelpTopic::INST_SAMPLE_END;
        case Cell::LOOP_END:     return HelpTopic::INST_LOOP_END;
        case Cell::REVERSE:      return HelpTopic::INST_REVERSE;
        case Cell::MIDI_CHANNEL: return HelpTopic::INST_MIDI_CHANNEL;
        case Cell::MIDI_BANK:    return HelpTopic::INST_MIDI_BANK;
        case Cell::MIDI_PROGRAM: return HelpTopic::INST_MIDI_PROGRAM;
        case Cell::MIDI_LENGTH:  return HelpTopic::INST_MIDI_LENGTH;
        case Cell::CC_NUMBER:    return HelpTopic::INST_MIDI_CC_NUMBER;
        case Cell::CC_VALUE:     return HelpTopic::INST_MIDI_CC_VALUE;
        case Cell::MIDI_BEND:    return HelpTopic::INST_MIDI_BEND;
    }
    return HelpTopic::NONE;
}

// PROJECT: two values, a name, then rows of buttons, read by column in the draw order
// (`project_editor.cpp`).
inline HelpTopic project_cell_topic(int row, int column) {
    if (row < 0 || row >= PROJECT_ROW_COUNT) return HelpTopic::NONE;
    switch (static_cast<ProjectRow>(row)) {
        case ProjectRow::TEMPO:
            return column == 2 ? HelpTopic::PROJECT_TAP : HelpTopic::PROJECT_TEMPO;
        case ProjectRow::TRANSPOSE: return HelpTopic::PROJECT_TRANSPOSE;
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

// GROOVE: the tick column, and the panel beside it.
inline HelpTopic groove_cell_topic(int column, int panel_row, int panel_column) {
    if (column != GROOVE_COL_PANEL) return HelpTopic::GROOVE_TIC;
    switch (panel_row) {
        case GROOVE_PANEL_NAME: return HelpTopic::GROOVE_NAME;
        case GROOVE_PANEL_FILE:
            return panel_column == 0 ? HelpTopic::GROOVE_SAVE : HelpTopic::GROOVE_LOAD;
        case GROOVE_PANEL_QNT:  return HelpTopic::GROOVE_QNT;
        case GROOVE_PANEL_SWG:  return HelpTopic::GROOVE_SWG;
        case GROOVE_PANEL_PPQ:  return HelpTopic::GROOVE_PPQ;
        default:                return HelpTopic::NONE;
    }
}

// SCALE: the NAME row has three cells; the rows below are degrees.
inline HelpTopic scale_cell_topic(int row, int column) {
    if (row == SCALE_NAME_ROW) {
        if (column == SCALE_NAME_COL_SAVE) return HelpTopic::SCALE_SAVE;
        if (column == SCALE_NAME_COL_LOAD) return HelpTopic::SCALE_LOAD;
        return HelpTopic::SCALE_NAME;
    }
    if (row == SCALE_KEY_ROW) return HelpTopic::SCALE_KEY;
    return (scale_row_degree(row) >= 0) ? HelpTopic::SCALE_DEGREE : HelpTopic::NONE;
}

// MODS: a row's meaning follows the slot's type (row 4 is HOLD, DEC or TRIG), asked in the module's
// order (`modulation.cpp`): LFO, then the AHD-shaped pair, then ADSR.
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

// INST.POOL: the value columns are the instrument's own fields and take INSTRUMENT's entries; only the
// name column has text of its own.
inline HelpTopic pool_cell_topic(int column) {
    switch (column) {
        case 1:  return HelpTopic::INST_VOLUME;
        case 2:  return HelpTopic::INST_REVERB_SEND;
        case 3:  return HelpTopic::INST_DELAY_SEND;
        case 4:  return HelpTopic::INST_EQ;
        default: return HelpTopic::POOL_SLOT;
    }
}

// MIXER: rows 2 and 3 exist only on the master strip (column 8).
inline HelpTopic mixer_cell_topic(int master_row, int column) {
    switch (mixer_cell_at(master_row, column)) {
        case MixerCell::TRACK_VOL:  return HelpTopic::MIXER_TRACK_VOL;
        case MixerCell::MASTER_VOL: return HelpTopic::MIXER_MASTER_VOL;
        case MixerCell::REV_WET:    return HelpTopic::MIXER_REVERB_RETURN;
        case MixerCell::DLY_WET:    return HelpTopic::MIXER_DELAY_RETURN;
        case MixerCell::MASTER_EQ:  return HelpTopic::MIXER_MASTER_EQ;
        case MixerCell::MASTER_FX:  return HelpTopic::MIXER_MASTER_FX;
        case MixerCell::LIMITER:    return HelpTopic::MIXER_LIMITER;
        case MixerCell::NONE:       break;
    }
    return HelpTopic::NONE;
}

// EFFECTS: the editable rows, by the module's own names.
inline HelpTopic effects_cell_topic(int row) {
    switch (row) {
        case EffectModule::ROW_MASTER_TYPE: return HelpTopic::FX_MASTER_TYPE;
        case EffectModule::ROW_REV_DECAY:   return HelpTopic::FX_REVERB_DECAY;
        case EffectModule::ROW_REV_SIZE:    return HelpTopic::FX_REVERB_SIZE;
        case EffectModule::ROW_REV_DAMP:    return HelpTopic::FX_REVERB_DAMP;
        case EffectModule::ROW_REV_EQ:      return HelpTopic::FX_REVERB_EQ;
        case EffectModule::ROW_DLY_TIME:    return HelpTopic::FX_DELAY_TIME;
        case EffectModule::ROW_DLY_FDBK:    return HelpTopic::FX_DELAY_FEEDBACK;
        case EffectModule::ROW_DLY_REV:     return HelpTopic::FX_DELAY_TO_REVERB;
        case EffectModule::ROW_DLY_EQ:      return HelpTopic::FX_DELAY_EQ;
        case EffectModule::ROW_DLY_TYPE:    return HelpTopic::FX_DELAY_TYPE;
        case EffectModule::ROW_DLY_TONE:    return HelpTopic::FX_DELAY_TONE;
        case EffectModule::ROW_DLY_WOBBLE:  return HelpTopic::FX_DELAY_WOBBLE;
        case EffectModule::ROW_DLY_PONG:    return HelpTopic::FX_DELAY_PONG;
        case EffectModule::ROW_REV_TYPE:    return HelpTopic::FX_REVERB_TYPE;
        case EffectModule::ROW_REV_PRE:     return HelpTopic::FX_REVERB_PRE;
        case EffectModule::ROW_REV_WIDE:    return HelpTopic::FX_REVERB_WIDE;
        case EffectModule::ROW_REV_MOD:     return HelpTopic::FX_REVERB_MOD;
        case EffectModule::ROW_REV_ALGO:    return HelpTopic::FX_REVERB_ALGO;
        case EffectModule::ROW_REV_SAVE:    return HelpTopic::FX_REVERB_SAVE;
        case EffectModule::ROW_REV_LOAD:    return HelpTopic::FX_REVERB_LOAD;
        case EffectModule::ROW_DLY_SAVE:    return HelpTopic::FX_DELAY_SAVE;
        case EffectModule::ROW_DLY_LOAD:    return HelpTopic::FX_DELAY_LOAD;
        default:                            return HelpTopic::NONE;
    }
}

// SETTINGS: column 2 is the row's second cell where it has one (the skin, STR, VOL, POW, TEMPLATE's
// second button).
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
        case SettingsRow::AUDIO_OUT:  return HelpTopic::SET_AUDIO_OUT;
        case SettingsRow::METRONOME:
            return second ? HelpTopic::SET_METRONOME_VOL : HelpTopic::SET_METRONOME;
        case SettingsRow::KB_INSERT:  return HelpTopic::SET_KB_INSERT;
        case SettingsRow::CURSOR:     return HelpTopic::SET_CURSOR;
        case SettingsRow::NAV:        return HelpTopic::SET_NAV;
        case SettingsRow::FOLDER:     return HelpTopic::SET_FOLDER;
        case SettingsRow::NOTE_PREV:  return HelpTopic::SET_NOTE_PREVIEW;
        case SettingsRow::VISUALIZER: return HelpTopic::SET_VISUALIZER;
        case SettingsRow::HELP:       return HelpTopic::SET_HELP;
        case SettingsRow::THEME:      return HelpTopic::SET_THEME;
        case SettingsRow::TEMPLATE:
            return second ? HelpTopic::SET_TEMPLATE_CLEAR : HelpTopic::SET_TEMPLATE_SAVE;
        case SettingsRow::RESUME:     return HelpTopic::SET_RESUME;
        case SettingsRow::TRACE:      return HelpTopic::SET_TRACE;
    }
    return HelpTopic::NONE;
}

// MIDI: one topic per row.
inline HelpTopic midi_cell_topic(int row) {
    if (row < 0 || row >= MIDI_ROW_COUNT) return HelpTopic::NONE;
    switch (static_cast<MidiRow>(row)) {
        case MidiRow::OUTPUT:   return HelpTopic::MIDI_OUTPUT;
        case MidiRow::INPUT:    return HelpTopic::MIDI_INPUT;
        case MidiRow::OFFSET:   return HelpTopic::MIDI_OFFSET;
        case MidiRow::SYNC:     return HelpTopic::MIDI_SYNC;
        case MidiRow::SYNC_IN:  return HelpTopic::MIDI_SYNC_IN;
        case MidiRow::CTL_CH:   return HelpTopic::MIDI_CTL_CH;
        case MidiRow::KEYS:     return HelpTopic::MIDI_KEYS;
        case MidiRow::PROG_CHG: return HelpTopic::MIDI_PROG_CHG;
        case MidiRow::VELOCITY: return HelpTopic::MIDI_VELOCITY;
        case MidiRow::MAPPING:  return HelpTopic::MIDI_MAPPING;
        case MidiRow::PANIC:    return HelpTopic::MIDI_PANIC;
        case MidiRow::TEST:     return HelpTopic::MIDI_TEST;
    }
    return HelpTopic::NONE;
}

// A mapping row's columns. The ADD row has none; `help_topic` asks about it first.
inline HelpTopic midi_map_cell_topic(int column) {
    switch (static_cast<MapCol>(column)) {
        case MapCol::CC:    return HelpTopic::MAP_CC;
        case MapCol::MIN:   return HelpTopic::MAP_MIN;
        case MapCol::MAX:   return HelpTopic::MAP_MAX;
        case MapCol::GROUP:
        case MapCol::PARAM: return HelpTopic::MAP_DEST;
        case MapCol::SCOPE: return HelpTopic::MAP_SCOPE;
    }
    return HelpTopic::NONE;
}

// The EQ editor's cursor is one int over 3×4: band = row / 4, parameter = row % 4.
inline HelpTopic eq_cell_topic(int cursor_row) {
    if (cursor_row < 0) return HelpTopic::NONE;
    switch (cursor_row % 4) {
        case 0:  return HelpTopic::EQ_TYPE;
        case 1:  return HelpTopic::EQ_FREQ;
        case 2:  return HelpTopic::EQ_GAIN;
        default: return HelpTopic::EQ_Q;
    }
}

// The theme editor's colour rows, in `theme_color_rows()` order (ui/theme.h). A row added there and
// not here shows the screen text; the lookup is bounded by this array.
inline constexpr HelpTopic THEME_COLOR_TOPICS[] = {
    HelpTopic::THEME_BACKGROUND, HelpTopic::THEME_ROW_4TH,     HelpTopic::THEME_ROW_CURSOR,
    HelpTopic::THEME_ROW_SELECT, HelpTopic::THEME_TXT_TITLE,   HelpTopic::THEME_TXT_PARAM,
    HelpTopic::THEME_TXT_VALUE,  HelpTopic::THEME_TXT_EMPTY,   HelpTopic::THEME_TXT_PLAY,
    HelpTopic::THEME_VIZ_BG,     HelpTopic::THEME_VIZ_LINE,    HelpTopic::THEME_VIZ_WAVE,
    HelpTopic::THEME_MTR_BG,     HelpTopic::THEME_MTR_LOW,     HelpTopic::THEME_MTR_MID,
    HelpTopic::THEME_MTR_HIGH,   HelpTopic::THEME_EQ_FILL,     HelpTopic::THEME_EQ_BORDER,
    HelpTopic::THEME_EQ_TXT,     HelpTopic::THEME_EQ_BG,
};

// The theme editor: THEME (name, SAVE, LOAD), RANDOMIZE (scheme, ROLL), then one row per colour.
inline HelpTopic theme_cell_topic(int row, int channel) {
    if (row == THEME_ROW_THEME) {
        if (channel == 1) return HelpTopic::THEME_SAVE;
        if (channel == 2) return HelpTopic::THEME_LOAD;
        return HelpTopic::THEME_NAME;
    }
    if (row == THEME_ROW_RANDOM) {
        return (channel == 1) ? HelpTopic::THEME_ROLL : HelpTopic::THEME_SCHEME;
    }
    const int index = theme_color_index(row);
    const int count = static_cast<int>(sizeof(THEME_COLOR_TOPICS) / sizeof(THEME_COLOR_TOPICS[0]));
    return (index >= 0 && index < count) ? THEME_COLOR_TOPICS[index] : HelpTopic::NONE;
}


// The sample editor. Row 10 column 1 is SENS or BY by slice method; row 19 column 3 (CHOP) exists only
// with slices.
// ⚠️ OPS_1/OPS_2 are indexed by column: keep them in `ops_row1()`/`ops_row2()` order (sample_editor.cpp).
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
            return (column == 2) ? HelpTopic::SE_BIT : HelpTopic::NONE;

        // Rows 3..8 are the selection; the column is which edge.
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

// The TSTRETCH box. DURATION is the editor's own cell, shown again.
inline HelpTopic stretch_cell_topic(StretchCell cell) {
    switch (cell) {
        case StretchCell::DURATION: return HelpTopic::SE_DURATION;
        case StretchCell::BPM:      return HelpTopic::SE_STRETCH_BPM;
        case StretchCell::PITCH:    return HelpTopic::SE_STRETCH_PITCH;
        case StretchCell::GRAIN:    return HelpTopic::SE_STRETCH_GRAIN;
        case StretchCell::PREVIEW:  return HelpTopic::SE_STRETCH_PREVIEW;
        case StretchCell::APPLY:    return HelpTopic::SE_STRETCH_APPLY;
    }
    return HelpTopic::SCREEN_SAMPLE_EDITOR;
}

}  // namespace detail

// What the cursor is on. Never NONE: a cell with no entry falls back to its screen.
// The arms read the same cursor fields each module's `cursor_context()` does.
inline HelpTopic help_topic(const AppState& s) {
    // ⚠️ The overlays first: neither changes `currentScreen`, and the screen under them is not on the
    // canvas. The EQ editor first of those — it can be up over the sample editor.
    if (s.eq.isOpen) {
        const HelpTopic eq = detail::eq_cell_topic(s.eq.cursorRow);
        return (eq == HelpTopic::NONE) ? HelpTopic::SCREEN_EQ : eq;
    }
    if (s.themeEditor.isOpen) {
        const HelpTopic th =
            detail::theme_cell_topic(s.themeEditor.cursorRow, s.themeEditor.cursorChannel);
        return (th == HelpTopic::NONE) ? HelpTopic::SCREEN_THEME : th;
    }
    // The send editor's cells are EFFECTS' own. Asked after the EQ editor, which opens over it.
    if (send_editor_up(s)) {
        const HelpTopic fx = detail::effects_cell_topic(s.sendEditor.cursorRow);
        return (fx == HelpTopic::NONE) ? HelpTopic::SCREEN_EFFECTS : fx;
    }

    // The TSTRETCH box covers the sample editor's own cells.
    if (stretch_editor_up(s)) return detail::stretch_cell_topic(s.sampleEditor.stretchCursor);

    HelpTopic cell = HelpTopic::NONE;

    switch (s.currentScreen) {
        case ScreenType::SONG:
            // Every reachable column is a track cell.
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
            // ⚠️ Guarded: the tools build an AppState with no project.
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
            cell = detail::groove_cell_topic(s.grooveCursorColumn, s.groovePanelRow,
                                             s.groovePanelColumn);
            break;
        case ScreenType::SCALE:
            cell = detail::scale_cell_topic(s.scaleCursorRow, s.scaleCursorColumn);
            break;
        case ScreenType::MODS:
            // Guarded like INSTRUMENT.
            if (s.project != nullptr) {
                const songcore::Instrument& ins =
                    s.project->instruments[static_cast<size_t>(s.currentInstrument)];
                // The cursor is (pair, side, row); the slot is which half of which pair.
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

        // The ADD row, one past the end of the list, is a button with its own topic.
        case ScreenType::MIDI_MAP:
            cell = (s.project && s.midiMapCursorRow >=
                                     static_cast<int>(s.project->midiMappings.size()))
                       ? HelpTopic::MAP_ADD
                       : detail::midi_map_cell_topic(s.midiMapCursorColumn);
            break;
        default:
            break;
    }

    return (cell == HelpTopic::NONE) ? help_screen_topic(s.currentScreen) : cell;
}

}  // namespace pt::ui
