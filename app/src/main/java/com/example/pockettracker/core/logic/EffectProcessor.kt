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
     * REPEAT (RXY) - M8-style retrigger with optional volume ramping:
     *
     * ## Format:
     * - R00 = cancel persistent REPEAT
     * - RX0 (Y=0, X=1-F): retrig every X ticks, no volume ramp
     * - RXY (Y!=0): retrig every Y ticks, volume ramp X
     *   - X=0: no volume change
     *   - X=1-7: decrease volume per retrig (1=slight, 7=heavy)
     *   - X=8: no volume change
     *   - X=9-F: increase volume per retrig (9=slight, F=heavy)
     *
     * ## Examples:
     * - R30 = retrig every 3 ticks (4 triggers/step), no vol ramp
     * - R03 = retrig every 3 ticks, no vol ramp
     * - R13 = retrig every 3 ticks, slight volume decrease
     * - R73 = retrig every 3 ticks, heavy volume decrease
     * - RF3 = retrig every 3 ticks, heavy volume increase
     *
     * Max interval: F (15 ticks = 1.25 steps)
     *
     * ## Persistence
     * REPEAT persists until cancelled by:
     * 1. A new note on the same track
     * 2. Any effect in the same FX column where REPEAT was set
     * 3. KILL effect (K00) in any FX column
     */
    val repeatCount: Int? = null,

    /**
     * Volume ramp for REPEAT effect (0-F), or null if no REPEAT.
     * Uses ADDITIVE delta per retrigger (accumulates across steps):
     * - 0 and 8: no change (delta = 0.0)
     * - 1-7: decrease volume per retrig (1=-0.02, 7=-0.30)
     * - 9-F: increase volume per retrig (9=+0.02, F=+0.30)
     */
    val repeatVolRamp: Int? = null,

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
    val pvxValue: Int? = null,

    // ===================================
    // EXTENSION PACK 3 EFFECTS
    // ===================================

    /**
     * DEL (Delay) ticks, or null if no DEL effect.
     *
     * DEL xx delays the row trigger by xx ticks.
     * - 00 = no delay
     * - 01-FF = delay by that many ticks before triggering
     *
     * Used for: swing, humanization, flams, odd groove patterns
     */
    val delayTicks: Int? = null,

    /**
     * CHA (Chance) value, or null if no CHA effect.
     *
     * CHA xy: probability-based effect execution
     * - x = probability (0-F): 0=never, F=always, 8=~50%
     * - y = target: 0=this row's note trigger, 1-3=FX slot 1-3
     *
     * Used for: generative music, random muting, stochastic patterns
     */
    val chanceValue: Int? = null,

    /**
     * RND (Randomize) value, or null if no RND effect.
     *
     * RND xy: randomizes the effect value in the PREVIOUS FX column
     * - x = minimum random value high nibble
     * - y = maximum random value high nibble
     * - Actual range: x0 to yF
     *
     * Example: VOL 80 / RND 4C → volume randomized between 40-CF
     */
    val rndValue: Int? = null,

    /**
     * RNL (Randomize Left) value, or null if no RNL effect.
     *
     * RNL xy: randomizes the effect value in the FX column to the LEFT
     * Same format as RND but targets the column immediately to the left.
     * - In FX2: targets FX1
     * - In FX3: targets FX2
     * - In FX1: no effect (nothing to the left)
     */
    val rnlValue: Int? = null,

    /**
     * TBL (Table) override, or null if no TBL effect.
     *
     * TBL xx: assigns table xx to the current instrument for this note.
     * - 00-FF = table ID
     * - Overrides instrument's default table assignment
     */
    val tableOverride: Int? = null,

    /**
     * THO (Table Hop) target row, or null if no THO effect.
     *
     * THO xx: jump to row xx in the current table.
     * - 00-0F = target row in table
     */
    val tableHopTarget: Int? = null,

    /**
     * GRV (Groove) ID, or null if no GRV effect.
     *
     * GRV xx: assign groove table xx to this track.
     * - 00 = disable groove (use default timing)
     * - 01-FF = groove table ID
     */
    val grooveId: Int? = null
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
        const val FX_CHA = 0x04       // CHA xy - Chance: x=probability (0-F), y=target FX slot (0=all)
        const val FX_DEL = 0x05       // DEL xx - Delay row by xx ticks before triggering
        const val FX_GRV = 0x07       // GRV xx - Assign groove table xx to this track
        const val FX_HOP = 0x08       // Hxx - HOP: Phrase (jump row on next phrase, FF=stop track), Table (jump row with repeat count)
        const val FX_TIC = 0x09       // Txx - Table tick rate (01-FB = tics/row, FC-FF = special modes)
        const val FX_ARPEGGIO = 0x0A  // Axx - Note pattern automation
        const val FX_KILL = 0x0B      // K00 - Kill sample
        const val FX_OFFSET = 0x0F    // Oxx - Sample start point
        const val FX_RND = 0x10       // RND xy - Randomize: apply random value to previous FX column
        const val FX_RNL = 0x11       // RNL xy - Randomize Left: apply random value to FX column to the left
        const val FX_REPEAT = 0x12    // Rxy - Retrigger (M8-style: y!=0: retrig y ticks + vol ramp x; y=0: retrig x ticks)
        const val FX_TBL = 0x14       // TBL xx - Set table xx for this instrument
        const val FX_THO = 0x15       // THO xx - Table Hop: jump to row xx in current table
        const val FX_VOLUME = 0x16    // Vxx - Volume automation

        // Pitch Effects (Phase 7)
        const val FX_PSL = 0x19       // PSL xx - Pitch Slide (portamento), xx = duration in ticks
        const val FX_PBN = 0x1A       // PBN xx - Pitch Bend, 00-7F = up, 80-FF = down, 00 = stop
        const val FX_PVB = 0x1B       // PVB xy - Vibrato, x = speed (0-F), y = depth (0-F)
        const val FX_PVX = 0x1C       // PVX xy - Extreme Vibrato (4x depth, 2x speed)

        /**
         * List of all valid effect types for UI cycling.
         * Used by editors to cycle through effect types with UP/DOWN.
         * Sorted by hex value: NONE, ARC, CHA, DEL, GRV, HOP, TIC, ARP, KIL, OFF, RND, RNL, RPT, TBL, THO, VOL, PSL, PBN, PVB, PVX
         */
        val EFFECT_TYPES = listOf(
            FX_NONE, FX_ARC, FX_CHA, FX_DEL, FX_GRV, FX_HOP, FX_TIC, FX_ARPEGGIO, FX_KILL, FX_OFFSET,
            FX_RND, FX_RNL, FX_REPEAT, FX_TBL, FX_THO, FX_VOLUME,
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
        var repeatVolRamp: Int? = null
        var hopValue: Int? = null

        // Pitch effects (Phase 7)
        var pslDuration: Int? = null
        var pbnValue: Int? = null
        var pvbValue: Int? = null
        var pvxValue: Int? = null

        // Extension Pack 3 effects
        var delayTicks: Int? = null
        var chanceValue: Int? = null
        var rndValue: Int? = null
        var rnlValue: Int? = null
        var tableOverride: Int? = null
        var tableHopTarget: Int? = null
        var grooveId: Int? = null

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
                    // M8-style RXY format:
                    // R00 = cancel (handled by PlaybackController)
                    // RX0 (Y=0): retrig every X ticks, no vol ramp
                    // RXY (Y!=0): retrig every Y ticks, vol ramp X
                    val highNibble = (value shr 4) and 0x0F
                    val lowNibble = value and 0x0F
                    if (lowNibble != 0) {
                        repeatCount = lowNibble      // Y = tic interval
                        repeatVolRamp = highNibble   // X = volume ramp
                    } else {
                        repeatCount = highNibble     // X = tic interval (when Y=0)
                        repeatVolRamp = 0            // No volume ramp
                    }
                    val rampDesc = when {
                        (repeatVolRamp ?: 0) == 0 || (repeatVolRamp ?: 0) == 8 -> "no vol ramp"
                        (repeatVolRamp ?: 0) in 1..7 -> "vol decrease ${repeatVolRamp}"
                        else -> "vol increase ${(repeatVolRamp ?: 0) - 8}"
                    }
                    logger.d(TAG, "🔁 REPEAT R${value.toString(16).uppercase().padStart(2, '0')}: " +
                            "retrig every ${repeatCount} ticks, $rampDesc")
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

                // ===================================
                // EXTENSION PACK 3 EFFECTS
                // ===================================

                FX_DEL -> {
                    delayTicks = value
                    logger.d(TAG, "⏳ DEL effect: delay $value ticks")
                }

                FX_CHA -> {
                    chanceValue = value
                    val probability = (value shr 4) and 0x0F
                    val target = value and 0x0F
                    val targetName = when (target) {
                        0 -> "note trigger"
                        in 1..3 -> "FX slot $target"
                        else -> "unknown"
                    }
                    logger.d(TAG, "🎲 CHA effect: probability=$probability/15, target=$targetName")
                }

                FX_RND -> {
                    rndValue = value
                    val minNibble = (value shr 4) and 0x0F
                    val maxNibble = value and 0x0F
                    logger.d(TAG, "🎲 RND effect: randomize previous FX value range ${minNibble}0-${maxNibble}F")
                }

                FX_RNL -> {
                    rnlValue = value
                    val minNibble = (value shr 4) and 0x0F
                    val maxNibble = value and 0x0F
                    logger.d(TAG, "🎲 RNL effect: randomize left FX value range ${minNibble}0-${maxNibble}F")
                }

                FX_TBL -> {
                    tableOverride = value
                    logger.d(TAG, "📋 TBL effect: set table ${value.toString(16).uppercase().padStart(2, '0')}")
                }

                FX_THO -> {
                    tableHopTarget = value
                    logger.d(TAG, "📋 THO effect: table hop to row ${value.toString(16).uppercase().padStart(2, '0')}")
                }

                FX_GRV -> {
                    grooveId = value
                    if (value == 0) {
                        logger.d(TAG, "🥁 GRV effect: disable groove (default timing)")
                    } else {
                        logger.d(TAG, "🥁 GRV effect: assign groove table ${value.toString(16).uppercase().padStart(2, '0')}")
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
            repeatVolRamp = repeatVolRamp,
            hopValue = hopValue,
            pslDuration = pslDuration,
            pbnValue = pbnValue,
            pvbValue = pvbValue,
            pvxValue = pvxValue,
            delayTicks = delayTicks,
            chanceValue = chanceValue,
            rndValue = rndValue,
            rnlValue = rnlValue,
            tableOverride = tableOverride,
            tableHopTarget = tableHopTarget,
            grooveId = grooveId
        )
    }
}