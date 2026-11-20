#include "ui/layout.h"

#include "ui/guide.h"
#include "ui/value_hint.h"

#include <algorithm>
#include <string>

#include "songcore/scales.h"
#include "ui/buttons.h"
#include "ui/clipboard.h"
#include "ui/fx_columns.h"
#include "ui/helpers.h"
#include "ui/modules/fx_helper_overlay.h"
#include "ui/modules/instrument_picker_overlay.h"
#include "ui/modules/map_picker_overlay.h"
#include "ui/modules/render_dialog.h"
#include "ui/overlays.h"
#include "ui/song_pointer.h"
#include "ui/ui_fonts.h"

namespace pt::ui {

namespace {

constexpr int STATUS_MAX_CHARS = 34;   // one line
}  // namespace

void TrackerLayout::draw(Canvas& c, const AppState& s) {
    c.set_font(ui_font_named(s.settings.fontName));
    c.set_case(static_cast<TextCase>(clamp(s.settings.textCase, 0, 2)));
    draw_frame(c, s);
    if (s.project) draw_button_locks(c, s);

    // These two are outside `draw_frame`, which the full-screen modules return from early.
    if (s.helpFull && s.project) helpOverlay_.draw(c, help_topic(s), s.theme);
    draw_loading_strip(c, s.loading, s.theme);
}

void TrackerLayout::draw_frame(Canvas& c, const AppState& s) {
    c.fill_rect(0, 0, DESIGN_W, DESIGN_H, s.theme.background);

    if (!s.project) return;

    if (full_screen_module(s)) {
        draw_full_screen(c, s);
    } else {
        draw_scope_strip(c, s);
        draw_editor(c, s);

        if (s.currentScreen != ScreenType::SETTINGS) draw_right_bar(c, s);

        // Over the visualizer. The guide carries both in its own lines.
        if (!guide_up(s)) {
            draw_status_line(c, s);
            draw_selection_clipboard(c, s);
        }
    }
    draw_overlays(c, s);
}

// ─── The file browser and sample editor: full screen, no furniture ───────────────────────────────
void TrackerLayout::draw_full_screen(Canvas& c, const AppState& s) {
    const Theme& t = s.theme;
    if (s.currentScreen == ScreenType::FILE_BROWSER) {
        fileBrowser_.draw(c, 0, 0, s.fileBrowser, t);
    } else {
        sampleEditor_.draw(c, 0, 0, s.sampleEditor, t);
        if (s.project) draw_stretch_editor(c, s.sampleEditor, s.project->tempo, t);
    }
}

// ─── The top panel: a visualizer, or the guide ───────────────────────────────────────────────────
void TrackerLayout::draw_scope_strip(Canvas& c, const AppState& s) {
    const Theme& t = s.theme;
    if (guide_up(s)) {
        helpPanel_.draw(c, SIDE_SPACER, SCREEN_SPACER, guide_lines(c, s), t);
        return;
    }

    OscilloscopeState os;
    os.waveform = s.waveform;
    os.theme    = t;

    const bool isOctaFull = (t.visualizerType == VisualizerType::OCTA_FULL);
    const bool isOcta     = (t.visualizerType == VisualizerType::OCTA);
    if (isOcta || isOctaFull) os.trackWaveforms = s.trackWaveforms;

    // OCTA_FULL shows all eight lanes; OCTA the tracks that have played, plus the preview lane when stopped.
    if (isOctaFull) {
        os.activeTrackMask = 0xFF;
    } else if (isOcta) {
        os.activeTrackMask = s.trackMask & 0xFF;
        if (!s.isPlaying && s.previewLaneActive) os.activeTrackMask |= (1 << PREVIEW_LANE);
    }

    if (t.visualizerType == VisualizerType::SPECTRUM ||
        t.visualizerType == VisualizerType::SPECTRUM_PEAKS) {
        os.spectrum = s.spectrum;
    }

    oscilloscope_.draw(c, SIDE_SPACER, SCREEN_SPACER, os);
}

// ─── What each screen's module is handed ─────────────────────────────────────────────────────────

namespace {

const songcore::Instrument& current_instrument(const AppState& s) {
    return s.project->instruments[static_cast<size_t>(s.currentInstrument)];
}

PhraseEditorState phrase_state(const AppState& s) {
    const songcore::Project& p = *s.project;
    PhraseEditorState ps{p.phrases[static_cast<size_t>(s.currentPhrase)]};
    ps.cursorRow      = s.phraseCursor.row;
    ps.cursorColumn   = s.phraseCursor.column;
    std::copy(std::begin(s.playheads), std::end(s.playheads), std::begin(ps.playheads));
    ps.selectionMode  = s.selection_mode();
    ps.isCellSelected = [&s](int row, int col) { return s.is_cell_selected(row, col); };
    ps.theme          = s.theme;
    ps.project        = &p;   // whether an AUS/AUF cell is live depends on every walk
    // Under NAV = SONG, the chain row and song cell the phrase is seen through; −1 under POOL.
    if (s.settings.navSongRelative) {
        ps.viaChain    = s.currentChain;
        ps.viaChainRow = s.chainCursor.row;
        ps.songRow     = s.songCursor.row;
        ps.songTrack   = pointer_track(s);
    }
    return ps;
}

ChainEditorState chain_state(const AppState& s) {
    ChainEditorState cs{s.project->chains[static_cast<size_t>(s.currentChain)]};
    cs.cursorRow      = s.chainCursor.row;
    cs.cursorColumn   = s.chainCursor.column;
    std::copy(std::begin(s.playheads), std::end(s.playheads), std::begin(cs.playheads));
    cs.selectionMode  = s.selection_mode();
    cs.isCellSelected = [&s](int row, int col) { return s.is_cell_selected(row, col); };
    cs.theme          = s.theme;
    if (s.settings.navSongRelative) {
        cs.songRow   = s.songCursor.row;
        cs.songTrack = pointer_track(s);
    }
    return cs;
}

SongEditorState song_state(const AppState& s) {
    SongEditorState ss{*s.project};
    ss.cursorRow      = s.songCursor.row;
    ss.cursorTrack    = s.songCursor.column;
    ss.scrollPosition = s.songScrollPosition;
    std::copy(std::begin(s.playheads), std::end(s.playheads), std::begin(ss.playheads));
    ss.liveMode       = s.liveMode;
    std::copy(std::begin(s.liveQueue), std::end(s.liveQueue), std::begin(ss.liveQueue));
    ss.blinkPhaseMs   = s.blinkPhaseMs;
    ss.selectionMode  = s.selection_mode();
    ss.isCellSelected = [&s](int row, int col) { return s.is_cell_selected(row, col); };
    ss.theme          = s.theme;
    return ss;
}

TableState table_state(const AppState& s) {
    TableState ts{s.project->tables[static_cast<size_t>(s.currentTable)]};
    ts.cursorRow    = s.tableCursor.row;
    ts.cursorColumn = s.tableCursor.column;
    for (int l = 0; l < TABLE_LANES; ++l) ts.playbackRows[l] = s.tablePlaybackRows[l];
    // The instrument's tic rate: one table can run at two speeds under two instruments.
    ts.ticRate        = current_instrument(s).tableTicRate;
    ts.selectionMode  = s.selection_mode();
    ts.isCellSelected = [&s](int row, int col) { return s.is_cell_selected(row, col); };
    ts.theme          = s.theme;
    return ts;
}

GrooveState groove_state(const AppState& s) {
    GrooveState gs{s.project->grooves[static_cast<size_t>(s.currentGroove)]};
    gs.cursorRow    = s.grooveCursor.row;
    gs.cursorColumn = s.grooveCursor.column;
    gs.panelRow     = s.groovePanel.row;
    gs.panelColumn  = s.groovePanel.column;
    gs.quantize     = s.grooveQuantize;
    gs.preset       = s.presetRows[static_cast<size_t>(PresetRow::GROOVE)];
    gs.theme        = s.theme;
    return gs;
}

ScaleState scale_state(const AppState& s) {
    ScaleState cs{s.project->scales[static_cast<size_t>(s.currentScale)]};
    cs.key          = s.project->scaleKey;
    cs.cursorRow    = s.scaleCursor.row;
    cs.cursorColumn = s.scaleCursor.column;
    cs.preset       = s.presetRows[static_cast<size_t>(PresetRow::SCALE)];
    // The pitch classes sounding now, from the voices (the sequencer runs ahead), all tracks in one mask.
    for (int i = 0; i < 8; ++i) {
        const songcore::Note n = s.trackNotes[i];
        if (n == songcore::Note::EMPTY()) continue;
        const int midi = songcore::note_to_midi(n);
        if (midi >= 0) cs.soundingMask |= 1u << songcore::scale_mod12(midi);
    }
    cs.theme = s.theme;
    return cs;
}

InstrumentEditorState instrument_state(const AppState& s) {
    InstrumentEditorState is{current_instrument(s)};
    is.cursorRow     = s.instrumentCursor.row;
    is.cursorColumn  = s.instrumentCursor.column;
    is.sfPresetName  = s.sfPresetName;
    is.sfPresetCount = s.sfPresetCount;
    is.sfPresetIndex = s.sfPresetIndex;
    is.theme         = s.theme;
    return is;
}

InstrumentPoolState pool_state(const AppState& s) {
    InstrumentPoolState ps{*s.project};
    ps.selectedInstrument = s.currentInstrument;   // its cursor ROW is the selected instrument itself
    ps.cursorColumn       = s.poolCursorColumn;
    ps.sampleRamBytes     = s.sampleRamBytes;
    ps.caps               = s.caps;
    ps.theme              = s.theme;
    return ps;
}

ModulationState mods_state(const AppState& s) {
    ModulationState ms{current_instrument(s)};
    ms.cursorRow  = s.modCursorRow;
    ms.cursorPair = s.modCursorPair;
    ms.cursorSide = s.modCursorSide;
    ms.theme      = s.theme;
    return ms;
}

MixerState mixer_state(const AppState& s) {
    MixerState xs{*s.project};
    xs.cursorColumn   = s.mixerCursorColumn;
    xs.mixerMasterRow = s.mixerMasterRow;
    xs.trackPeaks     = s.trackPeaks;
    xs.masterPeaks    = s.masterPeaks;
    xs.reverbPeaks    = &s.sendPeaks[0];
    xs.delayPeaks     = &s.sendPeaks[2];
    xs.peaksVersion   = s.peaksVersion;
    xs.theme          = s.theme;
    return xs;
}

EffectState effects_state(const AppState& s) {
    EffectState es{*s.project};
    es.cursorRow = s.effectsCursorRow;
    es.reverb    = s.presetRows[static_cast<size_t>(PresetRow::REVERB)];
    es.delay     = s.presetRows[static_cast<size_t>(PresetRow::DELAY)];
    es.theme     = s.theme;
    return es;
}

ProjectState project_state(const AppState& s) {
    ProjectState prs{*s.project};
    prs.cursorRow      = s.projectCursor.row;
    prs.cursorColumn   = s.projectCursor.column;
    prs.isRendering    = s.isRendering;
    prs.renderProgress = s.renderProgress;
    prs.isCollecting    = s.isCollecting;
    prs.collectProgress = s.collectProgress;
    prs.sampleRamBytes = s.sampleRamBytes;
    prs.freeRamBytes   = s.freeRamBytes;
    prs.caps           = s.caps;
    prs.theme          = s.theme;
    return prs;
}

SettingsState settings_state(const AppState& s) {
    SettingsState ss{s.settings};
    ss.cursorRow    = s.settingsCursor.row;
    ss.cursorColumn = s.settingsCursor.column;
    ss.layoutText   = s.layoutText;
    ss.skinText     = s.skinText;
    ss.overlayText  = s.overlayText;
    ss.audioOutText = s.audioOutText;
    ss.appVersion   = s.appVersion;
    ss.themeName    = s.theme.name;
    ss.caps         = s.caps;
    ss.theme        = s.theme;
    return ss;
}

MidiState midi_state(const AppState& s) {
    MidiState ms{*s.project, s.settings, s.midiOutDeviceNames, s.midiInDeviceNames};
    ms.lastCcChannel = s.midiInCcChannel;
    ms.clockBpm      = s.midiClockBpm;
    ms.cursorRow     = s.midiCursor.row;
    ms.cursorColumn  = s.midiCursor.column;
    ms.deviceIndex   = s.midiOutDeviceIndex;
    ms.inDeviceIndex = s.midiInDeviceIndex;
    ms.outOpenName   = s.midiOutOpenName;
    ms.inOpenName    = s.midiInOpenName;
    ms.autoOffsetMs  = s.midiAutoOffsetMs;
    ms.audioLoad     = s.audioLoad;
    ms.statusText    = s.midiStatusText;
    ms.caps          = s.caps;
    ms.theme         = s.theme;
    return ms;
}

MidiMapState midi_map_state(const AppState& s) {
    MidiMapState mm{*s.project};
    mm.cursorRow    = s.midiMapCursor.row;
    mm.cursorColumn = s.midiMapCursor.column;
    mm.theme        = s.theme;
    return mm;
}

EqState eq_state(const AppState& s) {
    EqState es{*s.project};
    es.slotIndex     = s.eq.slotIndex;
    es.cursorRow     = s.eq.cursorRow;
    es.caller        = s.eq.caller;
    es.spectrum      = s.eqSpectrum;
    es.spectrumCount = s.eqSpectrumCount;
    es.sampleRate    = static_cast<float>(s.eqSampleRate);
    es.theme         = s.theme;
    return es;
}

ThemeState theme_state(const AppState& s) {
    ThemeState ts;
    ts.theme  = s.theme;
    ts.editor = s.themeEditor;
    return ts;
}

}  // namespace

// ─── The editor, left of the right bar ───────────────────────────────────────────────────────────
// The EQ and theme editors replace the module but leave the furniture: the scope keeps moving under them.
void TrackerLayout::draw_editor(Canvas& c, const AppState& s) {
    Canvas::ClipScope clip(c, 0, 0, EDITOR_CLIP_RIGHT, DESIGN_H);
    const int         x = SIDE_SPACER;
    const int         y = EDITOR_Y;

    if (s.eq.isOpen) {
        eq_.draw(c, x, y, eq_state(s));
        return;
    }
    if (theme_editor_up(s)) {
        themeEditor_.draw(c, x, y, theme_state(s));
        return;
    }

    switch (s.currentScreen) {
        case ScreenType::PHRASE:     phraseEditor_.draw(c, x, y, phrase_state(s)); break;
        case ScreenType::CHAIN:      chainEditor_.draw(c, x, y, chain_state(s)); break;
        case ScreenType::SONG:       songEditor_.draw(c, x, y, song_state(s)); break;
        case ScreenType::TABLE:      tableModule_.draw(c, x, y, table_state(s)); break;
        case ScreenType::GROOVE:     grooveModule_.draw(c, x, y, groove_state(s)); break;
        case ScreenType::SCALE:      scaleModule_.draw(c, x, y, scale_state(s)); break;
        case ScreenType::INSTRUMENT: instrumentEditor_.draw(c, x, y, instrument_state(s)); break;
        case ScreenType::INST_POOL:  instrumentPool_.draw(c, x, y, pool_state(s)); break;
        case ScreenType::MODS:       modulation_.draw(c, x, y, mods_state(s)); break;
        case ScreenType::MIXER:      mixer_.draw(c, x, y, mixer_state(s)); break;
        case ScreenType::EFFECTS:    effects_.draw(c, x, y, effects_state(s)); break;
        case ScreenType::PROJECT:    project_.draw(c, x, y, project_state(s)); break;
        case ScreenType::SETTINGS:   settings_.draw(c, x, y, settings_state(s)); break;
        case ScreenType::MIDI:       midi_.draw(c, x, y, midi_state(s)); break;
        case ScreenType::MIDI_MAP:   midiMap_.draw(c, x, y, midi_map_state(s)); break;
        default:                     draw_placeholder(c, x, y, s.currentScreen, s.theme); break;
    }
}

// The boxes over everything, the bottom of the stack first, so the one that owns the buttons is on top.
// The rest are drawn in their own places: THEME and EQ in the editor's, STRETCH and ARE YOU SURE? by the
// sample editor, the full help and the load strip over the button locks (`draw`).
void TrackerLayout::draw_overlays(Canvas& c, const AppState& s) {
    const Theme& t = s.theme;
    for (size_t i = overlays_drawn_end(s); i-- > 0;) {
        if (!OVERLAYS[i].up(s)) continue;
        switch (OVERLAYS[i].id) {
            case Overlay::MAP_PICKER: draw_map_picker(c, s.mapPicker, t); break;
            case Overlay::INSTRUMENT_PICKER:
                if (s.project) draw_instrument_picker(c, s.instrumentPicker, s.project->instruments, t);
                break;
            case Overlay::SF_PRESET_PICKER:
                draw_sf_preset_picker(c, s.sfPresetPicker, t);
                break;
            case Overlay::MACRO_MODEL_PICKER: draw_macro_model_picker(c, s.macroModelPicker, t); break;
            case Overlay::FX_HELPER:  draw_fx_helper(c, s.fxHelper, t); break;
            case Overlay::SEND:
                draw_send_editor(c, s.sendEditor, effects_state(s));
                // Its cells have a guide of their own: again, over the strip the backdrop just dimmed.
                if (guide_up(s)) helpPanel_.draw(c, SIDE_SPACER, SCREEN_SPACER, guide_lines(c, s), t);
                break;
            case Overlay::QWERTY:        qwerty_.draw(c, s.qwerty, t); break;
            case Overlay::RENDER_DIALOG:
                draw_render_dialog(c, s.renderDialog, *s.project, s.isRendering, s.renderProgress, t);
                break;
            case Overlay::CONFIRM: draw_confirm_dialog(c, s.confirm, t); break;
            default: break;
        }
    }
}

bool TrackerLayout::has_falling_meters(const AppState& s) const {
    if (!s.project) return false;

    // The EQ editor's panel, asked what it last drew.
    if (s.eq.isOpen && !eq_.spectrum_at_rest()) return true;

    // ⚠️ Only when the mixer is drawn: under an overlay a falling marker never ages and the loop pins
    // at 60 Hz.
    if (s.currentScreen == ScreenType::MIXER && !overlay_in_editor(s) && !mixer_.peaks_at_rest())
        return true;

    // The SPECTRUM bars, likewise only while drawn.
    const VisualizerType vt = s.theme.visualizerType;
    if (vt != VisualizerType::SPECTRUM && vt != VisualizerType::SPECTRUM_PEAKS) return false;
    return !full_screen_module(s) && !oscilloscope_.bars_at_rest();
}

// ─── The status line, top-left of the scope strip ────────────────────────────────────────────────
// The strip's two text rows sit either side of its centre line, 3 px clear of it: a 15 px capital
// ends 3 px above the line on the first, starts 3 px below it on the second.
static constexpr int STRIP_MID   = SCREEN_SPACER + OscilloscopeModule::HEIGHT / 2;
static constexpr int STRIP_ROW_1 = STRIP_MID - 18;
static constexpr int STRIP_ROW_2 = STRIP_MID + 4;

void TrackerLayout::draw_status_line(Canvas& c, const AppState& s) const {
    if (draw_value_hint(c, s)) return;
    if (s.statusMessage.empty()) return;

    const std::string text =
        Canvas::clip_text(status_text(c, s.statusMessage, s.statusNameAt), STATUS_MAX_CHARS);
    const Argb        color = s.statusSuccess ? s.theme.vizWave : 0xFFFF0000;
    c.text(text, SIDE_SPACER + 10, STRIP_ROW_1, color);
}

// A held on a value whose cell names a unit: what it means, in two lines. False when there is none.
bool TrackerLayout::draw_value_hint(Canvas& c, const AppState& s) const {
    const ValueHint h = held_value_hint(c, s);
    if (h.head.empty()) return false;
    const int x = SIDE_SPACER + 10;
    c.text(Canvas::clip_text(h.head, STATUS_MAX_CHARS), x, STRIP_ROW_1, s.theme.vizWave);
    c.text(Canvas::clip_text(h.detail, STATUS_MAX_CHARS), x, STRIP_ROW_2, s.theme.textTitle);
    return true;
}

// ─── The selection scope ("SEL:ROW") over the clipboard ("PHR:2x3"), top-right of the strip ──────
void TrackerLayout::draw_selection_clipboard(Canvas& c, const AppState& s) const {
    const std::string sel  = s.selection.info();
    const std::string clip = s.clipboard ? s.clipboard->info() : std::string();
    if (sel.empty() && clip.empty()) return;

    const int x = SIDE_SPACER + OscilloscopeModule::WIDTH - 150;
    if (!sel.empty())
        c.label(sel, x, STRIP_ROW_1, s.theme.vizWave);
    if (!clip.empty())
        c.label(clip, x, sel.empty() ? STRIP_ROW_1 : STRIP_ROW_2, s.theme.textTitle);
}

// ─── ONE HAND: the locked buttons, boxed; one about to lock, plain ──────────────────────────────
// Each letter has its own place, so a lock never moves the others. Under the note monitor, or at the
// right on the full-screen modules, which have no right bar: the file browser's path line, the
// sample editor's empty row under SELECTION.
void TrackerLayout::draw_button_locks(Canvas& c, const AppState& s) const {
    if (!s.buttonsLocked && !s.buttonsLocking) return;
    static constexpr struct { Button button; const char* letter; } SLOTS[] = {
        {Button::A, "A"}, {Button::B, "B"}, {Button::L_SHIFT, "L"}, {Button::R_SHIFT, "R"},
        {Button::SELECT, "S"},
    };
    static constexpr int SLOT_W = 23;   // five fill the right bar; a box each, with a gap between
    const bool full = full_screen_module(s);
    const int  x0   = full ? DESIGN_W - SIDE_SPACER - 5 * SLOT_W : RIGHT_BAR_X;
    const int  y    = !full ? EDITOR_Y + HEADER_Y + 10 * ROW_HEIGHT + 10
                    : s.currentScreen == ScreenType::SAMPLE_EDITOR
                        ? SampleEditorModule::row_y(SampleRow::SELECTION) + ROW_HEIGHT
                        : SCREEN_SPACER + ROW_HEIGHT;
    for (int i = 0; i < 5; ++i) {
        const uint16_t bit = static_cast<uint16_t>(1u << static_cast<int>(SLOTS[i].button));
        const int      x   = x0 + i * SLOT_W + 3;
        if (s.buttonsLocked & bit) {
            c.fill_rect(x - 2, y, CHAR_W + 2, ROW_HEIGHT, s.theme.vizWave);
            c.label(SLOTS[i].letter, x, y + TEXT_PADDING, s.theme.background);
        } else if (s.buttonsLocking & bit) {
            c.label(SLOTS[i].letter, x, y + TEXT_PADDING, s.theme.vizWave);
        }
    }
}

void TrackerLayout::draw_right_bar(Canvas& c, const AppState& s) const {
    const Theme&             t = s.theme;
    const songcore::Project& p = *s.project;

    // On the editors' column header row.
    const int bpmRowY  = EDITOR_Y + HEADER_Y;  // 117
    const int bpmTextY = bpmRowY + TEXT_PADDING;      // 120

    c.label("T>", RIGHT_BAR_X + 2, bpmTextY, t.textEmpty);
    // What plays: a leading clock's tempo, else the song's.
    c.text(std::to_string(songcore::tempo_in_force(p)), RIGHT_BAR_X + 2 + 34, bpmTextY, t.textValue);

    // The note monitor: a blank row, then "1  C-4" per track.
    const int trackRowsStartY = bpmRowY + ROW_HEIGHT + ROW_HEIGHT;  // 159
    for (int i = 0; i < 8; ++i) {
        const int            textY = trackRowsStartY + (i * ROW_HEIGHT) + TEXT_PADDING;
        const songcore::Note note  = s.trackNotes[i];
        const bool           empty = (note == songcore::Note::EMPTY());

        c.text(std::to_string(i + 1), RIGHT_BAR_X + 2, textY, t.textParam);
        c.text(note_name(note), RIGHT_BAR_X + 2 + 34, textY, empty ? t.textEmpty : t.textValue);
    }

    NavigationMapState ns;
    ns.currentScreen      = s.currentScreen;
    ns.sourceColumn       = s.previousColumn;
    ns.instrumentFromPool = s.instrumentFromPool;
    ns.rHeld              = s.rHeld;
    ns.theme              = t;
    navigationMap_.draw(c, RIGHT_BAR_X, DESIGN_H - NavigationMapModule::HEIGHT - SCREEN_SPACER, ns);
}

void TrackerLayout::draw_placeholder(Canvas& c, int x, int y, ScreenType screen,
                                     const Theme& t) const {
    c.fill_rect(x, y, OscilloscopeModule::WIDTH, 392, t.background);

    c.label(screen_label(screen), x + 20, y + TEXT_PADDING, t.textTitle);

    const std::string message = c.cased("COMING SOON");
    const int         msgW    = c.text_width(message);
    c.text(message, x + (OscilloscopeModule::WIDTH - msgW) / 2, y + 180, t.textEmpty);
}

}  // namespace pt::ui
