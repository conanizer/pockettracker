package com.example.pockettracker.core.logging

/**
 * Platform-agnostic logging interface.
 *
 * Implementations:
 * - Android: AndroidLogger (uses android.util.Log)
 * - Linux: ConsoleLogger (uses println)
 * - Testing: MockLogger (captures logs for assertions)
 */
interface ILogger {
    /**
     * Log a debug message.
     */
    fun d(tag: String, message: String)

    /**
     * Log an info message.
     */
    fun i(tag: String, message: String)

    /**
     * Log a warning message.
     */
    fun w(tag: String, message: String)

    /**
     * Log an error message.
     */
    fun e(tag: String, message: String)

    /**
     * Log an error with exception.
     */
    fun e(tag: String, message: String, throwable: Throwable)
}
