package com.example.pockettracker.core.logic

import android.util.Log
import com.example.pockettracker.Project
import com.example.pockettracker.PhraseStep

/**
 * CLIPBOARD MANAGER
 *
 * Manages clipboard for copy/paste operations.
 *
 * ✅ PLATFORM-AGNOSTIC - No Android dependencies (except Log which will be abstracted later)
 *
 * NOTE: Full copy/paste implementation comes in MVP Milestone 2.5.
 * This provides the structure and data types for now.
 *
 * M8-Style Copy/Paste:
 * - SELECT + B: Enter selection mode
 * - B (in selection mode): Copy selection
 * - SELECT + A: Paste
 * - A + B: Cut (copy + delete)
 */
class ClipboardManager {
    private val TAG = "ClipboardManager"

    // ========================================
    // CLIPBOARD DATA
    // ========================================

    /**
     * Current clipboard contents.
     * Null = clipboard is empty.
     */
    var clipboard: ClipboardData? = null
        private set

    /**
     * Clipboard data container.
     *
     * @param type What kind of data is in the clipboard
     * @param data The actual clipboard data (List<PhraseStep>, Phrase, etc.)
     * @param width How many columns (for multi-column selections)
     * @param height How many rows
     */
    data class ClipboardData(
        val type: ClipboardType,
        val data: Any,  // Will be cast based on type
        val width: Int,
        val height: Int
    )

    /**
     * Types of clipboard content.
     */
    enum class ClipboardType {
        PHRASE_STEPS,   // Selection of phrase steps (rows × columns)
        PHRASE,         // Entire phrase (16 steps)
        CHAIN_ROWS,     // Selection of chain rows
        CHAIN           // Entire chain (16 phrases)
    }

    // ========================================
    // OPERATIONS
    // ========================================

    /**
     * Copy data to clipboard.
     *
     * @param type Type of data being copied
     * @param data The data to copy
     * @param width Number of columns
     * @param height Number of rows
     */
    fun copy(type: ClipboardType, data: Any, width: Int, height: Int) {
        clipboard = ClipboardData(type, data, width, height)
        Log.d(TAG, "📋 Copied: type=$type, ${width}x${height}")
    }

    /**
     * Cut data to clipboard (copy + delete source).
     *
     * TODO: Implement in Milestone 2.5
     * - Copy data to clipboard
     * - Delete source data
     * - Return CutResult
     */
    fun cut(type: ClipboardType, data: Any, width: Int, height: Int): CutResult {
        // Stub - full implementation in Milestone 2.5
        copy(type, data, width, height)
        Log.d(TAG, "⏳ Cut stub: type=$type (delete not implemented yet)")
        return CutResult.Success(width * height)
    }

    /**
     * Paste clipboard contents.
     *
     * TODO: Implement in Milestone 2.5
     * - Validate clipboard type matches target context
     * - Paste data at cursor position
     * - Handle oversized paste (truncate or wrap)
     *
     * @param project The project to paste into
     * @param targetScreen Which screen (PHRASE, CHAIN, etc.)
     * @param cursorRow Current cursor row
     * @param cursorColumn Current cursor column
     * @return PasteResult indicating success or failure
     */
    fun paste(
        project: Project,
        targetScreen: String,  // "PHRASE", "CHAIN", etc.
        cursorRow: Int,
        cursorColumn: Int
    ): PasteResult {
        val clip = clipboard ?: return PasteResult.NoClipboard

        // Stub - full implementation in Milestone 2.5
        Log.d(TAG, "⏳ Paste stub: type=${clip.type}, target=$targetScreen, pos=($cursorRow,$cursorColumn)")
        Log.d(TAG, "   Clipboard size: ${clip.width}x${clip.height}")

        return PasteResult.Success(itemsPasted = clip.width * clip.height)
    }

    /**
     * Clear clipboard.
     */
    fun clear() {
        clipboard = null
        Log.d(TAG, "🗑️ Clipboard cleared")
    }

    /**
     * Check if clipboard has data.
     */
    fun hasData(): Boolean = clipboard != null

    /**
     * Get clipboard info string for UI display.
     */
    fun getClipboardInfo(): String {
        val clip = clipboard ?: return "EMPTY"
        return "${clip.type} ${clip.width}x${clip.height}"
    }

    // ========================================
    // RESULT TYPES
    // ========================================

    /**
     * Result of paste operation.
     */
    sealed class PasteResult {
        object NoClipboard : PasteResult()
        data class Success(val itemsPasted: Int) : PasteResult()
        data class Error(val message: String) : PasteResult()
    }

    /**
     * Result of cut operation.
     */
    sealed class CutResult {
        data class Success(val itemsCut: Int) : CutResult()
        data class Error(val message: String) : CutResult()
    }
}
