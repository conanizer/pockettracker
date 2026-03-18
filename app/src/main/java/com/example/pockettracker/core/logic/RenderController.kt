package com.example.pockettracker.core.logic

import android.util.Log
import com.example.pockettracker.core.audio.AudioEngine
import com.example.pockettracker.core.data.Project
import com.example.pockettracker.core.storage.IFileSystem
import com.example.pockettracker.core.storage.WavWriter

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
 *   4. renderFrames(N) → C++ processes the queue offline and returns audio
 *   5. Write 16-bit stereo WAV
 */
class RenderController(
    private val audioEngine: AudioEngine,
    private val playbackController: PlaybackController,
    private val fileSystem: IFileSystem
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

            Log.d(TAG, "🎬 Scheduled $totalFrames frames (${totalFrames / audioBackend.getSampleRate()}s)")

            progressCallback?.onProgress(0.3f, "Rendering audio...")

            val sampleRate = audioBackend.getSampleRate()
            val audio = audioBackend.renderFrames(totalFrames.toInt(), sampleRate)

            progressCallback?.onProgress(0.85f, "Writing WAV file...")

            val n = totalFrames.toInt()
            val leftChannel  = FloatArray(n) { audio[it * 2] }
            val rightChannel = FloatArray(n) { audio[it * 2 + 1] }

            val outputDir = fileSystem.getRendersDirectory()
            val filename  = generateFilename(project.name, outputDir)

            val success = WavWriter.writeWav(
                fileSystem    = fileSystem,
                path          = filename,
                leftChannel   = leftChannel,
                rightChannel  = rightChannel,
                sampleRate    = sampleRate
            )

            return if (success) {
                val durationMs = (n.toLong() * 1000L) / sampleRate
                progressCallback?.onProgress(1f, "Done!")
                RenderResult.Success(filename, durationMs)
            } else {
                RenderResult.Error("Failed to write WAV file")
            }

        } catch (e: Exception) {
            Log.e(TAG, "❌ Render failed: ${e.message}", e)
            return RenderResult.Error(e.message ?: "Unknown error")
        } finally {
            audioBackend.stopAll()
            audioBackend.clearScheduledNotes()
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

            Log.d(TAG, "🎬 Selection render: $totalFrames frames, tracks=$selectedTrackIds")

            progressCallback?.onProgress(0.3f, "Rendering audio...")

            val sampleRate = audioBackend.getSampleRate()
            val audio = audioBackend.renderFrames(totalFrames.toInt(), sampleRate)

            progressCallback?.onProgress(0.85f, "Writing WAV file...")

            val n = totalFrames.toInt()
            val leftChannel  = FloatArray(n) { audio[it * 2] }
            val rightChannel = FloatArray(n) { audio[it * 2 + 1] }

            val outputDir = fileSystem.getResampledDirectory()
            val filename  = generateResampledFilename(outputDir, customBaseName)

            val success = WavWriter.writeWav(
                fileSystem   = fileSystem,
                path         = filename,
                leftChannel  = leftChannel,
                rightChannel = rightChannel,
                sampleRate   = sampleRate
            )

            return if (success) {
                val durationMs = (n.toLong() * 1000L) / sampleRate
                progressCallback?.onProgress(1f, "Done!")
                RenderResult.Success(filename, durationMs)
            } else {
                RenderResult.Error("Failed to write WAV file")
            }

        } catch (e: Exception) {
            Log.e(TAG, "❌ Selection render failed: ${e.message}", e)
            return RenderResult.Error(e.message ?: "Unknown error")
        } finally {
            audioBackend.stopAll()
            audioBackend.clearScheduledNotes()
            audioBackend.setOfflineRendering(false)
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

    /**
     * Push sample-playback params and modulation slots for every instrument used
     * in the song range.  Must be called before [scheduleSongForRender].
     */
    private fun setupInstrumentParams(project: Project, startRow: Int, endRow: Int) {
        val usedInstruments = mutableSetOf<Int>()

        for (row in startRow..endRow) {
            for (track in project.tracks) {
                if (track.mute) continue
                if (row >= track.chainRefs.size) continue
                val chainId = track.chainRefs[row]
                if (chainId !in 0..255) continue
                val chain = project.chains[chainId]
                for (slot in 0..15) {
                    val phraseId = chain.phraseRefs[slot]
                    if (phraseId !in 0..255) continue
                    for (step in project.phrases[phraseId].steps) {
                        if (!step.isEmpty() && step.instrument in 0..255)
                            usedInstruments.add(step.instrument)
                    }
                }
            }
        }

        for (instId in usedInstruments) {
            val instrument = project.instruments.getOrNull(instId) ?: continue
            if (instrument.sampleFilePath == null) continue

            val loopModeInt = when (instrument.loopMode) { "fwd" -> 1; "png" -> 2; else -> 0 }
            val filterTypeInt = when (instrument.filterType) { "lp" -> 1; "hp" -> 2; "bp" -> 3; else -> 0 }

            audioBackend.setInstrumentParams(
                instrumentId = instrument.sampleId,
                startPoint   = instrument.sampleStart,
                endPoint      = instrument.sampleEnd,
                reverse       = instrument.reverse,
                loopMode      = loopModeInt,
                loopStart     = instrument.loopStart,
                drive         = instrument.drive,
                crush         = instrument.crush,
                downsample    = instrument.downsample,
                filterType    = filterTypeInt,
                filterCut     = instrument.filterCut,
                filterRes     = instrument.filterRes
            )

            audioEngine.pushInstrumentModulation(instrument, project.tempo)
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
