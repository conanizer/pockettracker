package com.conanizer.pockettracker.platform.android

import android.content.Context
import android.media.midi.MidiDevice
import android.media.midi.MidiDeviceInfo
import android.media.midi.MidiInputPort
import android.media.midi.MidiManager
import android.util.Log

/**
 * The Android MIDI output port — the Java half of `songcore::IMidiOut`. Java because `MidiManager` is
 * the only route to USB, virtual and BLE MIDI (AMidi is API 29+ and still needs Java to open).
 * Everything musical is decided in C++.
 *
 * ⚠️ To SEND you open the device's INPUT port — port names are from the device's side. So the list
 * is `inputPortCount > 0`, and a keyboard correctly does not appear on it. Backwards, the port opens
 * and the synth stays silent.
 *
 * Every method runs on the SDL thread; [open] blocks it.
 */
class MidiOutManager(context: Context) {

    private val manager: MidiManager? =
        context.getSystemService(Context.MIDI_SERVICE) as? MidiManager

    // The snapshot the native indices refer to, refreshed only by [deviceCount] — the native side
    // always calls count → name → open in that order.
    private var devices: List<MidiDeviceInfo> = emptyList()

    private var device: MidiDevice? = null
    private var port: MidiInputPort? = null

    // One reusable buffer for [send]: no allocation per message.
    private val scratch = ByteArray(3)

    private var warnedNoManager = false

    /** How many devices we can SEND to. Re-enumerates: MIDI is hot-pluggable. */
    fun deviceCount(): Int {
        val m = manager
        if (m == null) {
            devices = emptyList()
            // Once: the list is rescanned every second.
            if (!warnedNoManager) {
                warnedNoManager = true
                Log.w(TAG, "no MidiManager on this device (no FEATURE_MIDI) - MIDI out unavailable")
            }
            return 0
        }
        // ⚠️ `inputPortCount`, see the class note. Sorted by id for a stable order in a session; the
        // setting stores the NAME, since a replug renumbers everything.
        @Suppress("DEPRECATION")   // getDevicesForTransport is API 33; this app's floor is 26
        devices = m.devices.filter { it.inputPortCount > 0 }.sortedBy { it.id }
        return devices.size
    }

    // What the OUTPUT row shows and settings.json stores (see [midiDeviceName]).
    fun deviceName(index: Int): String = devices.getOrNull(index)?.let(::midiDeviceName).orEmpty()

    // Opens device [index] for sending; true iff a port is live. ⚠️ Blocks up to 3 s ([openMidiDevice]).
    fun open(index: Int): Boolean {
        close()
        val m = manager ?: return false
        val info = devices.getOrNull(index) ?: return false

        val portNumber = info.ports
            .firstOrNull { it.type == MidiDeviceInfo.PortInfo.TYPE_INPUT }
            ?.portNumber
        if (portNumber == null) {
            Log.w(TAG, "device ${info.id} has no input port - cannot send to it")
            return false
        }

        val d = openMidiDevice(m, info) ?: return false

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

    // Releases the port and the device. ⚠️ No all-notes-off here: native sends it just before.
    fun close() {
        runCatching { port?.close() }.onFailure { Log.w(TAG, "port.close threw", it) }
        runCatching { device?.close() }.onFailure { Log.w(TAG, "device.close threw", it) }
        port = null
        device = null
    }

    /**
     * One MIDI message, 1–3 bytes. False = it did not go out.
     * ⚠️ `@Synchronized`: the sender thread and a panic on the frame loop both reach `scratch`.
     */
    @Synchronized
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
            // Counted natively. Not logged: a yanked cable would log a line per note.
            false
        }
    }

    /** A SysEx, whole: a USB packer may not join one sent three bytes at a time. */
    @Synchronized
    fun sendBytes(bytes: ByteArray, len: Int): Boolean {
        val p = port ?: return false
        if (len < 1 || len > bytes.size) return false
        return try {
            p.send(bytes, 0, len)
            true
        } catch (e: Exception) {
            false
        }
    }

    private companion object {
        const val TAG = "PocketTracker"
    }
}
