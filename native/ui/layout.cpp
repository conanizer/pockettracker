#include "ui/layout.h"

#include <algorithm>
#include <string>

#include "songcore/chords.h"
#include "songcore/scales.h"
#include "ui/buttons.h"
#include "ui/clipboard.h"
#include "ui/helpers.h"
#include "ui/modules/fx_helper_overlay.h"
#include "ui/modules/map_picker_overlay.h"
#include "ui/modules/render_dialog.h"
#include "ui/song_pointer.h"

namespace pt::ui {

namespace {

constexpr int STATUS_MAX_CHARS = 34;   // one line

// Is an overlay drawn in the editor's place? ⚠️ Mirrors `draw_editor`'s ifs: a new overlay there goes
// here too, or `has_falling_meters` holds the redraw open for ever.
bool editor_overlay_up(const AppState& s) {
    return s.eq.isOpen || s.themeEditor.isOpen;
}
}  // namespace

void TrackerLayout::draw(Canvas& c, const AppState& s) {
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
        return;
    }

    draw_scope_strip(c, s);
    draw_editor(c, s);

    if (s.currentScreen != ScreenType::SETTINGS) draw_right_bar(c, s);

    // Over the scope strip, unless help has it.
    if (!s.helpOpen) {
        draw_status_line(c, s);
        draw_selection_clipboard(c, s);
    }

    draw_overlays(c, s);
}

// ─── The file browser and sample editor: full screen, no furniture ───────────────────────────────
// The sample editor's help goes in the waveform's place.
void TrackerLayout::draw_full_screen(Canvas& c, const AppState& s) {
    const Theme& t = s.theme;
    if (s.currentScreen == ScreenType::FILE_BROWSER) {
        fileBrowser_.draw(c, 0, 0, s.fileBrowser, t);
    } else {
        sampleEditor_.draw(c, 0, 0, s.sampleEditor, t);
        // The TSTRETCH box dims the frame, so help is drawn after it.
        if (s.project) draw_stretch_editor(c, s.sampleEditor, s.project->tempo, t);
        if (s.helpOpen)
            helpPanel_.draw(c, SIDE_SPACER, SampleEditorModule::WAVEFORM_Y, help_topic(s), t,
                            SampleEditorModule::WAVEFORM_H);
    }
    if (s.qwerty.isOpen) qwerty_.draw(c, s.qwerty, t);
}

// ─── The oscilloscope strip, or the help panel in its place ──────────────────────────────────────
void TrackerLayout::draw_scope_strip(Canvas& c, const AppState& s) {
    const Theme& t = s.theme;
    if (s.helpOpen) {
        helpPanel_.draw(c, SIDE_SPACER, SCREEN_SPACER, help_topic(s), t);
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
    ps.cursorRow      = s.cursorRow;
    ps.cursorColumn   = s.cursorColumn;
    std::copy(std::begin(s.playheads), std::end(s.playheads), std::begin(ps.playheads));
    ps.selectionMode  = s.selection_mode();
    ps.isCellSelected = [&s](int row, int col) { return s.is_cell_selected(row, col); };
    ps.theme          = s.theme;
    ps.project        = &p;   // whether an AUS/AUF cell is live depends on every walk
    // Under NAV = SONG, the chain row and song cell the phrase is seen through; −1 under POOL.
    if (s.settings.navSongRelative) {
        ps.viaChain    = s.currentChain;
        ps.viaChainRow = pointer_chain_row(s);
        ps.songRow     = pointer_song_row(s);
        ps.songTrack   = pointer_track(s);
    }
    return ps;
}

ChainEditorState chain_state(const AppState& s) {
    ChainEditorState cs{s.project->chains[static_cast<size_t>(s.currentChain)]};
    cs.cursorRow      = s.cursorRow;
    cs.cursorColumn   = s.cursorColumn;
    std::copy(std::begin(s.playheads), std::end(s.playheads), std::begin(cs.playheads));
    cs.selectionMode  = s.selection_mode();
    cs.isCellSelected = [&s](int row, int col) { return s.is_cell_selected(row, col); };
    cs.theme          = s.theme;
    if (s.settings.navSongRelative) {
        cs.songRow   = pointer_song_row(s);
        cs.songTrack = pointer_track(s);
    }
    return cs;
}

SongEditorState song_state(const AppState& s) {
    SongEditorState ss{*s.project};
    ss.cursorRow      = s.cursorRow;
    ss.cursorTrack    = s.cursorColumn;
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
    ts.cursorRow    = s.tableCursorRow;
    ts.cursorColumn = s.tableCursorColumn;
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
    gs.cursorRow    = s.grooveCursorRow;
    gs.cursorColumn = s.grooveCursorColumn;
    gs.panelRow     = s.groovePanelRow;
    gs.panelColumn  = s.groovePanelColumn;
    gs.quantize     = s.grooveQuantize;
    gs.theme        = s.theme;
    return gs;
}

ScaleState scale_state(const AppState& s) {
    ScaleState cs{s.project->scales[static_cast<size_t>(s.currentScale)]};
    cs.key          = s.project->scaleKey;
    cs.cursorRow    = s.scaleCursorRow;
    cs.cursorColumn = s.scaleCursorColumn;
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
    is.cursorRow     = s.instrumentCursorRow;
    is.cursorColumn  = s.instrumentCursorColumn;
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
    es.theme     = s.theme;
    return es;
}

ProjectState project_state(const AppState& s) {
    ProjectState prs{*s.project};
    prs.cursorRow      = s.projectCursorRow;
    prs.cursorColumn   = s.projectCursorColumn;
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
    ss.cursorRow    = s.settingsCursorRow;
    ss.cursorColumn = s.settingsCursorColumn;
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
    MidiState ms{*s.project, s.settings, s.midiDeviceNames, s.midiInDeviceNames};
    ms.lastCcChannel = s.midiInCcChannel;
    ms.clockBpm      = s.midiClockBpm;
    ms.cursorRow     = s.midiCursorRow;
    ms.cursorColumn  = s.midiCursorColumn;
    ms.deviceIndex   = s.midiDeviceIndex;
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
    mm.cursorRow    = s.midiMapCursorRow;
    mm.cursorColumn = s.midiMapCursorColumn;
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
    if (s.themeEditor.isOpen) {
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

// Last, over everything — a modal's backdrop dims the whole frame.
void TrackerLayout::draw_overlays(Canvas& c, const AppState& s) {
    const Theme& t = s.theme;
    draw_fx_helper(c, s.fxHelper, t);
    draw_map_picker(c, s.mapPicker, t);
    if (send_editor_drawn(s)) {
        draw_send_editor(c, s.sendEditor, *s.project, t);
        // Its cells have help; the panel goes over the strip the backdrop just dimmed.
        if (s.helpOpen) helpPanel_.draw(c, SIDE_SPACER, SCREEN_SPACER, help_topic(s), t);
    }
    draw_render_dialog(c, s.renderDialog, *s.project, s.isRendering, s.renderProgress, t);
    if (s.qwerty.isOpen) qwerty_.draw(c, s.qwerty, t);
    draw_confirm_dialog(c, s.confirm, t);
}

bool TrackerLayout::has_falling_meters(const AppState& s) const {
    if (!s.project) return false;

    // The EQ editor's panel, asked what it last drew.
    if (s.eq.isOpen && !eq_.spectrum_at_rest()) return true;

    // ⚠️ Only when the mixer is drawn: under an overlay a falling marker never ages and the loop pins
    // at 60 Hz.
    if (s.currentScreen == ScreenType::MIXER && !editor_overlay_up(s) && !mixer_.peaks_at_rest())
        return true;

    // The SPECTRUM bars, likewise only while drawn.
    const VisualizerType vt = s.theme.visualizerType;
    if (vt != VisualizerType::SPECTRUM && vt != VisualizerType::SPECTRUM_PEAKS) return false;
    return !full_screen_module(s) && !s.helpOpen && !oscilloscope_.bars_at_rest();
}

// ─── The status line, top-left of the scope strip ────────────────────────────────────────────────
void TrackerLayout::draw_status_line(Canvas& c, const AppState& s) const {
    if (draw_chord_hint(c, s)) return;
    if (s.statusMessage.empty()) return;

    const std::string text = Canvas::clip_text(s.statusMessage, STATUS_MAX_CHARS);
    const Argb        color = s.statusSuccess ? s.theme.vizWave : 0xFFFF0000;
    c.draw_text(text, SIDE_SPACER + 10, SCREEN_SPACER + 10, color, CHAR_SPACING, FONT_SCALE);
}

// A held on a CHD value: the chord's name and its notes. In a PHRASE, made of this row's note in the
// track's scale as far as this phrase shows it; in a TABLE, on C-4 moved by the row's transpose, as the
// table plays it — unsnapped, and named as the note it lands on.
// False when the cursor is elsewhere.
bool TrackerLayout::draw_chord_hint(Canvas& c, const AppState& s) const {
    if (!s.aHeld || s.selection.active) return false;
    const songcore::Project& p = *s.project;
    int value = -1, root = 60;
    const songcore::Scale* scale = nullptr;
    int key = 0;
    std::string where;   // the root's name, or why there is none
    if (s.currentScreen == ScreenType::PHRASE) {
        const int col = s.cursorColumn;
        if (col != 5 && col != 7 && col != 9) return false;
        const songcore::Phrase&     ph   = p.phrases[static_cast<size_t>(s.currentPhrase)];
        const songcore::PhraseStep& step = ph.steps[static_cast<size_t>(s.cursorRow)];
        const int                   slot = (col - 3) / 2;
        if (songcore::step_fx_type(step, slot) != songcore::FX_CHD) return false;
        value = songcore::step_fx_value(step, slot);
        if (!songcore::note_is_pitch(step.note)) {
            where = "NEEDS A NOTE ON THIS ROW";
        } else {
            const songcore::NoteScale ns = songcore::phrase_note_scale(p, ph, s.cursorRow, step.instrument);
            root  = ns.snap(songcore::note_to_midi(step.note));
            scale = ns.scale;
            key   = ns.key;
        }
    } else if (s.currentScreen == ScreenType::TABLE) {
        const int col = s.tableCursorColumn;
        if (col != 4 && col != 6 && col != 8) return false;
        const songcore::TableRow& r =
            p.tables[static_cast<size_t>(s.currentTable)].rows[static_cast<size_t>(s.tableCursorRow)];
        const int slot = (col - 4) / 2;
        const int type = slot == 0 ? r.fx1Type : slot == 1 ? r.fx2Type : r.fx3Type;
        if (type != songcore::FX_CHD) return false;
        value = slot == 0 ? r.fx1Value : slot == 1 ? r.fx2Value : r.fx3Value;
        // The row's transpose moves the note the chord is built on: 00-7F up, 80-FF down.
        const int tsp = r.transpose < 0x80 ? r.transpose : r.transpose - 256;
        root = std::max(0, std::min(127, 60 + tsp));
    } else {
        return false;
    }

    std::string head  = std::string(songcore::chord_name(value)) + "  " + songcore::chord_layout_name(value);
    std::string notes = where;
    if (where.empty()) {
        if (s.currentScreen == ScreenType::PHRASE) {
            const char* name = songcore::NOTE_NAMES[songcore::scale_mod12(root)];
            head = std::string(name, name[1] == '-' ? 1 : 2) + " " + head;
        } else {
            notes = "ON " + songcore::note_name(songcore::note_from_midi(root)) + ":";
        }
        int semis[songcore::CHORD_MAX_NOTES];
        const int n = songcore::chord_notes(value, root, scale, key, semis);
        for (int i = 0; i < n; ++i) {
            if (!notes.empty()) notes += ' ';
            notes += songcore::note_name(songcore::note_from_midi(root + semis[i]));
        }
    }
    const int x = SIDE_SPACER + 10, y = SCREEN_SPACER + 10;
    c.draw_text(Canvas::clip_text(head, STATUS_MAX_CHARS), x, y, s.theme.vizWave, CHAR_SPACING, FONT_SCALE);
    c.draw_text(Canvas::clip_text(notes, STATUS_MAX_CHARS), x, y + 21, s.theme.textTitle, CHAR_SPACING,
                FONT_SCALE);
    return true;
}

// ─── The selection scope ("SEL:ROW") over the clipboard ("PHR:2x3"), top-right of the strip ──────
void TrackerLayout::draw_selection_clipboard(Canvas& c, const AppState& s) const {
    const std::string sel  = s.selection.info();
    const std::string clip = s.clipboard ? s.clipboard->info() : std::string();
    if (sel.empty() && clip.empty()) return;

    const int x = SIDE_SPACER + OscilloscopeModule::WIDTH - 150;
    const int y = SCREEN_SPACER + 10;

    if (!sel.empty())
        c.draw_text(sel, x, y, s.theme.vizWave, CHAR_SPACING, FONT_SCALE);
    if (!clip.empty())
        c.draw_text(clip, x, sel.empty() ? y : y + 21, s.theme.textTitle, CHAR_SPACING, FONT_SCALE);
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
    const int  y    = !full ? EDITOR_Y + ROW_HEIGHT + 14 + 10 * ROW_HEIGHT + 10
                    : s.currentScreen == ScreenType::SAMPLE_EDITOR ? SampleEditorModule::content_y(9)
                                                                   : SCREEN_SPACER + ROW_HEIGHT;
    for (int i = 0; i < 5; ++i) {
        const uint16_t bit = static_cast<uint16_t>(1u << static_cast<int>(SLOTS[i].button));
        const int      x   = x0 + i * SLOT_W + 3;
        if (s.buttonsLocked & bit) {
            c.fill_rect(x - 2, y, CHAR_W + 2, ROW_HEIGHT, s.theme.vizWave);
            c.draw_text(SLOTS[i].letter, x, y + TEXT_PADDING, s.theme.background, CHAR_SPACING, FONT_SCALE);
        } else if (s.buttonsLocking & bit) {
            c.draw_text(SLOTS[i].letter, x, y + TEXT_PADDING, s.theme.vizWave, CHAR_SPACING, FONT_SCALE);
        }
    }
}

void TrackerLayout::draw_right_bar(Canvas& c, const AppState& s) const {
    const Theme&             t = s.theme;
    const songcore::Project& p = *s.project;

    // On the editors' column header row.
    const int bpmRowY  = EDITOR_Y + ROW_HEIGHT + 14;  // 117
    const int bpmTextY = bpmRowY + TEXT_PADDING;      // 120

    c.draw_text("T>", RIGHT_BAR_X + 2, bpmTextY, t.textEmpty, CHAR_SPACING, FONT_SCALE);
    // What plays: a leading clock's tempo, else the song's.
    c.draw_text(std::to_string(songcore::tempo_in_force(p)), RIGHT_BAR_X + 2 + 34, bpmTextY, t.textValue,
                CHAR_SPACING, FONT_SCALE);

    // The note monitor: a blank row, then "1  C-4" per track.
    const int trackRowsStartY = bpmRowY + ROW_HEIGHT + ROW_HEIGHT;  // 159
    for (int i = 0; i < 8; ++i) {
        const int            textY = trackRowsStartY + (i * ROW_HEIGHT) + TEXT_PADDING;
        const songcore::Note note  = s.trackNotes[i];
        const bool           empty = (note == songcore::Note::EMPTY());

        c.draw_text(std::to_string(i + 1), RIGHT_BAR_X + 2, textY, t.textParam, CHAR_SPACING,
                    FONT_SCALE);
        c.draw_text(note_name(note), RIGHT_BAR_X + 2 + 34, textY, empty ? t.textEmpty : t.textValue,
                    CHAR_SPACING, FONT_SCALE);
    }

    NavigationMapState ns;
    ns.currentScreen      = s.currentScreen;
    ns.sourceColumn       = s.previousColumn;
    ns.instrumentFromPool = s.instrumentFromPool;
    ns.theme              = t;
    navigationMap_.draw(c, RIGHT_BAR_X, DESIGN_H - NavigationMapModule::HEIGHT - SCREEN_SPACER, ns);
}

void TrackerLayout::draw_placeholder(Canvas& c, int x, int y, ScreenType screen,
                                     const Theme& t) const {
    c.fill_rect(x, y, OscilloscopeModule::WIDTH, 392, t.background);

    c.draw_text(screen_label(screen), x + 20, y + TEXT_PADDING, t.textTitle, CHAR_SPACING,
                FONT_SCALE);

    const std::string message = "COMING SOON";
    const int         msgW    = Canvas::text_width(message, CHAR_SPACING, FONT_SCALE);
    c.draw_text(message, x + (OscilloscopeModule::WIDTH - msgW) / 2, y + 180, t.textEmpty,
                CHAR_SPACING, FONT_SCALE);
}

}  // namespace pt::ui
