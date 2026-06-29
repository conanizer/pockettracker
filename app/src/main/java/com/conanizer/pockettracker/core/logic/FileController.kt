package com.conanizer.pockettracker.core.logic

import com.conanizer.pockettracker.core.data.Instrument
import com.conanizer.pockettracker.core.data.InstrumentPreset
import com.conanizer.pockettracker.core.data.Project
import com.conanizer.pockettracker.core.data.TableRow
import com.conanizer.pockettracker.core.logging.ILogger
import com.conanizer.pockettracker.core.storage.FileInfo
import com.conanizer.pockettracker.core.storage.FileSortMode
import com.conanizer.pockettracker.core.storage.IFileSystem
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json

/**
 * FILE CONTROLLER
 *
 * Coordinates all project save/load and file-browser operations.
 *
 * ✅ PLATFORM-AGNOSTIC — No Android dependencies. Uses IFileSystem + ILogger interfaces.
 */
class FileController(
    private val fileSystem: IFileSystem,
    private val logger: ILogger
) {
    private val TAG = "FileController"

    private val json = Json {
        prettyPrint = true
        ignoreUnknownKeys = true
    }

    // ========================================
    // DIRECTORY ACCESSORS
    // ========================================

    fun getProjectsDirectory(): String = fileSystem.getProjectsDirectory()
    fun getSamplesDirectory(): String = fileSystem.getSamplesDirectory()
    fun getInstrumentsDirectory(): String = fileSystem.getInstrumentsDirectory()
    fun getSoundfontsDirectory(): String = fileSystem.getSoundfontsDirectory()
    fun getThemesDirectory(): String = fileSystem.getThemesDirectory()

    // ========================================
    // PROJECT OPERATIONS
    // ========================================

    fun saveProject(project: Project, filename: String): SaveResult {
        return try {
            val dirPath = fileSystem.getProjectsDirectory()
            val safeName = filename.replace(Regex("[^a-zA-Z0-9_\\-]"), "_")
            val filePath = "$dirPath/$safeName.ptp"
            val jsonString = json.encodeToString(project)
            val success = fileSystem.writeFile(filePath, jsonString)
            if (success) {
                logger.d(TAG, "✅ Project saved: $filePath")
                SaveResult.Success(filename)
            } else {
                logger.e(TAG, "❌ Save failed: $filename")
                SaveResult.Error("Failed to save project")
            }
        } catch (e: Exception) {
            logger.e(TAG, "❌ Save error: ${e.message}")
            SaveResult.Error(e.message ?: "Unknown error")
        }
    }

    fun loadProject(filename: String): LoadResult {
        return try {
            val dirPath = fileSystem.getProjectsDirectory()
            val filePath = "$dirPath/$filename.ptp"
            if (!fileSystem.fileExists(filePath)) {
                logger.e(TAG, "❌ File not found: $filePath")
                return LoadResult.Error("File not found: $filename")
            }
            val jsonString = fileSystem.readFile(filePath)
            decodeAndMigrate(jsonString, filename)
        } catch (e: Exception) {
            logger.e(TAG, "❌ Load error: ${e.message}")
            LoadResult.Error(e.message ?: "Unknown error")
        }
    }

    fun loadProject(fileInfo: FileInfo): LoadResult {
        return try {
            if (!fileSystem.fileExists(fileInfo.path)) {
                logger.e(TAG, "❌ File not found: ${fileInfo.path}")
                return LoadResult.Error("File not found: ${fileInfo.name}")
            }
            val jsonString = fileSystem.readFile(fileInfo.path)
            decodeAndMigrate(jsonString, fileInfo.name)
        } catch (e: Exception) {
            logger.e(TAG, "❌ Load error: ${e.message}")
            LoadResult.Error(e.message ?: "Unknown error")
        }
    }

    fun listProjects(): List<FileInfo> {
        val dirPath = fileSystem.getProjectsDirectory()
        val files = fileSystem.listFiles(dirPath, extension = "ptp")
        return fileSystem.sortFiles(files, FileSortMode.DATE_DESC)
    }

    fun deleteProject(filename: String): Boolean {
        return try {
            val dirPath = fileSystem.getProjectsDirectory()
            val filePath = "$dirPath/$filename.ptp"
            val deleted = fileSystem.deleteFile(filePath)
            if (deleted) logger.d(TAG, "✅ Deleted project: $filename")
            deleted
        } catch (e: Exception) {
            logger.e(TAG, "❌ Failed to delete project: ${e.message}")
            false
        }
    }

    // ========================================
    // FILE BROWSER OPERATIONS
    // ========================================

    fun deleteFileOrFolder(path: String): Boolean = fileSystem.deleteFileOrFolder(path)

    // ========================================
    // TEMPLATE PROJECT OPERATIONS
    // ========================================

    fun hasTemplate(): Boolean = fileSystem.fileExists(fileSystem.getTemplateProjectPath())

    fun saveTemplate(project: Project): Boolean {
        return try {
            val path = fileSystem.getTemplateProjectPath()
            val jsonString = json.encodeToString(project)
            val success = fileSystem.writeFile(path, jsonString)
            if (success) logger.d(TAG, "✅ Template saved: $path")
            success
        } catch (e: Exception) {
            logger.e(TAG, "❌ Template save error: ${e.message}")
            false
        }
    }

    fun loadTemplate(): LoadResult {
        return try {
            val path = fileSystem.getTemplateProjectPath()
            if (!fileSystem.fileExists(path)) return LoadResult.Error("No template")
            val jsonString = fileSystem.readFile(path)
            decodeAndMigrate(jsonString, "template")
        } catch (e: Exception) {
            logger.e(TAG, "❌ Template load error: ${e.message}")
            LoadResult.Error(e.message ?: "Unknown error")
        }
    }

    fun clearTemplate(): Boolean {
        val path = fileSystem.getTemplateProjectPath()
        return if (fileSystem.fileExists(path)) {
            val deleted = fileSystem.deleteFile(path)
            if (deleted) logger.d(TAG, "✅ Template cleared")
            deleted
        } else {
            true
        }
    }

    // ========================================
    // AUTOSAVE (crash-recovery) OPERATIONS
    // ========================================
    // A separate app-private autosave.ptp, written while there is unsaved work and cleared on every
    // clean save/load/new (see TrackerController). serialize/write are split so the caller can run
    // the serialization on the main thread (the project's sole mutator → tear-free) and the file
    // write on an IO thread; see AutosaveManager.

    /** Serialize a project to its JSON string. Call on the main thread, then hand the result to
     *  [writeAutosave] on an IO thread. */
    fun serializeProject(project: Project): String = json.encodeToString(project)

    /** Write a pre-serialized project JSON to the autosave file (atomic temp+rename in writeFile). */
    fun writeAutosave(jsonString: String): Boolean {
        return try {
            val ok = fileSystem.writeFile(fileSystem.getAutosaveFilePath(), jsonString)
            if (ok) logger.d(TAG, "✅ Autosaved")
            ok
        } catch (e: Exception) {
            logger.e(TAG, "❌ Autosave write error: ${e.message}")
            false
        }
    }

    /** Serialize + write the autosave in one synchronous call — for the onStop flush, which can't
     *  await a coroutine (the process may be killed right after). Call on the main thread (tear-free). */
    fun saveAutosave(project: Project): Boolean = writeAutosave(serializeProject(project))

    /** True if a crash-recovery autosave exists (i.e. the last session didn't exit cleanly). */
    fun hasAutosave(): Boolean = fileSystem.fileExists(fileSystem.getAutosaveFilePath())

    /** Load + migrate the crash-recovery autosave (same decode path as a normal project load). */
    fun loadAutosave(): LoadResult {
        return try {
            val path = fileSystem.getAutosaveFilePath()
            if (!fileSystem.fileExists(path)) return LoadResult.Error("No autosave")
            val jsonString = fileSystem.readFile(path)
            decodeAndMigrate(jsonString, "autosave")
        } catch (e: Exception) {
            logger.e(TAG, "❌ Autosave load error: ${e.message}")
            LoadResult.Error(e.message ?: "Unknown error")
        }
    }

    /** Delete the autosave file, if any. Called on every clean save/load/new. */
    fun clearAutosave(): Boolean {
        val path = fileSystem.getAutosaveFilePath()
        return if (fileSystem.fileExists(path)) fileSystem.deleteFile(path) else true
    }

    // ========================================
    // INSTRUMENT PRESET (.pti) OPERATIONS
    // ========================================

    fun saveInstrumentPreset(instrument: Instrument, tableRows: Array<TableRow>?, path: String): Boolean {
        return try {
            val preset = InstrumentPreset(instrument = instrument, tableRows = tableRows)
            val jsonString = json.encodeToString(preset)
            val success = fileSystem.writeFile(path, jsonString)
            if (success) logger.d(TAG, "✅ Instrument preset saved: $path")
            success
        } catch (e: Exception) {
            logger.e(TAG, "❌ Failed to save instrument preset: ${e.message}")
            false
        }
    }

    fun loadInstrumentPreset(path: String): InstrumentPreset? {
        return try {
            val jsonString = fileSystem.readFile(path)
            json.decodeFromString<InstrumentPreset>(jsonString)
        } catch (e: Exception) {
            logger.e(TAG, "❌ Failed to load instrument preset: ${e.message}")
            null
        }
    }

    // ========================================
    // PROJECT MIGRATION
    // ========================================

    /**
     * Shared load tail for all entry points (loadProject ×2, loadTemplate): validate structure,
     * migrate, log. Keeps validation + migration in one place so every load path is protected.
     * Throwing JSON-parse errors propagate to each caller's try/catch and become LoadResult.Error.
     */
    private fun decodeAndMigrate(jsonString: String, label: String): LoadResult {
        val decoded = json.decodeFromString<Project>(jsonString)
        val project = normalizeProject(decoded, label)
        migrateProject(project)
        logger.d(TAG, "✅ Project loaded: $label")
        return LoadResult.Success(project)
    }

    /**
     * Repair, don't reject. Deserialization with ignoreUnknownKeys=true happily produces wrong-sized
     * pools — a project saved before the instrument/table/groove pools were reduced 256 → 128, a
     * truncated/hand-edited file, or "tracks":[] — which would otherwise crash with an opaque
     * IndexOutOfBounds deep in playback (or, with a strict size check, fail to load at all). Truncate
     * over-long pools and pad short ones with fresh defaults borrowed from a default Project (so the
     * padding can never drift from the canonical sizes in TrackerData.kt). The file then loads with
     * whatever the current model can represent.
     *
     * Refs that point past a truncated pool (e.g. an old phrase step naming instrument 0xC8 after the
     * 256→128 reduction) are already clamped/skipped at schedule time — PlaybackController.scheduleStep
     * coerces the instrument id and AudioEngine.scheduleNote bound-checks it — so truncation can't
     * crash playback.
     *
     * Returns the input unchanged (no allocation) when every pool already matches — the normal case.
     */
    private fun normalizeProject(p: Project, label: String): Project {
        if (p.phrases.size == 256 && p.chains.size == 256 && p.tracks.size == 8 &&
            p.instruments.size == 128 && p.tables.size == 128 &&
            p.grooves.size == 128 && p.eqPresets.size == 128) return p

        val d = Project()  // canonical-sized default pools to borrow padding elements from
        val fixed = p.copy(
            phrases     = Array(256) { i -> p.phrases.getOrElse(i)     { d.phrases[i] } },
            chains      = Array(256) { i -> p.chains.getOrElse(i)      { d.chains[i] } },
            tracks      = Array(8)   { i -> p.tracks.getOrElse(i)      { d.tracks[i] } },
            instruments = Array(128) { i -> p.instruments.getOrElse(i) { d.instruments[i] } },
            tables      = Array(128) { i -> p.tables.getOrElse(i)      { d.tables[i] } },
            grooves     = Array(128) { i -> p.grooves.getOrElse(i)     { d.grooves[i] } },
            eqPresets   = Array(128) { i -> p.eqPresets.getOrElse(i)   { d.eqPresets[i] } }
        )
        logger.d(TAG, "📦 Normalized pools ($label) → canonical: phrases ${p.phrases.size}, " +
            "chains ${p.chains.size}, tracks ${p.tracks.size}, instruments ${p.instruments.size}, " +
            "tables ${p.tables.size}, grooves ${p.grooves.size}, eq ${p.eqPresets.size}")
        return fixed
    }

    private fun migrateProject(project: Project) {
        if (project.version < 1) {
            var migrated = 0
            for (table in project.tables) {
                for (row in table.rows) {
                    if (row.volume == 0xFF) {
                        row.volume = -1
                        migrated++
                    }
                }
            }
            if (migrated > 0) {
                logger.d(TAG, "📦 Migrated $migrated table volume values (0xFF → -1)")
            }
            project.version = 1
            logger.d(TAG, "📦 Project migrated from version 0 → 1")
        }
    }

    // ========================================
    // RESULT TYPES
    // ========================================

    sealed class SaveResult {
        data class Success(val filename: String) : SaveResult()
        data class Error(val message: String) : SaveResult()
    }

    sealed class LoadResult {
        data class Success(val project: Project) : LoadResult()
        data class Error(val message: String) : LoadResult()
    }
}
