package com.conanizer.pockettracker.core.logic

import com.conanizer.pockettracker.core.logging.ILogger
import com.conanizer.pockettracker.FileManager
import com.conanizer.pockettracker.core.data.Project
import com.conanizer.pockettracker.core.storage.FileInfo

/**
 * FILE CONTROLLER
 *
 * Coordinates project save/load operations.
 *
 * ✅ PLATFORM-AGNOSTIC - No Android dependencies (except Log which will be abstracted later)
 *
 * Architecture:
 * - FileController (this) - Business logic coordination
 * - FileManager - File operations
 * - IFileSystem - Platform abstraction
 */
class FileController(
    private val fileManager: FileManager,
    private val logger: ILogger
) {
    private val TAG = "FileController"

    /**
     * Save project to file.
     *
     * @param project The project to save
     * @param filename Filename without extension
     * @return SaveResult indicating success or failure
     */
    fun saveProject(project: Project, filename: String): SaveResult {
        return try {
            val success = fileManager.saveProject(project, filename)

            if (success) {
                logger.d(TAG, "✅ Project saved: $filename")
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

    /**
     * Load project from filename.
     *
     * @param filename Filename without extension
     * @return LoadResult with project or error
     */
    fun loadProject(filename: String): LoadResult {
        return try {
            val project = fileManager.loadProject(filename)

            if (project != null) {
                migrateProject(project)
                logger.d(TAG, "✅ Project loaded: $filename")
                LoadResult.Success(project)
            } else {
                logger.e(TAG, "❌ Load failed: $filename")
                LoadResult.Error("Failed to load project")
            }
        } catch (e: Exception) {
            logger.e(TAG, "❌ Load error: ${e.message}")
            LoadResult.Error(e.message ?: "Unknown error")
        }
    }

    /**
     * Load project from FileInfo.
     *
     * @param fileInfo File information from file browser
     * @return LoadResult with project or error
     */
    fun loadProject(fileInfo: FileInfo): LoadResult {
        return try {
            val project = fileManager.loadProject(fileInfo)

            if (project != null) {
                migrateProject(project)
                logger.d(TAG, "✅ Project loaded: ${fileInfo.name}")
                LoadResult.Success(project)
            } else {
                logger.e(TAG, "❌ Load failed: ${fileInfo.name}")
                LoadResult.Error("Failed to load project")
            }
        } catch (e: Exception) {
            logger.e(TAG, "❌ Load error: ${e.message}")
            LoadResult.Error(e.message ?: "Unknown error")
        }
    }

    /**
     * List all project files.
     *
     * @return List of project files
     */
    fun listProjects(): List<FileInfo> {
        return fileManager.listProjects()
    }

    /**
     * Delete a project file.
     *
     * @param filename Filename without extension
     * @return true if deleted successfully
     */
    fun deleteProject(filename: String): Boolean {
        return fileManager.deleteProject(filename)
    }

    // ========================================
    // PROJECT MIGRATION
    // ========================================

    /**
     * Migrate old project data to current format.
     * Called after deserialization to handle format changes.
     */
    private fun migrateProject(project: Project) {
        // Version 0 = pre-versioning format. Apply all legacy migrations.
        // Version 1+ = current format, skip migrations that would corrupt valid data.
        if (project.version < 1) {
            // Migration: Table volume 0xFF → -1 (Extension Pack 3)
            // Old format used 0xFF as "no change" sentinel, new format uses -1.
            // Only run on old files — on new files 0xFF is a valid volume value (full volume).
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
            project.version = 1  // Stamp so re-saves don't re-trigger migration
            logger.d(TAG, "📦 Project migrated from version 0 → 1")
        }
    }

    // ========================================
    // RESULT TYPES
    // ========================================

    /**
     * Result of save operation.
     */
    sealed class SaveResult {
        data class Success(val filename: String) : SaveResult()
        data class Error(val message: String) : SaveResult()
    }

    /**
     * Result of load operation.
     */
    sealed class LoadResult {
        data class Success(val project: Project) : LoadResult()
        data class Error(val message: String) : LoadResult()
    }
}
