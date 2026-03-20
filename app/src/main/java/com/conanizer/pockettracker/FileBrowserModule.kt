package com.conanizer.pockettracker

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import com.conanizer.pockettracker.core.storage.FileSortMode
import java.io.File
import java.text.SimpleDateFormat
import java.util.*

/**
 * FILE BROWSER MODULE - ENHANCED
 *
 * Reusable file browser with folder navigation, rename, and create folder support.
 *
 * Features:
 * - Navigate into folders and back to parent (..)
 * - Rename files and folders
 * - Create new folders
 * - Delete files and folders
 * - Sort by name, date, size
 * - Full screen mode (480px height - covers entire screen)
 * - Reusable for different file types (projects, samples, etc.)
 *
 * Controls:
 * - A: Open folder / Load file
 * - B: Back / Cancel operation
 * - D-PAD UP/DOWN: Move cursor
 * - D-PAD LEFT/RIGHT: Page jump (20 items)
 * - SELECT + A: Rename file/folder
 * - SELECT + B: Delete file/folder
 * - SELECT + R: Create new folder
 * - LT + D-PAD LEFT: Go to parent folder
 * - LT + D-PAD UP/DOWN: Change sort mode
 *
 * Size: 640×480 pixels (full screen)
 * Shows: 20 files/folders at a time (scrollable)
 */
class FileBrowserModule : TrackerModule {

    companion object {
        // Display constants
        const val VISIBLE_ROWS = 19  // File list rows (19 rows for files + 2 status bars)
        const val WIDTH = 640
        const val HEIGHT = 480  // Full screen height (covers oscilloscope)

        // Colors
        val COLOR_CURSOR = Color.Yellow
        val COLOR_FOLDER = Color(0xFF88CCFF)  // Light blue for folders
        val COLOR_FILE = Color(0xFFCCCCCC)    // Light gray for files
        val COLOR_VIDEO = Color(0xFFFFBB55)   // Amber for video/audio container files
        val COLOR_PARENT = Color(0xFFFFAA88)  // Orange for ".."
        val COLOR_INACTIVE = Color(0xFF666666)

        // Video/audio container extensions shown alongside WAV in instrument browser
        val VIDEO_EXTENSIONS = listOf("mp4", "mkv", "webm", "3gp", "m4a", "mov")
        val COLOR_BACKGROUND = Color(0xFF0a0a0a)
    }

    override val width = WIDTH
    override val height = HEIGHT

    // Font constants
    private val FONT_SCALE = 3
    private val CHAR_SPACING = 2
    private val ROW_HEIGHT = 21
    private val TEXT_PADDING = 3

    // Date formatter
    private val dateFormat = SimpleDateFormat("yyyy-MM-dd", Locale.US)

    /**
     * Browser item types - what can appear in the file list
     */
    sealed class BrowserItem {
        abstract val file: File
        abstract val displayName: String

        /** Parent directory entry (".." to go up) */
        data class Parent(override val file: File) : BrowserItem() {
            override val displayName = ".."
        }

        /** Folder entry */
        data class Folder(override val file: File) : BrowserItem() {
            override val displayName = "[${file.name}]"
        }

        /** File entry */
        data class FileItem(override val file: File, val extension: String) : BrowserItem() {
            override val displayName = file.nameWithoutExtension
        }
    }

    /**
     * Browser mode - what state is the browser in?
     */
    enum class BrowserMode {
        NORMAL,      // Normal browsing
        DELETE,      // Delete confirmation (A to confirm, B to cancel)
        RENAME,      // Rename file/folder (editing name)
        CREATE       // Create new folder (editing name)
    }

    /**
     * Browser state - all data needed to display and interact with the browser
     */
    data class State(
        val currentDirectory: File,
        val items: List<BrowserItem>,
        val cursor: Int = 0,
        val scroll: Int = 0,
        val mode: BrowserMode = BrowserMode.NORMAL,
        val sortMode: FileSortMode = FileSortMode.DATE_DESC,
        val renameBuffer: String = "",      // For RENAME/CREATE modes
        val renameCursor: Int = 0,          // Character position in rename buffer
        val statusMessage: String = "",
        val statusSuccess: Boolean = true,
        val fileExtension: String? = null,          // Single-extension filter (legacy)
        val fileExtensions: List<String>? = null    // Multi-extension filter (null = all files)
    ) {
        /** Effective extension set: fileExtensions wins over fileExtension */
        val activeExtensions: Set<String>?
            get() = when {
                fileExtensions != null -> fileExtensions.map { it.lowercase() }.toSet()
                fileExtension != null  -> setOf(fileExtension.lowercase())
                else                   -> null
            }
    }

    /**
     * Build item list from directory
     * Includes parent entry if not at root
     */
    fun buildItemList(
        directory: File,
        fileExtension: String? = null,          // null = show all files (single-ext legacy)
        fileExtensions: List<String>? = null,   // multi-ext filter (wins over fileExtension)
        showHidden: Boolean = false
    ): List<BrowserItem> {
        // Resolve effective extension set
        val effectiveExtensions: Set<String>? = when {
            fileExtensions != null -> fileExtensions.map { it.lowercase() }.toSet()
            fileExtension != null  -> setOf(fileExtension.lowercase())
            else                   -> null
        }
        val items = mutableListOf<BrowserItem>()

        android.util.Log.d("FileBrowser", "=== Building item list ===")
        android.util.Log.d("FileBrowser", "Directory: ${directory.absolutePath}")
        android.util.Log.d("FileBrowser", "Extension filter: $fileExtension")

        // Add parent directory entry if not at root
        if (directory.parentFile != null) {
            items.add(BrowserItem.Parent(directory.parentFile!!))
            android.util.Log.d("FileBrowser", "Added parent '..' entry")
        }

        // Get all items in directory
        val allItems = directory.listFiles() ?: emptyArray()

        // Add folders first
        val folders = allItems
            .filter { it.isDirectory }
            .filter { showHidden || !it.name.startsWith(".") }
            .sortedBy { it.name.lowercase() }
        android.util.Log.d("FileBrowser", "Found ${folders.size} folders")
        folders.forEach {
            items.add(BrowserItem.Folder(it))
            android.util.Log.d("FileBrowser", "  [FOLDER] ${it.name}")
        }

        // Add files
        val files = allItems.filter { it.isFile }
        android.util.Log.d("FileBrowser", "Total files in directory: ${files.size}")

        val matchedFiles = files
            .filter { showHidden || !it.name.startsWith(".") }
            .also { android.util.Log.d("FileBrowser", "After hidden filter: ${it.size}") }
            .filter {
                val match = effectiveExtensions == null || it.extension.lowercase() in effectiveExtensions
                android.util.Log.d("FileBrowser", "  File: ${it.name}, ext='${it.extension}', filter='$effectiveExtensions', match=$match")
                match
            }
            .sortedBy { it.name.lowercase() }

        android.util.Log.d("FileBrowser", "Matched ${matchedFiles.size} files")
        matchedFiles.forEach {
            items.add(BrowserItem.FileItem(it, it.extension))
            android.util.Log.d("FileBrowser", "  [FILE] ${it.name}")
        }

        android.util.Log.d("FileBrowser", "Total items in list: ${items.size}")
        android.util.Log.d("FileBrowser", "=========================")

        return items
    }

    /**
     * Sort items by sort mode
     * Always keeps parent (..) at top, folders before files
     */
    fun sortItems(items: List<BrowserItem>, sortMode: FileSortMode): List<BrowserItem> {
        // Always keep parent at top
        val parent = items.filterIsInstance<BrowserItem.Parent>()
        val folders = items.filterIsInstance<BrowserItem.Folder>()
        val files = items.filterIsInstance<BrowserItem.FileItem>()

        val sortedFolders = when (sortMode) {
            FileSortMode.NAME_ASC -> folders.sortedBy { it.file.name.lowercase() }
            FileSortMode.NAME_DESC -> folders.sortedByDescending { it.file.name.lowercase() }
            FileSortMode.DATE_ASC -> folders.sortedBy { it.file.lastModified() }
            FileSortMode.DATE_DESC -> folders.sortedByDescending { it.file.lastModified() }
            FileSortMode.SIZE_ASC -> folders.sortedBy { it.file.length() }
            FileSortMode.SIZE_DESC -> folders.sortedByDescending { it.file.length() }
        }

        val sortedFiles = when (sortMode) {
            FileSortMode.NAME_ASC -> files.sortedBy { it.file.name.lowercase() }
            FileSortMode.NAME_DESC -> files.sortedByDescending { it.file.name.lowercase() }
            FileSortMode.DATE_ASC -> files.sortedBy { it.file.lastModified() }
            FileSortMode.DATE_DESC -> files.sortedByDescending { it.file.lastModified() }
            FileSortMode.SIZE_ASC -> files.sortedBy { it.file.length() }
            FileSortMode.SIZE_DESC -> files.sortedByDescending { it.file.length() }
        }

        return parent + sortedFolders + sortedFiles
    }

    /**
     * Navigate into a folder
     */
    fun navigateToFolder(state: State, folder: File): State {
        android.util.Log.d("FileBrowser", ">>> NAVIGATING TO FOLDER: ${folder.absolutePath}")
        android.util.Log.d("FileBrowser", ">>> Extension filter: ${state.activeExtensions}")
        val newItems = buildItemList(folder, state.fileExtension, state.fileExtensions)
        android.util.Log.d("FileBrowser", ">>> New items count: ${newItems.size}")
        return state.copy(
            currentDirectory = folder,
            items = sortItems(newItems, state.sortMode),
            cursor = 0,
            scroll = 0,
            statusMessage = "",  // Don't show "Opened" messages
            statusSuccess = true
        )
    }

    /**
     * Navigate to parent directory
     */
    fun navigateToParent(state: State): State {
        val parent = state.currentDirectory.parentFile ?: return state
        return navigateToFolder(state, parent)
    }

    /**
     * Get cursor context for current position
     */
    fun getCursorContext(state: State): CursorContext {
        return when (state.mode) {
            BrowserMode.NORMAL, BrowserMode.DELETE -> {
                // Normal browsing - cursor on file/folder list
                CursorContextFactory.browserLine(state.cursor, state.items.size)
            }
            BrowserMode.RENAME, BrowserMode.CREATE -> {
                // Editing text - cursor on character
                val char = if (state.renameCursor < state.renameBuffer.length) {
                    state.renameBuffer[state.renameCursor]
                } else {
                    ' '
                }
                CursorContextFactory.character(char)
            }
        }
    }

    // =========================================================================
    // RENDERING
    // =========================================================================

    override fun DrawScope.draw(x: Int, y: Int, scale: Int, state: Any?) {
        val browserState = state as? State ?: return

        // Draw background
        drawRect(
            color = COLOR_BACKGROUND,
            topLeft = Offset((x * scale).toFloat(), (y * scale).toFloat()),
            size = Size((WIDTH * scale).toFloat(), (HEIGHT * scale).toFloat())
        )

        // ===================================
        // TOP STATUS BARS (2 lines at top)
        // ===================================
        val topBarY1 = y + TEXT_PADDING  // First line: SEL+ combos or status messages
        val topBarY2 = topBarY1 + ROW_HEIGHT  // Second line: Path and file count

        // Draw background for both top bars
        drawRect(
            color = Color(0xFF1a1a1a),
            topLeft = Offset((x * scale).toFloat(), (topBarY1 * scale).toFloat()),
            size = Size((WIDTH * scale).toFloat(), (ROW_HEIGHT * 2 * scale).toFloat())
        )

        // Line 1: SEL+ combos or status messages
        when (browserState.mode) {
            BrowserMode.NORMAL -> {
                // Normal mode: Show SEL+ combo controls
                drawBitmapText(
                    text = "SEL+A=RENAME SEL+B=DEL SEL+R=NEW",
                    x = x + 10,
                    y = topBarY1 + TEXT_PADDING,
                    scale = scale,
                    color = Color(0xFFaaaaaa),
                    spacing = CHAR_SPACING,
                    fontScale = FONT_SCALE
                )
            }
            BrowserMode.DELETE -> {
                val item = browserState.items.getOrNull(browserState.cursor)
                drawBitmapText(
                    text = "DELETE ${item?.displayName}? A=YES B=NO",
                    x = x + 10,
                    y = topBarY1 + TEXT_PADDING,
                    scale = scale,
                    color = Color.Red,
                    spacing = CHAR_SPACING,
                    fontScale = FONT_SCALE
                )
            }
            BrowserMode.RENAME -> {
                drawBitmapText(
                    text = "[RENAME] ${browserState.renameBuffer}_ ←→=MOVE A=OK B=CANCEL",
                    x = x + 10,
                    y = topBarY1 + TEXT_PADDING,
                    scale = scale,
                    color = Color.Cyan,
                    spacing = CHAR_SPACING,
                    fontScale = FONT_SCALE
                )
            }
            BrowserMode.CREATE -> {
                drawBitmapText(
                    text = "[NEW FOLDER] ${browserState.renameBuffer}_ ←→=MOVE A=OK B=CANCEL",
                    x = x + 10,
                    y = topBarY1 + TEXT_PADDING,
                    scale = scale,
                    color = Color.Cyan,
                    spacing = CHAR_SPACING,
                    fontScale = FONT_SCALE
                )
            }
        }

        // Line 2: Path only (always shown)
        val pathText = browserState.currentDirectory.absolutePath.let { path ->
            if (path.length > 36) "..${path.takeLast(34)}" else path
        }
        drawBitmapText(
            text = pathText,
            x = x + 10,
            y = topBarY2 + TEXT_PADDING,
            scale = scale,
            color = Color(0xFF888888),
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // Add spacer after top bars (where header was)
        var rowY = topBarY2 + ROW_HEIGHT + 5  // 5px spacer

        // ===================================
        // FILE/FOLDER LIST
        // ===================================
        val visibleItems = browserState.items.drop(browserState.scroll).take(VISIBLE_ROWS)

        visibleItems.forEachIndexed { index, item ->
            val itemIndex = browserState.scroll + index
            val isCursor = itemIndex == browserState.cursor

            // Row background (zebra striping)
            val bgColor = when {
                isCursor -> Color(0xFF333333)
                index % 2 == 0 -> COLOR_BACKGROUND
                else -> Color(0xFF111111)
            }

            drawRect(
                color = bgColor,
                topLeft = Offset((x * scale).toFloat(), (rowY * scale).toFloat()),
                size = Size((WIDTH * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
            )

            // Choose color based on item type and cursor
            val textColor = when {
                isCursor -> COLOR_CURSOR
                item is BrowserItem.Parent -> COLOR_PARENT
                item is BrowserItem.Folder -> COLOR_FOLDER
                item is BrowserItem.FileItem &&
                    item.extension.lowercase() in VIDEO_EXTENSIONS -> COLOR_VIDEO
                item is BrowserItem.FileItem -> COLOR_FILE
                else -> COLOR_INACTIVE
            }

            // Draw cursor indicator
            if (isCursor) {
                drawBitmapText(
                    text = ">",
                    x = x + 10,
                    y = rowY + TEXT_PADDING,
                    scale = scale,
                    color = COLOR_CURSOR,
                    spacing = CHAR_SPACING,
                    fontScale = FONT_SCALE
                )
            }

            // Draw item name (truncate if too long)
            val displayText = item.displayName.take(30)
            drawBitmapText(
                text = displayText,
                x = x + 30,
                y = rowY + TEXT_PADDING,
                scale = scale,
                color = textColor,
                spacing = CHAR_SPACING,
                fontScale = FONT_SCALE
            )

            // Draw file size/date for files (not folders or parent)
            if (item is BrowserItem.FileItem) {
                // File size
                val sizeText = formatFileSize(item.file.length())
                drawBitmapText(
                    text = sizeText,
                    x = x + 330,
                    y = rowY + TEXT_PADDING,
                    scale = scale,
                    color = Color(0xFF666666),
                    spacing = CHAR_SPACING,
                    fontScale = FONT_SCALE
                )

                // Date
                val dateText = dateFormat.format(Date(item.file.lastModified()))
                drawBitmapText(
                    text = dateText,
                    x = x + 440,
                    y = rowY + TEXT_PADDING,
                    scale = scale,
                    color = Color(0xFF666666),
                    spacing = CHAR_SPACING,
                    fontScale = FONT_SCALE
                )
            }

            rowY += ROW_HEIGHT
        }

        // ===================================
        // BOTTOM STATUS BAR (single line at screen bottom)
        // ===================================
        // Position at absolute bottom of screen
        val bottomBarY = y + HEIGHT - ROW_HEIGHT

        // Draw background
        drawRect(
            color = Color(0xFF1a1a1a),
            topLeft = Offset((x * scale).toFloat(), (bottomBarY * scale).toFloat()),
            size = Size((WIDTH * scale).toFloat(), (ROW_HEIGHT * scale).toFloat())
        )

        // Basic controls on left
        drawBitmapText(
            text = "A=OPEN B=BACK R+←=UP R+↑↓=SORT",
            x = x + 10,
            y = bottomBarY + TEXT_PADDING,
            scale = scale,
            color = Color(0xFFaaaaaa),
            spacing = CHAR_SPACING,
            fontScale = FONT_SCALE
        )

        // File count on right
        if (browserState.items.isNotEmpty()) {
            val countText = "${browserState.cursor + 1}/${browserState.items.size}"
            drawBitmapText(
                text = countText,
                x = x + 560,
                y = bottomBarY + TEXT_PADDING,
                scale = scale,
                color = Color(0xFFaaaaaa),
                spacing = CHAR_SPACING,
                fontScale = FONT_SCALE
            )
        }
    }

    /**
     * Format file size for display (B, KB, MB)
     */
    private fun formatFileSize(bytes: Long): String {
        return when {
            bytes < 1024 -> "${bytes}B"
            bytes < 1024 * 1024 -> "${bytes / 1024}KB"
            else -> "${bytes / (1024 * 1024)}MB"
        }
    }
}

/**
 * Legacy FileBrowserState for backward compatibility
 * Use FileBrowserModule.State for new code
 */
@Deprecated("Use FileBrowserModule.State instead")
data class FileBrowserState(
    val files: List<File>,
    val cursorPosition: Int = 0,
    val scrollPosition: Int = 0,
    val directoryPath: String = "",
    val sortMode: FileSortMode = FileSortMode.DATE_DESC,
    val deleteConfirmMode: Boolean = false
)
