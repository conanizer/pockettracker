package com.example.pockettracker

import android.content.Context
import android.util.Log
import kotlinx.serialization.encodeToString
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.json.Json
import java.io.File

/**
 * FILE MANAGER - HANDLES SAVE/LOAD OF PROJECTS
 *
 * Now with configurable file extension!
 */
class FileManager(
    private val context: Context,
    private val fileExtension: String = "ptp"  // ✨ Customizable! (PocketTracker)
) {

    private val TAG = "FileManager"

    // JSON serializer
    private val json = Json {
        prettyPrint = true
        ignoreUnknownKeys = true
        encodeDefaults = false
    }

    // Songs directory
    private val songsDir: File
        get() = File(context.filesDir, "Songs").apply {
            if (!exists()) {
                mkdirs()
                Log.d(TAG, "Created Songs directory: $absolutePath")
            }
        }

    /**
     * Save a project to a file
     */
    fun saveProject(project: Project, filename: String): Boolean {
        return try {
            val cleanFilename = filename.replace(Regex("[^a-zA-Z0-9_\\-]"), "_")
            val file = File(songsDir, "$cleanFilename.$fileExtension")  // ✨ Uses custom extension

            Log.d(TAG, "Saving to: ${file.name}")

            val jsonString = json.encodeToString(project)
            file.writeText(jsonString)

            val sizeKB = file.length() / 1024
            Log.d(TAG, "✅ Saved: ${file.name} (${sizeKB}KB)")
            true
        } catch (e: Exception) {
            Log.e(TAG, "❌ Save error: ${e.message}", e)
            false
        }
    }

    /**
     * Load a project from a file
     */
    fun loadProject(filename: String): Project? {
        return try {
            val cleanFilename = filename.replace(Regex("[^a-zA-Z0-9_\\-]"), "_")
            val file = File(songsDir, "$cleanFilename.$fileExtension")  // ✨ Uses custom extension

            if (!file.exists()) {
                Log.w(TAG, "❌ File not found: ${file.name}")
                return null
            }

            Log.d(TAG, "Loading: ${file.name}")

            val jsonString = file.readText()
            val project = json.decodeFromString<Project>(jsonString)

            Log.d(TAG, "✅ Loaded: ${project.name}")
            project
        } catch (e: Exception) {
            Log.e(TAG, "❌ Load error: ${e.message}", e)
            null
        }
    }

    /**
     * List all saved projects
     */
    fun listProjects(): List<String> {
        return try {
            val files = songsDir.listFiles()
                ?.filter { it.extension == fileExtension }  // ✨ Filter by custom extension
                ?.map { it.nameWithoutExtension }
                ?.sorted()
                ?: emptyList()

            Log.d(TAG, "📁 Found ${files.size} project(s) with .$fileExtension extension")
            files
        } catch (e: Exception) {
            Log.e(TAG, "❌ List error: ${e.message}", e)
            emptyList()
        }
    }

    /**
     * Delete a project file
     */
    fun deleteProject(filename: String): Boolean {
        return try {
            val cleanFilename = filename.replace(Regex("[^a-zA-Z0-9_\\-]"), "_")
            val file = File(songsDir, "$cleanFilename.$fileExtension")  // ✨ Uses custom extension

            val success = file.delete()
            Log.d(TAG, if (success) "✅ Deleted: $filename" else "❌ Delete failed: $filename")
            success
        } catch (e: Exception) {
            Log.e(TAG, "❌ Delete error: ${e.message}", e)
            false
        }
    }

    /**
     * Check if a project exists
     */
    fun projectExists(filename: String): Boolean {
        val cleanFilename = filename.replace(Regex("[^a-zA-Z0-9_\\-]"), "_")
        val file = File(songsDir, "$cleanFilename.$fileExtension")
        return file.exists()
    }

    /**
     * Get full path to project file
     */
    fun getProjectPath(filename: String): String {
        val cleanFilename = filename.replace(Regex("[^a-zA-Z0-9_\\-]"), "_")
        return File(songsDir, "$cleanFilename.$fileExtension").absolutePath
    }

    /**
     * Get file size in KB
     */
    fun getProjectSizeKB(filename: String): Long {
        val cleanFilename = filename.replace(Regex("[^a-zA-Z0-9_\\-]"), "_")
        val file = File(songsDir, "$cleanFilename.$fileExtension")
        return if (file.exists()) file.length() / 1024 else 0
    }
}

/**
 * USAGE EXAMPLES:
 *
 * // Use default .ptk extension
 * val fileManager = FileManager(context)
 *
 * // Use custom extension
 * val fileManager = FileManager(context, fileExtension = "json")  // .json files
 * val fileManager = FileManager(context, fileExtension = "pkt")   // .pkt files
 * val fileManager = FileManager(context, fileExtension = "song")  // .song files
 */