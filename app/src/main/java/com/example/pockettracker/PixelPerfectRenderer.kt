package com.example.pockettracker

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.translate
import androidx.compose.ui.platform.LocalDensity
import kotlin.math.min
import kotlinx.coroutines.delay
import android.os.SystemClock

/**
 * PIXEL-PERFECT TRACKER - MODULAR VERSION
 *
 * Screen: 640×480 (4:3 handheld)
 * Font: 15×15 pixels (5×5 bitmap × 3)
 * Rows: 21px each
 *
 * Modular architecture:
 * - OscilloscopeModule (620×70)
 * - PhraseEditorModule (620×392)
 * - NavigationMapModule (80×105) - NEW!
 * - Easy to add more modules!
 */

// Design constants
const val DESIGN_WIDTH_PX = 640
const val DESIGN_HEIGHT_PX = 480
const val SCREEN_SPACER = 6      // Space between modules
const val SIDE_SPACER = 10       // Space on sides

// PHASE 2: Feature flag for queue-based playback
// Set to true to use sample-accurate C++ note queue
// Set to false to use old Kotlin timing (for comparison)
const val USE_NOTE_QUEUE = true

@Composable
fun PixelPerfectTracker(
    currentScreen: ScreenType,
    project: Project,
    audioEngine: TrackerAudioEngine,
    cursorRow: Int,
    cursorColumn: Int,
    isPlaying: Boolean,
    previousColumn: Int,
    currentChain: Int,
    currentPhrase: Int,
    projectCursorRow: Int,
    projectCursorColumn: Int,
    projectStatusMessage: String,
    projectStatusSuccess: Boolean,
    projectVersion: Int,
    currentInstrument: Int,
    instrumentCursorRow: Int,
    instrumentCursorColumn: Int,
    instrumentStatusMessage: String,
    instrumentStatusSuccess: Boolean,
    fileBrowserState: FileBrowserModule.State? = null
) {
    android.util.Log.d("PixelPerfectTracker", "==== PixelPerfectTracker called ====")
    android.util.Log.d("PixelPerfectTracker", "Screen: $currentScreen")
    android.util.Log.d("PixelPerfectTracker", "Instrument cursor: row=$instrumentCursorRow, col=$instrumentCursorColumn")

    if (currentScreen == ScreenType.FILE_BROWSER) {
        android.util.Log.d("PixelPerfectTracker", "FILE_BROWSER screen, fileBrowserState=${if (fileBrowserState != null) "not null (${fileBrowserState.items.size} items)" else "NULL"}")
    }
    // Playback state
    var playbackRow by remember { mutableStateOf(0) }
    var playbackChainRow by remember { mutableStateOf(0) }
    var playbackPhraseStep by remember { mutableStateOf(0) }
    var playbackSongRow by remember { mutableStateOf(0) }

    // Playback loop - handles both Phrase and Chain screens
    LaunchedEffect(isPlaying, currentScreen) {
        if (isPlaying) {
            when (currentScreen) {
                ScreenType.PHRASE -> {
                    if (USE_NOTE_QUEUE) {
                        try {
                            // CRITICAL: Clear any leftover notes from previous playback
                            audioEngine.clearScheduledNotes()

                            // PHASE 2: Queue-based playback (sample-accurate timing)
                            val sampleRate = audioEngine.getDeviceSampleRate()
                            val msPerStep = (60000.0 / project.tempo / 4.0)
                            val framesPerStep = (msPerStep * sampleRate / 1000.0).toLong()
                            val framesPerPhrase = 16 * framesPerStep

                            // Maintain a continuous buffer of scheduled phrases
                            val lookaheadMs = 50L  // Minimal delay for responsive start
                            val lookaheadFrames = (lookaheadMs * sampleRate / 1000.0).toLong()
                            val bufferPhrases = 2  // Keep 2 phrases queued ahead for stability

                            // Start scheduling from current frame + lookahead
                            val playbackStartFrame = audioEngine.getCurrentFrame() + lookaheadFrames
                            var nextPhraseStartFrame = playbackStartFrame

                            // Helper function to schedule a single phrase
                            fun schedulePhrase(startFrame: Long) {
                                for (step in 0..15) {
                                    val targetFrame = startFrame + (step * framesPerStep)
                                    val phraseStep = project.phrases[currentPhrase].steps[step]

                                    if (!phraseStep.isEmpty()) {
                                        audioEngine.scheduleNote(
                                            targetFrame = targetFrame,
                                            note = phraseStep.note,
                                            instrumentId = phraseStep.instrument,
                                            trackId = 0,
                                            volume = phraseStep.volume / 255f,
                                            project = project
                                        )
                                    }
                                }
                            }

                            // Initial fill: schedule first phrase, then add to buffer gradually
                            schedulePhrase(nextPhraseStartFrame)
                            nextPhraseStartFrame += framesPerPhrase

                            // Continuous scheduling loop
                            while (isPlaying) {
                                val currentFrame = audioEngine.getCurrentFrame()

                                // Maintain 2-phrase buffer: schedule more when buffer gets low
                                val bufferRemaining = nextPhraseStartFrame - currentFrame
                                if (bufferRemaining < (bufferPhrases * framesPerPhrase)) {
                                    schedulePhrase(nextPhraseStartFrame)
                                    nextPhraseStartFrame += framesPerPhrase
                                }

                                // Update playback cursor based on current audio position
                                // Guard against negative values during startup
                                val framesIntoPlayback = maxOf(0L, currentFrame - playbackStartFrame)
                                val framesIntoPhrase = framesIntoPlayback % framesPerPhrase
                                playbackRow = (framesIntoPhrase / framesPerStep).toInt().coerceIn(0, 15)

                                // Update UI at display refresh rate (not audio rate)
                                delay(msPerStep.toLong())
                            }
                        } finally {
                            // CRITICAL: Clean up scheduled notes when playback stops
                            audioEngine.clearScheduledNotes()
                        }
                    } else {
                        // OLD: Kotlin timing (for comparison)
                        val stepDurationMs = (60000.0 / project.tempo / 4.0)
                        val startTime = SystemClock.elapsedRealtime().toDouble()
                        var stepCounter = 0

                        while (isPlaying) {
                            // Calculate target time for THIS step (not next step)
                            val targetTime = startTime + (stepCounter * stepDurationMs)
                            val currentTime = SystemClock.elapsedRealtime().toDouble()
                            val waitTime = targetTime - currentTime

                            // Hybrid timing: delay() for bulk, spin-wait for precision
                            if (waitTime > 2.0) {
                                // Use delay() for most of the wait (efficient, lets other threads run)
                                delay((waitTime - 1.5).toLong())
                            }

                            // Spin-wait for the last ~1.5ms for precise timing
                            while (SystemClock.elapsedRealtime().toDouble() < targetTime && isPlaying) {
                                // Busy-wait (precise but uses CPU)
                            }

                            // Now play the note at the precise target time
                            val step = project.phrases[currentPhrase].steps[playbackRow]
                            if (!step.isEmpty()) {
                                audioEngine.playNote(step.note, step.instrument, 0, step.volume / 255f, project)
                            }

                            playbackRow = (playbackRow + 1) % 16
                            stepCounter++
                        }
                    }
                }
                ScreenType.CHAIN -> {
                    if (USE_NOTE_QUEUE) {
                        try {
                            // CRITICAL: Clear any leftover notes from previous playback
                            audioEngine.clearScheduledNotes()

                            // PHASE 3: Queue-based chain playback (sample-accurate timing with transpose)
                            val chain = project.chains[currentChain]
                            val sampleRate = audioEngine.getDeviceSampleRate()
                            val msPerStep = (60000.0 / project.tempo / 4.0)
                            val framesPerStep = (msPerStep * sampleRate / 1000.0).toLong()
                            val framesPerPhrase = 16 * framesPerStep

                            // Maintain a continuous buffer
                            val lookaheadMs = 50L
                            val lookaheadFrames = (lookaheadMs * sampleRate / 1000.0).toLong()
                            val bufferPhrases = 2  // Keep 2 phrases queued ahead

                            // Start scheduling from current frame + lookahead
                            val playbackStartFrame = audioEngine.getCurrentFrame() + lookaheadFrames
                            var nextPhraseStartFrame = playbackStartFrame

                            // Track scheduled chain rows to map audio position to chain row
                            val scheduledRows = mutableListOf<Int>()
                            var nextRowToSchedule = playbackChainRow

                            // Helper function to find next non-empty chain row
                            fun findNextNonEmptyRow(startRow: Int): Int? {
                                var row = startRow
                                var attempts = 0
                                while (chain.isEmpty(row) && attempts < 16) {
                                    row = (row + 1) % 16
                                    attempts++
                                }
                                return if (attempts >= 16) null else row
                            }

                            // Helper function to schedule a single phrase from chain
                            fun scheduleChainPhrase(startFrame: Long, chainRow: Int) {
                                val phraseRef = chain.phraseRefs[chainRow]
                                val transposeSemitones = chain.getTransposeSemitones(chainRow)

                                for (step in 0..15) {
                                    val targetFrame = startFrame + (step * framesPerStep)
                                    val phraseStep = project.phrases[phraseRef].steps[step]

                                    if (!phraseStep.isEmpty()) {
                                        // Apply transpose to the note
                                        val originalMidi = phraseStep.note.toMidi()
                                        if (originalMidi >= 0) {
                                            val transposedMidi = (originalMidi + transposeSemitones).coerceIn(0, 127)
                                            val transposedNote = Note.fromMidi(transposedMidi)

                                            audioEngine.scheduleNote(
                                                targetFrame = targetFrame,
                                                note = transposedNote,
                                                instrumentId = phraseStep.instrument,
                                                trackId = 0,
                                                volume = phraseStep.volume / 255f,
                                                project = project
                                            )
                                        }
                                    }
                                }
                            }

                            // Find first non-empty row
                            val firstRow = findNextNonEmptyRow(nextRowToSchedule)
                            if (firstRow == null) {
                                // Chain is completely empty, stop playback
                                // (isPlaying will be set to false externally)
                            } else {
                                // Initial fill: schedule first phrase
                                scheduleChainPhrase(nextPhraseStartFrame, firstRow)
                                scheduledRows.add(firstRow)
                                nextPhraseStartFrame += framesPerPhrase
                                nextRowToSchedule = (firstRow + 1) % 16

                                // Continuous scheduling loop
                                while (isPlaying) {
                                    val currentFrame = audioEngine.getCurrentFrame()

                                    // Maintain 2-phrase buffer
                                    val bufferRemaining = nextPhraseStartFrame - currentFrame
                                    if (bufferRemaining < (bufferPhrases * framesPerPhrase)) {
                                        // Find next non-empty row
                                        val nextRow = findNextNonEmptyRow(nextRowToSchedule)
                                        if (nextRow != null) {
                                            scheduleChainPhrase(nextPhraseStartFrame, nextRow)
                                            scheduledRows.add(nextRow)
                                            nextPhraseStartFrame += framesPerPhrase
                                            nextRowToSchedule = (nextRow + 1) % 16
                                        }
                                    }

                                    // Calculate which phrase is currently playing based on audio position
                                    val framesIntoPlayback = maxOf(0L, currentFrame - playbackStartFrame)
                                    val currentPhraseIndex = (framesIntoPlayback / framesPerPhrase).toInt()
                                    val framesIntoPhrase = framesIntoPlayback % framesPerPhrase

                                    // Map phrase index to actual chain row
                                    if (currentPhraseIndex < scheduledRows.size) {
                                        playbackChainRow = scheduledRows[currentPhraseIndex]
                                    }

                                    playbackRow = (framesIntoPhrase / framesPerStep).toInt().coerceIn(0, 15)
                                    playbackPhraseStep = playbackRow

                                    delay(msPerStep.toLong())
                                }
                            }
                        } finally {
                            // CRITICAL: Clean up scheduled notes when playback stops
                            audioEngine.clearScheduledNotes()
                        }
                    } else {
                        // OLD: Kotlin timing for chain playback
                        val chain = project.chains[currentChain]
                        val stepDurationMs = (60000.0 / project.tempo / 4.0)
                        val startTime = SystemClock.elapsedRealtime().toDouble()
                        var stepCounter = 0

                    while (isPlaying) {
                        // Find next non-empty chain row
                        var attempts = 0
                        while (chain.isEmpty(playbackChainRow) && attempts < 16) {
                            playbackChainRow = (playbackChainRow + 1) % 16
                            attempts++
                        }

                        // If all rows are empty, stop playback
                        if (attempts >= 16) {
                            break
                        }

                        // Get phrase reference and transpose value
                        val phraseRef = chain.phraseRefs[playbackChainRow]
                        val transposeSemitones = chain.getTransposeSemitones(playbackChainRow)

                        // Play this phrase (all 16 steps)
                        for (stepIndex in 0..15) {
                            if (!isPlaying) break

                            // Calculate target time and wait with hybrid timing
                            val targetTime = startTime + (stepCounter * stepDurationMs)
                            val currentTime = SystemClock.elapsedRealtime().toDouble()
                            val waitTime = targetTime - currentTime

                            // Hybrid timing: delay() for bulk, spin-wait for precision
                            if (waitTime > 2.0) {
                                delay((waitTime - 1.5).toLong())
                            }
                            while (SystemClock.elapsedRealtime().toDouble() < targetTime && isPlaying) {
                                // Spin-wait for precision
                            }

                            // Now play note at precise time
                            val step = project.phrases[phraseRef].steps[stepIndex]
                            if (!step.isEmpty()) {
                                // Apply transpose to the note
                                val originalMidi = step.note.toMidi()
                                if (originalMidi >= 0) {
                                    val transposedMidi = (originalMidi + transposeSemitones).coerceIn(0, 127)
                                    val transposedNote = Note.fromMidi(transposedMidi)
                                    audioEngine.playNote(transposedNote, step.instrument, 0, step.volume / 255f, project)
                                }
                            }

                            playbackPhraseStep = stepIndex
                            stepCounter++
                        }

                        // Move to next chain row
                        playbackChainRow = (playbackChainRow + 1) % 16
                    }
                    }
                }
                ScreenType.SONG -> {
                    if (USE_NOTE_QUEUE) {
                        try {
                            // CRITICAL: Clear any leftover notes from previous playback
                            audioEngine.clearScheduledNotes()

                            // PHASE 3: Queue-based song playback (8-track polyphonic with sample-accurate timing)
                            val sampleRate = audioEngine.getDeviceSampleRate()
                            val msPerStep = (60000.0 / project.tempo / 4.0)
                            val framesPerStep = (msPerStep * sampleRate / 1000.0).toLong()
                            val framesPerPhrase = 16 * framesPerStep

                            // Maintain a continuous buffer
                            val lookaheadMs = 50L
                            val lookaheadFrames = (lookaheadMs * sampleRate / 1000.0).toLong()
                            val bufferPhrases = 2  // Keep 2 phrases queued ahead

                            // Start scheduling from current frame + lookahead
                            val playbackStartFrame = audioEngine.getCurrentFrame() + lookaheadFrames
                            var nextPhraseStartFrame = playbackStartFrame

                            // Track current position in song
                            var currentSongRow = playbackSongRow
                            var currentChainRow = playbackChainRow

                            // Data class to track each track's chain state
                            data class TrackState(
                                var chainId: Int,
                                var chainRow: Int,
                                var maxChainLength: Int
                            )

                            // Helper function to get active tracks at a song row
                            fun getActiveTracksAtSongRow(songRow: Int): List<TrackState> {
                                val activeTracks = mutableListOf<TrackState>()
                                for (trackId in 0..7) {
                                    val track = project.tracks[trackId]
                                    if (songRow < track.chainRefs.size) {
                                        val chainId = track.chainRefs[songRow]
                                        if (chainId >= 0 && chainId < 256) {
                                            val chain = project.chains[chainId]
                                            // Count non-empty rows in this chain
                                            var maxLength = 0
                                            for (i in 0..15) {
                                                if (!chain.isEmpty(i)) maxLength = i + 1
                                            }
                                            if (maxLength > 0) {
                                                activeTracks.add(TrackState(chainId, 0, maxLength))
                                            }
                                        }
                                    }
                                }
                                return activeTracks
                            }

                            // Helper function to schedule one phrase for all tracks
                            fun scheduleAllTracksAtPosition(startFrame: Long, songRow: Int, chainRow: Int) {
                                for (trackId in 0..7) {
                                    val track = project.tracks[trackId]
                                    if (songRow < track.chainRefs.size) {
                                        val chainId = track.chainRefs[songRow]
                                        if (chainId >= 0 && chainId < 256) {
                                            val chain = project.chains[chainId]
                                            if (!chain.isEmpty(chainRow)) {
                                                val phraseRef = chain.phraseRefs[chainRow]
                                                val transposeSemitones = chain.getTransposeSemitones(chainRow)

                                                // Schedule all 16 steps for this track
                                                for (step in 0..15) {
                                                    val targetFrame = startFrame + (step * framesPerStep)
                                                    val phraseStep = project.phrases[phraseRef].steps[step]

                                                    if (!phraseStep.isEmpty()) {
                                                        // Apply transpose to the note
                                                        val originalMidi = phraseStep.note.toMidi()
                                                        if (originalMidi >= 0) {
                                                            val transposedMidi = (originalMidi + transposeSemitones).coerceIn(0, 127)
                                                            val transposedNote = Note.fromMidi(transposedMidi)

                                                            audioEngine.scheduleNote(
                                                                targetFrame = targetFrame,
                                                                note = transposedNote,
                                                                instrumentId = phraseStep.instrument,
                                                                trackId = trackId,  // Use actual trackId for voice assignment
                                                                volume = phraseStep.volume / 255f,
                                                                project = project
                                                            )
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            // Get initial tracks
                            var activeTracks = getActiveTracksAtSongRow(currentSongRow)
                            if (activeTracks.isEmpty()) {
                                // No active tracks, stop playback
                            } else {
                                // Find max chain length among active tracks
                                var maxChainLength = activeTracks.maxOf { it.maxChainLength }

                                // Initial fill: schedule first phrase for all tracks
                                scheduleAllTracksAtPosition(nextPhraseStartFrame, currentSongRow, currentChainRow)
                                nextPhraseStartFrame += framesPerPhrase
                                currentChainRow++

                                // Continuous scheduling loop
                                while (isPlaying) {
                                    val currentFrame = audioEngine.getCurrentFrame()

                                    // Maintain 2-phrase buffer
                                    val bufferRemaining = nextPhraseStartFrame - currentFrame
                                    if (bufferRemaining < (bufferPhrases * framesPerPhrase)) {
                                        // Check if we need to advance to next song row
                                        if (currentChainRow >= maxChainLength) {
                                            // Move to next song row
                                            currentSongRow = (currentSongRow + 1) % 256
                                            currentChainRow = 0
                                            activeTracks = getActiveTracksAtSongRow(currentSongRow)
                                            maxChainLength = activeTracks.maxOfOrNull { it.maxChainLength } ?: 0

                                            if (maxChainLength == 0) {
                                                // No more active tracks, could loop song or stop
                                                currentSongRow = 0
                                                activeTracks = getActiveTracksAtSongRow(currentSongRow)
                                                maxChainLength = activeTracks.maxOfOrNull { it.maxChainLength } ?: 0
                                            }
                                        }

                                        // Schedule next phrase for all tracks
                                        if (currentChainRow < maxChainLength) {
                                            scheduleAllTracksAtPosition(nextPhraseStartFrame, currentSongRow, currentChainRow)
                                            nextPhraseStartFrame += framesPerPhrase
                                            currentChainRow++
                                        }
                                    }

                                    // Update playback cursor
                                    val framesIntoPlayback = maxOf(0L, currentFrame - playbackStartFrame)
                                    val framesIntoPhrase = framesIntoPlayback % framesPerPhrase
                                    playbackPhraseStep = (framesIntoPhrase / framesPerStep).toInt().coerceIn(0, 15)
                                    playbackSongRow = currentSongRow
                                    playbackChainRow = currentChainRow

                                    delay(msPerStep.toLong())
                                }
                            }
                        } finally {
                            // CRITICAL: Clean up scheduled notes when playback stops
                            audioEngine.clearScheduledNotes()
                        }
                    } else {
                        // OLD: Kotlin timing for song playback
                        // Calculate step duration in milliseconds (16th note)
                        val stepDurationMs = (60000.0 / project.tempo / 4.0)

                        // Use absolute timing with double precision to prevent drift
                        val startTime = SystemClock.elapsedRealtime().toDouble()
                        var stepCounter = 0

                    while (isPlaying) {
                        // Find the longest chain length at current song row
                        var maxChainLength = 0
                        for (trackId in 0..7) {
                            val track = project.tracks[trackId]
                            if (playbackSongRow < track.chainRefs.size) {
                                val chainId = track.chainRefs[playbackSongRow]
                                if (chainId >= 0) {
                                    val chain = project.chains[chainId]
                                    // Count non-empty rows in this chain
                                    var chainLength = 0
                                    for (i in 0..15) {
                                        if (!chain.isEmpty(i)) chainLength = i + 1
                                    }
                                    if (chainLength > maxChainLength) maxChainLength = chainLength
                                }
                            }
                        }

                        // If no chains at this song row, move to next song row
                        if (maxChainLength == 0) {
                            playbackSongRow = (playbackSongRow + 1) % 256
                            playbackChainRow = 0
                            continue
                        }

                        // Play through all chain rows for all active chains
                        while (playbackChainRow < maxChainLength && isPlaying) {
                            // Play the phrase at this chain row for all tracks
                            // All 16 steps are played with all tracks in sync
                            for (stepIndex in 0..15) {
                                if (!isPlaying) break

                                // Calculate target time and wait with hybrid timing
                                val targetTime = startTime + (stepCounter * stepDurationMs)
                                val currentTime = SystemClock.elapsedRealtime().toDouble()
                                val waitTime = targetTime - currentTime

                                // Hybrid timing: delay() for bulk, spin-wait for precision
                                if (waitTime > 2.0) {
                                    delay((waitTime - 1.5).toLong())
                                }
                                while (SystemClock.elapsedRealtime().toDouble() < targetTime && isPlaying) {
                                    // Spin-wait for precision
                                }

                                // Now play all 8 tracks simultaneously at precise time
                                for (trackId in 0..7) {
                                    val track = project.tracks[trackId]

                                    // Check if this track has a chain at current song row
                                    if (playbackSongRow < track.chainRefs.size) {
                                        val chainId = track.chainRefs[playbackSongRow]

                                        if (chainId >= 0 && chainId < 256) {
                                            val chain = project.chains[chainId]

                                            // Check if this chain row has a phrase
                                            if (!chain.isEmpty(playbackChainRow)) {
                                                val phraseRef = chain.phraseRefs[playbackChainRow]
                                                val transposeSemitones = chain.getTransposeSemitones(playbackChainRow)

                                                val step = project.phrases[phraseRef].steps[stepIndex]
                                                if (!step.isEmpty()) {
                                                    // Apply transpose to the note
                                                    val originalMidi = step.note.toMidi()
                                                    if (originalMidi >= 0) {
                                                        val transposedMidi = (originalMidi + transposeSemitones).coerceIn(0, 127)
                                                        val transposedNote = Note.fromMidi(transposedMidi)
                                                        // Use trackId as the track parameter for voice assignment
                                                        audioEngine.playNote(transposedNote, step.instrument, trackId, step.volume / 255f, project)
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }

                                // Update playback position
                                playbackPhraseStep = stepIndex
                                stepCounter++
                            }

                            // Move to next chain row
                            playbackChainRow++
                        }

                        // Finished all chain rows, move to next song row
                        playbackSongRow = (playbackSongRow + 1) % 256
                        playbackChainRow = 0
                    }
                    }
                }
                else -> {
                    // Other screens don't support playback yet
                }
            }
        } else {
            playbackRow = 0
            playbackChainRow = 0
            playbackPhraseStep = 0
            playbackSongRow = 0
            audioEngine.stopAll()
        }
    }

    val density = LocalDensity.current

    // Main container with letterboxing
    BoxWithConstraints(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black)
    ) {
        // Calculate scale factor
        val screenWidthPx = with(density) { maxWidth.toPx() }.toInt()
        val screenHeightPx = with(density) { maxHeight.toPx() }.toInt()

        val scaleX = screenWidthPx / DESIGN_WIDTH_PX
        val scaleY = screenHeightPx / DESIGN_HEIGHT_PX
        val scale = min(scaleX, scaleY).coerceAtLeast(1)

        val renderWidth = DESIGN_WIDTH_PX * scale
        val renderHeight = DESIGN_HEIGHT_PX * scale

        // Center the content
        val offsetX = (screenWidthPx - renderWidth) / 2f
        val offsetY = (screenHeightPx - renderHeight) / 2f

        // Main canvas - use key() to force redraw when state changes
        // Key on: projectVersion, screen type, and all cursor positions
        key(projectVersion, currentScreen, cursorRow, cursorColumn, projectCursorRow, projectCursorColumn, instrumentCursorRow, instrumentCursorColumn) {
            Canvas(modifier = Modifier.fillMaxSize()) {
                translate(offsetX, offsetY) {
                    // Use layout manager to draw modules
                    val layout = TrackerLayout()
                    with(layout) {
                        drawLayout(
                            scale = scale,
                            currentScreen = currentScreen,
                            project = project,
                            cursorRow = cursorRow,
                            cursorColumn = cursorColumn,
                            isPlaying = isPlaying,
                            playbackRow = playbackRow,
                            playbackChainRow = playbackChainRow,
                            playbackSongRow = playbackSongRow,
                            audioEngine = audioEngine,
                            previousColumn = previousColumn,
                            currentChain = currentChain,
                            currentPhrase = currentPhrase,
                            projectCursorRow = projectCursorRow,
                            projectCursorColumn = projectCursorColumn,
                            projectStatusMessage = projectStatusMessage,
                            projectStatusSuccess = projectStatusSuccess,
                            projectVersion = projectVersion,
                            currentInstrument = currentInstrument,
                            instrumentCursorRow = instrumentCursorRow,
                            instrumentCursorColumn = instrumentCursorColumn,
                            instrumentStatusMessage = instrumentStatusMessage,
                            instrumentStatusSuccess = instrumentStatusSuccess,
                            fileBrowserState = fileBrowserState
                        )
                    }
                }
            }
        }
    }
}

/**
 * LAYOUT MANAGER
 *
 * Positions and draws all modules
 * This is where you "build" your screen layout!
 */
class TrackerLayout {
    // ===================================
    // CREATE MODULE INSTANCES
    // ===================================
    private val oscilloscope = OscilloscopeModule(width = 620, height = 70)
    private val phraseEditor = PhraseEditorModule()
    private val navigationMap = NavigationMapModule()
    private val instrumentModule = InstrumentModule()
    private val chainEditor = ChainEditorModule()
    private val songEditor = SongEditorModule()
    private val projectModule = ProjectModule()
    private val fileBrowser = FileBrowserModule()
    /**
     * Main layout drawing function
     * This arranges all modules on the 640×480 screen
     */
    fun DrawScope.drawLayout(
        scale: Int,
        currentScreen: ScreenType,
        project: Project,
        cursorRow: Int,
        cursorColumn: Int,
        isPlaying: Boolean,
        playbackRow: Int,
        playbackChainRow: Int,
        playbackSongRow: Int,
        audioEngine: TrackerAudioEngine,
        previousColumn: Int,
        currentChain: Int,
        currentPhrase: Int = 0,
        projectCursorRow: Int = 0,
        projectCursorColumn: Int = 1,
        projectStatusMessage: String = "",
        projectStatusSuccess: Boolean = true,
        projectVersion: Int = 0,  // Version counter to force redraw on data changes
        currentInstrument: Int = 0,
        instrumentCursorRow: Int = 0,
        instrumentCursorColumn: Int = 1,
        instrumentStatusMessage: String = "",
        instrumentStatusSuccess: Boolean = true,
        fileBrowserState: FileBrowserModule.State? = null  // File browser state
    ) {
        // ===================================
        // DRAW BACKGROUND
        // ===================================
        drawRect(
            color = Color(0xFF0a0a0a),  // Dark background
            topLeft = Offset.Zero,
            size = Size(
                (DESIGN_WIDTH_PX * scale).toFloat(),
                (DESIGN_HEIGHT_PX * scale).toFloat()
            )
        )

        // ===================================
        // LEFT SIDE: Main content modules (620px wide)
        // ===================================

        // Start position: 10px from left edge (centers 620px in 640px)
        val moduleX = SIDE_SPACER
        var currentY = SCREEN_SPACER  // Start 6px from top

        // MODULE 1: OSCILLOSCOPE (waveform display)
        // Position: Top of screen
        // Size: 620×70
        with(oscilloscope) {
            draw(
                x = moduleX,
                y = currentY,
                scale = scale,
                state = audioEngine.waveformBuffer  // Pass audio waveform data
            )
        }
        // Move down for next module
        currentY += oscilloscope.height + SCREEN_SPACER  // 70 + 6 = 76px

        // MODULE 2: PHRASE EDITOR (main content)
        // Position: Below oscilloscope
        // Size: 620×392
        with(phraseEditor) {
            draw(
                x = moduleX,
                y = currentY,
                scale = scale,
                state = PhraseEditorState(
                    phrase = project.phrases[currentPhrase],
                    cursorRow = cursorRow,
                    cursorColumn = cursorColumn,
                    playbackRow = playbackRow,
                    isPlaying = isPlaying
                )
            )
        }

        // MODULE 2: Switch between editors based on current screen
        when (currentScreen) {
            ScreenType.PROJECT -> {
                with(projectModule) {
                    draw(
                        x = moduleX,
                        y = currentY,
                        scale = scale,
                        state = ProjectState(
                            project = project,
                            cursorRow = projectCursorRow,  // Pass cursor state
                            cursorColumn = projectCursorColumn,
                            statusMessage = projectStatusMessage,
                            isSuccess = projectStatusSuccess
                        )
                    )
                }
            }
            // ===================================
            // PHRASE SCREEN: Show phrase editor
            // ===================================
            ScreenType.PHRASE -> {
                with(phraseEditor) {
                    draw(
                        x = moduleX,
                        y = currentY,
                        scale = scale,
                        state = PhraseEditorState(
                            phrase = project.phrases[currentPhrase],
                            cursorRow = cursorRow,
                            cursorColumn = cursorColumn,
                            playbackRow = playbackRow,
                            isPlaying = isPlaying
                        )
                    )
                }
            }

            // ===================================
            // CHAIN SCREEN: Show chain editor
            // ===================================
            ScreenType.CHAIN -> {
                with(chainEditor) {
                    draw(
                        x = moduleX,
                        y = currentY,
                        scale = scale,
                        state = ChainEditorState(
                            chain = project.chains[currentChain],
                            cursorRow = cursorRow,
                            cursorColumn = cursorColumn,
                            playbackRow = playbackChainRow,
                            isPlaying = isPlaying
                        )
                    )
                }
            }

            // ===================================
            // SONG SCREEN: Show song editor
            // ===================================
            ScreenType.SONG -> {
                with(songEditor) {
                    draw(
                        x = moduleX,
                        y = currentY,
                        scale = scale,
                        state = SongEditorState(
                            project = project,
                            cursorRow = cursorRow,
                            cursorTrack = cursorColumn,  // Use cursorColumn as track selector
                            isPlaying = isPlaying && currentScreen == ScreenType.SONG,
                            playbackRow = playbackSongRow
                        )
                    )
                }
            }

            // ===================================
            // INSTRUMENT SCREEN: Show instrument editor
            // ===================================
            ScreenType.INSTRUMENT -> {
                with(instrumentModule) {
                    draw(
                        x = moduleX,
                        y = currentY,
                        scale = scale,
                        state = InstrumentState(
                            instrument = project.instruments[currentInstrument],
                            cursorRow = instrumentCursorRow,
                            cursorColumn = instrumentCursorColumn,
                            statusMessage = instrumentStatusMessage,
                            isSuccess = instrumentStatusSuccess
                        )
                    )
                }
            }

            // ===================================
            // FILE BROWSER: Full screen file selection
            // ===================================
            ScreenType.FILE_BROWSER -> {
                android.util.Log.d("FileBrowser", "Rendering FILE_BROWSER, state=${if (fileBrowserState != null) "not null" else "NULL"}")
                if (fileBrowserState != null) {
                    with(fileBrowser) {
                        draw(
                            x = 0,  // Full screen: start at 0, 0
                            y = 0,
                            scale = scale,
                            state = fileBrowserState
                        )
                    }
                } else {
                    android.util.Log.e("FileBrowser", "fileBrowserState is NULL - cannot render!")
                }
            }

            // ===================================
            // OTHER SCREENS: Show placeholder
            // ===================================
            else -> {
                drawPlaceholderScreen(
                    x = moduleX,
                    y = currentY,
                    scale = scale,
                    screenType = currentScreen
                )
            }
        }

        // Left side total: 6 + 70 + 6 + 392 + 6 = 480px ✓

        // ===================================
        // RIGHT SIDE: Navigation map (80px wide)
        // ===================================
        // Note: Hidden when FILE_BROWSER is active to give full screen space

        if (currentScreen != ScreenType.FILE_BROWSER) {
            // Calculate position for bottom-right corner
            // X position: 640 (screen width) - 80 (module width) - 10 (right margin) = 550px
            val navMapX = DESIGN_WIDTH_PX - navigationMap.width - SIDE_SPACER

            // Y position: 480 (screen height) - 105 (module height) - 6 (bottom margin) = 369px
            val navMapY = DESIGN_HEIGHT_PX - navigationMap.height - SCREEN_SPACER

            // MODULE 3: NAVIGATION MAP (shows current position in screen hierarchy)
            // Position: Bottom-right corner
            // Size: 80×105
            with(navigationMap) {
                draw(
                    x = navMapX,
                    y = navMapY,
                    scale = scale,
                    state = NavigationMapState(
                        currentScreen = currentScreen,
                        sourceColumn = previousColumn  // ✨ Use the passed value
                    )
                )
            }
        }

        // ===================================
        // LAYOUT COMPLETE!
        // ===================================
        // Left column: Oscilloscope + Phrase Editor (or placeholder)
        // Right corner: Navigation Map
    }

    /**
     * Draw a placeholder screen for screens that don't have modules yet
     * This shows the screen name and a "Coming Soon" message
     */
    private fun DrawScope.drawPlaceholderScreen(
        x: Int,
        y: Int,
        scale: Int,
        screenType: ScreenType
    ) {
        // Draw background (same size as phrase editor: 620×392)
        drawRect(
            color = Color(0xFF0a0a0a),
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size = Size((620 * scale).toFloat(), (392 * scale).toFloat())
        )

        // Draw screen title at top (15px font + 3px padding = starts at y+3)
        val titleY = y + 3
        drawBitmapText(
            text = screenType.label.uppercase(),
            x = x + 20,  // 20px from left edge
            y = titleY,
            scale = scale,
            color = Color.Cyan,
            spacing = 2,
            fontScale = 3  // 15×15 font
        )

        // Draw "COMING SOON" message in center
        // Calculate center position
        val messageY = y + 180  // Roughly centered vertically in 392px
        val message = "COMING SOON"

        // Calculate message width to center it
        // Each char is 5×3=15px wide, plus 2px spacing between chars
        val messageWidth = (message.length * 15) + ((message.length - 1) * 2)
        val messageX = x + (620 - messageWidth) / 2

        drawBitmapText(
            text = message,
            x = messageX,
            y = messageY,
            scale = scale,
            color = Color(0xFF666666),  // Dark gray
            spacing = 2,
            fontScale = 3
        )
    }
}

/**
 * BITMAP FONT RENDERING FUNCTIONS
 *
 * These are used by modules to draw text
 */

fun DrawScope.drawBitmapText(
    text: String,
    x: Int,
    y: Int,
    scale: Int,
    color: Color,
    spacing: Int = 2,
    fontScale: Int = 1
) {
    var currentX = x
    for (char in text) {
        drawBitmapChar(char, currentX, y, scale, color, fontScale)
        currentX += (5 * fontScale) + spacing
    }
}

fun DrawScope.drawBitmapChar(
    char: Char,
    x: Int,
    y: Int,
    scale: Int,
    color: Color,
    fontScale: Int = 1
) {
    val charData = FONT_5X5[char.uppercaseChar()]

    if (charData == null) {
        // Missing character - draw outline square
        drawRect(
            color = color,
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size = androidx.compose.ui.geometry.Size(
                (5 * fontScale * scale).toFloat(),
                (5 * fontScale * scale).toFloat()
            ),
            style = androidx.compose.ui.graphics.drawscope.Stroke(width = (scale * fontScale).toFloat())
        )
        return
    }

    // Draw each pixel of the 5×5 bitmap
    for (row in 0..4) {
        val rowData = charData[row].toInt()
        for (col in 0..4) {
            val isSet = (rowData and (1 shl (4 - col))) != 0
            if (isSet) {
                drawRect(
                    color = color,
                    topLeft = Offset(
                        ((x + col * fontScale) * scale).toFloat(),
                        ((y + row * fontScale) * scale).toFloat()
                    ),
                    size = androidx.compose.ui.geometry.Size(
                        (scale * fontScale).toFloat(),
                        (scale * fontScale).toFloat()
                    )
                )
            }
        }
    }
}

// Font data
private val FONT_5X5 = mapOf(
    '0' to byteArrayOf(0b01110, 0b10001, 0b10001, 0b10001, 0b01110),
    '1' to byteArrayOf(0b00100, 0b01100, 0b00100, 0b00100, 0b01110),
    '2' to byteArrayOf(0b01110, 0b10001, 0b00010, 0b00100, 0b11111),
    '3' to byteArrayOf(0b11111, 0b00010, 0b00110, 0b00001, 0b11110),
    '4' to byteArrayOf(0b10001, 0b10001, 0b11111, 0b00001, 0b00001),
    '5' to byteArrayOf(0b11111, 0b10000, 0b11110, 0b00001, 0b11110),
    '6' to byteArrayOf(0b01110, 0b10000, 0b11110, 0b10001, 0b01110),
    '7' to byteArrayOf(0b11111, 0b00001, 0b00010, 0b00100, 0b01000),
    '8' to byteArrayOf(0b01110, 0b10001, 0b01110, 0b10001, 0b01110),
    '9' to byteArrayOf(0b01110, 0b10001, 0b01111, 0b00001, 0b01110),
    'A' to byteArrayOf(0b01110, 0b10001, 0b11111, 0b10001, 0b10001),
    'B' to byteArrayOf(0b11110, 0b10001, 0b11110, 0b10001, 0b11110),
    'C' to byteArrayOf(0b01110, 0b10001, 0b10000, 0b10001, 0b01110),
    'D' to byteArrayOf(0b11110, 0b10001, 0b10001, 0b10001, 0b11110),
    'E' to byteArrayOf(0b11111, 0b10000, 0b11110, 0b10000, 0b11111),
    'F' to byteArrayOf(0b11111, 0b10000, 0b11110, 0b10000, 0b10000),
    'G' to byteArrayOf(0b01110, 0b10000, 0b10011, 0b10001, 0b01110),
    'H' to byteArrayOf(0b10001, 0b10001, 0b11111, 0b10001, 0b10001),
    'I' to byteArrayOf(0b01110, 0b00100, 0b00100, 0b00100, 0b01110),
    'J' to byteArrayOf(0b00111, 0b00001, 0b00001, 0b10001, 0b01110),
    'K' to byteArrayOf(0b10001, 0b10010, 0b11100, 0b10010, 0b10001),
    'L' to byteArrayOf(0b10000, 0b10000, 0b10000, 0b10000, 0b11111),
    'M' to byteArrayOf(0b10001, 0b11011, 0b10101, 0b10001, 0b10001),
    'N' to byteArrayOf(0b10001, 0b11001, 0b10101, 0b10011, 0b10001),
    'O' to byteArrayOf(0b01110, 0b10001, 0b10001, 0b10001, 0b01110),
    'P' to byteArrayOf(0b11110, 0b10001, 0b11110, 0b10000, 0b10000),
    'Q' to byteArrayOf(0b01110, 0b10001, 0b10001, 0b10010, 0b01101),
    'R' to byteArrayOf(0b11110, 0b10001, 0b11110, 0b10010, 0b10001),
    'S' to byteArrayOf(0b01111, 0b10000, 0b01110, 0b00001, 0b11110),
    'T' to byteArrayOf(0b11111, 0b00100, 0b00100, 0b00100, 0b00100),
    'U' to byteArrayOf(0b10001, 0b10001, 0b10001, 0b10001, 0b01110),
    'V' to byteArrayOf(0b10001, 0b10001, 0b10001, 0b01010, 0b00100),
    'W' to byteArrayOf(0b10001, 0b10001, 0b10101, 0b11011, 0b10001),
    'X' to byteArrayOf(0b10001, 0b01010, 0b00100, 0b01010, 0b10001),
    'Y' to byteArrayOf(0b10001, 0b01010, 0b00100, 0b00100, 0b00100),
    'Z' to byteArrayOf(0b11111, 0b00010, 0b00100, 0b01000, 0b11111),
    '-' to byteArrayOf(0b00000, 0b00000, 0b11111, 0b00000, 0b00000),
    '#' to byteArrayOf(0b01010, 0b11111, 0b01010, 0b11111, 0b01010),
    '.' to byteArrayOf(0b00000, 0b00000, 0b00000, 0b00000, 0b00100),
    ':' to byteArrayOf(0b00000, 0b00100, 0b00000, 0b00100, 0b00000),
    '/' to byteArrayOf(0b00001, 0b00010, 0b00100, 0b01000, 0b10000),
    ' ' to byteArrayOf(0b00000, 0b00000, 0b00000, 0b00000, 0b00000),
)