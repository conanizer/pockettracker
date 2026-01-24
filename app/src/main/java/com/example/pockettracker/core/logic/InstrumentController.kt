package com.example.pockettracker.core.logic

import com.example.pockettracker.core.data.Instrument
import com.example.pockettracker.core.data.Note
import com.example.pockettracker.core.data.Project
import com.example.pockettracker.core.audio.AudioEngine
import com.example.pockettracker.core.logging.ILogger

/**
 * InstrumentController
 *
 * Manages all instrument-related operations including:
 * - Instrument selection and navigation
 * - Sample loading from files
 * - Sample/instrument preview
 * - Instrument parameter updates
 *
 * ✅ PLATFORM-AGNOSTIC - No Android dependencies!
 *
 * Updated in Phase 1 refactoring to use the new AudioEngine architecture.
 * Updated in Phase 5 to remove Compose state dependencies.
 */
class InstrumentController(
    private val audioEngine: AudioEngine,
    private val logger: ILogger,
    private val stateObserver: StateObserver
) {
    private val TAG = "InstrumentController"

    // ═══════════════════════════════════════════════════════════════════════════
    // STATE
    // ═══════════════════════════════════════════════════════════════════════════

    /** Currently selected instrument (0-255) */
    var currentInstrument = 0
        set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    /** Last edited instrument ID (for cursor sync between screens) */
    var lastEditedInstrument = 0

    /** Cursor position on instrument screen */
    var cursorRow = 0
        set(value) {
            field = value
            stateObserver.onStateChanged()
        }
    var cursorColumn = 0
        set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    /** Status message for instrument screen */
    var statusMessage = ""
        set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    /** Status success flag (true = success/green, false = error/red) */
    var statusSuccess = true
        set(value) {
            field = value
            stateObserver.onStateChanged()
        }

    // ═══════════════════════════════════════════════════════════════════════════
    // NAVIGATION
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Navigate to previous instrument (L+LEFT on instrument screen)
     * Wraps around: 0 → 255
     *
     * @param onInstrumentChanged Optional callback to sync TrackerController.currentInstrument
     * @return New instrument ID
     */
    fun navigatePrevious(onInstrumentChanged: ((Int) -> Unit)? = null): Int {
        currentInstrument = if (currentInstrument > 0) currentInstrument - 1 else 255
        lastEditedInstrument = currentInstrument
        onInstrumentChanged?.invoke(currentInstrument)
        logger.d(TAG, "⬅️ Navigate to instrument ${formatHex(currentInstrument)}")
        return currentInstrument
    }

    /**
     * Navigate to next instrument (L+RIGHT on instrument screen)
     * Wraps around: 255 → 0
     *
     * @param onInstrumentChanged Optional callback to sync TrackerController.currentInstrument
     * @return New instrument ID
     */
    fun navigateNext(onInstrumentChanged: ((Int) -> Unit)? = null): Int {
        currentInstrument = if (currentInstrument < 255) currentInstrument + 1 else 0
        lastEditedInstrument = currentInstrument
        onInstrumentChanged?.invoke(currentInstrument)
        logger.d(TAG, "➡️ Navigate to instrument ${formatHex(currentInstrument)}")
        return currentInstrument
    }

    /**
     * Jump to specific instrument
     * Used when navigating from other screens (phrase → instrument)
     */
    fun jumpToInstrument(instrumentId: Int) {
        currentInstrument = instrumentId.coerceIn(0, 255)
        lastEditedInstrument = currentInstrument
        logger.d(TAG, "🎯 Jump to instrument ${formatHex(currentInstrument)}")
    }

    /**
     * Sync to last edited instrument (used when entering instrument screen)
     * Also prepares audio engine for the synced instrument.
     */
    fun syncToLastEdited(project: Project? = null) {
        currentInstrument = lastEditedInstrument
        logger.d(TAG, "🔄 Sync to last edited instrument ${formatHex(currentInstrument)}")
        if (project != null) {
            val instrument = project.instruments[currentInstrument]
            audioEngine.updateInstrumentBaseFrequency(instrument)
            audioEngine.updateInstrumentPlaybackParams(instrument)
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // SAMPLE LOADING
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Load sample from file into current instrument
     *
     * @param project Project containing instrument data
     * @param filePath Absolute path to WAV file
     * @return LoadResult indicating success or failure
     */
    fun loadSampleFromFile(project: Project, filePath: String): LoadResult {
        val instrument = project.instruments[currentInstrument]

        logger.d(TAG, "📂 Loading sample: inst=${formatHex(currentInstrument)}, path=$filePath")

        val success = audioEngine.loadSampleFromFile(instrument.id, filePath)

        return if (success) {
            // Update instrument metadata
            instrument.sampleFilePath = filePath
            instrument.sampleId = currentInstrument

            // Extract filename from path (last segment after last slash)
            val filename = filePath.substringAfterLast('/').substringBeforeLast('.')

            // Update status
            setStatus("Loaded: $filename", success = true)

            logger.d(TAG, "✅ Sample loaded successfully")
            LoadResult.Success
        } else {
            setStatus("Failed to load sample", success = false)
            logger.e(TAG, "❌ Sample loading failed")
            LoadResult.Error("Failed to load WAV file")
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // PREVIEW
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Preview sample file (without loading into instrument)
     * Plays at C-4 (261.63 Hz) as reference pitch
     *
     * @param filePath Absolute path to WAV file
     * @return True if successful
     */
    fun previewSampleFile(filePath: String): Boolean {
        logger.d(TAG, "🔊 Previewing sample file: $filePath")
        val success = audioEngine.previewSampleFile(filePath)

        if (!success) {
            setStatus("Cannot preview sample", success = false)
            logger.e(TAG, "❌ Preview failed")
        }

        return success
    }

    /**
     * Preview current instrument with all parameters
     * Plays at ROOT+DETUNE pitch
     *
     * NOTE: Caller should ensure currentInstrument is set to the desired instrument
     * before calling this (typically via syncToLastEdited or explicit assignment).
     *
     * @param project Project containing instrument data
     */
    fun previewInstrument(project: Project) {
        val instrument = project.instruments[currentInstrument]

        logger.d(TAG, "🎵 Previewing instrument ${formatHex(currentInstrument)}: root=${instrument.root}, detune=0x${formatHex(instrument.detune)}")

        audioEngine.previewInstrument(instrument)
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // PARAMETER UPDATES
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Update instrument root note
     * Recalculates base frequency (ROOT + DETUNE)
     */
    fun updateRoot(instrument: Instrument, note: Note) {
        instrument.root = note
        audioEngine.updateInstrumentBaseFrequency(instrument)
        logger.d(TAG, "🎹 Updated ROOT: ${instrument.root}")
    }


    /**
     * Update instrument detune parameter
     * Recalculates base frequency (ROOT + DETUNE)
     */
    fun updateDetune(instrument: Instrument, detune: Int) {
        instrument.detune = detune.coerceIn(0, 255)
        audioEngine.updateInstrumentBaseFrequency(instrument)
        logger.d(TAG, "🎚️ Updated DETUNE: 0x${formatHex(instrument.detune)}")
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Volume/Pan Parameters (MVP Expansion)
    // ─────────────────────────────────────────────────────────────────────────────

    /**
     * Update instrument volume
     * @param volume 00-FF (FF = max volume)
     */
    fun updateVolume(instrument: Instrument, volume: Int) {
        instrument.volume = volume.coerceIn(0, 255)
        logger.d(TAG, "🔊 Updated VOLUME: 0x${formatHex(instrument.volume)}")
    }

    /**
     * Update instrument pan
     * @param pan 00-FF (00=left, 80=center, FF=right)
     */
    fun updatePan(instrument: Instrument, pan: Int) {
        instrument.pan = pan.coerceIn(0, 255)
        logger.d(TAG, "🔊 Updated PAN: 0x${formatHex(instrument.pan)}")
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Distortion/Bitcrusher Parameters
    // ─────────────────────────────────────────────────────────────────────────────

    /**
     * Update drive (pre-gain boost)
     */
    fun updateDrive(instrument: Instrument, drive: Int) {
        instrument.drive = drive.coerceIn(0, 255)
        audioEngine.updateInstrumentPlaybackParams(instrument)
        logger.d(TAG, "🎛️ Updated DRIVE: 0x${formatHex(instrument.drive)}")
    }

    /**
     * Update crush (bit depth reduction)
     */
    fun updateCrush(instrument: Instrument, crush: Int) {
        instrument.crush = crush.coerceIn(0, 15)
        audioEngine.updateInstrumentPlaybackParams(instrument)
        logger.d(TAG, "🎛️ Updated CRUSH: ${instrument.crush}")
    }

    /**
     * Update downsample (sample rate reduction)
     */
    fun updateDownsample(instrument: Instrument, downsample: Int) {
        instrument.downsample = downsample.coerceIn(0, 15)
        audioEngine.updateInstrumentPlaybackParams(instrument)
        logger.d(TAG, "🎛️ Updated DOWNSAMPLE: ${instrument.downsample}")
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Filter Parameters
    // ─────────────────────────────────────────────────────────────────────────────

    /**
     * Update filter type
     */
    fun updateFilterType(instrument: Instrument, filterType: String) {
        instrument.filterType = filterType
        audioEngine.updateInstrumentPlaybackParams(instrument)
        logger.d(TAG, "🎛️ Updated FILTER TYPE: ${instrument.filterType}")
    }

    /**
     * Update filter cutoff frequency
     */
    fun updateFilterCut(instrument: Instrument, filterCut: Int) {
        instrument.filterCut = filterCut.coerceIn(0, 255)
        audioEngine.updateInstrumentPlaybackParams(instrument)
        logger.d(TAG, "🎛️ Updated FILTER CUT: 0x${formatHex(instrument.filterCut)}")
    }

    /**
     * Update filter resonance
     */
    fun updateFilterRes(instrument: Instrument, filterRes: Int) {
        instrument.filterRes = filterRes.coerceIn(0, 255)
        audioEngine.updateInstrumentPlaybackParams(instrument)
        logger.d(TAG, "🎛️ Updated FILTER RES: 0x${formatHex(instrument.filterRes)}")
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Sample Playback Parameters
    // ─────────────────────────────────────────────────────────────────────────────

    /**
     * Update sample start point
     */
    fun updateSampleStart(instrument: Instrument, sampleStart: Int) {
        instrument.sampleStart = sampleStart.coerceIn(0, 255)
        audioEngine.updateInstrumentPlaybackParams(instrument)
        logger.d(TAG, "🎛️ Updated SAMPLE START: 0x${formatHex(instrument.sampleStart)}")
    }

    /**
     * Update sample end point
     */
    fun updateSampleEnd(instrument: Instrument, sampleEnd: Int) {
        instrument.sampleEnd = sampleEnd.coerceIn(0, 255)
        audioEngine.updateInstrumentPlaybackParams(instrument)
        logger.d(TAG, "🎛️ Updated SAMPLE END: 0x${formatHex(instrument.sampleEnd)}")
    }

    /**
     * Update reverse playback
     */
    fun updateReverse(instrument: Instrument, reverse: Boolean) {
        instrument.reverse = reverse
        audioEngine.updateInstrumentPlaybackParams(instrument)
        logger.d(TAG, "🎛️ Updated REVERSE: ${instrument.reverse}")
    }

    /**
     * Update loop mode
     */
    fun updateLoopMode(instrument: Instrument, loopMode: String) {
        instrument.loopMode = loopMode
        audioEngine.updateInstrumentPlaybackParams(instrument)
        logger.d(TAG, "🎛️ Updated LOOP MODE: ${instrument.loopMode}")
    }

    /**
     * Update loop start point
     */
    fun updateLoopStart(instrument: Instrument, loopStart: Int) {
        instrument.loopStart = loopStart.coerceIn(0, 255)
        audioEngine.updateInstrumentPlaybackParams(instrument)
        logger.d(TAG, "🎛️ Updated LOOP START: 0x${formatHex(instrument.loopStart)}")
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Generic Update (legacy, kept for compatibility)
    // ─────────────────────────────────────────────────────────────────────────────

    /**
     * Update playback parameters (generic)
     * @deprecated Use specific update functions instead
     */
    fun updatePlaybackParams(instrument: Instrument) {
        audioEngine.updateInstrumentPlaybackParams(instrument)
        logger.d(TAG, "🎛️ Updated playback params for instrument ${formatHex(instrument.id)}")
    }

    /**
     * Update instrument parameter (generic)
     * Delegates to specific update functions based on parameter type
     *
     * @param instrument The instrument to update
     * @param parameter Which parameter changed
     */
    fun updateParameter(instrument: Instrument, parameter: InstrumentParameter) {
        when (parameter) {
            InstrumentParameter.ROOT, InstrumentParameter.DETUNE -> {
                // These affect base frequency calculation
                audioEngine.updateInstrumentBaseFrequency(instrument)
            }
            else -> {
                // All other parameters affect playback
                audioEngine.updateInstrumentPlaybackParams(instrument)
            }
        }

        lastEditedInstrument = instrument.id
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // STATUS MESSAGES
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Set status message (auto-cleared after 5 seconds by LaunchedEffect in UI)
     */
    fun setStatus(message: String, success: Boolean = true) {
        statusMessage = message
        statusSuccess = success
        logger.d(TAG, if (success) "✅ $message" else "❌ $message")
    }

    /**
     * Clear status message
     */
    fun clearStatus() {
        statusMessage = ""
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // HELPERS
    // ═══════════════════════════════════════════════════════════════════════════

    /** Format instrument ID as 2-digit hex (00-FF) */
    private fun formatHex(value: Int): String {
        return value.toString(16).padStart(2, '0').uppercase()
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// RESULT TYPES
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Result of sample loading operation
 */
sealed class LoadResult {
    object Success : LoadResult()
    data class Error(val message: String) : LoadResult()
}

/**
 * Instrument parameters that can be updated
 * Used to determine which audio engine update function to call
 */
enum class InstrumentParameter {
    ROOT,
    DETUNE,
    VOLUME,
    PAN,
    DRIVE,
    CRUSH,
    DOWNSAMPLE,
    FILTER_TYPE,
    FILTER_CUT,
    FILTER_RES,
    SAMPLE_START,
    SAMPLE_END,
    LOOP_MODE,
    LOOP_START
}
