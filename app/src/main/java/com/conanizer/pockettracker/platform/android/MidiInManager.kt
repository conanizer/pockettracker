package com.conanizer.pockettracker.platform.android

import android.content.Context
import android.media.midi.MidiDeviceInfo
import android.media.midi.MidiOutputPort
import android.media.midi.MidiReceiver
import android.util.Log

/**
 * The Android MIDI input port — the Java half of `songcore::IMidiIn` (Java for [MidiOutManager]'s
 * reason). Everything musical is decided in C++.
 *
 * ⚠️ To RECEIVE you open the device's OUTPUT port ([MidiPorts]). Backwards, the user's synth is
 * offered as something to play from, and its port opens and never delivers a byte.
 *
 * [onSend] runs on a binder thread; everything else on the SDL thread. They meet only at [ring],
 * which `@Synchronized` guards.
 */
class MidiInManager(context: Context) {

    private val ports = MidiPorts<MidiOutputPort>(context, "in", MidiDeviceInfo.PortInfo.TYPE_OUTPUT) { d, n ->
        d.openOutputPort(n)
    }
    private var receiver: Receiver? = null

    // ⚠️ 1024 bytes, drop the NEWEST — as `songcore::MidiInQueue`: dropping the oldest would lose
    // note-ons whose note-offs are still coming. It fills only while the app is paused and a keyboard
    // keeps sending; drops are counted and logged.
    private val ring = ByteArray(1024)
    private var head = 0
    private var count = 0
    private var dropped = 0L
    private var warnedDrop = false

    /** How many devices can SEND to us. */
    fun deviceCount(): Int = ports.deviceCount()

    // What the INPUT row shows and settings.json stores.
    fun deviceName(index: Int): String = ports.deviceName(index)

    // Opens device [index] for receiving; true iff a port is live. ⚠️ Blocks up to 3 s.
    fun open(index: Int): Boolean {
        close()
        val p = ports.open(index) ?: return false

        // Emptied before the connect, so the first byte to arrive is kept.
        synchronized(this) { head = 0; count = 0; dropped = 0L; warnedDrop = false }

        // ⚠️ Without the connect an open port is silent: the device is held and nothing arrives.
        val r = Receiver()
        p.connect(r)
        receiver = r
        return true
    }

    /** Disconnect and release. Safe to call twice; the native side does exactly that on a re-pick. */
    fun close() {
        val r = receiver
        val p = ports.port
        if (r != null && p != null) runCatching { p.disconnect(r) }
        receiver = null
        ports.close()

        // The only place a dropped byte is ever reported.
        val lost = synchronized(this) { dropped }
        if (lost > 0) Log.w(TAG, "MIDI in: $lost byte(s) dropped - the app was not draining (paused?)")
    }

    // Moves waiting bytes into [out], returns how many. Native calls it once a frame, right before
    // songcore drains its own queue.
    @Synchronized
    fun read(out: ByteArray): Int {
        val n = if (count < out.size) count else out.size
        for (i in 0 until n) out[i] = ring[(head + i) % ring.size]
        head = (head + n) % ring.size
        count -= n
        return n
    }

    // The binder-thread end of the ring.
    private inner class Receiver : MidiReceiver() {
        override fun onSend(msg: ByteArray, offset: Int, count: Int, timestamp: Long) {
            // ⚠️ The timestamp is dropped: a live key is scheduled on the audio clock, unrelated to it.
            push(msg, offset, count)
        }
    }

    @Synchronized
    private fun push(msg: ByteArray, offset: Int, len: Int) {
        for (i in 0 until len) {
            if (count >= ring.size) {
                dropped += (len - i).toLong()
                if (!warnedDrop) {
                    warnedDrop = true
                    Log.w(TAG, "MIDI in: the ${ring.size}-byte ring is full - dropping. " +
                            "Is the app paused, or the frame loop stalled?")
                }
                return
            }
            ring[(head + count) % ring.size] = msg[offset + i]
            count++
        }
    }

    private companion object {
        const val TAG = "PocketTracker"
    }
}
