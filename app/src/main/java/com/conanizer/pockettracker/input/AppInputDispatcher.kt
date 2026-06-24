package com.conanizer.pockettracker.input

import android.util.Log
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.setValue
import com.conanizer.pockettracker.ui.theme.AppTheme
import com.conanizer.pockettracker.platform.android.DeviceAdapter
import com.conanizer.pockettracker.ui.clearChainSlot
import com.conanizer.pockettracker.ui.clearSongChainRef
import com.conanizer.pockettracker.core.audio.AudioEngine
import com.conanizer.pockettracker.core.data.InstrumentType
import com.conanizer.pockettracker.core.data.MAIN_ROW_SCREENS
import com.conanizer.pockettracker.core.data.Note
import com.conanizer.pockettracker.core.data.ScreenType
import com.conanizer.pockettracker.core.data.VolumeUtils
import com.conanizer.pockettracker.core.logic.ClipboardManager
import com.conanizer.pockettracker.core.logic.EffectProcessor
import com.conanizer.pockettracker.core.logic.FileController
import com.conanizer.pockettracker.core.logic.InputAction
import com.conanizer.pockettracker.core.logic.InstrumentController
import com.conanizer.pockettracker.core.logic.LoadResult
import com.conanizer.pockettracker.core.logic.RenderController
import com.conanizer.pockettracker.core.logic.TrackerController
import com.conanizer.pockettracker.core.storage.FileSortMode
import com.conanizer.pockettracker.core.storage.WavWriter
import com.conanizer.pockettracker.ui.getTrackIndex
import com.conanizer.pockettracker.platform.android.AndroidFileSystem
import com.conanizer.pockettracker.platform.android.AndroidVideoAudioExtractor
import com.conanizer.pockettracker.platform.android.OboeAudioBackend
import com.conanizer.pockettracker.toFileInfo
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
import com.conanizer.pockettracker.ui.modules.InstrumentPoolModule
import com.conanizer.pockettracker.ui.modules.InstrumentPoolState
import com.conanizer.pockettracker.ui.modules.InstrumentState
import com.conanizer.pockettracker.ui.modules.MixerModule
import com.conanizer.pockettracker.ui.modules.MixerState
import com.conanizer.pockettracker.ui.modules.ModulationModule
import com.conanizer.pockettracker.ui.modules.ModulationState
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
import com.conanizer.pockettracker.ui.modules.ThemeEditorModule
import com.conanizer.pockettracker.ui.modules.ThemeEditorState
import com.conanizer.pockettracker.ui.overlays.EqCallerContext
import com.conanizer.pockettracker.ui.overlays.EqEditorState
import com.conanizer.pockettracker.ui.overlays.FxHelperState
import com.conanizer.pockettracker.ui.overlays.QwertyContext
import com.conanizer.pockettracker.ui.overlays.QwertyKeyboardState
import com.conanizer.pockettracker.ui.overlays.cursorBand
import com.conanizer.pockettracker.ui.overlays.cursorParam
import com.conanizer.pockettracker.ui.overlays.deleteChar
import com.conanizer.pockettracker.ui.overlays.fxMoveCursorDown
import com.conanizer.pockettracker.ui.overlays.fxMoveCursorLeft
import com.conanizer.pockettracker.ui.overlays.fxMoveCursorRight
import com.conanizer.pockettracker.ui.overlays.fxMoveCursorUp
import com.conanizer.pockettracker.ui.overlays.insertCurrentKey
import com.conanizer.pockettracker.ui.overlays.isOnActionRow
import com.conanizer.pockettracker.ui.overlays.moveCursorDown
import com.conanizer.pockettracker.ui.overlays.moveCursorLeft
import com.conanizer.pockettracker.ui.overlays.moveCursorRight
import com.conanizer.pockettracker.ui.overlays.moveCursorUp
import com.conanizer.pockettracker.ui.overlays.moveTextCursorLeft
import com.conanizer.pockettracker.ui.overlays.moveTextCursorRight
import com.conanizer.pockettracker.ui.overlays.selectedEffectCode
import com.conanizer.pockettracker.ui.overlays.withClampedCol
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import java.io.File

/** Tracks where a single A-press inserted into an empty cell (screen, row, col). */
data class InsertPosition(val screen: ScreenType, val row: Int, val col: Int)

/** Bundles all stable controller and module references for AppInputDispatcher. */
data class AppControllers(
    val trackerController: TrackerController,
    val audioEngine: AudioEngine,
    val audioBackend: OboeAudioBackend,
    val instrumentController: InstrumentController,
    val fileController: FileController,
    val renderController: RenderController,
    val clipboardManager: ClipboardManager,
    val deviceAdapter: DeviceAdapter,
    val videoExtractor: AndroidVideoAudioExtractor,
    val fileSystem: AndroidFileSystem,
    val coroutineScope: CoroutineScope,
    val chainEditorModule: ChainEditorModule,
    val phraseEditorModule: PhraseEditorModule,
    val songEditorModule: SongEditorModule,
    val projectModule: ProjectModule,
    val settingsModule: SettingsModule,
    val instrumentModule: InstrumentModule,
    val instrumentPoolModule: InstrumentPoolModule,
    val mixerModule: MixerModule,
    val effectModule: EffectModule,
    val eqModule: EqModule,
    val tableModule: TableModule,
    val grooveModule: GrooveModule,
    val modulationModule: ModulationModule,
    val fileBrowserModule: FileBrowserModule,
    val sampleEditorModule: SampleEditorModule
)

/** Bundles all Compose MutableState references that button handlers need to read or write. */
class AppStateRefs(
    val fileBrowserState: MutableState<FileBrowserModule.State>,
    val sampleEditorState: MutableState<SampleEditorState>,
    val qwertyKeyboardState: MutableState<QwertyKeyboardState>,
    val fxHelperState: MutableState<FxHelperState>,
    val eqEditorState: MutableState<EqEditorState>,
    val eqSpectrumData: MutableState<FloatArray?>,
    val layoutMode: MutableState<DeviceAdapter.LayoutMode>,
    val portraitSkinId: MutableState<String>,
    val scalingMode: MutableState<DeviceAdapter.ScalingMode>,
    val isRendering: MutableState<Boolean>,
    val isStemsRendering: MutableState<Boolean>,
    val renderProgress: MutableState<Float>,
    val showCleanDialog: MutableState<Boolean>,
    val cleanDialogTarget: MutableState<String>,
    val cleanDialogCursor: MutableState<Int>,
    val lastAInsertPosition: MutableState<InsertPosition?>,
    val insertBefore: MutableState<Boolean>,
    val instrumentFileBrowserAction: MutableState<String>,
    val previousScreen: MutableState<ScreenType>,
    val buttonSoundEnabled: MutableState<Boolean>,
    val buttonSoundVolume: MutableState<Int>,
    val buttonVibroEnabled: MutableState<Boolean>,
    val vibroPower: MutableState<Int>,
    val cursorRemember: MutableState<Boolean>,
    val notePreviewEnabled: MutableState<Boolean>,
    val overlayName: MutableState<String>,
    val overlayStrength: MutableState<Int>,
    val overlayFiles: List<String>,
    val trackPeakBuffer: FloatArray,
    val masterPeakBuffer: FloatArray,
    val sendPeakBuffer: FloatArray,
    val appTheme: MutableState<AppTheme>,
    val themeEditorState: MutableState<ThemeEditorState>,
    val showNewProjectDialog: MutableState<Boolean>,
    val showInstrTypeDialog: MutableState<Boolean>,
    val showRecoveryDialog: MutableState<Boolean>,
    val autosaveResumeAuto: MutableState<Boolean>
)

class AppInputDispatcher(val ctrl: AppControllers, val refs: AppStateRefs) {

    // ── Controller shortcuts ─────────────────────────────────────────────────
    private val trackerController get() = ctrl.trackerController
    private val audioEngine get() = ctrl.audioEngine
    private val audioBackend get() = ctrl.audioBackend
    private val instrumentController get() = ctrl.instrumentController
    private val fileController get() = ctrl.fileController
    private val renderController get() = ctrl.renderController
    private val clipboardManager get() = ctrl.clipboardManager
    private val deviceAdapter get() = ctrl.deviceAdapter
    private val videoExtractor get() = ctrl.videoExtractor
    private val fileSystem get() = ctrl.fileSystem
    private val coroutineScope get() = ctrl.coroutineScope

    // ── Module shortcuts ─────────────────────────────────────────────────────
    private val chainEditorModule get() = ctrl.chainEditorModule
    private val phraseEditorModule get() = ctrl.phraseEditorModule
    private val songEditorModule get() = ctrl.songEditorModule
    private val projectModule get() = ctrl.projectModule
    private val settingsModule get() = ctrl.settingsModule
    private val instrumentModule get() = ctrl.instrumentModule
    private val instrumentPoolModule get() = ctrl.instrumentPoolModule
    private val mixerModule get() = ctrl.mixerModule
    private val effectModule get() = ctrl.effectModule
    private val eqModule get() = ctrl.eqModule
    private val tableModule get() = ctrl.tableModule
    private val grooveModule get() = ctrl.grooveModule
    private val modulationModule get() = ctrl.modulationModule
    private val fileBrowserModule get() = ctrl.fileBrowserModule
    private val sampleEditorModule get() = ctrl.sampleEditorModule

    // ── State delegation (read/write via Compose MutableState) ───────────────
    private var fileBrowserState by refs.fileBrowserState
    private var sampleEditorState by refs.sampleEditorState
    private var qwertyKeyboardState by refs.qwertyKeyboardState
    private var fxHelperState by refs.fxHelperState
    private var eqEditorState by refs.eqEditorState
    private var eqSpectrumData by refs.eqSpectrumData
    private var layoutMode by refs.layoutMode
    private var portraitSkinId by refs.portraitSkinId
    private var scalingMode by refs.scalingMode
    private var isRendering by refs.isRendering
    private var isStemsRendering by refs.isStemsRendering
    private var renderProgress by refs.renderProgress
    private var showCleanDialog by refs.showCleanDialog
    private var cleanDialogTarget by refs.cleanDialogTarget
    private var cleanDialogCursor by refs.cleanDialogCursor
    private var lastAInsertPosition by refs.lastAInsertPosition
    private var insertBefore by refs.insertBefore
    private var instrumentFileBrowserAction by refs.instrumentFileBrowserAction
    private var previousScreen by refs.previousScreen
    private var buttonSoundEnabled by refs.buttonSoundEnabled
    private var buttonSoundVolume by refs.buttonSoundVolume
    private var buttonVibroEnabled by refs.buttonVibroEnabled
    private var vibroPower by refs.vibroPower
    private var cursorRemember by refs.cursorRemember
    private var notePreviewEnabled by refs.notePreviewEnabled
    private var overlayName by refs.overlayName
    private var overlayStrength by refs.overlayStrength
    private val overlayFiles get() = refs.overlayFiles
    private val trackPeakBuffer get() = refs.trackPeakBuffer
    private val masterPeakBuffer get() = refs.masterPeakBuffer
    private val sendPeakBuffer get() = refs.sendPeakBuffer
    private var appTheme by refs.appTheme
    private var themeEditorState by refs.themeEditorState
    private var showNewProjectDialog by refs.showNewProjectDialog
    private var showInstrTypeDialog by refs.showInstrTypeDialog
    private var showRecoveryDialog by refs.showRecoveryDialog
    private var autosaveResumeAuto by refs.autosaveResumeAuto

    // Dedicated return target for SETTINGS — set when entering, never overwritten by FILE_BROWSER navigation
    private var settingsReturnScreen: ScreenType = ScreenType.PROJECT

    // L+B double-tap timer for file browser select-all
    private var lastFileBrowserLBTime: Long = 0L

    private fun computeSliceCuePoints(state: SampleEditorState): IntArray = when (state.sliceMethod) {
        0 -> state.transientMarkers.filter { it > 0 && it < state.totalFrames }.toIntArray()
        1 -> {
            val div = state.sliceDivisions.coerceAtLeast(1)
            IntArray(div - 1) { i -> ((i + 1).toLong() * state.totalFrames / div).toInt() }
        }
        else -> state.transientMarkers.filter { it > 0 && it < state.totalFrames }.toIntArray()
    }

    fun syncVolumesToAudioBackend() {
        val project = trackerController.project
        for (i in 0 until 8) {
            audioBackend.setTrackVolume(i, VolumeUtils.hexToFloat(project.tracks[i].volume))
        }
        audioBackend.setMasterVolume(VolumeUtils.hexToFloat(project.masterVolume))
        audioBackend.setOttDepth(project.ottDepth)
        audioBackend.setMasterFx(project.masterBusFx)
        audioBackend.setDustDepth(project.dustDepth)
        audioBackend.setLimiterPreGain(project.limiterPreGain)
        Log.d("VolumeSync", "Synced all track/master volumes to audio backend")
    }

    fun reloadProjectSamples() {
        // Start every load/recovery from a clean native slate so the previous project's PCM and
        // soundfonts don't accumulate (REVIEW-3 5.1). clearAllSamples holds sampleEditMutex and stops
        // voices inside the lock (audio-safe); clearAllSoundfonts frees all SF slots + the path→slot
        // map. Both are repopulated by the load loop below.
        audioEngine.clearAllSamples()
        instrumentController.clearAllSoundfonts()
        var loadedCount = 0
        var failedCount = 0
        trackerController.project.instruments.forEach { instrument ->
            if (instrument.instrumentType == InstrumentType.SOUNDFONT && instrument.soundfontPath != null) {
                val path = instrument.soundfontPath!!
                val slot = audioEngine.backend.loadSoundfont(instrument.id, path)
                if (slot >= 0) {
                    instrumentController.sfSlotMap[path] = slot
                    val firstPreset = audioEngine.backend.getSoundfontFirstBankPreset(slot)
                    if (firstPreset[0] >= 0 &&
                        audioEngine.backend.getSoundfontPresetName(slot, instrument.sfBank, instrument.sfPreset) == "---") {
                        instrument.sfBank   = firstPreset[0]
                        instrument.sfPreset = firstPreset[1]
                    }
                    loadedCount++
                } else {
                    failedCount++
                }
            } else if (instrument.sampleFilePath != null) {
                val filePath = instrument.sampleFilePath!!
                val ext = filePath.substringAfterLast('.', "").lowercase()
                val loaded = if (ext == "mp3") {
                    // No WAV was ever written for a compressed source — re-decode it (streaming straight
                    // into native memory) each time the project is reopened.
                    audioEngine.loadSampleCompressed(instrument.id, filePath, videoExtractor)
                } else {
                    audioEngine.loadSampleFromFile(instrument.id, filePath)
                }
                if (loaded) {
                    audioEngine.updateInstrumentPlaybackParams(instrument)
                    // Cue points only exist in WAV files; compressed sources have none.
                    if (ext != "mp3") instrument.sliceMarkers = WavWriter.readCuePoints(filePath).map { it.toLong() }
                    loadedCount++
                } else {
                    failedCount++
                }
            }
        }
        if (loadedCount > 0 || failedCount > 0) {
            Log.d("ProjectLoad", "Sample reload complete: $loadedCount loaded, $failedCount failed")
        }
        syncVolumesToAudioBackend()
    }

    private fun applyFileBrowserInputAction(action: InputAction) {
        when (action) {
            is InputAction.SET_VALUE -> {
                val char = action.value.toChar()
                val buffer = fileBrowserState.renameBuffer.padEnd(12, ' ')
                val sb = StringBuilder(buffer)
                if (fileBrowserState.renameCursor < sb.length) {
                    sb.setCharAt(fileBrowserState.renameCursor, char)
                    fileBrowserState = fileBrowserState.copy(renameBuffer = sb.toString().trimEnd())
                }
            }
            is InputAction.DELETE -> {
                val buffer = fileBrowserState.renameBuffer.padEnd(12, ' ')
                val sb = StringBuilder(buffer)
                if (fileBrowserState.renameCursor < sb.length) {
                    sb.setCharAt(fileBrowserState.renameCursor, ' ')
                    fileBrowserState = fileBrowserState.copy(renameBuffer = sb.toString().trimEnd())
                }
            }
            else -> { }
        }
    }

    private fun isOnFxTypeColumn(): Boolean = when (trackerController.currentScreen) {
        ScreenType.PHRASE -> trackerController.cursorColumn == 4 ||
                             trackerController.cursorColumn == 6 ||
                             trackerController.cursorColumn == 8
        ScreenType.TABLE  -> trackerController.tableCursorColumn == 3 ||
                             trackerController.tableCursorColumn == 5 ||
                             trackerController.tableCursorColumn == 7
        else -> false
    }

    private fun getCurrentFxTypeIndex(): Int {
        val code = when (trackerController.currentScreen) {
            ScreenType.PHRASE -> {
                val step = trackerController.project.phrases[trackerController.currentPhrase]
                    .steps[trackerController.cursorRow]
                when (trackerController.cursorColumn) { 4 -> step.fx1Type; 6 -> step.fx2Type; 8 -> step.fx3Type; else -> 0 }
            }
            ScreenType.TABLE -> {
                val row = trackerController.project.tables[trackerController.currentTable]
                    .rows[trackerController.tableCursorRow]
                when (trackerController.tableCursorColumn) { 3 -> row.fx1Type; 5 -> row.fx2Type; 7 -> row.fx3Type; else -> 0 }
            }
            else -> 0
        }
        val idx = EffectProcessor.EFFECT_TYPES.indexOf(code)
        return if (idx < 0) 0 else idx
    }

    private fun applyFxTypeChange(effectCode: Int) {
        when (trackerController.currentScreen) {
            ScreenType.PHRASE -> {
                val step = trackerController.project.phrases[trackerController.currentPhrase]
                    .steps[trackerController.cursorRow]
                when (trackerController.cursorColumn) {
                    4 -> step.fx1Type = effectCode; 6 -> step.fx2Type = effectCode; 8 -> step.fx3Type = effectCode
                }
                trackerController.projectVersion++
            }
            ScreenType.TABLE -> {
                val row = trackerController.project.tables[trackerController.currentTable]
                    .rows[trackerController.tableCursorRow]
                when (trackerController.tableCursorColumn) {
                    3 -> row.fx1Type = effectCode; 5 -> row.fx2Type = effectCode; 7 -> row.fx3Type = effectCode
                }
                audioEngine.invalidateTable(trackerController.currentTable)
                trackerController.projectVersion++
            }
            else -> {}
        }
    }

    private fun adjustThemeColor(channel: Int, delta: Int) {
        val row = themeEditorState.cursorRow
        if (row < 1 || row > ThemeEditorModule.COLOR_ROWS.size) return  // row 0 = THEME header
        val colorRow = ThemeEditorModule.COLOR_ROWS[row - 1]
        val current = colorRow.get(appTheme)
        val r = ((current shr 16) and 0xFFL).toInt()
        val g = ((current shr 8)  and 0xFFL).toInt()
        val b = ( current         and 0xFFL).toInt()
        val newColor = 0xFF000000L or when (channel) {
            0 -> ((r + delta).coerceIn(0, 255).toLong() shl 16) or (g.toLong() shl 8) or b.toLong()
            1 -> (r.toLong() shl 16) or ((g + delta).coerceIn(0, 255).toLong() shl 8) or b.toLong()
            2 -> (r.toLong() shl 16) or (g.toLong() shl 8) or (b + delta).coerceIn(0, 255).toLong()
            else -> (r.toLong() shl 16) or (g.toLong() shl 8) or b.toLong()
        }
        appTheme = colorRow.set(appTheme, newColor)
    }

    private fun cycleNextBuiltinTheme() {
        val idx = AppTheme.Companion.BUILTINS.indexOfFirst { it.name == appTheme.name }
        val next = if (idx >= 0) (idx + 1) % AppTheme.Companion.BUILTINS.size else 0
        appTheme = AppTheme.Companion.BUILTINS[next].copy(visualizerType = appTheme.visualizerType)
    }

    private fun cyclePrevBuiltinTheme() {
        val idx = AppTheme.Companion.BUILTINS.indexOfFirst { it.name == appTheme.name }
        val prev = if (idx > 0) idx - 1 else AppTheme.Companion.BUILTINS.size - 1
        appTheme = AppTheme.Companion.BUILTINS[prev].copy(visualizerType = appTheme.visualizerType)
    }

    fun openEqEditor(slot: Int, caller: EqCallerContext) {
        eqEditorState = EqEditorState(
            isOpen = true,
            slotIndex = slot.coerceIn(0, 127),
            callerContext = caller
        )
    }

    fun applyCallerEqSlotChange(newSlot: Int) {
        val proj = trackerController.project
        when (val ctx = eqEditorState.callerContext) {
            is EqCallerContext.MasterEq      -> { proj.masterEqSlot = newSlot; audioBackend.setMasterEqSlot(newSlot) }
            is EqCallerContext.ReverbInputEq -> { proj.reverbInputEq = newSlot; audioBackend.setReverbInputEq(newSlot) }
            is EqCallerContext.DelayInputEq  -> { proj.delayInputEq = newSlot; audioBackend.setDelayInputEq(newSlot) }
            is EqCallerContext.InstrumentEq  -> { proj.instruments[ctx.instrId].eqSlot = newSlot; audioBackend.setInstrumentEqSlot(ctx.instrId, newSlot) }
            is EqCallerContext.SampleEditorFx -> { sampleEditorState = sampleEditorState.copy(fxValue = newSlot) }
        }
        trackerController.projectVersion++
    }

    // ── Sub-screen opening (item 3) ──────────────────────────────────────────
    //    Cells whose single-A opens a further screen (EQ editor / qwerty name editor). A is deferred
    //    to release on these cells by the InputMapper (see currentCellOpensSubScreen) so a held
    //    A+DPAD / A+B on the same cell still edits/resets the value; SELECT stays an alias.

    private fun openProjectNameEditor() {
        val currentName = trackerController.project.name.trimEnd()
        qwertyKeyboardState = QwertyKeyboardState(
            isOpen = true,
            text = currentName,
            maxLength = 20,
            textCursor = currentName.length.coerceAtMost(20),
            keyCursorRow = 0,
            keyCursorCol = 0,
            layout = 0,
            fieldLabel = "PROJECT NAME:",
            originalText = currentName,
            insertBefore = insertBefore,
            context = QwertyContext.PROJECT_NAME
        )
    }

    private fun openInstrumentNameEditor() {
        val instr = trackerController.project.instruments[trackerController.currentInstrument]
        val currentName = if (instr.hasDefaultName()) "" else instr.name.trimEnd()
        qwertyKeyboardState = QwertyKeyboardState(
            isOpen = true,
            text = currentName,
            maxLength = 20,
            textCursor = currentName.length.coerceAtMost(20),
            keyCursorRow = 0,
            keyCursorCol = 0,
            layout = 0,
            fieldLabel = "INSTRUMENT NAME:",
            originalText = currentName,
            insertBefore = insertBefore,
            context = QwertyContext.INSTRUMENT_NAME
        )
    }

    /**
     * Opens (or, when [peek], just detects) the sub-screen for the cell under the cursor. Covers the
     * EQ-value cells on INSTRUMENT/INST_POOL/MIXER/EFFECTS, the SAMPLE_EDITOR EQ-effect slot cell, and
     * the PROJECT/INSTRUMENT NAME cells. (The SAMPLE_EDITOR NAME row keeps its own A-handler; its other
     * A-confirm rows — crop/copy/APPLY/… — are left alone, so only the EQ slot cell defers here.)
     *
     * @param peek true = report only, no side effects (used by the InputMapper defer predicate).
     * @return true when the cursor is on a sub-screen-opening cell.
     */
    private fun openSubScreenAtCursor(peek: Boolean): Boolean {
        val proj = trackerController.project
        when (trackerController.currentScreen) {
            ScreenType.PROJECT ->
                if (trackerController.projectCursorRow == 2 && trackerController.projectCursorColumn >= 1) {
                    if (!peek) openProjectNameEditor()
                    return true
                }
            ScreenType.INSTRUMENT -> {
                val instr = proj.instruments[trackerController.currentInstrument]
                val isSF  = instr.instrumentType == InstrumentType.SOUNDFONT
                val row   = trackerController.instrumentCursorRow
                val col   = trackerController.instrumentCursorColumn
                if (row == 1) { if (!peek) openInstrumentNameEditor(); return true }
                if ((!isSF && row == 12 && col == 1) || (isSF && row == 14 && col == 1)) {
                    if (!peek) openEqEditor(instr.eqSlot.coerceAtLeast(0), EqCallerContext.InstrumentEq(trackerController.currentInstrument))
                    return true
                }
            }
            ScreenType.INST_POOL ->
                if (trackerController.poolCursorColumn == 4) {
                    if (!peek) openEqEditor(proj.instruments[trackerController.currentInstrument].eqSlot.coerceAtLeast(0), EqCallerContext.InstrumentEq(trackerController.currentInstrument))
                    return true
                }
            ScreenType.MIXER ->
                if (trackerController.mixerMasterRow == 1 && trackerController.mixerCursorColumn == 8) {
                    if (!peek) openEqEditor(proj.masterEqSlot.coerceAtLeast(0), EqCallerContext.MasterEq)
                    return true
                }
            ScreenType.EFFECTS -> when (trackerController.effectsCursorRow) {
                EffectModule.ROW_REV_EQ -> { if (!peek) openEqEditor(proj.reverbInputEq.coerceAtLeast(0), EqCallerContext.ReverbInputEq); return true }
                EffectModule.ROW_DLY_EQ -> { if (!peek) openEqEditor(proj.delayInputEq.coerceAtLeast(0), EqCallerContext.DelayInputEq); return true }
            }
            ScreenType.SAMPLE_EDITOR ->
                // Only the EQ-effect slot cell (row 16, col 1, EQ selected). Col 2 = APPLY keeps its
                // A-action; A+DPAD on col 1 still picks the slot.
                if (sampleEditorState.cursorRow == 16 && sampleEditorState.cursorCol == 1 && sampleEditorState.fxType == 3) {
                    if (!peek) openEqEditor(sampleEditorState.fxValue.coerceIn(0, 127), EqCallerContext.SampleEditorFx)
                    return true
                }
            else -> {}
        }
        return false
    }

    /** Defer predicate for single-A: true when the cursor cell opens a sub-screen and no overlay/dialog
     *  already owns the A button. */
    private fun currentCellOpensSubScreen(): Boolean {
        if (qwertyKeyboardState.isOpen || eqEditorState.isOpen || themeEditorState.isOpen || confirmDialogOpen()) return false
        return openSubScreenAtCursor(peek = true)
    }

    fun handleGenericInput(handlerFunction: (CursorContext) -> InputAction) {
        if (themeEditorState.isOpen) return
        if (eqEditorState.isOpen) {
            val eqState = EqState(
                project = trackerController.project,
                slotIndex = eqEditorState.slotIndex,
                cursorRow = eqEditorState.cursorRow,
                callerContext = eqEditorState.callerContext,
                spectrumData = eqSpectrumData
            )
            val context = eqModule.getCursorContext(eqState)
            val action  = handlerFunction(context)
            val result  = eqModule.handleInput(eqState, action) { trackerController.projectVersion++ }
            if (result.eqBandChanged) {
                val slot    = eqEditorState.slotIndex
                val bandIdx = eqEditorState.cursorBand
                val band    = trackerController.project.eqPresets[slot].bands[bandIdx]
                audioBackend.setEqBand(slot, bandIdx, band.type, band.freq, band.gain, band.q)
                when (val ctx = eqEditorState.callerContext) {
                    is EqCallerContext.MasterEq      -> audioBackend.setMasterEqSlot(slot)
                    is EqCallerContext.ReverbInputEq -> audioBackend.setReverbInputEq(slot)
                    is EqCallerContext.DelayInputEq  -> audioBackend.setDelayInputEq(slot)
                    is EqCallerContext.InstrumentEq  -> audioBackend.setInstrumentEqSlot(ctx.instrId, slot)
                    is EqCallerContext.SampleEditorFx -> { }
                }
                trackerController.projectVersion++
            }
            return
        }

        when (trackerController.currentScreen) {
            ScreenType.CHAIN -> {
                val chainState = ChainEditorState(
                    trackerController.project.chains[trackerController.currentChain],
                    trackerController.cursorRow,
                    trackerController.cursorColumn
                )
                val context = chainEditorModule.getCursorContext(chainState)
                val action = handlerFunction(context)
                val result = chainEditorModule.handleInput(chainState, action)
                if (result.modified) {
                    result.lastEditedPhrase?.let { trackerController.lastEditedPhrase = it }
                    result.lastEditedTranspose?.let { trackerController.lastEditedTranspose = it }
                    trackerController.projectVersion++
                }
            }
            ScreenType.PHRASE -> {
                val phraseState = PhraseEditorState(
                    trackerController.project.phrases[trackerController.currentPhrase],
                    trackerController.cursorRow,
                    trackerController.cursorColumn,
                    playbackRow = 0,
                    isPlaying = trackerController.isPlaying()
                )
                val context = phraseEditorModule.getCursorContext(phraseState)
                val action = handlerFunction(context)
                val result = phraseEditorModule.handleInput(phraseState, action, instrumentController)
                if (result.modified) {
                    if (result.lastEditedNote != null || result.lastEditedVolume != null || result.lastEditedInstrument != null) {
                        val step = trackerController.project.phrases[trackerController.currentPhrase].steps[trackerController.cursorRow]
                        if (step.note != Note.EMPTY) {
                            trackerController.lastEditedNote = step.note
                            trackerController.lastEditedVolume = step.volume
                            trackerController.lastEditedInstrument = step.instrument
                            if (notePreviewEnabled && result.lastEditedNote != null) {
                                val instrument = trackerController.project.instruments[step.instrument.coerceIn(0, 127)]
                                val sr = audioEngine.getDeviceSampleRate().toLong().coerceAtLeast(44100L)
                                val stepFrames = (60000.0 / trackerController.project.tempo / 4.0 * sr / 1000.0).toLong()
                                audioEngine.previewNoteWithTimeout(instrument, step.note, trackerController.project, stepFrames)
                            }
                        }
                    }
                    trackerController.projectVersion++
                }
            }
            ScreenType.SONG -> {
                val songState = SongEditorState(
                    trackerController.project,
                    trackerController.cursorRow,
                    cursorTrack = trackerController.cursorColumn,
                    scrollPosition = trackerController.songScrollPosition
                )
                val context = songEditorModule.getCursorContext(songState)
                val action = handlerFunction(context)
                val result = songEditorModule.handleInput(songState, action)
                if (result.modified) {
                    result.lastEditedChain?.let { trackerController.lastEditedChain = it }
                    trackerController.projectVersion++
                }
            }
            ScreenType.PROJECT -> {
                val projectState = ProjectState(
                    project = trackerController.project,
                    cursorRow = trackerController.projectCursorRow,
                    cursorColumn = trackerController.projectCursorColumn,
                    statusMessage = trackerController.statusMessage,
                    isSuccess = trackerController.statusSuccess,
                    isRendering = isRendering,
                    isStemsRendering = isStemsRendering,
                    renderProgress = renderProgress
                )
                val context = projectModule.getCursorContext(projectState)
                val action = handlerFunction(context)
                val result = projectModule.handleInput(projectState, action)
                if (result.modified) {
                    trackerController.projectVersion++
                    val proj = trackerController.project
                    if (trackerController.projectCursorRow == 0 && proj.delaySync) {
                        audioBackend.setDelayParams(proj.delayTime, proj.delayFeedback, proj.delaySync, proj.tempo.toFloat(), proj.delayWet)
                    }
                }
            }
            ScreenType.SETTINGS -> {
                val settingsState = SettingsState(
                    cursorRow = trackerController.settingsCursorRow,
                    cursorColumn = trackerController.settingsCursorColumn,
                    hasPhysicalButtons = deviceAdapter.hasPhysicalGameButtons(),
                    layoutMode = layoutMode,
                    currentSkinId = portraitSkinId,
                    availableSkins = SettingsModule.skinsForLayout(layoutMode),
                    scalingMode = scalingMode,
                    overlayFiles = overlayFiles,
                    overlayName = overlayName,
                    overlayStrength = overlayStrength,
                    buttonSoundEnabled = buttonSoundEnabled,
                    buttonSoundVolume = buttonSoundVolume,
                    buttonVibroEnabled = buttonVibroEnabled,
                    vibroPower = vibroPower,
                    insertBefore = insertBefore,
                    cursorRemember = cursorRemember,
                    notePreviewEnabled = notePreviewEnabled,
                    autosaveResumeAuto = autosaveResumeAuto,
                    visualizerType = appTheme.visualizerType,
                    currentThemeName = appTheme.name
                )
                val context = settingsModule.getCursorContext(settingsState)
                val action = handlerFunction(context)
                val result = settingsModule.handleInput(settingsState, action)
                if (result.modified) {
                    result.layoutMode?.let          { layoutMode          = it }
                    result.skinId?.let              { portraitSkinId      = it }
                    result.scalingMode?.let         { scalingMode         = it }
                    result.overlayName?.let         { overlayName         = it }
                    result.overlayStrength?.let     { overlayStrength     = it }
                    result.buttonSoundEnabled?.let  { buttonSoundEnabled  = it }
                    result.buttonSoundVolume?.let   { buttonSoundVolume   = it }
                    result.buttonVibroEnabled?.let  { buttonVibroEnabled  = it }
                    result.vibroPower?.let          { vibroPower          = it }
                    result.insertBefore?.let        { insertBefore        = it }
                    result.cursorRemember?.let      { cursorRemember      = it }
                    result.notePreviewEnabled?.let  { notePreviewEnabled  = it }
                    result.autosaveResumeAuto?.let  { autosaveResumeAuto  = it }
                    result.visualizerType?.let      { appTheme = appTheme.copy(visualizerType = it) }
                    trackerController.projectVersion++
                }
            }
            ScreenType.FILE_BROWSER -> {
                if (fileBrowserState.mode == FileBrowserModule.BrowserMode.RENAME ||
                    fileBrowserState.mode == FileBrowserModule.BrowserMode.CREATE) {
                    val context = fileBrowserModule.getCursorContext(fileBrowserState)
                    val action = handlerFunction(context)
                    applyFileBrowserInputAction(action)
                }
            }
            ScreenType.SAMPLE_EDITOR -> {
                val context = sampleEditorModule.getCursorContext(sampleEditorState)
                val action = handlerFunction(context)
                val result = sampleEditorModule.handleInput(sampleEditorState, action)
                if (result.modified) {
                    val prevRateMode = sampleEditorState.rateMode
                    sampleEditorState = sampleEditorState.applyResult(result)
                    if (result.rateMode != null && sampleEditorState.rateMode != prevRateMode) {
                        val newFactor = when (sampleEditorState.rateMode) { 1 -> 2; 2 -> 4; else -> 1 }
                        val instId = sampleEditorState.instrumentId
                        val oldLen = sampleEditorState.totalFrames
                        audioEngine.applyRateMode(instId, newFactor)
                        // RATE changes the slot's sample-rate ratio, but the 2-phrase lookahead has
                        // already scheduled notes with the OLD base frequency — they would play the
                        // re-decimated buffer at double/half pitch. Roll the schedule buffer back so
                        // upcoming phrases reschedule with the new ratio (no-op when not playing).
                        trackerController.playbackController.notifyDataChanged()
                        val newLen = audioEngine.getSampleLength(instId)
                        fun scaleFrame(f: Long) =
                            if (oldLen > 0) (f * newLen.toLong() / oldLen).coerceIn(0L, newLen.toLong()) else 0L
                        val channel = when (sampleEditorState.sourceMode) { 0 -> 0; 1 -> 1; else -> 2 }
                        val wfData = if (sampleEditorState.hasStereoData)
                            audioEngine.getSampleWaveformRangeSource(instId, 0, newLen, 620, channel)
                        else
                            audioEngine.getSampleWaveform(instId, 620)
                        sampleEditorState = sampleEditorState.copy(
                            totalFrames    = newLen,
                            sampleRate     = audioEngine.getOriginalSampleRate(instId),
                            waveformData   = wfData,
                            selectionStart = scaleFrame(sampleEditorState.selectionStart),
                            selectionEnd   = scaleFrame(sampleEditorState.selectionEnd),
                            slicePosition  = scaleFrame(sampleEditorState.slicePosition),
                            isModified     = true
                        )
                    }
                }
            }
            ScreenType.INSTRUMENT -> {
                val inst = trackerController.project.instruments[trackerController.currentInstrument]
                val instrumentState = InstrumentState(
                    instrument = inst,
                    cursorRow = trackerController.instrumentCursorRow,
                    cursorColumn = trackerController.instrumentCursorColumn,
                    statusMessage = trackerController.statusMessage,
                    isSuccess = trackerController.statusSuccess,
                    soundfontPresetName = instrumentController.getSoundfontPresetName(
                        trackerController.project
                    ),
                    soundfontPresetCount = instrumentController.getSoundfontPresetCount(inst),
                    soundfontPresetIndex = instrumentController.getSoundfontCurrentPresetIndex(inst)
                )
                val context = instrumentModule.getCursorContext(instrumentState)
                val action = handlerFunction(context)
                val result = instrumentModule.handleInput(instrumentState, action, instrumentController)
                if (result.modified) trackerController.projectVersion++
            }
            ScreenType.INST_POOL -> {
                val poolState = InstrumentPoolState(
                    project = trackerController.project,
                    selectedInstrument = trackerController.currentInstrument,
                    cursorColumn = trackerController.poolCursorColumn
                )
                val context = instrumentPoolModule.getCursorContext(poolState)
                val action = handlerFunction(context)
                if (instrumentPoolModule.handleInput(poolState, action, instrumentController))
                    trackerController.projectVersion++
            }
            ScreenType.MIXER -> {
                val mixerState = MixerState(
                    project = trackerController.project,
                    cursorColumn = trackerController.mixerCursorColumn,
                    mixerMasterRow = trackerController.mixerMasterRow,
                    trackPeaks = trackPeakBuffer,
                    masterPeaks = masterPeakBuffer,
                    reverbPeaks = floatArrayOf(sendPeakBuffer[0], sendPeakBuffer[1]),
                    delayPeaks = floatArrayOf(sendPeakBuffer[2], sendPeakBuffer[3])
                )
                val context = mixerModule.getCursorContext(mixerState)
                val action = handlerFunction(context)
                val result = mixerModule.handleInput(mixerState, action) { trackerController.projectVersion++ }
                if (result.modified) {
                    val proj = trackerController.project
                    val cursorCol = trackerController.mixerCursorColumn
                    val masterRow = trackerController.mixerMasterRow
                    when {
                        result.masterEqChanged   -> { val slot = proj.masterEqSlot; if (slot >= 0) audioBackend.setMasterEqSlot(slot); trackerController.projectVersion++ }
                        result.ottDepthChanged        -> { audioBackend.setOttDepth(proj.ottDepth); trackerController.projectVersion++ }
                        result.dustDepthChanged       -> { audioBackend.setDustDepth(proj.dustDepth); trackerController.projectVersion++ }
                        result.limiterPreGainChanged  -> { audioBackend.setLimiterPreGain(proj.limiterPreGain); trackerController.projectVersion++ }
                        result.reverbWetChanged       -> { audioBackend.setReverbParams(proj.reverbFeedback, proj.reverbDamp, proj.reverbWet); trackerController.projectVersion++ }
                        result.delayWetChanged        -> { audioBackend.setDelayParams(proj.delayTime, proj.delayFeedback, proj.delaySync, proj.tempo.toFloat(), proj.delayWet); trackerController.projectVersion++ }
                        masterRow == 0 && cursorCol < 8 -> audioBackend.setTrackVolume(cursorCol, VolumeUtils.hexToFloat(proj.tracks[cursorCol].volume))
                        masterRow == 0 -> audioBackend.setMasterVolume(VolumeUtils.hexToFloat(proj.masterVolume))
                    }
                }
            }
            ScreenType.EFFECTS -> {
                val effectState = EffectState(
                    project = trackerController.project,
                    cursorRow = trackerController.effectsCursorRow
                )
                val context = effectModule.getCursorContext(effectState)
                val action  = handlerFunction(context)
                val result  = effectModule.handleInput(effectState, action) { trackerController.projectVersion++ }
                if (result.modified) {
                    val proj = trackerController.project
                    when {
                        result.masterFxChanged        -> { audioBackend.setMasterFx(proj.masterBusFx); trackerController.projectVersion++ }
                        result.reverbParamsChanged    -> { audioBackend.setReverbParams(proj.reverbFeedback, proj.reverbDamp, proj.reverbWet); if (proj.reverbInputEq >= 0) audioBackend.setReverbInputEq(proj.reverbInputEq); trackerController.projectVersion++ }
                        result.delayReverbSendChanged -> { audioBackend.setDelayReverbSend(proj.delayReverbSend); trackerController.projectVersion++ }
                        result.delayParamsChanged     -> { audioBackend.setDelayParams(proj.delayTime, proj.delayFeedback, proj.delaySync, proj.tempo.toFloat(), proj.delayWet); if (proj.delayInputEq >= 0) audioBackend.setDelayInputEq(proj.delayInputEq); trackerController.projectVersion++ }
                        result.masterEqChanged        -> { if (proj.masterEqSlot >= 0) audioBackend.setMasterEqSlot(proj.masterEqSlot); trackerController.projectVersion++ }
                    }
                }
            }
            ScreenType.TABLE -> {
                val tableState = TableState(
                    trackerController.project.tables[trackerController.currentTable],
                    trackerController.tableCursorRow,
                    trackerController.tableCursorColumn,
                    playbackRow = null,
                    ticRate = trackerController.project.instruments.getOrNull(trackerController.currentInstrument)?.tableTicRate
                        ?: 0x06,
                    selectionMode = trackerController.inputController.isSelectionModeActive(),
                    isCellSelected = { row, col ->
                        trackerController.inputController.isCellSelected(
                            row,
                            col
                        )
                    }
                )
                val context = tableModule.getCursorContext(tableState)
                val action = handlerFunction(context)
                val result = tableModule.handleInput(tableState, action)
                if (result.modified) {
                    trackerController.projectVersion++
                    audioEngine.invalidateTable(trackerController.currentTable)
                }
            }
            ScreenType.GROOVE -> {
                val grooveState = GrooveState(
                    groove = trackerController.project.grooves[trackerController.currentGroove],
                    cursorRow = trackerController.grooveCursorRow,
                    cursorColumn = 1
                )
                val context = grooveModule.getCursorContext(grooveState)
                val action = handlerFunction(context)
                val result = grooveModule.handleInput(grooveState, action)
                if (result.modified) trackerController.projectVersion++
            }
            ScreenType.MODS -> {
                val modState = ModulationState(
                    instrument = trackerController.project.instruments[trackerController.currentInstrument],
                    cursorRow = trackerController.modCursorRow,
                    cursorPair = trackerController.modCursorPair,
                    cursorSide = trackerController.modCursorSide
                )
                val context = modulationModule.getCursorContext(modState)
                val action = handlerFunction(context)
                val result = modulationModule.handleInput(modState, action)
                if (result.modified) trackerController.projectVersion++
            }
            else -> { }
        }
    }

    fun handleSelectionOrSingleIncrement(handlerFunction: (CursorContext) -> InputAction) {
        if (!trackerController.inputController.isSelectionModeActive()) {
            handleGenericInput(handlerFunction)
            return
        }
        val bounds = trackerController.inputController.getSelectionBounds() ?: return
        when (trackerController.currentScreen) {
            ScreenType.PHRASE, ScreenType.CHAIN, ScreenType.SONG -> {
                val savedRow = trackerController.cursorRow
                for (row in bounds.topLeftRow..bounds.bottomRightRow) {
                    trackerController.cursorRow = row
                    handleGenericInput(handlerFunction)
                }
                trackerController.cursorRow = savedRow
            }
            ScreenType.TABLE -> {
                val savedRow = trackerController.tableCursorRow
                for (row in bounds.topLeftRow..bounds.bottomRightRow) {
                    trackerController.tableCursorRow = row
                    handleGenericInput(handlerFunction)
                }
                trackerController.tableCursorRow = savedRow
            }
            else -> handleGenericInput(handlerFunction)
        }
    }

    fun applyInputAction(action: InputAction) {
        when (action) {
            is InputAction.NAVIGATE_UP -> {
                when (trackerController.currentScreen) {
                    ScreenType.FILE_BROWSER -> {
                        if (fileBrowserState.items.isNotEmpty()) {
                            val newCursor = if (fileBrowserState.cursor > 0) fileBrowserState.cursor - 1 else fileBrowserState.items.size - 1
                            val newScroll = when {
                                newCursor < fileBrowserState.scroll -> newCursor
                                newCursor >= fileBrowserState.scroll + FileBrowserModule.VISIBLE_ROWS ->
                                    (newCursor - FileBrowserModule.VISIBLE_ROWS + 1).coerceAtLeast(0)
                                else -> fileBrowserState.scroll
                            }
                            fileBrowserState = fileBrowserState.copy(cursor = newCursor, scroll = newScroll)
                        }
                    }
                    ScreenType.SAMPLE_EDITOR -> {
                        val newRow = SampleEditorModule.rowAbove(sampleEditorState.cursorRow, sampleEditorState.sliceMethod)
                        sampleEditorState = sampleEditorState.copy(
                            cursorRow = newRow,
                            cursorCol = sampleEditorState.cursorCol.coerceAtMost(SampleEditorModule.maxColForRow(newRow, sampleEditorState.sliceMethod))
                        )
                    }
                    else -> trackerController.moveCursorUp()
                }
            }
            is InputAction.NAVIGATE_DOWN -> {
                when (trackerController.currentScreen) {
                    ScreenType.FILE_BROWSER -> {
                        if (fileBrowserState.items.isNotEmpty()) {
                            val newCursor = if (fileBrowserState.cursor < fileBrowserState.items.size - 1) fileBrowserState.cursor + 1 else 0
                            val newScroll = when {
                                newCursor >= fileBrowserState.scroll + FileBrowserModule.VISIBLE_ROWS ->
                                    newCursor - FileBrowserModule.VISIBLE_ROWS + 1
                                newCursor < fileBrowserState.scroll -> newCursor
                                else -> fileBrowserState.scroll
                            }
                            fileBrowserState = fileBrowserState.copy(cursor = newCursor, scroll = newScroll)
                        }
                    }
                    ScreenType.SAMPLE_EDITOR -> {
                        val newRow = SampleEditorModule.rowBelow(sampleEditorState.cursorRow, sampleEditorState.sliceMethod)
                        sampleEditorState = sampleEditorState.copy(
                            cursorRow = newRow,
                            cursorCol = sampleEditorState.cursorCol.coerceAtMost(SampleEditorModule.maxColForRow(newRow, sampleEditorState.sliceMethod))
                        )
                    }
                    else -> trackerController.moveCursorDown()
                }
            }
            is InputAction.NAVIGATE_LEFT -> {
                when (trackerController.currentScreen) {
                    ScreenType.FILE_BROWSER -> {
                        when (fileBrowserState.mode) {
                            FileBrowserModule.BrowserMode.RENAME, FileBrowserModule.BrowserMode.CREATE -> {
                                if (fileBrowserState.renameCursor > 0)
                                    fileBrowserState = fileBrowserState.copy(renameCursor = fileBrowserState.renameCursor - 1)
                            }
                            else -> {
                                if (fileBrowserState.items.isNotEmpty()) {
                                    val newCursor = (fileBrowserState.cursor - FileBrowserModule.VISIBLE_ROWS).coerceAtLeast(0)
                                    val newScroll = when {
                                        newCursor < fileBrowserState.scroll -> newCursor
                                        newCursor >= fileBrowserState.scroll + FileBrowserModule.VISIBLE_ROWS ->
                                            (newCursor - FileBrowserModule.VISIBLE_ROWS + 1).coerceAtLeast(0)
                                        else -> fileBrowserState.scroll
                                    }
                                    fileBrowserState = fileBrowserState.copy(cursor = newCursor, scroll = newScroll)
                                }
                            }
                        }
                    }
                    ScreenType.SAMPLE_EDITOR -> {
                        val row = sampleEditorState.cursorRow
                        val maxCol = SampleEditorModule.maxColForRow(row, sampleEditorState.sliceMethod)
                        val newCol = if (row in 13..14) {
                            if (sampleEditorState.cursorCol == 0) maxCol else sampleEditorState.cursorCol - 1
                        } else {
                            (sampleEditorState.cursorCol - 1).coerceAtLeast(0)
                        }
                        sampleEditorState = sampleEditorState.copy(cursorCol = newCol)
                    }
                    else -> trackerController.moveCursorLeft()
                }
            }
            is InputAction.NAVIGATE_RIGHT -> {
                when (trackerController.currentScreen) {
                    ScreenType.FILE_BROWSER -> {
                        when (fileBrowserState.mode) {
                            FileBrowserModule.BrowserMode.RENAME, FileBrowserModule.BrowserMode.CREATE -> {
                                if (fileBrowserState.renameCursor < 11)
                                    fileBrowserState = fileBrowserState.copy(renameCursor = fileBrowserState.renameCursor + 1)
                            }
                            else -> {
                                if (fileBrowserState.items.isNotEmpty()) {
                                    val newCursor = (fileBrowserState.cursor + FileBrowserModule.VISIBLE_ROWS).coerceAtMost(fileBrowserState.items.size - 1)
                                    val newScroll = when {
                                        newCursor >= fileBrowserState.scroll + FileBrowserModule.VISIBLE_ROWS ->
                                            newCursor - FileBrowserModule.VISIBLE_ROWS + 1
                                        newCursor < fileBrowserState.scroll -> newCursor
                                        else -> fileBrowserState.scroll
                                    }
                                    fileBrowserState = fileBrowserState.copy(cursor = newCursor, scroll = newScroll)
                                }
                            }
                        }
                    }
                    ScreenType.SAMPLE_EDITOR -> {
                        val row = sampleEditorState.cursorRow
                        val maxCol = SampleEditorModule.maxColForRow(row, sampleEditorState.sliceMethod)
                        val newCol = if (row in 13..14) {
                            (sampleEditorState.cursorCol + 1) % (maxCol + 1)
                        } else {
                            (sampleEditorState.cursorCol + 1).coerceAtMost(maxCol)
                        }
                        sampleEditorState = sampleEditorState.copy(cursorCol = newCol)
                    }
                    else -> trackerController.moveCursorRight()
                }
            }
            else -> { }
        }
    }

    fun handleDPadNavigation(handlerFunction: () -> InputAction) {
        if (trackerController.inputController.isSelectionModeActive()) {
            val action = handlerFunction()
            val direction = when (action) {
                InputAction.NAVIGATE_UP    -> "UP"
                InputAction.NAVIGATE_DOWN  -> "DOWN"
                InputAction.NAVIGATE_LEFT  -> "LEFT"
                InputAction.NAVIGATE_RIGHT -> "RIGHT"
                else -> null
            }
            if (direction != null) {
                val maxColumn = when (trackerController.currentScreen) {
                    ScreenType.PHRASE -> 9; ScreenType.CHAIN -> 2; ScreenType.SONG -> 8; else -> 1
                }
                // SONG is 256 rows deep (16 visible); PHRASE/CHAIN/TABLE are a single 16-row screen.
                val maxRow = if (trackerController.currentScreen == ScreenType.SONG) 255 else 15
                val edgeBefore = trackerController.inputController.selectionEnd
                trackerController.inputController.expandSelection(direction, maxRow, maxColumn)
                // Move the cursor with the selection's active edge so it stays on screen — fixes the
                // SONG case where the anchored cursor scrolled out of view once the edge passed row 16.
                // Only when the edge actually moved, so hitting a clamp (or DPAD in SCREEN-select mode)
                // can't teleport the cursor. Copy/cut/delete use the selection bounds (not the cursor),
                // so what's affected is unchanged; paste re-anchors wherever the user navigates next.
                val edge = trackerController.inputController.selectionEnd
                val sc = trackerController.currentScreen
                if (edge != null && edge != edgeBefore &&
                    (sc == ScreenType.PHRASE || sc == ScreenType.CHAIN || sc == ScreenType.SONG)) {
                    trackerController.cursorRow = edge.row
                    trackerController.cursorColumn = edge.column
                    if (sc == ScreenType.SONG) trackerController.scrollSongToRow(edge.row)
                }
                return
            }
        }
        val action = handlerFunction()
        applyInputAction(action)
    }

    fun handleDPadUp() {
        if (qwertyKeyboardState.isOpen) { qwertyKeyboardState = qwertyKeyboardState.moveCursorUp() }
        // All confirm dialogs are pure A-confirm/B-cancel — swallow DPAD so the cursor can't
        // move on the screen behind the modal (was only guarding CLEAN, and only Up/Down).
        else if (confirmDialogOpen()) { return }
        else if (themeEditorState.isOpen) {
            val row = themeEditorState.cursorRow
            themeEditorState = themeEditorState.copy(cursorRow = if (row > 0) row - 1 else ThemeEditorModule.MAX_ROW)
        }
        else if (eqEditorState.isOpen) {
            val p = eqEditorState.cursorParam
            if (p > 0) eqEditorState = eqEditorState.copy(cursorRow = eqEditorState.cursorBand * 4 + p - 1)
        }
        else handleDPadNavigation { trackerController.inputController.handleDPadUp() }
    }

    fun handleDPadDown() {
        if (qwertyKeyboardState.isOpen) { qwertyKeyboardState = qwertyKeyboardState.moveCursorDown() }
        else if (confirmDialogOpen()) { return }
        else if (themeEditorState.isOpen) {
            val row = themeEditorState.cursorRow
            themeEditorState = themeEditorState.copy(cursorRow = if (row < ThemeEditorModule.MAX_ROW) row + 1 else 0)
        }
        else if (eqEditorState.isOpen) {
            val p = eqEditorState.cursorParam
            if (p < 3) eqEditorState = eqEditorState.copy(cursorRow = eqEditorState.cursorBand * 4 + p + 1)
        }
        else handleDPadNavigation { trackerController.inputController.handleDPadDown() }
    }

    fun handleDPadLeft() {
        if (qwertyKeyboardState.isOpen) { qwertyKeyboardState = qwertyKeyboardState.moveCursorLeft() }
        else if (confirmDialogOpen()) { return }
        else if (themeEditorState.isOpen) {
            val ch = themeEditorState.cursorChannel
            themeEditorState = themeEditorState.copy(cursorChannel = if (ch > 0) ch - 1 else 2)
        }
        else if (eqEditorState.isOpen) {
            val b = eqEditorState.cursorBand
            if (b > 0) eqEditorState = eqEditorState.copy(cursorRow = (b - 1) * 4 + eqEditorState.cursorParam)
        }
        else handleDPadNavigation { trackerController.inputController.handleDPadLeft() }
    }

    fun handleDPadRight() {
        if (qwertyKeyboardState.isOpen) { qwertyKeyboardState = qwertyKeyboardState.moveCursorRight() }
        else if (confirmDialogOpen()) { return }
        else if (themeEditorState.isOpen) {
            val ch = themeEditorState.cursorChannel
            themeEditorState = themeEditorState.copy(cursorChannel = if (ch < 2) ch + 1 else 0)
        }
        else if (eqEditorState.isOpen) {
            val b = eqEditorState.cursorBand
            if (b < 2) eqEditorState = eqEditorState.copy(cursorRow = (b + 1) * 4 + eqEditorState.cursorParam)
        }
        else handleDPadNavigation { trackerController.inputController.handleDPadRight() }
    }

    fun handleButtonA() {
        if (qwertyKeyboardState.isOpen) {
            if (qwertyKeyboardState.isOnActionRow()) {
                // On-screen ABORT/APPLY buttons mirror physical SELECT/START.
                if (qwertyKeyboardState.keyCursorCol == 0) qwertyKeyboardState = QwertyKeyboardState()  // ABORT
                else handleStart()                                                                       // APPLY
            } else {
                qwertyKeyboardState = qwertyKeyboardState.insertCurrentKey()
            }
            return
        }
        if (showCleanDialog) {
            val target = cleanDialogTarget
            showCleanDialog = false
            if (target == "SEQ") {
                trackerController.cleanUnusedSeq()
            } else {
                trackerController.cleanUnusedInst()
                // Compacting replaced unused instruments with empty ones in the data model, but their
                // native sample/SF2 buffers are still loaded — reload from the compacted project to
                // actually free them (clearAll + reload). Without this the RAM only drops after a
                // save+reload. REVIEW-3 5.1.
                reloadProjectSamples()
            }
            return
        }
        if (showNewProjectDialog) {
            showNewProjectDialog = false
            trackerController.newProject()
            audioEngine.clearLoadedTables()
            return
        }
        if (showRecoveryDialog) {
            showRecoveryDialog = false
            // A = recover: load the autosave (stays dirty so the user is nudged to Save it for real),
            // then reload its samples — the autosave stores paths, not PCM. REVIEW-3 5.3 Phase B.
            if (trackerController.recoverFromAutosave()) reloadProjectSamples()
            return
        }
        if (showInstrTypeDialog) {
            showInstrTypeDialog = false
            instrumentController.currentInstrument = trackerController.currentInstrument
            val inst = trackerController.project.instruments[trackerController.currentInstrument]
            val newType = if (inst.instrumentType == InstrumentType.SOUNDFONT) InstrumentType.SAMPLER else InstrumentType.SOUNDFONT
            instrumentController.setInstrumentType(trackerController.project, newType)
            trackerController.projectVersion++
            return
        }
        if (trackerController.currentScreen == ScreenType.SAMPLE_EDITOR && sampleEditorState.showConfirmClose) {
            audioEngine.restoreFxPreviewBackup()
            audioEngine.freeSampleUndo(sampleEditorState.instrumentId)  // editor closing — undo unreachable (REVIEW-3 1.1)
            sampleEditorState = sampleEditorState.copy(showConfirmClose = false, isModified = false)
            trackerController.currentScreen = previousScreen
            return
        }
        if (eqEditorState.isOpen) return
        if (themeEditorState.isOpen) {
            if (themeEditorState.cursorRow == 0) {
                when (themeEditorState.cursorChannel) {
                    1 -> {  // SAVE — open QWERTY to name the theme
                        val themesDir = fileController.getThemesDirectory()
                        val name = appTheme.name.replace(Regex("[^a-zA-Z0-9_]"), "_").ifEmpty { "THEME" }
                        qwertyKeyboardState = QwertyKeyboardState(
                            isOpen = true,
                            text = name,
                            maxLength = 20,
                            textCursor = name.length.coerceAtMost(20),
                            keyCursorRow = 0,
                            keyCursorCol = 0,
                            layout = 0,
                            fieldLabel = "SAVE THEME:",
                            originalText = name,
                            insertBefore = insertBefore,
                            context = QwertyContext.THEME_SAVE,
                            contextExtra = themesDir
                        )
                    }
                    2 -> {  // LOAD — open file browser in Themes dir filtered to .ptt
                        val themesDir = fileController.getThemesDirectory()
                        themeEditorState = ThemeEditorState()
                        instrumentFileBrowserAction = "LOAD_THEME"
                        previousScreen = trackerController.currentScreen
                        fileBrowserState = fileBrowserModule.navigateToFolder(fileBrowserState.copy(fileExtensions = listOf("ptt"), mode = FileBrowserModule.BrowserMode.NORMAL, statusMessage = ""),
                            File(themesDir)
                        )
                        trackerController.currentScreen = ScreenType.FILE_BROWSER
                    }
                }
            }
            return
        }

        // Item 3: A on a sub-screen-opening cell opens it. The InputMapper has already deferred this
        // to release for those cells, so a held A+DPAD/A+B on the same cell edits/resets instead.
        if (openSubScreenAtCursor(peek = false)) return

        when (trackerController.currentScreen) {
            ScreenType.FILE_BROWSER -> handleConfirmAFileBrowser()

            ScreenType.PROJECT -> handleConfirmAProject()

            ScreenType.SETTINGS -> handleConfirmASettings()

            ScreenType.INSTRUMENT -> handleConfirmAInstrument()

            ScreenType.INST_POOL -> handleConfirmAInstrumentPool()

            ScreenType.SAMPLE_EDITOR -> handleConfirmASampleEditor()

            ScreenType.PHRASE -> handleConfirmAPhrase()

            ScreenType.CHAIN -> handleConfirmAChain()

            ScreenType.SONG -> handleConfirmASong()

            else -> { }
        }
    }

    // ── Per-screen A-button (confirm/insert) handlers, extracted from handleButtonA (REVIEW-3 4.1). ──
    //    Pure relocations of the former `when (currentScreen)` branches — behaviour unchanged. The
    //    global modal-dialog guards stay in handleButtonA; this is the per-screen orchestration only.

    private fun handleConfirmAFileBrowser() {
        when (fileBrowserState.mode) {
            FileBrowserModule.BrowserMode.NORMAL -> {
                val item = fileBrowserState.items.getOrNull(fileBrowserState.cursor)
                when (item) {
                    is FileBrowserModule.BrowserItem.Parent -> fileBrowserState = fileBrowserModule.navigateToParent(fileBrowserState)
                    is FileBrowserModule.BrowserItem.Folder -> fileBrowserState = fileBrowserModule.navigateToFolder(fileBrowserState, item.file)
                    is FileBrowserModule.BrowserItem.FileItem -> {
                        when (previousScreen) {
                            ScreenType.PROJECT -> {
                                val result = trackerController.loadProject(item.file.toFileInfo())
                                when (result) {
                                    is FileController.LoadResult.Success -> {
                                        result.project.name = item.file.nameWithoutExtension.take(20)
                                        trackerController.project = result.project
                                        reloadProjectSamples()
                                        audioEngine.clearLoadedTables()
                                        trackerController.statusMessage = "LOADED: ${item.file.nameWithoutExtension}"
                                        trackerController.statusSuccess = true
                                        trackerController.projectVersion++
                                        trackerController.markProjectClean()  // fresh load isn't dirty (REVIEW-3 5.3 fix)
                                        trackerController.currentScreen = previousScreen
                                    }
                                    is FileController.LoadResult.Error -> fileBrowserState = fileBrowserState.copy(statusMessage = "LOAD FAILED", statusSuccess = false)
                                }
                            }
                            ScreenType.SAMPLE_EDITOR -> {
                                if (item.file.extension.lowercase() == "wav") {
                                    val result = instrumentController.loadSampleFromFile(trackerController.project, item.file.absolutePath)
                                    if (result is LoadResult.Success) {
                                        trackerController.projectVersion++
                                        sampleEditorState = sampleEditorState.copy(sampleFilePath = item.file.absolutePath, isModified = false)
                                        trackerController.currentScreen = ScreenType.SAMPLE_EDITOR
                                    } else {
                                        fileBrowserState = fileBrowserState.copy(statusMessage = "LOAD FAILED", statusSuccess = false)
                                    }
                                }
                            }
                            ScreenType.INSTRUMENT, ScreenType.INST_POOL -> {
                                when (instrumentFileBrowserAction) {
                                    "LOAD_PRESET" -> {
                                        instrumentController.loadPreset(trackerController.project, item.file.absolutePath, videoExtractor)
                                        trackerController.projectVersion++
                                        trackerController.currentScreen = previousScreen
                                    }
                                    "LOAD_SOURCE" -> {
                                        val ext = item.file.extension.lowercase()
                                        // Default-named slots adopt the loaded file's name; custom names are kept.
                                        fun autoName() = trackerController.project.instruments[trackerController.currentInstrument].run {
                                            if (hasDefaultName()) name = item.file.nameWithoutExtension.take(20)
                                        }
                                        if (ext == "sf2" || ext == "sf3") {
                                            instrumentController.loadSoundfont(trackerController.project, item.file.absolutePath)
                                            autoName()
                                            trackerController.projectVersion++
                                            trackerController.currentScreen = previousScreen
                                        } else if (ext == "wav") {
                                            val result = instrumentController.loadSampleFromFile(trackerController.project, item.file.absolutePath)
                                            if (result is LoadResult.Success) {
                                                autoName()
                                                trackerController.projectVersion++
                                                trackerController.currentScreen = previousScreen
                                            } else {
                                                fileBrowserState = fileBrowserState.copy(statusMessage = "LOAD FAILED", statusSuccess = false)
                                            }
                                        } else if (ext == "mp3") {
                                            // Compressed audio → decode in-memory (no WAV written); instrument keeps the .mp3 path.
                                            val srcPath = item.file.absolutePath
                                            fileBrowserState = fileBrowserState.copy(statusMessage = "DECODING...", statusSuccess = true)
                                            coroutineScope.launch(Dispatchers.Default) {
                                                val result = instrumentController.loadSampleFromCompressed(trackerController.project, srcPath, videoExtractor)
                                                withContext(Dispatchers.Main) {
                                                    if (result is LoadResult.Success) {
                                                        autoName()
                                                        trackerController.projectVersion++
                                                        trackerController.currentScreen = previousScreen
                                                    } else {
                                                        val why = (result as? LoadResult.Error)?.message ?: "LOAD FAILED"
                                                        fileBrowserState = fileBrowserState.copy(statusMessage = why.uppercase().take(40), statusSuccess = false)
                                                    }
                                                }
                                            }
                                        } else if (videoExtractor.isSupportedVideo(item.file.absolutePath)) {
                                            val suggestedName = item.file.nameWithoutExtension + "_audio"
                                            qwertyKeyboardState = QwertyKeyboardState(
                                                isOpen = true,
                                                text = suggestedName,
                                                maxLength = 40,
                                                textCursor = suggestedName.length,
                                                fieldLabel = "SAVE AS:",
                                                originalText = suggestedName,
                                                insertBefore = insertBefore,
                                                clearOnFirstB = true,
                                                context = QwertyContext.VIDEO_EXTRACT,
                                                contextExtra = item.file.absolutePath
                                            )
                                        } else {
                                            fileBrowserState = fileBrowserState.copy(statusMessage = "LOAD FAILED", statusSuccess = false)
                                        }
                                    }
                                    "LOAD_SAMPLE_EDITOR" -> {
                                        if (item.file.extension.lowercase() == "wav") {
                                            val result = instrumentController.loadSampleFromFile(trackerController.project, item.file.absolutePath)
                                            if (result is LoadResult.Success) {
                                                trackerController.projectVersion++
                                                sampleEditorState = sampleEditorState.copy(sampleFilePath = item.file.absolutePath, isModified = false)
                                                trackerController.currentScreen = ScreenType.SAMPLE_EDITOR
                                            } else {
                                                fileBrowserState = fileBrowserState.copy(statusMessage = "LOAD FAILED", statusSuccess = false)
                                            }
                                        }
                                    }
                                    else -> {
                                        val ext = item.file.extension.lowercase()
                                        if (ext == "wav") {
                                            val result = instrumentController.loadSampleFromFile(trackerController.project, item.file.absolutePath)
                                            if (result is LoadResult.Success) {
                                                trackerController.projectVersion++
                                                trackerController.currentScreen = previousScreen
                                            } else {
                                                fileBrowserState = fileBrowserState.copy(statusMessage = "LOAD FAILED", statusSuccess = false)
                                            }
                                        } else if (ext == "mp3") {
                                            // Compressed audio → decode in-memory (no WAV written); instrument keeps the .mp3 path.
                                            val srcPath = item.file.absolutePath
                                            fileBrowserState = fileBrowserState.copy(statusMessage = "DECODING...", statusSuccess = true)
                                            coroutineScope.launch(Dispatchers.Default) {
                                                val result = instrumentController.loadSampleFromCompressed(trackerController.project, srcPath, videoExtractor)
                                                withContext(Dispatchers.Main) {
                                                    if (result is LoadResult.Success) {
                                                        trackerController.projectVersion++
                                                        trackerController.currentScreen = previousScreen
                                                    } else {
                                                        val why = (result as? LoadResult.Error)?.message ?: "LOAD FAILED"
                                                        fileBrowserState = fileBrowserState.copy(statusMessage = why.uppercase().take(40), statusSuccess = false)
                                                    }
                                                }
                                            }
                                        } else if (videoExtractor.isSupportedVideo(item.file.absolutePath)) {
                                            val suggestedName = item.file.nameWithoutExtension + "_audio"
                                            qwertyKeyboardState = QwertyKeyboardState(
                                                isOpen = true,
                                                text = suggestedName,
                                                maxLength = 40,
                                                textCursor = suggestedName.length,
                                                fieldLabel = "SAVE AS:",
                                                originalText = suggestedName,
                                                insertBefore = insertBefore,
                                                clearOnFirstB = true,
                                                context = QwertyContext.VIDEO_EXTRACT,
                                                contextExtra = item.file.absolutePath
                                            )
                                        } else {
                                            fileBrowserState = fileBrowserState.copy(statusMessage = "LOAD FAILED", statusSuccess = false)
                                        }
                                    }
                                }
                            }
                            ScreenType.SETTINGS -> {
                                if (instrumentFileBrowserAction == "LOAD_THEME" && item.file.extension.lowercase() == "ptt") {
                                    try {
                                        val loaded = Json { ignoreUnknownKeys = true; coerceInputValues = true }.decodeFromString<AppTheme>(item.file.readText())
                                        appTheme = loaded.copy(visualizerType = appTheme.visualizerType)
                                        instrumentFileBrowserAction = ""
                                        trackerController.currentScreen = previousScreen
                                        themeEditorState = ThemeEditorState(isOpen = true)
                                    } catch (e: Exception) {
                                        fileBrowserState = fileBrowserState.copy(statusMessage = "LOAD FAILED", statusSuccess = false)
                                    }
                                }
                            }
                            else -> {
                                val result = trackerController.loadProject(item.file.toFileInfo())
                                when (result) {
                                    is FileController.LoadResult.Success -> {
                                        trackerController.project = result.project
                                        reloadProjectSamples()
                                        audioEngine.clearLoadedTables()
                                        trackerController.projectVersion++
                                        trackerController.markProjectClean()  // fresh load isn't dirty (REVIEW-3 5.3 fix)
                                        trackerController.currentScreen = previousScreen
                                    }
                                    is FileController.LoadResult.Error -> fileBrowserState = fileBrowserState.copy(statusMessage = "LOAD FAILED", statusSuccess = false)
                                }
                            }
                        }
                    }
                    null -> { }
                }
            }
            FileBrowserModule.BrowserMode.DELETE -> {
                val item = fileBrowserState.items.getOrNull(fileBrowserState.cursor)
                if (item != null && item !is FileBrowserModule.BrowserItem.Parent) {
                    val deleted = fileController.deleteFileOrFolder(item.file.absolutePath)
                    if (deleted) {
                        val newItems = fileBrowserModule.buildItemList(fileBrowserState.currentDirectory, fileBrowserState.fileExtension, fileBrowserState.fileExtensions)
                        val sortedItems = fileBrowserModule.sortItems(newItems, fileBrowserState.sortMode)
                        val newCursor = fileBrowserState.cursor.coerceAtMost((newItems.size - 1).coerceAtLeast(0))
                        fileBrowserState = fileBrowserState.copy(items = sortedItems, cursor = newCursor, mode = FileBrowserModule.BrowserMode.NORMAL, statusMessage = "Deleted: ${item.displayName}", statusSuccess = true)
                    } else {
                        fileBrowserState = fileBrowserState.copy(mode = FileBrowserModule.BrowserMode.NORMAL, statusMessage = "Delete failed", statusSuccess = false)
                    }
                }
            }
            FileBrowserModule.BrowserMode.RENAME -> fileBrowserState = fileBrowserState.copy(mode = FileBrowserModule.BrowserMode.NORMAL)
            FileBrowserModule.BrowserMode.CREATE -> fileBrowserState = fileBrowserState.copy(mode = FileBrowserModule.BrowserMode.NORMAL)
        }
    }

    private fun handleConfirmAProject() {
        when (trackerController.projectCursorRow) {
            3 -> when (trackerController.projectCursorColumn) {
                1 -> {
                    when (val result = trackerController.saveProject(trackerController.project.name)) {
                        is FileController.SaveResult.Success -> { trackerController.statusMessage = "SAVED"; trackerController.statusSuccess = true }
                        is FileController.SaveResult.Error   -> { trackerController.statusMessage = "SAVE FAILED"; trackerController.statusSuccess = false }
                    }
                }
                2 -> {
                    previousScreen = trackerController.currentScreen
                    trackerController.currentScreen = ScreenType.FILE_BROWSER
                    val projectsDir = File(fileController.getProjectsDirectory())
                    fileBrowserState = fileBrowserModule.navigateToFolder(fileBrowserState.copy(fileExtension = "ptp", fileExtensions = null, mode = FileBrowserModule.BrowserMode.NORMAL, statusMessage = ""), projectsDir)
                }
                3 -> {
                    if (trackerController.isProjectDirty) {
                        showNewProjectDialog = true
                    } else {
                        trackerController.newProject()
                        audioEngine.clearLoadedTables()
                    }
                }
            }
            4 -> when (trackerController.projectCursorColumn) {
                1 -> {
                    if (!isRendering) {
                        isRendering = true
                        isStemsRendering = false
                        renderProgress = 0f
                        trackerController.statusMessage = "RENDERING..."
                        trackerController.statusSuccess = true
                        trackerController.stopPlayback()
                        coroutineScope.launch(Dispatchers.Default) {
                            val result = renderController.renderSongToWav(
                                project = trackerController.project,
                                progressCallback = object : RenderController.ProgressCallback {
                                    override fun onProgress(progress: Float, message: String) { renderProgress = progress }
                                }
                            )
                            withContext(Dispatchers.Main) {
                                isRendering = false
                                isStemsRendering = false
                                renderProgress = 0f
                                when (result) {
                                    is RenderController.RenderResult.Success -> { trackerController.statusMessage = "EXPORTED!"; trackerController.statusSuccess = true }
                                    is RenderController.RenderResult.Error   -> { trackerController.statusMessage = "EXPORT FAILED"; trackerController.statusSuccess = false }
                                }
                            }
                        }
                    }
                }
                2 -> {
                    if (!isRendering) {
                        isRendering = true
                        isStemsRendering = true
                        renderProgress = 0f
                        trackerController.statusMessage = "RENDERING STEMS..."
                        trackerController.statusSuccess = true
                        trackerController.stopPlayback()
                        coroutineScope.launch(Dispatchers.Default) {
                            val result = renderController.renderStemsToWav(
                                project = trackerController.project,
                                progressCallback = object : RenderController.ProgressCallback {
                                    override fun onProgress(progress: Float, message: String) { renderProgress = progress }
                                }
                            )
                            withContext(Dispatchers.Main) {
                                isRendering = false
                                isStemsRendering = false
                                renderProgress = 0f
                                when (result) {
                                    is RenderController.RenderResult.Success -> { trackerController.statusMessage = "STEMS EXPORTED!"; trackerController.statusSuccess = true }
                                    is RenderController.RenderResult.Error   -> { trackerController.statusMessage = "STEMS FAILED"; trackerController.statusSuccess = false }
                                }
                            }
                        }
                    }
                }
            }
            5 -> {
                val col = trackerController.projectCursorColumn
                if (col == 1 || col == 2) { cleanDialogTarget = if (col == 1) "SEQ" else "INST"; cleanDialogCursor = 0; showCleanDialog = true }
            }
            6 -> { previousScreen = trackerController.currentScreen; settingsReturnScreen = trackerController.currentScreen; trackerController.currentScreen = ScreenType.SETTINGS }
        }
    }

    private fun handleConfirmASettings() {
        // All value rows change via A+dpad (handled through getCursorContext/handleInput).
        // Single A is reserved for action rows only:
        when (trackerController.settingsCursorRow) {
            9 -> {
                themeEditorState = ThemeEditorState(isOpen = true)
            }
            10 -> {
                when (trackerController.settingsCursorColumn) {
                    1 -> {
                        val saved = fileController.saveTemplate(trackerController.project)
                        trackerController.statusMessage = if (saved) "TEMPLATE SAVED" else "SAVE FAILED"
                        trackerController.statusSuccess = saved
                    }
                    2 -> {
                        fileController.clearTemplate()
                        trackerController.statusMessage = "TEMPLATE CLEARED"
                        trackerController.statusSuccess = true
                    }
                }
            }
        }
    }

    private fun handleConfirmAInstrumentPool() {
        // A on the NAME column of an empty slot loads a source into it (M8: tap EDIT on an empty
        // slot). The selected pool row IS currentInstrument, so the existing LOAD_SOURCE flow loads
        // into the right slot, auto-names it, and returns here. The browser is filtered by the slot's
        // instrument type — SoundFont slots browse .sf2/.sf3, sampler slots browse .wav (matching the
        // INSTRUMENT screen's LOAD). Other columns edit via A+dpad.
        if (trackerController.poolCursorColumn != 0) return
        val inst = trackerController.project.instruments[trackerController.currentInstrument]
        val isSF = inst.instrumentType == InstrumentType.SOUNDFONT
        // "Empty" = no source loaded for the current type. Loaded slots are managed on the INSTRUMENT screen.
        if (isSF) { if (inst.soundfontPath != null) return } else { if (!inst.isFree()) return }
        instrumentController.currentInstrument = trackerController.currentInstrument
        instrumentFileBrowserAction = "LOAD_SOURCE"
        previousScreen = trackerController.currentScreen
        trackerController.currentScreen = ScreenType.FILE_BROWSER
        fileBrowserState = if (isSF) {
            fileBrowserModule.navigateToFolder(
                fileBrowserState.copy(fileExtensions = listOf("sf2", "sf3"),
                    mode = FileBrowserModule.BrowserMode.NORMAL, statusMessage = ""),
                File(fileController.getSoundfontsDirectory()))
        } else {
            fileBrowserModule.navigateToFolder(
                fileBrowserState.copy(fileExtensions = listOf("wav", "mp3") + FileBrowserModule.VIDEO_EXTENSIONS,
                    mode = FileBrowserModule.BrowserMode.NORMAL, statusMessage = ""),
                File(fileController.getSamplesDirectory()))
        }
    }

    private fun handleConfirmAInstrument() {
        instrumentController.currentInstrument = trackerController.currentInstrument
        val instrument = trackerController.project.instruments[trackerController.currentInstrument]
        when (trackerController.instrumentCursorRow) {
            0 -> when (trackerController.instrumentCursorColumn) {
                2 -> {
                    instrumentFileBrowserAction = "LOAD_PRESET"
                    previousScreen = trackerController.currentScreen
                    trackerController.currentScreen = ScreenType.FILE_BROWSER
                    val instrumentsDir = File(fileController.getInstrumentsDirectory())
                    fileBrowserState = fileBrowserModule.navigateToFolder(fileBrowserState.copy(fileExtensions = listOf("pti"), mode = FileBrowserModule.BrowserMode.NORMAL, statusMessage = ""), instrumentsDir)
                }
                3 -> {
                    val instrumentsDir = fileController.getInstrumentsDirectory()
                    File(instrumentsDir).mkdirs()
                    val defaultName = instrument.name.ifEmpty { "INST${instrument.id.toString(16).padStart(2,'0').uppercase()}" }
                    qwertyKeyboardState = QwertyKeyboardState(
                        isOpen = true,
                        text = defaultName,
                        maxLength = 20,
                        textCursor = defaultName.length,
                        fieldLabel = "SAVE PRESET:",
                        originalText = defaultName,
                        clearOnFirstB = true,
                        context = QwertyContext.INSTRUMENT_SAVE,
                        contextExtra = instrumentsDir,
                        insertBefore = insertBefore
                    )
                }
            }
            5 -> when (trackerController.instrumentCursorColumn) {
                2 -> {  // LOAD (col 2, left)
                    instrumentFileBrowserAction = "LOAD_SOURCE"
                    previousScreen = trackerController.currentScreen
                    trackerController.currentScreen = ScreenType.FILE_BROWSER
                    if (instrument.instrumentType == InstrumentType.SOUNDFONT) {
                        val soundfontsDir = File(fileController.getSoundfontsDirectory())
                        fileBrowserState = fileBrowserModule.navigateToFolder(fileBrowserState.copy(fileExtensions = listOf("sf2", "sf3"), mode = FileBrowserModule.BrowserMode.NORMAL, statusMessage = ""), soundfontsDir)
                    } else {
                        val samplesDir = File(fileController.getSamplesDirectory())
                        fileBrowserState = fileBrowserModule.navigateToFolder(fileBrowserState.copy(fileExtensions = listOf("wav", "mp3") + FileBrowserModule.VIDEO_EXTENSIONS, mode = FileBrowserModule.BrowserMode.NORMAL, statusMessage = ""), samplesDir)
                    }
                }
                3 -> {  // EDIT → sample editor (col 3, right; sampler only — SoundFonts have no editable waveform)
                    val inst = trackerController.project.instruments[trackerController.currentInstrument]
                    if (inst.instrumentType != InstrumentType.SOUNDFONT) {
                        val sampleId = trackerController.currentInstrument
                        sampleEditorState = SampleEditorState(
                            sampleId = sampleId,
                            instrumentId = trackerController.currentInstrument,
                            sampleName = inst.sampleFilePath?.substringAfterLast('/')
                                ?.substringBeforeLast('.') ?: "",
                            sampleFilePath = inst.sampleFilePath,
                            cursorRow = 1,
                            cursorCol = 0,
                            isModified = false
                        )
                        previousScreen = trackerController.currentScreen
                        trackerController.currentScreen = ScreenType.SAMPLE_EDITOR
                    }
                }
            }
        }
    }

    private fun handleConfirmASampleEditor() {
        val s = sampleEditorState
        val instId = s.instrumentId
        fun doDestructiveOp(op: () -> Unit) {
            audioEngine.restoreFxPreviewBackup()
            audioEngine.backupSample(instId)
            op()
            sampleEditorState = sampleEditorState.copy(totalFrames = audioEngine.getSampleLength(instId), waveformData = audioEngine.getSampleWaveform(instId, 620), isModified = true)
        }
        val startF = s.selectionStart.toInt()
        val endF   = s.selectionEnd.toInt()
        fun afterResize() {
            val newLen = audioEngine.getSampleLength(instId)
            sampleEditorState = sampleEditorState.copy(totalFrames = newLen, waveformData = audioEngine.getSampleWaveform(instId, 620), selectionStart = 0L, selectionEnd = newLen.toLong(), isModified = true)
        }
        when (s.cursorRow) {
            13 -> when (s.cursorCol) {
                0 -> { if (startF < endF) { audioEngine.restoreFxPreviewBackup(); audioEngine.backupSample(instId); audioEngine.cropSample(instId, startF, endF); afterResize() } }
                1 -> { audioEngine.copyRegion(instId, startF, endF) }
                2 -> { if (startF < endF) { audioEngine.restoreFxPreviewBackup(); audioEngine.backupSample(instId); audioEngine.copyRegion(instId, startF, endF); audioEngine.deleteSampleRegion(instId, startF, endF); afterResize() } }
                3 -> { if (startF < endF) { audioEngine.restoreFxPreviewBackup(); audioEngine.backupSample(instId); audioEngine.copyRegion(instId, startF, endF); audioEngine.pasteRegion(instId, sampleEditorState.totalFrames); afterResize() } }
                4 -> { if (audioEngine.getClipboardLength() > 0) { audioEngine.restoreFxPreviewBackup(); audioEngine.backupSample(instId); audioEngine.pasteRegion(instId, startF); afterResize() } }
                5 -> { if (startF < endF) { audioEngine.restoreFxPreviewBackup(); audioEngine.backupSample(instId); audioEngine.deleteSampleRegion(instId, startF, endF); afterResize() } }
            }
            14 -> when (s.cursorCol) {
                0 -> doDestructiveOp { audioEngine.normalizeSample(instId, startF, endF) }
                1 -> doDestructiveOp { audioEngine.fadeInSample(instId, startF, endF) }
                2 -> doDestructiveOp { audioEngine.fadeOutSample(instId, startF, endF) }
                3 -> doDestructiveOp { audioEngine.silenceRegion(instId, startF, endF) }
                4 -> doDestructiveOp { audioEngine.reverseSample(instId, startF, endF) }
                5 -> {
                    audioEngine.restoreFxPreviewBackup()
                    audioEngine.undoSample(instId)
                    // Undo restores a different sample length, so reset the selection to the full
                    // restored sample (0..newLen). Clamping instead would leave a partial selection
                    // after undoing a length-shrinking op (e.g. SYNC that shortened the sample).
                    // Slice marker is clamped, not reset, so it stays put if still in range.
                    val newLen = audioEngine.getSampleLength(instId)
                    sampleEditorState = sampleEditorState.copy(
                        totalFrames = newLen,
                        waveformData = audioEngine.getSampleWaveform(instId, 620),
                        selectionStart = 0L,
                        selectionEnd = newLen.toLong(),
                        slicePosition = sampleEditorState.slicePosition.coerceIn(0L, newLen.toLong())
                    )
                }
            }
            16 -> if (s.cursorCol == 2) {
                when {
                    s.fxType in 0..3 -> {
                        if (s.fxValue > 0 || s.fxType == 3) {
                            audioEngine.restoreFxPreviewBackup(); audioEngine.backupSample(instId)
                            audioEngine.applySampleFx(instId, s.fxType, s.fxValue, s.sampleRate.toFloat(), trackerController.project.limiterPreGain)
                            sampleEditorState = sampleEditorState.copy(waveformData = audioEngine.getSampleWaveform(instId, 620), isModified = true)
                        }
                    }
                    s.fxType == 4 -> when (s.syncType) {
                        0 -> {
                            val bpm = trackerController.project.tempo
                            val rawSecs = if (s.sampleRate > 0) s.totalFrames.toDouble() / s.sampleRate else 0.0
                            if (rawSecs > 0.0 && bpm > 0) {
                                val targetBeats = when (s.durationIndex) { 0->16.0;1->8.0;2->4.0;3->2.0;4->1.0;5->0.5;6->0.25;else->0.125 }
                                val targetSecs = targetBeats * 60.0 / bpm
                                val semitones = Math.round(12.0 * Math.log(rawSecs / targetSecs) / Math.log(2.0)).toInt().coerceIn(-24, 24)
                                if (semitones != 0) {
                                    audioEngine.restoreFxPreviewBackup(); audioEngine.backupSample(instId)
                                    val oldLen = s.totalFrames
                                    audioEngine.pitchShiftSample(instId, semitones)
                                    val newLen = audioEngine.getSampleLength(instId)
                                    fun scaleFrame(f: Long) = if (oldLen > 0) (f * newLen.toLong() / oldLen).coerceIn(0L, newLen.toLong()) else 0L
                                    sampleEditorState = sampleEditorState.copy(totalFrames = newLen, waveformData = audioEngine.getSampleWaveform(instId, 620), pitchSemitones = 0, selectionStart = 0L, selectionEnd = newLen.toLong(), slicePosition = scaleFrame(sampleEditorState.slicePosition), isModified = true)
                                }
                            }
                        }
                        1 -> {
                            val bpm = trackerController.project.tempo
                            val rawSecs = if (s.sampleRate > 0) s.totalFrames.toDouble() / s.sampleRate else 0.0
                            if (rawSecs > 0.0 && bpm > 0) {
                                val targetBeats = when (s.durationIndex) { 0->16.0;1->8.0;2->4.0;3->2.0;4->1.0;5->0.5;6->0.25;else->0.125 }
                                val targetSecs = targetBeats * 60.0 / bpm
                                val ratio = (targetSecs / rawSecs).toFloat()
                                if (ratio > 0.001f && (ratio < 0.999f || ratio > 1.001f)) {
                                    audioEngine.restoreFxPreviewBackup(); audioEngine.backupSample(instId)
                                    val oldLen = s.totalFrames
                                    audioEngine.timeStretchSample(instId, ratio)
                                    val newLen = audioEngine.getSampleLength(instId)
                                    fun scaleFrame(f: Long) = if (oldLen > 0) (f * newLen.toLong() / oldLen).coerceIn(0L, newLen.toLong()) else 0L
                                    sampleEditorState = sampleEditorState.copy(totalFrames = newLen, waveformData = audioEngine.getSampleWaveform(instId, 620), selectionStart = 0L, selectionEnd = newLen.toLong(), slicePosition = scaleFrame(sampleEditorState.slicePosition), isModified = true)
                                }
                            }
                        }
                    }
                }
            }
            18 -> {
                val currentName = s.sampleName
                qwertyKeyboardState = QwertyKeyboardState(
                    isOpen = true,
                    text = currentName,
                    maxLength = 20,
                    textCursor = currentName.length.coerceAtMost(20),
                    keyCursorRow = 0,
                    keyCursorCol = 0,
                    layout = 0,
                    fieldLabel = "SAMPLE NAME:",
                    originalText = currentName,
                    insertBefore = insertBefore,
                    context = QwertyContext.SAMPLE_NAME,
                    contextExtra = fileController.getSamplesDirectory()
                )
            }
            19 -> when (s.cursorCol) {
                0 -> {
                    instrumentFileBrowserAction = "LOAD_SAMPLE_EDITOR"
                    val samplesDir = File(fileController.getSamplesDirectory())
                    fileBrowserState = fileBrowserModule.navigateToFolder(fileBrowserState.copy(fileExtension = "wav", fileExtensions = listOf("wav"), mode = FileBrowserModule.BrowserMode.NORMAL, statusMessage = ""), samplesDir)
                    trackerController.currentScreen = ScreenType.FILE_BROWSER
                }
                1 -> {
                    val baseName = s.sampleName.ifEmpty { "SAMPLE" }
                    val samplesDir = fileController.getSamplesDirectory()
                    File(samplesDir).mkdirs()
                    val targetPath = "$samplesDir/$baseName.wav"
                    run {
                        val semitones = sampleEditorState.pitchSemitones
                        if (semitones != 0) {
                            val oldLen = sampleEditorState.totalFrames
                            audioEngine.pitchShiftSample(instId, semitones)
                            val newLen = audioEngine.getSampleLength(instId)
                            fun scaleFrame(f: Long) = if (oldLen > 0) (f * newLen.toLong() / oldLen).coerceIn(0L, newLen.toLong()) else 0L
                            audioEngine.updateInstrumentPlaybackParams(trackerController.project.instruments[instId])
                            trackerController.projectVersion++
                            sampleEditorState = sampleEditorState.copy(totalFrames = newLen, waveformData = audioEngine.getSampleWaveform(instId, 620), pitchSemitones = 0, rateMode = 0, selectionStart = scaleFrame(sampleEditorState.selectionStart), selectionEnd = scaleFrame(sampleEditorState.selectionEnd), slicePosition = scaleFrame(sampleEditorState.slicePosition))
                        }
                    }
                    if (!File(targetPath).exists()) {
                        val cuePoints = computeSliceCuePoints(sampleEditorState)
                        val srcMode   = sampleEditorState.sourceMode
                        val hasStereo = sampleEditorState.hasStereoData
                        coroutineScope.launch(Dispatchers.Default) {
                            val origRate = audioEngine.getOriginalSampleRate(instId)
                            val leftCh: FloatArray; val rightCh: FloatArray
                            if (!hasStereo) { val m = audioEngine.getSampleData(instId); leftCh = m; rightCh = m }
                            else when (srcMode) {
                                0 -> { val l = audioEngine.getSampleData(instId); leftCh = l; rightCh = l }
                                1 -> { val r = audioEngine.getSampleDataRight(instId); leftCh = r; rightCh = r }
                                2 -> { leftCh = audioEngine.getSampleData(instId); rightCh = audioEngine.getSampleDataRight(instId) }
                                else -> { val l = audioEngine.getSampleData(instId); val r = audioEngine.getSampleDataRight(instId); val m = FloatArray(l.size) { i -> (l[i] + r[i]) / 2f }; leftCh = m; rightCh = m }
                            }
                            val outputChannels = if (hasStereo && srcMode == 2) 2 else 1
                            val success = WavWriter.writeWav(fileSystem = fileSystem, path = targetPath, leftChannel = leftCh, rightChannel = rightCh, sampleRate = origRate, cuePoints = cuePoints, channels = outputChannels)
                            if (success && outputChannels == 1) audioEngine.loadSampleFromFile(instId, targetPath)
                            withContext(Dispatchers.Main) {
                                if (success) {
                                    trackerController.project.instruments[instId].sampleFilePath = targetPath
                                    trackerController.project.instruments[instId].sliceMarkers = cuePoints.map { it.toLong() }
                                    sampleEditorState = sampleEditorState.copy(sampleFilePath = targetPath, sampleName = baseName, isModified = false, hasStereoData = audioEngine.hasStereoData(instId))
                                    trackerController.currentScreen = previousScreen
                                }
                            }
                        }
                    } else {
                        var suggestedName = baseName; var n = 1
                        while (File("$samplesDir/$suggestedName.wav").exists()) { suggestedName = "${baseName}_${n.toString().padStart(4,'0')}"; n++ }
                        qwertyKeyboardState = QwertyKeyboardState(
                            isOpen = true,
                            text = suggestedName,
                            maxLength = 24,
                            textCursor = suggestedName.length.coerceAtMost(24),
                            keyCursorRow = 0,
                            keyCursorCol = 0,
                            layout = 0,
                            fieldLabel = "SAVE AS:",
                            originalText = suggestedName,
                            insertBefore = insertBefore,
                            clearOnFirstB = true,
                            context = QwertyContext.SAMPLE_SAVE,
                            contextExtra = samplesDir
                        )
                    }
                }
                2 -> {
                    run {
                        val semitones = sampleEditorState.pitchSemitones
                        if (semitones != 0) {
                            val oldLen = sampleEditorState.totalFrames
                            audioEngine.pitchShiftSample(instId, semitones)
                            val newLen = audioEngine.getSampleLength(instId)
                            fun scaleFrame(f: Long) = if (oldLen > 0) (f * newLen.toLong() / oldLen).coerceIn(0L, newLen.toLong()) else 0L
                            audioEngine.updateInstrumentPlaybackParams(trackerController.project.instruments[instId])
                            trackerController.projectVersion++
                            sampleEditorState = sampleEditorState.copy(totalFrames = newLen, waveformData = audioEngine.getSampleWaveform(instId, 620), pitchSemitones = 0, rateMode = 0, selectionStart = scaleFrame(sampleEditorState.selectionStart), selectionEnd = scaleFrame(sampleEditorState.selectionEnd), slicePosition = scaleFrame(sampleEditorState.slicePosition))
                        }
                    }
                    val filePath = trackerController.project.instruments[instId].sampleFilePath
                    if (filePath != null) {
                        val cuePoints = computeSliceCuePoints(sampleEditorState)
                        val srcMode   = sampleEditorState.sourceMode
                        val hasStereo = sampleEditorState.hasStereoData
                        coroutineScope.launch(Dispatchers.Default) {
                            val origRate = audioEngine.getOriginalSampleRate(instId)
                            val leftCh: FloatArray; val rightCh: FloatArray
                            if (!hasStereo) { val m = audioEngine.getSampleData(instId); leftCh = m; rightCh = m }
                            else when (srcMode) {
                                0 -> { val l = audioEngine.getSampleData(instId); leftCh = l; rightCh = l }
                                1 -> { val r = audioEngine.getSampleDataRight(instId); leftCh = r; rightCh = r }
                                2 -> { leftCh = audioEngine.getSampleData(instId); rightCh = audioEngine.getSampleDataRight(instId) }
                                else -> { val l = audioEngine.getSampleData(instId); val r = audioEngine.getSampleDataRight(instId); val m = FloatArray(l.size) { i -> (l[i] + r[i]) / 2f }; leftCh = m; rightCh = m }
                            }
                            val outputChannels = if (hasStereo && srcMode == 2) 2 else 1
                            val success = WavWriter.writeWav(fileSystem = fileSystem, path = filePath, leftChannel = leftCh, rightChannel = rightCh, sampleRate = origRate, cuePoints = cuePoints, channels = outputChannels)
                            if (success && outputChannels == 1) audioEngine.loadSampleFromFile(instId, filePath)
                            withContext(Dispatchers.Main) {
                                if (success) {
                                    trackerController.project.instruments[instId].sliceMarkers = cuePoints.map { it.toLong() }
                                    sampleEditorState = sampleEditorState.copy(isModified = false, hasStereoData = audioEngine.hasStereoData(instId))
                                    trackerController.currentScreen = previousScreen
                                }
                            }
                        }
                    }
                }
                3 -> {
                    if (s.sliceMethod == 2) return
                    val sliceCount = when (s.sliceMethod) { 0 -> s.transientMarkers.size + 1; 1 -> s.sliceDivisions.coerceAtLeast(1); else -> 0 }
                    if (sliceCount <= 0) return
                    val baseName = s.sampleName.ifEmpty { "SAMPLE" }.replace(Regex("[^A-Za-z0-9_-]"), "_")
                    val chopsDirPath = "${fileController.getSamplesDirectory()}/Chops/$baseName"
                    coroutineScope.launch(Dispatchers.Default) {
                        File(chopsDirPath).mkdirs()
                        val floats   = audioEngine.getSampleData(instId)
                        val origRate = audioEngine.getOriginalSampleRate(instId)
                        for (idx in 0 until sliceCount) {
                            val (startL, endL) = s.getSliceBounds(idx)
                            val start = startL.toInt().coerceIn(0, floats.size)
                            val end   = endL.toInt().coerceIn(start, floats.size)
                            if (end <= start) continue
                            val slice  = floats.copyOfRange(start, end)
                            val suffix = idx.toString().padStart(2, '0')
                            WavWriter.writeWavMono(fileSystem, "$chopsDirPath/${baseName}_$suffix.wav", slice, origRate)
                        }
                    }
                }
            }
        }
    }

    private fun handleConfirmAPhrase() {
        if (trackerController.cursorColumn == 1 && !trackerController.inputController.isSelectionModeActive()) {
            val phraseState = PhraseEditorState(
                trackerController.project.phrases[trackerController.currentPhrase],
                trackerController.cursorRow,
                trackerController.cursorColumn,
                playbackRow = 0,
                isPlaying = trackerController.isPlaying()
            )
            val context = phraseEditorModule.getCursorContext(phraseState)
            if (context.capabilities.isEmpty) {
                val step = trackerController.project.phrases[trackerController.currentPhrase].steps[trackerController.cursorRow]
                step.note = trackerController.lastEditedNote
                step.instrument = trackerController.lastEditedInstrument
                step.volume = trackerController.lastEditedVolume
                trackerController.projectVersion++
                if (notePreviewEnabled && step.note != Note.EMPTY) {
                    val instrument = trackerController.project.instruments[step.instrument.coerceIn(0, 127)]
                    val sr = audioEngine.getDeviceSampleRate().toLong().coerceAtLeast(44100L)
                    val msPerStep = 60000.0 / trackerController.project.tempo / 4.0
                    val phraseDurationFrames = (msPerStep * sr / 1000.0 * 16).toLong()
                    audioEngine.previewNoteWithTimeout(instrument, step.note, trackerController.project, phraseDurationFrames)
                }
            }
        }
    }

    private fun handleConfirmAChain() {
        val chain = trackerController.project.chains[trackerController.currentChain]
        if (chain.isEmpty(trackerController.cursorRow)) {
            chain.phraseRefs[trackerController.cursorRow] = trackerController.lastEditedPhrase
            chain.transposeValues[trackerController.cursorRow] = trackerController.lastEditedTranspose
            trackerController.projectVersion++
            lastAInsertPosition = InsertPosition(ScreenType.CHAIN, trackerController.cursorRow, trackerController.cursorColumn)
        } else {
            lastAInsertPosition = null
        }
    }

    private fun handleConfirmASong() {
        if (!trackerController.inputController.isSelectionModeActive()) {
            val track = trackerController.project.tracks[trackerController.cursorColumn - 1]
            while (track.chainRefs.size <= trackerController.cursorRow) track.chainRefs.add(-1)
            if (track.chainRefs[trackerController.cursorRow] == -1) {
                track.chainRefs[trackerController.cursorRow] = trackerController.lastEditedChain
                trackerController.projectVersion++
                lastAInsertPosition = InsertPosition(ScreenType.SONG, trackerController.cursorRow, trackerController.cursorColumn)
            } else {
                lastAInsertPosition = null
            }
        }
    }
    fun handleButtonB() {
        if (qwertyKeyboardState.isOpen) { qwertyKeyboardState = qwertyKeyboardState.deleteChar(); return }
        // Item 3: B closes the EQ editor (deferred to release by the InputMapper so B+DPAD preset
        // cycling inside it still works). SELECT remains an alias (handleSelect).
        if (eqEditorState.isOpen) { eqEditorState = EqEditorState(); return }
        if (showCleanDialog) { showCleanDialog = false; return }
        if (showNewProjectDialog) { showNewProjectDialog = false; return }
        if (showRecoveryDialog) { showRecoveryDialog = false; fileController.clearAutosave(); return }  // B = discard recovery
        if (showInstrTypeDialog) { showInstrTypeDialog = false; return }
        if (themeEditorState.isOpen) { themeEditorState = ThemeEditorState(); return }
        if (trackerController.currentScreen == ScreenType.SETTINGS) {
            trackerController.inputController.exitSelectionMode()
            trackerController.currentScreen = settingsReturnScreen
            return
        }
        if (trackerController.currentScreen == ScreenType.FILE_BROWSER && fileBrowserState.selectionMode) {
            val files = getFileBrowserSelectedFiles()
            if (files.isNotEmpty()) {
                val n = files.size
                fileBrowserState = fileBrowserState.copy(
                    selectionMode = false, selectionAnchor = -1,
                    fileClipboard = files, fileClipboardIsCut = false,
                    statusMessage = "CPY $n ${if (n == 1) "FILE" else "FILES"}", statusSuccess = true
                )
            }
            return
        }
        if (trackerController.inputController.isSelectionModeActive()) {
            val bounds = trackerController.inputController.getSelectionBounds()
            if (bounds != null) {
                when (trackerController.currentScreen) {
                    ScreenType.PHRASE -> clipboardManager.copyPhraseSteps(trackerController.project, trackerController.currentPhrase, bounds.topLeftRow, bounds.topLeftColumn, bounds.bottomRightRow, bounds.bottomRightColumn)
                    ScreenType.CHAIN  -> clipboardManager.copyChainRows(trackerController.project, trackerController.currentChain, bounds.topLeftRow, bounds.topLeftColumn, bounds.bottomRightRow, bounds.bottomRightColumn)
                    ScreenType.SONG   -> clipboardManager.copySongCells(trackerController.project, bounds.topLeftRow, bounds.topLeftColumn, bounds.bottomRightRow, bounds.bottomRightColumn)
                    ScreenType.TABLE  -> clipboardManager.copyTableRows(trackerController.project, trackerController.currentTable, bounds.topLeftRow, bounds.topLeftColumn, bounds.bottomRightRow, bounds.bottomRightColumn)
                    else -> { }
                }
                trackerController.inputController.exitSelectionMode()
            }
            return
        }
        when (trackerController.currentScreen) {
            ScreenType.SETTINGS -> trackerController.currentScreen = previousScreen
            ScreenType.SAMPLE_EDITOR -> {
                if (eqEditorState.isOpen) return
                if (sampleEditorState.showConfirmClose) {
                    sampleEditorState = sampleEditorState.copy(showConfirmClose = false)
                } else if (sampleEditorState.isModified) {
                    sampleEditorState = sampleEditorState.copy(showConfirmClose = true)
                } else {
                    audioEngine.restoreFxPreviewBackup()
                    audioEngine.freeSampleUndo(sampleEditorState.instrumentId)  // editor closing — undo unreachable (REVIEW-3 1.1)
                    trackerController.currentScreen = previousScreen
                }
                return
            }
            ScreenType.FILE_BROWSER -> {
                when (fileBrowserState.mode) {
                    FileBrowserModule.BrowserMode.NORMAL -> trackerController.currentScreen = previousScreen
                    else -> fileBrowserState = fileBrowserState.copy(mode = FileBrowserModule.BrowserMode.NORMAL, renameBuffer = "", renameCursor = 0)
                }
            }
            ScreenType.SONG, ScreenType.CHAIN, ScreenType.PHRASE -> { }
            ScreenType.PROJECT -> { }
            else -> { }
        }
    }
    // True while a simple confirm dialog (CLEAN / NEW PROJECT / INSTR TYPE / sample-editor
    // discard-changes) is open. These are modal yes/no prompts handled by A (confirm) and B (cancel)
    // in handleButtonA/B. Every OTHER input must be swallowed so it can't act on the screen behind
    // the dialog (e.g. SELECT clearing a chain ref or opening the EQ editor, START toggling
    // playback/preview). A/B naturally close the dialog before any combo can form, so guarding the
    // non-A/B entry points (SELECT, START, R+DPAD) is sufficient.
    // RULE: every new show*Dialog-style modal state MUST be added to this predicate.
    private fun confirmDialogOpen(): Boolean =
        showCleanDialog || showNewProjectDialog || showInstrTypeDialog || showRecoveryDialog ||
            sampleEditorState.showConfirmClose

    fun handleSelect() {
        if (confirmDialogOpen()) return
        if (qwertyKeyboardState.isOpen) { qwertyKeyboardState = QwertyKeyboardState(); return }
        if (themeEditorState.isOpen) { themeEditorState = ThemeEditorState(); return }
        if (eqEditorState.isOpen) { eqEditorState = EqEditorState(); return }
        when (trackerController.currentScreen) {
            // SELECT no longer clears the value under the cursor on the editor screens — deleting a value
            // is A+B (and selection delete). SELECT is left free here for context actions / future use.
            ScreenType.SONG -> { }
            ScreenType.CHAIN -> { }
            ScreenType.PHRASE -> { }
            ScreenType.PROJECT -> {
                // SELECT alias for the item-3 deferred-A open (shared helper).
                if (trackerController.projectCursorRow == 2 && trackerController.projectCursorColumn >= 1) openProjectNameEditor()
            }
            ScreenType.SAMPLE_EDITOR -> {
                if (sampleEditorState.cursorRow == 16 && sampleEditorState.fxType == 3) {
                    openEqEditor(sampleEditorState.fxValue.coerceIn(0, 127), EqCallerContext.SampleEditorFx)
                    return
                }
                if (sampleEditorState.cursorRow == 18) {
                    val currentName = sampleEditorState.sampleName
                    qwertyKeyboardState = QwertyKeyboardState(
                        isOpen = true,
                        text = currentName,
                        maxLength = 20,
                        textCursor = currentName.length.coerceAtMost(20),
                        keyCursorRow = 0,
                        keyCursorCol = 0,
                        layout = 0,
                        fieldLabel = "SAMPLE NAME:",
                        originalText = currentName,
                        insertBefore = insertBefore,
                        context = QwertyContext.SAMPLE_NAME,
                        contextExtra = fileController.getSamplesDirectory()
                    )
                }
                return
            }
            ScreenType.FILE_BROWSER -> { }
            ScreenType.EFFECTS -> {
                when (trackerController.effectsCursorRow) {
                    EffectModule.ROW_DLY_TIME -> {
                        val proj = trackerController.project
                        proj.delaySync = !proj.delaySync
                        if (proj.delaySync) proj.delayTime = proj.delayTime.coerceIn(0, 11)
                        audioBackend.setDelayParams(proj.delayTime, proj.delayFeedback, proj.delaySync, proj.tempo.toFloat(), proj.delayWet)
                        trackerController.projectVersion++
                    }
                    EffectModule.ROW_REV_EQ -> { val slot = trackerController.project.reverbInputEq; openEqEditor(if (slot < 0) 0 else slot, EqCallerContext.ReverbInputEq) }
                    EffectModule.ROW_DLY_EQ -> { val slot = trackerController.project.delayInputEq;  openEqEditor(if (slot < 0) 0 else slot, EqCallerContext.DelayInputEq) }
                }
            }
            ScreenType.MIXER -> {
                if (trackerController.mixerMasterRow == 1 && trackerController.mixerCursorColumn == 8) {
                    val slot = trackerController.project.masterEqSlot
                    openEqEditor(if (slot < 0) 0 else slot, EqCallerContext.MasterEq)
                }
            }
            ScreenType.INSTRUMENT -> {
                val instr = trackerController.project.instruments[trackerController.currentInstrument]
                val isSF  = instr.instrumentType == InstrumentType.SOUNDFONT
                val row   = trackerController.instrumentCursorRow
                val col   = trackerController.instrumentCursorColumn
                if (row == 1) { openInstrumentNameEditor(); return }  // NAME row → qwerty (item-3 shared helper)
                val onEq  = (!isSF && row == 12 && col == 1) || (isSF && row == 14 && col == 1)
                if (onEq) {
                    val slot = instr.eqSlot
                    openEqEditor(if (slot < 0) 0 else slot, EqCallerContext.InstrumentEq(trackerController.currentInstrument))
                }
            }
            ScreenType.INST_POOL -> {
                // SELECT on the EQ column opens the per-instrument EQ editor; other columns: no-op
                // (intentionally NOT the default "jump to main screen" behaviour).
                if (trackerController.poolCursorColumn == 4) {
                    val slot = trackerController.project.instruments[trackerController.currentInstrument].eqSlot
                    openEqEditor(if (slot < 0) 0 else slot, EqCallerContext.InstrumentEq(trackerController.currentInstrument))
                }
            }
            else -> {
                if (trackerController.currentScreen !in MAIN_ROW_SCREENS) {
                    trackerController.currentScreen = when (trackerController.previousColumn) {
                        0 -> ScreenType.SONG; 1 -> ScreenType.CHAIN; 2 -> ScreenType.PHRASE; 3 -> ScreenType.INSTRUMENT; 4 -> ScreenType.TABLE; else -> ScreenType.PHRASE
                    }
                }
            }
        }
    }
    fun handleStart() {
        if (confirmDialogOpen()) return
        if (qwertyKeyboardState.isOpen) {
            val typedText = qwertyKeyboardState.text.trimEnd()
            when (qwertyKeyboardState.context) {
                QwertyContext.PROJECT_NAME -> { trackerController.project.name = typedText; trackerController.projectVersion++ }
                QwertyContext.INSTRUMENT_NAME -> {
                    val inst = trackerController.project.instruments[trackerController.currentInstrument]
                    inst.name = typedText.ifBlank { inst.defaultName() }  // cleared name reverts to "INSTxx"
                    trackerController.projectVersion++
                }
                QwertyContext.FILE_RENAME -> {
                    val oldPath = qwertyKeyboardState.contextExtra
                    val newBaseName = typedText.ifEmpty { File(oldPath).nameWithoutExtension }
                    if (fileSystem.renameFile(oldPath, newBaseName))
                        fileBrowserState = fileBrowserModule.navigateToFolder(fileBrowserState, fileBrowserState.currentDirectory)
                }
                QwertyContext.FOLDER_CREATE -> {
                    val parentPath = qwertyKeyboardState.contextExtra
                    val folderName = typedText.ifEmpty { "NewFolder" }
                    if (fileSystem.createFolder(parentPath, folderName) != null)
                        fileBrowserState = fileBrowserModule.navigateToFolder(fileBrowserState, fileBrowserState.currentDirectory)
                }
                QwertyContext.INSTRUMENT_SAVE -> {
                    val name = typedText.ifEmpty { "PRESET" }
                    val filePath = "${qwertyKeyboardState.contextExtra}/$name.pti"
                    instrumentController.currentInstrument = trackerController.currentInstrument
                    instrumentController.savePreset(trackerController.project, filePath)
                    trackerController.projectVersion++
                }
                QwertyContext.SAMPLE_NAME -> {
                    val newName = typedText.ifEmpty { sampleEditorState.sampleName }
                    val instId = sampleEditorState.instrumentId
                    sampleEditorState = sampleEditorState.copy(sampleName = newName)
                    trackerController.project.instruments[instId].name = newName
                    trackerController.projectVersion++
                }
                QwertyContext.SAMPLE_SAVE -> {
                    val name = typedText.ifEmpty { "SAMPLE" }
                    val instId = sampleEditorState.instrumentId
                    val samplesDir = qwertyKeyboardState.contextExtra
                    run {
                        val semitones = sampleEditorState.pitchSemitones
                        if (semitones != 0) {
                            val oldLen = sampleEditorState.totalFrames
                            audioEngine.pitchShiftSample(instId, semitones)
                            val newLen = audioEngine.getSampleLength(instId)
                            fun scaleFrame(f: Long) = if (oldLen > 0) (f * newLen.toLong() / oldLen).coerceIn(0L, newLen.toLong()) else 0L
                            audioEngine.updateInstrumentPlaybackParams(trackerController.project.instruments[instId])
                            trackerController.projectVersion++
                            sampleEditorState = sampleEditorState.copy(totalFrames = newLen, waveformData = audioEngine.getSampleWaveform(instId, 620), pitchSemitones = 0, rateMode = 0, selectionStart = scaleFrame(sampleEditorState.selectionStart), selectionEnd = scaleFrame(sampleEditorState.selectionEnd), slicePosition = scaleFrame(sampleEditorState.slicePosition))
                        }
                    }
                    val cuePoints = computeSliceCuePoints(sampleEditorState)
                    val srcMode   = sampleEditorState.sourceMode
                    val hasStereo = sampleEditorState.hasStereoData
                    coroutineScope.launch(Dispatchers.Default) {
                        File(samplesDir).mkdirs()
                        var path = "$samplesDir/$name.wav"; var counter = 1
                        while (File(path).exists()) { path = "$samplesDir/${name}_${counter.toString().padStart(4,'0')}.wav"; counter++ }
                        val origRate = audioEngine.getOriginalSampleRate(instId)
                        val leftCh: FloatArray; val rightCh: FloatArray
                        if (!hasStereo) { val m = audioEngine.getSampleData(instId); leftCh = m; rightCh = m }
                        else when (srcMode) {
                            0 -> { val l = audioEngine.getSampleData(instId); leftCh = l; rightCh = l }
                            1 -> { val r = audioEngine.getSampleDataRight(instId); leftCh = r; rightCh = r }
                            2 -> { leftCh = audioEngine.getSampleData(instId); rightCh = audioEngine.getSampleDataRight(instId) }
                            else -> { val l = audioEngine.getSampleData(instId); val r = audioEngine.getSampleDataRight(instId); val m = FloatArray(l.size) { i -> (l[i] + r[i]) / 2f }; leftCh = m; rightCh = m }
                        }
                        val outputChannels = if (hasStereo && srcMode == 2) 2 else 1
                        val success = WavWriter.writeWav(fileSystem = fileSystem, path = path, leftChannel = leftCh, rightChannel = rightCh, sampleRate = origRate, cuePoints = cuePoints, channels = outputChannels)
                        if (success && outputChannels == 1) audioEngine.loadSampleFromFile(instId, path)
                        withContext(Dispatchers.Main) {
                            if (success) {
                                val savedName = File(path).nameWithoutExtension
                                trackerController.project.instruments[instId].sampleFilePath = path
                                sampleEditorState = sampleEditorState.copy(sampleFilePath = path, sampleName = savedName, isModified = false, hasStereoData = audioEngine.hasStereoData(instId))
                                trackerController.currentScreen = previousScreen
                            }
                        }
                    }
                }
                QwertyContext.THEME_SAVE -> {
                    val safeName = typedText.replace(Regex("[^a-zA-Z0-9_]"), "_").ifEmpty { "THEME" }
                    val filePath = "${qwertyKeyboardState.contextExtra}/$safeName.ptt"
                    val themeToSave = appTheme.copy(name = typedText.ifEmpty { appTheme.name })
                    fileSystem.writeFile(filePath, Json { prettyPrint = true }.encodeToString(themeToSave))
                    themeEditorState = ThemeEditorState(isOpen = true)
                }
                QwertyContext.VIDEO_EXTRACT -> {
                    val videoPath = qwertyKeyboardState.contextExtra ?: ""
                    val outputName = typedText.ifEmpty { File(videoPath).nameWithoutExtension + "_audio" }
                    coroutineScope.launch(Dispatchers.Default) {
                        val result = instrumentController.loadSampleFromVideo(trackerController.project, videoPath, videoExtractor, fileSystem, outputName)
                        withContext(Dispatchers.Main) {
                            if (result is LoadResult.Success) {
                                trackerController.projectVersion++
                                fileBrowserState = fileBrowserState.copy(statusMessage = "CONVERTED: $outputName.WAV", statusSuccess = true)
                                trackerController.currentScreen = previousScreen
                            } else {
                                fileBrowserState = fileBrowserState.copy(statusMessage = "CONVERT FAILED", statusSuccess = false)
                            }
                        }
                    }
                }
                QwertyContext.RESAMPLE -> {
                    val customName = typedText.ifEmpty { null }
                    val bounds = trackerController.inputController.getSelectionBounds()
                    if (bounds != null && !isRendering) {
                        val selectedTracks = (bounds.topLeftColumn - 1..bounds.bottomRightColumn - 1).filter { it in 0..7 }.toSet()
                        isRendering = true; renderProgress = 0f
                        trackerController.stopPlayback()
                        coroutineScope.launch(Dispatchers.Default) {
                            val result = renderController.renderSelectionToWav(
                                project = trackerController.project,
                                startRow = bounds.topLeftRow, endRow = bounds.bottomRightRow,
                                selectedTrackIds = selectedTracks,
                                progressCallback = object : RenderController.ProgressCallback {
                                    override fun onProgress(progress: Float, message: String) { renderProgress = progress }
                                },
                                customBaseName = customName
                            )
                            withContext(Dispatchers.Main) {
                                isRendering = false; renderProgress = 0f
                                when (result) {
                                    is RenderController.RenderResult.Success -> {
                                        val instId = instrumentController.createResampledInstrument(trackerController.project, result.filename)
                                        if (instId >= 0) {
                                            trackerController.statusMessage = "RESAMPLED → INST ${instId.toString(16).padStart(2,'0').uppercase()}"
                                            trackerController.statusSuccess = true; trackerController.projectVersion++
                                        }
                                    }
                                    is RenderController.RenderResult.Error -> { trackerController.statusMessage = "RESAMPLE FAILED"; trackerController.statusSuccess = false }
                                }
                            }
                        }
                    }
                }
            }
            qwertyKeyboardState = QwertyKeyboardState()
            return
        }

        // Everything below (playback + previews) needs the audio engine. Ignore START until the stream
        // is open — the UI now shows during the (sometimes slow) first stream-open instead of a loading
        // screen, so START can be pressed before the engine exists. Text/overlay START handled above.
        if (!audioEngine.isReady) return

        when (trackerController.currentScreen) {
            ScreenType.FILE_BROWSER -> {
                if (fileBrowserState.items.isNotEmpty()) {
                    val selectedFile = fileBrowserState.items[fileBrowserState.cursor].file
                    if (selectedFile.isFile) {
                        val ext = selectedFile.extension.lowercase()
                        if (ext == "wav" && (previousScreen == ScreenType.INSTRUMENT || previousScreen == ScreenType.INST_POOL)) {
                            trackerController.previewSampleFile(selectedFile.absolutePath)
                        } else if (ext == "mp3" || videoExtractor.isSupportedVideo(selectedFile.absolutePath)) {
                            fileBrowserState = fileBrowserState.copy(statusMessage = "EXTRACTING PREVIEW...", statusSuccess = true)
                            coroutineScope.launch(Dispatchers.Default) {
                                // MP3 preview: no cap (testing stage). Video containers keep the 30 s preview cap.
                                val previewCap = if (ext == "mp3") 0 else 30
                                val result = videoExtractor.extractAudio(selectedFile.absolutePath, maxDurationSec = previewCap)
                                withContext(Dispatchers.Main) {
                                    if (result.isSuccess) {
                                        val audio = result.getOrThrow()
                                        audioEngine.previewSampleData(audio.samples, audio.sampleRate, audio.samplesRight)
                                        fileBrowserState = fileBrowserState.copy(statusMessage = "", statusSuccess = true)
                                    } else {
                                        val why = result.exceptionOrNull()?.message ?: "PREVIEW FAILED"
                                        fileBrowserState = fileBrowserState.copy(statusMessage = why.uppercase().take(40), statusSuccess = false)
                                    }
                                }
                            }
                        }
                    }
                }
            }
            ScreenType.SAMPLE_EDITOR -> {
                val instId = sampleEditorState.instrumentId
                val total  = sampleEditorState.totalFrames
                val inst   = trackerController.project.instruments[instId]
                val savedStart = inst.sampleStart; val savedEnd = inst.sampleEnd; val savedRoot = inst.root
                val hasFxPreview = when (sampleEditorState.fxType) { 0,1,2 -> sampleEditorState.fxValue > 0; 3 -> true; else -> false }
                audioEngine.restoreFxPreviewBackup()
                if (hasFxPreview) {
                    audioEngine.saveFxPreviewBackup(instId)
                    audioEngine.applySampleFx(instId, sampleEditorState.fxType, sampleEditorState.fxValue, sampleEditorState.sampleRate.toFloat(), trackerController.project.limiterPreGain)
                }
                if (total > 0 && sampleEditorState.selectionEnd > sampleEditorState.selectionStart) {
                    inst.sampleStart = ((sampleEditorState.selectionStart * 255L) / total).toInt().coerceIn(0, 255)
                    inst.sampleEnd   = ((sampleEditorState.selectionEnd   * 255L) / total).toInt().coerceIn(0, 255)
                }
                val pitchSemitones = sampleEditorState.pitchSemitones
                if (pitchSemitones != 0) {
                    val shiftedMidi = (inst.root.toMidi() + pitchSemitones).coerceIn(0, 127)
                    inst.root = Note.fromMidi(shiftedMidi)
                }
                val previewSlot = audioEngine.prepareSampleEditorSourcePreview(instId, sampleEditorState.sourceMode)
                val savedSampleId = inst.sampleId
                if (previewSlot != instId) inst.sampleId = previewSlot
                // Push START/END to the slot that actually plays. For stereo samples in LEFT/RIGHT/MONO
                // source mode the preview routes through scratch slot 254 — pushing params before the
                // swap left slot 254 at default 0/255, so it played the whole sample and ignored the
                // selection markers.
                audioEngine.updateInstrumentPlaybackParams(inst)
                audioEngine.previewInstrumentDry(inst)
                if (previewSlot != instId) inst.sampleId = savedSampleId
                inst.root = savedRoot
                coroutineScope.launch {
                    delay(100)
                    inst.sampleStart = savedStart; inst.sampleEnd = savedEnd
                    audioEngine.updateInstrumentPlaybackParams(inst)
                    // Restore effects bypassed for dry preview
                    audioEngine.pushInstrumentEqAndSends(inst, trackerController.project)
                    audioEngine.pushInstrumentModulation(inst, trackerController.project.tempo)
                }
            }
            ScreenType.INSTRUMENT -> trackerController.previewInstrument()
            ScreenType.INST_POOL -> trackerController.previewInstrument()
            ScreenType.TABLE -> trackerController.previewInstrumentWithTable(trackerController.currentTable, trackerController.currentTable)
            ScreenType.MODS  -> trackerController.previewInstrument()
            else -> {
                if (isRendering) return
                if (trackerController.isPlaying()) {
                    trackerController.stopPlayback()
                } else {
                    when (trackerController.currentScreen) {
                        ScreenType.PHRASE  -> trackerController.playPhrase(trackerController.currentPhrase)
                        ScreenType.CHAIN   -> trackerController.playChain(trackerController.currentChain)
                        // SONG: start from the highlighted row (manual behaviour). Other screens
                        // have no song cursor, so they start from the top.
                        ScreenType.SONG    -> trackerController.playSong(startRow = trackerController.cursorRow)
                        ScreenType.MIXER, ScreenType.EFFECTS, ScreenType.PROJECT, ScreenType.SETTINGS -> trackerController.playSong()
                        else -> trackerController.togglePlayback()
                    }
                }
            }
        }
    }
    fun handleL()   { }
    fun handleR()   { }
    fun handleAUp() {
        if (themeEditorState.isOpen) {
            if (themeEditorState.cursorRow == 0) cyclePrevBuiltinTheme()
            else adjustThemeColor(themeEditorState.cursorChannel, +0x01)
            return
        }
        if (eqEditorState.isOpen) {
            handleGenericInput { ctx -> trackerController.inputController.handleAButton(ctx) }
        } else if (fxHelperState.isOpen) {
            fxHelperState = fxHelperState.fxMoveCursorUp()
        } else if (trackerController.currentScreen == ScreenType.SAMPLE_EDITOR && sampleEditorState.cursorRow in 3..8) {
            val step = maxOf(1L, sampleEditorState.totalFrames.toLong() / (256L shl sampleEditorState.zoomLevel))
            val maxFrame = sampleEditorState.totalFrames.toLong()
            fun snapFrame(f: Long) = if (sampleEditorState.snapEnabled) audioEngine.findZeroCrossing(sampleEditorState.instrumentId, f.toInt(), dir = 1).toLong() else f
            sampleEditorState = if (sampleEditorState.cursorCol == 0) {
                val raw = (sampleEditorState.selectionStart + step).coerceAtMost(sampleEditorState.selectionEnd - 1L)
                sampleEditorState.copy(selectionStart = snapFrame(raw).coerceAtMost(sampleEditorState.selectionEnd - 1L))
            } else {
                val raw = (sampleEditorState.selectionEnd + step).coerceAtMost(maxFrame)
                sampleEditorState.copy(selectionEnd = snapFrame(raw).coerceAtLeast(sampleEditorState.selectionStart + 1L).coerceAtMost(maxFrame))
            }
        } else if (isOnFxTypeColumn()) {
            val idx = getCurrentFxTypeIndex()
            fxHelperState = FxHelperState(isOpen = true, cursorRow = idx / 5, cursorCol = idx % 5)
        } else if (trackerController.currentScreen == ScreenType.INSTRUMENT &&
                   trackerController.instrumentCursorRow == 0 &&
                   trackerController.instrumentCursorColumn == 1) {
            val inst = trackerController.project.instruments[trackerController.currentInstrument]
            val hasSource = inst.sampleFilePath != null || inst.soundfontPath != null
            if (hasSource) {
                showInstrTypeDialog = true
            } else {
                instrumentController.currentInstrument = trackerController.currentInstrument
                val newType = if (inst.instrumentType == InstrumentType.SOUNDFONT) InstrumentType.SAMPLER else InstrumentType.SOUNDFONT
                instrumentController.setInstrumentType(trackerController.project, newType)
                trackerController.projectVersion++
            }
        } else {
            handleSelectionOrSingleIncrement { ctx -> trackerController.inputController.handleAButton(ctx) }
        }
    }

    fun handleADown() {
        if (themeEditorState.isOpen) {
            if (themeEditorState.cursorRow == 0) cycleNextBuiltinTheme()
            else adjustThemeColor(themeEditorState.cursorChannel, -0x01)
            return
        }
        if (eqEditorState.isOpen) {
            handleGenericInput { ctx -> trackerController.inputController.handleBButton(ctx) }
        } else if (fxHelperState.isOpen) {
            fxHelperState = fxHelperState.fxMoveCursorDown()
        } else if (trackerController.currentScreen == ScreenType.SAMPLE_EDITOR && sampleEditorState.cursorRow in 3..8) {
            val step = maxOf(1L, sampleEditorState.totalFrames.toLong() / (256L shl sampleEditorState.zoomLevel))
            fun snapFrame(f: Long) = if (sampleEditorState.snapEnabled) audioEngine.findZeroCrossing(sampleEditorState.instrumentId, f.toInt(), dir = -1).toLong() else f
            sampleEditorState = if (sampleEditorState.cursorCol == 0) {
                val raw = (sampleEditorState.selectionStart - step).coerceAtLeast(0L)
                sampleEditorState.copy(selectionStart = snapFrame(raw).coerceAtMost(sampleEditorState.selectionEnd - 1L))
            } else {
                val raw = (sampleEditorState.selectionEnd - step).coerceAtLeast(sampleEditorState.selectionStart + 1L)
                sampleEditorState.copy(selectionEnd = snapFrame(raw).coerceAtLeast(sampleEditorState.selectionStart + 1L))
            }
        } else if (isOnFxTypeColumn()) {
            val idx = getCurrentFxTypeIndex()
            fxHelperState = FxHelperState(isOpen = true, cursorRow = idx / 5, cursorCol = idx % 5)
        } else if (trackerController.currentScreen == ScreenType.INSTRUMENT &&
                   trackerController.instrumentCursorRow == 0 &&
                   trackerController.instrumentCursorColumn == 1) {
            val inst = trackerController.project.instruments[trackerController.currentInstrument]
            val hasSource = inst.sampleFilePath != null || inst.soundfontPath != null
            if (hasSource) {
                showInstrTypeDialog = true
            } else {
                instrumentController.currentInstrument = trackerController.currentInstrument
                val newType = if (inst.instrumentType == InstrumentType.SOUNDFONT) InstrumentType.SAMPLER else InstrumentType.SOUNDFONT
                instrumentController.setInstrumentType(trackerController.project, newType)
                trackerController.projectVersion++
            }
        } else {
            handleSelectionOrSingleIncrement { ctx -> trackerController.inputController.handleBButton(ctx) }
        }
    }

    fun handleALeft() {
        if (themeEditorState.isOpen) {
            if (themeEditorState.cursorRow >= 1) adjustThemeColor(themeEditorState.cursorChannel, -0x10)
            return
        }
        if (eqEditorState.isOpen) {
            handleGenericInput { ctx -> trackerController.inputController.handleALeft(ctx) }
        } else if (fxHelperState.isOpen) {
            fxHelperState = fxHelperState.fxMoveCursorLeft()
        } else if (trackerController.currentScreen == ScreenType.SAMPLE_EDITOR && sampleEditorState.cursorRow in 3..8) {
            val step = maxOf(1L, sampleEditorState.totalFrames.toLong() / (16L shl sampleEditorState.zoomLevel))
            fun snapFrame(f: Long) = if (sampleEditorState.snapEnabled) audioEngine.findZeroCrossing(sampleEditorState.instrumentId, f.toInt(), dir = -1).toLong() else f
            sampleEditorState = if (sampleEditorState.cursorCol == 0) {
                val raw = (sampleEditorState.selectionStart - step).coerceAtLeast(0L)
                sampleEditorState.copy(selectionStart = snapFrame(raw).coerceAtMost(sampleEditorState.selectionEnd - 1L))
            } else {
                val raw = (sampleEditorState.selectionEnd - step).coerceAtLeast(sampleEditorState.selectionStart + 1L)
                sampleEditorState.copy(selectionEnd = snapFrame(raw).coerceAtLeast(sampleEditorState.selectionStart + 1L))
            }
        } else {
            handleSelectionOrSingleIncrement { ctx -> trackerController.inputController.handleALeft(ctx) }
        }
    }

    fun handleARight() {
        if (themeEditorState.isOpen) {
            if (themeEditorState.cursorRow >= 1) adjustThemeColor(themeEditorState.cursorChannel, +0x10)
            return
        }
        if (eqEditorState.isOpen) {
            handleGenericInput { ctx -> trackerController.inputController.handleARight(ctx) }
        } else if (fxHelperState.isOpen) {
            fxHelperState = fxHelperState.fxMoveCursorRight()
        } else if (trackerController.currentScreen == ScreenType.SAMPLE_EDITOR && sampleEditorState.cursorRow in 3..8) {
            val step = maxOf(1L, sampleEditorState.totalFrames.toLong() / (16L shl sampleEditorState.zoomLevel))
            val maxFrame = sampleEditorState.totalFrames.toLong()
            fun snapFrame(f: Long) = if (sampleEditorState.snapEnabled) audioEngine.findZeroCrossing(sampleEditorState.instrumentId, f.toInt(), dir = 1).toLong() else f
            sampleEditorState = if (sampleEditorState.cursorCol == 0) {
                val raw = (sampleEditorState.selectionStart + step).coerceAtMost(sampleEditorState.selectionEnd - 1L)
                sampleEditorState.copy(selectionStart = snapFrame(raw).coerceAtMost(sampleEditorState.selectionEnd - 1L))
            } else {
                val raw = (sampleEditorState.selectionEnd + step).coerceAtMost(maxFrame)
                sampleEditorState.copy(selectionEnd = snapFrame(raw).coerceAtLeast(sampleEditorState.selectionStart + 1L).coerceAtMost(maxFrame))
            }
        } else {
            handleSelectionOrSingleIncrement { ctx -> trackerController.inputController.handleARight(ctx) }
        }
    }

    fun handleAB() {
        if (trackerController.inputController.isSelectionModeActive()) {
            val bounds = trackerController.inputController.getSelectionBounds()
            if (bounds != null) {
                when (trackerController.currentScreen) {
                    ScreenType.PHRASE -> clipboardManager.deletePhraseSteps(trackerController.project, trackerController.currentPhrase, bounds.topLeftRow, bounds.topLeftColumn, bounds.bottomRightRow, bounds.bottomRightColumn)
                    ScreenType.CHAIN  -> clipboardManager.deleteChainRows(trackerController.project, trackerController.currentChain, bounds.topLeftRow, bounds.topLeftColumn, bounds.bottomRightRow, bounds.bottomRightColumn)
                    ScreenType.SONG   -> clipboardManager.deleteSongCells(trackerController.project, bounds.topLeftRow, bounds.topLeftColumn, bounds.bottomRightRow, bounds.bottomRightColumn)
                    ScreenType.TABLE  -> { clipboardManager.deleteTableRows(trackerController.project, trackerController.currentTable, bounds.topLeftRow, bounds.topLeftColumn, bounds.bottomRightRow, bounds.bottomRightColumn); audioEngine.invalidateTable(trackerController.currentTable) }
                    else -> { }
                }
                trackerController.inputController.exitSelectionMode()
            }
        } else if (eqEditorState.isOpen) {
            // EQ overlay open → A+B resets the band param to its default (mirrors the other A-combo
            // handlers, which all check eqEditorState.isOpen before the screen-specific branches).
            handleGenericInput { ctx -> trackerController.inputController.handleABCombo(ctx) }
        } else if (trackerController.currentScreen == ScreenType.SAMPLE_EDITOR && sampleEditorState.cursorRow == 8) {
            sampleEditorState = when (sampleEditorState.cursorCol) {
                0 -> sampleEditorState.copy(selectionStart = 0L)
                1 -> sampleEditorState.copy(selectionEnd = sampleEditorState.totalFrames.toLong())
                else -> sampleEditorState
            }
        } else if (trackerController.currentScreen == ScreenType.INST_POOL && trackerController.poolCursorColumn == 0) {
            // A+B on the NAME column clears the instrument slot (M8: EDIT+OPTION clears instrument).
            instrumentController.clearInstrument(trackerController.project, trackerController.currentInstrument)
            trackerController.projectVersion++
        } else {
            handleGenericInput { ctx -> trackerController.inputController.handleABCombo(ctx) }
        }
    }

    fun handleBLeft() {
        if (themeEditorState.isOpen) return
        if (eqEditorState.isOpen) {
            val newSlot = (eqEditorState.slotIndex - 1).coerceIn(0, 127)
            eqEditorState = eqEditorState.copy(slotIndex = newSlot)
            applyCallerEqSlotChange(newSlot)
        } else {
            when (trackerController.currentScreen) {
                ScreenType.CHAIN -> { trackerController.currentChain = if (trackerController.currentChain > 0) trackerController.currentChain - 1 else 255; trackerController.lastEditedChain = trackerController.currentChain }
                ScreenType.PHRASE -> { trackerController.currentPhrase = if (trackerController.currentPhrase > 0) trackerController.currentPhrase - 1 else 255; trackerController.lastEditedPhrase = trackerController.currentPhrase }
                ScreenType.INSTRUMENT -> { val n = if (trackerController.currentInstrument > 0) trackerController.currentInstrument - 1 else 127; trackerController.currentInstrument = n; trackerController.lastEditedInstrument = n; instrumentController.currentInstrument = n }
                ScreenType.MODS -> { val n = if (trackerController.currentInstrument > 0) trackerController.currentInstrument - 1 else 127; trackerController.currentInstrument = n; trackerController.lastEditedInstrument = n; instrumentController.currentInstrument = n }
                ScreenType.TABLE  -> { val n = if (trackerController.currentTable > 0) trackerController.currentTable - 1 else 127; trackerController.currentTable = n; trackerController.lastEditedTable = n }
                ScreenType.GROOVE -> { trackerController.currentGroove = if (trackerController.currentGroove > 0) trackerController.currentGroove - 1 else 127 }
                else -> { }
            }
        }
    }

    fun handleBRight() {
        if (themeEditorState.isOpen) return
        if (eqEditorState.isOpen) {
            val newSlot = (eqEditorState.slotIndex + 1).coerceIn(0, 127)
            eqEditorState = eqEditorState.copy(slotIndex = newSlot)
            applyCallerEqSlotChange(newSlot)
        } else {
            when (trackerController.currentScreen) {
                ScreenType.CHAIN -> { trackerController.currentChain = if (trackerController.currentChain < 255) trackerController.currentChain + 1 else 0; trackerController.lastEditedChain = trackerController.currentChain }
                ScreenType.PHRASE -> { trackerController.currentPhrase = if (trackerController.currentPhrase < 255) trackerController.currentPhrase + 1 else 0; trackerController.lastEditedPhrase = trackerController.currentPhrase }
                ScreenType.INSTRUMENT -> { val n = if (trackerController.currentInstrument < 127) trackerController.currentInstrument + 1 else 0; trackerController.currentInstrument = n; trackerController.lastEditedInstrument = n; instrumentController.currentInstrument = n }
                ScreenType.MODS -> { val n = if (trackerController.currentInstrument < 127) trackerController.currentInstrument + 1 else 0; trackerController.currentInstrument = n; trackerController.lastEditedInstrument = n; instrumentController.currentInstrument = n }
                ScreenType.TABLE  -> { val n = if (trackerController.currentTable < 127) trackerController.currentTable + 1 else 0; trackerController.currentTable = n; trackerController.lastEditedTable = n }
                ScreenType.GROOVE -> { trackerController.currentGroove = if (trackerController.currentGroove < 127) trackerController.currentGroove + 1 else 0 }
                else -> { }
            }
        }
    }
    fun handleBUp()    { when (trackerController.currentScreen) {
        ScreenType.SONG      -> trackerController.moveSongBigUp()
        ScreenType.INST_POOL -> trackerController.poolBigUp()
        else -> {} } }
    fun handleBDown()  { when (trackerController.currentScreen) {
        ScreenType.SONG      -> trackerController.moveSongBigDown()
        ScreenType.INST_POOL -> trackerController.poolBigDown()
        else -> {} } }
    fun handleRUp() {
        if (confirmDialogOpen()) return
        if (qwertyKeyboardState.isOpen) { qwertyKeyboardState = qwertyKeyboardState.copy(layout = 0).withClampedCol(); return }
        if (themeEditorState.isOpen) return
        if (eqEditorState.isOpen || trackerController.currentScreen == ScreenType.SAMPLE_EDITOR || trackerController.currentScreen == ScreenType.SETTINGS) return
        if (trackerController.currentScreen == ScreenType.FILE_BROWSER) {
            val modes = FileSortMode.values()
            val currentIndex = modes.indexOf(fileBrowserState.sortMode)
            fileBrowserState = fileBrowserState.copy(sortMode = modes[(currentIndex + 1) % modes.size])
        } else {
            val (newScreen, newCol) = trackerController.navigateUp(trackerController.currentScreen, trackerController.previousColumn)
            if (newScreen != trackerController.currentScreen) {
                trackerController.saveCursorForScreen(trackerController.currentScreen)
                trackerController.restoreCursorForScreen(newScreen, cursorRemember)
                trackerController.inputController.exitSelectionMode()
            }
            trackerController.currentScreen = newScreen
            trackerController.previousColumn = newCol
        }
    }

    fun handleRDown() {
        if (confirmDialogOpen()) return
        if (qwertyKeyboardState.isOpen) { qwertyKeyboardState = qwertyKeyboardState.copy(layout = 1).withClampedCol(); return }
        if (themeEditorState.isOpen) return
        if (eqEditorState.isOpen || trackerController.currentScreen == ScreenType.SAMPLE_EDITOR || trackerController.currentScreen == ScreenType.SETTINGS) return
        if (trackerController.currentScreen == ScreenType.FILE_BROWSER) {
            val modes = FileSortMode.values()
            val currentIndex = modes.indexOf(fileBrowserState.sortMode)
            fileBrowserState = fileBrowserState.copy(sortMode = modes[(currentIndex - 1 + modes.size) % modes.size])
        } else {
            val (newScreen, newCol) = trackerController.navigateDown(trackerController.currentScreen, trackerController.previousColumn)
            if (newScreen != trackerController.currentScreen) {
                trackerController.saveCursorForScreen(trackerController.currentScreen)
                trackerController.restoreCursorForScreen(newScreen, cursorRemember)
                trackerController.inputController.exitSelectionMode()
            }
            trackerController.currentScreen = newScreen
            trackerController.previousColumn = newCol
        }
    }

    fun handleRLeft() {
        if (confirmDialogOpen()) return
        if (qwertyKeyboardState.isOpen) { qwertyKeyboardState = qwertyKeyboardState.moveTextCursorLeft(); return }
        if (themeEditorState.isOpen) return
        if (eqEditorState.isOpen || trackerController.currentScreen == ScreenType.SAMPLE_EDITOR || trackerController.currentScreen == ScreenType.SETTINGS) return
        if (trackerController.currentScreen == ScreenType.FILE_BROWSER) {
            fileBrowserState = fileBrowserModule.navigateToParent(fileBrowserState)
        } else {
            val (newScreen, newCol) = trackerController.navigateLeft(trackerController.currentScreen, trackerController.previousColumn)
            if (newScreen != trackerController.currentScreen) {
                when (trackerController.currentScreen) {
                    ScreenType.PHRASE -> {
                        val context = phraseEditorModule.getCursorContext(
                            PhraseEditorState(
                                trackerController.project.phrases[trackerController.currentPhrase],
                                trackerController.cursorRow,
                                trackerController.cursorColumn,
                                playbackRow = 0,
                                isPlaying = trackerController.isPlaying()
                            )
                        )
                        if (!context.capabilities.isEmpty) trackerController.lastEditedInstrument = trackerController.project.phrases[trackerController.currentPhrase].steps[trackerController.cursorRow].instrument
                    }
                    ScreenType.CHAIN -> { val ref = trackerController.project.chains[trackerController.currentChain].phraseRefs[trackerController.cursorRow]; if (ref >= 0) trackerController.lastEditedPhrase = ref }
                    ScreenType.SONG  -> { val track = trackerController.project.tracks[trackerController.cursorColumn - 1]; if (trackerController.cursorRow < track.chainRefs.size && track.chainRefs[trackerController.cursorRow] >= 0) trackerController.lastEditedChain = track.chainRefs[trackerController.cursorRow] }
                    else -> {}
                }
                when (newScreen) {
                    ScreenType.PHRASE     -> trackerController.currentPhrase = trackerController.lastEditedPhrase
                    ScreenType.CHAIN      -> trackerController.currentChain  = trackerController.lastEditedChain
                    ScreenType.INSTRUMENT -> trackerController.currentInstrument = trackerController.lastEditedInstrument
                    else -> {}
                }
                trackerController.saveCursorForScreen(trackerController.currentScreen)
                trackerController.restoreCursorForScreen(newScreen, cursorRemember)
                trackerController.inputController.exitSelectionMode()
            }
            trackerController.currentScreen = newScreen
            trackerController.previousColumn = newCol
        }
    }

    fun handleRRight() {
        if (confirmDialogOpen()) return
        if (qwertyKeyboardState.isOpen) { qwertyKeyboardState = qwertyKeyboardState.moveTextCursorRight(); return }
        if (themeEditorState.isOpen) return
        if (eqEditorState.isOpen || trackerController.currentScreen == ScreenType.FILE_BROWSER || trackerController.currentScreen == ScreenType.SAMPLE_EDITOR || trackerController.currentScreen == ScreenType.SETTINGS) return
        val (newScreen, newCol) = trackerController.navigateRight(trackerController.currentScreen, trackerController.previousColumn)
        if (newScreen != trackerController.currentScreen) {
            when (trackerController.currentScreen) {
                ScreenType.PHRASE -> {
                    val context = phraseEditorModule.getCursorContext(
                        PhraseEditorState(
                            trackerController.project.phrases[trackerController.currentPhrase],
                            trackerController.cursorRow,
                            trackerController.cursorColumn,
                            playbackRow = 0,
                            isPlaying = trackerController.isPlaying()
                        )
                    )
                    if (!context.capabilities.isEmpty) trackerController.lastEditedInstrument = trackerController.project.phrases[trackerController.currentPhrase].steps[trackerController.cursorRow].instrument
                }
                ScreenType.CHAIN -> { val ref = trackerController.project.chains[trackerController.currentChain].phraseRefs[trackerController.cursorRow]; if (ref >= 0) trackerController.lastEditedPhrase = ref }
                ScreenType.SONG  -> { val track = trackerController.project.tracks[trackerController.cursorColumn - 1]; if (trackerController.cursorRow < track.chainRefs.size && track.chainRefs[trackerController.cursorRow] >= 0) trackerController.lastEditedChain = track.chainRefs[trackerController.cursorRow] }
                else -> {}
            }
            when (newScreen) {
                ScreenType.INSTRUMENT -> trackerController.currentInstrument = trackerController.lastEditedInstrument
                ScreenType.PHRASE     -> trackerController.currentPhrase = trackerController.lastEditedPhrase
                ScreenType.CHAIN      -> trackerController.currentChain  = trackerController.lastEditedChain
                else -> {}
            }
            trackerController.saveCursorForScreen(trackerController.currentScreen)
            trackerController.restoreCursorForScreen(newScreen, cursorRemember)
            trackerController.inputController.exitSelectionMode()
        }
        trackerController.currentScreen = newScreen
        trackerController.previousColumn = newCol
    }
    fun handleLLeft()  { }
    fun handleLRight() { }
    fun handleLUp()    { }
    fun handleLDown()  { }
    fun handleLA() {
        if (themeEditorState.isOpen) return
        when (trackerController.currentScreen) {
            ScreenType.PHRASE -> {
                when (val action = trackerController.inputController.handleSelectA()) {
                    is InputAction.CUT -> {
                        val bounds = trackerController.inputController.getSelectionBounds()
                        if (bounds != null) { clipboardManager.cutPhraseSteps(trackerController.project, trackerController.currentPhrase, bounds.topLeftRow, bounds.topLeftColumn, bounds.bottomRightRow, bounds.bottomRightColumn); trackerController.projectVersion++; trackerController.inputController.exitSelectionMode() }
                    }
                    is InputAction.PASTE -> {
                        val result = clipboardManager.paste(trackerController.project, "PHRASE", trackerController.currentPhrase, trackerController.cursorRow, trackerController.cursorColumn)
                        if (result is ClipboardManager.PasteResult.Success) trackerController.projectVersion++
                    }
                    else -> { }
                }
            }
            ScreenType.CHAIN -> {
                when (val action = trackerController.inputController.handleSelectA()) {
                    is InputAction.CUT -> {
                        val bounds = trackerController.inputController.getSelectionBounds()
                        if (bounds != null) { clipboardManager.cutChainRows(trackerController.project, trackerController.currentChain, bounds.topLeftRow, bounds.topLeftColumn, bounds.bottomRightRow, bounds.bottomRightColumn); trackerController.projectVersion++; trackerController.inputController.exitSelectionMode() }
                    }
                    is InputAction.PASTE -> {
                        val result = clipboardManager.paste(trackerController.project, "CHAIN", trackerController.currentChain, trackerController.cursorRow, trackerController.cursorColumn)
                        if (result is ClipboardManager.PasteResult.Success) trackerController.projectVersion++
                    }
                    else -> { }
                }
            }
            ScreenType.SONG -> {
                when (val action = trackerController.inputController.handleSelectA()) {
                    is InputAction.CUT -> {
                        val bounds = trackerController.inputController.getSelectionBounds()
                        if (bounds != null) { clipboardManager.cutSongCells(trackerController.project, bounds.topLeftRow, bounds.topLeftColumn, bounds.bottomRightRow, bounds.bottomRightColumn); trackerController.projectVersion++; trackerController.inputController.exitSelectionMode() }
                    }
                    is InputAction.PASTE -> {
                        val result = clipboardManager.paste(trackerController.project, "SONG", 0, trackerController.cursorRow, trackerController.cursorColumn)
                        if (result is ClipboardManager.PasteResult.Success) trackerController.projectVersion++
                    }
                    else -> { }
                }
            }
            ScreenType.TABLE -> {
                when (val action = trackerController.inputController.handleSelectA()) {
                    is InputAction.CUT -> {
                        val bounds = trackerController.inputController.getSelectionBounds()
                        if (bounds != null) { clipboardManager.cutTableRows(trackerController.project, trackerController.currentTable, bounds.topLeftRow, bounds.topLeftColumn, bounds.bottomRightRow, bounds.bottomRightColumn); trackerController.projectVersion++; audioEngine.invalidateTable(trackerController.currentTable); trackerController.inputController.exitSelectionMode() }
                    }
                    is InputAction.PASTE -> {
                        val result = clipboardManager.paste(trackerController.project, "TABLE", trackerController.currentTable, trackerController.tableCursorRow, trackerController.tableCursorColumn)
                        if (result is ClipboardManager.PasteResult.Success) { trackerController.projectVersion++; audioEngine.invalidateTable(trackerController.currentTable) }
                    }
                    else -> { }
                }
            }
            ScreenType.FILE_BROWSER -> {
                if (fileBrowserState.mode != FileBrowserModule.BrowserMode.NORMAL) return
                if (fileBrowserState.selectionMode) {
                    val files = getFileBrowserSelectedFiles()
                    if (files.isNotEmpty()) {
                        val n = files.size
                        fileBrowserState = fileBrowserState.copy(
                            selectionMode = false, selectionAnchor = -1,
                            fileClipboard = files, fileClipboardIsCut = true,
                            statusMessage = "CUT $n ${if (n == 1) "FILE" else "FILES"}", statusSuccess = true
                        )
                    }
                } else if (fileBrowserState.fileClipboard.isNotEmpty()) {
                    pasteBrowserFiles()
                }
            }
            else -> { }
        }
    }

    fun handleLB() {
        when (trackerController.currentScreen) {
            ScreenType.PHRASE  -> trackerController.inputController.handleSelectB(trackerController.cursorRow, trackerController.cursorColumn, 9)
            ScreenType.CHAIN   -> trackerController.inputController.handleSelectB(trackerController.cursorRow, trackerController.cursorColumn, 2)
            ScreenType.SONG    -> trackerController.inputController.handleSelectB(trackerController.cursorRow, trackerController.cursorColumn, 8, maxRow = 255)
            ScreenType.TABLE   -> trackerController.inputController.handleSelectB(trackerController.tableCursorRow, trackerController.tableCursorColumn, 8)
            ScreenType.FILE_BROWSER -> {
                if (fileBrowserState.mode != FileBrowserModule.BrowserMode.NORMAL) return
                val now = System.currentTimeMillis()
                if (!fileBrowserState.selectionMode) {
                    fileBrowserState = fileBrowserState.copy(selectionMode = true, selectionAnchor = fileBrowserState.cursor)
                    lastFileBrowserLBTime = now
                } else if (now - lastFileBrowserLBTime <= 500L) {
                    // Select all (skipping the ".." parent entry)
                    val firstSelectable = if (fileBrowserState.items.firstOrNull() is FileBrowserModule.BrowserItem.Parent) 1 else 0
                    val lastIdx = (fileBrowserState.items.size - 1).coerceAtLeast(firstSelectable)
                    val newScroll = maxOf(0, lastIdx - FileBrowserModule.VISIBLE_ROWS + 1)
                    fileBrowserState = fileBrowserState.copy(selectionAnchor = firstSelectable, cursor = lastIdx, scroll = newScroll)
                    lastFileBrowserLBTime = 0L
                } else {
                    fileBrowserState = fileBrowserState.copy(selectionAnchor = fileBrowserState.cursor)
                    lastFileBrowserLBTime = now
                }
            }
            else -> { }
        }
    }

    private fun getFileBrowserSelectedFiles(): List<File> {
        val state = fileBrowserState
        val range = state.selectedRange ?: return emptyList()
        return range.mapNotNull { idx ->
            val item = state.items.getOrNull(idx)
            if (item != null && item !is FileBrowserModule.BrowserItem.Parent) item.file else null
        }
    }

    private fun pasteBrowserFiles() {
        val state = fileBrowserState
        if (state.fileClipboard.isEmpty()) return
        val destDir = state.currentDirectory
        var doneCount = 0; var failCount = 0
        for (srcFile in state.fileClipboard) {
            if (!srcFile.exists()) { failCount++; continue }
            var destFile = File(destDir, srcFile.name)
            if (destFile.absolutePath == srcFile.absolutePath) { doneCount++; continue }
            if (destFile.exists()) {
                var counter = 2
                val ext = srcFile.extension
                val base = srcFile.nameWithoutExtension
                do {
                    val newName = if (ext.isEmpty()) "${base}_$counter" else "${base}_$counter.$ext"
                    destFile = File(destDir, newName)
                    counter++
                } while (destFile.exists())
            }
            val ok = if (state.fileClipboardIsCut) fileSystem.moveFile(srcFile.absolutePath, destFile.absolutePath)
                     else srcFile.copyTo(destFile, overwrite = false).exists()
            if (ok) doneCount++ else failCount++
        }
        val verb = if (state.fileClipboardIsCut) "MOVED" else "COPIED"
        val newClipboard = if (state.fileClipboardIsCut) emptyList() else state.fileClipboard
        val msg = if (failCount == 0) "$verb $doneCount ${if (doneCount == 1) "FILE" else "FILES"}"
                  else "$verb $doneCount, FAILED $failCount"
        fileBrowserState = fileBrowserModule.navigateToFolder(
            state.copy(fileClipboard = newClipboard, statusMessage = msg, statusSuccess = failCount == 0),
            destDir
        )
    }

    fun handleSelectA() {
        if (trackerController.currentScreen == ScreenType.FILE_BROWSER && fileBrowserState.mode == FileBrowserModule.BrowserMode.NORMAL) {
            val item = fileBrowserState.items.getOrNull(fileBrowserState.cursor)
            if (item != null && item !is FileBrowserModule.BrowserItem.Parent) {
                val file = item.file
                val fieldLabel = when { file.isDirectory -> "FOLDER NAME:"; file.extension.lowercase() == "wav" -> "SAMPLE NAME:"; file.extension.lowercase() == "ptp" -> "PROJECT NAME:"; else -> "FILE NAME:" }
                val baseName = file.nameWithoutExtension
                qwertyKeyboardState = QwertyKeyboardState(
                    isOpen = true,
                    text = baseName,
                    textCursor = baseName.length.coerceAtMost(20),
                    fieldLabel = fieldLabel,
                    originalText = baseName,
                    insertBefore = insertBefore,
                    context = QwertyContext.FILE_RENAME,
                    contextExtra = file.absolutePath
                )
            }
        }
    }

    fun handleSelectB() {
        if (trackerController.currentScreen == ScreenType.FILE_BROWSER && fileBrowserState.mode == FileBrowserModule.BrowserMode.NORMAL) {
            val item = fileBrowserState.items.getOrNull(fileBrowserState.cursor)
            if (item != null && item !is FileBrowserModule.BrowserItem.Parent)
                fileBrowserState = fileBrowserState.copy(mode = FileBrowserModule.BrowserMode.DELETE, statusMessage = "", statusSuccess = true)
        }
    }

    fun handleSelectR() {
        if (trackerController.currentScreen == ScreenType.FILE_BROWSER && fileBrowserState.mode == FileBrowserModule.BrowserMode.NORMAL) {
            val currentDir = fileBrowserState.currentDirectory
            val initialText = "NEW FOLDER"
            qwertyKeyboardState = QwertyKeyboardState(
                isOpen = true,
                text = initialText,
                textCursor = initialText.length.coerceAtMost(20),
                fieldLabel = "FOLDER NAME:",
                originalText = initialText,
                insertBefore = insertBefore,
                context = QwertyContext.FOLDER_CREATE,
                contextExtra = currentDir.absolutePath
            )
        }
    }

    fun handleAA() {
        val currentScreen = trackerController.currentScreen
        if (currentScreen == ScreenType.SONG && trackerController.inputController.isSelectionModeActive()) {
            val suggestedName = renderController.generateResampledBaseName()
            qwertyKeyboardState = QwertyKeyboardState(
                isOpen = true,
                text = suggestedName,
                textCursor = suggestedName.length.coerceAtMost(20),
                fieldLabel = "SAMPLE NAME:",
                originalText = suggestedName,
                insertBefore = insertBefore,
                clearOnFirstB = true,
                context = QwertyContext.RESAMPLE
            )
            return
        }
        val currentPos = InsertPosition(currentScreen, trackerController.cursorRow, trackerController.cursorColumn)
        if (lastAInsertPosition != currentPos) return
        lastAInsertPosition = null
        when (currentScreen) {
            ScreenType.SONG -> {
                val track = trackerController.project.tracks[trackerController.cursorColumn - 1]
                val start = trackerController.lastEditedChain + 1
                val nextEmpty = ((start..255) + (0 until start)).firstOrNull { trackerController.project.chains[it].phraseRefs.all { ref -> ref == -1 } }
                if (nextEmpty != null) { track.chainRefs[trackerController.cursorRow] = nextEmpty; trackerController.lastEditedChain = nextEmpty; trackerController.projectVersion++ }
            }
            ScreenType.CHAIN -> {
                val chain = trackerController.project.chains[trackerController.currentChain]
                val start = trackerController.lastEditedPhrase + 1
                val nextEmpty = ((start..255) + (0 until start)).firstOrNull { trackerController.project.phrases[it].steps.all { step -> step.isEmpty() } }
                if (nextEmpty != null) { chain.phraseRefs[trackerController.cursorRow] = nextEmpty; chain.transposeValues[trackerController.cursorRow] = trackerController.lastEditedTranspose; trackerController.lastEditedPhrase = nextEmpty; trackerController.projectVersion++ }
            }
            else -> {}
        }
    }

    /** Phrase IDs referenced by any chain. A phrase counts as "used" even when its steps are
     *  empty (e.g. a silent spacer inside a pad chain), so clone targets must skip these or
     *  cloning would overwrite a phrase another chain depends on. */
    private fun usedPhraseIds(): Set<Int> {
        val used = HashSet<Int>()
        for (c in trackerController.project.chains) for (ref in c.phraseRefs) if (ref != -1) used.add(ref)
        return used
    }

    /** Chain IDs referenced by any song track (same "used even if empty" reasoning as phrases). */
    private fun usedChainIds(): Set<Int> {
        val used = HashSet<Int>()
        for (t in trackerController.project.tracks) for (ref in t.chainRefs) if (ref != -1) used.add(ref)
        return used
    }

    fun handleLBA() {
        when (trackerController.currentScreen) {
            ScreenType.SONG -> {
                val project = trackerController.project
                val track = project.tracks[trackerController.cursorColumn - 1]
                val currentChainId = track.chainRefs.getOrNull(trackerController.cursorRow) ?: -1
                if (currentChainId != -1) {
                    val src = project.chains[currentChainId]
                    val usedChains = usedChainIds()
                    val usedPhrases = usedPhraseIds()

                    // Destination must be a FREE chain slot: empty AND not referenced by any track.
                    val chainStart = currentChainId + 1
                    val dstChainId = ((chainStart..255) + (0 until chainStart)).firstOrNull {
                        it !in usedChains && project.chains[it].phraseRefs.all { ref -> ref == -1 }
                    }

                    // Deep clone: every unique phrase the chain references gets its own FREE slot
                    // (empty AND unreferenced — a used-but-empty phrase like a pad spacer must not be
                    // overwritten) so the cloned chain is fully independent. Duplicate refs map to the
                    // same clone; `reserved` tracks slots already taken within this clone.
                    val srcPhraseIds = src.phraseRefs.filter { it != -1 }.distinct()
                    val reserved = HashSet<Int>()
                    val phraseMap = HashMap<Int, Int>()
                    var enoughPhrases = true
                    for (pid in srcPhraseIds) {
                        val slot = (0..255).firstOrNull {
                            it !in reserved && it !in usedPhrases &&
                                project.phrases[it].steps.all { step -> step.isEmpty() }
                        }
                        if (slot == null) { enoughPhrases = false; break }
                        reserved.add(slot); phraseMap[pid] = slot
                    }

                    // Capacity is fully checked before any mutation — abort, never half-clone.
                    when {
                        dstChainId == null -> {
                            trackerController.statusMessage = "NO FREE CHAINS"; trackerController.statusSuccess = false
                        }
                        !enoughPhrases -> {
                            trackerController.statusMessage = "NO FREE PHRASES"; trackerController.statusSuccess = false
                        }
                        else -> {
                            for ((srcPid, dstPid) in phraseMap) {
                                val sp = project.phrases[srcPid]; val dp = project.phrases[dstPid]
                                sp.steps.forEachIndexed { i, step -> dp.steps[i] = step.copy() }
                            }
                            val dst = project.chains[dstChainId]
                            src.phraseRefs.forEachIndexed { i, ref ->
                                dst.phraseRefs[i] = if (ref == -1) -1 else phraseMap.getValue(ref)
                            }
                            src.transposeValues.copyInto(dst.transposeValues)
                            track.chainRefs[trackerController.cursorRow] = dstChainId
                            trackerController.lastEditedChain = dstChainId
                            trackerController.statusMessage = "CHAIN CLONED"; trackerController.statusSuccess = true
                            trackerController.projectVersion++
                        }
                    }
                }
            }
            ScreenType.CHAIN -> {
                val project = trackerController.project
                val chain = project.chains[trackerController.currentChain]
                val currentPhraseId = chain.phraseRefs[trackerController.cursorRow]
                if (currentPhraseId != -1) {
                    // Target the next FREE phrase: empty AND unreferenced (skip used-but-empty ones).
                    val usedPhrases = usedPhraseIds()
                    val start = currentPhraseId + 1
                    val nextEmpty = ((start..255) + (0 until start)).firstOrNull {
                        it !in usedPhrases && project.phrases[it].steps.all { step -> step.isEmpty() }
                    }
                    if (nextEmpty != null) {
                        val src = project.phrases[currentPhraseId]; val dst = project.phrases[nextEmpty]
                        src.steps.forEachIndexed { i, step -> dst.steps[i] = step.copy() }
                        chain.phraseRefs[trackerController.cursorRow] = nextEmpty; trackerController.lastEditedPhrase = nextEmpty; trackerController.projectVersion++
                    }
                }
            }
            ScreenType.PHRASE -> {
                val project = trackerController.project
                val srcPhraseId = trackerController.currentPhrase
                // Target the next FREE phrase: empty AND unreferenced (and never the source itself).
                val usedPhrases = usedPhraseIds()
                val start = srcPhraseId + 1
                val nextEmpty = ((start..255) + (0 until start)).firstOrNull {
                    it != srcPhraseId && it !in usedPhrases &&
                        project.phrases[it].steps.all { step -> step.isEmpty() }
                }
                if (nextEmpty != null) {
                    val src = project.phrases[srcPhraseId]; val dst = project.phrases[nextEmpty]
                    src.steps.forEachIndexed { i, step -> dst.steps[i] = step.copy() }
                    trackerController.currentPhrase = nextEmpty; trackerController.projectVersion++
                }
            }
            else -> {}
        }
        if (trackerController.inputController.isSelectionModeActive()) trackerController.inputController.exitSelectionMode()
    }
    fun handleLR() {
        if (trackerController.currentScreen == ScreenType.FILE_BROWSER && fileBrowserState.selectionMode) {
            fileBrowserState = fileBrowserState.copy(selectionMode = false, selectionAnchor = -1)
            return
        }
        if (trackerController.inputController.isSelectionModeActive()) trackerController.inputController.exitSelectionMode()
    }
    fun handleAReleased() {
        if (fxHelperState.isOpen) {
            applyFxTypeChange(fxHelperState.selectedEffectCode())
            fxHelperState = FxHelperState()
        }
    }

    // Backs the "press any button to stop preview" UX (ButtonHandlers.onStopPreview). The InputMapper
    // has already excluded START and A-held edit presses, so here we only need to silence the audition
    // when the current screen is one that can start a preview. Previews play on a dedicated voice, so
    // this never affects song playback. The EQ editor counts when opened over an instrument, where its
    // band edits sweep a held preview live.
    private fun stopActivePreview() {
        if (!audioEngine.isReady) return
        val previewScreen = when (trackerController.currentScreen) {
            ScreenType.FILE_BROWSER,
            ScreenType.INSTRUMENT,
            ScreenType.INST_POOL,
            ScreenType.MODS,
            ScreenType.TABLE -> true
            ScreenType.PHRASE -> notePreviewEnabled
            else -> false
        } || (eqEditorState.isOpen && eqEditorState.callerContext is EqCallerContext.InstrumentEq)
        if (previewScreen) audioEngine.stopPreview()
    }

    fun createButtonHandlers(): ButtonHandlers = ButtonHandlers(
        onDPadUp    = { handleDPadUp() },
        onDPadDown  = { handleDPadDown() },
        onDPadLeft  = { handleDPadLeft() },
        onDPadRight = { handleDPadRight() },
        onButtonA   = { handleButtonA() },
        onButtonB   = { handleButtonB() },
        onSelect    = { handleSelect() },
        onStart     = { handleStart() },
        onL         = { handleL() },
        onR         = { handleR() },
        onAUp       = { handleAUp() },
        onADown     = { handleADown() },
        onALeft     = { handleALeft() },
        onARight    = { handleARight() },
        onAB        = { handleAB() },
        onBLeft     = { handleBLeft() },
        onBRight    = { handleBRight() },
        onBUp       = { handleBUp() },
        onBDown     = { handleBDown() },
        onRUp       = { handleRUp() },
        onRDown     = { handleRDown() },
        onRLeft     = { handleRLeft() },
        onRRight    = { handleRRight() },
        onLLeft     = { handleLLeft() },
        onLRight    = { handleLRight() },
        onLUp       = { handleLUp() },
        onLDown     = { handleLDown() },
        onLA        = { handleLA() },
        onLB        = { handleLB() },
        onSelectA   = { handleSelectA() },
        onSelectB   = { handleSelectB() },
        onSelectR   = { handleSelectR() },
        onAA        = { handleAA() },
        onLBA       = { handleLBA() },
        onLR        = { handleLR() },
        onAReleased = { handleAReleased() },
        onStopPreview = { stopActivePreview() },
        // Item 3: defer single-A to release on sub-screen-opening cells; defer single-B to release
        // while the EQ editor is open (B = close). Keeps the A/B + DPAD combos on those cells intact.
        deferAToRelease = { currentCellOpensSubScreen() },
        deferBToRelease = { eqEditorState.isOpen }
    )
}
