#pragma once

// Every help topic, and the cursor → topic lookup (`help_topic.cpp`); the text is `help_text.h`.
// A cell table names its cell's topic here, so the lookup asks the table instead of restating it.

namespace pt::ui {

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
    SE_SNAP,   // no cell; the slot stays because the value indexes HELP_ENTRIES
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
    SET_HELP,   // no cell (the HELP row is retired); the slot stays because the value indexes HELP_ENTRIES
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
    PROJECT_COLLECT,
    SET_ONE_HAND,
    SET_ONE_HAND_HOLD,
    INST_MACRO_MODEL,
    INST_MACRO_TIMBRE,
    INST_MACRO_COLOR,
    MOD_POLARITY,
    MOD_CURVE,
    MOD_TRK_AMOUNT,
    SET_FONT,
    SET_FONT_CASE,

    COUNT
};

struct AppState;

// What the cursor is on, overlays first. Never NONE: a cell with no entry falls back to its screen.
HelpTopic help_topic(const AppState& s);

// The screen it is on — the in-place editors count as screens. Its entry lists the screen's gestures.
HelpTopic help_screen(const AppState& s);

}  // namespace pt::ui
