package com.conanizer.pockettracker.ui

import android.util.Log
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.graphics.FilterQuality
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.Paint
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.graphics.drawscope.drawIntoCanvas
import androidx.compose.ui.graphics.drawscope.translate
import com.conanizer.pockettracker.platform.android.DeviceAdapter
import kotlin.math.abs
import kotlin.math.min
import kotlinx.coroutines.delay
import com.conanizer.pockettracker.core.audio.AudioEngine
import com.conanizer.pockettracker.core.data.Note
import com.conanizer.pockettracker.core.data.Project
import com.conanizer.pockettracker.core.data.ScreenType
import com.conanizer.pockettracker.core.logic.EffectProcessor
import com.conanizer.pockettracker.core.logic.PlaybackController
import com.conanizer.pockettracker.ui.modules.ChainEditorModule
import com.conanizer.pockettracker.ui.modules.ChainEditorState
import com.conanizer.pockettracker.ui.modules.EffectModule
import com.conanizer.pockettracker.ui.modules.EffectState
import com.conanizer.pockettracker.ui.modules.EqModule
import com.conanizer.pockettracker.ui.modules.EqState
import com.conanizer.pockettracker.ui.modules.FileBrowserModule
import com.conanizer.pockettracker.ui.modules.GrooveModule
import com.conanizer.pockettracker.ui.modules.GrooveState
import com.conanizer.pockettracker.ui.modules.InstrumentModule
import com.conanizer.pockettracker.ui.modules.InstrumentState
import com.conanizer.pockettracker.ui.modules.MixerModule
import com.conanizer.pockettracker.ui.modules.MixerState
import com.conanizer.pockettracker.ui.modules.ModulationModule
import com.conanizer.pockettracker.ui.modules.ModulationState
import com.conanizer.pockettracker.ui.modules.NavigationMapModule
import com.conanizer.pockettracker.ui.modules.NavigationMapState
import com.conanizer.pockettracker.ui.modules.OscilloscopeModule
import com.conanizer.pockettracker.ui.modules.OscilloscopeState
import com.conanizer.pockettracker.ui.modules.PhraseEditorModule
import com.conanizer.pockettracker.ui.modules.PhraseEditorState
import com.conanizer.pockettracker.ui.modules.ProjectModule
import com.conanizer.pockettracker.ui.modules.ProjectState
import com.conanizer.pockettracker.ui.modules.SampleEditorModule
import com.conanizer.pockettracker.ui.modules.SampleEditorState
import com.conanizer.pockettracker.ui.modules.SettingsModule
import com.conanizer.pockettracker.ui.modules.SettingsState
import com.conanizer.pockettracker.ui.modules.SongEditorModule
import com.conanizer.pockettracker.ui.modules.SongEditorState
import com.conanizer.pockettracker.ui.modules.TableModule
import com.conanizer.pockettracker.ui.modules.TableState
import com.conanizer.pockettracker.ui.modules.ThemeEditorDrawState
import com.conanizer.pockettracker.ui.modules.ThemeEditorModule
import com.conanizer.pockettracker.ui.modules.ThemeEditorState
import com.conanizer.pockettracker.ui.overlays.EqEditorState
import com.conanizer.pockettracker.ui.overlays.FxHelperState
import com.conanizer.pockettracker.ui.overlays.QwertyKeyboardState
import com.conanizer.pockettracker.ui.overlays.descriptionLines
import com.conanizer.pockettracker.ui.overlays.qwertyRowsForLayout
import com.conanizer.pockettracker.ui.theme.AppTheme
import com.conanizer.pockettracker.ui.theme.VisualizerType
import kotlin.text.iterator

/**
 * PIXEL-PERFECT TRACKER - MODULAR VERSION
 *
 * Screen: 640×480 (4:3 handheld)
 * Font: 15×15 pixels (5×5 bitmap × 3)
 * Rows: 21px each
 *
 * Modular architecture:
 * - OscilloscopeModule (620×70)
 * - PhraseEditorModule (620×392)
 * - NavigationMapModule (80×105) - NEW!
 * - Easy to add more modules!
 */

// Design constants
const val DESIGN_WIDTH_PX = 640

// 6.1: below this peak amplitude the captured scope is treated as flat (silent), so the
// refresh loop stops forcing redraws. ~ -54 dBFS — visually indistinguishable from a flat line.
private const val SCOPE_SILENCE_THRESHOLD = 0.002f

/**
 * CompositionLocal that carries the current LayoutMode down the composition tree.
 * Set via CompositionLocalProvider in PocketTrackerApp; read in PixelPerfectTracker
 * so it reaches drawLayout without threading through every intermediate composable.
 */
val LocalLayoutMode = compositionLocalOf { DeviceAdapter.LayoutMode.FULL }

/**
 * CompositionLocal that carries the active AppTheme down the composition tree.
 * Set via CompositionLocalProvider in PocketTrackerApp; read in PixelPerfectTracker
 * and forwarded to drawLayout so all modules can access theme colors.
 */
val LocalAppTheme = compositionLocalOf { AppTheme.CLASSIC }
const val DESIGN_HEIGHT_PX = 480
const val SCREEN_SPACER = 6      // Space between modules
const val SIDE_SPACER = 10       // Space on sides

@Composable
fun PixelPerfectTracker(
    currentScreen: ScreenType,
    project: Project,
    audioEngine: AudioEngine,
    playbackController: PlaybackController,
    cursorRow: Int,
    cursorColumn: Int,
    isPlaying: Boolean,
    previousColumn: Int,
    currentChain: Int,
    currentPhrase: Int,
    projectCursorRow: Int,
    projectCursorColumn: Int,
    projectStatusMessage: String,
    projectStatusSuccess: Boolean,
    projectVersion: Int,
    currentInstrument: Int,
    instrumentCursorRow: Int,
    instrumentCursorColumn: Int,
    instrumentStatusMessage: String,
    instrumentStatusSuccess: Boolean,
    fileBrowserState: FileBrowserModule.State? = null,
    sampleEditorState: SampleEditorState? = null,
    // Copy/paste state
    selectionInfo: String = "",        // e.g., "SEL:CELL", "SEL:ROW", "SEL:ALL"
    clipboardInfo: String = "",        // e.g., "PHR:3x4", "CHN:1x8"
    selectionMode: Boolean = false,    // Whether selection mode is active
    isCellSelected: (Int, Int) -> Boolean = { _, _ -> false },  // Check if cell is selected
    // Mixer state
    mixerCursorColumn: Int = 0,        // 0-7 = tracks, 8 = master
    mixerMasterRow: Int = 0,           // 0 = volume row, 1 = OTT row
    trackPeaks: FloatArray = FloatArray(8),
    masterPeaks: FloatArray = FloatArray(2),
    sendPeaks: FloatArray = FloatArray(4),
    // Table state
    currentTable: Int = 0,
    tableCursorRow: Int = 0,
    tableCursorColumn: Int = 1,
    // Groove state
    currentGroove: Int = 0,
    grooveCursorRow: Int = 0,
    // Modulation state
    modCursorRow: Int = 0,
    modCursorPair: Int = 0,
    modCursorSide: Int = 0,
    // Effects screen cursor
    effectsCursorRow: Int = 0,
    // Render state (WAV export)
    isRendering: Boolean = false,
    renderProgress: Float = 0f,
    // Clean dialog state
    showCleanDialog: Boolean = false,
    cleanDialogTarget: String = "",  // "SEQ" or "INST"
    cleanDialogCursor: Int = 0,      // 0 = YES, 1 = NO
    // Simple confirm dialogs
    showNewProjectDialog: Boolean = false,
    showInstrTypeDialog: Boolean = false,
    showRecoveryDialog: Boolean = false,
    // Song scroll position
    songScrollPosition: Int = 0,
    // Scaling mode (for project screen display)
    scalingMode: DeviceAdapter.ScalingMode = DeviceAdapter.ScalingMode.INTEGER,
    buttonSoundEnabled: Boolean = false,
    buttonSoundVolume: Int = 255,
    buttonVibroEnabled: Boolean = false,
    vibroPower: Int = 255,
    // QWERTY keyboard overlay state
    qwertyKeyboardState: QwertyKeyboardState = QwertyKeyboardState(),
    // FX helper overlay state
    fxHelperState: FxHelperState = FxHelperState(),
    // EQ editor overlay state
    eqEditorState: EqEditorState = EqEditorState(),
    eqSpectrumData: FloatArray? = null,
    // Theme editor overlay state
    themeEditorState: ThemeEditorState = ThemeEditorState(),
    // Settings screen cursor
    settingsCursorRow: Int = 0,
    settingsCursorColumn: Int = 1,
    // Cursor remember setting (for settings screen display)
    cursorRemember: Boolean = false,
    // Note preview setting (for settings screen display)
    notePreviewEnabled: Boolean = true,
    // SoundFont preset navigation state
    soundfontPresetName: String = "",
    soundfontPresetCount: Int = 0,
    soundfontPresetIndex: Int = 0,
    // Overlay settings (for settings screen display)
    overlayFiles: List<String> = emptyList(),
    overlayName: String = "OFF",
    overlayStrength: Int = 128
) {
    // Playback state
    var playbackRow by remember { mutableStateOf(0) }
    var playbackChainRow by remember { mutableStateOf(0) }
    var playbackPhraseStep by remember { mutableStateOf(0) }
    var playbackSongRow by remember { mutableStateOf(0) }
    var trackNotes by remember { mutableStateOf(List(8) { Note.EMPTY }) }

    // Oscilloscope refresh ticker. Reading this inside the Canvas draw lambda forces a redraw;
    // the loop below only bumps it while something is actually animating (see 6.1).
    var oscilloscopeTicker by remember { mutableStateOf(0L) }

    // Read fresh inside the long-lived loop without restarting it (LaunchedEffect key stays Unit).
    val appTheme = LocalAppTheme.current
    val currentIsPlaying by rememberUpdatedState(isPlaying)
    val currentVizType by rememberUpdatedState(appTheme.visualizerType)

    // Oscilloscope / visualizer refresh loop (independent of playback position).
    // 6.1: a single Canvas can only redraw all-or-nothing, so reading the ticker here used to
    // repaint the entire 640×480 layout 60×/sec FOREVER — even sitting idle on a static phrase
    // grid, which the review flagged as the dominant battery drain on the handheld. Now the loop
    // refreshes the capture buffers and only bumps the ticker (→ redraw) while audio is audible
    // (song/phrase playing OR a one-shot preview still ringing). When idle it keeps polling cheaply
    // for audio onset but does NOT bump the ticker, so the Canvas repaints only on real state
    // changes (cursor move, value edit, playback row). One final bump on the active→idle edge draws
    // the flattened scope, then full-screen redraws stop entirely.
    LaunchedEffect(Unit) {
        var wasAudible = true
        while (true) {
            audioEngine.updateWaveformWithDecay(currentIsPlaying)
            if (currentVizType == VisualizerType.OCTA) audioEngine.updateTrackWaveforms()
            if (currentVizType == VisualizerType.SPECTRUM ||
                currentVizType == VisualizerType.SPECTRUM_PEAKS) audioEngine.updateSpectrum()

            val audible = currentIsPlaying ||
                audioEngine.waveformBuffer.any { abs(it) > SCOPE_SILENCE_THRESHOLD }
            if (audible) {
                oscilloscopeTicker++
                wasAudible = true
                delay(16L)            // ~60 fps while something moves
            } else {
                if (wasAudible) {     // active → idle edge: draw the now-flat scope one last time
                    oscilloscopeTicker++
                    wasAudible = false
                }
                delay(50L)            // idle: poll for audio onset, no ticker bump → no redraw
            }
        }
    }

    // Playback position update loop
    // This is SIMPLIFIED: all scheduling logic moved to PlaybackController.updatePlaybackBuffer()
    // When project data changes during playback, reschedule immediately so edits are
    // heard on the next phrase loop rather than 2-3 phrases later.
    // LaunchedEffect(projectVersion) restarts every time the version changes, so we
    // always read fresh values of isPlaying and projectVersion.
    LaunchedEffect(projectVersion) {
        if (isPlaying) {
            playbackController.notifyDataChanged()
        }
    }

    // Playback position update loop
    // This is SIMPLIFIED: all scheduling logic moved to PlaybackController.updatePlaybackBuffer()
    LaunchedEffect(isPlaying, currentScreen) {
        if (isPlaying) {
            trackNotes = List(8) { Note.EMPTY }  // Clear stale notes at start of new playback
            while (isPlaying) {
                // Update lookahead buffer - PlaybackController handles all scheduling
                playbackController.updatePlaybackBuffer()

                // Get current playback position for UI updates
                val position = playbackController.getPlaybackPosition()
                playbackRow = position.row
                playbackChainRow = position.chainRow
                playbackPhraseStep = position.phraseStep
                playbackSongRow = position.songRow
                // Derive notes from actual playback position (not schedule-ahead trackStates)
                trackNotes = playbackController.getCurrentPlayingNotes()

                // Update UI at 60 Hz
                delay(16L)
            }
        } else {
            // isPlaying just became false — clear monitor immediately
            // (can't do this after the while loop because LaunchedEffect cancels the old
            // coroutine when its keys change, so code after the loop never executes)
            trackNotes = List(8) { Note.EMPTY }
        }
    }

    // NOTE: BoxWithConstraints intentionally replaced with a single Canvas.
    // BoxWithConstraints uses SubcomposeLayout internally, which can destroy+recreate
    // child composables (including Canvas) during remeasure, causing SEGV_ACCERR in
    // RenderThread. Using a single Canvas with DrawScope.size is both simpler and safe:
    // the Canvas node is never destroyed, scale is computed in the draw phase.
    //
    // key() also intentionally ABSENT — same reason (render node destruction race).
    // 60fps oscilloscope: oscilloscopeTicker read inside draw lambda → snapshot observer
    // re-invokes only the draw phase, no full recomposition, no node destruction.
    //
    // TrackerLayout created with remember{} (NOT inside the draw lambda).
    // The draw lambda re-runs 60fps due to oscilloscopeTicker; allocating 12 module
    // objects per frame causes GC pressure that can crash the RenderThread on Android 11
    // (Snapdragon GPU drivers can't safely be paused mid-frame by the JVM GC).
    val layoutMode = LocalLayoutMode.current
    val layout = remember { TrackerLayout() }
    Canvas(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black)
    ) {
        // Compute integer scale from DrawScope.size (available in draw phase, no BoxWithConstraints needed).
        // Add 1px tolerance before dividing: dp↔px float conversion can lose up to 1px
        // (e.g. 1280px → 487.619dp → 1279.999px → 1279), which would drop scale by one level.
        val screenWidthPx = size.width.toInt()
        val screenHeightPx = size.height.toInt()
        val scaleX = (screenWidthPx + 1) / DESIGN_WIDTH_PX
        val scaleY = (screenHeightPx + 1) / DESIGN_HEIGHT_PX
        val scale = min(scaleX, scaleY).coerceAtLeast(1)
        val renderWidth = DESIGN_WIDTH_PX * scale
        val renderHeight = DESIGN_HEIGHT_PX * scale
        // Clamp to 0: if canvas is 1px narrower than renderWidth (due to rounding), avoid negative offset
        val offsetX = maxOf(0f, (screenWidthPx - renderWidth) / 2f)
        val offsetY = maxOf(0f, (screenHeightPx - renderHeight) / 2f)

        translate(offsetX, offsetY) {
            with(layout) {
                drawLayout(
                        scale = scale,
                        oscilloscopeTicker = oscilloscopeTicker,
                        currentScreen = currentScreen,
                        project = project,
                        cursorRow = cursorRow,
                        cursorColumn = cursorColumn,
                        isPlaying = isPlaying,
                        playbackRow = playbackRow,
                        playbackChainRow = playbackChainRow,
                        playbackSongRow = playbackSongRow,
                        audioEngine = audioEngine,
                        previousColumn = previousColumn,
                        currentChain = currentChain,
                        currentPhrase = currentPhrase,
                        projectCursorRow = projectCursorRow,
                        projectCursorColumn = projectCursorColumn,
                        projectStatusMessage = projectStatusMessage,
                        projectStatusSuccess = projectStatusSuccess,
                        projectVersion = projectVersion,
                        currentInstrument = currentInstrument,
                        instrumentCursorRow = instrumentCursorRow,
                        instrumentCursorColumn = instrumentCursorColumn,
                        instrumentStatusMessage = instrumentStatusMessage,
                        instrumentStatusSuccess = instrumentStatusSuccess,
                        fileBrowserState = fileBrowserState,
                        sampleEditorState = sampleEditorState,
                        selectionInfo = selectionInfo,
                        clipboardInfo = clipboardInfo,
                        selectionMode = selectionMode,
                        isCellSelected = isCellSelected,
                        mixerCursorColumn = mixerCursorColumn,
                        mixerMasterRow = mixerMasterRow,
                        trackPeaks = trackPeaks,
                        masterPeaks = masterPeaks,
                        sendPeaks = sendPeaks,
                        currentTable = currentTable,
                        tableCursorRow = tableCursorRow,
                        tableCursorColumn = tableCursorColumn,
                        currentGroove = currentGroove,
                        grooveCursorRow = grooveCursorRow,
                        modCursorRow = modCursorRow,
                        modCursorPair = modCursorPair,
                        modCursorSide = modCursorSide,
                        effectsCursorRow = effectsCursorRow,
                        isRendering = isRendering,
                        renderProgress = renderProgress,
                        showCleanDialog = showCleanDialog,
                        cleanDialogTarget = cleanDialogTarget,
                        cleanDialogCursor = cleanDialogCursor,
                        showNewProjectDialog = showNewProjectDialog,
                        showInstrTypeDialog = showInstrTypeDialog,
                        showRecoveryDialog = showRecoveryDialog,
                        layoutMode = layoutMode,
                        songScrollPosition = songScrollPosition,
                        scalingMode = scalingMode,
                        buttonSoundEnabled = buttonSoundEnabled,
                        buttonSoundVolume = buttonSoundVolume,
                        buttonVibroEnabled = buttonVibroEnabled,
                        vibroPower = vibroPower,
                        qwertyKeyboardState = qwertyKeyboardState,
                        fxHelperState = fxHelperState,
                        eqEditorState = eqEditorState,
                        eqSpectrumData = eqSpectrumData,
                        settingsCursorRow = settingsCursorRow,
                        settingsCursorColumn = settingsCursorColumn,
                        cursorRemember = cursorRemember,
                        notePreviewEnabled = notePreviewEnabled,
                        trackNotes = trackNotes,
                        soundfontPresetName  = soundfontPresetName,
                        soundfontPresetCount = soundfontPresetCount,
                        soundfontPresetIndex = soundfontPresetIndex,
                        appTheme             = appTheme,
                        themeEditorState     = themeEditorState,
                        overlayFiles         = overlayFiles,
                        overlayName          = overlayName,
                        overlayStrength      = overlayStrength
                    )
                }
            }
        }
}

/**
 * LAYOUT MANAGER
 *
 * Positions and draws all modules
 * This is where you "build" your screen layout!
 */
class TrackerLayout {
    // ===================================
    // CREATE MODULE INSTANCES
    // ===================================
    private val oscilloscope = OscilloscopeModule(width = 620, height = 70)
    private val phraseEditor = PhraseEditorModule()
    private val navigationMap = NavigationMapModule()
    private val instrumentModule = InstrumentModule()
    private val mixerModule = MixerModule()
    private val chainEditor = ChainEditorModule()
    private val songEditor = SongEditorModule()
    private val projectModule = ProjectModule()
    private val fileBrowser = FileBrowserModule()
    private val sampleEditorModule = SampleEditorModule()
    private val tableModule = TableModule()
    private val grooveModule = GrooveModule()
    private val modulationModule = ModulationModule()
    private val settingsModule = SettingsModule()
    private val effectModule = EffectModule()
    private val eqModule     = EqModule()
    private val themeEditorModule = ThemeEditorModule()
    /**
     * Main layout drawing function
     * This arranges all modules on the 640×480 screen
     */
    fun DrawScope.drawLayout(
        scale: Int,
        oscilloscopeTicker: Long = 0L,  // Read in draw scope → Canvas redraws at 60fps for oscilloscope
        currentScreen: ScreenType,
        project: Project,
        cursorRow: Int,
        cursorColumn: Int,
        isPlaying: Boolean,
        playbackRow: Int,
        playbackChainRow: Int,
        playbackSongRow: Int,
        audioEngine: AudioEngine,
        previousColumn: Int,
        currentChain: Int,
        currentPhrase: Int = 0,
        projectCursorRow: Int = 0,
        projectCursorColumn: Int = 1,
        projectStatusMessage: String = "",
        projectStatusSuccess: Boolean = true,
        projectVersion: Int = 0,  // Version counter to force redraw on data changes
        currentInstrument: Int = 0,
        instrumentCursorRow: Int = 0,
        instrumentCursorColumn: Int = 1,
        instrumentStatusMessage: String = "",
        instrumentStatusSuccess: Boolean = true,
        fileBrowserState: FileBrowserModule.State? = null,  // File browser state
        sampleEditorState: SampleEditorState? = null,
        // Copy/paste state
        selectionInfo: String = "",
        clipboardInfo: String = "",
        selectionMode: Boolean = false,
        isCellSelected: (Int, Int) -> Boolean = { _, _ -> false },
        // Mixer state
        mixerCursorColumn: Int = 0,
        mixerMasterRow: Int = 0,
        trackPeaks: FloatArray = FloatArray(8),
        masterPeaks: FloatArray = FloatArray(2),
        sendPeaks: FloatArray = FloatArray(4),
        // Table state
        currentTable: Int = 0,
        tableCursorRow: Int = 0,
        tableCursorColumn: Int = 1,
        // Groove state
        currentGroove: Int = 0,
        grooveCursorRow: Int = 0,
        // Modulation state
        modCursorRow: Int = 0,
        modCursorPair: Int = 0,
        modCursorSide: Int = 0,
        // Effects screen cursor
        effectsCursorRow: Int = 0,
        // Render state (WAV export)
        isRendering: Boolean = false,
        renderProgress: Float = 0f,
        // Clean dialog state
        showCleanDialog: Boolean = false,
        cleanDialogTarget: String = "",  // "SEQ" or "INST"
        cleanDialogCursor: Int = 0,      // 0 = YES, 1 = NO
        // Simple confirm dialogs (A=YES, B=NO)
        showNewProjectDialog: Boolean = false,
        showInstrTypeDialog: Boolean = false,
        showRecoveryDialog: Boolean = false,
        // Layout mode (from CompositionLocal, for display in project screen)
        layoutMode: DeviceAdapter.LayoutMode = DeviceAdapter.LayoutMode.FULL,
        // Song scroll position (viewport start row for 256-row song)
        songScrollPosition: Int = 0,
        // Scaling mode (for project screen display)
        scalingMode: DeviceAdapter.ScalingMode = DeviceAdapter.ScalingMode.INTEGER,
        buttonSoundEnabled: Boolean = false,
        buttonSoundVolume: Int = 255,
        buttonVibroEnabled: Boolean = false,
        vibroPower: Int = 255,
        // QWERTY keyboard overlay state
        qwertyKeyboardState: QwertyKeyboardState = QwertyKeyboardState(),
        // FX helper overlay state
        fxHelperState: FxHelperState = FxHelperState(),
        // EQ editor overlay state
        eqEditorState: EqEditorState = EqEditorState(),
        eqSpectrumData: FloatArray? = null,
        // Settings screen cursor
        settingsCursorRow: Int = 0,
        settingsCursorColumn: Int = 1,
        // Cursor remember setting (passed through to SettingsState for display)
        cursorRemember: Boolean = false,
        // Note preview setting (passed through to SettingsState for display)
        notePreviewEnabled: Boolean = true,
        // Track note monitor
        trackNotes: List<Note> = List(8) { Note.EMPTY },
        // SoundFont preset navigation state
        soundfontPresetName: String = "",
        soundfontPresetCount: Int = 0,
        soundfontPresetIndex: Int = 0,
        appTheme: AppTheme = AppTheme.CLASSIC,
        themeEditorState: ThemeEditorState = ThemeEditorState(),
        // Overlay settings (for settings screen display)
        overlayFiles: List<String> = emptyList(),
        overlayName: String = "OFF",
        overlayStrength: Int = 128
    ) {
        val t = appTheme
        // ===================================
        // DRAW BACKGROUND
        // ===================================
        drawRect(
            color = Color(t.background),
            topLeft = Offset.Zero,
            size = Size(
                (DESIGN_WIDTH_PX * scale).toFloat(),
                (DESIGN_HEIGHT_PX * scale).toFloat()
            )
        )

        // ===================================
        // LEFT SIDE: Main content modules (620px wide)
        // ===================================

        // Start position: 10px from left edge (centers 620px in 640px)
        val moduleX = SIDE_SPACER
        var currentY = SCREEN_SPACER  // Start 6px from top

        // Capture buffers (waveform / track waveforms / spectrum) are refreshed by the
        // oscilloscope ticker loop (6.1), not here — the draw only reads the latest snapshot.
        // We still need these flags to pick what to hand the oscilloscope module.
        val isOcta = appTheme.visualizerType == VisualizerType.OCTA
        val isSpectrum = appTheme.visualizerType == VisualizerType.SPECTRUM ||
                         appTheme.visualizerType == VisualizerType.SPECTRUM_PEAKS

        // MODULE 1: OSCILLOSCOPE (waveform display)
        // Position: Top of screen
        // Size: 620×70
        with(oscilloscope) {
            draw(
                x = moduleX,
                y = currentY,
                scale = scale,
                state = OscilloscopeState(
                    waveformBuffer = audioEngine.waveformBuffer,
                    appTheme = appTheme,
                    trackWaveforms = if (isOcta) audioEngine.trackWaveformBuffers else null,
                    // Tracks 0-7 from the scheduled-track mask; the preview lane (bit 8) only when
                    // stopped, so it never crowds the song scopes during playback.
                    activeTrackMask = if (isOcta) {
                        val base = audioEngine.phraseTrackMask and 0xFF
                        if (!isPlaying && audioEngine.previewLaneActive) base or (1 shl 8) else base
                    } else 0,
                    spectrumData = if (isSpectrum) audioEngine.spectrumBuffer else null
                )
            )
        }

        // Draw clipboard/selection indicator on the right side of oscilloscope
        if (selectionInfo.isNotEmpty() || clipboardInfo.isNotEmpty()) {
            val indicatorY = currentY + 10  // 10px from top
            val indicatorX = moduleX + 620 - 150  // Right-aligned within module

            // Show selection info (green)
            if (selectionInfo.isNotEmpty()) {
                drawBitmapText(
                    text = selectionInfo,
                    x = indicatorX,
                    y = indicatorY,
                    scale = scale,
                    color = Color(t.vizWave),
                    spacing = 2,
                    fontScale = 3
                )
            }

            // Show clipboard info below selection info
            if (clipboardInfo.isNotEmpty()) {
                val clipY = if (selectionInfo.isNotEmpty()) indicatorY + 21 else indicatorY
                drawBitmapText(
                    text = clipboardInfo,
                    x = indicatorX,
                    y = clipY,
                    scale = scale,
                    color = Color(t.textTitle),
                    spacing = 2,
                    fontScale = 3
                )
            }
        }

        // Move down for next module
        currentY += oscilloscope.height + SCREEN_SPACER  // 70 + 6 = 76px

        // MODULE 2: Switch between editors based on current screen.
        // FILE_BROWSER is full-screen and handled outside the clip region.
        // All other editors are clipped to the left of the right bar so that
        // row highlights / wide backgrounds don't bleed into the BPM display,
        // track note monitor, or navigation map.
        val editorClipRight = (DESIGN_WIDTH_PX - navigationMap.width - SIDE_SPACER - SCREEN_SPACER) * scale.toFloat()

        if (currentScreen == ScreenType.FILE_BROWSER) {
            if (fileBrowserState != null) {
                with(fileBrowser) {
                    draw(x = 0, y = 0, scale = scale, state = fileBrowserState.copy(appTheme = appTheme))
                }
            } else {
                Log.e("FileBrowser", "fileBrowserState is NULL - cannot render!")
            }
        } else if (currentScreen == ScreenType.SAMPLE_EDITOR && !eqEditorState.isOpen) {
            if (sampleEditorState != null) {
                with(sampleEditorModule) {
                    draw(x = 0, y = 0, scale = scale, state = sampleEditorState.copy(appTheme = appTheme))
                }
            }
        } else {
            clipRect(right = editorClipRight) {
                if (themeEditorState.isOpen) {
                    with(themeEditorModule) {
                        draw(
                            x = moduleX, y = currentY, scale = scale,
                            state = ThemeEditorDrawState(
                                theme = appTheme,
                                editorState = themeEditorState
                            )
                        )
                    }
                } else if (eqEditorState.isOpen) {
                    with(eqModule) {
                        draw(
                            x = moduleX, y = currentY, scale = scale,
                            state = EqState(
                                project = project,
                                slotIndex = eqEditorState.slotIndex,
                                cursorRow = eqEditorState.cursorRow,
                                callerContext = eqEditorState.callerContext,
                                spectrumData = eqSpectrumData,
                                appTheme = appTheme
                            )
                        )
                    }
                } else
                when (currentScreen) {
                    ScreenType.PROJECT -> {
                        with(projectModule) {
                            draw(
                                x = moduleX,
                                y = currentY,
                                scale = scale,
                                state = ProjectState(
                                    project = project,
                                    cursorRow = projectCursorRow,
                                    cursorColumn = projectCursorColumn,
                                    statusMessage = projectStatusMessage,
                                    isSuccess = projectStatusSuccess,
                                    isRendering = isRendering,
                                    renderProgress = renderProgress,
                                    appTheme = appTheme
                                )
                            )
                        }
                    }
                    // ===================================
                    // PHRASE SCREEN: Show phrase editor
                    // ===================================
                    ScreenType.PHRASE -> {
                        with(phraseEditor) {
                            draw(
                                x = moduleX,
                                y = currentY,
                                scale = scale,
                                state = PhraseEditorState(
                                    phrase = project.phrases[currentPhrase],
                                    cursorRow = cursorRow,
                                    cursorColumn = cursorColumn,
                                    playbackRow = playbackRow,
                                    isPlaying = isPlaying,
                                    selectionMode = selectionMode,
                                    isCellSelected = isCellSelected,
                                    appTheme = appTheme
                                )
                            )
                        }
                    }

                    // ===================================
                    // CHAIN SCREEN: Show chain editor
                    // ===================================
                    ScreenType.CHAIN -> {
                        with(chainEditor) {
                            draw(
                                x = moduleX,
                                y = currentY,
                                scale = scale,
                                state = ChainEditorState(
                                    chain = project.chains[currentChain],
                                    cursorRow = cursorRow,
                                    cursorColumn = cursorColumn,
                                    playbackRow = playbackChainRow,
                                    isPlaying = isPlaying,
                                    selectionMode = selectionMode,
                                    isCellSelected = isCellSelected,
                                    appTheme = appTheme
                                )
                            )
                        }
                    }

                    // ===================================
                    // SONG SCREEN: Show song editor
                    // ===================================
                    ScreenType.SONG -> {
                        with(songEditor) {
                            draw(
                                x = moduleX,
                                y = currentY,
                                scale = scale,
                                state = SongEditorState(
                                    project = project,
                                    cursorRow = cursorRow,
                                    cursorTrack = cursorColumn,
                                    isPlaying = isPlaying && currentScreen == ScreenType.SONG,
                                    playbackRow = playbackSongRow,
                                    selectionMode = selectionMode,
                                    isCellSelected = isCellSelected,
                                    scrollPosition = songScrollPosition,
                                    appTheme = appTheme
                                )
                            )
                        }
                    }

                    // ===================================
                    // INSTRUMENT SCREEN: Show instrument editor
                    // ===================================
                    ScreenType.INSTRUMENT -> {
                        with(instrumentModule) {
                            draw(
                                x = moduleX,
                                y = currentY,
                                scale = scale,
                                state = InstrumentState(
                                    instrument = project.instruments[currentInstrument],
                                    cursorRow = instrumentCursorRow,
                                    cursorColumn = instrumentCursorColumn,
                                    statusMessage = instrumentStatusMessage,
                                    isSuccess = instrumentStatusSuccess,
                                    soundfontPresetName = soundfontPresetName,
                                    soundfontPresetCount = soundfontPresetCount,
                                    soundfontPresetIndex = soundfontPresetIndex,
                                    appTheme = appTheme
                                )
                            )
                        }
                    }

                    // ===================================
                    // TABLE SCREEN: Show table editor
                    // ===================================
                    ScreenType.TABLE -> {
                        with(tableModule) {
                            // Find which track is playing with this table (if any)
                            var tablePlaybackRow: Int? = null
                            for (trackId in 0 until 8) {
                                if (audioEngine.getVoiceTableId(trackId) == currentTable) {
                                    val row = audioEngine.getVoiceTableRow(trackId)
                                    if (row >= 0) {
                                        tablePlaybackRow = row
                                        break
                                    }
                                }
                            }

                            draw(
                                x = moduleX,
                                y = currentY,
                                scale = scale,
                                state = TableState(
                                    table = project.tables[currentTable],
                                    cursorRow = tableCursorRow,
                                    cursorColumn = tableCursorColumn,
                                    playbackRow = tablePlaybackRow,
                                    ticRate = project.instruments.getOrNull(currentInstrument)?.tableTicRate
                                        ?: 0x06,
                                    selectionMode = selectionMode,
                                    isCellSelected = isCellSelected,
                                    appTheme = appTheme
                                )
                            )
                        }
                    }

                    // ===================================
                    // GROOVE SCREEN: Show groove pattern editor
                    // ===================================
                    ScreenType.GROOVE -> {
                        with(grooveModule) {
                            draw(
                                x = moduleX,
                                y = currentY,
                                scale = scale,
                                state = GrooveState(
                                    groove = project.grooves[currentGroove],
                                    cursorRow = grooveCursorRow,
                                    cursorColumn = 1,
                                    appTheme = appTheme
                                )
                            )
                        }
                    }

                    // ===================================
                    // MODS SCREEN: Show modulation editor
                    // ===================================
                    ScreenType.MODS -> {
                        with(modulationModule) {
                            draw(
                                x = moduleX,
                                y = currentY,
                                scale = scale,
                                state = ModulationState(
                                    instrument = project.instruments[currentInstrument],
                                    cursorRow = modCursorRow,
                                    cursorPair = modCursorPair,
                                    cursorSide = modCursorSide,
                                    appTheme = appTheme
                                )
                            )
                        }
                    }

                    // ===================================
                    // SETTINGS SCREEN: Show settings side menu
                    // ===================================
                    ScreenType.SETTINGS -> {
                        with(settingsModule) {
                            draw(
                                x = moduleX,
                                y = currentY,
                                scale = scale,
                                state = SettingsState(
                                    cursorRow = settingsCursorRow,
                                    cursorColumn = settingsCursorColumn,
                                    layoutMode = layoutMode,
                                    scalingMode = scalingMode,
                                    overlayFiles = overlayFiles,
                                    overlayName = overlayName,
                                    overlayStrength = overlayStrength,
                                    buttonSoundEnabled = buttonSoundEnabled,
                                    buttonSoundVolume = buttonSoundVolume,
                                    buttonVibroEnabled = buttonVibroEnabled,
                                    vibroPower = vibroPower,
                                    insertBefore = qwertyKeyboardState.insertBefore,
                                    cursorRemember = cursorRemember,
                                    notePreviewEnabled = notePreviewEnabled,
                                    visualizerType = t.visualizerType,
                                    currentThemeName = t.name,
                                    appTheme = appTheme
                                )
                            )
                        }
                    }

                    // ===================================
                    // MIXER SCREEN: Show mixer with 8 tracks + master
                    // ===================================
                    ScreenType.MIXER -> {
                        with(mixerModule) {
                            draw(
                                x = moduleX,
                                y = currentY,
                                scale = scale,
                                state = MixerState(
                                    project = project,
                                    cursorColumn = mixerCursorColumn,
                                    mixerMasterRow = mixerMasterRow,
                                    trackPeaks = trackPeaks,
                                    masterPeaks = masterPeaks,
                                    reverbPeaks = floatArrayOf(sendPeaks[0], sendPeaks[1]),
                                    delayPeaks = floatArrayOf(sendPeaks[2], sendPeaks[3]),
                                    appTheme = appTheme
                                )
                            )
                        }
                    }

                    // ===================================
                    // EFFECTS SCREEN: Reverb, delay, master EQ
                    // ===================================
                    ScreenType.EFFECTS -> {
                        with(effectModule) {
                            draw(
                                x = moduleX,
                                y = currentY,
                                scale = scale,
                                state = EffectState(
                                    project = project,
                                    cursorRow = effectsCursorRow,
                                    appTheme = appTheme
                                )
                            )
                        }
                    }

                    // ===================================
                    // OTHER SCREENS: Show placeholder
                    // ===================================
                    else -> {
                        drawPlaceholderScreen(
                            x = moduleX,
                            y = currentY,
                            scale = scale,
                            screenType = currentScreen,
                            t = appTheme
                        )
                    }
                }
            } // end clipRect
        } // end FILE_BROWSER if/else

        // Left side total: 6 + 70 + 6 + 392 + 6 = 480px ✓

        // ===================================
        // RIGHT SIDE: Navigation map (80px wide)
        // ===================================
        // Note: Hidden when FILE_BROWSER is active to give full screen space

        if (currentScreen != ScreenType.FILE_BROWSER && currentScreen != ScreenType.SETTINGS && currentScreen != ScreenType.SAMPLE_EDITOR) {
            val rightBarX = DESIGN_WIDTH_PX - navigationMap.width - SIDE_SPACER  // 515

            // BPM row aligns with column headers on all main editors:
            // editors lay out: header row (21px) + 14px spacer → column headers at y+35
            // Text within row is offset by TEXT_PADDING=3, so text at y+38 absolute = 82+35=117 row, 120 text
            val editorHeaderRowY = SCREEN_SPACER + oscilloscope.height + SCREEN_SPACER  // 82
            val bpmRowY = editorHeaderRowY + 21 + 14  // 117 — same as column header rows in editors
            val bpmTextY = bpmRowY + 3                // 120

            // ===================================
            // RIGHT BAR: BPM row
            // ===================================
            drawBitmapText("T>", rightBarX + 2, bpmTextY, scale, Color(t.textEmpty), spacing = 2, fontScale = 3)
            drawBitmapText(project.tempo.toString(), rightBarX + 2 + 34, bpmTextY, scale, Color(t.textValue), spacing = 2, fontScale = 3)

            // ===================================
            // RIGHT BAR: Track Note Monitor
            // One spacer row (21px) below BPM, then 8 track rows
            // Format: "1  C-4" — track num in gray, note in white/dim
            // ===================================
            val trackRowsStartY = bpmRowY + 21 + 21  // skip BPM row + one spacer row = 159
            for (i in 0..7) {
                val textY = trackRowsStartY + (i * 21) + 3
                val note = trackNotes.getOrElse(i) { Note.EMPTY }
                val noteColor = if (note == Note.EMPTY) Color(t.textEmpty) else Color(t.textValue)
                drawBitmapText((i + 1).toString(), rightBarX + 2, textY, scale, Color(t.textParam), spacing = 2, fontScale = 3)
                drawBitmapText(note.toString(), rightBarX + 2 + 34, textY, scale, noteColor, spacing = 2, fontScale = 3)
            }

            // ===================================
            // NAVIGATION MAP
            // ===================================
            val navMapX = rightBarX
            val navMapY = DESIGN_HEIGHT_PX - navigationMap.height - SCREEN_SPACER

            with(navigationMap) {
                draw(
                    x = navMapX,
                    y = navMapY,
                    scale = scale,
                    state = NavigationMapState(
                        currentScreen = currentScreen,
                        sourceColumn = previousColumn,
                        appTheme = appTheme
                    )
                )
            }
        }

        // ===================================
        // CLEAN CONFIRMATION DIALOG
        // Drawn on top of everything when active
        // ===================================
        if (showCleanDialog) {
            drawCleanDialog(scale, cleanDialogTarget, cleanDialogCursor, appTheme)
        }
        if (showNewProjectDialog) {
            drawSimpleConfirmDialog(scale, "NEW PROJECT?", appTheme)
        }
        if (showInstrTypeDialog) {
            drawSimpleConfirmDialog(scale, "CHANGE TYPE?", appTheme)
        }
        if (showRecoveryDialog) {
            drawSimpleConfirmDialog(scale, "RECOVER WORK?", appTheme)
        }

        // ===================================
        // QWERTY KEYBOARD OVERLAY
        // Drawn on top of everything when active
        // ===================================
        if (qwertyKeyboardState.isOpen) {
            drawQwertyKeyboard(qwertyKeyboardState, scale, appTheme)
        }

        // ===================================
        // FX HELPER OVERLAY
        // Drawn on top of everything when active
        // ===================================
        if (fxHelperState.isOpen) {
            drawFxHelper(fxHelperState, scale, appTheme)
        }

        // ===================================
        // LAYOUT COMPLETE!
        // ===================================
        // Left column: Oscilloscope + Phrase Editor (or placeholder)
        // Right corner: Navigation Map
    }

    private fun DrawScope.drawCleanDialog(scale: Int, target: String, @Suppress("UNUSED_PARAMETER") cursor: Int, t: AppTheme) {
        drawSimpleConfirmDialog(scale, "CLEAN $target?", t)
    }

    private fun DrawScope.drawSimpleConfirmDialog(scale: Int, title: String, t: AppTheme) {
        val boxW = 260
        val boxH = 55
        val boxX = (DESIGN_WIDTH_PX - boxW) / 2
        val boxY = (DESIGN_HEIGHT_PX - boxH) / 2
        val fs = 3; val cs = 2
        // char slot width at fs=3, cs=2 is 17px; textWidth = n*17-2
        fun tw(s: String) = if (s.isEmpty()) 0 else s.length * 17 - 2

        drawRect(
            color = Color(0xCC000000),
            topLeft = Offset.Zero,
            size = Size((DESIGN_WIDTH_PX * scale).toFloat(), (DESIGN_HEIGHT_PX * scale).toFloat())
        )
        drawRect(
            color = Color(t.meterBackground),
            topLeft = Offset((boxX * scale).toFloat(), (boxY * scale).toFloat()),
            size = Size((boxW * scale).toFloat(), (boxH * scale).toFloat())
        )
        drawRect(
            color = Color(t.textTitle),
            topLeft = Offset((boxX * scale).toFloat(), (boxY * scale).toFloat()),
            size = Size((boxW * scale).toFloat(), (boxH * scale).toFloat()),
            style = Stroke(width = scale.toFloat())
        )
        val instruction = "A=YES  B=NO"
        drawBitmapText(title,       boxX + (boxW - tw(title))       / 2, boxY + 8,  scale, Color(t.textTitle),  cs, fs)
        drawBitmapText(instruction, boxX + (boxW - tw(instruction)) / 2, boxY + 30, scale, Color(t.textCursor), cs, fs)
    }

    // ============================================================================
    // QWERTY KEYBOARD OVERLAY
    // ============================================================================

    /**
     * Draw the QWERTY keyboard overlay.
     *
     * Layout (box = 470×195px, centered at 320,240):
     *   y=  5  Header label (fieldLabel, grey, centered) — fontScale=4 (20px chars)
     *   y= 35  Text input row — dynamic centering, cursor=26px high
     *   y= 71  Key row 0  (Q-P  or  1-0)   — cellH=26px, gap=4px
     *   y=101  Key row 1  (A-L  or  !-_)
     *   y=131  Key row 2  (Z-M  or  <>-])
     *   y=161  Space bar
     *   y=195  → boxH = 195
     *
     * Box: 470×195, top-left at (85, 142)
     */
    private fun DrawScope.drawQwertyKeyboard(state: QwertyKeyboardState, scale: Int, t: AppTheme) {
        val fs = 4          // font scale for keys (20×20px chars)
        val cs = 3          // char spacing (slightly wider than before)
        val charW = 5 * fs + cs  // 23px per char slot

        // ── Box geometry ──────────────────────────────────────────────────────
        val boxW = 470
        val boxH = 195
        val boxX = (DESIGN_WIDTH_PX - boxW) / 2    // 85
        val boxY = (DESIGN_HEIGHT_PX - boxH) / 2   // 142

        // Inner usable area (5px padding each side)
        val innerX = boxX + 5
        val innerW = boxW - 10   // 460px = 20 chars × 23px

        // ── Semi-transparent backdrop ─────────────────────────────────────────
        drawRect(
            color = Color(0xCC000000),
            topLeft = Offset.Zero,
            size = Size((DESIGN_WIDTH_PX * scale).toFloat(), (DESIGN_HEIGHT_PX * scale).toFloat())
        )

        // ── Dialog box ────────────────────────────────────────────────────────
        drawRect(
            color = Color(t.meterBackground),
            topLeft = Offset((boxX * scale).toFloat(), (boxY * scale).toFloat()),
            size = Size((boxW * scale).toFloat(), (boxH * scale).toFloat())
        )
        drawRect(
            color = Color(t.textTitle),
            topLeft = Offset((boxX * scale).toFloat(), (boxY * scale).toFloat()),
            size = Size((boxW * scale).toFloat(), (boxH * scale).toFloat()),
            style = Stroke(width = scale.toFloat())
        )

        // ── Context-sensitive header (centered) ───────────────────────────────
        val labelW = state.fieldLabel.length * charW
        val labelX = boxX + (boxW - labelW) / 2
        drawBitmapText(
            text = state.fieldLabel,
            x = labelX,
            y = boxY + 5 + 3,  // +3 for text baseline padding
            scale = scale,
            color = Color(t.textParam),
            spacing = cs,
            fontScale = fs
        )

        // ── Text input row (dynamically centered on current text) ─────────────
        val textRowY = boxY + 35
        val cellH = 26     // unified cell height for both text cursor and key cells
        val boxCenterX = boxX + boxW / 2  // 85 + 235 = 320

        // Center text around the box's horizontal midpoint.
        // When text is empty, cursor sits at centre; as chars are added the group stays centred.
        val textLen = state.text.length
        val textDisplayW = textLen * charW
        val textStartX = (boxCenterX - textDisplayW / 2).coerceAtLeast(innerX)

        // Draw each text character and the cursor highlight
        // Iterate through current text length + 1 to cover the end-of-text cursor position
        for (i in 0..textLen) {
            val charPixelX = textStartX + i * charW
            val isCursor = (i == state.textCursor)

            // Cursor highlight box: full cellH tall, charWidth wide, aligned with character
            if (isCursor) {
                drawRect(
                    color = Color(t.textCursor.darken(0.27f)),
                    topLeft = Offset((charPixelX * scale).toFloat(), (textRowY * scale).toFloat()),
                    size = Size(((5 * fs) * scale).toFloat(), (cellH * scale).toFloat())
                )
            }

            // Draw actual character (skip the end-of-text phantom position)
            if (i < textLen) {
                drawBitmapText(
                    text = state.text[i].toString(),
                    x = charPixelX,
                    y = textRowY + 3,  // baseline padding inside cell
                    scale = scale,
                    color = if (isCursor) Color(t.textCursor) else Color(t.textValue),
                    spacing = cs,
                    fontScale = fs
                )
            }
        }

        // ── Key rows ──────────────────────────────────────────────────────────
        val rows = qwertyRowsForLayout(state.layout)
        val cellW = 46     // key cell width (innerW=460 / 10 keys = 46px)
        val rowBaseY = boxY + 71
        val rowGap = 4     // gap between rows

        for (rowIdx in rows.indices) {
            val row = rows[rowIdx]
            val rowY = rowBaseY + rowIdx * (cellH + rowGap)

            if (rowIdx == 3) {
                // Space bar: single wide key, centered
                val spaceW = 7 * cellW   // 322px wide
                val spaceX = boxX + (boxW - spaceW) / 2
                val isSpaceCursor = (state.keyCursorRow == rowIdx)

                drawRect(
                    color = if (isSpaceCursor) Color(t.textCursor.darken(0.27f)) else Color(t.meterBackground),
                    topLeft = Offset((spaceX * scale).toFloat(), (rowY * scale).toFloat()),
                    size = Size((spaceW * scale).toFloat(), (cellH * scale).toFloat())
                )
                if (isSpaceCursor) {
                    drawRect(
                        color = Color(t.textCursor),
                        topLeft = Offset((spaceX * scale).toFloat(), (rowY * scale).toFloat()),
                        size = Size((spaceW * scale).toFloat(), (cellH * scale).toFloat()),
                        style = Stroke(width = scale.toFloat())
                    )
                }
                drawBitmapText(
                    text = "SPACE",
                    x = spaceX + (spaceW - 5 * charW) / 2,
                    y = rowY + 3,
                    scale = scale,
                    color = if (isSpaceCursor) Color(t.textCursor) else Color(t.textParam),
                    spacing = cs,
                    fontScale = fs
                )
            } else {
                // Normal key row: offset to center shorter rows
                val rowTotalW = row.size * cellW
                val rowOffsetX = (innerW - rowTotalW) / 2
                val rowStartX = innerX + rowOffsetX

                for (colIdx in row.indices) {
                    val keyChar = row[colIdx]
                    val cellX = rowStartX + colIdx * cellW
                    val isCursor = (state.keyCursorRow == rowIdx && state.keyCursorCol == colIdx)

                    // Key background
                    drawRect(
                        color = if (isCursor) Color(t.textCursor.darken(0.27f)) else Color(t.meterBackground),
                        topLeft = Offset((cellX * scale).toFloat(), (rowY * scale).toFloat()),
                        size = Size(((cellW - 1) * scale).toFloat(), (cellH * scale).toFloat())
                    )

                    // Cursor border
                    if (isCursor) {
                        drawRect(
                            color = Color(t.textCursor),
                            topLeft = Offset((cellX * scale).toFloat(), (rowY * scale).toFloat()),
                            size = Size(((cellW - 1) * scale).toFloat(), (cellH * scale).toFloat()),
                            style = Stroke(width = scale.toFloat())
                        )
                    }

                    // Key character (centered in cell)
                    val charX = cellX + (cellW - 1 - 5 * fs) / 2
                    drawBitmapText(
                        text = keyChar.toString(),
                        x = charX,
                        y = rowY + 3,
                        scale = scale,
                        color = if (isCursor) Color(t.textCursor) else Color(t.textValue),
                        spacing = cs,
                        fontScale = fs
                    )
                }
            }
        }
    }

    // ============================================================================
    // FX HELPER OVERLAY
    // ============================================================================

    /**
     * Draw the FX helper overlay.
     *
     * Layout (box = 580×222px, centered on 640×480 canvas):
     *   Description area: up to 4 lines × ROW_HEIGHT (84px)
     *   8px spacer
     *   "EFFECT" header (21px), centered
     *   8px spacer
     *   4 rows × 6 cols effect grid (84px)
     *
     * Box: 580×222, top-left at (30, 129)
     */
    private fun DrawScope.drawFxHelper(state: FxHelperState, scale: Int, t: AppTheme) {
        val fs = 3          // font scale (15px chars)
        val cs = 2          // char spacing
        val charW = 5 * fs + cs  // 17px per char slot
        val rowH = 21       // row height

        // ── Box geometry ─────────────────────────────────────────────────────
        val boxW = 580
        val boxH = 222
        val boxX = (DESIGN_WIDTH_PX - boxW) / 2    // 30
        val boxY = (DESIGN_HEIGHT_PX - boxH) / 2   // 129
        val innerX = boxX + 10

        // ── Semi-transparent backdrop ─────────────────────────────────────────
        drawRect(
            color = Color(0xCC000000),
            topLeft = Offset.Zero,
            size = Size((DESIGN_WIDTH_PX * scale).toFloat(), (DESIGN_HEIGHT_PX * scale).toFloat())
        )

        // ── Dialog box ────────────────────────────────────────────────────────
        drawRect(
            color = Color(t.meterBackground),
            topLeft = Offset((boxX * scale).toFloat(), (boxY * scale).toFloat()),
            size = Size((boxW * scale).toFloat(), (boxH * scale).toFloat())
        )
        drawRect(
            color = Color(t.textTitle),
            topLeft = Offset((boxX * scale).toFloat(), (boxY * scale).toFloat()),
            size = Size((boxW * scale).toFloat(), (boxH * scale).toFloat()),
            style = Stroke(width = scale.toFloat())
        )

        // ── Description lines (up to 4) ───────────────────────────────────────
        val descLines = state.descriptionLines()
        var textY = boxY + 8
        for (i in 0 until 4) {
            val line = descLines.getOrNull(i) ?: break
            drawBitmapText(
                text = line,
                x = innerX,
                y = textY + 3,
                scale = scale,
                color = Color(t.textValue),
                spacing = cs,
                fontScale = fs
            )
            textY += rowH
        }

        // ── "EFFECT" header (centered) ────────────────────────────────────────
        val headerY = boxY + 8 + 4 * rowH + 8   // 8 + 84 + 8 = 100px from top
        val headerText = "EFFECT"
        val headerW = headerText.length * charW
        val headerX = boxX + (boxW - headerW) / 2
        drawBitmapText(
            text = headerText,
            x = headerX,
            y = headerY + 3,
            scale = scale,
            color = Color(t.textTitle),
            spacing = cs,
            fontScale = fs
        )

        // ── Effect grid (6 cols × 4 rows) ─────────────────────────────────────
        val gridY = headerY + rowH + 8           // after header + spacer
        val cellW = 80
        val gridCols = 6
        val gridW = gridCols * cellW             // 480px
        val gridX = boxX + (boxW - gridW) / 2   // center in box

        val effectTypes = EffectProcessor.EFFECT_TYPES
        for (i in effectTypes.indices) {
            val row = i / gridCols
            val col = i % gridCols
            val cellX = gridX + col * cellW
            val cellY = gridY + row * rowH

            val isCursor = (state.cursorRow == row && state.cursorCol == col)
            val effectName = getEffectTypeName(effectTypes[i])

            // Cursor highlight background
            if (isCursor) {
                drawRect(
                    color = Color(t.textCursor.darken(0.27f)),
                    topLeft = Offset((cellX * scale).toFloat(), (cellY * scale).toFloat()),
                    size = Size((cellW * scale).toFloat(), (rowH * scale).toFloat())
                )
                drawRect(
                    color = Color(t.textCursor),
                    topLeft = Offset((cellX * scale).toFloat(), (cellY * scale).toFloat()),
                    size = Size((cellW * scale).toFloat(), (rowH * scale).toFloat()),
                    style = Stroke(width = scale.toFloat())
                )
            }

            // Effect name centered in cell
            val nameW = 3 * charW
            val nameX = cellX + (cellW - nameW) / 2
            drawBitmapText(
                text = effectName,
                x = nameX,
                y = cellY + 3,
                scale = scale,
                color = when {
                    isCursor -> Color(t.textCursor)
                    effectTypes[i] == EffectProcessor.FX_NONE -> Color(t.textEmpty)
                    else -> Color(t.textValue)
                },
                spacing = cs,
                fontScale = fs
            )
        }
    }

    /**
     * Draw a placeholder screen for screens that don't have modules yet
     * This shows the screen name and a "Coming Soon" message
     */
    private fun DrawScope.drawPlaceholderScreen(
        x: Int,
        y: Int,
        scale: Int,
        screenType: ScreenType,
        t: AppTheme = AppTheme.CLASSIC
    ) {
        // Draw background (same size as phrase editor: 620×392)
        drawRect(
            color = Color(t.background),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size = Size((620 * scale).toFloat(), (392 * scale).toFloat())
        )

        // Draw screen title at top (15px font + 3px padding = starts at y+3)
        val titleY = y + 3
        drawBitmapText(
            text = screenType.label.uppercase(),
            x = x + 20,  // 20px from left edge
            y = titleY,
            scale = scale,
            color = Color(t.textTitle),
            spacing = 2,
            fontScale = 3  // 15×15 font
        )

        // Draw "COMING SOON" message in center
        // Calculate center position
        val messageY = y + 180  // Roughly centered vertically in 392px
        val message = "COMING SOON"

        // Calculate message width to center it
        // Each char is 5×3=15px wide, plus 2px spacing between chars
        val messageWidth = (message.length * 15) + ((message.length - 1) * 2)
        val messageX = x + (620 - messageWidth) / 2

        drawBitmapText(
            text = message,
            x = messageX,
            y = messageY,
            scale = scale,
            color = Color(t.textEmpty),
            spacing = 2,
            fontScale = 3
        )
    }
}

/**
 * BITMAP FONT RENDERING FUNCTIONS
 *
 * These are used by modules to draw text
 */

fun DrawScope.drawBitmapText(
    text: String,
    x: Int,
    y: Int,
    scale: Int,
    color: Color,
    spacing: Int = 2,
    fontScale: Int = 1
) {
    var currentX = x
    for (char in text) {
        drawBitmapChar(char, currentX, y, scale, color, fontScale)
        currentX += (5 * fontScale) + spacing
    }
}

/**
 * Pixel-perfect paint for bitmap font rendering.
 * isAntiAlias = false prevents sub-pixel blending between adjacent pixels.
 */
private val _bitmapPaint = Paint().apply {
    isAntiAlias = false
}

// Dedicated paint for the glyph-atlas path: isAntiAlias=false + FilterQuality.None so the upscaled
// cells stay hard-edged and crisp even when the letterbox offset is fractional (matches the per-rect
// path exactly). Separate from _bitmapPaint so its per-glyph colorFilter never leaks into the
// fallback drawing. colorFilter is set (to a cached tint) per glyph just before each draw.
private val _atlasPaint = Paint().apply {
    isAntiAlias = false
    filterQuality = FilterQuality.None
}

// ───────────────────────────────────────────────────────────────────────────
// GLYPH ATLAS (6.2)
// ───────────────────────────────────────────────────────────────────────────
// A full phrase screen is ~700+ glyphs, and the old per-glyph path emitted up to ~7 drawRect calls
// each (thousands of draw ops/frame at 60 fps during playback). Instead we pre-render the 128 ASCII
// glyphs once into a single 640×5 white-on-transparent ImageBitmap and stamp each glyph with ONE
// tinted drawImage. FilterQuality.None (nearest-neighbour) keeps the 5×5 cells pixel-perfect at any
// integer scale, so a single native-resolution atlas covers every fontScale/scale combination.
// Non-ASCII glyphs (arrows ↑↓←→) and unmapped chars keep the original per-row run drawing below.

private const val ATLAS_CELL = 5  // each glyph is 5×5 in the source bitmap
private val ATLAS_CELL_SIZE = IntSize(ATLAS_CELL, ATLAS_CELL)

private val GLYPH_ATLAS: ImageBitmap by lazy {
    val cols = 128
    val w = cols * ATLAS_CELL  // 640
    val h = ATLAS_CELL         // 5
    val px = IntArray(w * h)   // ARGB_8888; 0 = transparent
    val white = 0xFFFFFFFF.toInt()
    for (code in 0 until cols) {
        val glyph = FONT_5X5_ASCII[code] ?: continue
        val baseX = code * ATLAS_CELL
        for (row in 0 until ATLAS_CELL) {
            val bits = glyph[row].toInt()
            for (col in 0 until ATLAS_CELL) {
                if ((bits shr (4 - col)) and 1 != 0) px[row * w + baseX + col] = white
            }
        }
    }
    android.graphics.Bitmap.createBitmap(px, w, h, android.graphics.Bitmap.Config.ARGB_8888).asImageBitmap()
}

// Cache one ColorFilter per tint colour. ColorFilter.tint() allocates a JVM object + native peer, so
// recreating it per glyph would reintroduce exactly the per-frame GC churn 6.3 removed. Theme palettes
// are tiny, so a linear scan of packed ARGB ints (no boxing) is both zero-alloc and fast. Draw-thread
// only.
private var tintKeys = IntArray(16)
private var tintVals = arrayOfNulls<ColorFilter>(16)
private var tintCount = 0
private fun tintFor(color: Color): ColorFilter {
    val argb = color.toArgb()
    for (i in 0 until tintCount) if (tintKeys[i] == argb) return tintVals[i]!!
    if (tintCount == tintKeys.size) {
        tintKeys = tintKeys.copyOf(tintCount * 2)
        tintVals = tintVals.copyOf(tintCount * 2)
    }
    val cf = ColorFilter.tint(color)
    tintKeys[tintCount] = argb
    tintVals[tintCount] = cf
    tintCount++
    return cf
}

fun DrawScope.drawBitmapChar(
    char: Char,
    x: Int,
    y: Int,
    scale: Int,
    color: Color,
    fontScale: Int = 1
) {
    val code = char.code
    // ASCII glyph-atlas fast path: one tinted image blit instead of up to ~7 drawRects per glyph.
    if (code in 0..127) {
        val glyph = FONT_5X5_ASCII[code]
        if (glyph != null) {
            val size = ATLAS_CELL * fontScale * scale
            drawIntoCanvas { canvas ->
                _atlasPaint.colorFilter = tintFor(color)
                canvas.drawImageRect(
                    image = GLYPH_ATLAS,
                    srcOffset = IntOffset(code * ATLAS_CELL, 0),
                    srcSize = ATLAS_CELL_SIZE,
                    dstOffset = IntOffset(x * scale, y * scale),
                    dstSize = IntSize(size, size),
                    paint = _atlasPaint
                )
            }
            return
        }
    }

    // Non-ASCII (arrows ↑↓←→) and unmapped chars: original per-row run drawing.
    val charData = FONT_5X5[char] ?: FONT_5X5[char.uppercaseChar()]
    if (charData == null) {
        // Missing character - draw outline square
        drawRect(
            color = color,
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size = Size(
                (5 * fontScale * scale).toFloat(),
                (5 * fontScale * scale).toFloat()
            ),
            style = Stroke(width = (scale * fontScale).toFloat())
        )
        return
    }

    drawIntoCanvas { canvas ->
        val paint = _bitmapPaint
        paint.color = color
        // Each row: merge consecutive set pixels into a single rect to eliminate internal edges
        for (row in 0..4) {
            val rowData = charData[row].toInt()
            val rowY = ((y + row * fontScale) * scale).toFloat()
            val rowBottom = rowY + (fontScale * scale).toFloat()
            var col = 0
            while (col <= 4) {
                if ((rowData and (1 shl (4 - col))) != 0) {
                    val runStart = col
                    while (col <= 4 && (rowData and (1 shl (4 - col))) != 0) col++
                    val left  = ((x + runStart * fontScale) * scale).toFloat()
                    val right = left + ((col - runStart) * fontScale * scale).toFloat()
                    canvas.drawRect(left, rowY, right, rowBottom, paint)
                } else {
                    col++
                }
            }
        }
    }
}

// Font data is now imported from BitmapFont5x5.kt (internal val FONT_5X5)