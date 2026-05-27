package com.conanizer.pockettracker.core.audio

import com.conanizer.pockettracker.core.data.Instrument
import com.conanizer.pockettracker.core.data.InstrumentType
import com.conanizer.pockettracker.core.data.ModDest
import com.conanizer.pockettracker.core.data.ModType
import com.conanizer.pockettracker.core.data.Note
import com.conanizer.pockettracker.core.data.Project
import com.conanizer.pockettracker.core.data.Table
import com.conanizer.pockettracker.core.data.VolumeUtils
import com.conanizer.pockettracker.core.logging.ILogger
import com.conanizer.pockettracker.core.logic.PlaybackController
import com.conanizer.pockettracker.core.resources.IResourceLoader
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

class AudioEngine(
    internal val backend: IAudioBackend,
    private val resourceLoader: IResourceLoader,
    private val logger: ILogger
) {
    private val TAG = "AudioEngine"

    // Waveform buffer for visualization (620 samples for 620px width oscilloscope)
    val waveformBuffer = FloatArray(620) { 0f }

    // Per-track waveform buffers for OCTA visualizer
    private val trackWaveformBufferFlat = FloatArray(8 * 620)
    private val activeTrackFlags = BooleanArray(8)
    val trackWaveformBuffers: Array<FloatArray> = Array(8) { FloatArray(620) }

    // Spectrum buffer for SPECTRUM/SPECTRUM_PEAKS visualizer (40 log-spaced bins, 0-1)
    val spectrumBuffer = FloatArray(40)

    // Bitmask of tracks that had notes scheduled in the current phrase.
    // Set per-track when a note is scheduled; cleared on clearScheduledNotes().
    // Used by OCTA visualizer so the layout stays stable for the full phrase duration.
    var phraseTrackMask: Int = 0
        private set

    // Sample metadata (base frequencies and sample rate compensation ratios)
    private val sampleBaseFrequencies = mutableMapOf<Int, Float>()
    private val sampleRateRatios = mutableMapOf<Int, Float>()
    // Original ratios cached for non-destructive RATE mode (cleared when new sample is loaded)
    private val originalSampleRateRatios = mutableMapOf<Int, Float>()

    // Injected by PlaybackController so scheduleNote() can route SF instruments without Android dependency.
    var sfSlotProvider: ((soundfontPath: String) -> Int?)? = null

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

    private fun loadAllSamples() {
        // No default samples loaded - users load their own via file browser
        logger.d(TAG, "✅ Audio engine initialized (no default samples)")
    }

    fun getDeviceSampleRate(): Int = backend.getSampleRate()

    fun loadSampleFromFile(instrumentId: Int, filePath: String): Boolean {
        try {
            val file = File(filePath)
            if (!file.exists() || !file.canRead()) {
                logger.e(TAG, "Cannot read file: $filePath")
                return false
            }

            val (left, right, adjustedBaseFreq) = loadWavFileFromPath(filePath)
            if (right != null) {
                backend.loadSampleStereo(instrumentId, left, right)
            } else {
                backend.loadSample(instrumentId, left)
            }

            sampleBaseFrequencies[instrumentId] = adjustedBaseFreq
            sampleRateRatios[instrumentId] = adjustedBaseFreq / 261.63f
            originalSampleRateRatios.remove(instrumentId)

            logger.d(TAG, "✅ Loaded sample: instrumentId=$instrumentId, length=${left.size}, stereo=${right != null}, baseFreq=$adjustedBaseFreq, path=$filePath")
            return true
        } catch (e: Exception) {
            logger.e(TAG, "❌ Error loading sample from file: ${e.message}")
            return false
        }
    }

    private fun loadWavFileFromPath(filePath: String): Triple<FloatArray, FloatArray?, Float> {
        File(filePath).inputStream().use { inputStream ->
            val fileSize = inputStream.available()
            val buffer = ByteArray(fileSize)
            inputStream.read(buffer)

            return parseWavBuffer(buffer)
        }
    }

    private fun parseWavBuffer(buffer: ByteArray): Triple<FloatArray, FloatArray?, Float> {
        if (buffer.size < 12) throw IllegalArgumentException("WAV buffer too small: ${buffer.size}")

        fun readShortAt(pos: Int) = ByteBuffer.wrap(buffer, pos, 2).order(ByteOrder.LITTLE_ENDIAN).short.toInt()
        fun readIntAt(pos: Int)   = ByteBuffer.wrap(buffer, pos, 4).order(ByteOrder.LITTLE_ENDIAN).int

        // Scan all chunks from the start — do NOT assume fmt is at a fixed offset.
        // Some files have a JUNK/bext/RF64 chunk before "fmt ", which shifts all byte positions.
        var scanPos = 12  // Skip RIFF + WAVE header (12 bytes)
        var fmtStart  = -1
        var dataStart = -1
        var dataSize  = -1
        while (scanPos + 8 <= buffer.size) {
            val chunkId   = String(buffer, scanPos, 4, Charsets.US_ASCII)
            val chunkSize = readIntAt(scanPos + 4)
            when (chunkId) {
                "fmt " -> fmtStart = scanPos + 8  // record fmt data position
                "data" -> {
                    dataStart = scanPos + 8
                    dataSize  = chunkSize.coerceAtMost(buffer.size - dataStart)
                }
            }
            if (fmtStart >= 0 && dataStart >= 0) break
            scanPos += 8 + chunkSize + (chunkSize and 1)  // pad to even boundary per WAV spec
        }
        if (fmtStart  < 0) throw IllegalArgumentException("No 'fmt ' chunk found in WAV file")
        if (dataStart < 0) throw IllegalArgumentException("No 'data' chunk found in WAV file")

        // Read fmt fields at their correct relative offsets inside the fmt chunk data
        val audioFormat   = readShortAt(fmtStart + 0) and 0xFFFF  // 1=PCM, 3=IEEE float, 65534=extensible
        val channels      = readShortAt(fmtStart + 2)
        val sampleRate    = readIntAt  (fmtStart + 4)
        val bitsPerSample = readShortAt(fmtStart + 14)

        // WAVE_FORMAT_EXTENSIBLE (0xFFFE=65534): actual encoding is in the sub-format GUID.
        // The GUID starts 24 bytes into the fmt data; its first 2 bytes are the real format code
        // (1=PCM, 3=IEEE float) — identical to the standard audioFormat field.
        val effectiveFormat = if (audioFormat == 65534 && fmtStart + 26 <= buffer.size) {
            readShortAt(fmtStart + 24) and 0xFFFF
        } else {
            audioFormat
        }

        logger.d(TAG, "WAV: format=$audioFormat effective=$effectiveFormat channels=$channels sampleRate=$sampleRate bits=$bitsPerSample")

        if (bitsPerSample == 0) throw IllegalArgumentException("WAV bitsPerSample is 0 — corrupt header?")
        if (sampleRate    == 0) throw IllegalArgumentException("WAV sampleRate is 0 — corrupt header?")

        val bytesPerSample = bitsPerSample / 8
        val totalSamples   = dataSize / bytesPerSample
        val rawSamples     = FloatArray(totalSamples)

        when {
            effectiveFormat == 3 && bitsPerSample == 32 -> {
                // IEEE 32-bit float — read directly
                val fb = ByteBuffer.wrap(buffer, dataStart, totalSamples * 4)
                    .order(ByteOrder.LITTLE_ENDIAN).asFloatBuffer()
                for (i in rawSamples.indices) rawSamples[i] = fb.get(i)
            }
            effectiveFormat == 1 && bitsPerSample == 16 -> {
                val sb = ByteBuffer.wrap(buffer, dataStart, totalSamples * 2)
                    .order(ByteOrder.LITTLE_ENDIAN).asShortBuffer()
                for (i in rawSamples.indices) rawSamples[i] = sb.get(i) / 32768f
            }
            effectiveFormat == 1 && bitsPerSample == 24 -> {
                // 24-bit PCM: 3 bytes per sample, little-endian signed
                for (i in rawSamples.indices) {
                    val bytePos = dataStart + i * 3
                    val b0 = buffer[bytePos].toInt() and 0xFF
                    val b1 = buffer[bytePos + 1].toInt() and 0xFF
                    val b2 = buffer[bytePos + 2].toInt()  // signed — sign-extends into 32 bits
                    rawSamples[i] = ((b2 shl 16) or (b1 shl 8) or b0) / 8388608f  // 2^23
                }
            }
            effectiveFormat == 1 && bitsPerSample == 32 -> {
                // 32-bit PCM integer
                val ib = ByteBuffer.wrap(buffer, dataStart, totalSamples * 4)
                    .order(ByteOrder.LITTLE_ENDIAN).asIntBuffer()
                for (i in rawSamples.indices) rawSamples[i] = ib.get(i) / 2147483648f  // 2^31
            }
            else -> throw IllegalArgumentException("Unsupported WAV format=$audioFormat (effective=$effectiveFormat) bits=$bitsPerSample")
        }

        // Sample rate compensation: adjust base frequency so the engine plays at correct pitch
        val deviceSampleRate = getDeviceSampleRate()
        val adjustedBaseFreq = 261.63f * (deviceSampleRate.toFloat() / sampleRate.toFloat())

        // Stereo: keep L and R channels separate; mono: right = null
        return if (channels == 2) {
            val left  = FloatArray(rawSamples.size / 2) { i -> rawSamples[i * 2] }
            val right = FloatArray(rawSamples.size / 2) { i -> rawSamples[i * 2 + 1] }
            Triple(left, right, adjustedBaseFreq)
        } else {
            Triple(rawSamples, null, adjustedBaseFreq)
        }
    }

    fun previewSampleFile(filePath: String): Boolean {
        try {
            val file = File(filePath)
            if (!file.exists() || !file.canRead()) {
                logger.e(TAG, "Cannot read file: $filePath")
                return false
            }

            backend.stopAll()

            val (left, right, adjustedBaseFreq) = loadWavFileFromPath(filePath)
            if (right != null) {
                backend.loadSampleStereo(255, left, right)
            } else {
                backend.loadSample(255, left)
            }

            // CRITICAL: Resume stream so audio callback processes the scheduled note
            backend.resumeStream()

            val c4Freq = 261.63f
            val targetFrame = backend.getCurrentFrame() + 100
            backend.scheduleNote(
                frame = targetFrame,
                sampleId = 255,
                trackId = 0,
                freq = c4Freq,
                baseFreq = adjustedBaseFreq,
                vol = 1.0f,
                pan = 0.5f
            )

            logger.d(TAG, "🔊 Preview sample at C-4: $filePath (baseFreq=$adjustedBaseFreq)")
            return true
        } catch (e: Exception) {
            logger.e(TAG, "❌ Error previewing sample: ${e.message}")
            return false
        }
    }

    fun previewSampleData(samples: FloatArray, sampleRate: Int, samplesRight: FloatArray? = null): Boolean {
        return try {
            backend.stopAll()
            if (samplesRight != null) {
                backend.loadSampleStereo(255, samples, samplesRight)
            } else {
                backend.loadSample(255, samples)
            }
            backend.resumeStream()
            val adjustedBaseFreq = 261.63f * (getDeviceSampleRate().toFloat() / sampleRate.toFloat())
            backend.scheduleNote(
                frame = backend.getCurrentFrame() + 100,
                sampleId = 255,
                trackId = 0,
                freq = 261.63f,
                baseFreq = adjustedBaseFreq,
                vol = 1.0f,
                pan = 0.5f
            )
            true
        } catch (e: Exception) {
            logger.e(TAG, "❌ Error previewing sample data: ${e.message}")
            false
        }
    }

    fun previewInstrument(
        instrument: Instrument,
        project: Project? = null,
        tableIdOverride: Int = -1
    ) {
        // SF path: route through scheduleNote so mods, tables, and tracking work correctly.
        if (instrument.instrumentType == InstrumentType.SOUNDFONT && project != null) {
            backend.resumeStream()
            val targetFrame = backend.getCurrentFrame() + 100L
            scheduleNote(
                targetFrame = targetFrame,
                note = instrument.root,
                instrumentId = instrument.id,
                trackId = 0,
                volume = VolumeUtils.hexToFloat(instrument.volume),
                phraseVol = 1.0f,
                pan = VolumeUtils.hexToFloat(instrument.pan),
                project = project,
                tableIdOverride = tableIdOverride
            )
            // Hard-stop after 2 seconds so SF sustain notes don't ring forever during preview.
            val sr = backend.getSampleRate().toLong().coerceAtLeast(44100L)
            backend.scheduleKill(targetFrame + sr * 2, 0)
            return
        }

        val sampleId = instrument.sampleId

        val rootFreq = instrument.root.toFrequency()
        val detuneSemitones = (instrument.detune shr 4).toFloat()
        val detuneFraction = (instrument.detune and 0x0F) / 16.0f
        val totalDetuneSemitones = detuneSemitones + detuneFraction - 8.0f
        val detuneMultiplier = Math.pow(2.0, (totalDetuneSemitones / 12.0)).toFloat()
        val targetFreq = rootFreq * detuneMultiplier

        val sampleRateRatio = sampleRateRatios[sampleId] ?: 1.0f
        val compensatedBaseFreq = 261.63f * sampleRateRatio

        // CRITICAL: Resume stream so audio callback processes the scheduled note
        backend.resumeStream()

        val targetFrame = backend.getCurrentFrame() + 100

        val volume = VolumeUtils.hexToFloat(instrument.volume)
        val pan = VolumeUtils.hexToFloat(instrument.pan)

        val tableId = if (tableIdOverride >= 0) tableIdOverride else instrument.id
        val tableTicRate = instrument.tableTicRate

        // Force reload table when previewing (user may have just edited it)
        if (tableId >= 0 && project != null && tableId < 256) {
            forceReloadTable(project.tables[tableId])
        }

        // Push current modulation params, EQ, and sends so preview reflects latest UI edits
        val tempo = project?.tempo ?: 120
        pushInstrumentModulation(instrument, tempo)
        if (project != null) pushInstrumentEqAndSends(instrument, project)

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

    fun previewNoteWithTimeout(
        instrument: Instrument,
        note: Note,
        project: Project?,
        durationFrames: Long
    ) {
        if (note == Note.EMPTY) return

        if (instrument.instrumentType == InstrumentType.SOUNDFONT && project != null) {
            backend.resumeStream()
            val targetFrame = backend.getCurrentFrame() + 100L
            scheduleNote(
                targetFrame = targetFrame,
                note = note,
                instrumentId = instrument.id,
                trackId = 0,
                volume = VolumeUtils.hexToFloat(instrument.volume),
                phraseVol = 1.0f,
                pan = VolumeUtils.hexToFloat(instrument.pan),
                project = project,
                tableIdOverride = -1
            )
            backend.scheduleKill(targetFrame + durationFrames, 0)
            return
        }

        val sampleId = instrument.sampleId
        val detuneSemitones = (instrument.detune shr 4).toFloat()
        val detuneFraction = (instrument.detune and 0x0F) / 16.0f
        val totalDetuneSemitones = detuneSemitones + detuneFraction - 8.0f
        val detuneMultiplier = Math.pow(2.0, (totalDetuneSemitones / 12.0)).toFloat()
        val targetFreq = note.toFrequency() * detuneMultiplier

        val sampleRateRatio = sampleRateRatios[sampleId] ?: 1.0f
        val compensatedBaseFreq = 261.63f * sampleRateRatio

        backend.resumeStream()
        val targetFrame = backend.getCurrentFrame() + 100L

        val volume = VolumeUtils.hexToFloat(instrument.volume)
        val pan = VolumeUtils.hexToFloat(instrument.pan)
        val tableId = instrument.id
        val tableTicRate = instrument.tableTicRate

        if (project != null && tableId < 256) forceReloadTable(project.tables[tableId])

        val tempo = project?.tempo ?: 120
        pushInstrumentModulation(instrument, tempo)
        if (project != null) pushInstrumentEqAndSends(instrument, project)

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
            noteOctave = note.octave,
            notePitch = note.pitch,
            pslInitialOffset = 0f,
            pslDuration = 0f,
            pbnRate = 0f,
            vibratoSpeed = 0f,
            vibratoDepth = 0f
        )
        backend.scheduleKill(targetFrame + durationFrames, 0)
    }

    fun calculateInstrumentBaseFrequency(instrument: Instrument): Float {
        val rootFreq = instrument.root.toFrequency()
        val detuneSemitones = (instrument.detune shr 4).toFloat()
        val detuneFraction = (instrument.detune and 0x0F) / 16.0f
        val totalDetuneSemitones = detuneSemitones + detuneFraction - 8.0f
        val detuneMultiplier = Math.pow(2.0, (totalDetuneSemitones / 12.0)).toFloat()
        val sampleRateRatio = sampleRateRatios[instrument.sampleId] ?: 1.0f
        return rootFreq * detuneMultiplier * sampleRateRatio
    }

    fun updateInstrumentBaseFrequency(instrument: Instrument) {
        val baseFreq = calculateInstrumentBaseFrequency(instrument)
        sampleBaseFrequencies[instrument.sampleId] = baseFreq
        logger.d(TAG, "📝 Updated base frequency for instrument ${instrument.id}: $baseFreq Hz")
    }

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

    fun updateWaveform() {
        backend.updateWaveform(waveformBuffer)
    }

    fun decayWaveform() {
        backend.decayWaveform()
    }

    fun decayPeaks() {
        backend.decayPeaks()
    }

    fun updateWaveformWithDecay(isPlaying: Boolean) {
        if (!isPlaying) {
            backend.decayWaveform()
        }
        backend.updateWaveform(waveformBuffer)
    }

    fun updateTrackWaveforms() {
        backend.getTrackWaveforms(trackWaveformBufferFlat, activeTrackFlags)
        for (t in 0 until 8) {
            trackWaveformBufferFlat.copyInto(trackWaveformBuffers[t], 0, t * 620, (t + 1) * 620)
        }
    }

    fun getActiveTrackMask(): Int {
        var mask = 0
        for (t in 0 until 8) {
            if (activeTrackFlags[t]) mask = mask or (1 shl t)
        }
        return mask
    }

    fun updateSpectrum() {
        val result = backend.getSpectrumMagnitudes(spectrumBuffer.size)
        result.copyInto(spectrumBuffer, 0, 0, minOf(result.size, spectrumBuffer.size))
    }

    fun stopAll() {
        backend.stopAll()
        waveformBuffer.fill(0f)
        phraseTrackMask = 0
    }

    fun killTrack(trackId: Int) {
        backend.killTrack(trackId)
    }

    fun scheduleKill(frame: Long, trackId: Int) {
        backend.scheduleKill(frame, trackId)
    }

    fun getCurrentFrame(): Long = backend.getCurrentFrame()

    fun scheduleNote(
        targetFrame: Long,
        note: Note,
        instrumentId: Int,
        trackId: Int,
        volume: Float = 1.0f,
        phraseVol: Float = 1.0f,
        pan: Float = 0.5f,
        project: Project,
        startPointOverride: Int = -1,
        pslInitialOffset: Float = 0f,
        pslDuration: Float = 0f,
        pbnRate: Float = 0f,
        vibratoSpeed: Float = 0f,
        vibratoDepth: Float = 0f,
        tableIdOverride: Int = -1,
        tableStartRow: Int = -1,
        transposeSemitones: Int = 0
    ) {
        if (note == Note.EMPTY) return
        if (trackId in 0..7) phraseTrackMask = phraseTrackMask or (1 shl trackId)

        val instrument = if (instrumentId in 0..255) {
            project.instruments[instrumentId]
        } else {
            logger.w(TAG, "❌ Invalid instrumentId=$instrumentId, skipping note")
            return
        }

        // ── SoundFont path ────────────────────────────────────────────────────────
        // Handled first so arpeggio/repeat retriggers reach SF instruments too.
        if (instrument.instrumentType == InstrumentType.SOUNDFONT) {
            val path = instrument.soundfontPath ?: return
            val slot = sfSlotProvider?.invoke(path) ?: return
            val baseMidi = (note.octave + 1) * 12 + note.pitch
            val transpose = instrument.root.toMidi() - 60
            val midiNote = (baseMidi + transpose).coerceIn(0, 127)
            val velocity = (volume * 127).toInt().coerceIn(1, 127)
            // Convert tick-based pitch params to frame-based (same as sampler path)
            val tempo = project.tempo
            val sr = backend.getSampleRate().toFloat()
            val framesPerTic = sr / (tempo / 60f * 4f * 12f)
            val framesPerStep = framesPerTic * 12f
            val pslDurationFrames = if (pslDuration > 0f) pslDuration * framesPerTic else 0f
            val pbnRatePerFrame   = if (pbnRate  != 0f)  pbnRate  / framesPerStep  else 0f

            // Push mod slots, EQ, and send levels to C++ so SF voice picks them up at trigger.
            pushInstrumentModulation(instrument, tempo)
            pushInstrumentEqAndSends(instrument, project)
            // Apply envelope overrides every trigger so TSF preset has correct ATK/DEC/SUS/REL
            // before the note plays. Without this, KIL → noteOff uses the SF2 file's native
            // (often instant) release instead of the user-configured REL value.
            applySoundfontEnvelopeOverrides(instrument)
            // Reset instrumentParams[sfId] to drive=0 + correct filter before every trigger.
            // Without this, stale WAV drive/filter values from a previous render or project load
            // persist in the C++ instrumentParams array and get applied to SF voice output,
            // causing distortion or silence depending on what was left behind.
            applySoundfontFilterOverrides(instrument)

            // Table setup — same logic as sampler path
            val sfTableId = if (tableIdOverride >= 0) tableIdOverride else instrumentId
            val sfTicRate = instrument.tableTicRate
            if (sfTableId in 0..255) {
                ensureTableLoaded(project.tables[sfTableId])
            }

            backend.resumeStream()
            backend.scheduleSoundfontNote(
                targetFrame, trackId, slot,
                midiNote, velocity, volume, pan,
                instrument.sfBank, instrument.sfPreset,
                pslInitialOffset, pslDurationFrames, pbnRatePerFrame, vibratoSpeed, vibratoDepth,
                phraseVol = phraseVol,
                sampleId = instrumentId,
                tableId = sfTableId,
                tableTicRate = sfTicRate,
                noteOctave = note.octave,
                notePitch = note.pitch,
                tableStartRow = tableStartRow
            )
            return
        }
        // ── Sampler path ──────────────────────────────────────────────────────────

        // Skip if instrument has no sample loaded — sampleFilePath == null means empty slot.
        // This prevents stale C++ sample data from playing when an instrument looks empty in the UI.
        if (instrument.sampleFilePath == null) return

        val sampleId = instrument.sampleId

        val tableId = if (tableIdOverride >= 0) tableIdOverride else instrumentId
        val tableTicRate = instrument.tableTicRate

        if (tableId in 0..255) {
            ensureTableLoaded(project.tables[tableId])
        }

        logger.d(TAG, "📋 scheduleNote: inst=$instrumentId → sampleId=$sampleId, note=$note, frame=$targetFrame, vol=${"%.4f".format(volume)}, pan=$pan, tableId=$tableId")

        val baseFreq = sampleBaseFrequencies[sampleId] ?: 261.63f

        // Handle slice playback (CUT/TRU mode): note selects slice, pitch comes from ROOT + transpose.
        // Slice index uses the raw phrase note (before transpose) so ROOT and chain/master transpose
        // only affect playback pitch, not which slice gets triggered.
        var effectiveNote = note
        var effectiveStartOverride = startPointOverride
        var effectiveEndOverride = -1
        if (instrument.slicingMode != 0 && instrument.sliceMarkers.isNotEmpty()) {
            val markers = instrument.sliceMarkers
            // Raw phrase note (undo transpose baked in by PlaybackController)
            val rawNoteMidi = note.toMidi() - transposeSemitones
            // N markers define N+1 slices: C-0=slice0, C#0=slice1, etc.
            val sliceIndex = rawNoteMidi.coerceAtLeast(0).coerceAtMost(markers.size)
            val totalFrames = backend.getSampleLength(sampleId).toLong()
            if (totalFrames > 0) {
                val sliceStart = if (sliceIndex == 0) 0L else markers[sliceIndex - 1]
                effectiveStartOverride = ((sliceStart * 255L) / totalFrames).toInt().coerceIn(0, 255)
                // Pitch = ROOT + chain/master transpose (phrase note has no effect on pitch)
                effectiveNote = Note.fromMidi((instrument.root.toMidi() + transposeSemitones).coerceIn(0, 127))
                if (instrument.slicingMode == 1) { // CUT: stop at next slice boundary
                    val sliceEnd = if (sliceIndex < markers.size) markers[sliceIndex] else totalFrames
                    effectiveEndOverride = ((sliceEnd * 255L) / totalFrames).toInt().coerceIn(0, 255)
                }
                // TRU: effectiveEndOverride stays -1 → plays to instrument's natural sampleEnd
            }
        }

        val frequency = effectiveNote.toFrequency()

        // Push modulation params, EQ, and send levels to C++ engine.
        // Must be done before scheduleNoteWithTable so the engine has correct params at trigger time.
        val tempo = project.tempo
        pushInstrumentModulation(instrument, tempo)
        pushInstrumentEqAndSends(instrument, project)

        // Convert tick-based pitch effect params to frame-based so C++ needs no tempo knowledge.
        // framesPerTic = sampleRate / (tempo/60 * 4 steps/beat * 12 tics/step)
        val sampleRate = backend.getSampleRate().toFloat()
        val framesPerTic = sampleRate / (tempo / 60f * 4f * 12f)
        val framesPerStep = framesPerTic * 12f  // TICS_PER_STEP = 12
        val pslDurationFrames = if (pslDuration > 0f) pslDuration * framesPerTic else 0f
        // pbnRate is semitones/step (per EffectProcessor docs: "PBN 10 = 1 semitone per step")
        val pbnRatePerFrame  = if (pbnRate  != 0f)  pbnRate  / framesPerStep  else 0f

        // Resume stream so audio callback can process the queue
        backend.resumeStream()

        // Always use scheduleNoteWithTable - C++ handles tableId=-1 as "no table"
        backend.scheduleNoteWithTable(
            targetFrame, sampleId, trackId, frequency, baseFreq, volume, phraseVol, pan,
            effectiveStartOverride, effectiveEndOverride, tableId, tableTicRate, effectiveNote.octave, effectiveNote.pitch,
            pslInitialOffset, pslDurationFrames, pbnRatePerFrame, vibratoSpeed, vibratoDepth,
            tableStartRow
        )
    }

    fun clearScheduledNotes() {
        backend.clearScheduledNotes()
        phraseTrackMask = 0
    }

    fun clearScheduledNotesFrom(fromFrame: Long) {
        backend.clearScheduledNotesFrom(fromFrame)
    }

    fun clearAllSamples() {
        sampleBaseFrequencies.clear()
        sampleRateRatios.clear()
        originalSampleRateRatios.clear()
        backend.clearAllSamples()
    }

    fun getSampleLength(instrumentId: Int): Int = backend.getSampleLength(instrumentId)
    fun getSampleWaveform(instrumentId: Int, numBins: Int): FloatArray = backend.getSampleWaveform(instrumentId, numBins)
    fun getSampleWaveformRange(instrumentId: Int, startFrame: Int, endFrame: Int, numBins: Int): FloatArray = backend.getSampleWaveformRange(instrumentId, startFrame, endFrame, numBins)
    fun getSampleData(instrumentId: Int): FloatArray = backend.getSampleData(instrumentId)

    fun getSpectrumMagnitudes(numBins: Int): FloatArray = backend.getSpectrumMagnitudes(numBins)

    fun hasStereoData(instrumentId: Int): Boolean = backend.hasStereoData(instrumentId)
    fun getSampleDataRight(instrumentId: Int): FloatArray = backend.getSampleDataRight(instrumentId)
    /** channel: 0=left, 1=right, 2=averaged */
    fun getSampleWaveformRangeSource(instrumentId: Int, startFrame: Int, endFrame: Int, numBins: Int, channel: Int): FloatArray =
        backend.getSampleWaveformRangeSource(instrumentId, startFrame, endFrame, numBins, channel)

    /**
     * @param instId Instrument/sample ID to prepare from
     * @param sourceMode 0=LEFT, 1=RIGHT, 2=STEREO, 3=MONO
     * @return instId if no special handling needed, or 254 for the temp mono slot
     */
    fun prepareSampleEditorSourcePreview(instId: Int, sourceMode: Int): Int {
        if (!hasStereoData(instId) || sourceMode == 2) return instId
        val left = getSampleData(instId)
        val data = when (sourceMode) {
            0    -> left
            1    -> getSampleDataRight(instId)
            else -> {  // MONO (3): average L+R
                val right = getSampleDataRight(instId)
                FloatArray(left.size) { i -> (left[i] + right[i]) / 2f }
            }
        }
        backend.loadSample(254, data)
        sampleRateRatios[instId]?.let { sampleRateRatios[254] = it }
        return 254
    }
    fun normalizeSample(instrumentId: Int, startFrame: Int, endFrame: Int) = backend.normalizeSample(instrumentId, startFrame, endFrame)
    fun fadeInSample(instrumentId: Int, startFrame: Int, endFrame: Int) = backend.fadeInSample(instrumentId, startFrame, endFrame)
    fun fadeOutSample(instrumentId: Int, startFrame: Int, endFrame: Int) = backend.fadeOutSample(instrumentId, startFrame, endFrame)
    fun silenceRegion(instrumentId: Int, startFrame: Int, endFrame: Int) = backend.silenceRegion(instrumentId, startFrame, endFrame)
    fun reverseSample(instrumentId: Int, startFrame: Int, endFrame: Int) = backend.reverseSample(instrumentId, startFrame, endFrame)
    fun backupSample(instrumentId: Int) = backend.backupSample(instrumentId)
    fun undoSample(instrumentId: Int) = backend.undoSample(instrumentId)
    fun saveFxPreviewBackup(instrumentId: Int) = backend.saveFxPreviewBackup(instrumentId)
    fun restoreFxPreviewBackup() = backend.restoreFxPreviewBackup()
    fun getSamplePlaybackPosition(instrumentId: Int): Float = backend.getSamplePlaybackPosition(instrumentId)
    fun cropSample(instrumentId: Int, startFrame: Int, endFrame: Int) = backend.cropSample(instrumentId, startFrame, endFrame)
    fun deleteSampleRegion(instrumentId: Int, startFrame: Int, endFrame: Int) = backend.deleteSampleRegion(instrumentId, startFrame, endFrame)
    fun copyRegion(instrumentId: Int, startFrame: Int, endFrame: Int) = backend.copyRegion(instrumentId, startFrame, endFrame)
    fun pasteRegion(instrumentId: Int, insertAt: Int) = backend.pasteRegion(instrumentId, insertAt)
    fun getClipboardLength(): Int = backend.getClipboardLength()
    fun downsampleSample(instrumentId: Int, factor: Int) {
        backend.downsampleSample(instrumentId, factor)
        sampleRateRatios[instrumentId]?.let { sampleRateRatios[instrumentId] = it * factor }
    }

    fun applyRateMode(instrumentId: Int, factor: Int) {
        if (factor <= 1) {
            // Restore HIGH: reset ratio to original and discard cache.
            originalSampleRateRatios.remove(instrumentId)?.let { origRatio ->
                sampleRateRatios[instrumentId] = origRatio
            }
        } else {
            // Cache original ratio on first departure from HIGH.
            if (!originalSampleRateRatios.containsKey(instrumentId)) {
                sampleRateRatios[instrumentId]?.let { originalSampleRateRatios[instrumentId] = it }
            }
            // Set ratio relative to original so pitch stays correct at any rate.
            originalSampleRateRatios[instrumentId]?.let { origRatio ->
                sampleRateRatios[instrumentId] = origRatio * factor
            }
        }
        backend.applyRateMode(instrumentId, factor)
    }

    fun pitchShiftSample(instrumentId: Int, semitones: Int) {
        // Pitch shift clears the RATE cache in C++, so the shifted buffer becomes the new "original".
        // Clear originalSampleRateRatios too so RATE mode ratios are not based on stale state.
        originalSampleRateRatios.remove(instrumentId)
        backend.pitchShiftSample(instrumentId, semitones.toFloat())
    }

    fun timeStretchSample(instrumentId: Int, ratio: Float) {
        originalSampleRateRatios.remove(instrumentId)
        backend.timeStretchSample(instrumentId, ratio)
    }

    fun applySampleFx(instrumentId: Int, fxType: Int, fxValue: Int, sampleRate: Float, limiterPreGain: Int = 0) {
        backend.applySampleFx(instrumentId, fxType, fxValue, sampleRate, limiterPreGain)
    }

    fun findZeroCrossing(instrumentId: Int, frame: Int): Int = backend.findZeroCrossing(instrumentId, frame)
    fun detectTransients(instrumentId: Int, sensitivity: Int): IntArray = backend.detectTransients(instrumentId, sensitivity)

    fun getOriginalSampleRate(instrumentId: Int): Int {
        val ratio = sampleRateRatios[instrumentId] ?: return 44100
        val deviceRate = getDeviceSampleRate()
        return (deviceRate / ratio).toInt().coerceAtLeast(8000)
    }

    fun getActiveTrackNotes(): List<Note> {
        val encoded = backend.getTrackActiveNotes()  // int[8], -1 or octave*12+pitch
        return encoded.map { enc ->
            if (enc < 0) Note.EMPTY else Note(pitch = enc % 12, octave = enc / 12)
        }
    }

    fun calculateTargetFrame(startFrame: Long, stepNumber: Int, tempo: Int): Long {
        val sampleRate = getDeviceSampleRate()
        val msPerStep = (60000.0 / tempo / 4.0)
        val framesPerStep = (msPerStep * sampleRate / 1000.0).toLong()
        return startFrame + (stepNumber * framesPerStep)
    }

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

    fun applySoundfontEnvelopeOverrides(instrument: Instrument) {
        val path = instrument.soundfontPath ?: return
        val slot = sfSlotProvider?.invoke(path) ?: return  // sfSlot (0-7), not instrument index
        val ov = instrument.sfOverrides
        backend.setSoundfontEnvelopeOverrides(slot, instrument.sfBank, instrument.sfPreset,
            ov.ampAttack, ov.ampDecay, ov.ampSustain, ov.ampRelease)
    }

    fun applySoundfontFilterOverrides(instrument: Instrument) {
        updateInstrumentPlaybackParams(instrument)
    }

    private val loadedTables = mutableSetOf<Int>()

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

        val firstRow = table.rows[0]
        logger.d(TAG, "📋 Loading table ${table.id}: row0=[transpose=${firstRow.transpose}, vol=${firstRow.volume}]")

        backend.loadTable(table.id, rowData)
        loadedTables.add(table.id)
        logger.d(TAG, "📋 Loaded table ${table.id} to native layer")
    }

    fun ensureTableLoaded(table: Table) {
        if (table.id !in loadedTables) {
            loadTable(table)
        }
    }

    fun forceReloadTable(table: Table) {
        loadedTables.remove(table.id)
        loadTable(table)
    }

    fun invalidateTable(tableId: Int) {
        loadedTables.remove(tableId)
        logger.d(TAG, "🔄 Invalidated table $tableId cache")
    }

    fun clearLoadedTables() {
        loadedTables.clear()
        logger.d(TAG, "🗑️ Cleared loaded tables tracking")
    }

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
        if (trackId in 0..7) phraseTrackMask = phraseTrackMask or (1 shl trackId)

        val instrument = if (instrumentId in 0..255) {
            project.instruments[instrumentId]
        } else {
            logger.w(TAG, "❌ Invalid instrumentId=$instrumentId, skipping note")
            return
        }

        val sampleId = instrument.sampleId
        val tableId = if (tableIdOverride >= 0) tableIdOverride else instrumentId
        val tableTicRate = instrument.tableTicRate

        if (tableId >= 0 && tableId < 256) {
            ensureTableLoaded(project.tables[tableId])
        }

        val baseFreq = sampleBaseFrequencies[sampleId] ?: 261.63f
        val frequency = note.toFrequency()

        // Convert tick-based pitch params to frame-based (same as scheduleNote)
        val tempo = project.tempo
        val sr = backend.getSampleRate().toFloat()
        val framesPerTic = sr / (tempo / 60f * 4f * 12f)
        val framesPerStep = framesPerTic * 12f
        val pslDurationFrames = if (pslDuration > 0f) pslDuration * framesPerTic else 0f
        val pbnRatePerFrame   = if (pbnRate  != 0f)  pbnRate  / framesPerStep  else 0f

        // Resume stream so audio callback can process the queue
        backend.resumeStream()

        logger.d(TAG, "📋 scheduleNoteWithTable: inst=$instrumentId → sampleId=$sampleId, note=$note, frame=$targetFrame, tableId=$tableId, ticRate=$tableTicRate")

        backend.scheduleNoteWithTable(
            targetFrame, sampleId, trackId, frequency, baseFreq, volume, 1.0f, pan,
            startPointOverride, -1, tableId, tableTicRate, note.octave, note.pitch,
            pslInitialOffset, pslDurationFrames, pbnRatePerFrame, vibratoSpeed, vibratoDepth,
            tableStartRow
        )
    }

    fun getVoiceTableRow(trackId: Int): Int = backend.getVoiceTableRow(trackId)

    fun getVoiceTableId(trackId: Int): Int = backend.getVoiceTableId(trackId)

    fun setVoiceTableRow(trackId: Int, row: Int) {
        backend.setVoiceTableRow(trackId, row)
    }

    // Uses sample-accurate queue so the Vxx change fires at the correct step boundary.
    fun scheduleTrackPhraseVol(targetFrame: Long, trackId: Int, phraseVol: Float) {
        backend.scheduleTrackPhraseVol(targetFrame, trackId, phraseVol)
    }

    fun setPitchSlide(trackId: Int, targetSemitones: Float, durationTicks: Float, tempo: Int) {
        backend.setPitchSlide(trackId, targetSemitones, durationTicks, tempo)
    }

    fun setPitchBend(trackId: Int, semitonesPerTick: Float, tempo: Int) {
        backend.setPitchBend(trackId, semitonesPerTick, tempo)
    }

    fun setVibrato(trackId: Int, speed: Float, depth: Float) {
        backend.setVibrato(trackId, speed, depth)
    }

    fun clearPitchMod(trackId: Int) {
        backend.clearPitchMod(trackId)
    }

    fun setInitialPitchOffset(trackId: Int, semitones: Float) {
        backend.setInitialPitchOffset(trackId, semitones)
    }

    // KILL effect: triggers ADSR release (sustain → release) so looped samples fade rather than cut hard.
    fun triggerNoteOff(trackId: Int) {
        backend.triggerNoteOff(trackId)
    }

    fun scheduleNoteOff(frame: Long, trackId: Int) {
        backend.scheduleNoteOff(frame, trackId)
    }

    fun pushInstrumentModulation(instrument: Instrument, tempo: Int) {
        val sampleId = instrument.sampleId
        val sampleRate = getDeviceSampleRate().toFloat()

        // Frames per tic: at 120 BPM, 4 steps/beat, 12 tics/step → ~230 samples/tic
        val beatsPerSecond = tempo / 60.0f
        val stepsPerBeat = 4.0f
        val ticsPerStep = PlaybackController.TICS_PER_STEP.toFloat()
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
                ModType.SCALAR -> {
                    if (dest == 0) { backend.setInstrumentModulation(sampleId, slotIndex, 0,0,0f,0,0,0); continue }
                    // amount (0x00-0xFF) is the fixed output value; no time params needed
                    val amount = slot.amount / 255.0f
                    backend.setInstrumentModulation(sampleId, slotIndex, 6, dest, amount,
                        0, 0, 0)
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

    fun pushInstrumentEqAndSends(instrument: Instrument, project: com.conanizer.pockettracker.core.data.Project) {
        val sampleId = instrument.sampleId
        backend.setInstrumentEqSlot(sampleId, instrument.eqSlot)
        backend.setInstrumentSendLevels(sampleId, instrument.reverbSend, instrument.delaySend)
    }

    fun close() {
        backend.close()
    }
}
