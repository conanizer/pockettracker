package com.example.pockettracker

import com.example.pockettracker.core.data.Chain
import com.example.pockettracker.core.data.PhraseStep
import com.example.pockettracker.core.data.Track

/**
 * EDITOR HELPERS
 *
 * Helper functions for all editor screens (Phrase, Chain, Song, Project).
 * Migrated from MainActivity during Phase 4 cleanup (January 2025).
 *
 * This file contains UI-specific data manipulation functions that don't belong
 * in controllers (too UI-specific) but were cluttering MainActivity.
 */

// ═══════════════════════════════════════════════════════════════════════════
// PHRASE EDITOR HELPERS
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Clear effect (set to NONE) - A+B shortcut
 * @param fxSlot Which FX slot (1, 2, or 3)
 */
fun clearEffect(step: PhraseStep, fxSlot: Int) {
    when (fxSlot) {
        1 -> {
            step.fx1Type = 0x00
            step.fx1Value = 0x00
        }
        2 -> {
            step.fx2Type = 0x00
            step.fx2Value = 0x00
        }
        3 -> {
            step.fx3Type = 0x00
            step.fx3Value = 0x00
        }
    }
}

/**
 * Get effect type 3-letter name for display
 * Returns: ---, ARC, CHA, DEL, GRV, HOP, TIC, ARP, KIL, OFF, RND, RNL, RPT, TBL, THO, VOL, PSL, PBN, PVB, PVX
 */
fun getEffectTypeName(effectType: Int): String {
    return when (effectType) {
        0x03 -> "ARC"  // Arpeggio Config
        0x04 -> "CHA"  // Chance
        0x05 -> "DEL"  // Delay row
        0x07 -> "GRV"  // Groove assign
        0x08 -> "HOP"  // Table hop (jump to row)
        0x09 -> "TIC"  // Table tick rate
        0x0A -> "ARP"  // Arpeggio
        0x0B -> "KIL"  // Kill
        0x0F -> "OFF"  // Offset
        0x10 -> "RND"  // Randomize previous FX
        0x11 -> "RNL"  // Randomize left FX
        0x12 -> "RPT"  // Repeat
        0x14 -> "TBL"  // Table assign
        0x15 -> "THO"  // Table Hop
        0x16 -> "VOL"  // Volume
        // Pitch effects (Phase 7)
        0x19 -> "PSL"  // Pitch Slide (portamento)
        0x1A -> "PBN"  // Pitch Bend
        0x1B -> "PVB"  // Vibrato
        0x1C -> "PVX"  // Extreme Vibrato
        else -> "---"   // NONE
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// CHAIN EDITOR HELPERS
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Clear chain slot (set to empty)
 */
fun clearChainSlot(chain: Chain, row: Int) {
    chain.phraseRefs[row] = -1  // -1 = empty
    chain.transposeValues[row] = 0x00  // Reset transpose to default
}

// ═══════════════════════════════════════════════════════════════════════════
// SONG EDITOR HELPERS
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Clear chain reference in song track (set to empty)
 */
fun clearSongChainRef(track: Track, row: Int) {
    if (row < track.chainRefs.size) {
        track.chainRefs[row] = -1
    }
}

/**
 * Get track index from cursor column (1-8 → 0-7)
 */
fun getTrackIndex(cursorColumn: Int): Int {
    return (cursorColumn - 1).coerceIn(0, 7)
}

