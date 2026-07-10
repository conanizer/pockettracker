package com.conanizer.pockettracker.trace

import com.conanizer.pockettracker.core.audio.AudioEngine
import com.conanizer.pockettracker.core.data.Project
import com.conanizer.pockettracker.core.data.framesPerStep
import com.conanizer.pockettracker.core.logic.EffectProcessor
import com.conanizer.pockettracker.core.logic.PlaybackController
import com.conanizer.pockettracker.core.trace.EventTrace

/**
 * Drives the REAL Kotlin sequencer (PlaybackController + EffectProcessor + AudioEngine) against
 * a fake backend with a synthetic frame clock, recording golden traces through the EventTrace
 * tap. No device, no wall clock, no audio: the event stream is a pure function of
 * (project, transport command, mode, sampleRate) — FIX-1 — so this IS the sequencer's behavior.
 *
 * One harness per trace: fresh controllers, fresh TrackStates, zero cross-trace state.
 */
class TraceHarness(private val sampleRate: Int) {

    companion object {
        /** Live sessions start at a nonzero fake frame on purpose: the trace's session-relative
         *  frame base (latched at T PLAY) is what makes device and host traces comparable, and a
         *  zero start would let an absolute-frame bug pass unnoticed. */
        const val LIVE_START_FRAME = 977L

        /** Frames the fake clock advances between updatePlaybackBuffer() polls. Any fixed cadence
         *  yields the same events (SC-3); this one is deterministic and documented. */
        const val CLOCK_STEP = 512L

        /** SHA-1 hex of the canonical serialized project JSON — the trace header's project id. */
        fun projectSha1(serializedJson: String): String = EventTrace.projectSha1(serializedJson)
    }

    private val backend = FakeAudioBackend(sampleRate)
    private val audioEngine = AudioEngine(backend, FakeResourceLoader, FakeLogger)
    private val effectProcessor = EffectProcessor(FakeLogger)
    private val playback = PlaybackController(audioEngine, effectProcessor, FakeLogger, FakeStateObserver)

    /** Render-mode trace: the offline scheduling path (fresh state, muted tracks skipped). */
    fun renderTrace(project: Project, rows: IntRange, projectSha: String): String {
        val sb = StringBuilder()
        EventTrace.begin(sb, projectSha)
        try {
            playback.scheduleSongForRender(project, rows.first, rows.last)
        } finally {
            EventTrace.end()
        }
        return sb.toString()
    }

    /** Live-mode trace: start transport, advance the fake clock a fixed horizon, stop. */
    fun liveTrace(project: Project, mode: GoldenProjects.LiveMode, projectSha: String): String {
        backend.frameClock = LIVE_START_FRAME
        val horizonFrames = framesPerStep(project.tempo, sampleRate) * 16L * mode.horizonPhrases
        val sb = StringBuilder()
        EventTrace.begin(sb, projectSha)
        try {
            when (mode.kind) {
                "SONG" -> playback.playSong(project, mode.arg)
                "CHAIN" -> playback.playChain(project, mode.arg)
                "PHRASE" -> playback.playPhrase(project, mode.arg)
                else -> error("unknown live mode ${mode.kind}")
            }
            while (backend.frameClock - LIVE_START_FRAME < horizonFrames) {
                playback.updatePlaybackBuffer()
                backend.frameClock += CLOCK_STEP
            }
            playback.stop()
        } finally {
            EventTrace.end()
        }
        return sb.toString()
    }
}
