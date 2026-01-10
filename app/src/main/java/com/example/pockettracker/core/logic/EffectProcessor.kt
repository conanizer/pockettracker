package com.example.pockettracker.core.logic

import com.example.pockettracker.core.logging.ILogger
import com.example.pockettracker.Instrument
import com.example.pockettracker.PhraseStep
import com.example.pockettracker.core.audio.IAudioBackend

/**
 * Resolved parameters for a phrase step after applying effects.
 *
 * This is returned by resolveStepParams() and contains the final values
 * to use when scheduling a note, with effects taking priority over defaults.
 *
 * Priority order:
 * - startPoint: OFFSET effect (Oxx) > instrument.sampleStart
 * - volume: VOLUME effect (Vxx) > step.volume
 * - killAtFrame: Only set if KILL effect (K00) is present
 */
data class ResolvedStepParams(
    /** Sample start point (0-255), or -1 to use instrument default */
    val startPoint: Int = -1,

    /** Volume (0.0-1.0), resolved from VOLUME effect or step.volume */
    val volume: Float = 1.0f,

    /** Frame at which to kill the note, or null if no KILL effect */
    val killAtFrame: Long? = null,

    /** Arpeggio semitones (high nibble, low nibble), or null if no ARPEGGIO effect */
    val arpeggioValue: Int? = null,

    /** Repeat count (1-15), or null if no REPEAT effect */
    val repeatCount: Int? = null
)

/**
 * EFFECT PROCESSOR
 *
 * Processes effect commands and applies them to scheduled notes.
 *
 * ✅ PLATFORM-AGNOSTIC - No Android dependencies (except Log which will be abstracted later)
 *
 * NOTE: Full effects implementation comes in MVP Milestone 2.
 * This provides the structure and integration points for now.
 *
 * TOP-5 Effects (to be implemented in Milestone 2):
 * - Arpeggio (Axx) - Note pattern automation
 * - Offset (Oxx) - Sample start point automation
 * - Volume (Vxx) - Volume automation within step
 * - Kill (K00) - Stop sample immediately
 * - Repeat (Rxx) - Retrigger sample N times per step
 */
class EffectProcessor(
    private val audioBackend: IAudioBackend,
    private val logger: ILogger
) {
    private val TAG = "EffectProcessor"

    // ========================================
    // EFFECT TYPE CONSTANTS
    // ========================================

    companion object {
        // Effect type constants
        const val FX_NONE = 0x00      // No effect

        // TOP-5 Effects (Phrase screen only)
        const val FX_ARPEGGIO = 0x0A  // Axx - Note pattern automation
        const val FX_KILL = 0x0B      // K00 - Kill sample
        const val FX_OFFSET = 0x0F    // Oxx - Sample start point
        const val FX_REPEAT = 0x12    // Rxx - Retrigger sample
        const val FX_VOLUME = 0x16    // Vxx - Volume automation

        /**
         * List of all valid effect types for UI cycling.
         * Used by editors to cycle through effect types with UP/DOWN.
         * Order: NONE, ARPEGGIO, KILL, OFFSET, REPEAT, VOLUME (sorted by hex value)
         */
        val EFFECT_TYPES = listOf(FX_NONE, FX_ARPEGGIO, FX_KILL, FX_OFFSET, FX_REPEAT, FX_VOLUME)

        // More effects will be added in Milestone 2
        // Table screen effects (Post-MVP):
        // - Pitch bend, vibrato, filter sweep, etc.
    }

    // ========================================
    // EFFECT RESOLUTION (Single source of truth)
    // ========================================

    /**
     * Resolve all effect parameters for a phrase step.
     *
     * This is the SINGLE SOURCE OF TRUTH for effect resolution.
     * Returns resolved values with effects taking priority over defaults:
     * - OFFSET (Oxx) overrides instrument.sampleStart
     * - VOLUME (Vxx) overrides step.volume
     * - KILL (K00) sets killAtFrame
     * - ARPEGGIO (Axx) sets arpeggioValue
     * - REPEAT (Rxx) sets repeatCount
     *
     * @param step The phrase step containing effect data
     * @param baseFrame When this step triggers (for KILL timing)
     * @param defaultVolume Default volume if no VOLUME effect (typically step.volume / 255f)
     * @return ResolvedStepParams with all effect values resolved
     */
    fun resolveStepParams(
        step: PhraseStep,
        baseFrame: Long,
        defaultVolume: Float
    ): ResolvedStepParams {
        var startPoint = -1  // -1 = use instrument default
        var volume = defaultVolume
        var killAtFrame: Long? = null
        var arpeggioValue: Int? = null
        var repeatCount: Int? = null

        // Check all 3 FX columns
        for (fxSlot in 1..3) {
            val (type, value) = when (fxSlot) {
                1 -> step.fx1Type to step.fx1Value
                2 -> step.fx2Type to step.fx2Value
                3 -> step.fx3Type to step.fx3Value
                else -> 0 to 0
            }

            when (type) {
                FX_OFFSET -> {
                    startPoint = value
                    logger.d(TAG, "📍 OFFSET effect: startPoint=$value (0x${value.toString(16).uppercase()})")
                }
                FX_VOLUME -> {
                    volume = value / 255.0f
                    logger.d(TAG, "🔊 VOLUME effect: volume=$volume (raw=$value)")
                }
                FX_KILL -> {
                    killAtFrame = baseFrame
                    logger.d(TAG, "🔪 KILL effect: scheduled at frame $baseFrame")
                }
                FX_ARPEGGIO -> {
                    arpeggioValue = value
                    val semi1 = (value shr 4) and 0x0F
                    val semi2 = value and 0x0F
                    logger.d(TAG, "🎵 ARPEGGIO effect: +$semi1, +$semi2 semitones")
                }
                FX_REPEAT -> {
                    repeatCount = value
                    logger.d(TAG, "🔁 REPEAT effect: $value retriggers")
                }
            }
        }

        return ResolvedStepParams(
            startPoint = startPoint,
            volume = volume,
            killAtFrame = killAtFrame,
            arpeggioValue = arpeggioValue,
            repeatCount = repeatCount
        )
    }

    // ========================================
    // EFFECT APPLICATION
    // ========================================

    /**
     * Apply all effects from a phrase step.
     *
     * @param step The phrase step with effect commands
     * @param baseFrame When this step triggers
     * @param stepDuration Duration of step in frames
     * @param trackId Which track (0-7)
     * @param baseFrequency Base frequency for the note
     * @param baseVolume Base volume for the note
     * @param sampleId Sample ID to play
     */
    fun applyEffects(
        step: PhraseStep,
        baseFrame: Long,
        stepDuration: Long,
        trackId: Int,
        baseFrequency: Float,
        baseVolume: Float,
        sampleId: Int
    ) {
        // Process FX1
        if (step.fx1Type != 0x00) {
            applyEffect(
                type = step.fx1Type,
                value = step.fx1Value,
                frame = baseFrame,
                duration = stepDuration,
                trackId = trackId,
                frequency = baseFrequency,
                volume = baseVolume,
                sampleId = sampleId
            )
        }

        // Process FX2
        if (step.fx2Type != 0x00) {
            applyEffect(
                type = step.fx2Type,
                value = step.fx2Value,
                frame = baseFrame,
                duration = stepDuration,
                trackId = trackId,
                frequency = baseFrequency,
                volume = baseVolume,
                sampleId = sampleId
            )
        }

        // Process FX3
        if (step.fx3Type != 0x00) {
            applyEffect(
                type = step.fx3Type,
                value = step.fx3Value,
                frame = baseFrame,
                duration = stepDuration,
                trackId = trackId,
                frequency = baseFrequency,
                volume = baseVolume,
                sampleId = sampleId
            )
        }
    }

    /**
     * Apply a single effect.
     */
    private fun applyEffect(
        type: Int,
        value: Int,
        frame: Long,
        duration: Long,
        trackId: Int,
        frequency: Float,
        volume: Float,
        sampleId: Int
    ) {
        logger.d(TAG, "⚡ applyEffect: type=0x${type.toString(16).uppercase()}, value=0x${value.toString(16).uppercase()}, frame=$frame")
        when (type) {
            FX_ARPEGGIO -> applyArpeggio(value, frame, duration, trackId, frequency, volume, sampleId)
            FX_OFFSET -> applyOffset(value, frame, trackId, frequency, volume, sampleId)
            FX_VOLUME -> applyVolume(value, frame, duration, trackId, frequency, sampleId)
            FX_KILL -> applyKill(frame, trackId)
            FX_REPEAT -> applyRepeat(value, frame, duration, trackId, frequency, volume, sampleId)
            else -> {
                logger.w(TAG, "Unknown effect type: 0x${type.toString(16).uppercase()}")
            }
        }
    }

    // ========================================
    // EFFECT IMPLEMENTATIONS (STUBS FOR NOW)
    // ========================================
    // These will be fully implemented in Milestone 2

    /**
     * Arpeggio: Play note + semitone1 + semitone2 in sequence.
     *
     * Value format: high nibble = semitone1, low nibble = semitone2
     * Example: A37 = 3 semitones up, 7 semitones up
     *
     * TODO: Implement in Milestone 2
     * - Extract semitones from value
     * - Calculate 3 frequencies (base, +sem1, +sem2)
     * - Schedule 3 notes within step duration (dividing time equally)
     */
    private fun applyArpeggio(
        value: Int,
        frame: Long,
        duration: Long,
        trackId: Int,
        baseFreq: Float,
        volume: Float,
        sampleId: Int
    ) {
        // Stub - full implementation in Milestone 2
        logger.d(TAG, "⏳ Arpeggio effect stub: value=0x${value.toString(16).uppercase()} (implementation pending)")
    }

    /**
     * Offset: Override sample start point.
     *
     * Value: 0x00-0xFF maps to 0%-100% of sample length
     * Example: O80 = start at 50% through the sample
     *
     * TODO: Implement in Milestone 2
     * - Calculate new start point from value
     * - Update IAudioBackend to support per-note start point override
     */
    private fun applyOffset(
        value: Int,
        frame: Long,
        trackId: Int,
        frequency: Float,
        volume: Float,
        sampleId: Int
    ) {
        // Stub - full implementation in Milestone 2
        logger.d(TAG, "⏳ Offset effect stub: value=0x${value.toString(16).uppercase()} (implementation pending)")
    }

    /**
     * Volume: Volume automation within step.
     *
     * Value: 0x00-0xFF = 0%-100% volume
     * Example: V80 = 50% volume
     *
     * TODO: Implement in Milestone 2
     * - Convert value to volume multiplier
     * - Apply to scheduled note
     */
    private fun applyVolume(
        value: Int,
        frame: Long,
        duration: Long,
        trackId: Int,
        frequency: Float,
        sampleId: Int
    ) {
        // Stub - full implementation in Milestone 2
        logger.d(TAG, "⏳ Volume effect stub: value=0x${value.toString(16).uppercase()} (implementation pending)")
    }

    /**
     * Kill: Stop sample at exact frame time.
     *
     * Value: Always 00
     * Example: K00 = stop playback on this track
     *
     * ✅ IMPLEMENTED - Schedules kill to happen at exact frame time
     */
    private fun applyKill(
        frame: Long,
        trackId: Int
    ) {
        audioBackend.scheduleKill(frame, trackId)
        logger.d(TAG, "🔪 Kill effect: Track $trackId scheduled to stop at frame $frame")
    }

    /**
     * Repeat: Retrigger sample N times per step.
     *
     * Value: Number of retriggers (1-15)
     * Example: R04 = retrigger 4 times within step
     *
     * TODO: Implement in Milestone 2
     * - Calculate retrigger timing (divide step duration)
     * - Schedule multiple notes at calculated intervals
     */
    private fun applyRepeat(
        value: Int,
        frame: Long,
        duration: Long,
        trackId: Int,
        frequency: Float,
        volume: Float,
        sampleId: Int
    ) {
        // Stub - full implementation in Milestone 2
        logger.d(TAG, "⏳ Repeat effect stub: value=0x${value.toString(16).uppercase()} (implementation pending)")
    }
}
