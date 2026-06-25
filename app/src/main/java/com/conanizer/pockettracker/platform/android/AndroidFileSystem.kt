package com.conanizer.pockettracker.platform.android

import android.content.Context
import android.os.Environment
import android.util.Log
import com.conanizer.pockettracker.core.storage.IFileSystem
import com.conanizer.pockettracker.core.storage.FileInfo
import com.conanizer.pockettracker.core.storage.FileSortMode
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

        if (!projectsDir.exists()) {
            if (projectsDir.mkdirs()) {
                Log.d(TAG, "✅ Created projects directory: ${projectsDir.absolutePath}")
            } else {
                Log.w(TAG, "⚠️ Could not create projects directory — storage permission may be missing")
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
            if (samplesDir.mkdirs()) {
                Log.d(TAG, "✅ Created samples directory: ${samplesDir.absolutePath}")
            } else {
                Log.w(TAG, "⚠️ Could not create samples directory — storage permission may be missing")
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
            val tmpFile = File("$path.tmp")
            tmpFile.writeText(content)
            val renamed = tmpFile.renameTo(File(path))
            if (!renamed) {
                // renameTo can fail across filesystems; fall back to direct write
                tmpFile.copyTo(File(path), overwrite = true)
                tmpFile.delete()
            }
            // No success log here: it fires on every write — including the ~3s autosave — and each
            // high-level caller (saveProject/saveTemplate/writeAutosave) already logs its own result.
            // (Also drops a File.length() stat per write.)
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

    override fun moveFile(sourcePath: String, destPath: String): Boolean {
        return try {
            val src = File(sourcePath)
            val dst = File(destPath)
            dst.parentFile?.mkdirs()
            // renameTo works cross-directory on the same filesystem (internal storage)
            val moved = src.renameTo(dst)
            if (!moved) {
                // Fallback: copy then delete (handles cross-filesystem moves)
                src.copyTo(dst, overwrite = false)
                src.delete()
            } else true
        } catch (e: Exception) {
            Log.e(TAG, "❌ Failed to move file: ${e.message}")
            false
        }
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
        // For API 30+ (Android 11+), check MANAGE_EXTERNAL_STORAGE
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
            return Environment.isExternalStorageManager()
        }
        // For API 29 and below, check if directory is accessible
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

    /**
     * Get the renders directory (for WAV export).
     * Creates it if it doesn't exist.
     *
     * Location: Documents/PocketTracker/Renders/
     */
    override fun getRendersDirectory(): String {
        val documentsDir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS)
        val rendersDir = File(documentsDir, "PocketTracker/Renders")

        if (!rendersDir.exists()) {
            if (rendersDir.mkdirs()) {
                Log.d(TAG, "Created renders directory: ${rendersDir.absolutePath}")
            } else {
                Log.w(TAG, "⚠️ Could not create renders directory — storage permission may be missing")
            }
        }

        return rendersDir.absolutePath
    }

    /**
     * Get the resampled samples directory.
     * Creates it if it doesn't exist.
     *
     * Location: Documents/PocketTracker/Samples/Resampled/
     */
    override fun getResampledDirectory(): String {
        val documentsDir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS)
        val resampledDir = File(documentsDir, "PocketTracker/Samples/Resampled")

        if (!resampledDir.exists()) {
            if (resampledDir.mkdirs()) {
                Log.d(TAG, "Created resampled directory: ${resampledDir.absolutePath}")
            } else {
                Log.w(TAG, "⚠️ Could not create resampled directory — storage permission may be missing")
            }
        }

        return resampledDir.absolutePath
    }

    /**
     * Get the instrument presets directory.
     * Location: Documents/PocketTracker/Instruments/
     */
    override fun getInstrumentsDirectory(): String {
        val documentsDir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS)
        val dir = File(documentsDir, "PocketTracker/Instruments")
        if (!dir.exists() && !dir.mkdirs())
            Log.w(TAG, "⚠️ Could not create instruments directory — storage permission may be missing")
        return dir.absolutePath
    }

    override fun getSoundfontsDirectory(): String {
        val documentsDir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS)
        val dir = File(documentsDir, "PocketTracker/Soundfonts")
        if (!dir.exists() && !dir.mkdirs())
            Log.w(TAG, "⚠️ Could not create soundfonts directory — storage permission may be missing")
        return dir.absolutePath
    }

    override fun getThemesDirectory(): String {
        val documentsDir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS)
        val dir = File(documentsDir, "PocketTracker/Themes")
        if (!dir.exists() && !dir.mkdirs())
            Log.w(TAG, "⚠️ Could not create themes directory — storage permission may be missing")
        return dir.absolutePath
    }

    /**
     * Get the template project path.
     * Stored in app-private internal storage (not exposed to the file browser).
     */
    override fun getTemplateProjectPath(): String {
        return File(context.filesDir, "template.ptp").absolutePath
    }

    override fun getAutosaveFilePath(): String {
        return File(context.filesDir, "autosave.ptp").absolutePath
    }

    /**
     * Write binary data to file (overwrites if exists).
     */
    override fun writeBytes(path: String, data: ByteArray): Boolean {
        return try {
            val tmpFile = File("$path.tmp")
            tmpFile.writeBytes(data)
            val renamed = tmpFile.renameTo(File(path))
            if (!renamed) {
                tmpFile.copyTo(File(path), overwrite = true)
                tmpFile.delete()
            }
            Log.d(TAG, "Wrote binary file: $path (${File(path).length()} bytes)")
            true
        } catch (e: Exception) {
            Log.e(TAG, "Failed to write binary file: ${e.message}")
            false
        }
    }
}
