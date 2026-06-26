package com.conanizer.pockettracker.core.logic

import com.conanizer.pockettracker.core.audio.AudioEngine
import com.conanizer.pockettracker.core.data.Project
import com.conanizer.pockettracker.core.logging.ILogger
import com.conanizer.pockettracker.core.storage.IFileSystem
import com.conanizer.pockettracker.core.storage.WavStreamWriter

/**
 * RENDER CONTROLLER
 *
 * Renders the song to a WAV file by delegating scheduling entirely to
 * [PlaybackController.scheduleSongForRender].  This guarantees that groove,
 * DEL, arpeggio, HOP, pitch effects and every other effect behaves identically
 * to live playback — there is no separate scheduling code path here.
 *
 * Rendering process:
 *   1. Find song bounds (first / last used row)
 *   2. Set up instrument params (sample points, filter, modulation)
 *   3. scheduleSongForRender → fills the C++ note queue at frames 0..N
 *   4. renderToWavFile → renders in ~5 s chunks, streaming each chunk to a
 *      16-bit stereo WAV via WavStreamWriter (flat memory use, real progress)
 */
class RenderController(
    private val audioEngine: AudioEngine,
    private val playbackController: PlaybackController,
    private val fileSystem: IFileSystem,
    private val logger: ILogger
) {
    private val audioBackend get() = audioEngine.backend

    companion object {
        private const val TAG = "RenderController"

        // Frames per renderFrames() JNI call: ~5 s of stereo float ≈ 1.7 MB per chunk.
        // Rendering the whole song in ONE call held ~4 full-song copies in RAM at peak
        // (C++ vector + jfloatArray + channel splits + WAV ByteBuffer) — an OOM kill on
        // 1 GB devices for songs of a few minutes. Chunking keeps peak memory flat and
        // makes real render progress reporting possible.
        private const val RENDER_CHUNK_FRAMES = 220_500
    }

    /**
     * Render [totalFrames] from the already-scheduled C++ note queue into a 16-bit stereo WAV
     * at [path], in RENDER_CHUNK_FRAMES slices. Reports progress over [progressFrom]..[progressTo].
     * The engine keeps its state (frame counter, scheduled notes) across chunks, so chunked
     * output is bit-identical to a single renderFrames(totalFrames) call.
     */
    private fun renderToWavFile(
        totalFrames: Long,
        sampleRate: Int,
        path: String,
        progressCallback: ProgressCallback?,
        progressFrom: Float,
        progressTo: Float,
        progressLabel: String
    ): Boolean {
        val writer = WavStreamWriter(path, sampleRate)
        try {
            var rendered = 0L
            while (rendered < totalFrames) {
                val chunk = minOf(RENDER_CHUNK_FRAMES.toLong(), totalFrames - rendered).toInt()
                val audio = audioBackend.renderFrames(chunk, sampleRate)
                writer.appendInterleaved(audio, chunk)
                rendered += chunk
                val p = progressFrom + (progressTo - progressFrom) * (rendered.toFloat() / totalFrames)
                progressCallback?.onProgress(p, progressLabel)
            }
            return writer.finish()
        } catch (e: Exception) {
            writer.abort()
            throw e
        }
    }

    sealed class RenderResult {
        data class Success(val filename: String, val durationMs: Long) : RenderResult()
        data class Error(val message: String) : RenderResult()
    }

    interface ProgressCallback {
        fun onProgress(progress: Float, message: String)
    }

    fun renderSongToWav(
        project: Project,
        progressCallback: ProgressCallback? = null
    ): RenderResult {
        // Silence the live stream so it cannot consume note queue entries during export
        audioBackend.setOfflineRendering(true)
        try {
            progressCallback?.onProgress(0f, "Analyzing song...")

            val (startRow, endRow) = findSongBounds(project)
            if (startRow < 0) return RenderResult.Error("Song is empty")

            // Prepare audio engine for offline rendering
            audioBackend.stopAll()
            audioBackend.clearScheduledNotes()
            audioBackend.resetFrameCounter()

            // Push all instrument params (sample points, filter, modulation)
            setupInstrumentParams(project, startRow, endRow)

            progressCallback?.onProgress(0.1f, "Scheduling notes...")

            // Schedule the entire song using PlaybackController's scheduling.
            // Groove, DEL, arpeggio, HOP, pitch effects all work automatically.
            val totalFrames = playbackController.scheduleSongForRender(project, startRow, endRow)

            if (totalFrames <= 0L) return RenderResult.Error("Song produced no audio")

            logger.d(TAG, "🎬 Scheduled $totalFrames frames (${totalFrames / audioBackend.getSampleRate()}s)")

            progressCallback?.onProgress(0.3f, "Rendering audio...")

            // Reset active bus effect for clean offline render.
            applyMasterBusForRender(project)

            val sampleRate = audioBackend.getSampleRate()
            val outputDir = fileSystem.getRendersDirectory()
            val filename  = generateFilename(project.name, outputDir)

            val success = renderToWavFile(
                totalFrames, sampleRate, filename,
                progressCallback, 0.3f, 0.98f, "Rendering audio..."
            )

            return if (success) {
                val durationMs = (totalFrames * 1000L) / sampleRate
                progressCallback?.onProgress(1f, "Done!")
                RenderResult.Success(filename, durationMs)
            } else {
                RenderResult.Error("Failed to write WAV file")
            }

        } catch (e: Exception) {
            logger.e(TAG, "❌ Render failed: ${e.message}")
            return RenderResult.Error(e.message ?: "Unknown error")
        } finally {
            audioBackend.stopAll()
            audioBackend.clearScheduledNotes()
            restoreMasterEq(project)
            audioBackend.setOfflineRendering(false)  // Always re-enable live playback
        }
    }

    /**
     * Render the selected song rows + tracks to a WAV in the Resampled directory.
     *
     * @param project         Project to render
     * @param startRow        First song row (inclusive)
     * @param endRow          Last song row (inclusive)
     * @param selectedTrackIds 0-indexed track IDs to include (0-7)
     * @param progressCallback Optional progress updates
     * @return RenderResult.Success with path in Samples/Resampled/, or Error
     */
    fun renderSelectionToWav(
        project: Project,
        startRow: Int,
        endRow: Int,
        selectedTrackIds: Set<Int>,
        progressCallback: ProgressCallback? = null,
        customBaseName: String? = null
    ): RenderResult {
        audioBackend.setOfflineRendering(true)
        try {
            progressCallback?.onProgress(0f, "Preparing selection...")

            audioBackend.stopAll()
            audioBackend.clearScheduledNotes()
            audioBackend.resetFrameCounter()

            setupInstrumentParams(project, startRow, endRow)

            progressCallback?.onProgress(0.1f, "Scheduling notes...")

            val totalFrames = playbackController.scheduleSelectionForRender(
                project, startRow, endRow, selectedTrackIds
            )

            if (totalFrames <= 0L) return RenderResult.Error("Selection produced no audio")

            logger.d(TAG, "🎬 Selection render: $totalFrames frames, tracks=$selectedTrackIds")

            progressCallback?.onProgress(0.3f, "Rendering audio...")

            // Honor the project's master bus (OTT *or* DUST) — was hardcoded to OTT, so a DUST project's
            // resample didn't match playback (REVIEW-3 2.2).
            applyMasterBusForRender(project)

            val sampleRate = audioBackend.getSampleRate()
            val outputDir = fileSystem.getResampledDirectory()
            val filename  = generateResampledFilename(outputDir, customBaseName)

            val success = renderToWavFile(
                totalFrames, sampleRate, filename,
                progressCallback, 0.3f, 0.98f, "Rendering audio..."
            )

            return if (success) {
                val durationMs = (totalFrames * 1000L) / sampleRate
                progressCallback?.onProgress(1f, "Done!")
                RenderResult.Success(filename, durationMs)
            } else {
                RenderResult.Error("Failed to write WAV file")
            }

        } catch (e: Exception) {
            logger.e(TAG, "❌ Selection render failed: ${e.message}")
            return RenderResult.Error(e.message ?: "Unknown error")
        } finally {
            audioBackend.stopAll()
            audioBackend.clearScheduledNotes()
            restoreMasterEq(project)
            audioBackend.setOfflineRendering(false)
        }
    }

    /**
     * Render each active track as a separate stereo WAV stem, plus reverb and delay send returns.
     *
     * Output folder: Renders/{project_name}/
     * Files: {name}_1..N (sequential, not track-index), {name}_reverb, {name}_delay
     *
     * Track stems: dry signal only (no OTT/DUST/masterEQ, limiter applied).
     * Send stems: all tracks feed their sends; OTT/DUST/masterEQ bypassed, limiter applied.
     */
    fun renderStemsToWav(
        project: Project,
        progressCallback: ProgressCallback? = null
    ): RenderResult {
        audioBackend.setOfflineRendering(true)
        try {
            progressCallback?.onProgress(0f, "Analyzing song...")

            val (startRow, endRow) = findSongBounds(project)
            if (startRow < 0) return RenderResult.Error("Song is empty")

            audioBackend.stopAll()
            audioBackend.clearScheduledNotes()

            setupInstrumentParams(project, startRow, endRow)
            audioBackend.setLimiterPreGain(project.limiterPreGain)

            val sampleRate = audioBackend.getSampleRate()

            // Active = non-muted and has at least one chain reference in the song
            val activeTracks = (0..7).filter { trackId ->
                val track = project.tracks.getOrNull(trackId) ?: return@filter false
                !track.mute && (0 until 256).any { row ->
                    row < track.chainRefs.size && track.chainRefs[row] in 0..255
                }
            }
            if (activeTracks.isEmpty()) return RenderResult.Error("No active tracks")

            // Collect instruments used in the song range to check send routing
            val usedInstrIds = project.collectUsedInstruments(startRow, endRow)
            val hasReverbSend = usedInstrIds.any { (project.instruments.getOrNull(it)?.reverbSend ?: 0) > 0 }
            val hasDelaySend  = usedInstrIds.any { (project.instruments.getOrNull(it)?.delaySend  ?: 0) > 0 }

            val safeProjectName = project.name
                .replace(Regex("[^a-zA-Z0-9_\\-]"), "_")
                .take(32)
                .ifEmpty { "project" }
            val rendersDir = fileSystem.getRendersDirectory()
            val stemDir = if (fileSystem.fileExists("$rendersDir/$safeProjectName")) {
                "$rendersDir/$safeProjectName"
            } else {
                fileSystem.createFolder(rendersDir, safeProjectName) ?: "$rendersDir/$safeProjectName"
            }

            val sendPasses = (if (hasReverbSend) 1 else 0) + (if (hasDelaySend) 1 else 0)
            val totalPasses = activeTracks.size + sendPasses
            var passIndex = 0

            for ((stemIdx, trackId) in activeTracks.withIndex()) {
                val label = "Rendering track ${stemIdx + 1}/${activeTracks.size}..."
                progressCallback?.onProgress(passIndex.toFloat() / totalPasses, label)

                audioBackend.stopAll()
                audioBackend.clearScheduledNotes()
                audioBackend.resetFrameCounter()

                val totalFrames = playbackController.scheduleSongForRender(project, startRow, endRow)
                if (totalFrames > 0L) {
                    audioBackend.setStemsMode(trackId + 1)
                    renderToWavFile(
                        totalFrames, sampleRate, "$stemDir/${safeProjectName}_${stemIdx + 1}.wav",
                        progressCallback,
                        passIndex.toFloat() / totalPasses, (passIndex + 1).toFloat() / totalPasses, label
                    )
                    audioBackend.setStemsMode(0)
                }
                passIndex++
            }

            // Reverb stem (only if instruments use reverb send)
            if (hasReverbSend) {
                val label = "Rendering reverb stem..."
                progressCallback?.onProgress(passIndex.toFloat() / totalPasses, label)
                audioBackend.stopAll()
                audioBackend.clearScheduledNotes()
                audioBackend.resetFrameCounter()
                val reverbFrames = playbackController.scheduleSongForRender(project, startRow, endRow)
                if (reverbFrames > 0L) {
                    audioBackend.setStemsMode(9)
                    renderToWavFile(
                        reverbFrames, sampleRate, "$stemDir/${safeProjectName}_reverb.wav",
                        progressCallback,
                        passIndex.toFloat() / totalPasses, (passIndex + 1).toFloat() / totalPasses, label
                    )
                    audioBackend.setStemsMode(0)
                }
                passIndex++
            }

            // Delay stem (only if instruments use delay send)
            if (hasDelaySend) {
                val label = "Rendering delay stem..."
                progressCallback?.onProgress(passIndex.toFloat() / totalPasses, label)
                audioBackend.stopAll()
                audioBackend.clearScheduledNotes()
                audioBackend.resetFrameCounter()
                val delayFrames = playbackController.scheduleSongForRender(project, startRow, endRow)
                if (delayFrames > 0L) {
                    audioBackend.setStemsMode(10)
                    renderToWavFile(
                        delayFrames, sampleRate, "$stemDir/${safeProjectName}_delay.wav",
                        progressCallback,
                        passIndex.toFloat() / totalPasses, (passIndex + 1).toFloat() / totalPasses, label
                    )
                    audioBackend.setStemsMode(0)
                }
            }

            progressCallback?.onProgress(1f, "Done!")
            return RenderResult.Success(stemDir, 0L)

        } catch (e: Exception) {
            logger.e(TAG, "❌ Stems render failed: ${e.message}")
            return RenderResult.Error(e.message ?: "Unknown error")
        } finally {
            audioBackend.setStemsMode(0)
            audioBackend.stopAll()
            audioBackend.clearScheduledNotes()
            restoreMasterEq(project)
            audioBackend.setOfflineRendering(false)
        }
    }

    // ─────────────────────────────────────────────────────────────────────────

    /**
     * Push the project's master-bus effect (OTT or DUST) + limiter for a clean offline render
     * (the no-warmup-fade *ForRender variants). Both the full-song and selection renders need this so
     * the export matches playback — the selection path previously hardcoded OTT and silently ignored a
     * DUST master bus (REVIEW-3 2.2). Stems bypass the master bus (setStemsMode), so they don't use it.
     */
    private fun applyMasterBusForRender(project: Project) {
        audioBackend.setMasterFx(project.masterBusFx)
        if (project.masterBusFx == 0)
            audioBackend.setOttDepthForRender(project.ottDepth)
        else
            audioBackend.setDustDepthForRender(project.dustDepth)
        audioBackend.setLimiterPreGain(project.limiterPreGain)
        // Start from the configured master EQ so an EQM effect in the song animates from the right
        // baseline (and a prior render's EQM override can't bleed into this one). restoreMasterEq()
        // in each finally returns the live bus to this slot after export.
        audioBackend.setMasterEqSlot(project.masterEqSlot)
    }

    /** Return the master bus EQ to the project's configured slot after a render (an EQM effect in the
     *  song mutates the global master EQ; without this the live bus would stay on the last EQM preset). */
    private fun restoreMasterEq(project: Project) {
        audioBackend.setMasterEqSlot(project.masterEqSlot)
    }

    private fun findSongBounds(project: Project): Pair<Int, Int> {
        var first = -1
        var last  = -1
        for (row in 0 until 256) {
            val hasContent = project.tracks.any { track ->
                row < track.chainRefs.size && track.chainRefs[row] in 0..255
            }
            if (hasContent) {
                if (first < 0) first = row
                last = row
            }
        }
        return Pair(first, last)
    }

    /**
     * Push sample-playback params and modulation slots for every instrument used
     * in the song range.  Must be called before [scheduleSongForRender].
     */
    private fun setupInstrumentParams(project: Project, startRow: Int, endRow: Int) {
        val usedInstruments = project.collectUsedInstruments(startRow, endRow)

        for (instId in usedInstruments) {
            val instrument = project.instruments.getOrNull(instId) ?: continue
            if (instrument.isSoundfont()) {
                // SF instrument: ensure instrumentParams[instId] has safe defaults so stale
                // WAV params from a previous render or project load don't bleed into SF output.
                // (isSoundfont(), not sampleFilePath == null — the latter is also true for empty
                // sampler slots, which belong on the sampler path below — REVIEW-3 3.3.)
                audioEngine.applySoundfontFilterOverrides(instrument)
                audioEngine.pushInstrumentModulation(instrument, project.tempo)
                audioEngine.pushInstrumentEqAndSends(instrument, project)
                continue
            }

            val loopModeInt = when (instrument.loopMode) { "fwd" -> 1; "png" -> 2; else -> 0 }
            val filterTypeInt = when (instrument.filterType) { "lp" -> 1; "hp" -> 2; "bp" -> 3; else -> 0 }

            audioBackend.setInstrumentParams(
                instrumentId = instrument.sampleId,
                startPoint   = instrument.sampleStart,
                endPoint      = instrument.sampleEnd,
                reverse       = instrument.reverse,
                loopMode      = loopModeInt,
                loopStart     = instrument.loopStart,
                loopEnd       = instrument.loopEnd,
                drive         = instrument.drive,
                crush         = instrument.crush,
                downsample    = instrument.downsample,
                filterType    = filterTypeInt,
                filterCut     = instrument.filterCut,
                filterRes     = instrument.filterRes
            )

            audioEngine.pushInstrumentModulation(instrument, project.tempo)
            audioEngine.pushInstrumentEqAndSends(instrument, project)
        }
    }

    private fun generateFilename(projectName: String, outputDir: String): String {
        val base = projectName.replace(Regex("[^a-zA-Z0-9_\\-]"), "_").take(32)
        var index = 1
        var filename: String
        do {
            filename = "$outputDir/${base}_${index.toString().padStart(4, '0')}.wav"
            index++
        } while (fileSystem.fileExists(filename) && index < 10000)
        return filename
    }

    /**
     * Generate the auto-suggested base name for a resampled file (e.g. "Resample_0001").
     * Call this before opening the QWERTY keyboard so the user sees the suggested name.
     */
    fun generateResampledBaseName(): String {
        val outputDir = fileSystem.getResampledDirectory()
        var index = 1
        var baseName: String
        do {
            baseName = "Resample_${index.toString().padStart(4, '0')}"
            index++
        } while (fileSystem.fileExists("$outputDir/$baseName.wav") && index < 10000)
        return baseName
    }

    private fun generateResampledFilename(outputDir: String, customBaseName: String? = null): String {
        if (customBaseName != null) {
            val safe = customBaseName.replace(Regex("[^a-zA-Z0-9_\\-]"), "_").take(32)
            return "$outputDir/$safe.wav"
        }
        var index = 1
        var filename: String
        do {
            filename = "$outputDir/Resample_${index.toString().padStart(4, '0')}.wav"
            index++
        } while (fileSystem.fileExists(filename) && index < 10000)
        return filename
    }
}
