package com.conanizer.pockettracker.platform.android

import com.conanizer.pockettracker.core.audio.IAudioBackend
import android.util.Log

/**
 * Android implementation of IAudioBackend using Oboe library.
 *
 * This class wraps the native C++ audio engine (native-audio.cpp) via JNI.
 * All actual audio processing happens in C++ for maximum performance and low latency.
 *
 * Architecture:
 * - Kotlin (this class) → JNI → C++ Oboe audio engine
 * - C++ handles: Sample-accurate queue, 8-voice polyphony, filters, effects
 * - Kotlin handles: High-level API, sample loading, scheduling
 *
 * Thread Safety:
 * - All methods are thread-safe (C++ uses mutex protection)
 * - Safe to call from UI thread or background threads
 *
 * Performance:
 * - LowLatency + Exclusive mode for minimal latency (<50ms on most devices)
 * - Sample-accurate timing (<0.02ms jitter)
 * - 44.1kHz or 48kHz (automatically detected)
 */
class OboeAudioBackend : IAudioBackend {

    private val TAG = "OboeAudioBackend"

    init {
        // Load native library (C++ audio engine with Oboe)
        try {
            System.loadLibrary("pockettracker")
            Log.d(TAG, "✅ Native library loaded successfully")
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "❌ Failed to load native library: ${e.message}")
            throw e
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // IAudioBackend Implementation
    // ═══════════════════════════════════════════════════════════════════════════

    override fun create(): Boolean {
        val success = native_create()
        if (success) {
            Log.d(TAG, "✅ Audio stream created successfully")
        } else {
            Log.e(TAG, "❌ Failed to create audio stream")
        }
        return success
    }

    override fun loadSample(id: Int, samples: FloatArray) {
        native_loadSample(id, samples)
        Log.d(TAG, "📦 Loaded sample $id (${samples.size} samples)")
    }

    override fun loadSampleStereo(id: Int, left: FloatArray, right: FloatArray) {
        native_loadSampleStereo(id, left, right)
        Log.d(TAG, "📦 Loaded stereo sample $id (${left.size} frames)")
    }

    override fun loadSampleFromWav(id: Int, path: String): Int {
        val rate = native_loadSampleFromWav(id, path)
        Log.d(TAG, "📦 Loaded WAV sample $id from $path (rate=$rate)")
        return rate
    }

    override fun hasStereoData(id: Int): Boolean = native_hasStereoData(id)

    override fun clearAllSamples() {
        native_clearAllSamples()
        Log.d(TAG, "🗑️ Cleared all loaded samples")
    }

    override fun clearSample(id: Int) {
        native_clearSample(id)
    }

    override fun freeSampleUndo(id: Int) = native_freeSampleUndo(id)

    override fun getSampleLength(id: Int): Int = native_getSampleLength(id)
    override fun getSampleWaveform(id: Int, numBins: Int): FloatArray = native_getSampleWaveform(id, numBins)
    override fun getSampleWaveformRange(id: Int, startFrame: Int, endFrame: Int, numBins: Int): FloatArray = native_getSampleWaveformRange(id, startFrame, endFrame, numBins)
    override fun getSampleWaveformRangeSource(id: Int, startFrame: Int, endFrame: Int, numBins: Int, channel: Int): FloatArray = native_getSampleWaveformRangeSource(id, startFrame, endFrame, numBins, channel)
    override fun getSampleData(id: Int): FloatArray = native_getSampleData(id)
    override fun getSampleDataRight(id: Int): FloatArray = native_getSampleDataRight(id)
    override fun normalizeSample(id: Int, startFrame: Int, endFrame: Int) = native_normalizeSample(id, startFrame, endFrame)
    override fun fadeInSample(id: Int, startFrame: Int, endFrame: Int) = native_fadeInSample(id, startFrame, endFrame)
    override fun fadeOutSample(id: Int, startFrame: Int, endFrame: Int) = native_fadeOutSample(id, startFrame, endFrame)
    override fun silenceRegion(id: Int, startFrame: Int, endFrame: Int) = native_silenceRegion(id, startFrame, endFrame)
    override fun reverseSample(id: Int, startFrame: Int, endFrame: Int) = native_reverseSample(id, startFrame, endFrame)
    override fun backupSample(id: Int) = native_backupSample(id)
    override fun undoSample(id: Int) = native_undoSample(id)
    override fun saveFxPreviewBackup(id: Int) = native_saveFxPreviewBackup(id)
    override fun restoreFxPreviewBackup() = native_restoreFxPreviewBackup()
    override fun getSamplePlaybackPosition(id: Int): Float = native_getSamplePlaybackPosition(id)
    override fun cropSample(id: Int, startFrame: Int, endFrame: Int) = native_cropSample(id, startFrame, endFrame)
    override fun deleteSampleRegion(id: Int, startFrame: Int, endFrame: Int) = native_deleteSampleRegion(id, startFrame, endFrame)
    override fun copyRegion(id: Int, startFrame: Int, endFrame: Int) = native_copyRegion(id, startFrame, endFrame)
    override fun pasteRegion(id: Int, insertAt: Int) = native_pasteRegion(id, insertAt)
    override fun getClipboardLength(): Int = native_getClipboardLength()
    override fun downsampleSample(id: Int, factor: Int) = native_downsampleSample(id, factor)
    override fun applyRateMode(id: Int, factor: Int) = native_applyRateMode(id, factor)
    override fun pitchShiftSample(id: Int, semitones: Float) = native_pitchShiftSample(id, semitones)
    override fun timeStretchSample(id: Int, ratio: Float) = native_timeStretchSample(id, ratio)
    override fun applySampleFx(id: Int, fxType: Int, fxValue: Int, sampleRate: Float, limiterPreGain: Int) = native_applySampleFx(id, fxType, fxValue, sampleRate, limiterPreGain)
    override fun findZeroCrossing(id: Int, frame: Int, dir: Int): Int = native_findZeroCrossing(id, frame, dir)
    override fun detectTransients(id: Int, sensitivity: Int): IntArray =
        native_detectTransients(id, sensitivity, 128)

    override fun getTrackActiveNotes(): IntArray = native_getTrackActiveNotes()

    override fun scheduleNote(
        frame: Long,
        sampleId: Int,
        trackId: Int,
        freq: Float,
        baseFreq: Float,
        vol: Float,
        pan: Float,
        startPointOverride: Int
    ) {
        native_scheduleNote(frame, sampleId, trackId, freq, baseFreq, vol, pan, startPointOverride)
    }

    override fun getCurrentFrame(): Long {
        return native_getCurrentFrame()
    }

    override fun clearScheduledNotes() {
        native_clearScheduledNotes()
    }

    override fun clearScheduledNotesFrom(fromFrame: Long) {
        native_clearScheduledNotesFrom(fromFrame)
        Log.d(TAG, "🗑️ Cleared scheduled notes")
    }

    override fun resumeStream() {
        native_resumeStream()
    }

    override fun stopAll() {
        native_stopAll()
        Log.d(TAG, "⏹️ Stopped all voices")
    }

    override fun killTrack(trackId: Int) {
        native_killTrack(trackId)
        Log.d(TAG, "🔪 Killed track $trackId")
    }

    override fun scheduleKill(frame: Long, trackId: Int) {
        native_scheduleKill(frame, trackId)
        Log.d(TAG, "⏱️ Scheduled kill: track $trackId at frame $frame")
    }

    override fun getSampleRate(): Int {
        return native_getSampleRate()
    }

    override fun updateWaveform(buffer: FloatArray) {
        native_getWaveform(buffer)
    }

    override fun getTrackWaveforms(buffer: FloatArray, activeTracks: BooleanArray) {
        native_getTrackWaveforms(buffer, activeTracks)
    }

    override fun setInstrumentParams(
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
        native_setInstrumentParams(
            instrumentId,
            startPoint,
            endPoint,
            reverse,
            loopMode,
            loopStart,
            drive,
            crush,
            downsample,
            filterType,
            filterCut,
            filterRes
        )
    }

    override fun close() {
        stopAll()
        native_delete()
        Log.d(TAG, "🔌 Audio backend closed")
    }

    override fun getTrackPeaks(buffer: FloatArray) {
        if (buffer.size >= 16) {
            native_getTrackPeaks(buffer)
        }
    }

    override fun getMasterPeaks(buffer: FloatArray) {
        if (buffer.size >= 2) {
            native_getMasterPeaks(buffer)
        }
    }

    override fun getSendPeaks(buffer: FloatArray) {
        if (buffer.size >= 4) {
            native_getSendPeaks(buffer)
        }
    }

    override fun renderFrames(numFrames: Int, sampleRate: Int): FloatArray {
        return native_renderFrames(numFrames, sampleRate) ?: FloatArray(numFrames * 2)
    }

    override fun resetFrameCounter() {
        native_resetFrameCounter()
    }

    override fun getFrameCounter(): Long {
        return native_getFrameCounter()
    }

    override fun decayPeaks() {
        native_decayPeaks()
    }

    override fun decayWaveform() {
        native_decayWaveform()
    }

    override fun getSpectrumMagnitudes(numBins: Int): FloatArray = native_getSpectrumMagnitudes(numBins)
    override fun getSpectrumMagnitudesForSource(source: Int, instrId: Int, numBins: Int): FloatArray =
        native_getSpectrumMagnitudesForSource(source, instrId, numBins)

    override fun setTrackVolume(trackId: Int, volume: Float) {
        native_setTrackVolume(trackId, volume)
    }

    override fun setMasterVolume(volume: Float) {
        native_setMasterVolume(volume)
    }

    override fun setOttDepth(depth: Int) {
        native_setOttDepth(depth)
    }

    override fun setOttDepthForRender(depth: Int) {
        native_setOttDepthForRender(depth)
    }

    override fun setMasterFx(fx: Int) {
        native_setMasterFx(fx)
    }

    override fun setDustDepth(depth: Int) {
        native_setDustDepth(depth)
    }

    override fun setDustDepthForRender(depth: Int) {
        native_setDustDepthForRender(depth)
    }

    override fun setLimiterPreGain(depth: Int) {
        native_setLimiterPreGain(depth)
    }

    override fun setStemsMode(mode: Int) {
        native_setStemsMode(mode)
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // EQ / SEND / REVERB / DELAY METHODS
    // ═══════════════════════════════════════════════════════════════════════════

    override fun setEqBand(slot: Int, band: Int, type: Int, freqHex: Int, gainHex: Int, qHex: Int) {
        native_setEqBand(slot, band, type, freqHex, gainHex, qHex)
    }

    override fun setInstrumentEqSlot(instrId: Int, slot: Int) {
        native_setInstrumentEqSlot(instrId, slot)
    }

    override fun setInstrumentSendLevels(instrId: Int, reverbSend: Int, delaySend: Int) {
        native_setInstrumentSendLevels(instrId, reverbSend, delaySend)
    }

    override fun setDelayReverbSend(sendHex: Int) {
        native_setDelayReverbSend(sendHex)
    }

    override fun setReverbParams(feedbackHex: Int, dampHex: Int, wetHex: Int) {
        native_setReverbParams(feedbackHex, dampHex, wetHex)
    }

    override fun setDelayParams(timeOrSubdiv: Int, feedbackHex: Int, syncMode: Boolean, bpm: Float, wetHex: Int) {
        native_setDelayParams(timeOrSubdiv, feedbackHex, syncMode, bpm, wetHex)
    }

    override fun setReverbInputEq(slot: Int) {
        native_setReverbInputEq(slot)
    }

    override fun setDelayInputEq(slot: Int) {
        native_setDelayInputEq(slot)
    }

    override fun setMasterEqSlot(slot: Int) {
        native_setMasterEqSlot(slot)
    }

    override fun loadTable(tableId: Int, rowData: ByteArray) {
        if (rowData.size != 128) {
            Log.e(TAG, "❌ loadTable: Invalid rowData size ${rowData.size} (expected 128)")
            return
        }
        native_loadTable(tableId, rowData)
        Log.d(TAG, "📋 Loaded table $tableId")
    }

    override fun scheduleNoteWithTable(
        frame: Long,
        sampleId: Int,
        trackId: Int,
        freq: Float,
        baseFreq: Float,
        vol: Float,
        phraseVol: Float,
        pan: Float,
        startPointOverride: Int,
        endPointOverride: Int,
        tableId: Int,
        tableTicRate: Int,
        noteOctave: Int,
        notePitch: Int,
        pslInitialOffset: Float,
        pslDuration: Float,
        pbnRate: Float,
        vibratoSpeed: Float,
        vibratoDepth: Float,
        tableStartRow: Int
    ) {
        native_scheduleNoteWithTable(frame, sampleId, trackId, freq, baseFreq, vol, phraseVol, pan,
            startPointOverride, endPointOverride, tableId, tableTicRate, noteOctave, notePitch,
            pslInitialOffset, pslDuration, pbnRate, vibratoSpeed, vibratoDepth,
            tableStartRow)
    }

    override fun getVoiceTableRow(trackId: Int): Int {
        return native_getVoiceTableRow(trackId)
    }

    override fun getVoiceTableId(trackId: Int): Int {
        return native_getVoiceTableId(trackId)
    }

    override fun setVoiceTableRow(trackId: Int, row: Int) {
        native_setVoiceTableRow(trackId, row)
    }

    override fun scheduleTrackPhraseVol(targetFrame: Long, trackId: Int, phraseVol: Float) {
        native_scheduleTrackPhraseVol(targetFrame, trackId, phraseVol)
    }

    override fun setPitchSlide(trackId: Int, targetSemitones: Float, durationTicks: Float, tempo: Int) {
        native_setPitchSlide(trackId, targetSemitones, durationTicks, tempo)
        Log.d(TAG, "🎵 Pitch slide: track=$trackId, target=$targetSemitones, duration=$durationTicks ticks")
    }

    override fun setPitchBend(trackId: Int, semitonesPerTick: Float, tempo: Int) {
        native_setPitchBend(trackId, semitonesPerTick, tempo)
        if (semitonesPerTick == 0f) {
            Log.d(TAG, "🎵 Pitch bend stopped: track=$trackId")
        } else {
            Log.d(TAG, "🎵 Pitch bend: track=$trackId, rate=$semitonesPerTick semitones/tick")
        }
    }

    override fun setVibrato(trackId: Int, speed: Float, depth: Float) {
        native_setVibrato(trackId, speed, depth)
        if (depth < 0.01f) {
            Log.d(TAG, "🎵 Vibrato stopped: track=$trackId")
        } else {
            Log.d(TAG, "🎵 Vibrato: track=$trackId, speed=${speed}Hz, depth=$depth semitones")
        }
    }

    override fun clearPitchMod(trackId: Int) {
        native_clearPitchMod(trackId)
        Log.d(TAG, "🎵 Pitch mod cleared: track=$trackId")
    }

    override fun setInitialPitchOffset(trackId: Int, semitones: Float) {
        native_setInitialPitchOffset(trackId, semitones)
        Log.d(TAG, "🎵 Pitch offset set: track=$trackId, offset=$semitones semitones")
    }

    override fun setInstrumentModulation(
        sampleId: Int,
        slotIndex: Int,
        type: Int,
        dest: Int,
        amount: Float,
        attackSamples: Int,
        holdSamples: Int,
        decaySamples: Int,
        sustainLevel: Float,
        lfoHz: Float,
        oscShape: Int,
        releaseSamples: Int
    ) {
        native_setInstrumentModulation(sampleId, slotIndex, type, dest, amount,
            attackSamples, holdSamples, decaySamples, sustainLevel, lfoHz, oscShape, releaseSamples)
    }

    override fun clearInstrumentModulation(sampleId: Int) {
        native_clearInstrumentModulation(sampleId)
    }

    override fun triggerNoteOff(trackId: Int) {
        native_triggerNoteOff(trackId)
    }

    override fun scheduleNoteOff(frame: Long, trackId: Int) {
        native_scheduleNoteOff(frame, trackId)
    }

    override fun setOfflineRendering(rendering: Boolean) {
        native_setOfflineRendering(rendering)
    }

    // SoundFont methods
    override fun loadSoundfont(instrumentId: Int, filePath: String): Int =
        native_loadSoundfont(instrumentId, filePath)

    override fun setSoundfontPreset(sfSlot: Int, bank: Int, preset: Int) {
        native_setSoundfontPreset(sfSlot, bank, preset)
    }

    override fun scheduleSoundfontNote(
        frame: Long, trackId: Int, sfSlot: Int,
        midiNote: Int, velocity: Int, vol: Float, pan: Float, bank: Int, preset: Int,
        pslInitialOffset: Float, pslDuration: Float,
        pbnRate: Float, vibratoSpeed: Float, vibratoDepth: Float,
        phraseVol: Float, sampleId: Int,
        tableId: Int, tableTicRate: Int, noteOctave: Int, notePitch: Int, tableStartRow: Int,
        detuneSemitones: Float
    ) {
        native_scheduleSoundfontNote(
            frame, trackId, sfSlot, midiNote, velocity, vol, pan, bank, preset,
            pslInitialOffset, pslDuration, pbnRate, vibratoSpeed, vibratoDepth,
            phraseVol, sampleId, tableId, tableTicRate, noteOctave, notePitch, tableStartRow,
            detuneSemitones
        )
    }

    override fun setSoundfontEnvelopeOverrides(instrumentId: Int,
                                               atk: Int, dec: Int, sus: Int, rel: Int) {
        native_setSoundfontEnvelopeOverrides(instrumentId, atk, dec, sus, rel)
    }

    override fun setSoundfontFilterOverrides(sampleId: Int, filterType: Int,
                                             filterCut: Int, filterRes: Int) {
        native_setSoundfontFilterOverrides(sampleId, filterType, filterCut, filterRes)
    }

    override fun unloadSoundfont(sfSlot: Int) {
        native_unloadSoundfont(sfSlot)
    }

    override fun clearAllSoundfonts() {
        native_clearAllSoundfonts()
    }

    override fun getSoundfontPresetName(sfSlot: Int, bank: Int, preset: Int): String =
        native_getSoundfontPresetName(sfSlot, bank, preset) ?: "---"

    override fun getSoundfontFirstBankPreset(sfSlot: Int): IntArray =
        native_getSoundfontFirstBankPreset(sfSlot)

    override fun getSoundfontPresetCount(sfSlot: Int): Int =
        native_getSoundfontPresetCount(sfSlot)

    override fun getSoundfontPresetAt(sfSlot: Int, index: Int): IntArray =
        native_getSoundfontPresetAt(sfSlot, index)

    // ═══════════════════════════════════════════════════════════════════════════
    // Native Methods (JNI → C++)
    // ═══════════════════════════════════════════════════════════════════════════

    private external fun native_create(): Boolean
    private external fun native_delete()
    private external fun native_loadSample(sampleId: Int, sampleData: FloatArray)
    private external fun native_loadSampleStereo(sampleId: Int, leftData: FloatArray, rightData: FloatArray)
    private external fun native_loadSampleFromWav(id: Int, path: String): Int
    private external fun native_hasStereoData(sampleId: Int): Boolean
    private external fun native_clearAllSamples()
    private external fun native_clearSample(id: Int)
    private external fun native_freeSampleUndo(id: Int)
    private external fun native_getTrackActiveNotes(): IntArray
    private external fun native_scheduleNote(
        targetFrame: Long,
        sampleId: Int,
        trackId: Int,
        frequency: Float,
        baseFrequency: Float,
        volume: Float,
        pan: Float,
        startPointOverride: Int
    )
    private external fun native_getCurrentFrame(): Long
    private external fun native_clearScheduledNotes()
    private external fun native_clearScheduledNotesFrom(fromFrame: Long)
    private external fun native_resumeStream()
    private external fun native_stopAll()
    private external fun native_killTrack(trackId: Int)
    private external fun native_scheduleKill(frame: Long, trackId: Int)
    private external fun native_getSampleRate(): Int
    private external fun native_getWaveform(outArray: FloatArray)
    private external fun native_setInstrumentParams(
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
    )
    private external fun native_getTrackPeaks(outArray: FloatArray)
    private external fun native_getMasterPeaks(outArray: FloatArray)
    private external fun native_getSendPeaks(outArray: FloatArray)
    private external fun native_renderFrames(numFrames: Int, sampleRate: Int): FloatArray?
    private external fun native_resetFrameCounter()
    private external fun native_getFrameCounter(): Long

    private external fun native_decayPeaks()
    private external fun native_decayWaveform()
    private external fun native_getTrackWaveforms(outArray: FloatArray, activeFlags: BooleanArray)
    private external fun native_getSpectrumMagnitudes(numBins: Int): FloatArray
    private external fun native_getSpectrumMagnitudesForSource(source: Int, instrId: Int, numBins: Int): FloatArray
    private external fun native_setTrackVolume(trackId: Int, volume: Float)
    private external fun native_setMasterVolume(volume: Float)
    private external fun native_setOttDepth(depth: Int)
    private external fun native_setOttDepthForRender(depth: Int)
    private external fun native_setMasterFx(fx: Int)
    private external fun native_setDustDepth(depth: Int)
    private external fun native_setDustDepthForRender(depth: Int)
    private external fun native_setLimiterPreGain(depth: Int)
    private external fun native_setStemsMode(mode: Int)

    // EQ / send / reverb / delay methods
    private external fun native_setEqBand(slot: Int, band: Int, type: Int, freqHex: Int, gainHex: Int, qHex: Int)
    private external fun native_setInstrumentEqSlot(instrId: Int, slot: Int)
    private external fun native_setInstrumentSendLevels(instrId: Int, reverbSend: Int, delaySend: Int)
    private external fun native_setDelayReverbSend(sendHex: Int)
    private external fun native_setReverbParams(feedbackHex: Int, dampHex: Int, wetHex: Int)
    private external fun native_setDelayParams(timeOrSubdiv: Int, feedbackHex: Int, syncMode: Boolean, bpm: Float, wetHex: Int)
    private external fun native_setReverbInputEq(slot: Int)
    private external fun native_setDelayInputEq(slot: Int)
    private external fun native_setMasterEqSlot(slot: Int)

    private external fun native_loadTable(tableId: Int, rowData: ByteArray)
    private external fun native_scheduleNoteWithTable(
        targetFrame: Long,
        sampleId: Int,
        trackId: Int,
        frequency: Float,
        baseFrequency: Float,
        volume: Float,
        phraseVolume: Float,
        pan: Float,
        startPointOverride: Int,
        endPointOverride: Int,
        tableId: Int,
        tableTicRate: Int,
        noteOctave: Int,
        notePitch: Int,
        pslInitialOffset: Float,
        pslDuration: Float,
        pbnRate: Float,
        vibratoSpeed: Float,
        vibratoDepth: Float,
        tableStartRow: Int
    )
    private external fun native_getVoiceTableRow(trackId: Int): Int
    private external fun native_getVoiceTableId(trackId: Int): Int
    private external fun native_setVoiceTableRow(trackId: Int, row: Int)
    private external fun native_scheduleTrackPhraseVol(targetFrame: Long, trackId: Int, phraseVol: Float)

    private external fun native_setPitchSlide(trackId: Int, targetSemitones: Float, durationTicks: Float, tempo: Int)
    private external fun native_setPitchBend(trackId: Int, semitonesPerTick: Float, tempo: Int)
    private external fun native_setVibrato(trackId: Int, speed: Float, depth: Float)
    private external fun native_clearPitchMod(trackId: Int)
    private external fun native_setInitialPitchOffset(trackId: Int, semitones: Float)

    private external fun native_setInstrumentModulation(
        sampleId: Int, slotIndex: Int, type: Int, dest: Int, amount: Float,
        attackSamples: Int, holdSamples: Int, decaySamples: Int,
        sustainLevel: Float, lfoHz: Float, oscShape: Int, releaseSamples: Int
    )
    private external fun native_clearInstrumentModulation(sampleId: Int)
    private external fun native_triggerNoteOff(trackId: Int)
    private external fun native_scheduleNoteOff(frame: Long, trackId: Int)
    private external fun native_setOfflineRendering(rendering: Boolean)

    // SoundFont JNI declarations
    private external fun native_loadSoundfont(instrumentId: Int, path: String): Int
    private external fun native_setSoundfontPreset(sfSlot: Int, bank: Int, preset: Int)
    private external fun native_scheduleSoundfontNote(
        frame: Long, trackId: Int, sfSlot: Int,
        midiNote: Int, velocity: Int, vol: Float, pan: Float, bank: Int, preset: Int,
        pslInitialOffset: Float, pslDuration: Float,
        pbnRate: Float, vibratoSpeed: Float, vibratoDepth: Float,
        phraseVol: Float, sampleId: Int,
        tableId: Int, tableTicRate: Int, noteOctave: Int, notePitch: Int, tableStartRow: Int,
        detuneSemitones: Float
    )
    private external fun native_setSoundfontEnvelopeOverrides(instrumentId: Int,
                                                              atk: Int, dec: Int, sus: Int, rel: Int)
    private external fun native_setSoundfontFilterOverrides(sampleId: Int, filterType: Int,
                                                            filterCut: Int, filterRes: Int)
    private external fun native_unloadSoundfont(sfSlot: Int)
    private external fun native_clearAllSoundfonts()
    private external fun native_getSoundfontPresetName(sfSlot: Int, bank: Int, preset: Int): String?
    private external fun native_getSoundfontFirstBankPreset(sfSlot: Int): IntArray
    private external fun native_getSoundfontPresetCount(sfSlot: Int): Int
    private external fun native_getSoundfontPresetAt(sfSlot: Int, index: Int): IntArray

    // Sample editor JNI declarations
    private external fun native_getSampleLength(id: Int): Int
    private external fun native_getSampleWaveform(id: Int, numBins: Int): FloatArray
    private external fun native_getSampleWaveformRange(id: Int, startFrame: Int, endFrame: Int, numBins: Int): FloatArray
    private external fun native_getSampleWaveformRangeSource(id: Int, startFrame: Int, endFrame: Int, numBins: Int, channel: Int): FloatArray
    private external fun native_getSampleData(id: Int): FloatArray
    private external fun native_getSampleDataRight(id: Int): FloatArray
    private external fun native_normalizeSample(id: Int, startFrame: Int, endFrame: Int)
    private external fun native_fadeInSample(id: Int, startFrame: Int, endFrame: Int)
    private external fun native_fadeOutSample(id: Int, startFrame: Int, endFrame: Int)
    private external fun native_silenceRegion(id: Int, startFrame: Int, endFrame: Int)
    private external fun native_reverseSample(id: Int, startFrame: Int, endFrame: Int)
    private external fun native_backupSample(id: Int)
    private external fun native_undoSample(id: Int)
    private external fun native_saveFxPreviewBackup(id: Int)
    private external fun native_restoreFxPreviewBackup()
    private external fun native_getSamplePlaybackPosition(id: Int): Float
    private external fun native_cropSample(id: Int, startFrame: Int, endFrame: Int)
    private external fun native_deleteSampleRegion(id: Int, startFrame: Int, endFrame: Int)
    private external fun native_copyRegion(id: Int, startFrame: Int, endFrame: Int)
    private external fun native_pasteRegion(id: Int, insertAt: Int)
    private external fun native_getClipboardLength(): Int
    private external fun native_downsampleSample(id: Int, factor: Int)
    private external fun native_applyRateMode(id: Int, factor: Int)
    private external fun native_pitchShiftSample(id: Int, semitones: Float)
    private external fun native_timeStretchSample(id: Int, ratio: Float)
    private external fun native_applySampleFx(id: Int, fxType: Int, fxValue: Int, sampleRate: Float, limiterPreGain: Int)
    private external fun native_findZeroCrossing(id: Int, frame: Int, dir: Int): Int
    private external fun native_detectTransients(id: Int, sensitivity: Int, maxMarkers: Int): IntArray
}
