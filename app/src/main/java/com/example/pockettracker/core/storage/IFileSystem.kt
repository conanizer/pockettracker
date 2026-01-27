package com.example.pockettracker.core.storage

/**
 * Platform-agnostic file system interface.
 *
 * This interface provides file and directory operations without depending on
 * Android-specific classes (java.io.File, Context, Environment).
 *
 * Implementations:
 * - Android: AndroidFileSystem (uses java.io.File, scoped storage)
 * - Linux: LinuxFileSystem (future - uses std filesystem)
 */
interface IFileSystem {
    /**
     * Get the projects directory path.
     * Creates the directory if it doesn't exist.
     * @return Absolute path to projects directory
     */
    fun getProjectsDirectory(): String

    /**
     * Get the samples directory path.
     * Creates the directory if it doesn't exist.
     * @return Absolute path to samples directory
     */
    fun getSamplesDirectory(): String

    /**
     * Read entire file as text.
     * @param path Absolute path to file
     * @return File contents as string
     * @throws Exception if file doesn't exist or can't be read
     */
    fun readFile(path: String): String

    /**
     * Write text to file (overwrites if exists).
     * @param path Absolute path to file
     * @param content Text content to write
     * @return true if successful
     */
    fun writeFile(path: String, content: String): Boolean

    /**
     * Delete a file.
     * @param path Absolute path to file
     * @return true if deleted successfully
     */
    fun deleteFile(path: String): Boolean

    /**
     * Delete a file or folder (recursively if folder).
     * @param path Absolute path to file/folder
     * @return true if deleted successfully
     */
    fun deleteFileOrFolder(path: String): Boolean

    /**
     * Rename a file or folder.
     * @param oldPath Absolute path to existing file/folder
     * @param newName New name (without path, but with extension for files)
     * @return true if renamed successfully
     */
    fun renameFile(oldPath: String, newName: String): Boolean

    /**
     * Check if file or folder exists.
     * @param path Absolute path
     * @return true if exists
     */
    fun fileExists(path: String): Boolean

    /**
     * Create a new folder.
     * @param parentPath Parent directory path
     * @param folderName Name of new folder
     * @return Absolute path to created folder, or null if failed
     */
    fun createFolder(parentPath: String, folderName: String): String?

    /**
     * List files and folders in a directory.
     * @param directoryPath Absolute path to directory
     * @param extension Optional file extension filter (e.g., "ptp", "wav")
     * @return List of file/folder information
     */
    fun listFiles(directoryPath: String, extension: String? = null): List<FileInfo>

    /**
     * Sort files by the given mode.
     * @param files List of FileInfo to sort
     * @param sortMode How to sort
     * @return Sorted list
     */
    fun sortFiles(files: List<FileInfo>, sortMode: FileSortMode): List<FileInfo>

    /**
     * Check if storage is accessible and writable.
     * @return true if storage is available
     */
    fun hasStoragePermission(): Boolean

    /**
     * Get parent directory path from a file/folder path.
     * @param path Absolute path to file/folder
     * @return Parent directory path, or null if no parent
     */
    fun getParentPath(path: String): String?

    /**
     * Get the renders directory path (for WAV export).
     * Creates the directory if it doesn't exist.
     * @return Absolute path to renders directory
     */
    fun getRendersDirectory(): String

    /**
     * Write binary data to file (overwrites if exists).
     * @param path Absolute path to file
     * @param data Byte array to write
     * @return true if successful
     */
    fun writeBytes(path: String, data: ByteArray): Boolean
}

/**
 * Platform-agnostic file/folder information.
 * Replaces java.io.File for portability.
 */
data class FileInfo(
    val path: String,              // Absolute path
    val name: String,              // File/folder name
    val extension: String,         // File extension (e.g., "ptp", "wav"), empty for folders
    val isDirectory: Boolean,      // true if folder, false if file
    val size: Long,                // File size in bytes (0 for folders)
    val lastModified: Long         // Last modified timestamp (milliseconds since epoch)
) {
    /**
     * Get name without extension.
     * Example: "mysong.ptp" → "mysong"
     */
    val nameWithoutExtension: String
        get() = if (extension.isEmpty()) name else name.removeSuffix(".$extension")
}

/**
 * File sorting modes.
 * Platform-agnostic enum with display labels for UI.
 */
enum class FileSortMode(val label: String) {
    DATE_DESC("DATE ↓"),   // Newest first (default)
    DATE_ASC("DATE ↑"),    // Oldest first
    NAME_ASC("NAME ↑"),    // A-Z
    NAME_DESC("NAME ↓"),   // Z-A
    SIZE_ASC("SIZE ↑"),    // Smallest first
    SIZE_DESC("SIZE ↓")    // Largest first
}
