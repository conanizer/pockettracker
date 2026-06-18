package com.conanizer.pockettracker.core.logic

import com.conanizer.pockettracker.core.data.Instrument
import com.conanizer.pockettracker.core.data.InstrumentPreset
import com.conanizer.pockettracker.core.data.InstrumentType
import com.conanizer.pockettracker.core.data.Note
import com.conanizer.pockettracker.core.data.Project
import com.conanizer.pockettracker.core.audio.AudioEngine
import com.conanizer.pockettracker.core.logging.ILogger
import com.conanizer.pockettracker.core.media.IVideoAudioExtractor
import com.conanizer.pockettracker.core.storage.IFileSystem
import com.conanizer.pockettracker.core.storage.WavWriter

class InstrumentController(
    private val audioEngine: AudioEngine,
    private val logger: ILogger,
    private val stateObserver: StateObserver,
    private val fileController: FileController? = null
) {
    private val TAG = "InstrumentController"

    // SoundFont slot cache: maps soundfont file path → C++ slot index (0-3)
    val sfSlotMap: MutableMap<String, Int> = mutableMapOf()

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
            audioEngine.updateInstrumentPlaybackParams(instrument)
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // SAMPLE LOADING
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Unload all samples from the audio engine.
     * Called on new project to prevent stale samples from playing on empty instruments.
     */
    fun clearAllSamples() {
        audioEngine.clearAllSamples()
        logger.d(TAG, "🗑️ All samples unloaded for new project")
    }

    /**
     * Unload ALL soundfonts from the audio engine and drop the path→slot map. Called whenever the
     * project changes (new / load) so cached SF2s — which cost ≈2× their file size in RAM — don't
     * accumulate across projects. The 4-slot LRU cache otherwise only frees a slot on eviction
     * (loading a 5th distinct SF2), so NEW/load would leave the old SF2 resident (REVIEW-3 5.1 fix).
     */
    fun clearAllSoundfonts() {
        audioEngine.backend.clearAllSoundfonts()
        sfSlotMap.clear()
        logger.d(TAG, "🗑️ All soundfonts unloaded for project change")
    }

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

            // Read cue points from WAV so slice playback works without opening the sample editor
            val cuePoints = WavWriter.readCuePoints(filePath)
            instrument.sliceMarkers = cuePoints.map { it.toLong() }
            // Auto-enable TRU slice mode when the WAV carries cue/slice data
            if (cuePoints.isNotEmpty() && instrument.slicingMode == 0) {
                instrument.slicingMode = 2
            }

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

    /**
     * Extract audio from a video file, save as WAV in the Samples directory,
     * then load it into the current instrument — reusing the existing WAV pipeline.
     *
     * @param project      Project containing the instrument data
     * @param videoPath    Absolute path to the video/audio container file
     * @param extractor    Platform-specific audio extractor
     * @param fileSystem   Platform file system for writing the WAV
     * @return LoadResult indicating success or failure
     */
    fun loadSampleFromVideo(
        project: Project,
        videoPath: String,
        extractor: IVideoAudioExtractor,
        fileSystem: IFileSystem,
        outputName: String? = null
    ): LoadResult {
        setStatus("Extracting audio...", success = true)
        logger.d(TAG, "🎬 Extracting audio from video: $videoPath")

        val result = extractor.extractAudio(videoPath)
        if (result.isFailure) {
            val msg = result.exceptionOrNull()?.message ?: "Unknown error"
            setStatus("Extract failed: $msg", success = false)
            logger.e(TAG, "❌ Video extraction failed: $msg")
            return LoadResult.Error(msg)
        }

        val audio = result.getOrThrow()
        logger.d(TAG, "✅ Extracted ${audio.samples.size} samples at ${audio.sampleRate}Hz (${audio.sourceFormat})")

        // Save extracted audio as WAV in Samples/ so the existing load pipeline handles it
        val baseName = outputName ?: (videoPath.substringAfterLast('/').substringBeforeLast('.') + "_audio")
        val wavFilename = "$baseName.wav"
        val wavPath = "${fileSystem.getSamplesDirectory()}/$wavFilename"

        setStatus("Saving WAV...", success = true)
        val written = if (audio.samplesRight != null) {
            WavWriter.writeWav(fileSystem, wavPath, audio.samples, audio.samplesRight, audio.sampleRate)
        } else {
            WavWriter.writeWavMono(fileSystem, wavPath, audio.samples, audio.sampleRate)
        }
        if (!written) {
            setStatus("Failed to save WAV", success = false)
            logger.e(TAG, "❌ Failed to write WAV: $wavPath")
            return LoadResult.Error("Failed to write WAV file")
        }

        logger.d(TAG, "💾 WAV saved: $wavPath")

        // Load the saved WAV through the normal pipeline (handles sample-rate compensation etc.)
        return loadSampleFromFile(project, wavPath)
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

        // SOUNDFONT path — route through AudioEngine.scheduleNote so pushInstrumentModulation,
        // table setup, and all effect params are applied exactly as in phrase playback.
        if (instrument.instrumentType == InstrumentType.SOUNDFONT) {
            val path = instrument.soundfontPath ?: run {
                logger.d(TAG, "⏭️ Skipping soundfont preview: no soundfont loaded")
                return
            }
            sfSlotMap[path] ?: run {
                logger.d(TAG, "⏭️ Skipping soundfont preview: slot not in sfSlotMap")
                return
            }
            val note = instrument.root
            logger.d(TAG, "🎵 Previewing soundfont instrument ${formatHex(currentInstrument)} bank=${instrument.sfBank} preset=${instrument.sfPreset} note=$note")
            audioEngine.backend.resumeStream()
            val frame = audioEngine.backend.getCurrentFrame() + 2
            audioEngine.scheduleNote(
                targetFrame = frame,
                note = note,
                instrumentId = currentInstrument,
                trackId = 0,
                volume = instrument.volume / 255f,
                phraseVol = 1.0f,
                pan = instrument.pan / 255f,
                project = project,
                // Audition the root note itself; without this, note==root + the (60-root) phrase
                // transpose collapses to a fixed C-4 and ROOT has no effect on the SF preview.
                isRootAudition = true
            )
            // Schedule note-off after ~1 second so preview doesn't sustain forever
            val sampleRate = audioEngine.backend.getSampleRate().takeIf { it > 0 } ?: 44100
            audioEngine.backend.scheduleKill(frame + sampleRate, 0)
            return
        }

        // Don't play if no sample is loaded
        if (instrument.sampleFilePath == null) {
            logger.d(TAG, "⏭️ Skipping preview for instrument ${formatHex(currentInstrument)}: no sample loaded")
            return
        }

        // Each instrument uses the table with the same ID (instrument 03 → table 03)
        logger.d(TAG, "🎵 Previewing instrument ${formatHex(currentInstrument)} (uses table ${formatHex(currentInstrument)}): root=${instrument.root}, detune=0x${formatHex(instrument.detune)}")

        // Pass project so table can be loaded
        audioEngine.previewInstrument(instrument, project)
    }

    /**
     * Preview instrument with a specific table
     * Table processing is applied during audio playback
     *
     * @param project Project containing instrument and table data
     * @param instrumentId Instrument to preview
     * @param tableId Table to use for preview (overrides instrument's default tableId)
     */
    fun previewInstrumentWithTable(project: Project, instrumentId: Int, tableId: Int) {
        val instrument = project.instruments[instrumentId.coerceIn(0, 255)]

        logger.d(TAG, "🎵 Previewing instrument ${formatHex(instrumentId)} with table ${formatHex(tableId)}: root=${instrument.root}, detune=0x${formatHex(instrument.detune)}")

        // Pass project and tableId override so the specified table is used
        audioEngine.previewInstrument(instrument, project, tableIdOverride = tableId)
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // PARAMETER UPDATES
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Update instrument root note.
     * Playback base frequency (ROOT × rate ratio / detune) is derived at schedule time (REVIEW-3 3.2).
     */
    fun updateRoot(instrument: Instrument, note: Note) {
        instrument.root = note
        logger.d(TAG, "🎹 Updated ROOT: ${instrument.root}")
    }


    /**
     * Update instrument detune parameter.
     * Playback base frequency (ROOT × rate ratio / detune) is derived at schedule time (REVIEW-3 3.2).
     */
    fun updateDetune(instrument: Instrument, detune: Int) {
        instrument.detune = detune.coerceIn(0, 255)
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

    fun updateSlicingMode(instrument: Instrument, mode: Int) {
        instrument.slicingMode = mode.coerceIn(0, 2)
        stateObserver.onStateChanged()
        logger.d(TAG, "🎛️ Updated SLICE: ${listOf("OFF","CUT","TRU")[instrument.slicingMode]}")
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Send Levels and EQ Slot
    // ─────────────────────────────────────────────────────────────────────────────

    fun updateReverbSend(instrument: Instrument, value: Int) {
        instrument.reverbSend = value.coerceIn(0, 255)
        stateObserver.onStateChanged()
    }

    fun updateDelaySend(instrument: Instrument, value: Int) {
        instrument.delaySend = value.coerceIn(0, 255)
        stateObserver.onStateChanged()
    }

    fun updateEqSlot(instrument: Instrument, value: Int) {
        instrument.eqSlot = value  // -1 = off, 0-127 = slot
        stateObserver.onStateChanged()
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Table Parameters
    // ─────────────────────────────────────────────────────────────────────────────

    /**
     * Update table TIC rate (ticks per table row)
     * @param ticRate 00-FF (default 06 = 6 tics per row = 2 rows per phrase step)
     */
    fun updateTableTicRate(instrument: Instrument, ticRate: Int) {
        instrument.tableTicRate = ticRate.coerceIn(0, 255)
        logger.d(TAG, "🎛️ Updated TBL TIC: 0x${formatHex(instrument.tableTicRate)}")
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Resampling
    // ─────────────────────────────────────────────────────────────────────────────

    /**
     * Load a rendered WAV file into the first empty instrument slot and set defaults.
     *
     * @param project  Project to add the instrument to
     * @param wavPath  Absolute path to the rendered WAV file
     * @return Instrument ID of the new instrument, or -1 if no empty slot / load failed
     */
    fun createResampledInstrument(project: Project, wavPath: String): Int {
        // Find the first genuinely free slot. isFree() (not sampleFilePath == null) so we don't claim a
        // configured SoundFont slot — those also have a null sampleFilePath, and overwriting one would
        // leave a broken SOUNDFONT-typed instrument with a sample behind it (REVIEW-3 2.1).
        val slotId = project.instruments.indexOfFirst { it.isFree() }
        if (slotId < 0) {
            setStatus("No empty instrument slot", success = false)
            logger.e(TAG, "❌ Resample: no empty instrument slot")
            return -1
        }

        val success = audioEngine.loadSampleFromFile(slotId, wavPath)
        if (!success) {
            setStatus("Failed to load resample", success = false)
            logger.e(TAG, "❌ Resample: could not load $wavPath")
            return -1
        }

        val instrument = project.instruments[slotId]
        // isFree() already guarantees a clean SAMPLER slot, but reset type/SF path defensively so the
        // claimed slot can never be left in a hybrid state.
        instrument.instrumentType = InstrumentType.SAMPLER
        instrument.soundfontPath = null
        instrument.sampleFilePath = wavPath
        instrument.sampleId = slotId
        instrument.root = Note.fromString("C-4")
        instrument.volume = 0xFF
        instrument.pan = 0x80

        audioEngine.updateInstrumentPlaybackParams(instrument)

        logger.d(TAG, "✅ Resampled instrument created: slot=${formatHex(slotId)}, path=$wavPath")
        return slotId
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // SOUNDFONT OPERATIONS
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Load an SF2/SF3 file into the current instrument slot.
     * Sets the instrument type to SOUNDFONT and stores the path.
     */
    fun loadSoundfont(project: Project, filePath: String) {
        val instrument = project.instruments[currentInstrument]
        val slot = audioEngine.backend.loadSoundfont(instrument.id, filePath)
        if (slot < 0) {
            setStatus("SF LOAD FAILED", success = false)
            return
        }
        sfSlotMap[filePath] = slot
        instrument.soundfontPath = filePath
        instrument.instrumentType = InstrumentType.SOUNDFONT

        // Initialize bank/preset to the first preset that actually exists in this SF2
        val firstPreset = audioEngine.backend.getSoundfontFirstBankPreset(slot)
        if (firstPreset[0] >= 0) {
            instrument.sfBank   = firstPreset[0]
            instrument.sfPreset = firstPreset[1]
        }

        // Apply any stored overrides to the freshly-loaded TSF preset regions
        audioEngine.applySoundfontEnvelopeOverrides(instrument)
        audioEngine.applySoundfontFilterOverrides(instrument)

        val name = filePath.substringAfterLast('/').substringBeforeLast('.')
        setStatus("SF LOADED: $name", success = true)
    }

    /**
     * Update the SF2 bank and preset for the current instrument.
     * Also refreshes the C++ slot preset so previews use the new sound.
     */
    fun updateSfBank(instrument: Instrument, bank: Int) {
        instrument.sfBank = bank
        val path = instrument.soundfontPath ?: return
        val slot = sfSlotMap[path] ?: return
        audioEngine.backend.setSoundfontPreset(slot, bank, instrument.sfPreset)
    }

    fun updateSfPreset(instrument: Instrument, preset: Int) {
        instrument.sfPreset = preset
        val path = instrument.soundfontPath ?: return
        val slot = sfSlotMap[path] ?: return
        audioEngine.backend.setSoundfontPreset(slot, instrument.sfBank, preset)
    }

    /**
     * Navigate to the preset at [index] in the SF2 preset list.
     * Updates both sfBank and sfPreset to the values for that preset.
     */
    fun setSoundfontPresetByIndex(instrument: Instrument, index: Int) {
        val path = instrument.soundfontPath ?: return
        val slot = sfSlotMap[path] ?: return
        val pair = audioEngine.backend.getSoundfontPresetAt(slot, index)
        if (pair[0] < 0) return
        instrument.sfBank   = pair[0]
        instrument.sfPreset = pair[1]
        audioEngine.backend.setSoundfontPreset(slot, pair[0], pair[1])
    }

    /**
     * Returns the list index of the current bank+preset, or 0 if not found.
     */
    fun getSoundfontCurrentPresetIndex(instrument: Instrument): Int {
        val path = instrument.soundfontPath ?: return 0
        val slot = sfSlotMap[path] ?: return 0
        val count = audioEngine.backend.getSoundfontPresetCount(slot)
        for (i in 0 until count) {
            val pair = audioEngine.backend.getSoundfontPresetAt(slot, i)
            if (pair[0] == instrument.sfBank && pair[1] == instrument.sfPreset) return i
        }
        return 0
    }

    /**
     * Returns the total number of presets in the loaded SF2, or 0 if none.
     */
    fun getSoundfontPresetCount(instrument: Instrument): Int {
        val path = instrument.soundfontPath ?: return 0
        val slot = sfSlotMap[path] ?: return 0
        return audioEngine.backend.getSoundfontPresetCount(slot)
    }

    fun updateSoundfontPreset(project: Project, bank: Int, preset: Int) {
        val instrument = project.instruments[currentInstrument]
        instrument.sfBank = bank
        instrument.sfPreset = preset
        val path = instrument.soundfontPath ?: return
        val slot = sfSlotMap[path] ?: return
        audioEngine.backend.setSoundfontPreset(slot, bank, preset)
    }

    /**
     * Return the preset display name for a soundfont instrument.
     * Returns "---" if not loaded.
     */
    fun getSoundfontPresetName(project: Project): String {
        val instrument = project.instruments[currentInstrument]
        val path = instrument.soundfontPath ?: return "---"
        val slot = sfSlotMap[path] ?: return "---"
        return audioEngine.backend.getSoundfontPresetName(slot, instrument.sfBank, instrument.sfPreset)
    }

    /**
     * Preview a soundfont note for the current instrument.
     */
    fun previewSoundfontNote(project: Project, midiNote: Int = 60) {
        val instrument = project.instruments[currentInstrument]
        val path = instrument.soundfontPath ?: return
        val slot = sfSlotMap[path] ?: return
        val frame = audioEngine.backend.getCurrentFrame() + 2
        audioEngine.backend.scheduleSoundfontNote(
            frame, 0, slot, midiNote, 100, 1.0f, 0.5f, instrument.sfBank, instrument.sfPreset
        )
    }

    fun updateSfAttack(instrument: Instrument, value: Int) {
        instrument.sfOverrides = instrument.sfOverrides.copy(ampAttack = value)
        audioEngine.applySoundfontEnvelopeOverrides(instrument)
    }
    fun updateSfDecay(instrument: Instrument, value: Int) {
        instrument.sfOverrides = instrument.sfOverrides.copy(ampDecay = value)
        audioEngine.applySoundfontEnvelopeOverrides(instrument)
    }
    fun updateSfSustain(instrument: Instrument, value: Int) {
        instrument.sfOverrides = instrument.sfOverrides.copy(ampSustain = value)
        audioEngine.applySoundfontEnvelopeOverrides(instrument)
    }
    fun updateSfRelease(instrument: Instrument, value: Int) {
        instrument.sfOverrides = instrument.sfOverrides.copy(ampRelease = value)
        audioEngine.applySoundfontEnvelopeOverrides(instrument)
    }
    fun updateSfFilterCut(instrument: Instrument, value: Int) {
        instrument.sfOverrides = instrument.sfOverrides.copy(filterCut = value)
        audioEngine.applySoundfontFilterOverrides(instrument)
    }
    fun updateSfFilterRes(instrument: Instrument, value: Int) {
        instrument.sfOverrides = instrument.sfOverrides.copy(filterRes = value)
        audioEngine.applySoundfontFilterOverrides(instrument)
    }

    /**
     * Change the instrument type. Clears sound-source metadata for the old type.
     */
    fun setInstrumentType(project: Project, newType: InstrumentType) {
        val instrument = project.instruments[currentInstrument]
        instrument.instrumentType = newType
        // Clear stale source metadata from the previous type so preview/table
        // screens don't misidentify the instrument as the old type.
        if (newType == InstrumentType.SOUNDFONT) {
            instrument.sampleFilePath = null
            // The WAV slot is now logically empty (sampleFilePath == null); free its C++ buffers too so
            // the sample doesn't linger in memory after the slot becomes a SoundFont.
            audioEngine.clearSample(currentInstrument)
        } else {
            instrument.soundfontPath = null
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // INSTRUMENT PRESET (.pti) OPERATIONS
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Save the current instrument as a .pti preset file.
     * Embeds table data if the instrument has a table assigned.
     */
    fun savePreset(project: Project, filePath: String) {
        val fc = fileController ?: run { setStatus("NO FILE CTRL", false); return }
        val instrument = project.instruments[currentInstrument]
        // Use the explicitly assigned table, or fall back to the instrument's natural table (id == index)
        val effectiveTableId = if (instrument.tableId in 0..255) instrument.tableId else instrument.id
        val candidateRows = project.tables[effectiveTableId].rows
        // Only embed table data if it has non-default content (avoid bloating every preset)
        val hasContent = candidateRows.any { r -> r.transpose != 0 || r.volume != -1 || r.fx1Type != 0 }
        val tableRows = if (hasContent) candidateRows.copyOf() else null
        val ok = fc.saveInstrumentPreset(instrument, tableRows, filePath)
        setStatus(if (ok) "SAVED: ${instrument.name}" else "SAVE FAILED", ok)
    }

    /**
     * Load a .pti preset file into the current instrument slot.
     * Auto-loads the source file (WAV or SF2) from the stored path.
     * If the source is missing, loads parameters only and shows a warning.
     */
    fun loadPreset(project: Project, filePath: String) {
        val fc = fileController ?: run { setStatus("NO FILE CTRL", false); return }
        val preset = fc.loadInstrumentPreset(filePath) ?: run {
            setStatus("LOAD FAILED", false)
            return
        }

        val instrument = project.instruments[currentInstrument]
        val src = preset.instrument

        // Copy all parameters (preserve id)
        instrument.name            = src.name
        instrument.instrumentType  = src.instrumentType
        instrument.volume          = src.volume
        instrument.pan             = src.pan
        instrument.root            = src.root
        instrument.detune          = src.detune
        instrument.drive           = src.drive
        instrument.crush           = src.crush
        instrument.downsample      = src.downsample
        instrument.filterType      = src.filterType
        instrument.filterCut       = src.filterCut
        instrument.filterRes       = src.filterRes
        instrument.sampleStart     = src.sampleStart
        instrument.sampleEnd       = src.sampleEnd
        instrument.reverse         = src.reverse
        instrument.loopMode        = src.loopMode
        instrument.loopStart       = src.loopStart
        instrument.tableTicRate    = src.tableTicRate
        instrument.slicingMode     = src.slicingMode
        instrument.sfBank          = src.sfBank
        instrument.sfPreset        = src.sfPreset
        instrument.sfOverrides     = src.sfOverrides
        instrument.modSlots        = src.modSlots.copyOf()

        // Load table data if embedded — always into the destination instrument's own table slot
        // (instrument index = table index, so INST01 always owns TABLE01)
        if (preset.tableRows != null) {
            val targetTableId = currentInstrument
            preset.tableRows.forEachIndexed { i, row -> project.tables[targetTableId].rows[i] = row }
            instrument.tableId = targetTableId
            audioEngine.loadTable(project.tables[targetTableId])
        }

        // Auto-load source file
        when (instrument.instrumentType) {
            InstrumentType.SAMPLER -> {
                val path = src.sampleFilePath
                if (path != null) {
                    instrument.sampleFilePath = path
                    val ok = audioEngine.loadSampleFromFile(instrument.id, path)
                    if (!ok) {
                        setStatus("SRC MISSING: ${path.substringAfterLast('/')}", false)
                    } else {
                        instrument.sampleId = currentInstrument
                        instrument.sliceMarkers = WavWriter.readCuePoints(path).map { it.toLong() }
                        // Push all parameters (filter, drive, start/end, etc.) to C++ for this slot
                        audioEngine.updateInstrumentPlaybackParams(instrument)
                        setStatus("LOADED: ${src.name}", true)
                    }
                } else {
                    setStatus("LOADED: ${src.name}", true)
                }
            }
            InstrumentType.SOUNDFONT -> {
                val path = src.soundfontPath
                if (path != null) {
                    instrument.soundfontPath = path
                    val slot = audioEngine.backend.loadSoundfont(instrument.id, path)
                    if (slot < 0) {
                        setStatus("SRC MISSING: ${path.substringAfterLast('/')}", false)
                    } else {
                        sfSlotMap[path] = slot
                        // Validate saved bank/preset; fall back to first available if not found
                        val presetName = audioEngine.backend.getSoundfontPresetName(slot, instrument.sfBank, instrument.sfPreset)
                        if (presetName == "---") {
                            val firstPreset = audioEngine.backend.getSoundfontFirstBankPreset(slot)
                            if (firstPreset[0] >= 0) {
                                instrument.sfBank   = firstPreset[0]
                                instrument.sfPreset = firstPreset[1]
                            }
                        }
                        audioEngine.applySoundfontEnvelopeOverrides(instrument)
                        audioEngine.applySoundfontFilterOverrides(instrument)
                        setStatus("LOADED: ${src.name}", true)
                    }
                } else {
                    setStatus("LOADED: ${src.name}", true)
                }
            }
        }
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
