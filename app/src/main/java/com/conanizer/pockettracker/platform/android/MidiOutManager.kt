package com.conanizer.pockettracker.platform.android

import android.content.Context
import android.media.midi.MidiDevice
import android.media.midi.MidiDeviceInfo
import android.media.midi.MidiInputPort
import android.media.midi.MidiManager
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * The Android MIDI **output** port — the Java half of `songcore::IMidiOut` (MIDI plan phase B2b).
 *
 * ## ⚠️ Why this file exists at all, when Phase E deleted the Kotlin UI
 *
 * Convergence left `app/src/main/java` a four-file shim; this is the fifth, and the MIDI plan named
 * the cost when it was ratified rather than discovering it here. `android.media.midi.MidiManager` is
 * the **only** sanctioned route to USB, virtual and BLE MIDI devices on Android, and it is a Java
 * API. The NDK's AMidi does not save us: it is API 29+ (this app's floor is 26) and it *still*
 * requires Java-side `MidiManager` to enumerate and open a device — it only makes the byte I/O
 * native, which was never the part that needed help. Raw `UsbManager` bulk transfers would mean
 * hand-writing the USB-MIDI class driver and would lose virtual (app-to-app) devices entirely.
 *
 * So this is the same shape [ButtonSoundManager] and [ButtonHapticManager] already have: an Android
 * system service with no C++ twin, reached through one narrow outward hook. Everything above it —
 * the serializer, the note lifecycle, the gate lengths, the panic, the OFFSET — is C++ and is shared
 * with the Windows and Linux builds. Nothing musical is decided here.
 *
 * ## ⚠️⚠️ THE DIRECTION GOTCHA — the easiest thing in this file to get backwards
 *
 * **To SEND MIDI you open the device's INPUT port.** Port names are written from the *device's* point
 * of view: a synth you play *has* an input; a keyboard you play *from* has an output. So the list this
 * class exposes is `inputPortCount > 0` — devices that can RECEIVE — and a MIDI keyboard correctly
 * does not appear on it. Backwards, this lists exactly the wrong devices and the symptom is "the port
 * opened and the synth is silent", which reads as a sequencer bug rather than a port one.
 *
 * ## Threading
 *
 * Every method is called from the **SDL thread** (the native frame loop), never the UI thread.
 * `MidiInputPort.send` is safe from any thread. [open] BLOCKS that thread — see its own note.
 */
class MidiOutManager(context: Context) {

    private val manager: MidiManager? =
        context.getSystemService(Context.MIDI_SERVICE) as? MidiManager

    /**
     * The snapshot the native side's indices refer to. Refreshed by [deviceCount] and by nothing
     * else, which is what makes `deviceCount()` → `deviceName(i)` → `openDevice(i)` a consistent
     * sequence: the C++ caller (`InputDispatcher::refresh_midi_devices`) always walks it in that order.
     */
    private var devices: List<MidiDeviceInfo> = emptyList()

    private var device: MidiDevice? = null
    private var port: MidiInputPort? = null

    /**
     * One reusable buffer, because [send] runs per MIDI message and a 3-byte allocation per note is
     * pure garbage-collector pressure on the one path that must not stutter. Safe only because every
     * call arrives on the single thread that pumps the queue — see the class note.
     */
    private val scratch = ByteArray(3)

    /** How many devices we can SEND to. Re-enumerates: MIDI is hot-pluggable. */
    fun deviceCount(): Int {
        val m = manager
        if (m == null) {
            devices = emptyList()
            Log.w(TAG, "no MidiManager on this device (no FEATURE_MIDI) - MIDI out unavailable")
            return 0
        }
        // ⚠️ `inputPortCount > 0`, not output — see the class note. Sorted by id so the order is
        // stable within a session; the SETTING is stored as a NAME precisely because it is not stable
        // ACROSS one (a replug renumbers everything).
        @Suppress("DEPRECATION")   // getDevicesForTransport is API 33; this app's floor is 26
        devices = m.devices.filter { it.inputPortCount > 0 }.sortedBy { it.id }
        return devices.size
    }

    /**
     * What the OUTPUT row shows, and what settings.json stores. Never empty for a valid index: a
     * device with no advertised name still gets `MIDI <id>`, because a blank row is indistinguishable
     * from the OFF row above it.
     */
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
     * Open device [index] for sending. Returns whether a port is actually live.
     *
     * ⚠️ **THIS BLOCKS THE CALLING THREAD**, for up to [OPEN_TIMEOUT_MS]. `MidiManager.openDevice` is
     * asynchronous and there is no synchronous form, so something has to wait — and the alternative
     * was considered and rejected: returning an optimistic `true` and letting the port arrive later
     * would make the MIDI screen's OUTPUT row claim a device that may never open, and that row's one
     * design rule (plan §0.1) is that it shows what is OPEN, not what was WANTED. A row that lies
     * about a cable is worse than a frame-loop stall the user caused by pressing a button, which is
     * what this is: [open] is only ever reached from picking a port or from boot. Real opens take
     * tens of milliseconds; the timeout exists for the device that is unplugged mid-handshake.
     */
    fun open(index: Int): Boolean {
        close()
        val m = manager ?: return false
        val info = devices.getOrNull(index) ?: return false

        // ⚠️ A GUARD AGAINST A FUTURE CALLER, not against today's. The callback below is delivered on
        // the MAIN looper, so waiting for it FROM the main thread would deadlock this app solid. The
        // native side calls from the SDL thread and always will; this makes the assumption fail loudly
        // in a log rather than silently as a five-second ANR if that ever changes.
        if (Looper.myLooper() == Looper.getMainLooper()) {
            Log.e(TAG, "open() called on the main thread - refused (it would deadlock on openDevice)")
            return false
        }

        val portNumber = info.ports
            .firstOrNull { it.type == MidiDeviceInfo.PortInfo.TYPE_INPUT }
            ?.portNumber
        if (portNumber == null) {
            Log.w(TAG, "device ${info.id} has no input port - cannot send to it")
            return false
        }

        // The captured `opened` is published across threads by the latch, not by @Volatile:
        // countDown() happens-before a returning await(), which is exactly the edge needed.
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
            d.openInputPort(portNumber)
        } catch (e: Exception) {
            Log.w(TAG, "openInputPort($portNumber) threw", e)
            null
        }
        if (p == null) {
            Log.w(TAG, "openInputPort($portNumber) failed - the port may be in use by another app")
            runCatching { d.close() }
            return false
        }

        device = d
        port = p
        Log.i(TAG, "MIDI out open: ${deviceName(index)} (device ${info.id}, input port $portNumber)")
        return true
    }

    /**
     * Release the port and the device.
     *
     * ⚠️ Does NOT send all-notes-off — the native side does that immediately before calling this
     * (`MidiOutBase::panic_all_channels`), so the panic is written once for the ALSA and Android
     * backends both rather than twice in two languages.
     */
    fun close() {
        runCatching { port?.close() }.onFailure { Log.w(TAG, "port.close threw", it) }
        runCatching { device?.close() }.onFailure { Log.w(TAG, "device.close threw", it) }
        port = null
        device = null
    }

    /** One MIDI message, 1–3 bytes, already serialized by songcore. False = it did not go out. */
    fun send(b0: Int, b1: Int, b2: Int, len: Int): Boolean {
        val p = port ?: return false
        if (len < 1 || len > 3) return false
        scratch[0] = b0.toByte()
        scratch[1] = b1.toByte()
        scratch[2] = b2.toByte()
        return try {
            p.send(scratch, 0, len)
            true
        } catch (e: Exception) {
            // Counted on the native side and surfaced by error_count(). Not logged per message: a
            // yanked cable would otherwise write a line per note for as long as the song plays.
            false
        }
    }

    private companion object {
        const val TAG = "PocketTracker"
        const val OPEN_TIMEOUT_MS = 3000L
    }
}
