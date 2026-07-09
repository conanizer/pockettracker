package com.conanizer.pockettracker.core.logic

import kotlin.properties.ReadWriteProperty
import kotlin.reflect.KProperty

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

/**
 * Property delegate: stores each set (through [transform]) and notifies this observer —
 * replaces the ~30 identical `set(value) { field = value(+coerce); stateObserver
 * .onStateChanged() }` bodies across controllers. [transform] runs at set-time, so dynamic
 * bounds (e.g. `project.grooves.lastIndex`) stay live. Properties with additional side
 * effects keep explicit setters.
 */
fun <T> StateObserver.observed(initial: T, transform: (T) -> T = { it }): ReadWriteProperty<Any?, T> =
    object : ReadWriteProperty<Any?, T> {
        private var value = initial
        override fun getValue(thisRef: Any?, property: KProperty<*>): T = value
        override fun setValue(thisRef: Any?, property: KProperty<*>, newValue: T) {
            value = transform(newValue)
            this@observed.onStateChanged()
        }
    }
