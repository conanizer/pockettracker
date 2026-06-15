package com.conanizer.pockettracker.core.logic

import com.conanizer.pockettracker.core.data.PhraseStep
import com.conanizer.pockettracker.core.data.Project

/**
 * Shared song → chain → phrase → step traversal for *static* analysis of the song (e.g. "which
 * instruments does this row range use?"). The render paths each hand-rolled this nested walk together
 * with its bounds guards, so the copies could silently drift; centralizing it removes the duplication
 * and the "did this copy forget the `row < chainRefs.size` guard?" class of bug (REVIEW-3 3.1).
 *
 * Deliberately NOT used by the two other song walks, which have different semantics on purpose:
 *  - `PlaybackController` — the live scheduler walks by playback position / HOP / checkpoints, a
 *    fundamentally different traversal.
 *  - `TrackerController.collectUsedRefs` (CLEAN) — spans the *whole* song, counts muted tracks as
 *    used (so CLEAN can't delete a muted track's content), and gathers chain + phrase + instrument
 *    refs. Folding it in here would change its behavior.
 */

/**
 * Visit every phrase step in song rows [startRow]..[endRow] (inclusive) across all 8 tracks, applying
 * the same bounds guards every time: a track row past the end, or one pointing at an empty chain/phrase
 * slot, is skipped. Muted tracks are skipped unless [includeMuted] is true.
 */
inline fun Project.forEachStepInSongRange(
    startRow: Int,
    endRow: Int,
    includeMuted: Boolean = false,
    action: (PhraseStep) -> Unit
) {
    for (row in startRow..endRow) {
        for (track in tracks) {
            if (!includeMuted && track.mute) continue
            if (row >= track.chainRefs.size) continue
            val chainId = track.chainRefs[row]
            if (chainId !in 0..255) continue
            val chain = chains[chainId]
            for (slot in 0..15) {
                val phraseId = chain.phraseRefs[slot]
                if (phraseId !in 0..255) continue
                for (step in phrases[phraseId].steps) action(step)
            }
        }
    }
}

/**
 * Instrument IDs (0..255) used by any non-empty step in song rows [startRow]..[endRow]. Muted tracks
 * are skipped, matching the render paths (which don't render muted tracks).
 */
fun Project.collectUsedInstruments(startRow: Int, endRow: Int): Set<Int> {
    val used = mutableSetOf<Int>()
    forEachStepInSongRange(startRow, endRow) { step ->
        if (!step.isEmpty() && step.instrument in 0..255) used.add(step.instrument)
    }
    return used
}
