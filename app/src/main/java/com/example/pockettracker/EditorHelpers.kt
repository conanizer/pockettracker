package com.example.pockettracker

import com.example.pockettracker.core.logic.EffectProcessor

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
 * Cycle note value (C-4, C#4, D-4, etc.)
 * @param direction +1 to go up, -1 to go down
 */
fun cycleNote(step: PhraseStep, direction: Int) {
    if (step.note == Note.EMPTY) {
        step.note = Note.fromMidi(60) // Start at C-4
    } else {
        val currentMidi = step.note.toMidi()
        val newMidi = (currentMidi + direction).coerceIn(0, 119)
        step.note = Note.fromMidi(newMidi)
    }
}

/**
 * Cycle volume value (00-FF, in steps of 16)
 */
fun cycleVolume(step: PhraseStep, direction: Int) {
    val newVolume = (step.volume + direction * 16).coerceIn(0, 255)
    step.volume = newVolume
}

/**
 * Cycle instrument (0-11 for default samples)
 */
fun cycleInstrument(step: PhraseStep, direction: Int) {
    step.instrument = ((step.instrument + direction + 128) % 12).coerceIn(0, 12)
}

/**
 * Cycle effect type for FX columns (A+UP/DOWN on effect name column)
 * Effect names: ---, ARP, KIL, OFF, RPT, VOL
 * @param fxSlot Which FX slot (1, 2, or 3)
 * @param direction +1 to go to next effect, -1 to go to previous
 */
fun cycleEffectType(step: PhraseStep, fxSlot: Int, direction: Int) {
    // Use centralized effect types list from EffectProcessor
    val effectTypes = EffectProcessor.EFFECT_TYPES

    val currentType = when (fxSlot) {
        1 -> step.fx1Type
        2 -> step.fx2Type
        3 -> step.fx3Type
        else -> return
    }

    val currentIndex = effectTypes.indexOf(currentType).takeIf { it >= 0 } ?: 0
    val newIndex = (currentIndex + direction + effectTypes.size) % effectTypes.size
    val newType = effectTypes[newIndex]

    when (fxSlot) {
        1 -> step.fx1Type = newType
        2 -> step.fx2Type = newType
        3 -> step.fx3Type = newType
    }
}

/**
 * Cycle effect value for FX columns (A+LEFT/RIGHT on effect value column)
 * @param fxSlot Which FX slot (1, 2, or 3)
 * @param direction +1 to increment, -1 to decrement
 */
fun cycleEffectValue(step: PhraseStep, fxSlot: Int, direction: Int) {
    val currentValue = when (fxSlot) {
        1 -> step.fx1Value
        2 -> step.fx2Value
        3 -> step.fx3Value
        else -> return
    }

    val newValue = (currentValue + direction).coerceIn(0, 255)

    when (fxSlot) {
        1 -> step.fx1Value = newValue
        2 -> step.fx2Value = newValue
        3 -> step.fx3Value = newValue
    }
}

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
 * Returns: ---, ARP, KIL, OFF, RPT, VOL
 */
fun getEffectTypeName(effectType: Int): String {
    return when (effectType) {
        0x0A -> "ARP"  // Arpeggio
        0x0B -> "KIL"  // Kill
        0x0F -> "OFF"  // Offset
        0x12 -> "RPT"  // Repeat
        0x16 -> "VOL"  // Volume
        else -> "---"   // NONE
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// CHAIN EDITOR HELPERS
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Insert phrase reference with last used value
 * Returns true if successfully inserted, false if slot already has value
 */
fun insertChainPhrase(chain: Chain, row: Int, lastPhraseValue: Int = 0): Boolean {
    if (chain.phraseRefs[row] == 0xFF) {
        chain.phraseRefs[row] = lastPhraseValue
        chain.transposeValues[row] = 0x00  // Default transpose
        return true  // Successfully inserted
    }
    return false  // Already has a value
}

/**
 * Edit phrase reference by delta
 * @param large If true, changes by 16 (0x10), else by 1
 */
fun editChainPhraseValue(chain: Chain, row: Int, direction: Int, large: Boolean = false) {
    val currentRef = chain.phraseRefs[row]
    if (currentRef == 0xFF) return  // Empty, can't edit

    val step = if (large) 16 else 1
    val newRef = (currentRef + direction * step + 256) % 255  // 0-254, wrap around, skip 255
    chain.phraseRefs[row] = newRef
}

/**
 * Edit transpose value
 * @param large If true, changes by 12 semitones (octave), else by 1
 */
fun editChainTransposeValue(chain: Chain, row: Int, direction: Int, large: Boolean = false) {
    val currentTranspose = chain.transposeValues[row]
    val step = if (large) 12 else 1
    val newTranspose = (currentTranspose + direction * step).coerceIn(0, 255)
    chain.transposeValues[row] = newTranspose
}

/**
 * Clear chain slot (set to empty)
 */
fun clearChainSlot(chain: Chain, row: Int) {
    chain.phraseRefs[row] = 0xFF
    chain.transposeValues[row] = 0x00  // Reset transpose to default
}

/**
 * Navigate to next/previous chain (B+UP/DOWN)
 */
fun navigateChain(current: Int, direction: Int): Int {
    return (current + direction + 256) % 256  // Wrap 0-255
}

// ═══════════════════════════════════════════════════════════════════════════
// SONG EDITOR HELPERS
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Insert chain reference in song track
 * Returns true if successfully inserted
 */
fun insertSongChainRef(track: Track, row: Int, lastChainValue: Int = 0): Boolean {
    // Expand track list if needed
    while (track.chainRefs.size <= row) {
        track.chainRefs.add(-1)  // Fill with empty slots
    }

    // If empty, insert
    if (row < track.chainRefs.size && track.chainRefs[row] == -1) {
        track.chainRefs[row] = lastChainValue
        return true
    }
    return false
}

/**
 * Edit chain reference in song track
 */
fun editSongChainRef(track: Track, row: Int, direction: Int) {
    if (row >= track.chainRefs.size) return

    val currentRef = track.chainRefs[row]
    if (currentRef == -1) return  // Empty, can't edit

    // Cycle 0-255
    val newRef = (currentRef + direction + 256) % 256
    track.chainRefs[row] = newRef
}

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

// ═══════════════════════════════════════════════════════════════════════════
// PROJECT SCREEN HELPERS
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Handle LEFT button for Project screen
 * Each row has different column limits
 */
fun handleProjectCursorLeft(cursorRow: Int, cursorColumn: Int): Int {
    val minColumn = getProjectMinColumn(cursorRow)
    if (cursorColumn > minColumn) {
        return cursorColumn - 1
    }
    return cursorColumn  // Already at leftmost
}

/**
 * Handle RIGHT button for Project screen
 */
fun handleProjectCursorRight(cursorRow: Int, cursorColumn: Int): Int {
    val maxColumn = getProjectMaxColumn(cursorRow)
    if (cursorColumn < maxColumn) {
        return cursorColumn + 1
    }
    return cursorColumn  // Already at rightmost
}

/**
 * Get MINIMUM cursor column for each Project row
 * Column 0 is always the label (not selectable)
 */
fun getProjectMinColumn(row: Int): Int {
    return 1  // Always 1 (column 0 is the label)
}

/**
 * Get MAXIMUM cursor column for each Project row
 */
fun getProjectMaxColumn(row: Int): Int {
    return when (row) {
        0 -> 1      // TEMPO: only 1 value column
        1 -> 1      // TRANSPOSE: only 1 value column
        2 -> 12     // NAME: 12 character positions
        3 -> 3      // PROJECT: LOAD, SAVE, NEW (3 options)
        4 -> 1      // EXPORT: only 1 value column
        5 -> 1      // CLEAN: only 1 value column
        6 -> 1      // SYSTEM: only 1 value column
        else -> 1
    }
}
