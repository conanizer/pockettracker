#include "ui/layout.h"

#include <string>

#include "ui/helpers.h"
#include "ui/modules/fx_helper_overlay.h"

namespace pt::ui {

void TrackerLayout::draw(Canvas& c, const AppState& s) {
    const Theme& t = s.theme;

    c.fill_rect(0, 0, DESIGN_W, DESIGN_H, t.background);

    if (!s.project) return;  // no document: the background is the honest thing to draw

    // ── The FILE BROWSER is FULL-SCREEN, and returns before any of the furniture ─────────────────
    //
    // It covers the whole 640×480: no oscilloscope strip, no right bar, no navigation map. That is not
    // a shortcut — it is what `FileBrowserModule.HEIGHT = 480` means, and it is why the browser is a
    // POPUP in ScreenType's own comment rather than a cell in the 5×5 grid. Nineteen file rows and two
    // status bars need the whole panel; a 115px nav map beside them would cost four characters of every
    // filename.
    //
    // The QWERTY keyboard still draws on top (it can be open OVER the browser — SELECT+A renames a
    // file), so the early return is *before* the furniture and *after* nothing.
    if (s.currentScreen == ScreenType::FILE_BROWSER) {
        fileBrowser_.draw(c, 0, 0, s.fileBrowser, t);
        if (s.qwerty.isOpen) qwerty_.draw(c, s.qwerty, t);
        return;
    }

    const songcore::Project& p       = *s.project;
    const int                moduleX = SIDE_SPACER;

    // ── The oscilloscope strip ───────────────────────────────────────────────────────────────────
    {
        OscilloscopeState os;
        os.waveform = s.waveform;
        os.theme    = t;

        const bool isOctaFull = (t.visualizerType == VisualizerType::OCTA_FULL);
        const bool isOcta     = (t.visualizerType == VisualizerType::OCTA);
        if (isOcta || isOctaFull) os.trackWaveforms = s.trackWaveforms;

        // OCTA_FULL forces all 8 song lanes on, whatever is scheduled, so the strip never reflows
        // mid-song. OCTA shows only the tracks that have played — plus the preview lane, and ONLY
        // while stopped: during playback a preview scope would crowd the eight that matter.
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
    // Clipped to the left of the right bar. FILE_BROWSER and SAMPLE_EDITOR are full-screen and draw
    // OUTSIDE this clip when they land (S6/S7); everything else lives inside it.
    {
        Canvas::ClipScope clip(c, 0, 0, EDITOR_CLIP_RIGHT, DESIGN_H);

        switch (s.currentScreen) {
            case ScreenType::PHRASE: {
                PhraseEditorState ps{p.phrases[static_cast<size_t>(s.currentPhrase)]};
                ps.cursorRow      = s.cursorRow;
                ps.cursorColumn   = s.cursorColumn;
                ps.playbackRow    = s.playbackRow;
                ps.isPlaying      = s.isPlaying;
                ps.selectionMode  = s.selection_mode();
                ps.isCellSelected = [&s](int row, int col) { return s.is_cell_selected(row, col); };
                ps.theme          = t;
                phraseEditor_.draw(c, moduleX, EDITOR_Y, ps);
                break;
            }

            case ScreenType::CHAIN: {
                ChainEditorState cs{p.chains[static_cast<size_t>(s.currentChain)]};
                cs.cursorRow      = s.cursorRow;
                cs.cursorColumn   = s.cursorColumn;
                cs.playbackRow    = s.playbackChainRow;
                cs.isPlaying      = s.isPlaying;
                cs.selectionMode  = s.selection_mode();
                cs.isCellSelected = [&s](int row, int col) { return s.is_cell_selected(row, col); };
                cs.theme          = t;
                chainEditor_.draw(c, moduleX, EDITOR_Y, cs);
                break;
            }

            case ScreenType::SONG: {
                SongEditorState ss{p};
                ss.cursorRow      = s.cursorRow;
                ss.cursorTrack    = s.cursorColumn;  // on SONG the cursor column IS the track (1..8)
                ss.scrollPosition = s.songScrollPosition;
                ss.isPlaying      = s.isPlaying;
                ss.playbackRow    = s.playbackSongRow;
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
                ts.playbackRow  = s.tablePlaybackRow;
                // The tic rate is the INSTRUMENT's, not the table's — the same table run by two
                // instruments runs at two speeds, and this shows the one you are looking through.
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
                gs.cursorColumn = 1;  // the tick column is the only editable one
                gs.theme        = t;
                grooveModule_.draw(c, moduleX, EDITOR_Y, gs);
                break;
            }

            case ScreenType::INSTRUMENT: {
                InstrumentEditorState is{p.instruments[static_cast<size_t>(s.currentInstrument)]};
                is.cursorRow     = s.instrumentCursorRow;
                is.cursorColumn  = s.instrumentCursorColumn;
                // The SF2's preset list — the engine's answer, read back once a frame (engine_feed.h).
                // Zeroes and "---" with no engine, which is exactly what lets ptshot draw this screen.
                is.sfPresetName  = s.sfPresetName;
                is.sfPresetCount = s.sfPresetCount;
                is.sfPresetIndex = s.sfPresetIndex;
                is.theme         = t;
                instrumentEditor_.draw(c, moduleX, EDITOR_Y, is);
                break;
            }

            case ScreenType::INST_POOL: {
                InstrumentPoolState ps{p};
                // Its cursor ROW is the selected instrument itself — the pool is a navigator, not a
                // table with a cursor of its own.
                ps.selectedInstrument = s.currentInstrument;
                ps.cursorColumn       = s.poolCursorColumn;
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
                // The engine's meters, as the feed last read them (ui/engine_feed.h). All zeroes with
                // no engine — which is silence, and exactly what lets `ptshot` draw this screen.
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

            default:
                draw_placeholder(c, moduleX, EDITOR_Y, s.currentScreen, t);
                break;
        }
    }

    // ── The right bar ────────────────────────────────────────────────────────────────────────────
    // Hidden on the screens that take the whole 640×480 for themselves. (FILE_BROWSER returned long
    // before this line; SETTINGS and SAMPLE_EDITOR still route through the placeholder.)
    if (s.currentScreen != ScreenType::SETTINGS && s.currentScreen != ScreenType::SAMPLE_EDITOR) {
        draw_right_bar(c, s);
    }

    // ── The overlays ─────────────────────────────────────────────────────────────────────────────
    // LAST, over everything, including the right bar — an overlay is modal, and its backdrop dims the
    // whole frame. (The EQ editor and the theme editor join them here as they land.)
    draw_fx_helper(c, s.fxHelper, t);
    if (s.qwerty.isOpen) qwerty_.draw(c, s.qwerty, t);
}

void TrackerLayout::draw_right_bar(Canvas& c, const AppState& s) const {
    const Theme&             t = s.theme;
    const songcore::Project& p = *s.project;

    // The BPM row lines up with the COLUMN HEADER row of every editor, and is derived rather than
    // written down so it cannot drift from them: an editor lays out a title row (21px) then a 14px
    // spacer, putting its column headers at EDITOR_Y + 35 — which is 117.
    const int bpmRowY  = EDITOR_Y + ROW_HEIGHT + 14;  // 117
    const int bpmTextY = bpmRowY + TEXT_PADDING;      // 120

    c.draw_text("T>", RIGHT_BAR_X + 2, bpmTextY, t.textEmpty, CHAR_SPACING, FONT_SCALE);
    c.draw_text(std::to_string(p.tempo), RIGHT_BAR_X + 2 + 34, bpmTextY, t.textValue, CHAR_SPACING,
                FONT_SCALE);

    // The note monitor: one blank row below the BPM, then the 8 tracks. "1  C-4" — the track number
    // dim, the note bright while it sounds.
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
    // 620 wide, as the Kotlin has it — wider than the 510 the real editors use, and wider than the
    // clip, so "COMING SOON" sits slightly left of the visible centre. Faithfully odd: this is what
    // the Android app draws, and a placeholder is not the place to diverge from it.
    c.fill_rect(x, y, OscilloscopeModule::WIDTH, 392, t.background);

    c.draw_text(screen_label(screen), x + 20, y + TEXT_PADDING, t.textTitle, CHAR_SPACING,
                FONT_SCALE);

    const std::string message = "COMING SOON";
    const int         msgW    = Canvas::text_width(message, CHAR_SPACING, FONT_SCALE);
    c.draw_text(message, x + (OscilloscopeModule::WIDTH - msgW) / 2, y + 180, t.textEmpty,
                CHAR_SPACING, FONT_SCALE);
}

}  // namespace pt::ui
