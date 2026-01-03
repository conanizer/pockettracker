package com.example.pockettracker.core.logic

import com.example.pockettracker.Instrument
import com.example.pockettracker.Note
import com.example.pockettracker.Project
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
    var cursorColumn = 1
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
     */
    fun navigatePrevious() {
        currentInstrument = if (currentInstrument > 0) currentInstrument - 1 else 255
        lastEditedInstrument = currentInstrument
        logger.d(TAG, "⬅️ Navigate to instrument ${formatHex(currentInstrument)}")
    }

    /**
     * Navigate to next instrument (L+RIGHT on instrument screen)
     * Wraps around: 255 → 0
     */
    fun navigateNext() {
        currentInstrument = if (currentInstrument < 255) currentInstrument + 1 else 0
        lastEditedInstrument = currentInstrument
        logger.d(TAG, "➡️ Navigate to instrument ${formatHex(currentInstrument)}")
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
     */
    fun syncToLastEdited() {
        currentInstrument = lastEditedInstrument
        logger.d(TAG, "🔄 Sync to last edited instrument ${formatHex(currentInstrument)}")
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

    /**
     * Update playback parameters
     * Called whenever START/END/REVERSE/LOOP/DRIVE/CRUSH/FILTER changes
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
    DRIVE,
    CRUSH,
    DOWNSAMPLE,
    FILTER_TYPE,
    FILTER_CUT,
    FILTER_RES,
    SAMPLE_START,
    SAMPLE_END,
    REVERSE,
    LOOP_MODE,
    LOOP_START
}
