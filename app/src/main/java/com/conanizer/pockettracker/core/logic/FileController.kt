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
            val project = json.decodeFromString<Project>(jsonString)
            migrateProject(project)
            logger.d(TAG, "✅ Project loaded: $filename")
            LoadResult.Success(project)
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
            val project = json.decodeFromString<Project>(jsonString)
            migrateProject(project)
            logger.d(TAG, "✅ Project loaded: ${fileInfo.name}")
            LoadResult.Success(project)
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
