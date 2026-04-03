package com.conanizer.pockettracker

import android.util.Log
import com.conanizer.pockettracker.core.data.InstrumentPreset
import com.conanizer.pockettracker.core.data.Project
import com.conanizer.pockettracker.core.storage.IFileSystem
import com.conanizer.pockettracker.core.storage.FileInfo
import com.conanizer.pockettracker.core.storage.FileSortMode
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json

/**
 * FILE MANAGER
 *
 * Handles saving and loading PocketTracker project files (.ptp)
 *
 * ✅ PLATFORM-AGNOSTIC - Uses IFileSystem interface (no Android dependencies)
 *
 * Storage location: Documents/PocketTracker/
 * This is accessible to the user for backup/sharing and survives app uninstall.
 */
class FileManager(private val fileSystem: IFileSystem) {

    private val TAG = "FileManager"

    // JSON serializer with pretty printing
    private val json = Json {
        prettyPrint = true
        ignoreUnknownKeys = true
    }

    /**
     * Get the PocketTracker projects directory path.
     * Creates it if it doesn't exist.
     *
     * Location: Documents/PocketTracker/Projects/
     */
    fun getProjectsDirectory(): String {
        return fileSystem.getProjectsDirectory()
    }

    /**
     * Get the samples directory path.
     * Creates it if it doesn't exist.
     * Returns: Documents/PocketTracker/Samples
     */
    fun getSamplesDirectory(): String {
        return fileSystem.getSamplesDirectory()
    }

    fun getInstrumentsDirectory(): String = fileSystem.getInstrumentsDirectory()

    fun getSoundfontsDirectory(): String = fileSystem.getSoundfontsDirectory()

    /**
     * Save an InstrumentPreset as a .pti file.
     * @param preset The preset to save
     * @param path Absolute file path (should end in .pti)
     * @return true on success
     */
    fun saveInstrumentPreset(preset: InstrumentPreset, path: String): Boolean {
        return try {
            val jsonString = json.encodeToString(preset)
            fileSystem.writeFile(path, jsonString)
            Log.d(TAG, "✅ Instrument preset saved: $path")
            true
        } catch (e: Exception) {
            Log.e(TAG, "❌ Failed to save instrument preset: ${e.message}")
            false
        }
    }

    /**
     * Load an InstrumentPreset from a .pti file.
     * @param path Absolute file path
     * @return InstrumentPreset on success, null on failure
     */
    fun loadInstrumentPreset(path: String): InstrumentPreset? {
        return try {
            val jsonString = fileSystem.readFile(path) ?: return null
            json.decodeFromString<InstrumentPreset>(jsonString)
        } catch (e: Exception) {
            Log.e(TAG, "❌ Failed to load instrument preset: ${e.message}")
            null
        }
    }

    /**
     * List all project files in the directory.
     * Returns list of .ptp files sorted by date (newest first) by default.
     */
    fun listProjects(): List<FileInfo> {
        val dirPath = getProjectsDirectory()
        val files = fileSystem.listFiles(dirPath, extension = "ptp")
        return fileSystem.sortFiles(files, FileSortMode.DATE_DESC)
    }

    /**
     * Sort files by the given mode.
     */
    fun sortFiles(files: List<FileInfo>, sortMode: FileSortMode): List<FileInfo> {
        return fileSystem.sortFiles(files, sortMode)
    }

    /**
     * Save project to file.
     *
     * @param project The project to save
     * @param filename Filename without extension (will add .ptp)
     * @return true if saved successfully
     */
    fun saveProject(project: Project, filename: String): Boolean {
        return try {
            val dirPath = getProjectsDirectory()

            // Sanitize filename
            val safeName = filename.replace(Regex("[^a-zA-Z0-9_\\-]"), "_")
            val filePath = "$dirPath/$safeName.ptp"

            // Serialize to JSON
            val jsonString = json.encodeToString(project)

            // Write to file
            val success = fileSystem.writeFile(filePath, jsonString)

            if (success) {
                Log.d(TAG, "✅ Saved project: $filePath")
            }
            success
        } catch (e: Exception) {
            Log.e(TAG, "❌ Failed to save project: ${e.message}")
            e.printStackTrace()
            false
        }
    }

    /**
     * Load project from file.
     *
     * @param filename Filename without extension (will add .ptp)
     * @return Loaded project, or null if failed
     */
    fun loadProject(filename: String): Project? {
        return try {
            val dirPath = getProjectsDirectory()
            val filePath = "$dirPath/$filename.ptp"

            if (!fileSystem.fileExists(filePath)) {
                Log.e(TAG, "❌ File not found: $filePath")
                return null
            }

            // Read and deserialize
            val jsonString = fileSystem.readFile(filePath)
            val project = json.decodeFromString<Project>(jsonString)

            Log.d(TAG, "✅ Loaded project: $filePath")
            project
        } catch (e: Exception) {
            Log.e(TAG, "❌ Failed to load project: ${e.message}")
            e.printStackTrace()
            null
        }
    }

    /**
     * Load project from FileInfo object.
     * Used by file browser.
     */
    fun loadProject(fileInfo: FileInfo): Project? {
        return try {
            if (!fileSystem.fileExists(fileInfo.path)) {
                Log.e(TAG, "❌ File not found: ${fileInfo.path}")
                return null
            }

            val jsonString = fileSystem.readFile(fileInfo.path)
            val project = json.decodeFromString<Project>(jsonString)

            Log.d(TAG, "✅ Loaded project: ${fileInfo.path}")
            project
        } catch (e: Exception) {
            Log.e(TAG, "❌ Failed to load project: ${e.message}")
            e.printStackTrace()
            null
        }
    }

    /**
     * Delete project file.
     */
    fun deleteProject(filename: String): Boolean {
        return try {
            val dirPath = getProjectsDirectory()
            val filePath = "$dirPath/$filename.ptp"

            val deleted = fileSystem.deleteFile(filePath)
            if (deleted) {
                Log.d(TAG, "✅ Deleted project: $filename")
            }
            deleted
        } catch (e: Exception) {
            Log.e(TAG, "❌ Failed to delete project: ${e.message}")
            false
        }
    }

    /**
     * Rename a file or folder.
     *
     * @param oldPath Absolute path to the file/folder to rename
     * @param newName New name (without path, but keeps extension for files)
     * @return true if renamed successfully
     */
    fun renameFile(oldPath: String, newName: String): Boolean {
        return fileSystem.renameFile(oldPath, newName)
    }

    /**
     * Create a new folder.
     *
     * @param parentPath Parent directory path
     * @param folderName Name of new folder
     * @return Absolute path to created folder, or null if failed
     */
    fun createFolder(parentPath: String, folderName: String): String? {
        return fileSystem.createFolder(parentPath, folderName)
    }

    /**
     * Delete a file or folder (and all contents if folder).
     *
     * @param path Absolute path to the file/folder to delete
     * @return true if deleted successfully
     */
    fun deleteFileOrFolder(path: String): Boolean {
        return fileSystem.deleteFileOrFolder(path)
    }

    /**
     * Check if storage permissions are available.
     * On Android 6+ (API 23+), external storage requires runtime permission.
     */
    fun hasStoragePermission(): Boolean {
        return fileSystem.hasStoragePermission()
    }
}
