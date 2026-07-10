package com.conanizer.pockettracker.trace

import com.conanizer.pockettracker.core.audio.IAudioBackend
import com.conanizer.pockettracker.core.logging.ILogger
import com.conanizer.pockettracker.core.logic.StateObserver
import com.conanizer.pockettracker.core.resources.IResourceLoader
import com.conanizer.pockettracker.core.resources.SampleData
import com.conanizer.pockettracker.core.storage.FileInfo
import com.conanizer.pockettracker.core.storage.FileSortMode
import com.conanizer.pockettracker.core.storage.IFileSystem
import java.io.File

/**
 * Fakes for the golden-trace harness. The EventTrace tap sits ABOVE the backend, so the fake
 * absorbs every engine call; the only state that matters is the controllable frame clock and the
 * configurable sample rate that drive PlaybackController's scheduling arithmetic.
 */
class FakeAudioBackend(private val sampleRate: Int) : IAudioBackend {
    /** The synthetic audio clock — the harness advances this and polls updatePlaybackBuffer(). */
    var frameClock: Long = 0L

    override fun create() = true
    override fun getCurrentFrame() = frameClock
    override fun getSampleRate() = sampleRate

    // Sample management — inert
    override fun loadSample(id: Int, samples: FloatArray) {}
    override fun loadSampleStereo(id: Int, left: FloatArray, right: FloatArray) {}
    override fun beginSampleLoad(id: Int, channels: Int, estimatedFrames: Int) = true
    override fun fillSampleChunk(id: Int, interleaved: ShortArray, frameCount: Int, channels: Int) {}
    override fun finalizeSampleLoad(id: Int) = 0
    override fun cancelSampleLoad(id: Int) {}
    override fun loadSampleFromWav(id: Int, path: String) = 0
    override fun loadSampleFromCompressed(id: Int, path: String) = 0
    override fun hasStereoData(id: Int) = false
    override fun clearAllSamples() {}
    override fun clearSample(id: Int) {}
    override fun freeSampleUndo(id: Int) {}
    override fun getSampleLength(id: Int) = 44100
    override fun getSampleWaveform(id: Int, numBins: Int) = FloatArray(numBins)
    override fun getSampleWaveformRange(id: Int, startFrame: Int, endFrame: Int, numBins: Int) = FloatArray(numBins)
    override fun getSampleWaveformRangeSource(id: Int, startFrame: Int, endFrame: Int, numBins: Int, channel: Int) = FloatArray(numBins)
    override fun getSampleData(id: Int) = FloatArray(0)
    override fun getSampleDataRight(id: Int) = FloatArray(0)
    override fun normalizeSample(id: Int, startFrame: Int, endFrame: Int) {}
    override fun fadeInSample(id: Int, startFrame: Int, endFrame: Int) {}
    override fun fadeOutSample(id: Int, startFrame: Int, endFrame: Int) {}
    override fun silenceRegion(id: Int, startFrame: Int, endFrame: Int) {}
    override fun reverseSample(id: Int, startFrame: Int, endFrame: Int) {}
    override fun backupSample(id: Int) {}
    override fun undoSample(id: Int) {}
    override fun saveFxPreviewBackup(id: Int) {}
    override fun restoreFxPreviewBackup() {}
    override fun getSamplePlaybackPosition(id: Int) = 0f
    override fun cropSample(id: Int, startFrame: Int, endFrame: Int) {}
    override fun deleteSampleRegion(id: Int, startFrame: Int, endFrame: Int) {}
    override fun copyRegion(id: Int, startFrame: Int, endFrame: Int) {}
    override fun pasteRegion(id: Int, insertAt: Int) {}
    override fun prepareSourcePreview(dstId: Int, srcId: Int, mode: Int) {}
    override fun getClipboardLength() = 0
    override fun downsampleSample(id: Int, factor: Int) {}
    override fun applyRateMode(id: Int, factor: Int) {}
    override fun pitchShiftSample(id: Int, semitones: Float) {}
    override fun timeStretchSample(id: Int, ratio: Float) {}
    override fun applySampleFx(id: Int, fxType: Int, fxValue: Int, sampleRate: Float, limiterPreGain: Int) {}
    override fun findZeroCrossing(id: Int, frame: Int, dir: Int) = frame
    override fun detectTransients(id: Int, sensitivity: Int) = IntArray(0)

    // Scheduling — inert (the tap has already recorded the event above this seam)
    override fun getTrackActiveNotes() = IntArray(8) { -1 }
    override fun scheduleNote(frame: Long, sampleId: Int, trackId: Int, freq: Float, baseFreq: Float, vol: Float, pan: Float, startPointOverride: Int) {}
    override fun clearScheduledNotes() {}
    override fun clearScheduledNotesFrom(fromFrame: Long) {}
    override fun resumeStream() {}
    override fun stopAll() {}
    override fun killTrack(trackId: Int) {}
    override fun scheduleKill(frame: Long, trackId: Int) {}
    override fun loadTable(tableId: Int, rowData: ByteArray) {}
    override fun scheduleNoteWithTable(frame: Long, sampleId: Int, trackId: Int, freq: Float, baseFreq: Float, vol: Float, phraseVol: Float, pan: Float, startPointOverride: Int, endPointOverride: Int, tableId: Int, tableTicRate: Int, noteOctave: Int, notePitch: Int, pslInitialOffset: Float, pslDuration: Float, pbnRate: Float, vibratoSpeed: Float, vibratoDepth: Float, tableStartRow: Int) {}
    override fun getVoiceTableRow(trackId: Int) = -1
    override fun getVoiceTableId(trackId: Int) = -1
    override fun scheduleVoiceTableRow(targetFrame: Long, trackId: Int, row: Int) {}
    override fun scheduleTrackPhraseVol(targetFrame: Long, trackId: Int, phraseVol: Float) {}
    override fun scheduleVoicePan(targetFrame: Long, trackId: Int, pan: Float) {}
    override fun scheduleVoiceReverbSend(targetFrame: Long, trackId: Int, send: Float) {}
    override fun scheduleVoiceDelaySend(targetFrame: Long, trackId: Int, send: Float) {}
    override fun scheduleVoiceReverse(targetFrame: Long, trackId: Int, reverse: Boolean, restart: Boolean) {}
    override fun scheduleVoiceEqSlot(targetFrame: Long, trackId: Int, slot: Int) {}
    override fun scheduleMasterEqSlotAt(targetFrame: Long, slot: Int) {}
    override fun schedulePitchBend(targetFrame: Long, trackId: Int, semitonesPerTick: Float, tempo: Int) {}
    override fun scheduleVibrato(targetFrame: Long, trackId: Int, speed: Float, depth: Float) {}
    override fun setInstrumentModulation(sampleId: Int, slotIndex: Int, type: Int, dest: Int, amount: Float, attackSamples: Int, holdSamples: Int, decaySamples: Int, sustainLevel: Float, lfoHz: Float, oscShape: Int, releaseSamples: Int, lfoTrigMode: Int) {}
    override fun clearInstrumentModulation(sampleId: Int) {}
    override fun triggerNoteOff(trackId: Int) {}
    override fun scheduleNoteOff(frame: Long, trackId: Int) {}
    override fun setOfflineRendering(rendering: Boolean) {}
    override fun setTempo(tempo: Int) {}

    // Stream/mixer/EQ/sends — inert
    override fun updateWaveform(buffer: FloatArray) {}
    override fun getTrackWaveforms(buffer: FloatArray, activeTracks: BooleanArray) {}
    override fun setInstrumentParams(instrumentId: Int, startPoint: Int, endPoint: Int, reverse: Boolean, loopMode: Int, loopStart: Int, loopEnd: Int, drive: Int, crush: Int, downsample: Int, filterType: Int, filterCut: Int, filterRes: Int) {}
    override fun close() {}
    override fun getTrackPeaks(buffer: FloatArray) {}
    override fun getMasterPeaks(buffer: FloatArray) {}
    override fun getSendPeaks(buffer: FloatArray) {}
    override fun renderFrames(numFrames: Int, sampleRate: Int) = FloatArray(numFrames * 2)
    override fun resetFrameCounter() {}
    override fun getFrameCounter() = 0L
    override fun decayPeaks() {}
    override fun decayWaveform() {}
    override fun getSpectrumMagnitudes(numBins: Int) = FloatArray(numBins)
    override fun getSpectrumMagnitudesForSource(source: Int, instrId: Int, numBins: Int) = FloatArray(numBins)
    override fun setTrackVolume(trackId: Int, volume: Float) {}
    override fun setMasterVolume(volume: Float) {}
    override fun setOttDepth(depth: Int) {}
    override fun setOttDepthForRender(depth: Int) {}
    override fun setMasterFx(fx: Int) {}
    override fun setDustDepth(depth: Int) {}
    override fun setDustDepthForRender(depth: Int) {}
    override fun setLimiterPreGain(depth: Int) {}
    override fun setStemsMode(mode: Int) {}
    override fun setEqBand(slot: Int, band: Int, type: Int, freqHex: Int, gainHex: Int, qHex: Int) {}
    override fun setInstrumentEqSlot(instrId: Int, slot: Int) {}
    override fun setInstrumentSendLevels(instrId: Int, reverbSend: Int, delaySend: Int) {}
    override fun setDelayReverbSend(sendHex: Int) {}
    override fun setReverbParams(feedbackHex: Int, dampHex: Int, wetHex: Int) {}
    override fun setDelayParams(timeOrSubdiv: Int, feedbackHex: Int, syncMode: Boolean, bpm: Float, wetHex: Int) {}
    override fun setReverbInputEq(slot: Int) {}
    override fun setDelayInputEq(slot: Int) {}
    override fun setMasterEqSlot(slot: Int) {}

    // SoundFont — no slots in the harness: SF NoteOns are traced at the seam and dropped here
    override fun loadSoundfont(instrumentId: Int, filePath: String) = -1
    override fun setSoundfontPreset(sfSlot: Int, bank: Int, preset: Int) {}
    override fun scheduleSoundfontNote(frame: Long, trackId: Int, sfSlot: Int, midiNote: Int, velocity: Int, vol: Float, pan: Float, bank: Int, preset: Int, pslInitialOffset: Float, pslDuration: Float, pbnRate: Float, vibratoSpeed: Float, vibratoDepth: Float, phraseVol: Float, sampleId: Int, tableId: Int, tableTicRate: Int, noteOctave: Int, notePitch: Int, tableStartRow: Int, detuneSemitones: Float) {}
    override fun setSoundfontEnvelopeOverrides(instrumentId: Int, atk: Int, dec: Int, sus: Int, rel: Int) {}
    override fun setSoundfontFilterOverrides(sampleId: Int, filterType: Int, filterCut: Int, filterRes: Int) {}
    override fun unloadSoundfont(sfSlot: Int) {}
    override fun clearAllSoundfonts() {}
    override fun getSoundfontPresetName(sfSlot: Int, bank: Int, preset: Int) = "---"
    override fun getSoundfontFirstBankPreset(sfSlot: Int) = intArrayOf(-1, -1)
    override fun getSoundfontPresetCount(sfSlot: Int) = 0
    override fun getSoundfontPresetAt(sfSlot: Int, index: Int) = intArrayOf(-1, -1)
}

object FakeLogger : ILogger {
    override fun d(tag: String, message: String) {}
    override fun i(tag: String, message: String) {}
    override fun w(tag: String, message: String) {}
    override fun e(tag: String, message: String) {}
    override fun e(tag: String, message: String, throwable: Throwable) {}
}

object FakeResourceLoader : IResourceLoader {
    override fun loadWav(name: String) = SampleData(FloatArray(0), 44100, 1)
}

object FakeStateObserver : StateObserver {
    override fun onStateChanged() {}
}

/**
 * Minimal java.io-backed IFileSystem rooted at one directory — lets the harness reuse
 * FileController's exact Json configuration (and its .ptp naming) for the golden project files.
 */
class JvmFileSystem(private val root: File) : IFileSystem {
    private fun dir(name: String): String {
        val d = File(root, name); d.mkdirs(); return d.absolutePath
    }

    override fun getProjectsDirectory() = dir(".")
    override fun getSamplesDirectory() = dir("samples")
    override fun getInstrumentsDirectory() = dir("instruments")
    override fun getSoundfontsDirectory() = dir("soundfonts")
    override fun getThemesDirectory() = dir("themes")
    override fun getRendersDirectory() = dir("renders")
    override fun getResampledDirectory() = dir("resampled")
    override fun getTemplateProjectPath() = File(root, "template.ptp").absolutePath
    override fun getAutosaveFilePath() = File(root, "autosave.ptp").absolutePath

    override fun readFile(path: String) = File(path).readText(Charsets.UTF_8)
    override fun writeFile(path: String, content: String): Boolean {
        File(path).parentFile?.mkdirs()
        // Match AndroidFileSystem's newline behavior: write content verbatim, UTF-8.
        File(path).writeText(content, Charsets.UTF_8)
        return true
    }

    override fun writeBytes(path: String, data: ByteArray): Boolean {
        File(path).parentFile?.mkdirs(); File(path).writeBytes(data); return true
    }

    override fun deleteFile(path: String) = File(path).delete()
    override fun deleteFileOrFolder(path: String) = File(path).deleteRecursively()
    override fun renameFile(oldPath: String, newName: String): Boolean {
        val f = File(oldPath); return f.renameTo(File(f.parentFile, newName))
    }

    override fun fileExists(path: String) = File(path).exists()
    override fun createFolder(parentPath: String, folderName: String): String? {
        val f = File(parentPath, folderName); return if (f.mkdirs() || f.isDirectory) f.absolutePath else null
    }

    override fun listFiles(directoryPath: String, extension: String?): List<FileInfo> =
        (File(directoryPath).listFiles() ?: emptyArray())
            .filter { extension == null || it.extension.equals(extension, ignoreCase = true) }
            .map { FileInfo(it.absolutePath, it.name, it.extension, it.isDirectory, it.length(), it.lastModified()) }

    override fun sortFiles(files: List<FileInfo>, sortMode: FileSortMode) = files
    override fun hasStoragePermission() = true
    override fun getParentPath(path: String): String? = File(path).parent
    override fun moveFile(sourcePath: String, destPath: String): Boolean {
        File(destPath).parentFile?.mkdirs(); return File(sourcePath).renameTo(File(destPath))
    }
}
