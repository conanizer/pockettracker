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
 *
 * TIC SYSTEM:
 * Effects like REPEAT and ARPEGGIO use tics for precise timing.
 * A step is divided into TICS_PER_STEP tics (default: 12).
 * See PlaybackController.TICS_PER_STEP for configuration.
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

    /**
     * Arpeggio config (ARC Cxx), or null if no ARC effect.
     *
     * High nibble (X) = mode:
     *   0 = UP (default): root -> +X -> +Y
     *   1 = DOWN: +Y -> +X -> root
     *   2 = PINGPONG: root -> +X -> +Y -> +X -> ...
     *   3 = RANDOM
     *
     * Low nibble (Y) = speed in tics:
     *   4 = default (3 notes/step at 12 tics)
     *   1 = fast (12 notes/step)
     *   6 = slow (2 notes/step)
     *
     * TODO (Post-MVP): Additional ARC modes to consider:
     *   4 = UP_OCT: root -> +X -> +Y -> root+12 -> +X+12 -> +Y+12 -> ...
     *   5 = DOWN_OCT: reverse of UP_OCT
     *   6 = CHORD: all notes triggered simultaneously (no arpeggio)
     *   7 = SHUFFLE: like RANDOM but never repeats same note twice
     */
    val arcValue: Int? = null,

    /**
     * Repeat tic interval, or null if no REPEAT effect.
     *
     * REPEAT (Rxx) uses tic-interval approach (LGPT/M8 style):
     *
     * ## Sub-step intervals (R01-R0B) - multiple triggers within one step:
     * - R00 = no effect / cancel persistent REPEAT
     * - R01 = retrig every 1 tic = 12 triggers/step (fastest)
     * - R02 = retrig every 2 tics = 6 triggers/step
     * - R03 = retrig every 3 tics = 4 triggers/step (triplets!)
     * - R04 = retrig every 4 tics = 3 triggers/step
     * - R06 = retrig every 6 tics = 2 triggers/step
     *
     * ## Multi-step intervals (R0C+) - one trigger every N steps:
     * - R0C (12) = every 1 step (same timing as no retrig)
     * - R12 (18) = every 1.5 steps (dotted quarter notes!)
     * - R18 (24) = every 2 steps
     * - R24 (36) = every 3 steps
     * - R30 (48) = every 4 steps (4 kicks in 16-step phrase!)
     * - R3C (60) = every 5 steps
     * - R60 (96) = every 8 steps (2 triggers per phrase)
     *
     * ## Persistence
     * REPEAT persists until cancelled by:
     * 1. A new note on the same track
     * 2. Any effect in the same FX column where REPEAT was set
     * 3. KILL effect (K00) in any FX column
     */
    val repeatCount: Int? = null,

    /**
     * HOP effect value, or null if no HOP effect (Phase 5).
     *
     * HOP (Hxx) in phrase context:
     * - HOPFF (0xFF) = Stop track playback
     * - HOP XY = Jump to row Y on the NEXT phrase in chain
     *   - X is ignored in phrase context (used for repeat count in tables)
     *   - Y (low nibble) = target row (0x00-0x0F)
     *
     * Used for:
     * - Odd time signatures (e.g., HOP at row F to row 4 = 5/4 time)
     * - Track muting (HOPFF)
     */
    val hopValue: Int? = null,

    // ===================================
    // PITCH EFFECTS (Phase 7)
    // ===================================

    /**
     * PSL (Pitch Slide) duration in ticks, or null if no PSL effect.
     *
     * PSL xx enables portamento for the note:
     * - When a new note is triggered after a previous note, the pitch
     *   slides from the previous note's pitch to the new pitch.
     * - xx = slide duration in ticks (01 = fast, FF = slow)
     * - PSL 00 = instant (no slide)
     *
     * Example:
     * Row 0: C-4 01 FF ---      ← Note plays at C-4
     * Row 4: E-4 01 FF PSL 18  ← Pitch slides from C-4 to E-4 over 24 ticks
     */
    val pslDuration: Int? = null,

    /**
     * PBN (Pitch Bend) value, or null if no PBN effect.
     *
     * PBN xx bends the pitch continuously:
     * - 00 = stop bending (pitch stays at current offset)
     * - 01-7F = bend UP at rate (value / 16) semitones per step
     * - 80-FF = bend DOWN at rate ((value & 0x7F) / 16) semitones per step
     *
     * Persists until: PBN 00, new note, or KILL effect.
     *
     * Example:
     * Row 0: C-4 01 FF PBN 10  ← Bend up 1 semitone per step
     * Row 8: --- -- -- PBN 00  ← Stop bending
     */
    val pbnValue: Int? = null,

    /**
     * PVB (Vibrato) value, or null if no PVB effect.
     *
     * PVB xy sets vibrato parameters:
     * - x (high nibble) = speed (0-F): Hz = 2 + x * 0.5 (2Hz to 9.5Hz)
     * - y (low nibble) = depth (0-F): semitones = y * 0.125 (0 to 1.875 semitones)
     * - PVB 00 = stop vibrato
     *
     * Persists until: PVB 00, new note, or KILL effect.
     *
     * Example:
     * Row 0: C-4 01 FF PVB 64  ← Medium speed (6), subtle depth (4)
     */
    val pvbValue: Int? = null,

    /**
     * PVX (Extreme Vibrato) value, or null if no PVX effect.
     *
     * Same format as PVB but with 4x depth and 2x speed for wild wobble effects.
     *
     * PVX xy:
     * - x = speed: effective Hz = (2 + x * 0.5) * 2
     * - y = depth: effective semitones = y * 0.125 * 4
     */
    val pvxValue: Int? = null
)

/**
 * EFFECT PROCESSOR
 *
 * Processes effect commands and applies them to scheduled notes.
 *
 * ✅ PLATFORM-AGNOSTIC - No Android dependencies
 *
 * ## TIC SYSTEM
 * PocketTracker uses a tic-based timing system (like LGPT/M8):
 * - Each step is divided into TICS_PER_STEP tics (default: 12)
 * - Time-based effects (REPEAT, ARPEGGIO) operate at tic resolution
 * - 12 tics allows triplets (divisible by 3), half-steps (÷2), and quarter-steps (÷4)
 * - Future: Configurable via Groove screen (post-MVP)
 *
 * ## TOP-5 Effects (MVP Milestone 2)
 * - Arpeggio (Axx) - Note pattern automation (3 notes per step)
 * - Offset (Oxx) - Sample start point automation ✅ IMPLEMENTED
 * - Volume (Vxx) - Volume automation within step ✅ IMPLEMENTED
 * - Kill (K00) - Stop sample immediately ✅ IMPLEMENTED
 * - Repeat (Rxx) - Retrigger with persistence ✅ IMPLEMENTED
 *   - R01-R0B: Sub-step intervals (multiple triggers per step)
 *   - R0C+: Multi-step intervals (one trigger every N steps)
 *   - Persists until new note, same-column FX, or KILL
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

        // TOP-5 Effects (Phrase screen)
        const val FX_ARC = 0x03       // Cxx - Arpeggio Config (mode/speed)
        const val FX_HOP = 0x08       // Hxx - HOP: Phrase (jump row on next phrase, FF=stop track), Table (jump row with repeat count)
        const val FX_TIC = 0x09       // Txx - Table tick rate (01-FB = tics/row, FC-FF = special modes)
        const val FX_ARPEGGIO = 0x0A  // Axx - Note pattern automation
        const val FX_KILL = 0x0B      // K00 - Kill sample
        const val FX_OFFSET = 0x0F    // Oxx - Sample start point
        const val FX_REPEAT = 0x12    // Rxx - Retrigger sample
        const val FX_VOLUME = 0x16    // Vxx - Volume automation

        // Pitch Effects (Phase 7)
        const val FX_PSL = 0x19       // PSL xx - Pitch Slide (portamento), xx = duration in ticks
        const val FX_PBN = 0x1A       // PBN xx - Pitch Bend, 00-7F = up, 80-FF = down, 00 = stop
        const val FX_PVB = 0x1B       // PVB xy - Vibrato, x = speed (0-F), y = depth (0-F)
        const val FX_PVX = 0x1C       // PVX xy - Extreme Vibrato (4x depth, 2x speed)

        /**
         * List of all valid effect types for UI cycling.
         * Used by editors to cycle through effect types with UP/DOWN.
         * Order: NONE, ARC, HOP, TIC, ARPEGGIO, KILL, OFFSET, REPEAT, VOLUME, PSL, PBN, PVB, PVX (sorted by hex value)
         */
        val EFFECT_TYPES = listOf(
            FX_NONE, FX_ARC, FX_HOP, FX_TIC, FX_ARPEGGIO, FX_KILL, FX_OFFSET, FX_REPEAT, FX_VOLUME,
            FX_PSL, FX_PBN, FX_PVB, FX_PVX
        )
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
        var arcValue: Int? = null
        var repeatCount: Int? = null
        var hopValue: Int? = null

        // Pitch effects (Phase 7)
        var pslDuration: Int? = null
        var pbnValue: Int? = null
        var pvbValue: Int? = null
        var pvxValue: Int? = null

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

                FX_ARC -> {
                    arcValue = value
                    val mode = (value shr 4) and 0x0F
                    val speed = value and 0x0F
                    val modeNames = listOf("UP", "DOWN", "PINGPONG", "RANDOM")
                    val modeName = modeNames.getOrElse(mode) { "UP" }
                    logger.d(TAG, "🎼 ARC effect: mode=$modeName, speed=$speed tics")
                }

                FX_REPEAT -> {
                    repeatCount = value
                    logger.d(TAG, "🔁 REPEAT effect: $value retriggers")
                }

                FX_HOP -> {
                    hopValue = value
                    if (value == 0xFF) {
                        logger.d(TAG, "🦘 HOP FF: stop track")
                    } else {
                        val targetRow = value and 0x0F
                        logger.d(TAG, "🦘 HOP effect: jump to row $targetRow on next phrase")
                    }
                }

                // ===================================
                // PITCH EFFECTS (Phase 7)
                // ===================================

                FX_PSL -> {
                    pslDuration = value
                    logger.d(TAG, "🎵 PSL effect: portamento duration=$value ticks")
                }

                FX_PBN -> {
                    pbnValue = value
                    if (value == 0) {
                        logger.d(TAG, "🎵 PBN effect: stop pitch bend")
                    } else {
                        val direction = if (value < 0x80) "UP" else "DOWN"
                        val rate = (value and 0x7F) / 16f
                        logger.d(TAG, "🎵 PBN effect: bend $direction at $rate semitones/step")
                    }
                }

                FX_PVB -> {
                    pvbValue = value
                    if (value == 0) {
                        logger.d(TAG, "🎵 PVB effect: stop vibrato")
                    } else {
                        val speed = 2f + ((value shr 4) and 0x0F) * 0.5f
                        val depth = (value and 0x0F) * 0.125f
                        logger.d(TAG, "🎵 PVB effect: speed=${speed}Hz, depth=$depth semitones")
                    }
                }

                FX_PVX -> {
                    pvxValue = value
                    if (value == 0) {
                        logger.d(TAG, "🎵 PVX effect: stop extreme vibrato")
                    } else {
                        val speed = (2f + ((value shr 4) and 0x0F) * 0.5f) * 2f
                        val depth = (value and 0x0F) * 0.125f * 4f
                        logger.d(TAG, "🎵 PVX effect: speed=${speed}Hz, depth=$depth semitones (extreme)")
                    }
                }
            }
        }

        return ResolvedStepParams(
            startPoint = startPoint,
            volume = volume,
            killAtFrame = killAtFrame,
            arpeggioValue = arpeggioValue,
            arcValue = arcValue,
            repeatCount = repeatCount,
            hopValue = hopValue,
            pslDuration = pslDuration,
            pbnValue = pbnValue,
            pvbValue = pvbValue,
            pvxValue = pvxValue
        )
    }
}