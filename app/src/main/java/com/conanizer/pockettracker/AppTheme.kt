package com.conanizer.pockettracker

import kotlinx.serialization.Serializable

@Serializable
enum class VisualizerType { SCOPE, BARS, PEAKS, MIRROR, FLAT }

/**
 * Tracker UI color scheme.
 *
 * Colors stored as ARGB Long (e.g. 0xFF00FF00L) so the data class is JSON-serializable
 * for .ptt theme files. Convert to Compose Color at point of use: Color(theme.vizWave).
 *
 * All defaults match the current hardcoded CLASSIC look exactly — zero visual change
 * until modules are updated to read from theme in Phase 3.
 */
@Serializable
data class AppTheme(
    val name: String = "CLASSIC",

    // ── Row backgrounds ────────────────────────────────────────────────────────
    val background: Long    = 0xFF0A0A0AL,   // module fill + default row
    val rowEvery4th: Long   = 0xFF151515L,   // beat-accent rows (every 4th)
    val rowCursor: Long     = 0xFF333333L,   // cursor row highlight
    val rowPlayback: Long   = 0xFF004400L,   // current playback row
    val rowSelection: Long  = 0xFF1A3A1AL,  // selection region

    // ── Text roles ─────────────────────────────────────────────────────────────
    val textTitle: Long     = 0xFF00FFFFL,  // screen headers (cyan)
    val textParam: Long     = 0xFF808080L,  // inactive param label (Color.Gray)
    val textValue: Long     = 0xFFFFFFFFL,  // inactive param value (white)
    val textCursor: Long    = 0xFFFFFF00L,  // cursor-highlighted cell (yellow)
    val textEmpty: Long     = 0xFF666666L,  // empty / placeholder

    // ── Visualizer (oscilloscope bar) ──────────────────────────────────────────
    val vizBackground: Long = 0xFF0A0A0AL,
    val vizCenterLine: Long = 0xFF333333L,
    val vizWave: Long       = 0xFF00FF00L,  // waveform line / bar fill

    // ── Mixer dBFS meters ──────────────────────────────────────────────────────
    val meterBackground: Long = 0xFF1A1A1AL,
    val meterLow: Long        = 0xFF00CC00L,
    val meterMid: Long        = 0xFFCCCC00L,
    val meterHigh: Long       = 0xFFCC0000L,
    val meterBorder: Long     = 0xFF444444L,

    // ── Visualizer mode ────────────────────────────────────────────────────────
    val visualizerType: VisualizerType = VisualizerType.SCOPE
) {
    companion object {
        val CLASSIC = AppTheme(name = "CLASSIC")

        val AMBER = AppTheme(
            name          = "AMBER",
            rowPlayback   = 0xFF332200L,
            rowSelection  = 0xFF3A2A00L,
            textTitle     = 0xFFFFCC00L,
            textParam     = 0xFF806040L,
            textValue     = 0xFFEECC88L,
            textCursor    = 0xFFFFFF00L,
            textEmpty     = 0xFF664422L,
            vizCenterLine = 0xFF442200L,
            vizWave       = 0xFFFF8800L,
            meterLow      = 0xFFCC8800L,
            meterMid      = 0xFFCC4400L,
            meterHigh     = 0xFFCC0000L
        )

        val BLUE = AppTheme(
            name          = "BLUE",
            rowPlayback   = 0xFF001144L,
            rowSelection  = 0xFF002266L,
            textTitle     = 0xFF88CEFFL,
            textParam     = 0xFF4488AAL,
            textValue     = 0xFFAADDFFL,
            textCursor    = 0xFF00FFFFL,
            textEmpty     = 0xFF224466L,
            vizCenterLine = 0xFF112244L,
            vizWave       = 0xFF0088FFL,
            meterLow      = 0xFF0088CCL,
            meterMid      = 0xFF0044CCL,
            meterHigh     = 0xFF8800CCL
        )

        val MONO = AppTheme(
            name          = "MONO",
            rowPlayback   = 0xFF222222L,
            rowSelection  = 0xFF333333L,
            textTitle     = 0xFFFFFFFFL,
            textParam     = 0xFF888888L,
            textValue     = 0xFFFFFFFFL,
            textCursor    = 0xFFFFFFFFL,
            textEmpty     = 0xFF444444L,
            vizCenterLine = 0xFF222222L,
            vizWave       = 0xFFCCCCCCL,
            meterLow      = 0xFFCCCCCCL,
            meterMid      = 0xFF888888L,
            meterHigh     = 0xFF444444L
        )

        val BUILTINS = listOf(CLASSIC, AMBER, BLUE, MONO)
    }
}
