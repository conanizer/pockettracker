package com.conanizer.pockettracker

import android.Manifest
import android.content.Intent
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.util.Log
import android.view.InputDevice
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import androidx.annotation.Keep
import androidx.core.content.ContextCompat
import androidx.core.splashscreen.SplashScreen.Companion.installSplashScreen
import androidx.core.view.WindowCompat
import com.conanizer.pockettracker.input.PadClassifier
import com.conanizer.pockettracker.input.VirtualButton
import com.conanizer.pockettracker.platform.android.ButtonHapticManager
import com.conanizer.pockettracker.platform.android.ButtonSoundManager
import com.conanizer.pockettracker.platform.android.MidiInManager
import com.conanizer.pockettracker.platform.android.MidiOutManager
import org.json.JSONObject
import org.libsdl.app.SDLActivity
import java.io.File
import java.util.function.Consumer

/**
 * The Android entry point. The app is the shared C++ SDL shell; this class does only what needs Java:
 * the libraries and roots, the splash and immersive window, the folder picker and the [SafStorage]
 * hooks, the old-settings import, and button feedback, MIDI and background playback over JNI.
 *
 * ⚠️ No storage permission is asked: storage is a folder the user grants in the system picker.
 * The save on leaving is native (an event watcher in `shell/app.cpp`), not a lifecycle override here.
 */
class MainActivity : SDLActivity() {

    // Button clicks and vibration, reached by the shell through one JNI call (`onButtonFeedback`).
    private var buttonSound:  ButtonSoundManager?  = null
    private var buttonHaptic: ButtonHapticManager? = null

    // ⚠️ SDL loads `SDL_main` from the LAST library, so `pockettracker-sdl` stays last. The engine
    // (`libpockettracker.so`) is its NEEDED dependency and loads without being listed.
    override fun getLibraries(): Array<String> = arrayOf(
        "SDL2",
        "pockettracker-sdl"
    )

    // argv[1]: the media root — not the user's files, which are in the granted folder, over SAF.
    // argv[2]: `filesDir`, for settings.json, the template and the autosave: read at boot, before
    // any folder can be picked.
    override fun getArguments(): Array<String> = arrayOf(appRoot(), privateRoot())

    private fun appRoot(): String =
        File(
            Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS),
            "PocketTracker"
        ).absolutePath

    private fun privateRoot(): String = filesDir.absolutePath

    // Immersive sticky. ⚠️ Not cosmetic: with the status bar the window is 1280×904 and the integer
    // scale of 640×480 drops from 2× to 1×. API 30+ needs the DecorView attached first, hence `post`.
    private fun hideSystemBars() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.decorView.post {
                window.insetsController?.apply {
                    hide(WindowInsets.Type.systemBars())
                    systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                }
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                or View.SYSTEM_UI_FLAG_FULLSCREEN
                or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
            )
        }
    }

    // A swipe-down or the permission screen leaves the bars up; hide them again on focus.
    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        // ⚠️ First: it swaps the activity's theme. The manifest theme draws the splash before any
        // code runs; this call hands over from it (and back-ports it below API 31).
        installSplashScreen()

        // Logged always: a fresh install with no folder looks like a permission failure from outside.
        // ⚠️ The count, not `homeRootId()`, which writes state.
        Log.i(TAG, "storage: SAF, ${safStorage.rootCount()} granted folder(s), " +
                   "privateRoot=${privateRoot()}")

        // ⚠️ Edge-to-edge, or Android keeps padding for the hidden bars (1× again). Before
        // `super.onCreate()`, where SDL builds its surface at the size it sees then.
        WindowCompat.setDecorFitsSystemWindows(window, false)

        // Draw behind a notch too, or landscape loses height — and 2× becomes 1× again.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            window.attributes.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
        }

        // ⚠️ All before `super.onCreate()`: it starts the SDL thread, which reads the settings,
        // reopens the saved MIDI devices and calls back for button feedback at once.
        importLegacySettings()

        buttonSound  = ButtonSoundManager(this)
        buttonHaptic = ButtonHapticManager(this)

        midiOut = MidiOutManager(this)
        midiIn = MidiInManager(this)

        super.onCreate(savedInstanceState)
        hideSystemBars()
        askForNotificationsOnce()
    }

    // Android 13+ hides the playback notification (and its STOP) without this. Asked once.
    private fun askForNotificationsOnce() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return
        if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED) return
        val prefs = getSharedPreferences(SHELL_PREFS, MODE_PRIVATE)
        if (prefs.getBoolean(NOTIFICATIONS_ASKED_KEY, false)) return
        prefs.edit().putBoolean(NOTIFICATIONS_ASKED_KEY, true).apply()
        requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), REQ_NOTIFICATIONS)
    }

    // ── A song left playing keeps playing ────────────────────────────────────────────────────────
    //
    // ⚠️ The service starts here, before `super.onPause()`: Android refuses one started from the
    // background, and the native loop asks at `super.onPause()` whether it came up. If not, the
    // song stops.
    override fun onPause() {
        var started = false
        try {
            if (nativeIsPlaying()) {
                ContextCompat.startForegroundService(this, Intent(this, PlaybackService::class.java))
                started = true
            }
        } catch (e: Exception) {
            Log.w(TAG, "playback service refused: $e")
        } catch (e: UnsatisfiedLinkError) {
            Log.w(TAG, "native library not loaded - no background playback")
        }
        try { nativeSetServiceStarted(started) } catch (_: UnsatisfiedLinkError) {}
        unwatchScreenRecording()
        super.onPause()
    }

    override fun onResume() {
        // Before the native loop is told it is back, so the sound reopens on the right path at once.
        watchScreenRecording()
        super.onResume()
        stopPlaybackService()
    }

    // Harmless when not running. Also called from native when a song stops in the background, so
    // `@Keep` and a `-keep` in `proguard-rules.pro`, like every JNI method resolved by name.
    @Keep
    fun stopPlaybackService() {
        try { nativeSetServiceStarted(false) } catch (_: UnsatisfiedLinkError) {}
        stopService(Intent(this, PlaybackService::class.java))
    }

    private external fun nativeIsPlaying(): Boolean
    private external fun nativeSetServiceStarted(started: Boolean)

    // ── A screen recording hears the app ─────────────────────────────────────────────────────────
    //
    // The fast audio path does not reach a recording, so native leaves it while one shows the app.
    // ⚠️ Android 15+ only: earlier versions tell the app nothing.
    // ⚠️ Watched only while resumed: leaving the screen also reads as "not recorded", and a song
    // playing on in the background must not have its sound reopened for that.
    private val recordingCallback = Consumer<Int> { state -> onScreenRecording(state) }
    private val recordingEnded = Runnable { setPlaybackCaptured(false) }

    private fun watchScreenRecording() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.VANILLA_ICE_CREAM) return
        try {
            onScreenRecording(windowManager.addScreenRecordingCallback(mainExecutor, recordingCallback))
        } catch (e: Exception) {
            Log.w(TAG, "screen recording state unavailable: $e")
        }
    }

    private fun unwatchScreenRecording() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.VANILLA_ICE_CREAM) return
        try { windowManager.removeScreenRecordingCallback(recordingCallback) } catch (_: Exception) {}
        window.decorView.removeCallbacks(recordingEnded)
    }

    private fun onScreenRecording(state: Int) {
        window.decorView.removeCallbacks(recordingEnded)
        if (state == WindowManager.SCREEN_RECORDING_STATE_VISIBLE) {
            setPlaybackCaptured(true)
        } else {
            // ⚠️ Late on purpose: a stream reopened while the recorder still holds the sound is
            // refused the fast path, and would stay slow.
            window.decorView.postDelayed(recordingEnded, RECORDING_END_SETTLE_MS)
        }
    }

    private fun setPlaybackCaptured(captured: Boolean) {
        try { nativeSetPlaybackCaptured(captured) } catch (_: UnsatisfiedLinkError) {}
    }

    private external fun nativeSetPlaybackCaptured(captured: Boolean)

    override fun onDestroy() {
        buttonSound?.release()
        buttonSound  = null
        buttonHaptic = null
        // ⚠️ A backstop: the shell closes the port itself, but a kill or crash it does not reach
        // would leave the last note hanging on the hardware. `close()` is idempotent.
        midiOut?.close()
        midiOut      = null
        // ⚠️ Same for input: an open port keeps the device from every other app.
        midiIn?.close()
        midiIn       = null
        super.onDestroy()
    }

    /**
     * Every on-screen button press and release, from native on the SDL thread, with the live
     * BTN SOUND / BTN VIBRO settings. [button] is in `VirtualButton`'s order (= `pt::ui::Button`'s).
     * The sound plays from here; the haptic is posted to the UI thread.
     * ⚠️ Resolved by name over JNI: `@Keep` plus a `-keep`, or release cannot find it.
     */
    @Keep
    fun onButtonFeedback(
        button: Int, down: Boolean,
        soundOn: Boolean, soundVolume: Int,
        vibroOn: Boolean, vibroPower: Int
    ) {
        // `entries`, not `values()`: this runs on every press, and `values()` copies the array.
        val vb = VirtualButton.entries.getOrNull(button) ?: return

        buttonSound?.let { s ->
            s.enabled = soundOn
            s.volume  = (soundVolume.coerceIn(0, 255)) / 255f
            if (down) s.onPress(vb) else s.onRelease(vb)
        }

        buttonHaptic?.let { h ->
            h.enabled = vibroOn
            h.power   = vibroPower.coerceIn(1, 255)
            if (h.enabled) {
                val view = window.decorView
                runOnUiThread { if (down) h.onPress(view) else h.onRelease(view) }
            }
        }
    }

    /**
     * Called from native to choose the touch or FULL layout: true iff a real game controller is
     * attached. ⚠️ SDL counts the emulator's keyboard as a controller; the tests that tell a pad
     * from an impostor live in [PadClassifier]. JNI by name: `@Keep` + `-keep`.
     */
    @Keep
    fun hasPhysicalGameButtons(): Boolean {
        for (deviceId in InputDevice.getDeviceIds()) {
            val device  = InputDevice.getDevice(deviceId) ?: continue
            val verdict = PadClassifier.classify(device)
            if (verdict.isPad) {
                Log.i(TAG, "physical game buttons: '${device.name}' (${verdict.reason})")
                return true
            }
        }
        return false
    }

    /**
     * May the screen turn to landscape now? True only while the FULL layout is in force; the shell's
     * layout gate calls this whenever the answer changes.
     * ⚠️ A launch-time `SDL_HINT_ORIENTATIONS` cannot take the permission back; [requestedOrientation]
     * re-orients the running activity. Arrives on the SDL thread, runs on the UI thread. JNI by name.
     */
    @Keep
    fun setLandscapeAllowed(allowed: Boolean) {
        runOnUiThread {
            val view = window.decorView
            view.removeCallbacks(portraitCheck)
            requestedOrientation =
                if (allowed) ActivityInfo.SCREEN_ORIENTATION_FULL_USER
                else         ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT
            if (!allowed) view.postDelayed(portraitCheck, ORIENTATION_SETTLE_MS)
        }
    }

    /**
     * ⚠️ A device that ignores the portrait request would leave the app unable to wake: at the next
     * resume SDL refuses the landscape surface and the screen stays black. So if it is still
     * landscape once a rotation would have finished, rotation is handed back.
     * `Log.w`: release strips `v`/`d`/`i`.
     */
    private val portraitCheck = Runnable {
        if (requestedOrientation == ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT &&
            resources.configuration.orientation != Configuration.ORIENTATION_PORTRAIT) {
            requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_FULL_USER
            Log.w(TAG, "portrait request refused by this device - rotation handed back, " +
                       "or SDL would refuse the next surface and the app could not wake")
        }
    }

    /**
     * Every input device and the verdict on it, plus the device identity — for an "it opened without
     * the on-screen buttons" report ([hasPhysicalGameButtons] logs only a find). Returned as text so
     * native writes it to `pockettracker-log.txt`; `Log` reaches logcat only. JNI by name.
     */
    @Keep
    fun describeInputDevices(): String {
        val sb = StringBuilder()
        sb.append("device:  ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
            .append("  (Android ").append(Build.VERSION.RELEASE)
            .append(", API ").append(Build.VERSION.SDK_INT).append(")\n")

        val ids = InputDevice.getDeviceIds()
        sb.append("input:   ").append(ids.size).append(" device(s) enumerated\n")
        for (deviceId in ids) {
            val d = InputDevice.getDevice(deviceId)
            if (d == null) {
                sb.append("input:     id=").append(deviceId).append("  <null>\n")
                continue
            }
            val s = d.sources
            sb.append("input:     id=").append(deviceId)
                .append("  sources=0x").append(Integer.toHexString(s))
                .append("  gamepad=").append((s and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD)
                .append(" joystick=").append((s and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK)
                .append(" keyboard=").append((s and InputDevice.SOURCE_KEYBOARD) == InputDevice.SOURCE_KEYBOARD)
                .append(" virtual=").append(d.isVirtual)
                .append("  '").append(d.name).append("'\n")
            sb.append("input:            -> ").append(PadClassifier.classify(d).reason).append('\n')
        }
        sb.append("input:   hasPhysicalGameButtons() = ").append(hasPhysicalGameButtons())
        return sb.toString()
    }

    // ── EXTERNAL MIDI out ────────────────────────────────────────────────────────────────────────
    //
    // By-name JNI hooks forwarding to [MidiOutManager]. ⚠️ Each `@Keep` AND in `proguard-rules.pro`:
    // a renamed one kills MIDI in release only.

    private var midiOut: MidiOutManager? = null

    /** How many devices this phone can send MIDI to right now. Re-enumerates — MIDI is hot-pluggable. */
    @Keep
    fun midiDeviceCount(): Int = midiOut?.deviceCount() ?: 0

    /** Display name of device [index], from the snapshot [midiDeviceCount] just took. */
    @Keep
    fun midiDeviceName(index: Int): String = midiOut?.deviceName(index).orEmpty()

    /** Open device [index] for sending. ⚠️ Blocks the SDL thread up to ~3 s (`MidiOutManager.open`). */
    @Keep
    fun midiOpenDevice(index: Int): Boolean = midiOut?.open(index) ?: false

    /** ⚠️ The native side sends all-notes-off on all 16 channels immediately BEFORE calling this. */
    @Keep
    fun midiCloseDevice() {
        midiOut?.close()
    }

    /** One serialized MIDI message, 1–3 bytes. False = it did not go out (counted natively). */
    @Keep
    fun midiSend(b0: Int, b1: Int, b2: Int, len: Int): Boolean =
        midiOut?.send(b0, b1, b2, len) ?: false

    // ── MIDI IN ──────────────────────────────────────────────────────────────────────────────────
    //
    // The same hooks for input: the list is the devices with an OUTPUT port (to receive, you open
    // that), and native polls [midiInRead] once a frame. ⚠️ `@Keep` AND `proguard-rules.pro`.

    private var midiIn: MidiInManager? = null

    /** How many devices can send MIDI to this phone right now. Re-enumerates — MIDI is hot-pluggable. */
    @Keep
    fun midiInDeviceCount(): Int = midiIn?.deviceCount() ?: 0

    /** Display name of input device [index], from the snapshot [midiInDeviceCount] just took. */
    @Keep
    fun midiInDeviceName(index: Int): String = midiIn?.deviceName(index).orEmpty()

    /** Open input device [index]. ⚠️ Blocks the SDL thread up to ~3 s, as [midiOpenDevice] does. */
    @Keep
    fun midiInOpenDevice(index: Int): Boolean = midiIn?.open(index) ?: false

    /** Disconnect the receiver and release the port. Idempotent; the native side re-picks freely. */
    @Keep
    fun midiInCloseDevice() {
        midiIn?.close()
    }

    // ── Storage Access Framework ─────────────────────────────────────────────────────────────────
    //
    // `saf-filesystem.cpp` reaches `ContentResolver` / `DocumentsContract` through these delegates
    // to [SafStorage]. ⚠️ `@Keep` AND a `proguard-rules.pro` rule each.

    private val safStorage: SafStorage by lazy { SafStorage(this) }

    /** How many folders the user has granted. Zero is the fresh-install state, not an error. */
    @Keep
    fun safRootCount(): Int = safStorage.rootCount()

    /** `<id>\t<displayName>\t<docUri>\t<live 0|1>` for granted tree [index], or "" if it is gone. */
    @Keep
    fun safRootInfo(index: Int): String = safStorage.rootInfo(index)

    /** The id of the tree the app's seven folders live in; "" when nothing usable is granted. */
    @Keep
    fun safHomeRootId(): String = safStorage.homeRootId()

    /** Make granted tree [id] the home root — the user picking it in the browser. False = refused. */
    @Keep
    fun safSetHomeRoot(id: String): Boolean = safStorage.setHomeRoot(id)

    /** Release the grant on tree [id] — FORGET FOLDER. Deletes nothing; drops a permission. */
    @Keep
    fun safForgetRoot(id: String): Boolean = safStorage.forgetRoot(id)

    /**
     * ADD FOLDER…: opens the system folder picker. True = it is on screen, not that a folder was
     * granted; the grant arrives in [onActivityResult].
     * ⚠️ Waits for the launch only: waiting for the answer would park the SDL thread, which runs the
     * save on leaving. Bounded at 2 s, since SDLActivity can block the UI thread on the SDL thread.
     */
    @Keep
    fun safRequestRoot(): Boolean {
        val launched = java.util.concurrent.atomic.AtomicBoolean(false)
        val done     = java.util.concurrent.CountDownLatch(1)
        runOnUiThread {
            try {
                launched.set(safStorage.requestRoot(this, REQ_ADD_ROOT))
            } finally {
                // ⚠️ In `finally`, or a throw parks the SDL thread for the full timeout.
                done.countDown()
            }
        }
        if (!done.await(2, java.util.concurrent.TimeUnit.SECONDS))
            Log.w(TAG, "saf: folder picker did not launch within 2s - UI thread busy?")
        return launched.get()
    }

    /**
     * The picker's answer. ⚠️ `super` first: SDLActivity has results of its own.
     * Nothing is pushed to native: the browser re-lists the granted folders when the app returns.
     */
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQ_ADD_ROOT) return
        if (resultCode != RESULT_OK || data?.data == null) {
            // Cancelling is a normal answer: the user is back on the roots directory's ADD FOLDER… row.
            Log.i(TAG, "saf: folder picker cancelled (result=$resultCode)")
            return
        }
        safStorage.takeGrant(data.data)
    }

    /** Every child of a directory document in one query — see [SafStorage.listChildren] for the format. */
    @Keep
    fun safListChildren(dirDocUri: String): String = safStorage.listChildren(dirDocUri)

    /** Whole document → bytes, or null. */
    @Keep
    fun safReadFile(docUri: String): ByteArray? = safStorage.readFile(docUri)

    /** An OS descriptor the caller OWNS (`detachFd`), or -1. This is `pt_fopen`'s hook. */
    @Keep
    fun safOpenFd(docUri: String, mode: String): Int = safStorage.openFd(docUri, mode)

    /** Create (or find) a sub-directory document, returning its URI or "". */
    @Keep
    fun safCreateDir(parentDocUri: String, name: String): String =
        safStorage.createDir(parentDocUri, name)

    /** Create (or find) a FILE document, returning its URI or "". The write half's create. */
    @Keep
    fun safCreateFile(parentDocUri: String, name: String): String =
        safStorage.createFile(parentDocUri, name)

    /** Delete a document; a directory goes with its whole subtree. */
    @Keep
    fun safDelete(docUri: String): Boolean = safStorage.deleteDoc(docUri)

    /** Rename a document, returning the URI it has AFTERWARDS (it may differ), or "". */
    @Keep
    fun safRename(docUri: String, newName: String): String = safStorage.renameDoc(docUri, newName)

    /** Move between directory documents, returning the new URI — or "" meaning "copy+delete instead". */
    @Keep
    fun safMove(docUri: String, fromParentDocUri: String, toParentDocUri: String): String =
        safStorage.moveDoc(docUri, fromParentDocUri, toParentDocUri)

    /**
     * Moves what arrived since the last frame into [out]; returns the byte count. [out] is allocated
     * once natively and reused; what does not fit stays in `MidiInManager`'s ring.
     */
    @Keep
    fun midiInRead(out: ByteArray): Int = midiIn?.read(out) ?: 0

    /**
     * One-time import of the old app's SharedPreferences into settings.json — Android's format, so
     * read here rather than parsed in C++.
     * ⚠️ Versioned, not keyed off "settings.json is absent", so a later version can still run.
     * ⚠️ The defaults below are the OLD app's, not the C++ ones: what survives is what the user had
     * (button sound on, volume 0x80). `app_theme` passes through as is — the C++ writes the same bytes.
     * Debug and release do not share preferences (the `.debug` suffix).
     */
    private fun importLegacySettings() {
        val prefs = getSharedPreferences("pockettracker_ui", MODE_PRIVATE)
        val done  = prefs.getInt(IMPORT_VERSION_KEY, 0)
        if (done >= SETTINGS_IMPORT_VERSION) {
            Log.i(TAG, "settings import: already at v$done, nothing to do")
            return
        }

        // ⚠️ The file `SafFileSystem::settings_path` reads, or the import goes nowhere.
        val target = File(filesDir, "settings.json")

        // An existing settings.json wins — its values were chosen in this app. Still stamped.
        if (target.exists()) {
            prefs.edit().putInt(IMPORT_VERSION_KEY, SETTINGS_IMPORT_VERSION).apply()
            Log.i(TAG, "settings import: ${target.name} already exists - keeping it, marked v$SETTINGS_IMPORT_VERSION")
            return
        }

        // Nothing to migrate is a fresh install: the C++ defaults are right. Stamped.
        if (prefs.all.isEmpty()) {
            prefs.edit().putInt(IMPORT_VERSION_KEY, SETTINGS_IMPORT_VERSION).apply()
            Log.i(TAG, "settings import: no prefs to migrate (fresh install), marked v$SETTINGS_IMPORT_VERSION")
            return
        }

        try {
            val json = JSONObject()

            // ── The rows every platform has ──────────────────────────────────────────────────────
            json.put("scalingBilinear",
                     prefs.getString("scaling_mode", null) == "BILINEAR")
            json.put("insertBefore",       prefs.getBoolean("kb_insert_before", true))
            json.put("cursorRemember",     prefs.getBoolean("cursor_remember", false))
            json.put("notePreview",        prefs.getBoolean("note_preview", true))
            json.put("autosaveResumeAuto", prefs.getBoolean("autosave_resume_auto", false))

            // `trace` and `engine_cpp_v2` are not imported: developer switches, never the user's.

            // ── The device rows that are plain scalars ───────────────────────────────────────────
            json.put("buttonSound",       prefs.getBoolean("button_sound", true))
            json.put("buttonSoundVolume", prefs.getInt("button_sound_volume", 0x80))
            json.put("buttonVibro",       prefs.getBoolean("button_vibro", true))
            json.put("vibroPower",        prefs.getInt("vibro_power", 255))
            json.put("overlayStrength",   prefs.getInt("overlay_strength", 128))

            // ── SKIN and OVERLAY, by id (v2) ─────────────────────────────────────────────────────
            // LAYOUT is not imported: the shell picks it by orientation and controller.
            json.put("portrait_skin", prefs.getString("portrait_skin", DEFAULT_SKIN_ID))
            json.put("overlay_name",  prefs.getString("overlay_name", "OFF"))

            // ── The theme ────────────────────────────────────────────────────────────────────────
            // Both `appTheme` and `theme` (the name, for an older build), as `serialize_settings` writes.
            val storedTheme = prefs.getString("app_theme", null)
            if (storedTheme != null) {
                val parsed = JSONObject(storedTheme)
                json.put("appTheme", parsed)
                json.put("theme", parsed.optString("name", "CLASSIC"))
                // The visualizer is a top-level int in settings.json, not a theme field.
                json.put("visualizer", visualizerIndex(parsed.optString("visualizerType", "SCOPE")))
            }

            target.parentFile?.mkdirs()
            target.writeText(json.toString(2) + "\n")
            prefs.edit().putInt(IMPORT_VERSION_KEY, SETTINGS_IMPORT_VERSION).apply()
            Log.i(TAG, "settings import: wrote ${target.absolutePath} " +
                       "(${json.length()} keys, theme=${json.optString("theme", "-")}), marked v$SETTINGS_IMPORT_VERSION")
        } catch (e: Exception) {
            // ⚠️ Not stamped, so the next launch retries; not fatal — a crash would cost the app.
            Log.e(TAG, "settings import FAILED - settings will fall back to defaults: ${e.message}", e)
        }
    }

    /** `VisualizerType`'s ordinal, the order in `theme.h`. */
    private fun visualizerIndex(name: String): Int =
        listOf("SCOPE", "FLAT", "OCTA", "OCTA_FULL", "SPECTRUM", "SPECTRUM_PEAKS").indexOf(name).coerceAtLeast(0)

    private companion object {
        const val TAG = "PocketTrackerSDL"

        /** How long an accepted rotation takes, before [portraitCheck] concludes it was refused. */
        const val ORIENTATION_SETTLE_MS = 2000L

        const val RECORDING_END_SETTLE_MS = 1000L

        /** Bump with new keys to migrate. v1: the plain rows and theme; v2: SKIN and OVERLAY. */
        const val SETTINGS_IMPORT_VERSION = 2
        const val IMPORT_VERSION_KEY = "settings_import_version"

        /** The old app's default skin — also the shell's fallback for an unknown id. */
        const val DEFAULT_SKIN_ID = "amiga-2"

        /** [safRequestRoot]'s `startActivityForResult` code, matched in [onActivityResult]. */
        const val REQ_ADD_ROOT = 1001

        /** [askForNotificationsOnce] — the request code, and where "already asked" is remembered. */
        const val REQ_NOTIFICATIONS = 1002
        const val SHELL_PREFS = "pockettracker_shell"
        const val NOTIFICATIONS_ASKED_KEY = "notifications_asked"
    }
}
