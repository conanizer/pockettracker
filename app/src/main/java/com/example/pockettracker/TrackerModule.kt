package com.example.pockettracker

import androidx.compose.ui.graphics.drawscope.DrawScope

/**
 * BASE INTERFACE FOR ALL TRACKER MODULES
 *
 * Each module is a self-contained visual component that:
 * - Knows its own dimensions
 * - Draws itself at a given position
 * - Takes state data as input
 *
 * Think of modules as LEGO blocks - they all fit together
 * because they follow the same interface!
 */
interface TrackerModule {
    /**
     * Module width in design pixels
     * Example: 620px
     */
    val width: Int

    /**
     * Module height in design pixels
     * Example: 392px
     */
    val height: Int

    /**
     * Draw the module at specified position
     *
     * @param x X position in design pixels (before scaling)
     * @param y Y position in design pixels (before scaling)
     * @param scale Screen scale factor (1×, 2×, 3×, etc.)
     * @param state Module-specific state data (cast to appropriate type)
     */
    fun DrawScope.draw(
        x: Int,
        y: Int,
        scale: Int,
        state: Any? = null
    )
}