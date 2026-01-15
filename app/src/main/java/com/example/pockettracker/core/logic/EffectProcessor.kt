package com.example.pockettracker.core.logic

import com.example.pockettracker.core.logging.ILogger
import com.example.pockettracker.core.data.PhraseStep
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
                    logger.d(
                        TAG,
                        "📍 OFFSET effect: startPoint=$value (0x${value.toString(16).uppercase()})"
                    )
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
}