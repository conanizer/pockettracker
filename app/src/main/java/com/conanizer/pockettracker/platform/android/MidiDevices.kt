package com.conanizer.pockettracker.platform.android

import android.media.midi.MidiDevice
import android.media.midi.MidiDeviceInfo
import android.media.midi.MidiManager
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

// What the MIDI rows show and settings.json stores. Never empty: a blank row would look like OFF.
internal fun midiDeviceName(info: MidiDeviceInfo): String {
    val props = info.properties

    props.getString(MidiDeviceInfo.PROPERTY_NAME)
        ?.trim()?.takeIf { it.isNotEmpty() }
        ?.let { return it }

    val maker = props.getString(MidiDeviceInfo.PROPERTY_MANUFACTURER)?.trim().orEmpty()
    val product = props.getString(MidiDeviceInfo.PROPERTY_PRODUCT)?.trim().orEmpty()
    val joined = listOf(maker, product).filter { it.isNotEmpty() }.joinToString(" ")
    return joined.ifEmpty { "MIDI ${info.id}" }
}

// Opens [info], blocking up to 3 s: `openDevice` has no synchronous form, and the MIDI rows show what
// is OPEN, not what was wanted. A real open takes tens of ms. Null on failure, logged.
internal fun openMidiDevice(m: MidiManager, info: MidiDeviceInfo): MidiDevice? {
    // The callback arrives on the main looper, so waiting on the main thread would deadlock.
    if (Looper.myLooper() == Looper.getMainLooper()) {
        Log.e(TAG, "open() called on the main thread - refused (it would deadlock on openDevice)")
        return null
    }

    // The latch publishes `opened`: countDown() happens-before a returning await().
    var opened: MidiDevice? = null
    val latch = CountDownLatch(1)
    m.openDevice(info, { d -> opened = d; latch.countDown() }, Handler(Looper.getMainLooper()))

    val arrived = latch.await(OPEN_TIMEOUT_MS, TimeUnit.MILLISECONDS)
    val d = opened
    if (!arrived || d == null) {
        Log.w(TAG, "openDevice(${info.id}) ${if (arrived) "returned null" else "timed out"}")
        return null
    }
    return d
}

private const val TAG = "PocketTracker"
private const val OPEN_TIMEOUT_MS = 3000L
