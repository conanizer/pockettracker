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

    override fun clearAllSamples() {
        native_clearAllSamples()
        Log.d(TAG, "🗑️ Cleared all loaded samples")
    }

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
        if (buffer.size >= 8) {
            native_getTrackPeaks(buffer)
        }
    }

    override fun getMasterPeaks(buffer: FloatArray) {
        if (buffer.size >= 2) {
            native_getMasterPeaks(buffer)
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

    // ═══════════════════════════════════════════════════════════════════════════
    // PHASE 1 BUG FIXES: DECAY AND REAL-TIME VOLUME
    // ═══════════════════════════════════════════════════════════════════════════

    override fun decayPeaks() {
        native_decayPeaks()
    }

    override fun decayWaveform() {
        native_decayWaveform()
    }

    override fun setTrackVolume(trackId: Int, volume: Float) {
        native_setTrackVolume(trackId, volume)
    }

    override fun setMasterVolume(volume: Float) {
        native_setMasterVolume(volume)
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // TABLE METHODS (Phase 3.5)
    // ═══════════════════════════════════════════════════════════════════════════

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
        pan: Float,
        startPointOverride: Int,
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
        native_scheduleNoteWithTable(frame, sampleId, trackId, freq, baseFreq, vol, pan,
            startPointOverride, tableId, tableTicRate, noteOctave, notePitch,
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

    // ═══════════════════════════════════════════════════════════════════════════
    // PITCH MODULATION METHODS (Phase 6)
    // ═══════════════════════════════════════════════════════════════════════════

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

    // ═══════════════════════════════════════════════════════════════════════════
    // MODULATION METHODS (Phase 4 — AHD)
    // ═══════════════════════════════════════════════════════════════════════════

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

    // ═══════════════════════════════════════════════════════════════════════════
    // Native Methods (JNI → C++)
    // ═══════════════════════════════════════════════════════════════════════════

    private external fun native_create(): Boolean
    private external fun native_delete()
    private external fun native_loadSample(sampleId: Int, sampleData: FloatArray)
    private external fun native_clearAllSamples()
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
    private external fun native_renderFrames(numFrames: Int, sampleRate: Int): FloatArray?
    private external fun native_resetFrameCounter()
    private external fun native_getFrameCounter(): Long

    // Phase 1 bug fixes
    private external fun native_decayPeaks()
    private external fun native_decayWaveform()
    private external fun native_setTrackVolume(trackId: Int, volume: Float)
    private external fun native_setMasterVolume(volume: Float)

    // Phase 3.5 table methods
    private external fun native_loadTable(tableId: Int, rowData: ByteArray)
    private external fun native_scheduleNoteWithTable(
        targetFrame: Long,
        sampleId: Int,
        trackId: Int,
        frequency: Float,
        baseFrequency: Float,
        volume: Float,
        pan: Float,
        startPointOverride: Int,
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

    // Phase 6 pitch modulation methods
    private external fun native_setPitchSlide(trackId: Int, targetSemitones: Float, durationTicks: Float, tempo: Int)
    private external fun native_setPitchBend(trackId: Int, semitonesPerTick: Float, tempo: Int)
    private external fun native_setVibrato(trackId: Int, speed: Float, depth: Float)
    private external fun native_clearPitchMod(trackId: Int)
    private external fun native_setInitialPitchOffset(trackId: Int, semitones: Float)

    // Phase 4 modulation methods
    private external fun native_setInstrumentModulation(
        sampleId: Int, slotIndex: Int, type: Int, dest: Int, amount: Float,
        attackSamples: Int, holdSamples: Int, decaySamples: Int,
        sustainLevel: Float, lfoHz: Float, oscShape: Int, releaseSamples: Int
    )
    private external fun native_clearInstrumentModulation(sampleId: Int)
    private external fun native_triggerNoteOff(trackId: Int)
    private external fun native_scheduleNoteOff(frame: Long, trackId: Int)
    private external fun native_setOfflineRendering(rendering: Boolean)
}
