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
    // KIL xx: extra latency (in ticks) added between the KIL row and the actual voice stop.
    // 00 = stop at the row; 0C = one full step later (TICS_PER_STEP). Resolved alongside killAtFrame.
    val killOffsetTicks: Int = 0,
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
    // PVB: high nibble=speed, low nibble=depth (semitones = y*0.125). Speed is tempo-synced:
    // (2 + x*0.5) Hz at 120 BPM, scaled by tempo/120 (PlaybackController), so the wobble tracks BPM.
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
    val grooveId: Int? = null,
    // PIT xx: signed semitone offset applied to pitch (never affects slice index); null = no PIT FX
    val pitSemitones: Int? = null,
    // SLI xx: explicit slice index (0-255); overrides note-based selection; works with SLICE=OFF
    val sliIndex: Int? = null,

    // ── Live per-note / mixer FX — null = effect not present on this step ──
    // PAN xx: per-note pan 00-FF (80=center). Overrides instrument pan for this note only.
    val panValue: Int? = null,
    // REV xx: per-note reverb send 00-FF. Overrides the instrument's reverb send for this note only.
    val reverbSendValue: Int? = null,
    // DEL xx: per-note delay send 00-FF. Overrides the instrument's delay send for this note only.
    val delaySendValue: Int? = null,
    // BCK 0/1: sampler playback direction (0=reverse, 1=forward). Live-toggleable for scratching.
    val bckValue: Int? = null,
    // EQN xx: per-note EQ preset slot 00-7F. Applies an EQ preset to this note's voice only.
    val eqnSlot: Int? = null,
    // EQM xx: master/mixer EQ preset slot 00-7F. Persists until next EQM; reset to mixer value on stop.
    val eqmSlot: Int? = null
)

class EffectProcessor(
    private val audioBackend: IAudioBackend,
    private val logger: ILogger
) {
    private val TAG = "EffectProcessor"

    companion object {
        // Per-step scheduling trace. Off in shipped builds: the logger.d() calls below build a
        // string for every effect on every scheduling pass even when logcat hides them — wasted
        // work on low-end devices during playback. Flip to true for effect-by-effect debugging.
        const val TRACE = false

        const val FX_NONE = 0x00

        const val FX_ARC = 0x03       // Cxx - Arpeggio Config (mode/speed)
        const val FX_CHA = 0x04       // CHA xy - Chance: x=probability (0-F), y=target FX slot (0=all)
        const val FX_LAT = 0x05       // LAT xx - Latency: delay row trigger by xx ticks
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
        const val FX_PIT = 0x1D       // PIT xx - Pitch offset in semitones (00-7F=+0..+127, 80-FF=-128..-1); never affects slice index
        const val FX_SLI = 0x1E       // SLI xx - Slice index override (00-FF); works even when SLICE mode is OFF

        // ── Live per-note / mixer FX — all routed through the sample-accurate ParamUpdateQueue ──
        const val FX_PAN   = 0x1F     // PAN xx - Per-note pan override (00=L, 80=center, FF=R); next note reverts to instrument PAN
        const val FX_RSEND = 0x20     // REV xx - Per-note reverb send (00-FF); affects only this note, not the instrument
        const val FX_DSEND = 0x21     // DEL xx - Per-note delay send (00-FF); affects only this note, not the instrument
        const val FX_BCK   = 0x22     // BCK 0/1 - Sampler playback direction (00=reverse, 01=forward); live scratch toggle
        const val FX_EQN   = 0x23     // EQN xx - Per-note EQ preset slot (00-7F); affects only this note
        const val FX_EQM   = 0x24     // EQM xx - Master/mixer EQ preset slot (00-7F); persists until next EQM, resets on stop

        val EFFECT_TYPES = listOf(
            FX_NONE, FX_ARC, FX_CHA, FX_LAT, FX_GRV, FX_HOP, FX_TIC, FX_ARPEGGIO, FX_KILL, FX_OFFSET,
            FX_RND, FX_RNL, FX_REPEAT, FX_TBL, FX_THO, FX_VOLUME,
            FX_PSL, FX_PBN, FX_PVB, FX_PVX, FX_PIT, FX_SLI,
            // Last grid row (centered): the four send/EQ FX
            FX_PAN, FX_BCK, FX_RSEND, FX_DSEND, FX_EQN, FX_EQM
        )

        // Single source of truth for effect code → 3-letter display name, keyed off the FX_* constants
        // (not literal hex) so codes and names can never drift apart. UI and PlaybackController both use
        // effectName(); the UI's EditorHelpers.getEffectTypeName() delegates here.
        // NOTE: the on-screen name for FX_REPEAT is "RPT" — the user manual still says "REP" (doc drift).
        val FX_NAMES: Map<Int, String> = mapOf(
            FX_ARC to "ARC", FX_CHA to "CHA", FX_LAT to "LAT", FX_GRV to "GRV", FX_HOP to "HOP",
            FX_TIC to "TIC", FX_ARPEGGIO to "ARP", FX_KILL to "KIL", FX_OFFSET to "OFF",
            FX_RND to "RND", FX_RNL to "RNL", FX_REPEAT to "RPT", FX_TBL to "TBL", FX_THO to "THO",
            FX_VOLUME to "VOL", FX_PSL to "PSL", FX_PBN to "PBN", FX_PVB to "PVB", FX_PVX to "PVX",
            FX_PIT to "PIT", FX_SLI to "SLI",
            FX_PAN to "PAN", FX_RSEND to "REV", FX_DSEND to "DEL", FX_BCK to "BCK",
            FX_EQN to "EQN", FX_EQM to "EQM"
        )

        /** Effect code → 3-letter name, or "---" for NONE/unknown. */
        fun effectName(code: Int): String = FX_NAMES[code] ?: "---"

        /** Max value (inclusive) for an effect's parameter byte. Effects that reference the table,
         *  groove, or EQ preset pools cap at 0x7F (128 slots, 0x00–0x7F); all others use the full
         *  0xFF range. */
        fun effectValueMax(effectType: Int): Int =
            if (effectType == FX_TBL || effectType == FX_GRV ||
                effectType == FX_EQN || effectType == FX_EQM) 127 else 255
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
        var killOffsetTicks = 0
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
        var pitSemitones: Int? = null
        var sliIndex: Int? = null
        var panValue: Int? = null
        var reverbSendValue: Int? = null
        var delaySendValue: Int? = null
        var bckValue: Int? = null
        var eqnSlot: Int? = null
        var eqmSlot: Int? = null

        for (fxSlot in 1..3) {
            val (type, value) = step.fx(fxSlot)

            when (type) {
                FX_OFFSET -> {
                    startPoint = value
                    if (TRACE) logger.d(TAG, "📍 OFFSET effect: startPoint=$value (0x${value.toString(16).uppercase()})")
                }

                FX_VOLUME -> {
                    volume = value / 255.0f
                    volumeFromVxx = true
                    if (TRACE) logger.d(TAG, "🔊 VOLUME effect: volume=$volume (raw=$value)")
                }

                FX_KILL -> {
                    killAtFrame = baseFrame
                    killOffsetTicks = value  // xx = ticks of latency before the stop fires (00 = at the row)
                    if (TRACE) logger.d(TAG, "🔪 KILL effect: scheduled at frame $baseFrame (+$value ticks)")
                }

                FX_ARPEGGIO -> {
                    arpeggioValue = value
                    val semi1 = (value shr 4) and 0x0F
                    val semi2 = value and 0x0F
                    if (TRACE) logger.d(TAG, "🎵 ARPEGGIO effect: +$semi1, +$semi2 semitones")
                }

                FX_ARC -> {
                    arcValue = value
                    val mode = (value shr 4) and 0x0F
                    val speed = value and 0x0F
                    val modeNames = listOf("UP", "DOWN", "PINGPONG", "RANDOM")
                    val modeName = modeNames.getOrElse(mode) { "UP" }
                    if (TRACE) logger.d(TAG, "🎼 ARC effect: mode=$modeName, speed=$speed tics")
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
                    if (TRACE) logger.d(TAG, "🔁 REPEAT R${value.toString(16).uppercase().padStart(2, '0')}: " +
                            "retrig every ${repeatCount} ticks, $rampDesc")
                }

                FX_HOP -> {
                    hopValue = value
                    if (value == 0xFF) {
                        if (TRACE) logger.d(TAG, "🦘 HOP FF: stop track")
                    } else {
                        val targetRow = value and 0x0F
                        if (TRACE) logger.d(TAG, "🦘 HOP effect: jump to row $targetRow on next phrase")
                    }
                }

                FX_PSL -> {
                    pslDuration = value
                    if (TRACE) logger.d(TAG, "🎵 PSL effect: portamento duration=$value ticks")
                }

                FX_PBN -> {
                    pbnValue = value
                    if (value == 0) {
                        if (TRACE) logger.d(TAG, "🎵 PBN effect: stop pitch bend")
                    } else {
                        val direction = if (value < 0x80) "UP" else "DOWN"
                        val rate = (value and 0x7F) / 16f
                        if (TRACE) logger.d(TAG, "🎵 PBN effect: bend $direction at $rate semitones/step")
                    }
                }

                FX_PVB -> {
                    pvbValue = value
                    if (value == 0) {
                        if (TRACE) logger.d(TAG, "🎵 PVB effect: stop vibrato")
                    } else {
                        val speed = 2f + ((value shr 4) and 0x0F) * 0.5f
                        val depth = (value and 0x0F) * 0.125f
                        if (TRACE) logger.d(TAG, "🎵 PVB effect: speed=${speed}Hz, depth=$depth semitones")
                    }
                }

                FX_PVX -> {
                    pvxValue = value
                    if (value == 0) {
                        if (TRACE) logger.d(TAG, "🎵 PVX effect: stop extreme vibrato")
                    } else {
                        val speed = (2f + ((value shr 4) and 0x0F) * 0.5f) * 2f
                        val depth = (value and 0x0F) * 0.125f * 4f
                        if (TRACE) logger.d(TAG, "🎵 PVX effect: speed=${speed}Hz, depth=$depth semitones (extreme)")
                    }
                }

                FX_LAT -> {
                    delayTicks = value
                    if (TRACE) logger.d(TAG, "⏳ LAT effect: delay trigger by $value ticks")
                }

                FX_PAN -> {
                    panValue = value
                    if (TRACE) logger.d(TAG, "🎚️ PAN effect: pan=$value (80=center)")
                }

                FX_RSEND -> {
                    reverbSendValue = value
                    if (TRACE) logger.d(TAG, "🌫️ REV effect: per-note reverb send=$value")
                }

                FX_DSEND -> {
                    delaySendValue = value
                    if (TRACE) logger.d(TAG, "🔁 DEL effect: per-note delay send=$value")
                }

                FX_BCK -> {
                    bckValue = value
                    if (TRACE) logger.d(TAG, "⏪ BCK effect: direction=${if (value == 0) "reverse" else "forward"}")
                }

                FX_EQN -> {
                    eqnSlot = value
                    if (TRACE) logger.d(TAG, "🎛️ EQN effect: per-note EQ slot=$value")
                }

                FX_EQM -> {
                    eqmSlot = value
                    if (TRACE) logger.d(TAG, "🎛️ EQM effect: master EQ slot=$value")
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
                    if (TRACE) logger.d(TAG, "🎲 CHA effect: probability=$probability/15, target=$targetName")
                }

                FX_RND -> {
                    rndValue = value
                    val minNibble = (value shr 4) and 0x0F
                    val maxNibble = value and 0x0F
                    if (TRACE) logger.d(TAG, "🎲 RND effect: randomize previous FX value range ${minNibble}0-${maxNibble}F")
                }

                FX_RNL -> {
                    rnlValue = value
                    val minNibble = (value shr 4) and 0x0F
                    val maxNibble = value and 0x0F
                    if (TRACE) logger.d(TAG, "🎲 RNL effect: randomize left FX value range ${minNibble}0-${maxNibble}F")
                }

                FX_TBL -> {
                    tableOverride = value
                    if (TRACE) logger.d(TAG, "📋 TBL effect: set table ${value.toString(16).uppercase().padStart(2, '0')}")
                }

                FX_THO -> {
                    tableHopTarget = value
                    if (TRACE) logger.d(TAG, "📋 THO effect: table hop to row ${value.toString(16).uppercase().padStart(2, '0')}")
                }

                FX_GRV -> {
                    grooveId = value
                    if (value == 0) {
                        if (TRACE) logger.d(TAG, "🥁 GRV effect: disable groove (default timing)")
                    } else {
                        if (TRACE) logger.d(TAG, "🥁 GRV effect: assign groove table ${value.toString(16).uppercase().padStart(2, '0')}")
                    }
                }

                FX_PIT -> {
                    pitSemitones = if (value < 0x80) value else value - 256
                    if (TRACE) logger.d(TAG, "🎵 PIT effect: pitch offset=$pitSemitones semitones")
                }

                FX_SLI -> {
                    sliIndex = value
                    if (TRACE) logger.d(TAG, "🎵 SLI effect: slice index=$value")
                }
            }
        }

        return ResolvedStepParams(
            startPoint = startPoint,
            volume = volume,
            volumeFromVxx = volumeFromVxx,
            killAtFrame = killAtFrame,
            killOffsetTicks = killOffsetTicks,
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
            grooveId = grooveId,
            pitSemitones = pitSemitones,
            sliIndex = sliIndex,
            panValue = panValue,
            reverbSendValue = reverbSendValue,
            delaySendValue = delaySendValue,
            bckValue = bckValue,
            eqnSlot = eqnSlot,
            eqmSlot = eqmSlot
        )
    }
}