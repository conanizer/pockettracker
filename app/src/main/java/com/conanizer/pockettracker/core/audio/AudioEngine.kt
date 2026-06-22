package com.conanizer.pockettracker.core.audio

import com.conanizer.pockettracker.core.data.Instrument
import com.conanizer.pockettracker.core.data.InstrumentType
import com.conanizer.pockettracker.core.data.ModDest
import com.conanizer.pockettracker.core.data.ModType
import com.conanizer.pockettracker.core.data.Note
import com.conanizer.pockettracker.core.data.Project
import com.conanizer.pockettracker.core.data.Table
import com.conanizer.pockettracker.core.data.TICS_PER_STEP
import com.conanizer.pockettracker.core.data.VolumeUtils
import com.conanizer.pockettracker.core.logging.ILogger
import com.conanizer.pockettracker.core.media.IVideoAudioExtractor
import com.conanizer.pockettracker.core.resources.IResourceLoader
import java.io.File
import kotlin.math.pow

class AudioEngine(
    internal val backend: IAudioBackend,
    private val resourceLoader: IResourceLoader,
    private val logger: ILogger
) {
    private val TAG = "AudioEngine"
    private val PREVIEW_TRACK_ID = 8  // dedicated voice slot for all previews (outside song tracks 0-7)

    companion object {
        // Reference pitch for C-4 (middle C) in Hz. Used as the canonical sample base frequency
        // before sample-rate compensation and detune are applied.
        const val C4_HZ = 261.63f

        // Per-note scheduling trace. Off in shipped builds: the scheduleNote log below formats a
        // string for every note even when logcat hides it. Flip to true for note-level debugging.
        const val TRACE = false
    }

    /**
     * Frames per modulation tic at the given tempo. One step = TICS_PER_STEP tics; the sequencer
     * runs 4 steps/beat. Used to convert tick-based pitch-effect params to frame-based for C++.
     */
    private fun framesPerTicAt(tempo: Int): Float =
        backend.getSampleRate().toFloat() / (tempo / 60f * 4f * TICS_PER_STEP)

    // Waveform buffer for visualization (620 samples for 620px width oscilloscope)
    val waveformBuffer = FloatArray(620) { 0f }

    // Per-track waveform buffers for OCTA visualizer.
    // 9 lanes: 8 song tracks + 1 preview lane (index 8) so sampler/sample/note previews — which
    // play on PREVIEW_TRACK_ID, outside tracks 0-7 — get their own scope. Must match C++
    // AudioEngine::TRACK_WAVEFORM_COUNT.
    private val trackWaveformBufferFlat = FloatArray(9 * 620)
    private val activeTrackFlags = BooleanArray(9)
    val trackWaveformBuffers: Array<FloatArray> = Array(9) { FloatArray(620) }

    /** True when the preview lane (index 8) had audio last block — drives the OCTA preview scope. */
    val previewLaneActive: Boolean get() = activeTrackFlags[8]

    // Spectrum buffer for SPECTRUM/SPECTRUM_PEAKS visualizer (40 log-spaced bins, 0-1)
    val spectrumBuffer = FloatArray(40)

    // Bitmask of tracks that had notes scheduled in the current phrase.
    // Set per-track when a note is scheduled; cleared on clearScheduledNotes().
    // Used by OCTA visualizer so the layout stays stable for the full phrase duration.
    var phraseTrackMask: Int = 0
        private set

    // Sample-rate compensation ratio per slot (deviceRate / wavRate). The playback base frequency
    // (ROOT × ratio / detune) is derived from this on demand via calculateInstrumentBaseFrequency() —
    // there is no separate base-frequency cache to keep in sync (REVIEW-3 3.2; the dual cache was the
    // root cause of REVIEW-2's RATE-pitch bug, where one copy went stale while the other stayed right).
    private val sampleRateRatios = mutableMapOf<Int, Float>()
    // Original ratios cached for non-destructive RATE mode (cleared when new sample is loaded)
    private val originalSampleRateRatios = mutableMapOf<Int, Float>()

    // Injected by PlaybackController so scheduleNote() can route SF instruments without Android dependency.
    var sfSlotProvider: ((soundfontPath: String) -> Int?)? = null

    // True once backend.create() has opened the native stream. The UI now renders during the
    // (sometimes multi-second) first stream-open instead of behind a loading screen, so the visualizer
    // poll methods below — and START playback — must no-op until this flips.
    @Volatile var isReady = false
        private set

    fun create(): Boolean {
        val success = backend.create()
        if (success) {
            logger.d(TAG, "✅ Audio engine created successfully")
            isReady = true
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

            // Decode entirely in native memory (no Java-heap round trip). A multi-MB sample used to
            // need the whole file ByteArray + both float channels live at once on the capped Java
            // heap, which OOM-killed large loads even on a 3 GB device (REVIEW-3 6.2). Native returns
            // the WAV sample rate; the rate-compensation ratio is still computed here.
            val wavRate = backend.loadSampleFromWav(instrumentId, filePath)
            if (wavRate <= 0) {
                logger.e(TAG, "❌ Failed to load sample (native WAV decode) from $filePath")
                return false
            }

            sampleRateRatios[instrumentId] = getDeviceSampleRate().toFloat() / wavRate.toFloat()
            originalSampleRateRatios.remove(instrumentId)

            logger.d(TAG, "✅ Loaded sample: instrumentId=$instrumentId, length=${backend.getSampleLength(instrumentId)}, stereo=${backend.hasStereoData(instrumentId)}, rateRatio=${sampleRateRatios[instrumentId]}, path=$filePath")
            return true
        } catch (e: Exception) {
            logger.e(TAG, "❌ Error loading sample from file: ${e.message}")
            return false
        }
    }

    /**
     * Load already-decoded PCM (e.g. from an MP3/container via the audio extractor) straight into a
     * persistent instrument slot — no intermediate WAV file. Sets the same sample-rate-compensation
     * ratio loadSampleFromFile does, so playback pitch is correct for non-device-rate sources
     * (a 48 kHz MP3 would otherwise play ~8.8% sharp on a 44.1 kHz device).
     */
    fun loadSampleData(instrumentId: Int, samples: FloatArray, samplesRight: FloatArray?, sampleRate: Int): Boolean {
        return try {
            if (samples.isEmpty() || sampleRate <= 0) return false
            if (samplesRight != null && samplesRight.size == samples.size) {
                backend.loadSampleStereo(instrumentId, samples, samplesRight)
            } else {
                backend.loadSample(instrumentId, samples)
            }
            sampleRateRatios[instrumentId] = getDeviceSampleRate().toFloat() / sampleRate.toFloat()
            originalSampleRateRatios.remove(instrumentId)
            logger.d(TAG, "✅ Loaded in-memory sample: id=$instrumentId frames=${samples.size} rate=$sampleRate stereo=${samplesRight != null}")
            true
        } catch (e: Exception) {
            logger.e(TAG, "❌ Error loading in-memory sample: ${e.message}")
            false
        }
    }

    /**
     * Load a compressed/container file (e.g. MP3) into instrument slot [instrumentId]. Prefers the
     * STREAMING path — the extractor decodes block-by-block straight into native memory, so the whole
     * PCM never lands on the Java heap (which previously capped loads at a few MB and left the heap
     * inflated). Falls back to the whole-file decode for sources the streaming path can't pre-size
     * (no duration metadata). Sets the sample-rate-compensation ratio either way. Returns success.
     */
    fun loadSampleCompressed(instrumentId: Int, path: String, extractor: IVideoAudioExtractor): Boolean {
        val sink = object : IVideoAudioExtractor.PcmSink {
            override fun onFormat(sampleRate: Int, channels: Int, estimatedFrames: Int) {
                backend.beginSampleLoad(instrumentId, channels, estimatedFrames)
            }
            override fun onChunk(interleaved: ShortArray, frameCount: Int, channels: Int) {
                backend.fillSampleChunk(instrumentId, interleaved, frameCount, channels)
            }
        }
        val streamed = try {
            extractor.extractAudioToSink(path, maxDurationSec = 0, sink)
        } catch (e: Exception) {
            Result.failure(e)
        }
        if (streamed.isSuccess) {
            val info = streamed.getOrThrow()
            val frames = backend.finalizeSampleLoad(instrumentId)
            if (frames > 0) {
                sampleRateRatios[instrumentId] = getDeviceSampleRate().toFloat() / info.sampleRate.toFloat()
                originalSampleRateRatios.remove(instrumentId)
                logger.d(TAG, "✅ Streamed compressed sample: id=$instrumentId frames=$frames rate=${info.sampleRate} (${info.sourceFormat})")
                return true
            }
            backend.cancelSampleLoad(instrumentId)
            return false
        }

        // Fallback: whole-file decode (handles sources without duration metadata). Free any partial first.
        backend.cancelSampleLoad(instrumentId)
        logger.d(TAG, "↩︎ Streaming unavailable (${streamed.exceptionOrNull()?.message}); whole-file decode")
        val result = extractor.extractAudio(path, maxDurationSec = 0)
        if (result.isFailure) {
            logger.e(TAG, "❌ Compressed load failed: ${result.exceptionOrNull()?.message}")
            return false
        }
        val audio = result.getOrThrow()
        return loadSampleData(instrumentId, audio.samples, audio.samplesRight, audio.sampleRate)
    }

    // WAV file decoding now happens in C++ (backend.loadSampleFromWav → AudioEngine::loadSampleFromWavFile)
    // so a multi-MB sample never has to fit in the capped Java heap (REVIEW-3 6.2). The old Kotlin
    // loadWavFileFromPath/parseWavBuffer were removed once the native path was device-confirmed.
    // (previewSampleData further down still takes in-memory floats — resampled audio — which is a
    // separate path with no file to stream.)

    fun previewSampleFile(filePath: String): Boolean {
        try {
            val file = File(filePath)
            if (!file.exists() || !file.canRead()) {
                logger.e(TAG, "Cannot read file: $filePath")
                return false
            }

            // Stop only the previous preview voice — leave song playback untouched.
            backend.killTrack(PREVIEW_TRACK_ID)

            // Native decode (see loadSampleFromFile / REVIEW-3 6.2) — no Java-heap copy of the file.
            val wavRate = backend.loadSampleFromWav(255, filePath)
            if (wavRate <= 0) {
                logger.e(TAG, "❌ Failed to preview sample (native WAV decode): $filePath")
                return false
            }
            val adjustedBaseFreq = C4_HZ * (getDeviceSampleRate().toFloat() / wavRate.toFloat())

            backend.resumeStream()

            val c4Freq = C4_HZ
            val targetFrame = backend.getCurrentFrame() + 100
            backend.scheduleNote(
                frame = targetFrame,
                sampleId = 255,
                trackId = PREVIEW_TRACK_ID,
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
            backend.killTrack(PREVIEW_TRACK_ID)
            if (samplesRight != null) {
                backend.loadSampleStereo(255, samples, samplesRight)
            } else {
                backend.loadSample(255, samples)
            }
            backend.resumeStream()
            val adjustedBaseFreq = C4_HZ * (getDeviceSampleRate().toFloat() / sampleRate.toFloat())
            backend.scheduleNote(
                frame = backend.getCurrentFrame() + 100,
                sampleId = 255,
                trackId = PREVIEW_TRACK_ID,
                freq = C4_HZ,
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
                trackId = PREVIEW_TRACK_ID,
                volume = VolumeUtils.hexToFloat(instrument.volume),
                phraseVol = 1.0f,
                pan = VolumeUtils.hexToFloat(instrument.pan),
                project = project,
                tableIdOverride = tableIdOverride,
                isRootAudition = true
            )
            // Hard-stop after 2 seconds so SF sustain notes don't ring forever during preview.
            val sr = backend.getSampleRate().toLong().coerceAtLeast(44100L)
            backend.scheduleKill(targetFrame + sr * 2, PREVIEW_TRACK_ID)
            return
        }

        val sampleId = instrument.sampleId

        val rootFreq = instrument.root.toFrequency()
        val detuneSemitones = (instrument.detune shr 4).toFloat()
        val detuneFraction = (instrument.detune and 0x0F) / 16.0f
        val totalDetuneSemitones = detuneSemitones + detuneFraction - 8.0f
        val detuneMultiplier = 2.0.pow(totalDetuneSemitones / 12.0).toFloat()
        val targetFreq = rootFreq * detuneMultiplier

        val sampleRateRatio = sampleRateRatios[sampleId] ?: 1.0f
        val compensatedBaseFreq = C4_HZ * sampleRateRatio

        // CRITICAL: Resume stream so audio callback processes the scheduled note
        backend.resumeStream()

        val targetFrame = backend.getCurrentFrame() + 100

        val volume = VolumeUtils.hexToFloat(instrument.volume)
        val pan = VolumeUtils.hexToFloat(instrument.pan)

        val tableId = if (tableIdOverride >= 0) tableIdOverride else instrument.id
        val tableTicRate = instrument.tableTicRate

        // Force reload table when previewing (user may have just edited it)
        if (tableId >= 0 && project != null && tableId < project.tables.size) {
            forceReloadTable(project.tables[tableId])
        }

        // Push current modulation params, EQ, and sends so preview reflects latest UI edits
        val tempo = project?.tempo ?: 120
        pushInstrumentModulation(instrument, tempo)
        if (project != null) pushInstrumentEqAndSends(instrument, project)

        backend.scheduleNoteWithTable(
            frame = targetFrame,
            sampleId = sampleId,
            trackId = PREVIEW_TRACK_ID,
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

    // Plays the sample at ROOT pitch with no instrument effects (no filter, EQ, sends, or mod).
    // Used by the sample editor so the user hears the raw waveform, not the processed sound.
    // Caller must restore EQ/sends/mod via pushInstrumentEqAndSends + pushInstrumentModulation
    // after a suitable delay (the effect params are zeroed here to take effect at voice trigger).
    fun previewInstrumentDry(instrument: Instrument) {
        if (instrument.instrumentType == InstrumentType.SOUNDFONT) return
        val sampleId = instrument.sampleId

        val rootFreq = instrument.root.toFrequency()
        val detuneSemitones = (instrument.detune shr 4).toFloat()
        val detuneFraction = (instrument.detune and 0x0F) / 16.0f
        val totalDetuneSemitones = detuneSemitones + detuneFraction - 8.0f
        val detuneMultiplier = 2.0.pow(totalDetuneSemitones / 12.0).toFloat()
        val targetFreq = rootFreq * detuneMultiplier

        val sampleRateRatio = sampleRateRatios[sampleId] ?: 1.0f
        val compensatedBaseFreq = C4_HZ * sampleRateRatio

        backend.killTrack(PREVIEW_TRACK_ID)
        backend.resumeStream()

        backend.clearInstrumentModulation(sampleId)
        backend.setInstrumentEqSlot(sampleId, -1)
        backend.setInstrumentSendLevels(sampleId, 0, 0)

        backend.scheduleNote(
            frame = backend.getCurrentFrame() + 100,
            sampleId = sampleId,
            trackId = PREVIEW_TRACK_ID,
            freq = targetFreq,
            baseFreq = compensatedBaseFreq,
            vol = 1.0f,
            pan = 0.5f
        )

        logger.d(TAG, "🔊 [DRY] Preview instrument ${instrument.id.toString(16).padStart(2,'0').uppercase()}: freq=$targetFreq Hz")
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
                trackId = PREVIEW_TRACK_ID,
                volume = VolumeUtils.hexToFloat(instrument.volume),
                phraseVol = 1.0f,
                pan = VolumeUtils.hexToFloat(instrument.pan),
                project = project,
                tableIdOverride = -1
            )
            backend.scheduleKill(targetFrame + durationFrames, PREVIEW_TRACK_ID)
            return
        }

        val sampleId = instrument.sampleId
        val detuneSemitones = (instrument.detune shr 4).toFloat()
        val detuneFraction = (instrument.detune and 0x0F) / 16.0f
        val totalDetuneSemitones = detuneSemitones + detuneFraction - 8.0f
        val detuneMultiplier = 2.0.pow(totalDetuneSemitones / 12.0).toFloat()
        val targetFreq = note.toFrequency() * detuneMultiplier

        val sampleRateRatio = sampleRateRatios[sampleId] ?: 1.0f
        val compensatedBaseFreq = C4_HZ * sampleRateRatio

        backend.resumeStream()
        val targetFrame = backend.getCurrentFrame() + 100L

        val volume = VolumeUtils.hexToFloat(instrument.volume)
        val pan = VolumeUtils.hexToFloat(instrument.pan)
        val tableId = instrument.id
        val tableTicRate = instrument.tableTicRate

        if (project != null && tableId < project.tables.size) forceReloadTable(project.tables[tableId])

        val tempo = project?.tempo ?: 120
        pushInstrumentModulation(instrument, tempo)
        if (project != null) pushInstrumentEqAndSends(instrument, project)

        backend.scheduleNoteWithTable(
            frame = targetFrame,
            sampleId = sampleId,
            trackId = PREVIEW_TRACK_ID,
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
        backend.scheduleKill(targetFrame + durationFrames, PREVIEW_TRACK_ID)
    }

    fun calculateInstrumentBaseFrequency(instrument: Instrument): Float {
        val rootFreq = instrument.root.toFrequency()
        val detuneSemitones = (instrument.detune shr 4).toFloat()
        val detuneFraction = (instrument.detune and 0x0F) / 16.0f
        val totalDetuneSemitones = detuneSemitones + detuneFraction - 8.0f
        val detuneMultiplier = 2.0.pow(totalDetuneSemitones / 12.0).toFloat()
        val sampleRateRatio = sampleRateRatios[instrument.sampleId] ?: 1.0f
        // Detune DIVIDES the base frequency: playback rate = noteFreq / baseFreq, so a sharper detune
        // must lower baseFreq to raise the pitch. Multiplying here inverted it (sharper → lower), which
        // disagreed with the preview (and SF), where detune scales the target frequency. Default detune
        // (0x80 → multiplier 1.0) is unchanged.
        return rootFreq * sampleRateRatio / detuneMultiplier
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
        if (!isReady) return
        if (!isPlaying) {
            backend.decayWaveform()
        }
        backend.updateWaveform(waveformBuffer)
    }

    fun updateTrackWaveforms() {
        if (!isReady) return
        backend.getTrackWaveforms(trackWaveformBufferFlat, activeTrackFlags)
        for (t in 0 until 9) {
            trackWaveformBufferFlat.copyInto(trackWaveformBuffers[t], 0, t * 620, (t + 1) * 620)
        }
    }

    fun updateSpectrum() {
        if (!isReady) return
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
        // MIDI velocity (0–127) for the SF2 path: when ≥0 it drives TSF's velocity directly and the
        // SF channel volume is left at 1.0 (TSF applies its own velocity curve). -1 = legacy: derive
        // velocity from `volume` (used by retrig/arp). The sampler path ignores this.
        midiVelocity: Int = -1,
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
        transposeSemitones: Int = 0,
        pitSemitones: Int = 0,
        sliIndex: Int = -1,
        arpSemitoneOffset: Int = 0,
        // True only for the instrument-screen "audition the root" preview: the SF path then plays the
        // root note as written instead of applying the (60 - root) phrase transpose (which, when the
        // note IS the root, would cancel to a fixed C-4). No effect on the sampler path.
        isRootAudition: Boolean = false
    ) {
        if (note == Note.EMPTY) return
        if (trackId in 0..7) phraseTrackMask = phraseTrackMask or (1 shl trackId)

        val instrument = if (instrumentId in project.instruments.indices) {
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
            val baseMidi = (note.octave + 1) * 12 + note.pitch + arpSemitoneOffset
            // ROOT acts as a transpose, matching the sampler: ROOT below C-4 raises pitch, above lowers it.
            // (C-4/60 is neutral.) Sign is 60 - root, NOT root - 60, so SF and sampler agree on direction.
            // For the root-audition preview we skip the transpose so it plays the root note itself
            // (otherwise note==root + (60-root) would always collapse to C-4, ignoring ROOT).
            val transpose = if (isRootAudition) 0 else (60 - instrument.root.toMidi())
            val midiNote = (baseMidi + transpose).coerceIn(0, 127)
            // VOL column = MIDI velocity → TSF applies its own (dB) velocity curve. When a velocity
            // is supplied, the SF channel volume (noteVol) stays at 1.0 so we don't double-curve;
            // instrument-vol/Vxx arrives via phraseVol. Legacy callers (retrig/arp) pass -1.
            val velocity  = if (midiVelocity >= 0) midiVelocity.coerceIn(1, 127)
                            else (volume * 127).toInt().coerceIn(1, 127)
            val sfNoteVol = if (midiVelocity >= 0) 1.0f else volume
            // Detune (same encoding as sampler): high nibble = semitones, low nibble = 1/16th, centred at 0x80.
            // Applied to the SF voice as a fractional pitch-wheel offset (sampler bakes it into baseFreq instead).
            val sfDetune = (instrument.detune shr 4).toFloat() + (instrument.detune and 0x0F) / 16.0f - 8.0f
            // Convert tick-based pitch params to frame-based (same as sampler path)
            val tempo = project.tempo
            val framesPerTic = framesPerTicAt(tempo)
            val framesPerStep = framesPerTic * TICS_PER_STEP
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
            if (sfTableId in project.tables.indices) {
                ensureTableLoaded(project.tables[sfTableId])
            }

            backend.resumeStream()
            backend.scheduleSoundfontNote(
                targetFrame, trackId, slot,
                midiNote, velocity, sfNoteVol, pan,
                instrument.sfBank, instrument.sfPreset,
                pslInitialOffset, pslDurationFrames, pbnRatePerFrame, vibratoSpeed, vibratoDepth,
                phraseVol = phraseVol,
                sampleId = instrumentId,
                tableId = sfTableId,
                tableTicRate = sfTicRate,
                noteOctave = note.octave,
                notePitch = note.pitch,
                tableStartRow = tableStartRow,
                detuneSemitones = sfDetune
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

        if (tableId in project.tables.indices) {
            ensureTableLoaded(project.tables[tableId])
        }

        if (TRACE) logger.d(TAG, "📋 scheduleNote: inst=$instrumentId → sampleId=$sampleId, note=$note, frame=$targetFrame, vol=${"%.4f".format(volume)}, pan=$pan, tableId=$tableId")

        // Base frequency = ROOT × sampleRateRatio / detune, computed fresh from the live instrument so
        // it can never drift from the rate ratio (REVIEW-3 3.2). Slice mode overrides it below.
        val baseFreq = calculateInstrumentBaseFrequency(instrument)

        // Slice playback: triggered by slicingMode (CUT/TRU) or an explicit SLI FX.
        // SLI works even when slicingMode=OFF as long as the sample has markers.
        // Pitch is always ROOT + chain/master transpose; phrase note never affects pitch in slice mode.
        // PIT FX is applied after this block so it shifts pitch without touching slice selection.
        var effectiveNote = note
        var effectiveBaseFreq = baseFreq  // may be overridden below for slice mode
        var effectiveStartOverride = startPointOverride
        var effectiveEndOverride = -1
        val useSliceLogic = (instrument.slicingMode != 0 || sliIndex >= 0) && instrument.sliceMarkers.isNotEmpty()
        if (useSliceLogic) {
            val markers = instrument.sliceMarkers
            // SLI explicit index takes priority; otherwise derive from raw phrase note (before transpose).
            // C-4 (MIDI 60 in standard MIDI) = slice 0; C#4 = slice 1; etc.
            // ROOT has no effect on slice mapping — it only affects playback pitch.
            val resolvedSliceIndex = if (sliIndex >= 0) {
                sliIndex.coerceAtMost(markers.size)
            } else {
                (note.toMidi() - transposeSemitones - 60).coerceAtLeast(0).coerceAtMost(markers.size)
            }
            val totalFrames = backend.getSampleLength(sampleId).toLong()
            if (totalFrames > 0) {
                val sliceStart = if (resolvedSliceIndex == 0) 0L else markers[resolvedSliceIndex - 1]
                effectiveStartOverride = ((sliceStart * 255L) / totalFrames).toInt().coerceIn(0, 255)
                // Pitch = ROOT + chain/master transpose; phrase note column only selects slice.
                effectiveNote = Note.fromMidi((instrument.root.toMidi() + transposeSemitones).coerceIn(0, 127))
                // The standard baseFreq includes ROOT (= ROOT × sampleRateRatio / detune), so when
                // effectiveNote is also ROOT-based the C++ rate calculation (freq/baseFreq) would
                // cancel ROOT out and ROOT would have no effect on pitch. Fix: strip ROOT from baseFreq
                // so it only carries sampleRateRatio / detune, letting ROOT appear only in the numerator.
                val sampleRateRatio = sampleRateRatios[sampleId] ?: 1.0f
                val detuneSemitones = (instrument.detune shr 4).toFloat()
                val detuneFraction = (instrument.detune and 0x0F) / 16.0f
                val totalDetuneSemitones = detuneSemitones + detuneFraction - 8.0f
                val detuneMultiplier = 2.0.pow(totalDetuneSemitones / 12.0).toFloat()
                // Detune DIVIDES (same as calculateInstrumentBaseFrequency): sharper detune → higher pitch.
                effectiveBaseFreq = C4_HZ * sampleRateRatio / detuneMultiplier
                // CUT end boundary only when slicingMode=CUT; OFF+SLI or TRU play to sample end
                if (instrument.slicingMode == 1) {
                    val sliceEnd = if (resolvedSliceIndex < markers.size) markers[resolvedSliceIndex] else totalFrames
                    effectiveEndOverride = ((sliceEnd * 255L) / totalFrames).toInt().coerceIn(0, 255)
                }
            }
        }

        // PIT FX: shift pitch by semitones after slice logic so it never affects slice selection
        if (pitSemitones != 0) {
            effectiveNote = Note.fromMidi((effectiveNote.toMidi() + pitSemitones).coerceIn(0, 127))
        }
        // ARP offset: applied to pitch only — slice index was already resolved from the base note
        if (arpSemitoneOffset != 0) {
            effectiveNote = Note.fromMidi((effectiveNote.toMidi() + arpSemitoneOffset).coerceIn(0, 127))
        }

        val frequency = effectiveNote.toFrequency()

        // Push modulation params, EQ, and send levels to C++ engine.
        // Must be done before scheduleNoteWithTable so the engine has correct params at trigger time.
        val tempo = project.tempo
        pushInstrumentModulation(instrument, tempo)
        pushInstrumentEqAndSends(instrument, project)

        // Convert tick-based pitch effect params to frame-based so C++ needs no tempo knowledge.
        val framesPerTic = framesPerTicAt(tempo)
        val framesPerStep = framesPerTic * TICS_PER_STEP
        val pslDurationFrames = if (pslDuration > 0f) pslDuration * framesPerTic else 0f
        // pbnRate is semitones/step (per EffectProcessor docs: "PBN 10 = 1 semitone per step")
        val pbnRatePerFrame  = if (pbnRate  != 0f)  pbnRate  / framesPerStep  else 0f

        // Resume stream so audio callback can process the queue
        backend.resumeStream()

        // Always use scheduleNoteWithTable - C++ handles tableId=-1 as "no table"
        backend.scheduleNoteWithTable(
            targetFrame, sampleId, trackId, frequency, effectiveBaseFreq, volume, phraseVol, pan,
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
        sampleRateRatios.clear()
        originalSampleRateRatios.clear()
        backend.clearAllSamples()
    }

    /** Free the C++ buffers + cached metadata for one slot (e.g. when switching it to SoundFont). */
    fun clearSample(instrumentId: Int) {
        sampleRateRatios.remove(instrumentId)
        originalSampleRateRatios.remove(instrumentId)
        backend.clearSample(instrumentId)
    }

    /** Free the sample editor's single-level undo backup for [instrumentId] (call on editor close — the
     *  undo is unreachable once the editor is gone, so it's just wasted RAM). See REVIEW-3 1.1. */
    fun freeSampleUndo(instrumentId: Int) = backend.freeSampleUndo(instrumentId)

    fun getSampleLength(instrumentId: Int): Int = backend.getSampleLength(instrumentId)
    fun getSampleWaveform(instrumentId: Int, numBins: Int): FloatArray = backend.getSampleWaveform(instrumentId, numBins)
    fun getSampleWaveformRange(instrumentId: Int, startFrame: Int, endFrame: Int, numBins: Int): FloatArray = backend.getSampleWaveformRange(instrumentId, startFrame, endFrame, numBins)
    fun getSampleData(instrumentId: Int): FloatArray = backend.getSampleData(instrumentId)

    fun getSpectrumMagnitudes(numBins: Int): FloatArray = backend.getSpectrumMagnitudes(numBins)
    fun getSpectrumMagnitudesForSource(source: Int, instrId: Int, numBins: Int): FloatArray =
        backend.getSpectrumMagnitudesForSource(source, instrId, numBins)

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
        // Playback base freq is derived from sampleRateRatios at schedule time (REVIEW-3 3.2), so
        // scaling the ratio is the whole job — there is no second cache to keep in sync.
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
        // Playback base freq is derived from sampleRateRatios at schedule time (REVIEW-3 3.2) — the
        // updated ratio above is all that's needed; there's no separate base-frequency cache to scale.
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

    fun findZeroCrossing(instrumentId: Int, frame: Int, dir: Int = 0): Int = backend.findZeroCrossing(instrumentId, frame, dir)
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
        loopEnd: Int,
        drive: Int,
        crush: Int,
        downsample: Int,
        filterType: Int,
        filterCut: Int,
        filterRes: Int
    ) {
        backend.setInstrumentParams(
            instrumentId, startPoint, endPoint, reverse, loopMode, loopStart, loopEnd,
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
            loopEnd = instrument.loopEnd,
            drive = instrument.drive,
            crush = instrument.crush,
            downsample = instrument.downsample,
            filterType = filterTypeInt,
            filterCut = instrument.filterCut,
            filterRes = instrument.filterRes
        )
    }

    fun applySoundfontEnvelopeOverrides(instrument: Instrument) {
        // Store the override keyed by instrument id; C++ applies it atomically at note trigger
        // (REVIEW-3 5.1 SF de-dup). No slot/bank/preset needed — the trigger uses the note's own
        // bank/preset, so two instruments sharing one de-duplicated handle stay isolated.
        val ov = instrument.sfOverrides
        backend.setSoundfontEnvelopeOverrides(instrument.id,
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
        val framesPerTic = sampleRate / (beatsPerSecond * stepsPerBeat * TICS_PER_STEP)

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
        isReady = false
        backend.close()
    }
}
