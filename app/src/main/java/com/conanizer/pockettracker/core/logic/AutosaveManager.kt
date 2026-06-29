package com.conanizer.pockettracker.core.logic

import com.conanizer.pockettracker.core.data.Project
import com.conanizer.pockettracker.core.logging.ILogger
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * AUTOSAVE MANAGER (crash-recovery)
 *
 * Persists the working project to an app-private autosave.ptp so a crash, OOM-kill or force-quit
 * doesn't lose unsaved edits. A clean save/load/new clears the file (see TrackerController), so its
 * presence at next launch signals an unclean exit — that's what the recovery prompt keys on.
 *
 * This class owns only the THREADING split, not the trigger: the debounce lives in the caller (a
 * LaunchedEffect keyed on projectVersion). [autosave] serializes on the caller's thread — which must
 * be the main thread, the project's sole mutator, so the serialization walk can never observe a
 * half-applied edit — then writes the resulting bytes on [ioDispatcher]. The audio engine is native
 * and never touched here, so an autosave can't glitch playback.
 *
 * ✅ PLATFORM-AGNOSTIC — no Android dependencies (coroutines + FileController only).
 */
class AutosaveManager(
    private val fileController: FileController,
    private val logger: ILogger,
    private val ioDispatcher: CoroutineDispatcher = Dispatchers.IO
) {
    private val TAG = "AutosaveManager"

    /**
     * Serialize [project] on the current (main) thread, then write it to the autosave file off the
     * main thread. Never throws — an autosave failure must not disturb the app.
     */
    suspend fun autosave(project: Project) {
        try {
            val jsonString = fileController.serializeProject(project)  // main thread: tear-free
            withContext(ioDispatcher) { fileController.writeAutosave(jsonString) }
        } catch (e: Exception) {
            logger.e(TAG, "❌ Autosave failed: ${e.message}")
        }
    }
}
