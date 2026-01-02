package com.example.pockettracker.platform.android

import android.content.Context
import android.os.Environment
import android.util.Log
import com.example.pockettracker.core.storage.IFileSystem
import com.example.pockettracker.core.storage.FileInfo
import com.example.pockettracker.core.storage.FileSortMode
import java.io.File

/**
 * Android implementation of IFileSystem using scoped storage.
 *
 * Uses:
 * - android.content.Context for app-specific paths
 * - android.os.Environment for Documents directory
 * - java.io.File for file operations
 *
 * Storage location: Documents/PocketTracker/
 * This is accessible to the user for backup/sharing and survives app uninstall.
 */
class AndroidFileSystem(
    private val context: Context
) : IFileSystem {

    private val TAG = "AndroidFileSystem"

    /**
     * Get the PocketTracker projects directory.
     * Creates it if it doesn't exist.
     *
     * Location: Documents/PocketTracker/Projects/
     */
    override fun getProjectsDirectory(): String {
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
                val fallback = File(context.filesDir, "Projects")
                fallback.mkdirs()
                return fallback.absolutePath
            }
        }

        return projectsDir.absolutePath
    }

    /**
     * Get the samples directory.
     * Creates it if it doesn't exist.
     *
     * Location: Documents/PocketTracker/Samples/
     */
    override fun getSamplesDirectory(): String {
        val documentsDir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS)
        val samplesDir = File(documentsDir, "PocketTracker/Samples")

        if (!samplesDir.exists()) {
            val created = samplesDir.mkdirs()
            if (created) {
                Log.d(TAG, "✅ Created samples directory: ${samplesDir.absolutePath}")
            } else {
                Log.e(TAG, "❌ Failed to create samples directory")
                // Fallback to internal storage if external fails
                val fallback = File(context.filesDir, "Samples")
                fallback.mkdirs()
                return fallback.absolutePath
            }
        }

        return samplesDir.absolutePath
    }

    /**
     * Read entire file as text.
     */
    override fun readFile(path: String): String {
        val file = File(path)
        if (!file.exists()) {
            throw IllegalArgumentException("File not found: $path")
        }
        if (!file.canRead()) {
            throw IllegalArgumentException("File not readable: $path")
        }

        return file.readText()
    }

    /**
     * Write text to file (overwrites if exists).
     */
    override fun writeFile(path: String, content: String): Boolean {
        return try {
            val file = File(path)
            file.writeText(content)
            Log.d(TAG, "✅ Wrote file: $path (${file.length()} bytes)")
            true
        } catch (e: Exception) {
            Log.e(TAG, "❌ Failed to write file: ${e.message}")
            false
        }
    }

    /**
     * Delete a file.
     */
    override fun deleteFile(path: String): Boolean {
        return try {
            val file = File(path)
            val deleted = file.delete()
            if (deleted) {
                Log.d(TAG, "✅ Deleted file: $path")
            }
            deleted
        } catch (e: Exception) {
            Log.e(TAG, "❌ Failed to delete file: ${e.message}")
            false
        }
    }

    /**
     * Delete a file or folder (recursively if folder).
     */
    override fun deleteFileOrFolder(path: String): Boolean {
        return try {
            val file = File(path)
            val deleted = file.deleteRecursively()
            if (deleted) {
                Log.d(TAG, "✅ Deleted: $path")
            }
            deleted
        } catch (e: Exception) {
            Log.e(TAG, "❌ Failed to delete: ${e.message}")
            false
        }
    }

    /**
     * Rename a file or folder.
     */
    override fun renameFile(oldPath: String, newName: String): Boolean {
        return try {
            val oldFile = File(oldPath)

            // Sanitize new name
            val safeName = newName.replace(Regex("[^a-zA-Z0-9_\\-.]"), "_")

            // Keep original extension for files
            val finalName = if (oldFile.isFile && oldFile.extension.isNotEmpty()) {
                // If newName already has the extension, don't add it again
                if (safeName.endsWith(".${oldFile.extension}")) {
                    safeName
                } else {
                    "$safeName.${oldFile.extension}"
                }
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
     * Check if file or folder exists.
     */
    override fun fileExists(path: String): Boolean {
        return File(path).exists()
    }

    /**
     * Create a new folder.
     */
    override fun createFolder(parentPath: String, folderName: String): String? {
        return try {
            val parentDir = File(parentPath)

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
                newFolder.absolutePath
            } else {
                null
            }
        } catch (e: Exception) {
            Log.e(TAG, "❌ Failed to create folder: ${e.message}")
            null
        }
    }

    /**
     * List files and folders in a directory.
     */
    override fun listFiles(directoryPath: String, extension: String?): List<FileInfo> {
        val dir = File(directoryPath)

        if (!dir.exists() || !dir.isDirectory) {
            return emptyList()
        }

        val files = dir.listFiles { file ->
            // If extension filter specified, only include matching files
            if (extension != null) {
                file.isDirectory || file.extension.equals(extension, ignoreCase = true)
            } else {
                true
            }
        } ?: return emptyList()

        return files.map { file ->
            FileInfo(
                path = file.absolutePath,
                name = file.name,
                extension = file.extension,
                isDirectory = file.isDirectory,
                size = if (file.isFile) file.length() else 0L,
                lastModified = file.lastModified()
            )
        }
    }

    /**
     * Sort files by the given mode.
     */
    override fun sortFiles(files: List<FileInfo>, sortMode: FileSortMode): List<FileInfo> {
        return when (sortMode) {
            FileSortMode.DATE_DESC -> files.sortedByDescending { it.lastModified }
            FileSortMode.DATE_ASC -> files.sortedBy { it.lastModified }
            FileSortMode.NAME_ASC -> files.sortedBy { it.nameWithoutExtension.lowercase() }
            FileSortMode.NAME_DESC -> files.sortedByDescending { it.nameWithoutExtension.lowercase() }
            FileSortMode.SIZE_ASC -> files.sortedBy { it.size }
            FileSortMode.SIZE_DESC -> files.sortedByDescending { it.size }
        }
    }

    /**
     * Check if storage is accessible and writable.
     */
    override fun hasStoragePermission(): Boolean {
        // For API 29+ (Android 10+), we use scoped storage which doesn't require permission
        // For API 23-28, we would need WRITE_EXTERNAL_STORAGE permission
        val dir = File(getProjectsDirectory())
        return dir.exists() && dir.canWrite()
    }

    /**
     * Get parent directory path from a file/folder path.
     */
    override fun getParentPath(path: String): String? {
        val file = File(path)
        return file.parent
    }
}
