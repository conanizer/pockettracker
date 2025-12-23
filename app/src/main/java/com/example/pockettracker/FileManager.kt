package com.example.pockettracker

import android.content.Context
import android.os.Environment
import android.util.Log
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import java.io.File

/**
 * FILE MANAGER
 *
 * Handles saving and loading PocketTracker project files (.ptp)
 *
 * Storage location: Documents/PocketTracker/
 * This is accessible to the user for backup/sharing and survives app uninstall.
 */
class FileManager(private val context: Context) {

    private val TAG = "FileManager"

    // JSON serializer with pretty printing
    private val json = Json {
        prettyPrint = true
        ignoreUnknownKeys = true
    }

    /**
     * Get the PocketTracker projects directory
     * Creates it if it doesn't exist
     *
     * Location: Documents/PocketTracker/Projects/
     */
    fun getProjectsDirectory(): File {
        // Use Documents folder (shared storage, survives uninstall)
        val documentsDir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS)
        val projectsDir = File(documentsDir, "PocketTracker/Projects")

        // Create directory if it doesn't exist
        if (!projectsDir.exists()) {
            val created = projectsDir.mkdirs()
            if (created) {
                Log.d(TAG, "✅ Created projects directory: ${projectsDir.absolutePath}")
            } else {
                Log.e(TAG, "❌ Failed to create projects directory")
                // Fallback to internal storage if external fails
                return File(context.filesDir, "Projects").apply { mkdirs() }
            }
        }

        return projectsDir
    }

    /**
     * Get the samples directory
     * Creates it if it doesn't exist
     * Returns: Documents/PocketTracker/Samples
     */
    fun getSamplesDirectory(): File {
        val documentsDir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS)
        val samplesDir = File(documentsDir, "PocketTracker/Samples")

        if (!samplesDir.exists()) {
            val created = samplesDir.mkdirs()
            if (created) {
                Log.d(TAG, "✅ Created samples directory: ${samplesDir.absolutePath}")
            } else {
                Log.e(TAG, "❌ Failed to create samples directory")
                // Fallback to internal storage if external fails
                return File(context.filesDir, "Samples").apply { mkdirs() }
            }
        }

        return samplesDir
    }

    /**
     * List all project files in the directory
     * Returns list of .ptp files sorted by date (newest first) by default
     */
    fun listProjects(): List<File> {
        val dir = getProjectsDirectory()
        return dir.listFiles { file ->
            file.isFile && file.extension == "ptp"
        }?.sortedByDescending { it.lastModified() } ?: emptyList()
    }

    /**
     * Sort files by the given mode
     */
    fun sortFiles(files: List<File>, sortMode: FileSortMode): List<File> {
        return when (sortMode) {
            FileSortMode.DATE_DESC -> files.sortedByDescending { it.lastModified() }
            FileSortMode.DATE_ASC -> files.sortedBy { it.lastModified() }
            FileSortMode.NAME_ASC -> files.sortedBy { it.nameWithoutExtension.lowercase() }
            FileSortMode.NAME_DESC -> files.sortedByDescending { it.nameWithoutExtension.lowercase() }
            FileSortMode.SIZE_ASC -> files.sortedBy { it.length() }
            FileSortMode.SIZE_DESC -> files.sortedByDescending { it.length() }
        }
    }

    /**
     * Save project to file
     *
     * @param project The project to save
     * @param filename Filename without extension (will add .ptp)
     * @return true if saved successfully
     */
    fun saveProject(project: Project, filename: String): Boolean {
        return try {
            val dir = getProjectsDirectory()

            // Sanitize filename
            val safeName = filename.replace(Regex("[^a-zA-Z0-9_\\-]"), "_")
            val file = File(dir, "$safeName.ptp")

            // Serialize to JSON
            val jsonString = json.encodeToString(project)

            // Write to file
            file.writeText(jsonString)

            Log.d(TAG, "✅ Saved project: ${file.absolutePath} (${file.length()} bytes)")
            true
        } catch (e: Exception) {
            Log.e(TAG, "❌ Failed to save project: ${e.message}")
            e.printStackTrace()
            false
        }
    }

    /**
     * Load project from file
     *
     * @param filename Filename without extension (will add .ptp)
     * @return Loaded project, or null if failed
     */
    fun loadProject(filename: String): Project? {
        return try {
            val dir = getProjectsDirectory()
            val file = File(dir, "$filename.ptp")

            if (!file.exists()) {
                Log.e(TAG, "❌ File not found: ${file.absolutePath}")
                return null
            }

            // Read and deserialize
            val jsonString = file.readText()
            val project = json.decodeFromString<Project>(jsonString)

            Log.d(TAG, "✅ Loaded project: ${file.absolutePath}")
            project
        } catch (e: Exception) {
            Log.e(TAG, "❌ Failed to load project: ${e.message}")
            e.printStackTrace()
            null
        }
    }

    /**
     * Load project from File object
     * Used by file browser
     */
    fun loadProject(file: File): Project? {
        return try {
            if (!file.exists()) {
                Log.e(TAG, "❌ File not found: ${file.absolutePath}")
                return null
            }

            val jsonString = file.readText()
            val project = json.decodeFromString<Project>(jsonString)

            Log.d(TAG, "✅ Loaded project: ${file.absolutePath}")
            project
        } catch (e: Exception) {
            Log.e(TAG, "❌ Failed to load project: ${e.message}")
            e.printStackTrace()
            null
        }
    }

    /**
     * Delete project file
     */
    fun deleteProject(filename: String): Boolean {
        return try {
            val dir = getProjectsDirectory()
            val file = File(dir, "$filename.ptp")

            val deleted = file.delete()
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
     * Rename a file or folder
     *
     * @param oldFile The file/folder to rename
     * @param newName New name (without path or extension)
     * @return true if renamed successfully
     */
    fun renameFile(oldFile: File, newName: String): Boolean {
        return try {
            // Sanitize new name
            val safeName = newName.replace(Regex("[^a-zA-Z0-9_\\-]"), "_")

            // Keep original extension for files
            val finalName = if (oldFile.isFile && oldFile.extension.isNotEmpty()) {
                "$safeName.${oldFile.extension}"
            } else {
                safeName
            }

            val newFile = File(oldFile.parent, finalName)

            // Check if target already exists
            if (newFile.exists()) {
                Log.e(TAG, "❌ Cannot rename: file already exists: $finalName")
                return false
            }

            val renamed = oldFile.renameTo(newFile)
            if (renamed) {
                Log.d(TAG, "✅ Renamed: ${oldFile.name} → $finalName")
            }
            renamed
        } catch (e: Exception) {
            Log.e(TAG, "❌ Failed to rename file: ${e.message}")
            false
        }
    }

    /**
     * Create a new folder
     *
     * @param parentDir Parent directory
     * @param folderName Name of new folder
     * @return The created folder, or null if failed
     */
    fun createFolder(parentDir: File, folderName: String): File? {
        return try {
            // Sanitize folder name
            val safeName = folderName.replace(Regex("[^a-zA-Z0-9_\\-]"), "_")
            val newFolder = File(parentDir, safeName)

            // Check if folder already exists
            if (newFolder.exists()) {
                Log.e(TAG, "❌ Cannot create folder: already exists: $safeName")
                return null
            }

            val created = newFolder.mkdirs()
            if (created) {
                Log.d(TAG, "✅ Created folder: ${newFolder.absolutePath}")
                newFolder
            } else {
                null
            }
        } catch (e: Exception) {
            Log.e(TAG, "❌ Failed to create folder: ${e.message}")
            null
        }
    }

    /**
     * Delete a file or folder (and all contents if folder)
     *
     * @param file The file/folder to delete
     * @return true if deleted successfully
     */
    fun deleteFileOrFolder(file: File): Boolean {
        return try {
            val deleted = file.deleteRecursively()
            if (deleted) {
                Log.d(TAG, "✅ Deleted: ${file.absolutePath}")
            }
            deleted
        } catch (e: Exception) {
            Log.e(TAG, "❌ Failed to delete: ${e.message}")
            false
        }
    }

    /**
     * Check if storage permissions are available
     * On Android 6+ (API 23+), external storage requires runtime permission
     */
    fun hasStoragePermission(): Boolean {
        // For API 29+ (Android 10+), we use scoped storage which doesn't require permission
        // For API 23-28, we would need WRITE_EXTERNAL_STORAGE permission
        val dir = getProjectsDirectory()
        return dir.exists() && dir.canWrite()
    }
}
