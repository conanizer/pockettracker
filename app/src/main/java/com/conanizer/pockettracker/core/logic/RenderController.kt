package com.conanizer.pockettracker.core.logic

import com.conanizer.pockettracker.core.audio.AudioEngine
import com.conanizer.pockettracker.core.audio.ISongcore
import com.conanizer.pockettracker.core.data.Project
import com.conanizer.pockettracker.core.logging.ILogger
import com.conanizer.pockettracker.core.storage.IFileSystem

/**
 * RENDER CONTROLLER — the *policy* half of an offline render.
 *
 * Since songcore S6b this class decides only what a render IS: which rows, which tracks, where the
 * file goes, what the user sees. Everything that touches audio — readying the engine, pushing the
 * project into it, the chunked render loop, the decay tail, the WAV writer — lives in C++
 * (`native/songcore/render.h` + `engine_setup.h`) behind [ISongcore]'s three render verbs. The Linux
 * shell calls that same code; nothing about how a render *sounds* is written twice.
 *
 * Scheduling stays outside those verbs on purpose: [PlaybackController] may fill the note queue with
 * either sequencer (SETTINGS → ENG), and rendering the same project on both must produce byte-identical
 * WAVs. That check only means something while the sequencer is the *only* thing that differs.
 *
 *   1. Find the song bounds (first / last used row)
 *   2. songcore.prepareRender  — silence the live stream, reset the engine + effect chains, push the project
 *   3. PlaybackController      — schedule the notes at frames 0..N (Kotlin or C++ sequencer)
 *   4. songcore.renderToWav    — render the span + its decay tail, streaming to a 16-bit stereo WAV
 *   5. songcore.finishRender   — restore the engine for live playback (always, even on failure)
 */
class RenderController(
    private val audioEngine: AudioEngine,
    private val playbackController: PlaybackController,
    private val songcore: ISongcore,
    private val fileSystem: IFileSystem,
    private val logger: ILogger
) {
    private val audioBackend get() = audioEngine.backend

    companion object {
        private const val TAG = "RenderController"
    }

    sealed class RenderResult {
        data class Success(val filename: String, val durationMs: Long) : RenderResult()
        data class Error(val message: String) : RenderResult()
    }

    interface ProgressCallback {
        fun onProgress(progress: Float, message: String)
    }

    /** Map songcore's 0..1 render progress onto a slice of this render's overall progress bar. */
    private fun progressSlice(
        callback: ProgressCallback?,
        from: Float,
        to: Float,
        label: String
    ): ISongcore.RenderProgress? =
        callback?.let { cb -> ISongcore.RenderProgress { f -> cb.onProgress(from + (to - from) * f, label) } }

    /**
     * Hand songcore the project, then ready the engine for a render of [startRow]..[endRow].
     *
     * The push is unconditional — songcore holds a *copy* of the project and `prepareRender` pushes
     * that copy's instruments, mixer and FX into the engine, so it must be current whichever sequencer
     * is about to schedule. (In C++ mode `scheduleSongForRender` pushes again; a second parse of the
     * blob costs nothing next to a render.)
     */
    private fun prepare(project: Project, startRow: Int, endRow: Int) {
        playbackController.songcorePushProject(project)
        songcore.prepareRender(startRow, endRow)
    }

    /**
     * `finishRender()` stops voices, clears the note queue and restores the master EQ — so it must run
     * only when a render actually readied the engine. Each render below tracks that with a `prepared`
     * flag: a path that bails first (empty song, no active tracks) has touched nothing, and tearing the
     * engine down anyway would kill whatever was playing at the time for a render that never started.
     */

    fun renderSongToWav(
        project: Project,
        progressCallback: ProgressCallback? = null
    ): RenderResult {
        var prepared = false
        try {
            progressCallback?.onProgress(0f, "Analyzing song...")

            val (startRow, endRow) = findSongBounds(project)
            if (startRow < 0) return RenderResult.Error("Song is empty")

            prepare(project, startRow, endRow)
            prepared = true

            progressCallback?.onProgress(0.1f, "Scheduling notes...")

            // Groove, DEL, arpeggio, HOP and the pitch effects all come for free: this is the same
            // scheduling code live playback runs, not a second copy of it.
            val songFrames = playbackController.scheduleSongForRender(project, startRow, endRow)
            if (songFrames <= 0L) return RenderResult.Error("Song produced no audio")

            logger.d(TAG, "🎬 Scheduled $songFrames frames (${songFrames / audioBackend.getSampleRate()}s)")

            progressCallback?.onProgress(0.3f, "Rendering audio...")

            val sampleRate = audioBackend.getSampleRate()
            val outputDir  = fileSystem.getRendersDirectory()
            val filename   = generateFilename(project.name, outputDir)

            // The file is longer than songFrames: the render now runs on past the last step until the
            // reverb tail, delay repeats and note releases have died away.
            val framesWritten = songcore.renderToWav(
                filename, songFrames,
                progress = progressSlice(progressCallback, 0.3f, 0.98f, "Rendering audio...")
            )
            if (framesWritten <= 0L) return RenderResult.Error("Failed to write WAV file")

            progressCallback?.onProgress(1f, "Done!")
            return RenderResult.Success(filename, (framesWritten * 1000L) / sampleRate)

        } catch (e: Exception) {
            logger.e(TAG, "❌ Render failed: ${e.message}")
            return RenderResult.Error(e.message ?: "Unknown error")
        } finally {
            if (prepared) songcore.finishRender()
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
        var prepared = false
        try {
            progressCallback?.onProgress(0f, "Preparing selection...")

            prepare(project, startRow, endRow)
            prepared = true

            progressCallback?.onProgress(0.1f, "Scheduling notes...")

            val songFrames = playbackController.scheduleSelectionForRender(
                project, startRow, endRow, selectedTrackIds
            )
            if (songFrames <= 0L) return RenderResult.Error("Selection produced no audio")

            logger.d(TAG, "🎬 Selection render: $songFrames frames, tracks=$selectedTrackIds")

            progressCallback?.onProgress(0.3f, "Rendering audio...")

            val sampleRate = audioBackend.getSampleRate()
            val outputDir  = fileSystem.getResampledDirectory()
            val filename   = generateResampledFilename(outputDir, customBaseName)

            val framesWritten = songcore.renderToWav(
                filename, songFrames,
                progress = progressSlice(progressCallback, 0.3f, 0.98f, "Rendering audio...")
            )
            if (framesWritten <= 0L) return RenderResult.Error("Failed to write WAV file")

            progressCallback?.onProgress(1f, "Done!")
            return RenderResult.Success(filename, (framesWritten * 1000L) / sampleRate)

        } catch (e: Exception) {
            logger.e(TAG, "❌ Selection render failed: ${e.message}")
            return RenderResult.Error(e.message ?: "Unknown error")
        } finally {
            if (prepared) songcore.finishRender()
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
     *
     * Every pass re-prepares the engine, so a stem can no longer begin inside the previous stem's
     * reverb tail — and each one runs on past its last step until its own tail has decayed.
     */
    fun renderStemsToWav(
        project: Project,
        progressCallback: ProgressCallback? = null
    ): RenderResult {
        var prepared = false
        try {
            progressCallback?.onProgress(0f, "Analyzing song...")

            val (startRow, endRow) = findSongBounds(project)
            if (startRow < 0) return RenderResult.Error("Song is empty")

            // Silences the live stream before anything else, as the single-file renders do.
            prepare(project, startRow, endRow)
            prepared = true

            // Active = non-muted and has at least one chain reference in the song
            val activeTracks = (0..7).filter { trackId ->
                val track = project.tracks.getOrNull(trackId) ?: return@filter false
                !track.mute && (0 until 256).any { row ->
                    row < track.chainRefs.size && track.chainRefs[row] in 0..255
                }
            }
            if (activeTracks.isEmpty()) return RenderResult.Error("No active tracks")

            // Collect instruments used in the song range to check send routing
            val usedInstrIds  = project.collectUsedInstruments(startRow, endRow)
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

            val sendPasses  = (if (hasReverbSend) 1 else 0) + (if (hasDelaySend) 1 else 0)
            val totalPasses = activeTracks.size + sendPasses
            var passIndex   = 0

            // One stem pass. stemsMode: 1-8 = track N, 9 = reverb return, 10 = delay return.
            // Stems bypass the master bus (OTT/DUST/master EQ) by design, so they don't apply it.
            //
            // prepareRender per pass, not once: it is what wipes the effect chains, and without it
            // each stem would begin inside the *previous* stem's reverb tail. (The project itself was
            // pushed above and hasn't changed, so it isn't re-pushed here.)
            fun renderStem(stemsMode: Int, path: String, label: String) {
                progressCallback?.onProgress(passIndex.toFloat() / totalPasses, label)

                songcore.prepareRender(startRow, endRow)
                val songFrames = playbackController.scheduleSongForRender(project, startRow, endRow)
                if (songFrames > 0L) {
                    songcore.renderToWav(
                        path, songFrames, stemsMode = stemsMode, applyMasterBus = false,
                        progress = progressSlice(
                            progressCallback,
                            passIndex.toFloat() / totalPasses,
                            (passIndex + 1).toFloat() / totalPasses,
                            label
                        )
                    )
                }
                passIndex++
            }

            for ((stemIdx, trackId) in activeTracks.withIndex()) {
                renderStem(
                    stemsMode = trackId + 1,
                    path      = "$stemDir/${safeProjectName}_${stemIdx + 1}.wav",
                    label     = "Rendering track ${stemIdx + 1}/${activeTracks.size}..."
                )
            }
            if (hasReverbSend) {
                renderStem(9,  "$stemDir/${safeProjectName}_reverb.wav", "Rendering reverb stem...")
            }
            if (hasDelaySend) {
                renderStem(10, "$stemDir/${safeProjectName}_delay.wav",  "Rendering delay stem...")
            }

            progressCallback?.onProgress(1f, "Done!")
            return RenderResult.Success(stemDir, 0L)

        } catch (e: Exception) {
            logger.e(TAG, "❌ Stems render failed: ${e.message}")
            return RenderResult.Error(e.message ?: "Unknown error")
        } finally {
            if (prepared) songcore.finishRender()
        }
    }

    // ─────────────────────────────────────────────────────────────────────────

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
