package com.example.pockettracker.core.audio

import com.example.pockettracker.core.data.Instrument
import com.example.pockettracker.core.data.Note
import com.example.pockettracker.core.data.Project
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
    private val backend: IAudioBackend,
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
                vol = 1.0f
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
     */
    fun previewInstrument(instrument: Instrument) {
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
        backend.scheduleNote(
            frame = targetFrame,
            sampleId = sampleId,
            trackId = 0,
            freq = targetFreq,
            baseFreq = compensatedBaseFreq,
            vol = 1.0f
        )

        logger.d(TAG, "🔊 Preview instrument ${instrument.id.toString(16).padStart(2,'0').uppercase()}: freq=$targetFreq Hz")
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
    fun playNote(note: Note, instrumentId: Int, trackId: Int, volume: Float = 1.0f, project: Project? = null) {
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
            vol = volume
        )
    }

    /**
     * Update waveform buffer from audio output.
     */
    fun updateWaveform() {
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
     */
    fun scheduleNote(
        targetFrame: Long,
        note: Note,
        instrumentId: Int,
        trackId: Int,
        volume: Float = 1.0f,
        project: Project,
        startPointOverride: Int = -1  // -1 = use instrument default, 0-255 = Offset effect override
    ) {
        if (note == Note.EMPTY) return

        val sampleId = if (instrumentId in 0..255) {
            project.instruments[instrumentId].sampleId
        } else {
            android.util.Log.w("AudioEngine", "❌ Invalid instrumentId=$instrumentId, skipping note")
            return
        }

        // Debug: Log what we're scheduling
        android.util.Log.d("AudioEngine", "📋 scheduleNote: inst=$instrumentId → sampleId=$sampleId, note=$note, frame=$targetFrame")

        val baseFreq = sampleBaseFrequencies[sampleId] ?: 261.63f
        val frequency = note.toFrequency()

        // Resume stream so audio callback can process the queue
        backend.resumeStream()

        backend.scheduleNote(targetFrame, sampleId, trackId, frequency, baseFreq, volume, startPointOverride)
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

    /**
     * Close audio engine and release resources.
     */
    fun close() {
        backend.close()
    }
}
