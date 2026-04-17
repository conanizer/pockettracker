package com.conanizer.pockettracker.core.data

import kotlinx.serialization.Serializable

/**
 * Portable instrument preset file format (.pti).
 *
 * Bundles all instrument parameters (type, source path, all settings, mod slots)
 * plus embedded table data so the preset is self-contained across projects.
 *
 * Serialized as JSON with ignoreUnknownKeys = true for forward compatibility.
 */
@Serializable
data class InstrumentPreset(
    val version: Int = 1,
    val instrument: Instrument,
    val tableRows: Array<TableRow>? = null  // Embedded table rows (null if instrument.tableId == -1)
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is InstrumentPreset) return false
        return version == other.version &&
            instrument == other.instrument &&
            tableRows.contentDeepEquals(other.tableRows)
    }

    override fun hashCode(): Int {
        var result = version
        result = 31 * result + instrument.hashCode()
        result = 31 * result + (tableRows?.contentDeepHashCode() ?: 0)
        return result
    }
}
