package com.conanizer.pockettracker.core.logic

import com.conanizer.pockettracker.core.logging.ILogger
import com.conanizer.pockettracker.core.data.PhraseStep
import com.conanizer.pockettracker.core.audio.IAudioBackend

data class ResolvedStepParams(
    val startPoint: Int = -1,
    val volume: Float = 1.0f,
    // True when set by Vxx effect, not just the step volume column
    val volumeFromVxx: Boolean = false,
    val killAtFrame: Long? = null,
    // Axx: high nibble = +semitone1, low nibble = +semitone2
    val arpeggioValue: Int? = null,

    // Cxx: high nibble = mode (0=UP,1=DOWN,2=PINGPONG,3=RANDOM), low nibble = speed in tics
    val arcValue: Int? = null,

    // Rxy: y!=0 → retrig every y ticks + vol ramp x; y=0 → retrig every x ticks
    val repeatCount: Int? = null,
    // 0/8=no change, 1-7=decrease per retrig, 9-F=increase per retrig
    val repeatVolRamp: Int? = null,

    // Hxx: 0xFF=stop track, else low nibble = target row on next phrase
    val hopValue: Int? = null,
    // PSL: portamento duration in ticks (01=fast, FF=slow, 00=instant)
    val pslDuration: Int? = null,
    // PBN: 00=stop, 01-7F=bend up, 80-FF=bend down; rate = (val & 0x7F) / 16 semitones/step
    val pbnValue: Int? = null,
    // PVB: high nibble=speed (Hz = 2 + x*0.5), low nibble=depth (semitones = y*0.125)
    val pvbValue: Int? = null,
    // PVX: same format as PVB but 4x depth and 2x speed
    val pvxValue: Int? = null,
    val delayTicks: Int? = null,
    // CHA: high nibble=probability (0=never, F=always), low nibble=target (0=note, 1-3=FX slot)
    val chanceValue: Int? = null,

    // RND xy: randomize previous FX column value; range x0 to yF
    val rndValue: Int? = null,
    // RNL xy: same as RND but targets the FX column to the left
    val rnlValue: Int? = null,
    val tableOverride: Int? = null,
    val tableHopTarget: Int? = null,
    // 00=disable groove, 01-FF=groove table ID
    val grooveId: Int? = null
)

class EffectProcessor(
    private val audioBackend: IAudioBackend,
    private val logger: ILogger
) {
    private val TAG = "EffectProcessor"

    companion object {
        const val FX_NONE = 0x00

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

        const val FX_PSL = 0x19       // PSL xx - Pitch Slide (portamento), xx = duration in ticks
        const val FX_PBN = 0x1A       // PBN xx - Pitch Bend, 00-7F = up, 80-FF = down, 00 = stop
        const val FX_PVB = 0x1B       // PVB xy - Vibrato, x = speed (0-F), y = depth (0-F)
        const val FX_PVX = 0x1C       // PVX xy - Extreme Vibrato (4x depth, 2x speed)

        val EFFECT_TYPES = listOf(
            FX_NONE, FX_ARC, FX_CHA, FX_DEL, FX_GRV, FX_HOP, FX_TIC, FX_ARPEGGIO, FX_KILL, FX_OFFSET,
            FX_RND, FX_RNL, FX_REPEAT, FX_TBL, FX_THO, FX_VOLUME,
            FX_PSL, FX_PBN, FX_PVB, FX_PVX
        )
    }

    fun resolveStepParams(
        step: PhraseStep,
        baseFrame: Long,
        defaultVolume: Float
    ): ResolvedStepParams {
        var startPoint = -1  // -1 = use instrument default
        var volume = defaultVolume
        var volumeFromVxx = false
        var killAtFrame: Long? = null
        var arpeggioValue: Int? = null
        var arcValue: Int? = null
        var repeatCount: Int? = null
        var repeatVolRamp: Int? = null
        var hopValue: Int? = null

        var pslDuration: Int? = null
        var pbnValue: Int? = null
        var pvbValue: Int? = null
        var pvxValue: Int? = null
        var delayTicks: Int? = null
        var chanceValue: Int? = null
        var rndValue: Int? = null
        var rnlValue: Int? = null
        var tableOverride: Int? = null
        var tableHopTarget: Int? = null
        var grooveId: Int? = null

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
                    volumeFromVxx = true
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
            volumeFromVxx = volumeFromVxx,
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