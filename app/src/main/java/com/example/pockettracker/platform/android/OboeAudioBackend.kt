package com.example.pockettracker.platform.android

import com.example.pockettracker.core.audio.IAudioBackend
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

    override fun scheduleNote(
        frame: Long,
        sampleId: Int,
        trackId: Int,
        freq: Float,
        baseFreq: Float,
        vol: Float
    ) {
        native_scheduleNote(frame, sampleId, trackId, freq, baseFreq, vol)
    }

    override fun getCurrentFrame(): Long {
        return native_getCurrentFrame()
    }

    override fun clearScheduledNotes() {
        native_clearScheduledNotes()
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

    // ═══════════════════════════════════════════════════════════════════════════
    // Native Methods (JNI → C++)
    // ═══════════════════════════════════════════════════════════════════════════

    private external fun native_create(): Boolean
    private external fun native_delete()
    private external fun native_loadSample(sampleId: Int, sampleData: FloatArray)
    private external fun native_scheduleNote(
        targetFrame: Long,
        sampleId: Int,
        trackId: Int,
        frequency: Float,
        baseFrequency: Float,
        volume: Float
    )
    private external fun native_getCurrentFrame(): Long
    private external fun native_clearScheduledNotes()
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
}
