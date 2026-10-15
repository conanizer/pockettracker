package com.conanizer.pockettracker.platform.android

import android.content.Context
import android.media.midi.MidiDevice
import android.media.midi.MidiDeviceInfo
import android.media.midi.MidiOutputPort
import android.media.midi.MidiManager
import android.media.midi.MidiReceiver
import android.util.Log

/**
 * The Android MIDI input port — the Java half of `songcore::IMidiIn` (Java for [MidiOutManager]'s
 * reason). Everything musical is decided in C++.
 *
 * ⚠️ To RECEIVE you open the device's OUTPUT port — the opposite of [MidiOutManager]. Backwards, the
 * user's synth is offered as something to play from, and its port opens and never delivers a byte.
 *
 * [onSend] runs on a binder thread; everything else on the SDL thread. They meet only at [ring],
 * which `@Synchronized` guards.
 */
class MidiInManager(context: Context) {

    private val manager: MidiManager? =
        context.getSystemService(Context.MIDI_SERVICE) as? MidiManager

    /** The snapshot the native indices refer to, refreshed only by [deviceCount]. */
    private var devices: List<MidiDeviceInfo> = emptyList()

    private var device: MidiDevice? = null
    private var port: MidiOutputPort? = null
    private var receiver: Receiver? = null

    // ⚠️ 1024 bytes, drop the NEWEST — as `songcore::MidiInQueue`: dropping the oldest would lose
    // note-ons whose note-offs are still coming. It fills only while the app is paused and a keyboard
    // keeps sending; drops are counted and logged.
    private val ring = ByteArray(1024)
    private var head = 0
    private var count = 0
    private var dropped = 0L
    private var warnedDrop = false

    private var warnedNoManager = false

    /** How many devices can SEND to us. Re-enumerates: MIDI is hot-pluggable. */
    fun deviceCount(): Int {
        val m = manager
        if (m == null) {
            devices = emptyList()
            // Once: the list is rescanned every second.
            if (!warnedNoManager) {
                warnedNoManager = true
                Log.w(TAG, "no MidiManager on this device (no FEATURE_MIDI) - MIDI in unavailable")
            }
            return 0
        }
        // ⚠️ `outputPortCount`, see the class note. Sorted by id; the setting stores the NAME.
        @Suppress("DEPRECATION")   // getDevicesForTransport is API 33; this app's floor is 26
        devices = m.devices.filter { it.outputPortCount > 0 }.sortedBy { it.id }
        return devices.size
    }

    // What the INPUT row shows and settings.json stores (see [midiDeviceName]).
    fun deviceName(index: Int): String = devices.getOrNull(index)?.let(::midiDeviceName).orEmpty()

    // Opens device [index] for receiving; true iff a port is live. ⚠️ Blocks up to 3 s ([openMidiDevice]).
    fun open(index: Int): Boolean {
        close()
        val m = manager ?: return false
        val info = devices.getOrNull(index) ?: return false

        val portNumber = info.ports
            .firstOrNull { it.type == MidiDeviceInfo.PortInfo.TYPE_OUTPUT }
            ?.portNumber
        if (portNumber == null) {
            Log.w(TAG, "device ${info.id} has no output port - it cannot send to us")
            return false
        }

        val d = openMidiDevice(m, info) ?: return false

        val p = try {
            d.openOutputPort(portNumber)
        } catch (e: Exception) {
            Log.w(TAG, "openOutputPort($portNumber) threw", e)
            null
        }
        if (p == null) {
            Log.w(TAG, "openOutputPort($portNumber) failed - the port may be in use by another app")
            runCatching { d.close() }
            return false
        }

        // ⚠️ Without the connect an open port is silent: the device is held and nothing arrives.
        val r = Receiver()
        p.connect(r)

        synchronized(this) { head = 0; count = 0; dropped = 0L; warnedDrop = false }

        device = d
        port = p
        receiver = r
        Log.i(TAG, "MIDI in open: ${deviceName(index)} (device ${info.id}, output port $portNumber)")
        return true
    }

    /** Disconnect and release. Safe to call twice; the native side does exactly that on a re-pick. */
    fun close() {
        val r = receiver
        val p = port
        if (r != null && p != null) runCatching { p.disconnect(r) }
        runCatching { p?.close() }.onFailure { Log.w(TAG, "port.close threw", it) }
        runCatching { device?.close() }.onFailure { Log.w(TAG, "device.close threw", it) }
        receiver = null
        port = null
        device = null

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
