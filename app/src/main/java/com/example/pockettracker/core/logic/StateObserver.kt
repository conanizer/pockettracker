package com.example.pockettracker.core.logic

/**
 * Platform-agnostic state change observer.
 *
 * Controllers notify this when their state changes,
 * allowing UI layers to trigger recomposition.
 *
 * ✅ PLATFORM-AGNOSTIC - No Android dependencies!
 */
interface StateObserver {
    /**
     * Called when controller state changes.
     * UI layer should trigger recomposition.
     */
    fun onStateChanged()
}
