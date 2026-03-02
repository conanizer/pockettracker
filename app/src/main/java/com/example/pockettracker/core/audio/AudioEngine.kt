package com.example.pockettracker.core.audio

import com.example.pockettracker.core.data.Instrument
import com.example.pockettracker.core.data.ModDest
import com.example.pockettracker.core.data.ModType
import com.example.pockettracker.core.data.Note
import com.example.pockettracker.core.data.Project
import com.example.pockettracker.core.data.Table
import com.example.pockettracker.core.data.VolumeUtils
import com.example.pockettracker.core.logging.ILogger
import com.example.pockettracker.core.resources.IResourceLoader
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Platform-agnostic audio engine.
 *
 * This wraps the IAudioBackend interface to provide high-level audio functionality
 * without depending on Android-specific code.
 *
 * ✅ FULLY PLATFORM-AGNOSTIC - No Android dependencies!
 * - Uses interfaces for all platform-specific operations
 * - No Context dependency
 * - Pure Kotlin business logic
 *
 * @param backend Platform-specific audio backend (e.g., OboeAudioBackend on Android)
 * @param resourceLoader Platform-specific resource loader (e.g., AndroidResourceLoader on Android)
 * @param logger Platform-specific logger (e.g., AndroidLogger on Android)
 */
class AudioEngine(
    internal val backend: IAudioBackend,
    private val resourceLoader: IResourceLoader,
    private val logger: ILogger
) {
    private val TAG = "AudioEngine"

    // Waveform buffer for visualization (620 samples for 620px width oscilloscope)
    val waveformBuffer = FloatArray(620) { 0f }

    // Sample metadata (base frequencies and sample rate compensation ratios)
    private val sampleBaseFrequencies = mutableMapOf<Int, Float>()
    private val sampleRateRatios = mutableMapOf<Int, Float>()

    /**
     * Initialize audio engine and load default samples.
     * @return true if successful
     */
    fun create(): Boolean {
        val success = backend.create()
        if (success) {
            logger.d(TAG, "✅ Audio engine created successfully")
            loadAllSamples()
        } else {
            logger.e(TAG, "❌ Failed to create audio engine")
        }
        return success
    }

    /**
     * Load default samples from resources.
     *
     * Currently disabled - app starts with empty instrument slots.
     * Users load samples via the file browser.
     *
     * This method can be re-enabled in the future if we want to include
     * default samples for quick start or demo purposes.
     */
    private fun loadAllSamples() {
        // No default samples loaded - users load their own via file browser
        logger.d(TAG, "✅ Audio engine initialized (no default samples)")
    }

    /**
     * Get device sample rate.
     */
    fun getDeviceSampleRate(): Int = backend.getSampleRate()

    /**
     * Load WAV file from external path (for user samples).
     * @param instrumentId Which instrument slot (0-255)
     * @param filePath Absolute path to WAV file
     * @return True if successful
     */
    fun loadSampleFromFile(instrumentId: Int, filePath: String): Boolean {
        try {
            val file = File(filePath)
            if (!file.exists() || !file.canRead()) {
                logger.e(TAG, "Cannot read file: $filePath")
                return false
            }

            val (samples, adjustedBaseFreq) = loadWavFileFromPath(filePath)
            backend.loadSample(instrumentId, samples)

            sampleBaseFrequencies[instrumentId] = adjustedBaseFreq
            sampleRateRatios[instrumentId] = adjustedBaseFreq / 261.63f

            logger.d(TAG, "✅ Loaded sample: instrumentId=$instrumentId, length=${samples.size}, baseFreq=$adjustedBaseFreq, path=$filePath")
            return true
        } catch (e: Exception) {
            logger.e(TAG, "❌ Error loading sample from file: ${e.message}")
            return false
        }
    }

    /**
     * Load WAV file from external path.
     */
    private fun loadWavFileFromPath(filePath: String): Pair<FloatArray, Float> {
        File(filePath).inputStream().use { inputStream ->
            val fileSize = inputStream.available()
            val buffer = ByteArray(fileSize)
            inputStream.read(buffer)

            return parseWavBuffer(buffer)
        }
    }

    /**
     * Parse WAV file buffer into float samples and base frequency.
     * Handles stereo -> mono conversion and sample rate compensation.
     */
    private fun parseWavBuffer(buffer: ByteArray): Pair<FloatArray, Float> {
        // Read number of channels from WAV header (bytes 22-23)
        val channels = ByteBuffer.wrap(buffer, 22, 2)
            .order(ByteOrder.LITTLE_ENDIAN)
            .short.toInt()

        // Read sample rate from WAV header (bytes 24-27)
        val sampleRate = ByteBuffer.wrap(buffer, 24, 4)
            .order(ByteOrder.LITTLE_ENDIAN)
            .int

        // Skip WAV header (44 bytes)
        val dataStart = 44
        val audioDataSize = buffer.size - dataStart
        val shortBuffer = ByteBuffer.wrap(buffer, dataStart, audioDataSize)
            .order(ByteOrder.LITTLE_ENDIAN)
            .asShortBuffer()

        // Convert 16-bit samples to float (-1.0 to 1.0)
        val rawSamples = FloatArray(shortBuffer.remaining())
        for (i in rawSamples.indices) {
            rawSamples[i] = shortBuffer.get(i) / 32768f
        }

        // If stereo, mix down to mono by averaging L+R channels
        val samples = if (channels == 2) {
            FloatArray(rawSamples.size / 2) { i ->
                (rawSamples[i * 2] + rawSamples[i * 2 + 1]) / 2f
            }
        } else {
            rawSamples
        }

        // Calculate adjusted base frequency for sample rate compensation
        val deviceSampleRate = getDeviceSampleRate()
        val sampleRateRatio = deviceSampleRate.toFloat() / sampleRate.toFloat()
        val adjustedBaseFreq = 261.63f * sampleRateRatio

        return Pair(samples, adjustedBaseFreq)
    }

    /**
     * Preview a WAV file without permanently loading it.
     * Uses temporary slot 255 for preview playback.
     */
    fun previewSampleFile(filePath: String): Boolean {
        try {
            val file = File(filePath)
            if (!file.exists() || !file.canRead()) {
                logger.e(TAG, "Cannot read file: $filePath")
                return false
            }

            // Stop all audio before loading new sample
            backend.stopAll()

            val (samples, adjustedBaseFreq) = loadWavFileFromPath(filePath)
            backend.loadSample(255, samples)

            // CRITICAL: Resume stream so audio callback processes the scheduled note
            backend.resumeStream()

            // Play at C-4 as reference pitch
            // Schedule slightly in the future to avoid race conditions
            val c4Freq = 261.63f
            val targetFrame = backend.getCurrentFrame() + 100  // Small buffer
            backend.scheduleNote(
                frame = targetFrame,
                sampleId = 255,
                trackId = 0,
                freq = c4Freq,
                baseFreq = adjustedBaseFreq,
                vol = 1.0f,
                pan = 0.5f  // Center
            )

            logger.d(TAG, "🔊 Preview sample at C-4: $filePath (baseFreq=$adjustedBaseFreq)")
            return true
        } catch (e: Exception) {
            logger.e(TAG, "❌ Error previewing sample: ${e.message}")
            return false
        }
    }

    /**
     * Preview current instrument with all parameters applied.
     * If project is provided, table processing will be applied.
     * Each instrument uses the table with the same ID (instrument 03 → table 03).
     *
     * @param instrument Instrument to preview
     * @param project Project for table data (optional)
     * @param tableIdOverride Override the table ID (-1 = use instrument's ID as table ID)
     */
    fun previewInstrument(
        instrument: Instrument,
        project: Project? = null,
        tableIdOverride: Int = -1
    ) {
        val sampleId = instrument.sampleId

        // Calculate target frequency from ROOT + DETUNE
        val rootFreq = instrument.root.toFrequency()

        // Apply detune
        val detuneSemitones = (instrument.detune shr 4).toFloat()
        val detuneFraction = (instrument.detune and 0x0F) / 16.0f
        val totalDetuneSemitones = detuneSemitones + detuneFraction - 8.0f
        val detuneMultiplier = Math.pow(2.0, (totalDetuneSemitones / 12.0)).toFloat()

        val targetFreq = rootFreq * detuneMultiplier

        // Get compensated base frequency
        val sampleRateRatio = sampleRateRatios[sampleId] ?: 1.0f
        val compensatedBaseFreq = 261.63f * sampleRateRatio

        // CRITICAL: Resume stream so audio callback processes the scheduled note
        backend.resumeStream()

        // Schedule slightly in the future to avoid race conditions
        val targetFrame = backend.getCurrentFrame() + 100  // Small buffer

        // Apply instrument volume and pan
        val volume = VolumeUtils.hexToFloat(instrument.volume)
        val pan = VolumeUtils.hexToFloat(instrument.pan)  // 0x00=left, 0x80=center, 0xFF=right

        // Use override if provided, otherwise use instrument's ID as table ID
        val tableId = if (tableIdOverride >= 0) tableIdOverride else instrument.id
        val tableTicRate = instrument.tableTicRate

        // Force reload table when previewing (user may have just edited it)
        if (tableId >= 0 && project != null && tableId < 256) {
            forceReloadTable(project.tables[tableId])
        }

        // Push current modulation params so preview reflects latest UI edits
        val tempo = project?.tempo ?: 120
        pushInstrumentModulation(instrument, tempo)

        backend.scheduleNoteWithTable(
            frame = targetFrame,
            sampleId = sampleId,
            trackId = 0,
            freq = targetFreq,
            baseFreq = compensatedBaseFreq,
            vol = volume,
            pan = pan,
            startPointOverride = -1,
            tableId = tableId,
            tableTicRate = tableTicRate,
            noteOctave = instrument.root.octave,
            notePitch = instrument.root.pitch,
            pslInitialOffset = 0f,
            pslDuration = 0f,
            pbnRate = 0f,
            vibratoSpeed = 0f,
            vibratoDepth = 0f
        )

        logger.d(TAG, "🔊 Preview instrument ${instrument.id.toString(16).padStart(2,'0').uppercase()}: freq=$targetFreq Hz, vol=$volume, pan=$pan, tableId=$tableId")
    }

    /**
     * Calculate the effective base frequency for an instrument.
     */
    fun calculateInstrumentBaseFrequency(instrument: Instrument): Float {
        val rootFreq = instrument.root.toFrequency()

        // Apply detune
        val detuneSemitones = (instrument.detune shr 4).toFloat()
        val detuneFraction = (instrument.detune and 0x0F) / 16.0f
        val totalDetuneSemitones = detuneSemitones + detuneFraction - 8.0f
        val detuneMultiplier = Math.pow(2.0, (totalDetuneSemitones / 12.0)).toFloat()

        // Apply sample rate compensation
        val sampleRateRatio = sampleRateRatios[instrument.sampleId] ?: 1.0f

        return rootFreq * detuneMultiplier * sampleRateRatio
    }

    /**
     * Update the base frequency for an instrument based on its ROOT and DETUNE.
     */
    fun updateInstrumentBaseFrequency(instrument: Instrument) {
        val baseFreq = calculateInstrumentBaseFrequency(instrument)
        sampleBaseFrequencies[instrument.sampleId] = baseFreq
        logger.d(TAG, "📝 Updated base frequency for instrument ${instrument.id}: $baseFreq Hz")
    }

    /**
     * Play a note on a specific track.
     */
    fun playNote(note: Note, instrumentId: Int, trackId: Int, volume: Float = 1.0f, pan: Float = 0.5f, project: Project? = null) {
        if (note == Note.EMPTY) return

        val sampleId = if (project != null && instrumentId in 0..255) {
            project.instruments[instrumentId].sampleId
        } else {
            instrumentId % 12
        }

        val baseFreq = sampleBaseFrequencies[sampleId] ?: 261.63f
        val frequency = note.toFrequency()

        backend.scheduleNote(
            frame = backend.getCurrentFrame(),
            sampleId = sampleId,
            trackId = trackId,
            freq = frequency,
            baseFreq = baseFreq,
            vol = volume,
            pan = pan
        )
    }

    /**
     * Update waveform buffer from audio output.
     */
    fun updateWaveform() {
        backend.updateWaveform(waveformBuffer)
    }

    /**
     * Decay waveform buffer (call when playback is stopped).
     * This smoothly fades out the oscilloscope display.
     */
    fun decayWaveform() {
        backend.decayWaveform()
    }

    /**
     * Decay peak meters (call when playback is stopped).
     * This smoothly fades out the mixer meter display.
     */
    fun decayPeaks() {
        backend.decayPeaks()
    }

    /**
     * Update waveform with automatic decay when not playing.
     * @param isPlaying Whether playback is currently active
     */
    fun updateWaveformWithDecay(isPlaying: Boolean) {
        if (!isPlaying) {
            backend.decayWaveform()
        }
        backend.updateWaveform(waveformBuffer)
    }

    /**
     * Stop all playback.
     */
    fun stopAll() {
        backend.stopAll()
        waveformBuffer.fill(0f)
    }

    /**
     * Kill a specific track's voice immediately (for K00 Kill effect).
     */
    fun killTrack(trackId: Int) {
        backend.killTrack(trackId)
    }

    /**
     * Schedule a kill event at a specific frame (for sample-accurate Kill effect).
     */
    fun scheduleKill(frame: Long, trackId: Int) {
        backend.scheduleKill(frame, trackId)
    }

    /**
     * Get current audio frame counter (for sample-accurate scheduling).
     */
    fun getCurrentFrame(): Long = backend.getCurrentFrame()

    /**
     * Schedule a note to be played at exact audio frame.
     * Automatically handles table processing - each instrument uses the table with the same ID.
     * Supports per-note pitch modulation effects (PSL, PBN, PVB, PVX).
     *
     * @param pslInitialOffset PSL initial pitch offset in semitones (0 = no PSL)
     * @param pslDuration PSL slide duration in ticks (0 = no slide)
     * @param pbnRate PBN pitch bend rate in semitones/tick (0 = no bend)
     * @param vibratoSpeed PVB/PVX vibrato speed in Hz (0 = no vibrato)
     * @param vibratoDepth PVB/PVX vibrato depth in semitones (0 = no vibrato)
     */
    fun scheduleNote(
        targetFrame: Long,
        note: Note,
        instrumentId: Int,
        trackId: Int,
        volume: Float = 1.0f,
        pan: Float = 0.5f,  // 0.0=left, 0.5=center, 1.0=right
        project: Project,
        startPointOverride: Int = -1,  // -1 = use instrument default, 0-255 = Offset effect override
        pslInitialOffset: Float = 0f,
        pslDuration: Float = 0f,
        pbnRate: Float = 0f,
        vibratoSpeed: Float = 0f,
        vibratoDepth: Float = 0f,
        tableIdOverride: Int = -1,  // TBL effect: override table ID (-1 = use instrument default)
        tableStartRow: Int = -1     // THO effect: force table start row (-1 = default)
    ) {
        if (note == Note.EMPTY) return

        val instrument = if (instrumentId in 0..255) {
            project.instruments[instrumentId]
        } else {
            android.util.Log.w("AudioEngine", "❌ Invalid instrumentId=$instrumentId, skipping note")
            return
        }

        val sampleId = instrument.sampleId

        // Use TBL override if provided, otherwise use instrument ID as table ID
        val tableId = if (tableIdOverride >= 0) tableIdOverride else instrumentId
        val tableTicRate = instrument.tableTicRate

        // Ensure table is loaded
        if (tableId in 0..255) {
            ensureTableLoaded(project.tables[tableId])
        }

        // Debug: Log what we're scheduling
        android.util.Log.d("AudioEngine", "📋 scheduleNote: inst=$instrumentId → sampleId=$sampleId, note=$note, frame=$targetFrame, vol=${"%.4f".format(volume)}, pan=$pan, tableId=$tableId")

        val baseFreq = sampleBaseFrequencies[sampleId] ?: 261.63f
        val frequency = note.toFrequency()

        // Push modulation params to C++ engine (Phase 4 — AHD)
        // Must be done before scheduleNoteWithTable so the engine has correct params at trigger time
        val tempo = project.tempo
        pushInstrumentModulation(instrument, tempo)

        // Resume stream so audio callback can process the queue
        backend.resumeStream()

        // Always use scheduleNoteWithTable - C++ handles tableId=-1 as "no table"
        backend.scheduleNoteWithTable(
            targetFrame, sampleId, trackId, frequency, baseFreq, volume, pan,
            startPointOverride, tableId, tableTicRate, note.octave, note.pitch,
            pslInitialOffset, pslDuration, pbnRate, vibratoSpeed, vibratoDepth,
            tableStartRow
        )
    }

    /**
     * Clear all scheduled notes.
     */
    fun clearScheduledNotes() {
        backend.clearScheduledNotes()
    }

    /**
     * Calculate target frame for a note based on tempo and step number.
     */
    fun calculateTargetFrame(startFrame: Long, stepNumber: Int, tempo: Int): Long {
        val sampleRate = getDeviceSampleRate()
        val msPerStep = (60000.0 / tempo / 4.0)
        val framesPerStep = (msPerStep * sampleRate / 1000.0).toLong()
        return startFrame + (stepNumber * framesPerStep)
    }

    /**
     * Set playback parameters for an instrument.
     */
    fun setInstrumentParams(
        instrumentId: Int,
        startPoint: Int,
        endPoint: Int,
        reverse: Boolean,
        loopMode: Int,
        loopStart: Int,
        drive: Int,
        crush: Int,
        downsample: Int,
        filterType: Int,
        filterCut: Int,
        filterRes: Int
    ) {
        backend.setInstrumentParams(
            instrumentId, startPoint, endPoint, reverse, loopMode, loopStart,
            drive, crush, downsample, filterType, filterCut, filterRes
        )
    }

    /**
     * Update instrument parameters from Instrument data class.
     */
    fun updateInstrumentPlaybackParams(instrument: Instrument) {
        val loopModeInt = when (instrument.loopMode) {
            "fwd" -> 1
            "png" -> 2
            else -> 0
        }

        val filterTypeInt = when (instrument.filterType) {
            "lp" -> 1
            "hp" -> 2
            "bp" -> 3
            else -> 0
        }

        setInstrumentParams(
            instrumentId = instrument.sampleId,
            startPoint = instrument.sampleStart,
            endPoint = instrument.sampleEnd,
            reverse = instrument.reverse,
            loopMode = loopModeInt,
            loopStart = instrument.loopStart,
            drive = instrument.drive,
            crush = instrument.crush,
            downsample = instrument.downsample,
            filterType = filterTypeInt,
            filterCut = instrument.filterCut,
            filterRes = instrument.filterRes
        )
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // TABLE SUPPORT (Phase 3.5)
    // ═══════════════════════════════════════════════════════════════════════════

    /** Track which tables have been loaded to native layer */
    private val loadedTables = mutableSetOf<Int>()

    /**
     * Load a table to the native audio engine.
     *
     * Serializes the table data into the format expected by C++:
     * 16 rows × 8 bytes per row = 128 bytes
     * Each row: [transpose, volume, fx1Type, fx1Value, fx2Type, fx2Value, fx3Type, fx3Value]
     *
     * @param table The table to load
     */
    fun loadTable(table: Table) {
        val rowData = ByteArray(128)
        for (rowIndex in 0 until 16) {
            val row = table.rows[rowIndex]
            val offset = rowIndex * 8
            rowData[offset + 0] = row.transpose.toByte()
            rowData[offset + 1] = row.volume.toByte()
            rowData[offset + 2] = row.fx1Type.toByte()
            rowData[offset + 3] = row.fx1Value.toByte()
            rowData[offset + 4] = row.fx2Type.toByte()
            rowData[offset + 5] = row.fx2Value.toByte()
            rowData[offset + 6] = row.fx3Type.toByte()
            rowData[offset + 7] = row.fx3Value.toByte()
        }

        // Log first row for debugging
        val firstRow = table.rows[0]
        android.util.Log.d(TAG, "📋 Loading table ${table.id}: row0=[transpose=${firstRow.transpose}, vol=${firstRow.volume}]")

        backend.loadTable(table.id, rowData)
        loadedTables.add(table.id)
        logger.d(TAG, "📋 Loaded table ${table.id} to native layer")
    }

    /**
     * Ensure a table is loaded to the native layer.
     * Only loads if not already loaded.
     *
     * @param table The table to ensure is loaded
     */
    fun ensureTableLoaded(table: Table) {
        if (table.id !in loadedTables) {
            loadTable(table)
        }
    }

    /**
     * Force reload a table to the native layer.
     * Use this after editing table data to ensure changes take effect.
     *
     * @param table The table to reload
     */
    fun forceReloadTable(table: Table) {
        loadedTables.remove(table.id)
        loadTable(table)
    }

    /**
     * Mark a table as needing reload (call after editing table data).
     *
     * @param tableId The table ID that was modified
     */
    fun invalidateTable(tableId: Int) {
        loadedTables.remove(tableId)
        android.util.Log.d(TAG, "🔄 Invalidated table $tableId cache")
    }

    /**
     * Clear loaded table tracking (call when project changes).
     */
    fun clearLoadedTables() {
        loadedTables.clear()
        logger.d(TAG, "🗑️ Cleared loaded tables tracking")
    }

    /**
     * Schedule a note with explicit table control.
     * Use this when you need to override the default table (instrument ID = table ID).
     *
     * @param targetFrame When this note should trigger (audio frame)
     * @param note The note to play
     * @param instrumentId Instrument ID (0-255)
     * @param trackId Track ID (0-7)
     * @param volume Note volume (0.0-1.0)
     * @param pan Pan position (0.0=left, 0.5=center, 1.0=right)
     * @param project Project for instrument lookup
     * @param startPointOverride Optional start point override (-1 = use instrument default)
     * @param tableIdOverride Override table ID (-1 = use instrument ID as table ID)
     * @param pslInitialOffset PSL initial pitch offset in semitones (0 = no PSL)
     * @param pslDuration PSL slide duration in ticks (0 = no slide)
     * @param pbnRate PBN pitch bend rate in semitones/tick (0 = no bend)
     * @param vibratoSpeed PVB/PVX vibrato speed in Hz (0 = no vibrato)
     * @param vibratoDepth PVB/PVX vibrato depth in semitones (0 = no vibrato)
     */
    fun scheduleNoteWithTable(
        targetFrame: Long,
        note: Note,
        instrumentId: Int,
        trackId: Int,
        volume: Float = 1.0f,
        pan: Float = 0.5f,
        project: Project,
        startPointOverride: Int = -1,
        tableIdOverride: Int = -1,
        pslInitialOffset: Float = 0f,
        pslDuration: Float = 0f,
        pbnRate: Float = 0f,
        vibratoSpeed: Float = 0f,
        vibratoDepth: Float = 0f,
        tableStartRow: Int = -1
    ) {
        if (note == Note.EMPTY) return

        val instrument = if (instrumentId in 0..255) {
            project.instruments[instrumentId]
        } else {
            android.util.Log.w("AudioEngine", "❌ Invalid instrumentId=$instrumentId, skipping note")
            return
        }

        val sampleId = instrument.sampleId

        // Use override if provided, otherwise use instrument ID as table ID
        val tableId = if (tableIdOverride >= 0) tableIdOverride else instrumentId
        val tableTicRate = instrument.tableTicRate

        // Ensure table is loaded
        if (tableId >= 0 && tableId < 256) {
            ensureTableLoaded(project.tables[tableId])
        }

        val baseFreq = sampleBaseFrequencies[sampleId] ?: 261.63f
        val frequency = note.toFrequency()

        // Resume stream so audio callback can process the queue
        backend.resumeStream()

        android.util.Log.d("AudioEngine", "📋 scheduleNoteWithTable: inst=$instrumentId → sampleId=$sampleId, note=$note, frame=$targetFrame, tableId=$tableId, ticRate=$tableTicRate")

        backend.scheduleNoteWithTable(
            targetFrame, sampleId, trackId, frequency, baseFreq, volume, pan,
            startPointOverride, tableId, tableTicRate, note.octave, note.pitch,
            pslInitialOffset, pslDuration, pbnRate, vibratoSpeed, vibratoDepth,
            tableStartRow
        )
    }

    /**
     * Get the current table row for a voice on a specific track.
     * Used for UI feedback (highlighting current table row during playback).
     *
     * @param trackId Which track to query (0-7)
     * @return Current table row (0-15), or -1 if no active voice or no table
     */
    fun getVoiceTableRow(trackId: Int): Int {
        return backend.getVoiceTableRow(trackId)
    }

    /**
     * Get the table ID being used by a voice on a specific track.
     *
     * @param trackId Which track to query (0-7)
     * @return Table ID (0-255), or -1 if no active voice or no table
     */
    fun getVoiceTableId(trackId: Int): Int {
        return backend.getVoiceTableId(trackId)
    }

    /**
     * Set table row for a voice (THO effect from phrase on empty step).
     * Jumps the currently playing voice's table to the specified row.
     */
    fun setVoiceTableRow(trackId: Int, row: Int) {
        backend.setVoiceTableRow(trackId, row)
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // PITCH MODULATION (Phase 7)
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Set pitch slide for a voice (PSL effect).
     *
     * @param trackId Which track (0-7)
     * @param targetSemitones Target pitch offset in semitones
     * @param durationTicks Duration in ticks
     * @param tempo Current tempo in BPM
     */
    fun setPitchSlide(trackId: Int, targetSemitones: Float, durationTicks: Float, tempo: Int) {
        backend.setPitchSlide(trackId, targetSemitones, durationTicks, tempo)
    }

    /**
     * Set continuous pitch bend for a voice (PBN effect).
     *
     * @param trackId Which track (0-7)
     * @param semitonesPerTick Rate of pitch change per tick
     * @param tempo Current tempo in BPM
     */
    fun setPitchBend(trackId: Int, semitonesPerTick: Float, tempo: Int) {
        backend.setPitchBend(trackId, semitonesPerTick, tempo)
    }

    /**
     * Set vibrato for a voice (PVB/PVX effect).
     *
     * @param trackId Which track (0-7)
     * @param speed LFO frequency in Hz
     * @param depth Modulation depth in semitones
     */
    fun setVibrato(trackId: Int, speed: Float, depth: Float) {
        backend.setVibrato(trackId, speed, depth)
    }

    /**
     * Clear all pitch modulation for a voice.
     *
     * @param trackId Which track (0-7)
     */
    fun clearPitchMod(trackId: Int) {
        backend.clearPitchMod(trackId)
    }

    /**
     * Set initial pitch offset for a voice (used by PSL portamento effect).
     *
     * @param trackId Which track (0-7)
     * @param semitones Pitch offset in semitones
     */
    fun setInitialPitchOffset(trackId: Int, semitones: Float) {
        backend.setInitialPitchOffset(trackId, semitones)
    }

    /**
     * Trigger note-off for ADSR/TRIG modulators on a track.
     * Transitions sustain (stage 3) → release (stage 4), allowing a smooth fade-out.
     * Called on KILL effect so looped samples with ADSR mod fade rather than cut hard.
     *
     * @param trackId Which track (0-7)
     */
    fun triggerNoteOff(trackId: Int) {
        backend.triggerNoteOff(trackId)
    }

    /**
     * Schedule a note-off event at a specific audio frame.
     * Triggers ADSR release (sustain → release) at sample-accurate timing.
     * Used to implement step-end note-off for ADSR envelopes.
     *
     * @param frame When to trigger note-off (absolute audio frame)
     * @param trackId Which track (0-7)
     */
    fun scheduleNoteOff(frame: Long, trackId: Int) {
        backend.scheduleNoteOff(frame, trackId)
    }

    /**
     * Push instrument modulation slots to the C++ engine before scheduling a note.
     *
     * Converts AHD tick values → audio samples using current tempo + sample rate,
     * then calls setInstrumentModulation for each active slot.
     */
    fun pushInstrumentModulation(instrument: Instrument, tempo: Int) {
        val sampleId = instrument.sampleId
        val sampleRate = getDeviceSampleRate().toFloat()

        // Frames per tic: at 120 BPM, 4 steps/beat, 12 tics/step → ~230 samples/tic
        val beatsPerSecond = tempo / 60.0f
        val stepsPerBeat = 4.0f
        val ticsPerStep = 12.0f
        val framesPerTic = sampleRate / (beatsPerSecond * stepsPerBeat * ticsPerStep)

        var anyActive = false
        for (slotIndex in 0..3) {
            val slot = instrument.modSlots[slotIndex]

            val dest = when (slot.dest) {
                ModDest.VOLUME        -> 1
                ModDest.PAN           -> 2
                ModDest.PITCH         -> 3
                ModDest.FINE_PITCH    -> 4
                ModDest.FILTER_CUTOFF -> 5
                ModDest.FILTER_RES    -> 6
                ModDest.SAMPLE_START  -> 7
                ModDest.MOD_AMT       -> 8   // Routes this slot's output to scale next slot's amount
                ModDest.MOD_RATE      -> 9   // Routes this slot's output to scale next slot's time/freq
                ModDest.MOD_BOTH      -> 10  // Routes to both amount and rate of next slot
                else                  -> 0
            }

            when (slot.type) {
                ModType.AHD -> {
                    if (dest == 0) { backend.setInstrumentModulation(sampleId, slotIndex, 0,0,0f,0,0,0); continue }
                    val amount       = slot.amount / 255.0f
                    val attackSamples  = (slot.attack * framesPerTic).toInt().coerceAtLeast(0)
                    val holdSamples    = (slot.hold   * framesPerTic).toInt().coerceAtLeast(0)
                    val decaySamples   = (slot.decay  * framesPerTic).toInt().coerceAtLeast(0)
                    backend.setInstrumentModulation(sampleId, slotIndex, 1, dest, amount,
                        attackSamples, holdSamples, decaySamples)
                    anyActive = true
                }
                ModType.ADSR -> {
                    if (dest == 0) { backend.setInstrumentModulation(sampleId, slotIndex, 0,0,0f,0,0,0); continue }
                    val amount         = slot.amount  / 255.0f
                    val sustainLevel   = slot.sustain / 255.0f
                    val attackSamples  = (slot.attack   * framesPerTic).toInt().coerceAtLeast(0)
                    val decaySamples   = (slot.decay    * framesPerTic).toInt().coerceAtLeast(0)
                    val releaseSamples = (slot.release  * framesPerTic).toInt().coerceAtLeast(0)
                    backend.setInstrumentModulation(sampleId, slotIndex, 2, dest, amount,
                        attackSamples, 0, decaySamples, sustainLevel,
                        releaseSamples = releaseSamples)
                    anyActive = true
                }
                ModType.LFO -> {
                    if (dest == 0) { backend.setInstrumentModulation(sampleId, slotIndex, 0,0,0f,0,0,0); continue }
                    val amount   = slot.amount / 255.0f
                    // lfoFreq 0x00-0xFF → 0.1 to 20 Hz (linear)
                    val lfoHz    = (slot.lfoFreq + 1) * 20.0f / 256.0f
                    val oscShape = slot.oscShape
                    backend.setInstrumentModulation(sampleId, slotIndex, 3, dest, amount,
                        0, 0, 0, 0.5f, lfoHz, oscShape)
                    anyActive = true
                }
                ModType.DRUM -> {
                    // DRUM = AHD semantics: ATK=spike attack, HOLD=body, DEC=tail
                    // Passes type=4 so C++ can distinguish for future per-type behaviour
                    if (dest == 0) { backend.setInstrumentModulation(sampleId, slotIndex, 0,0,0f,0,0,0); continue }
                    val amount        = slot.amount / 255.0f
                    val attackSamples = (slot.attack * framesPerTic).toInt().coerceAtLeast(0)
                    val holdSamples   = (slot.hold   * framesPerTic).toInt().coerceAtLeast(0)
                    val decaySamples  = (slot.decay  * framesPerTic).toInt().coerceAtLeast(0)
                    backend.setInstrumentModulation(sampleId, slotIndex, 4, dest, amount,
                        attackSamples, holdSamples, decaySamples)
                    anyActive = true
                }
                ModType.TRIG -> {
                    // TRIG = ADSR semantics (future: externally triggered); passes type=5
                    if (dest == 0) { backend.setInstrumentModulation(sampleId, slotIndex, 0,0,0f,0,0,0); continue }
                    val amount         = slot.amount  / 255.0f
                    val sustainLevel   = slot.sustain / 255.0f
                    val attackSamples  = (slot.attack   * framesPerTic).toInt().coerceAtLeast(0)
                    val decaySamples   = (slot.decay    * framesPerTic).toInt().coerceAtLeast(0)
                    val releaseSamples = (slot.release  * framesPerTic).toInt().coerceAtLeast(0)
                    backend.setInstrumentModulation(sampleId, slotIndex, 5, dest, amount,
                        attackSamples, 0, decaySamples, sustainLevel,
                        releaseSamples = releaseSamples)
                    anyActive = true
                }
                else -> {
                    // NONE or future types — clear this slot
                    backend.setInstrumentModulation(sampleId, slotIndex, 0, 0, 0f, 0, 0, 0)
                }
            }
        }
        if (!anyActive) {
            backend.clearInstrumentModulation(sampleId)
        }
    }

    /**
     * Close audio engine and release resources.
     */
    fun close() {
        backend.close()
    }
}
