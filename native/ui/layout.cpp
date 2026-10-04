#include "ui/layout.h"

#include <algorithm>
#include <string>

#include "songcore/scales.h"   // scale_mod12 — the SCALE screen's sounding-pitch mask
#include "ui/clipboard.h"
#include "ui/helpers.h"
#include "ui/modules/fx_helper_overlay.h"
#include "ui/modules/map_picker_overlay.h"
#include "ui/modules/render_dialog.h"
#include "ui/song_pointer.h"   // NAV = SONG — the cell the CHAIN/PHRASE headers read out

namespace pt::ui {

namespace {

/** The status strip's budget, in columns — it is one line high and shares it with nothing. */
constexpr int STATUS_MAX_CHARS = 34;

/**
 * Is an overlay standing in the editor's place (so `currentScreen`'s module is NOT drawn)? Overlays do
 * not change `currentScreen`, so a module that ages its picture in its own draw stops ageing under one
 * — `has_falling_meters` must ask, or it holds the idle gate open forever.
 * ⚠️ Mirrors `draw`'s if/else chain below: a new overlay there goes here too.
 */
bool editor_overlay_up(const AppState& s) {
    return s.eq.isOpen || s.themeEditor.isOpen;
}
}  // namespace

void TrackerLayout::draw(Canvas& c, const AppState& s) {
    draw_frame(c, s);

    // The full help overlay, on every screen — here rather than with the other modals because the FILE
    // BROWSER returns early from `draw_frame`, and it has no other help.
    if (s.helpFull && s.project) helpOverlay_.draw(c, help_topic(s), s.theme);

    // ⚠️⚠️ OUTSIDE `draw_frame`: the file browser and the sample editor return from its middle, and every
    // load starts on those screens. One draw site after everything, reachable from every screen.
    draw_loading_strip(c, s.loading, s.theme);
}

void TrackerLayout::draw_frame(Canvas& c, const AppState& s) {
    const Theme& t = s.theme;

    c.fill_rect(0, 0, DESIGN_W, DESIGN_H, t.background);

    if (!s.project) return;  // no document: the background alone

    // ── The FILE BROWSER and SAMPLE EDITOR are FULL-SCREEN ───────────────────────────────────────
    //
    // They return before any furniture (no scope strip, right bar or nav map): the browser needs the
    // width for nineteen rows of filenames, a waveform every pixel of it. The QWERTY keyboard still draws
    // on top (rename; A on the NAME row).
    // ⚠️ `&& !s.eq.isOpen`: the EQ editor opened from the sample editor's FX row REPLACES it and the
    // normal furniture returns.
    // The sample editor's HELP goes in the waveform's place: same left edge and width as the strip,
    // 85 px taller. Never over its "ARE YOU SURE?" — `on_select` refuses help while that is up.
    if (full_screen_module(s)) {
        if (s.currentScreen == ScreenType::FILE_BROWSER) {
            fileBrowser_.draw(c, 0, 0, s.fileBrowser, t);
        } else {
            sampleEditor_.draw(c, 0, 0, s.sampleEditor, t);
            if (s.helpOpen)
                helpPanel_.draw(c, SIDE_SPACER, SampleEditorModule::WAVEFORM_Y, help_topic(s), t,
                                SampleEditorModule::WAVEFORM_H);
        }
        if (s.qwerty.isOpen) qwerty_.draw(c, s.qwerty, t);
        return;
    }

    const songcore::Project& p       = *s.project;
    const int                moduleX = SIDE_SPACER;

    // ── The oscilloscope strip — or the HELP PANEL standing in for it ────────────────────────────
    //
    // Help takes the whole strip (three 21 px lines = 63 of its 70), so the scope, status line and
    // selection readout are not drawn under it.
    if (s.helpOpen) {
        helpPanel_.draw(c, moduleX, SCREEN_SPACER, help_topic(s), t);
    } else {
        OscilloscopeState os;
        os.waveform = s.waveform;
        os.theme    = t;

        const bool isOctaFull = (t.visualizerType == VisualizerType::OCTA_FULL);
        const bool isOcta     = (t.visualizerType == VisualizerType::OCTA);
        if (isOcta || isOctaFull) os.trackWaveforms = s.trackWaveforms;

        // OCTA_FULL forces all 8 lanes on, so the strip never reflows mid-song. OCTA shows the tracks
        // that have played — plus the preview lane, only while stopped.
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

        oscilloscope_.draw(c, moduleX, SCREEN_SPACER, os);
    }

    // ── The editor ───────────────────────────────────────────────────────────────────────────────
    // Clipped to the left of the right bar.
    {
        Canvas::ClipScope clip(c, 0, 0, EDITOR_CLIP_RIGHT, DESIGN_H);

        // ── The EQ EDITOR replaces the module, and leaves the furniture alone ────────────────────
        //
        // Same clip and origin; the scope, BPM, note monitor and nav map keep drawing — an EQ is dialled
        // while a note rings, and the note monitor shows that it still is.
        if (s.eq.isOpen) {
            EqState es{p};
            es.slotIndex     = s.eq.slotIndex;
            es.cursorRow     = s.eq.cursorRow;
            es.caller        = s.eq.caller;
            es.spectrum      = s.eqSpectrum;
            es.spectrumCount = s.eqSpectrumCount;
            es.sampleRate    = static_cast<float>(s.eqSampleRate);
            es.theme         = t;
            eq_.draw(c, moduleX, EDITOR_Y, es);
        } else if (s.themeEditor.isOpen) {
            // ── The THEME EDITOR replaces the module on the same terms ───────────────────────────
            //
            // The oscilloscope strip keeps drawing — VIZ BG / LINE / WAVE are the strip, dialled against
            // a moving waveform (START passes through). The right bar is absent because SETTINGS hides
            // it, so the meter colours (MTR *) cannot be previewed in situ.
            ThemeState ts;
            ts.theme  = t;
            ts.editor = s.themeEditor;
            themeEditor_.draw(c, moduleX, EDITOR_Y, ts);
        } else switch (s.currentScreen) {   // an overlay is drawn INSTEAD of `currentScreen`
            case ScreenType::PHRASE: {
                PhraseEditorState ps{p.phrases[static_cast<size_t>(s.currentPhrase)]};
                ps.cursorRow      = s.cursorRow;
                ps.cursorColumn   = s.cursorColumn;
                std::copy(std::begin(s.playheads), std::end(s.playheads), std::begin(ps.playheads));
                ps.selectionMode  = s.selection_mode();
                ps.isCellSelected = [&s](int row, int col) { return s.is_cell_selected(row, col); };
                ps.theme          = t;
                // Whether an AUS/AUF cell is live depends on every chain walk this phrase appears in;
                // the editor finds them from the phrase's id.
                ps.project        = &p;
                // Under NAV = SONG the phrase is seen THROUGH a chain row and a song cell, which decide
                // where the next B+D-pad press goes. −1 under POOL (ui/song_pointer.h).
                if (s.settings.navSongRelative) {
                    ps.viaChain    = s.currentChain;
                    ps.viaChainRow = pointer_chain_row(s);
                    ps.songRow     = pointer_song_row(s);
                    ps.songTrack   = pointer_track(s);
                }
                phraseEditor_.draw(c, moduleX, EDITOR_Y, ps);
                break;
            }

            case ScreenType::CHAIN: {
                ChainEditorState cs{p.chains[static_cast<size_t>(s.currentChain)]};
                cs.cursorRow      = s.cursorRow;
                cs.cursorColumn   = s.cursorColumn;
                std::copy(std::begin(s.playheads), std::end(s.playheads), std::begin(cs.playheads));
                cs.selectionMode  = s.selection_mode();
                cs.isCellSelected = [&s](int row, int col) { return s.is_cell_selected(row, col); };
                cs.theme          = t;
                if (s.settings.navSongRelative) {   // see the PHRASE arm above
                    cs.songRow   = pointer_song_row(s);
                    cs.songTrack = pointer_track(s);
                }
                chainEditor_.draw(c, moduleX, EDITOR_Y, cs);
                break;
            }

            case ScreenType::SONG: {
                SongEditorState ss{p};
                ss.cursorRow      = s.cursorRow;
                ss.cursorTrack    = s.cursorColumn;  // on SONG the column IS the track (1..8)
                ss.scrollPosition = s.songScrollPosition;
                std::copy(std::begin(s.playheads), std::end(s.playheads), std::begin(ss.playheads));
                ss.liveMode      = s.liveMode;
                std::copy(std::begin(s.liveQueue), std::end(s.liveQueue), std::begin(ss.liveQueue));
                ss.blinkPhaseMs  = s.blinkPhaseMs;
                ss.selectionMode  = s.selection_mode();
                ss.isCellSelected = [&s](int row, int col) { return s.is_cell_selected(row, col); };
                ss.theme          = t;
                songEditor_.draw(c, moduleX, EDITOR_Y, ss);
                break;
            }

            case ScreenType::TABLE: {
                TableState ts{p.tables[static_cast<size_t>(s.currentTable)]};
                ts.cursorRow    = s.tableCursorRow;
                ts.cursorColumn = s.tableCursorColumn;
                for (int l = 0; l < TABLE_LANES; ++l) ts.playbackRows[l] = s.tablePlaybackRows[l];
                // The tic rate is the INSTRUMENT's, not the table's — one table run by two instruments
                // runs at two speeds; this shows the one you are looking through.
                ts.ticRate      = p.instruments[static_cast<size_t>(s.currentInstrument)].tableTicRate;
                ts.selectionMode  = s.selection_mode();
                ts.isCellSelected = [&s](int row, int col) { return s.is_cell_selected(row, col); };
                ts.theme          = t;
                tableModule_.draw(c, moduleX, EDITOR_Y, ts);
                break;
            }

            case ScreenType::GROOVE: {
                GrooveState gs{p.grooves[static_cast<size_t>(s.currentGroove)]};
                gs.cursorRow    = s.grooveCursorRow;
                gs.cursorColumn = s.grooveCursorColumn;
                gs.panelRow     = s.groovePanelRow;
                gs.panelColumn  = s.groovePanelColumn;
                gs.quantize     = s.grooveQuantize;
                gs.theme        = t;
                grooveModule_.draw(c, moduleX, EDITOR_Y, gs);
                break;
            }

            case ScreenType::SCALE: {
                ScaleState cs{p.scales[static_cast<size_t>(s.currentScale)]};
                cs.key          = p.scaleKey;
                cs.cursorRow    = s.scaleCursorRow;
                cs.cursorColumn = s.scaleCursorColumn;
                // The pitch classes coming out of the speaker, from the voice readback the note monitor
                // uses — ⚠️ NOT the sequencer, two phrases ahead. All eight tracks fold into one mask.
                for (int i = 0; i < 8; ++i) {
                    const songcore::Note n = s.trackNotes[i];
                    if (n == songcore::Note::EMPTY()) continue;
                    const int midi = songcore::note_to_midi(n);
                    if (midi >= 0) cs.soundingMask |= 1u << songcore::scale_mod12(midi);
                }
                cs.theme     = t;
                scaleModule_.draw(c, moduleX, EDITOR_Y, cs);
                break;
            }

            case ScreenType::INSTRUMENT: {
                InstrumentEditorState is{p.instruments[static_cast<size_t>(s.currentInstrument)]};
                is.cursorRow     = s.instrumentCursorRow;
                is.cursorColumn  = s.instrumentCursorColumn;
                // The SF2's preset list as the engine last answered (engine_feed.h); zeroes and "---"
                // with no engine, which a headless screenshot draws.
                is.sfPresetName  = s.sfPresetName;
                is.sfPresetCount = s.sfPresetCount;
                is.sfPresetIndex = s.sfPresetIndex;
                is.theme         = t;
                instrumentEditor_.draw(c, moduleX, EDITOR_Y, is);
                break;
            }

            case ScreenType::INST_POOL: {
                InstrumentPoolState ps{p};
                // Its cursor ROW is the selected instrument itself.
                ps.selectedInstrument = s.currentInstrument;
                ps.cursorColumn       = s.poolCursorColumn;
                ps.sampleRamBytes     = s.sampleRamBytes;
                ps.caps               = s.caps;
                ps.theme              = t;
                instrumentPool_.draw(c, moduleX, EDITOR_Y, ps);
                break;
            }

            case ScreenType::MODS: {
                ModulationState ms{p.instruments[static_cast<size_t>(s.currentInstrument)]};
                ms.cursorRow  = s.modCursorRow;
                ms.cursorPair = s.modCursorPair;
                ms.cursorSide = s.modCursorSide;
                ms.theme      = t;
                modulation_.draw(c, moduleX, EDITOR_Y, ms);
                break;
            }

            case ScreenType::MIXER: {
                MixerState xs{p};
                xs.cursorColumn   = s.mixerCursorColumn;
                xs.mixerMasterRow = s.mixerMasterRow;
                // The engine's meters as last read (ui/engine_feed.h); zeroes with no engine.
                xs.trackPeaks   = s.trackPeaks;
                xs.masterPeaks  = s.masterPeaks;
                xs.reverbPeaks  = &s.sendPeaks[0];
                xs.delayPeaks   = &s.sendPeaks[2];
                xs.peaksVersion = s.peaksVersion;
                xs.theme        = t;
                mixer_.draw(c, moduleX, EDITOR_Y, xs);
                break;
            }

            case ScreenType::EFFECTS: {
                EffectState es{p};
                es.cursorRow = s.effectsCursorRow;
                es.theme     = t;
                effects_.draw(c, moduleX, EDITOR_Y, es);
                break;
            }

            case ScreenType::PROJECT: {
                ProjectState prs{p};
                prs.cursorRow      = s.projectCursorRow;
                prs.cursorColumn   = s.projectCursorColumn;
                prs.isRendering    = s.isRendering;
                prs.renderProgress = s.renderProgress;
                prs.sampleRamBytes = s.sampleRamBytes;
                prs.freeRamBytes   = s.freeRamBytes;
                prs.caps           = s.caps;
                prs.theme          = t;
                project_.draw(c, moduleX, EDITOR_Y, prs);
                break;
            }

            case ScreenType::SETTINGS: {
                SettingsState ss{s.settings};
                ss.cursorRow    = s.settingsCursorRow;
                ss.cursorColumn = s.settingsCursorColumn;
                // The DEVICE rows' text — only the platform can name what an index means. Empty on the
                // shell, which does not draw them.
                ss.layoutText   = s.layoutText;
                ss.skinText     = s.skinText;
                ss.overlayText  = s.overlayText;
                ss.audioOutText = s.audioOutText;
                ss.themeName    = t.name;
                ss.caps         = s.caps;
                ss.theme        = t;
                settings_.draw(c, moduleX, EDITOR_Y, ss);
                break;
            }

            case ScreenType::MIDI: {
                // The port lists as the dispatcher enumerated them — text the module paints but does
                // not own.
                MidiState ms{p, s.settings, s.midiDeviceNames, s.midiInDeviceNames};
                ms.lastCcChannel  = s.midiInCcChannel;
                ms.cursorRow    = s.midiCursorRow;
                ms.cursorColumn = s.midiCursorColumn;
                ms.deviceIndex   = s.midiDeviceIndex;
                ms.inDeviceIndex = s.midiInDeviceIndex;
                ms.outOpenName   = s.midiOutOpenName;
                ms.inOpenName    = s.midiInOpenName;
                ms.autoOffsetMs  = s.midiAutoOffsetMs;
                ms.audioLoad     = s.audioLoad;
                ms.statusText    = s.midiStatusText;
                ms.caps          = s.caps;
                ms.theme         = t;
                midi_.draw(c, moduleX, EDITOR_Y, ms);
                break;
            }

            case ScreenType::MIDI_MAP: {
                MidiMapState mm{p};
                mm.cursorRow    = s.midiMapCursorRow;
                mm.cursorColumn = s.midiMapCursorColumn;
                mm.theme        = t;
                midiMap_.draw(c, moduleX, EDITOR_Y, mm);
                break;
            }

            default:
                draw_placeholder(c, moduleX, EDITOR_Y, s.currentScreen, t);
                break;
        }
    }

    // ── The right bar ────────────────────────────────────────────────────────────────────────────
    //
    // Hidden on SETTINGS: no playhead or notes to monitor there.
    if (s.currentScreen != ScreenType::SETTINGS) draw_right_bar(c, s);

    // ── The status line, and the selection/clipboard readout ──────────────────────────────────────
    // Over the scope strip so every screen can report: status top-LEFT, selection + clipboard
    // top-RIGHT. Both stand down while HELP is up — help IS the strip.
    if (!s.helpOpen) {
        draw_status_line(c, s);
        draw_selection_clipboard(c, s);
    }

    // ── The overlays ─────────────────────────────────────────────────────────────────────────────
    // LAST, over everything including the right bar — a modal's backdrop dims the whole frame.
    draw_fx_helper(c, s.fxHelper, t);
    draw_map_picker(c, s.mapPicker, t);
    draw_render_dialog(c, s.renderDialog, *s.project, s.isRendering, s.renderProgress, t);
    if (s.qwerty.isOpen) qwerty_.draw(c, s.qwerty, t);
    draw_confirm_dialog(c, s.confirm, t);
}

bool TrackerLayout::has_falling_meters(const AppState& s) const {
    if (!s.project) return false;   // `draw` stops at the background: nothing of ours on screen

    // The EQ editor's spectrum panel: nothing ages in it (the feed re-polls every frame and the values
    // fall to zero with the audio) — what hangs is the last frame DRAWN, so the module is asked what
    // it last drew. Gated on the panel being up, and asked first: the EQ replaces the editor anywhere.
    if (s.eq.isOpen && !eq_.spectrum_at_rest()) return true;

    // ⚠️ MIXER: gated on the mixer being DRAWN, not merely selected — under an overlay a marker caught
    // mid-fall never ages and would pin the loop at 60 Hz.
    if (s.currentScreen == ScreenType::MIXER && !editor_overlay_up(s) && !mixer_.peaks_at_rest())
        return true;

    // The SPECTRUM bars, on the same terms: they fall inside the draw. Off the strip (full-screen
    // modules, other visualizer modes, ⚠️ help up) nothing would ever bring this back to false.
    const VisualizerType vt = s.theme.visualizerType;
    if (vt != VisualizerType::SPECTRUM && vt != VisualizerType::SPECTRUM_PEAKS) return false;
    return !full_screen_module(s) && !s.helpOpen && !oscilloscope_.bars_at_rest();
}

// ─── The global status line ──────────────────────────────────────────────────────────────────────
//
// "SAVED" · "EXPORTED!" · "SEQ CLEANED" · "NO FREE PHRASES" — over the scope strip's top-left, so an
// action on ANY screen can report (SAVE, EXPORT and COMPACT have no other feedback). The full-screen
// editors keep their own status lines and return before this.
void TrackerLayout::draw_status_line(Canvas& c, const AppState& s) const {
    if (s.statusMessage.empty()) return;

    // A longer message is cut with a marker, not wrapped: one line high.
    const std::string text = Canvas::clip_text(s.statusMessage, STATUS_MAX_CHARS);
    const Argb        color = s.statusSuccess ? s.theme.vizWave : 0xFFFF0000;
    c.draw_text(text, SIDE_SPACER + 10, SCREEN_SPACER + 10, color, CHAR_SPACING, FONT_SCALE);
}

// ─── The selection scope and the clipboard, top-right of the scope strip ───────────────────────────
//
// Two stacked lines: the live selection scope ("SEL:CELL" / "SEL:ROW" / "SEL:ALL", in vizWave) and
// below it the clipboard ("PHR:2x3", in textTitle), 150 px in from the strip's right edge.
// The clipboard is reached through AppState's pointer, null with no dispatcher (headless screenshots): then only the
// selection half appears.
void TrackerLayout::draw_selection_clipboard(Canvas& c, const AppState& s) const {
    const std::string sel  = s.selection.info();
    const std::string clip = s.clipboard ? s.clipboard->info() : std::string();
    if (sel.empty() && clip.empty()) return;

    // 150 px in from the scope strip's right edge.
    const int x = SIDE_SPACER + OscilloscopeModule::WIDTH - 150;
    const int y = SCREEN_SPACER + 10;

    if (!sel.empty())
        c.draw_text(sel, x, y, s.theme.vizWave, CHAR_SPACING, FONT_SCALE);
    if (!clip.empty())
        // Below the selection line when both are up, else in its place.
        c.draw_text(clip, x, sel.empty() ? y : y + 21, s.theme.textTitle, CHAR_SPACING, FONT_SCALE);
}

void TrackerLayout::draw_right_bar(Canvas& c, const AppState& s) const {
    const Theme&             t = s.theme;
    const songcore::Project& p = *s.project;

    // The BPM row lines up with every editor's COLUMN HEADER row (title 21 px + 14 px spacer), derived
    // so it cannot drift.
    const int bpmRowY  = EDITOR_Y + ROW_HEIGHT + 14;  // 117
    const int bpmTextY = bpmRowY + TEXT_PADDING;      // 120

    c.draw_text("T>", RIGHT_BAR_X + 2, bpmTextY, t.textEmpty, CHAR_SPACING, FONT_SCALE);
    c.draw_text(std::to_string(p.tempo), RIGHT_BAR_X + 2 + 34, bpmTextY, t.textValue, CHAR_SPACING,
                FONT_SCALE);

    // The note monitor: a blank row below the BPM, then the 8 tracks — "1  C-4", the number dim, the
    // note bright while it sounds.
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
    // 620 wide, wider than the clip, so "COMING SOON" sits slightly left of the visible centre.
    c.fill_rect(x, y, OscilloscopeModule::WIDTH, 392, t.background);

    c.draw_text(screen_label(screen), x + 20, y + TEXT_PADDING, t.textTitle, CHAR_SPACING,
                FONT_SCALE);

    const std::string message = "COMING SOON";
    const int         msgW    = Canvas::text_width(message, CHAR_SPACING, FONT_SCALE);
    c.draw_text(message, x + (OscilloscopeModule::WIDTH - msgW) / 2, y + 180, t.textEmpty,
                CHAR_SPACING, FONT_SCALE);
}

}  // namespace pt::ui
