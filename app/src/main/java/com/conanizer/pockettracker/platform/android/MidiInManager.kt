package com.conanizer.pockettracker.platform.android

import android.content.Context
import android.media.midi.MidiDevice
import android.media.midi.MidiDeviceInfo
import android.media.midi.MidiOutputPort
import android.media.midi.MidiManager
import android.media.midi.MidiReceiver
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * The Android MIDI **input** port — the Java half of `songcore::IMidiIn` (MIDI plan phase E5).
 *
 * The mirror of [MidiOutManager], written for the same unavoidable reason: `MidiManager` is the only
 * sanctioned route to USB, virtual and BLE MIDI on Android and it is a Java API (AMidi is API 29+, this
 * app's floor is 26, and it still needs the Java side to enumerate and open). Everything above it — the
 * MIDI 1.0 parser, the channel→track map, which instrument a live key plays, whether the key goes back
 * out on the cable — is C++ and is shared with the Windows and Linux builds. Nothing musical is
 * decided here.
 *
 * ## ⚠️⚠️ THE DIRECTION GOTCHA — and it is the OPPOSITE of [MidiOutManager]'s
 *
 * **To RECEIVE MIDI you open the device's OUTPUT port.** Port names are written from the *device's*
 * point of view: a keyboard you play FROM has an output; a synth you play HAS an input. So the list
 * this class exposes is `outputPortCount > 0` — devices that can SEND — where [MidiOutManager] asks for
 * `inputPortCount > 0`. Backwards, this offers the user's synth as something to play from, and the
 * symptom is a port that opens and never delivers a byte: a wrong list, never an error.
 *
 * ## ⚠️ Threading, and the one lock in this file
 *
 * [onSend] arrives on a **binder thread** owned by the MIDI service. Everything else ([deviceCount],
 * [deviceName], [open], [close], [read]) is called from the **SDL thread** (the native frame loop).
 * Those two meet at [ring], and that is the only thing `@Synchronized` here protects. The C++ side
 * holds no lock of its own for this, deliberately: the lock lives next to the data it protects rather
 * than in a language that cannot see it — the same argument [MidiOutManager.send] carries since B3.
 */
class MidiInManager(context: Context) {

    private val manager: MidiManager? =
        context.getSystemService(Context.MIDI_SERVICE) as? MidiManager

    /** The snapshot the native side's indices refer to. Refreshed by [deviceCount] and nothing else. */
    private var devices: List<MidiDeviceInfo> = emptyList()

    private var device: MidiDevice? = null
    private var port: MidiOutputPort? = null
    private var receiver: Receiver? = null

    // ── The ring, and why it is these numbers ────────────────────────────────────────────────────
    //
    // ⚠️ 1024 bytes and DROP THE NEWEST — deliberately the same capacity and the same policy as
    // `songcore::MidiInQueue`, whose reasoning is written out in midi_in.h: MIDI 1.0 is 31 250 baud
    // ≈ 1 040 three-byte messages a second, so a 60 Hz drain has two orders of magnitude of headroom,
    // and dropping the OLDEST instead would throw away note-ONs whose note-OFFs are still coming — the
    // one failure mode that costs a stuck note.
    //
    // ⚠️ **THE CASE THAT ACTUALLY FILLS IT IS THE PAUSED ACTIVITY**, not a fast player: SDL freezes the
    // native thread while the app is in the background, so nothing reads this for as long as the user is
    // away and a connected keyboard keeps sending. That is why the drops are counted and logged rather
    // than assumed impossible.
    private val ring = ByteArray(1024)
    private var head = 0
    private var count = 0
    private var dropped = 0L
    private var warnedDrop = false

    /** How many devices can SEND to us. Re-enumerates: MIDI is hot-pluggable. */
    fun deviceCount(): Int {
        val m = manager
        if (m == null) {
            devices = emptyList()
            Log.w(TAG, "no MidiManager on this device (no FEATURE_MIDI) - MIDI in unavailable")
            return 0
        }
        // ⚠️ `outputPortCount > 0`, not input — see the class note. Sorted by id so the order is stable
        // within a session; the SETTING is a NAME precisely because it is not stable across one.
        @Suppress("DEPRECATION")   // getDevicesForTransport is API 33; this app's floor is 26
        devices = m.devices.filter { it.outputPortCount > 0 }.sortedBy { it.id }
        return devices.size
    }

    /** What the INPUT row shows, and what settings.json stores. Never empty for a valid index. */
    fun deviceName(index: Int): String {
        val info = devices.getOrNull(index) ?: return ""
        val props = info.properties

        props.getString(MidiDeviceInfo.PROPERTY_NAME)
            ?.trim()?.takeIf { it.isNotEmpty() }
            ?.let { return it }

        val maker = props.getString(MidiDeviceInfo.PROPERTY_MANUFACTURER)?.trim().orEmpty()
        val product = props.getString(MidiDeviceInfo.PROPERTY_PRODUCT)?.trim().orEmpty()
        val joined = listOf(maker, product).filter { it.isNotEmpty() }.joinToString(" ")
        return joined.ifEmpty { "MIDI ${info.id}" }
    }

    /**
     * Open device [index] for receiving. Returns whether a port is actually live.
     *
     * ⚠️ **THIS BLOCKS THE CALLING THREAD** for up to [OPEN_TIMEOUT_MS], for [MidiOutManager.open]'s
     * reason exactly: `MidiManager.openDevice` is asynchronous, and the MIDI screen's rows show what is
     * OPEN rather than what was WANTED. An optimistic `true` would make the INPUT row claim a keyboard
     * that may never arrive — and on an input path, "the row says it is open and nothing happens" is
     * indistinguishable from every other way MIDI in can be silent.
     */
    fun open(index: Int): Boolean {
        close()
        val m = manager ?: return false
        val info = devices.getOrNull(index) ?: return false

        // A guard against a FUTURE caller: the callback below is delivered on the main looper, so
        // waiting for it FROM the main thread would deadlock the app solid. The native side calls from
        // the SDL thread and always will; this makes that assumption fail in a log rather than as an ANR.
        if (Looper.myLooper() == Looper.getMainLooper()) {
            Log.e(TAG, "open() called on the main thread - refused (it would deadlock on openDevice)")
            return false
        }

        val portNumber = info.ports
            .firstOrNull { it.type == MidiDeviceInfo.PortInfo.TYPE_OUTPUT }
            ?.portNumber
        if (portNumber == null) {
            Log.w(TAG, "device ${info.id} has no output port - it cannot send to us")
            return false
        }

        var opened: MidiDevice? = null
        val latch = CountDownLatch(1)
        m.openDevice(info, { d -> opened = d; latch.countDown() }, Handler(Looper.getMainLooper()))

        val arrived = latch.await(OPEN_TIMEOUT_MS, TimeUnit.MILLISECONDS)
        val d = opened
        if (!arrived || d == null) {
            Log.w(TAG, "openDevice(${info.id}) ${if (arrived) "returned null" else "timed out"}")
            return false
        }

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

        // ⚠️ **THE CONNECT IS WHAT MAKES BYTES FLOW, and an open port without one is silent forever** —
        // the exact shape of winmm's `midiInStart` trap (midi-in-winmm.cpp): the device is held, no
        // other app can have it, and nothing arrives. It reads as "the keyboard is broken".
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

        // ⚠️ The number beside the verdict, and the only place it can be said: a session that dropped
        // bytes and one that did not are otherwise identical from every seat in the app.
        val lost = synchronized(this) { dropped }
        if (lost > 0) Log.w(TAG, "MIDI in: $lost byte(s) dropped - the app was not draining (paused?)")
    }

    /**
     * Move up to `out.size` waiting bytes into [out]; returns how many. Called once a frame by
     * `AndroidMidiIn::pump`, immediately before songcore drains its own queue.
     */
    @Synchronized
    fun read(out: ByteArray): Int {
        val n = if (count < out.size) count else out.size
        for (i in 0 until n) out[i] = ring[(head + i) % ring.size]
        head = (head + n) % ring.size
        count -= n
        return n
    }

    /** The binder-thread end of the ring. Nothing else in this class runs on that thread. */
    private inner class Receiver : MidiReceiver() {
        override fun onSend(msg: ByteArray, offset: Int, count: Int, timestamp: Long) {
            // ⚠️ The timestamp is DISCARDED, and that is a decision. `MidiManager` stamps events in
            // `System.nanoTime()`, but the app has nowhere to put it: a live key is scheduled against
            // the AUDIO clock at `clock + 100` frames (songcore/host.h) and the two clocks have no
            // agreed relationship. Keeping the value would mean pretending to a precision the injection
            // does not have. winmm's backend discards its timestamp for the same reason.
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
        const val OPEN_TIMEOUT_MS = 3000L
    }
}
