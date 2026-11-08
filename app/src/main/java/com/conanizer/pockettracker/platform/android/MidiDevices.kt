package com.conanizer.pockettracker.platform.android

import android.content.Context
import android.media.midi.MidiDevice
import android.media.midi.MidiDeviceInfo
import android.media.midi.MidiManager
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.io.Closeable
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * One direction's device list and open port, shared by [MidiOutManager] and [MidiInManager].
 *
 * ⚠️ Port names are from the device's side: to SEND you open its INPUT port, to RECEIVE its OUTPUT
 * port. [portType] is the device's port this direction opens; backwards, a port opens and nothing
 * goes through. Every method runs on the SDL thread; [open] blocks it.
 */
internal class MidiPorts<P : Closeable>(
    context: Context,
    private val direction: String,   // "in" or "out", for the log
    private val portType: Int,       // MidiDeviceInfo.PortInfo.TYPE_INPUT or TYPE_OUTPUT
    private val openPort: (MidiDevice, Int) -> P?,
) {
    private val manager: MidiManager? =
        context.getSystemService(Context.MIDI_SERVICE) as? MidiManager

    // The snapshot the native indices refer to, refreshed only by [deviceCount] — the native side
    // always calls count → name → open in that order.
    private var devices: List<MidiDeviceInfo> = emptyList()

    private var device: MidiDevice? = null
    var port: P? = null
        private set

    private var warnedNoManager = false

    /** How many devices have a port of [portType]. Re-enumerates: MIDI is hot-pluggable. */
    fun deviceCount(): Int {
        val m = manager
        if (m == null) {
            devices = emptyList()
            // Once: the list is rescanned every second.
            if (!warnedNoManager) {
                warnedNoManager = true
                Log.w(TAG, "no MidiManager on this device (no FEATURE_MIDI) - MIDI $direction unavailable")
            }
            return 0
        }
        // Sorted by id for a stable order in a session; the setting stores the NAME, since a replug
        // renumbers everything.
        @Suppress("DEPRECATION")   // getDevicesForTransport is API 33; this app's floor is 26
        devices = m.devices.filter {
            if (portType == MidiDeviceInfo.PortInfo.TYPE_INPUT) it.inputPortCount > 0 else it.outputPortCount > 0
        }.sortedBy { it.id }
        return devices.size
    }

    // What the MIDI row shows and settings.json stores (see [midiDeviceName]).
    fun deviceName(index: Int): String = devices.getOrNull(index)?.let(::midiDeviceName).orEmpty()

    // Opens device [index] and its first port of [portType]; the port, or null. ⚠️ Blocks up to 3 s
    // ([openMidiDevice]).
    fun open(index: Int): P? {
        close()
        val m = manager ?: return null
        val info = devices.getOrNull(index) ?: return null

        val portNumber = info.ports.firstOrNull { it.type == portType }?.portNumber
        if (portNumber == null) {
            Log.w(TAG, "MIDI $direction: device ${info.id} has no port of that direction")
            return null
        }

        val d = openMidiDevice(m, info) ?: return null
        val p = try {
            openPort(d, portNumber)
        } catch (e: Exception) {
            Log.w(TAG, "MIDI $direction: opening port $portNumber threw", e)
            null
        }
        if (p == null) {
            Log.w(TAG, "MIDI $direction: port $portNumber failed - it may be in use by another app")
            runCatching { d.close() }
            return null
        }

        device = d
        port = p
        Log.i(TAG, "MIDI $direction open: ${deviceName(index)} (device ${info.id}, port $portNumber)")
        return p
    }

    /** Releases the port and the device. Safe to call twice. */
    fun close() {
        runCatching { port?.close() }.onFailure { Log.w(TAG, "port.close threw", it) }
        runCatching { device?.close() }.onFailure { Log.w(TAG, "device.close threw", it) }
        port = null
        device = null
    }
}

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

    // ⚠️ A device that answers after the wait gave up is closed by the callback, or it stays held
    // from every other app until the process dies.
    val lock = Any()
    var opened: MidiDevice? = null
    var gaveUp = false
    val latch = CountDownLatch(1)
    m.openDevice(info, { d ->
        val late = synchronized(lock) {
            if (!gaveUp) opened = d
            gaveUp
        }
        if (late) runCatching { d?.close() }
        latch.countDown()
    }, Handler(Looper.getMainLooper()))

    val arrived = latch.await(OPEN_TIMEOUT_MS, TimeUnit.MILLISECONDS)
    val d = synchronized(lock) {
        gaveUp = true
        opened
    }
    if (d == null) {
        Log.w(TAG, "openDevice(${info.id}) ${if (arrived) "returned null" else "timed out"}")
        return null
    }
    return d
}

private const val TAG = "PocketTracker"
private const val OPEN_TIMEOUT_MS = 3000L
