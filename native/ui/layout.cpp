#include "ui/layout.h"

#include <string>

#include "ui/clipboard.h"
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

    // ── The SAMPLE EDITOR is full-screen too, and for the same reason ────────────────────────────
    //
    // `SampleEditorModule.height = 480`. A waveform wants every pixel of the width, and the two things
    // the right bar shows — the BPM and the eight tracks' notes — have nothing to say about a sample
    // being trimmed while the transport is stopped. The keyboard draws over it (A on the NAME row).
    //
    // ⚠️ `&& !s.eq.isOpen` — the EQ editor opened from the sample editor's FX row REPLACES it rather
    // than covering it, and the frame goes back to the normal furniture (scope strip, right bar). That
    // is Kotlin's, at PixelPerfectRenderer:474, and it is the right call: the EQ is 495×392 and would
    // sit in a 640×480 waveform's middle like a dialog nobody asked for.
    if (s.currentScreen == ScreenType::SAMPLE_EDITOR && !s.eq.isOpen) {
        sampleEditor_.draw(c, 0, 0, s.sampleEditor, t);
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

        // ── The EQ EDITOR takes the editor's place, and leaves the furniture alone ───────────────
        //
        // Not a dialog over the screen: it REPLACES the module, inside the same clip, at the same
        // origin — and the oscilloscope, the BPM, the note monitor and the nav map all keep drawing
        // around it. That is deliberate on both platforms. An EQ is dialled WHILE a note rings, and the
        // note monitor is how you see that it still is; a full-screen editor would hide the one readout
        // that tells you whether you are listening to anything at all.
        if (s.eq.isOpen) {
            EqState es{p};
            es.slotIndex     = s.eq.slotIndex;
            es.cursorRow     = s.eq.cursorRow;
            es.caller        = s.eq.caller;
            es.spectrum      = s.eqSpectrum;
            es.spectrumCount = s.eqSpectrumCount;
            es.theme         = t;
            eq_.draw(c, moduleX, EDITOR_Y, es);
        } else if (s.themeEditor.isOpen) {
            // ── The THEME EDITOR takes the editor's place, on the same terms as the EQ (S9) ──────
            //
            // Same origin, same clip — and the OSCILLOSCOPE STRIP above it keeps drawing, which is not a
            // courtesy but the point. Three of the seventeen colours (VIZ BG, VIZ LINE, VIZ WAVE) are the
            // strip, and START passes straight through this overlay to the transport, so you dial the
            // waveform's colour against a moving waveform. Hide the strip and VIZ WAVE is a number.
            //
            // ⚠️ The RIGHT BAR is absent, and that is SETTINGS' doing, not this overlay's: `draw` skips
            // it whenever `currentScreen` is SETTINGS (Kotlin: PixelPerfectRenderer:801, same list), and
            // SETTINGS is what the editor is raised from. So the meter colours (MTR *) are the three the
            // editor CANNOT show you in situ — the meters live on MIXER. Nothing to fix; just the honest
            // limit of a 510px panel that has taken the editor's place.
            ThemeState ts;
            ts.theme  = t;
            ts.editor = s.themeEditor;
            themeEditor_.draw(c, moduleX, EDITOR_Y, ts);
        } else switch (s.currentScreen) {   // the overlay is drawn INSTEAD of `currentScreen`
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

            case ScreenType::PROJECT: {
                ProjectState prs{p};
                prs.cursorRow      = s.projectCursorRow;
                prs.cursorColumn   = s.projectCursorColumn;
                prs.isRendering    = s.isRendering;
                prs.renderProgress = s.renderProgress;
                prs.sampleRamBytes = s.sampleRamBytes;
                prs.caps           = s.caps;
                prs.theme          = t;
                project_.draw(c, moduleX, EDITOR_Y, prs);
                break;
            }

            case ScreenType::SETTINGS: {
                SettingsState ss{s.settings};
                ss.cursorRow    = s.settingsCursorRow;
                ss.cursorColumn = s.settingsCursorColumn;
                // The display strings for the DEVICE rows. Empty on the shell, which does not draw
                // them — the module edits indices; only the platform can name what an index means.
                ss.layoutText   = s.layoutText;
                ss.skinText     = s.skinText;
                ss.overlayText  = s.overlayText;
                ss.themeName    = t.name;
                ss.caps         = s.caps;
                ss.theme        = t;
                settings_.draw(c, moduleX, EDITOR_Y, ss);
                break;
            }

            default:
                draw_placeholder(c, moduleX, EDITOR_Y, s.currentScreen, t);
                break;
        }
    }

    // ── The right bar ────────────────────────────────────────────────────────────────────────────
    //
    // Hidden on SETTINGS — which is NOT because it is full-screen (it is a 510×392 panel like every
    // other editor) but because Android hides it there too (PixelPerfectRenderer:801). A settings
    // panel has no playhead and no notes to monitor; the eight-track readout beside it would be
    // reporting on a song nobody is looking at.
    if (s.currentScreen != ScreenType::SETTINGS) draw_right_bar(c, s);

    // ── The status line, and the selection/clipboard readout ──────────────────────────────────────
    // Both sit over the scope strip so every screen can report: the status message top-LEFT, the
    // selection scope + clipboard contents top-RIGHT. See each method — and the bug that four sessions
    // of this port shipped without EITHER (the readouts were computed and drawn nowhere).
    draw_status_line(c, s);
    draw_selection_clipboard(c, s);

    // ── The overlays ─────────────────────────────────────────────────────────────────────────────
    // LAST, over everything, including the right bar and the status line — an overlay is modal, and
    // its backdrop dims the whole frame. (The EQ editor and the theme editor join them here.)
    draw_fx_helper(c, s.fxHelper, t);
    if (s.qwerty.isOpen) qwerty_.draw(c, s.qwerty, t);
    draw_confirm_dialog(c, s.confirm, t);
}

// ─── The global status line ──────────────────────────────────────────────────────────────────────
//
// "SAVED" · "EXPORTED!" · "SEQ CLEANED" · "CHAIN CLONED" · "NO FREE PHRASES". Drawn over the
// oscilloscope strip's top-left corner, which is Kotlin's placement (PixelPerfectRenderer:444) and
// costs no editor row — the point being that an action on ANY screen can report back.
//
// ⚠️ THE PORT HAS BEEN SETTING THIS SINCE S3 AND DRAWING IT NEVER. `AppState::statusMessage` has 22
// writers in the dispatcher — every clone, every failed insert — and until this function existed not
// one of them was visible. It survived because the screens that had landed all showed their result
// in the grid itself: clone a chain and you can SEE the chain. PROJECT is the first screen where the
// message IS the result — a SAVE looks exactly like a failed save without it — which is why S7 is
// the session that found it.
//
// Full-screen editors (the browser, the sample editor) draw over this area and keep their own inline
// status lines; both return long before this call.
void TrackerLayout::draw_status_line(Canvas& c, const AppState& s) const {
    if (s.statusMessage.empty()) return;

    // `statusMessage.take(34)` — a longer message is cut, not wrapped: the strip is one line high.
    const std::string text = s.statusMessage.substr(0, 34);
    const Argb        color = s.statusSuccess ? s.theme.vizWave : 0xFFFF0000;
    c.draw_text(text, SIDE_SPACER + 10, SCREEN_SPACER + 10, color, CHAR_SPACING, FONT_SCALE);
}

// ─── The selection scope and the clipboard, top-right of the scope strip ───────────────────────────
//
// A direct port of PixelPerfectRenderer.kt:407-438. Two stacked lines in the scope strip's top-right
// corner (the status line owns the top-left): the LIVE selection scope — "SEL:CELL" / "SEL:ROW" /
// "SEL:ALL", in vizWave green — and, below it, the CLIPBOARD's contents — "PHR:2x3" / "SNG:4x1" / …, in
// textTitle. Same right-edge inset (WIDTH − 150), same 21px stack gap, same two colours as the Kotlin.
//
// ⚠️ THE PORT COMPUTED BOTH STRINGS AND DREW NEITHER. `Selection::info()` and `Clipboard::info()` — and
// the dispatcher's `clipboard()` accessor, whose own comment says it is "for the top-strip readout" —
// were dead seams: defined, zero callers. It is the exact shape of the status line beside it, which was
// "set since S3 and drawn never" until S7 found it; this is the same miss, one readout over.
//
// ⚠️ The clipboard is reached through AppState's pointer, NULL under a tool with no dispatcher (ptshot).
// Then only the selection half can appear — which is correct, because with no dispatcher there is no
// clipboard, and `s.selection` is the tool's own to set (`--selection`).
void TrackerLayout::draw_selection_clipboard(Canvas& c, const AppState& s) const {
    const std::string sel  = s.selection.info();
    const std::string clip = s.clipboard ? s.clipboard->info() : std::string();
    if (sel.empty() && clip.empty()) return;

    // moduleX + WIDTH − 150, i.e. 150px in from the scope strip's right edge (Kotlin: moduleX + 620 - 150).
    const int x = SIDE_SPACER + OscilloscopeModule::WIDTH - 150;
    const int y = SCREEN_SPACER + 10;

    if (!sel.empty())
        c.draw_text(sel, x, y, s.theme.vizWave, CHAR_SPACING, FONT_SCALE);
    if (!clip.empty())
        // Below the selection line when both are up, else in its place — Kotlin's `clipY`.
        c.draw_text(clip, x, sel.empty() ? y : y + 21, s.theme.textTitle, CHAR_SPACING, FONT_SCALE);
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
