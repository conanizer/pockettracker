package com.conanizer.pockettracker.platform.android

import android.content.Context
import android.media.midi.MidiDeviceInfo
import android.media.midi.MidiInputPort

/**
 * The Android MIDI output port — the Java half of `songcore::IMidiOut`. Java because `MidiManager` is
 * the only route to USB, virtual and BLE MIDI (AMidi is API 29+ and still needs Java to open).
 * Everything musical is decided in C++.
 *
 * ⚠️ To SEND you open the device's INPUT port ([MidiPorts]), so a keyboard correctly does not appear
 * on the list. Every method runs on the SDL thread but [send], which the MIDI sender thread calls too.
 */
class MidiOutManager(context: Context) {

    private val ports = MidiPorts<MidiInputPort>(context, "out", MidiDeviceInfo.PortInfo.TYPE_INPUT) { d, n ->
        d.openInputPort(n)
    }

    // One reusable buffer for [send]: no allocation per message.
    private val scratch = ByteArray(3)

    /** How many devices we can SEND to. */
    fun deviceCount(): Int = ports.deviceCount()

    // What the OUTPUT row shows and settings.json stores.
    fun deviceName(index: Int): String = ports.deviceName(index)

    // Opens device [index] for sending; true iff a port is live. ⚠️ Blocks up to 3 s.
    fun open(index: Int): Boolean = ports.open(index) != null

    // Releases the port and the device. ⚠️ No all-notes-off here: native sends it just before.
    fun close() = ports.close()

    /**
     * One MIDI message, 1–3 bytes. False = it did not go out.
     * ⚠️ `@Synchronized`: the sender thread and a panic on the frame loop both reach `scratch`.
     */
    @Synchronized
    fun send(b0: Int, b1: Int, b2: Int, len: Int): Boolean {
        val p = ports.port ?: return false
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
        val p = ports.port ?: return false
        if (len < 1 || len > bytes.size) return false
        return try {
            p.send(bytes, 0, len)
            true
        } catch (e: Exception) {
            false
        }
    }
}
