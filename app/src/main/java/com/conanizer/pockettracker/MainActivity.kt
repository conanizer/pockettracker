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

/**
 * PocketTracker's Android entry point: an `SDLActivity` subclass. The app is the shared C++ SDL shell
 * (`shell/`, `native/ui/`); this class does only what is genuinely Java's: point SDL at the native
 * libraries and the two roots, own the splash screen and the immersive / edge-to-edge window, serve
 * the Storage Access Framework (the folder picker and the [SafStorage] delegates), run the one-shot
 * SharedPreferences → settings.json import, and route button feedback, MIDI and background playback
 * over JNI hooks.
 *
 * ⚠️ It asks for NO storage permission: storage is a folder the user grants from the system picker.
 * The only runtime ask is notifications, for the playback service's STOP.
 *
 * ⚠️ The system bars are Java's; the lifecycle is NOT — the autosave/settings flush is an
 * `SDL_AddEventWatch` watcher in `shell/app.cpp`, because `SDL_APP_WILLENTERBACKGROUND` fires on the
 * native thread inside the frame loop's own `SDL_PollEvent`. The back button is armed in
 * `shell/android-main.cpp` and mapped in `shell/sdl-input.cpp`.
 */
class MainActivity : SDLActivity() {

    // ── Button feedback ──────────────────────────────────────────────────────────────────────────
    //
    // SoundPool clicks and Vibrator pulses are Android system services, reached by the shell through
    // ONE JNI call (`onButtonFeedback`). Created before `super.onCreate()` starts the SDL thread, so
    // the samples are loading before the first tap.
    private var buttonSound:  ButtonSoundManager?  = null
    private var buttonHaptic: ButtonHapticManager? = null

    /**
     * ⚠️ ORDER MATTERS AND THE LAST ONE IS SPECIAL. `SDLActivity.getMainSharedObject()` takes the
     * LAST entry and `dlsym`s `SDL_main` out of `lib<that>.so` — so `pockettracker-sdl` must be
     * last, and it is the library `shell/android-main.cpp` compiles into.
     *
     * `libpockettracker.so` (the engine) is deliberately absent: it is a NEEDED dependency of
     * `libpockettracker-sdl.so`, so the dynamic linker loads it from the same directory without
     * being told. Listing it here as well would load it twice by two different mechanisms for no
     * benefit.
     */
    override fun getLibraries(): Array<String> = arrayOf(
        "SDL2",
        "pockettracker-sdl"
    )

    /**
     * The two roots the shell is booted with.
     *
     * argv[1] — the media root. ⚠️ NOT where the user's files are: those live in the granted folder,
     * over SAF, and this process cannot read `Documents/PocketTracker`. Resolved here because
     * `ui::default_app_root()` finds nothing on Android and would fall through to a relative path.
     *
     * argv[2] — `filesDir`: settings.json, template.ptp and autosave.ptp are read during the native
     * boot, before any picker can run, so they live in app-private storage. `config.json`
     * (hand-edited) sits in the granted tree instead (`SafFileSystem::config_path`).
     */
    override fun getArguments(): Array<String> = arrayOf(appRoot(), privateRoot())

    private fun appRoot(): String =
        File(
            Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS),
            "PocketTracker"
        ).absolutePath

    private fun privateRoot(): String = filesDir.absolutePath

    /**
     * Hide the status and navigation bars (immersive sticky) — NOT cosmetic.
     *
     * ⚠️⚠️ THE STATUS BAR COSTS A WHOLE SCALING FACTOR: visible, the window is 1280×904, and INTEGER
     * scaling of the 640×480 design computes `min(2, 1)` = 1× — a quarter of the area. Hidden, 2× is
     * pixel-exact. `dest_rect()` re-reads the output size every frame, so no resize handler is needed.
     *
     * ⚠️ `decorView.post`: API 30+ needs the DecorView ATTACHED before `insetsController` is non-null.
     */
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

    /** Re-apply immersive mode whenever the window regains focus — a swipe-down or the permission
     *  screen returning otherwise leaves the bars up, and with them the 1× window. */
    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        // ── THE SPLASH SCREEN ────────────────────────────────────────────────────────────────────
        //
        // ⚠️ BOTH HALVES ARE REQUIRED: the manifest theme supplies the windowBackground the system
        // draws before any code runs (API 31+), and this call hands over to `postSplashScreenTheme`
        // and back-ports the splash below API 31. FIRST, ahead of `setDecorFitsSystemWindows`,
        // because it swaps the activity's theme. `splash_bg` #0A0A0A equals the tracker's default
        // background, so the handover has no seam.
        installSplashScreen()

        // ⚠️ NOTHING IS ASKED FOR, AND THE LINE SAYING SO IS UNCONDITIONAL: a browser on ADD FOLDER…
        // alone is the correct fresh install, and from outside it looks like a permission failure;
        // the count tells them apart. ⚠️ The COUNT, not `homeRootId()`, which stamps state.
        Log.i(TAG, "storage: SAF, ${safStorage.rootCount()} granted folder(s), " +
                   "privateRoot=${privateRoot()}")

        // ⚠️⚠️ EDGE-TO-EDGE: `hideSystemBars()` alone leaves the window at 1280x904 — Android keeps
        // reserving inset padding for a hidden bar unless the content may draw there (still 1×).
        // ⚠️ BEFORE `super.onCreate()`, where SDLActivity builds its surface: afterwards the surface
        // is created at the inset size and resized, and every consumer sees the wrong number first.
        WindowCompat.setDecorFitsSystemWindows(window, false)

        // Draw behind a punch-hole/notch too: in landscape the cutout is on a short edge, and
        // without this the panel gives back less height — the same 2x-becomes-1x arithmetic.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            window.attributes.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
        }

        // ⚠️ BEFORE `super.onCreate()` for a harder reason than the two above: that call starts the
        // SDL thread, which runs `SDL_main`, which calls `load_settings()`. A settings file written
        // after that point is one the app has already read past.
        importLegacySettings()

        // The feedback managers, before super.onCreate() starts the SDL thread that calls back into
        // onButtonFeedback. Constructing the SoundPool early lets it load the click samples off the
        // critical path, so the first tap is not silent while they decode.
        buttonSound  = ButtonSoundManager(this)
        buttonHaptic = ButtonHapticManager(this)

        // The MIDI ports, before `super.onCreate()` too: the native boot re-opens the saved devices
        // (`boot_midi_port()`). Only the system service is taken here.
        midiOut = MidiOutManager(this)
        // …and the INPUT port (`boot_midi_in_port()`).
        midiIn = MidiInManager(this)

        super.onCreate(savedInstanceState)
        hideSystemBars()
        askForNotificationsOnce()
    }

    /**
     * Android 13+: the playback notification — and its STOP — is hidden without this permission, while
     * the song itself still plays on. Asked once, on the first launch that can ask; Android stops
     * offering the dialog after two refusals anyway.
     */
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
    // ⚠️ **STARTED HERE, BEFORE `super.onPause()`, AND THE ORDER IS THE FEATURE.** Android refuses a
    // foreground service started from the background, and this is the last moment the activity still
    // counts as in front. `super.onPause()` is what tells the native loop it is leaving, and the loop
    // asks then whether the service came up — so the answer has to be stored first. If the start is
    // refused, the loop stops the song and hands the sound back, as it always did.
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
        super.onPause()
    }

    override fun onResume() {
        super.onResume()
        stopPlaybackService()
    }

    /**
     * Takes the playback service down; harmless when it is not running. Also **called from native**
     * (`shell/android-main.cpp`) once a song stops in the background — so `@Keep` and a
     * `proguard-rules.pro` `-keep`, like every method resolved by name over JNI.
     */
    @Keep
    fun stopPlaybackService() {
        try { nativeSetServiceStarted(false) } catch (_: UnsatisfiedLinkError) {}
        stopService(Intent(this, PlaybackService::class.java))
    }

    private external fun nativeIsPlaying(): Boolean
    private external fun nativeSetServiceStarted(started: Boolean)

    override fun onDestroy() {
        buttonSound?.release()
        buttonSound  = null
        buttonHaptic = null
        // ⚠️ The BACKSTOP, not the normal path. The shell's teardown panics and closes the port
        // through `midiCloseDevice()` while the SDL thread is still alive; this catches the death it
        // does not reach — a kill, a config change, a crash — where an open port would otherwise hold
        // the last note on the hardware until the user power-cycles it. `close()` is idempotent.
        midiOut?.close()
        midiOut      = null
        // ⚠️ The same backstop for the input port: an open MidiOutputPort holds the DEVICE, so no
        // other app could use the keyboard until this process dies.
        midiIn?.close()
        midiIn       = null
        super.onDestroy()
    }

    /**
     * Called from native (`shell/android-main.cpp`, on the SDL thread) on every virtual-button press
     * and release. The touch layer decides to fire and passes the live BTN SOUND / BTN VIBRO scalars;
     * this routes them to the two managers. ⚠️ Resolved by name over JNI: `@Keep` plus a `-keep` in
     * `proguard-rules.pro`, or release gets an `UnsatisfiedLinkError`.
     *
     * @param button ordinal of the virtual button — `VirtualButton`'s order, which is
     *               `pt::ui::Button`'s (native/ui/buttons.h).
     * @param down   true = press feel, false = release (a lift or a slide-off).
     *
     * ⚠️ The haptic is posted to the UI thread (the bottom fallback is `View.performHapticFeedback`);
     * the sound plays straight from here, lowest latency.
     */
    @Keep
    fun onButtonFeedback(
        button: Int, down: Boolean,
        soundOn: Boolean, soundVolume: Int,
        vibroOn: Boolean, vibroPower: Int
    ) {
        // `entries`, not `values()`: this runs on every press AND every release, and values() hands
        // back a fresh defensive copy of the array each call.
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
     * **Called from native (`shell/android-main.cpp`) to decide the touch vs FULL layout.** True iff a
     * real game controller is attached; the shared shell draws the on-screen gamepad + PORTRAIT2 skin
     * only when this is false and the hardware is a touchscreen.
     *
     * ⚠️ SDL counts the emulator's keyboard as a game controller (a bare DPAD source), so
     * `SdlInput::controller_count()` reads 1 with no pad; the tests that tell a pad from an impostor
     * are Java-only and live in [PadClassifier]. Called by name over JNI: `@Keep` plus a `-keep`.
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
     * May the screen rotate into LANDSCAPE right now? Called by the shell's layout gate whenever the
     * answer changes: true only while the FULL layout is in force — a controller is driving and there
     * are no on-screen buttons.
     *
     * ⚠️ A LAUNCH-TIME `SDL_HINT_ORIENTATIONS` CANNOT TAKE THE PERMISSION BACK: a phone launched with
     * a controller would stay free to sit in landscape after it is switched off, drawing a layout no
     * SETTINGS row names. [requestedOrientation] makes Android re-orient the running activity.
     * `SENSOR_PORTRAIT` keeps both ways up; `FULL_USER` hands rotation back as at launch.
     *
     * ⚠️ On the UI thread (this arrives on the SDL thread). Called by name over JNI: `@Keep` + `-keep`.
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
     * ⚠️⚠️ A DEVICE THAT IGNORES THE PORTRAIT REQUEST LEAVES THE APP UNABLE TO WAKE UP, so the request
     * is checked and handed back when it did not take. A landscape-only panel (or a ROM ignoring
     * orientation requests) stays landscape; at the NEXT resume `SDLSurface.surfaceChanged` refuses
     * the mismatched surface, `nativeResume` is never called, and the screen stays black until
     * relaunch. So if the configuration is still landscape once a rotation would have finished,
     * rotation goes back to `FULL_USER`. The on-screen buttons are the layout gate's, unaffected.
     *
     * ⚠️ `Log.w`, not `Log.i`: release strips `v`/`d`/`i` (proguard-rules.pro).
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
     * Everything [hasPhysicalGameButtons] looked at, as text, plus the device identity — the payload
     * of a "it opened without the on-screen buttons" report.
     *
     * ⚠️ UNCONDITIONAL: [hasPhysicalGameButtons] logs only when it FINDS a pad, so the state needing
     * explanation leaves no record otherwise. Every device is listed with its raw `sources` and the
     * reason [PadClassifier] gave — the same call as the verdict, so they cannot drift.
     *
     * ⚠️ Returned as a STRING so native `printf`s it into `pockettracker-log.txt` (Kotlin's `Log.i`
     * reaches logcat only). Called by name over JNI: `@Keep` + `-keep`.
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
    // Five by-name JNI hooks forwarding to `MidiOutManager` (which explains why MidiManager is
    // unavoidable, and that to SEND you open the device's INPUT port).
    // ⚠️ All `@Keep` AND listed in `proguard-rules.pro`: a renamed member kills MIDI in release only.
    // `shell/midi-out-android.cpp` logs at resolve time whether it found them.

    private var midiOut: MidiOutManager? = null

    /** How many devices this phone can send MIDI to right now. Re-enumerates — MIDI is hot-pluggable. */
    @Keep
    fun midiDeviceCount(): Int = midiOut?.deviceCount() ?: 0

    /** Display name of device [index], from the snapshot [midiDeviceCount] just took. */
    @Keep
    fun midiDeviceName(index: Int): String = midiOut?.deviceName(index).orEmpty()

    /**
     * Open device [index] for sending. ⚠️ BLOCKS the calling (SDL) thread for up to ~3 s while
     * `MidiManager.openDevice` completes — see `MidiOutManager.open` for why waiting is the right
     * answer and an optimistic `true` is not.
     */
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
    // Five more, mirroring the block above except: the list is `outputPortCount > 0` (to RECEIVE
    // you open the device's OUTPUT port), and the last hook is a READ — native POLLS this port once
    // a frame (midi-in-android.cpp says why). ⚠️ `@Keep` AND `proguard-rules.pro`, and that file's
    // hook count must be updated with any new hook.

    private var midiIn: MidiInManager? = null

    /** How many devices can send MIDI to this phone right now. Re-enumerates — MIDI is hot-pluggable. */
    @Keep
    fun midiInDeviceCount(): Int = midiIn?.deviceCount() ?: 0

    /** Display name of input device [index], from the snapshot [midiInDeviceCount] just took. */
    @Keep
    fun midiInDeviceName(index: Int): String = midiIn?.deviceName(index).orEmpty()

    /**
     * Open input device [index]. ⚠️ BLOCKS the calling (SDL) thread for up to ~3 s, for
     * [midiOpenDevice]'s reason: the MIDI screen's rows show what is OPEN, not what was wanted.
     */
    @Keep
    fun midiInOpenDevice(index: Int): Boolean = midiIn?.open(index) ?: false

    /** Disconnect the receiver and release the port. Idempotent; the native side re-picks freely. */
    @Keep
    fun midiInCloseDevice() {
        midiIn?.close()
    }

    // ── Storage Access Framework ─────────────────────────────────────────────────────────────────
    //
    // `shell/saf-filesystem.cpp` reaches `ContentResolver` / `DocumentsContract` through these
    // one-line delegates to [SafStorage]. ⚠️ `@Keep` AND a `proguard-rules.pro` rule each — CI reads
    // the count from that file.

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
     * **ADD FOLDER… — open the system folder picker.** True = it is on screen, NOT that a folder was
     * granted; the grant lands in [onActivityResult] and the native side never waits for it.
     *
     * ⚠️⚠️ CALLED FROM THE SDL THREAD, and `startActivityForResult` is the UI thread's: posted, and
     * waited for only until the LAUNCH. Waiting for the ANSWER would park the SDL thread, so the
     * autosave and settings flush (`SDL_APP_WILLENTERBACKGROUND`, delivered inside its
     * `SDL_PollEvent`) could not run while the picker is up — exactly when Android may kill us.
     * ⚠️ BOUNDED: SDLActivity blocks the UI thread on the SDL thread in `surfaceDestroyed`, so a
     * rotation could aim the two at each other; two seconds turns that into a `false`.
     */
    @Keep
    fun safRequestRoot(): Boolean {
        val launched = java.util.concurrent.atomic.AtomicBoolean(false)
        val done     = java.util.concurrent.CountDownLatch(1)
        runOnUiThread {
            try {
                launched.set(safStorage.requestRoot(this, REQ_ADD_ROOT))
            } finally {
                // ⚠️ In a `finally`: a throw on the UI thread that never counted down would park the
                // SDL thread for the full timeout on every press.
                done.countDown()
            }
        }
        if (!done.await(2, java.util.concurrent.TimeUnit.SECONDS))
            Log.w(TAG, "saf: folder picker did not launch within 2s - UI thread busy?")
        return launched.get()
    }

    /**
     * The picker's answer.
     *
     * ⚠️ `super` first and unconditionally: `SDLActivity` has its own result handling, and swallowing
     * a result that was never ours is the kind of break that shows up as an unrelated feature dying.
     *
     * Nothing is pushed to native here. The grant is persisted, and the browser picks it up when the
     * app returns to the foreground — the roots directory is never cached, so re-listing IS the
     * refresh (`shell/saf-filesystem.cpp`).
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
     * Move whatever has arrived since the last frame into [out]; returns how many bytes.
     *
     * ⚠️ The array is allocated ONCE on the native side and reused — no allocation per frame on the one
     * thread that must not stutter. Anything this does not take stays in `MidiInManager`'s ring.
     */
    @Keep
    fun midiInRead(out: ByteArray): Int = midiIn?.read(out) ?: 0

    /**
     * The one-time SharedPreferences → settings.json migration, for users upgrading from the app's
     * earlier releases, which kept settings in SharedPreferences. In Kotlin because SharedPreferences
     * is Android's format: reading it with the old defaults is a fact; parsing its XML from C++ would
     * be a guess.
     *
     * ⚠️⚠️ VERSIONED, NOT KEYED OFF "settings.json IS ABSENT": that guard gets one chance and makes
     * any later pass unreachable. v2 added the SKIN and OVERLAY selections to the fresh-install write.
     * An existing settings.json still WINS (below).
     *
     * ⚠️ THE DEFAULTS BELOW ARE THE OLD APP'S, NOT `SettingsValues`'s, on purpose: what must survive
     * is what the user EXPERIENCED (button sound/vibro on, sound volume 0x80); the C++ defaults would
     * silently switch button sound off for everyone who left it alone.
     *
     * ⚠️ `app_theme` is passed through VERBATIM: `serialize_theme` emits the same bytes as the old
     * serializer, with identical colour defaults (both omit fields at their default) — checked field
     * by field, so re-serialising would only add a second format.
     *
     * ⚠️ Debug and release do NOT share SharedPreferences (`applicationIdSuffix = ".debug"`).
     */
    private fun importLegacySettings() {
        val prefs = getSharedPreferences("pockettracker_ui", MODE_PRIVATE)
        val done  = prefs.getInt(IMPORT_VERSION_KEY, 0)
        if (done >= SETTINGS_IMPORT_VERSION) {
            Log.i(TAG, "settings import: already at v$done, nothing to do")
            return
        }

        // ⚠️ `filesDir`, and it is the same place `SafFileSystem::settings_path` reads from: this must
        // write the file the native side will OPEN. A migration whose output nothing opens is a
        // migration that silently did not happen.
        val target = File(filesDir, "settings.json")

        // ⚠️ An existing settings.json WINS, and the version is still stamped: its values were chosen
        // in this UI, and re-importing older prefs over them would be a regression.
        if (target.exists()) {
            prefs.edit().putInt(IMPORT_VERSION_KEY, SETTINGS_IMPORT_VERSION).apply()
            Log.i(TAG, "settings import: ${target.name} already exists - keeping it, marked v$SETTINGS_IMPORT_VERSION")
            return
        }

        // Nothing to migrate FROM is not a failure: it is a fresh install, and the C++ defaults are
        // the right answer. Stamp it so this never runs again.
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

            // ⚠️ `trace` and `engine_cpp_v2` are NOT imported: a developer switch, and a choice of a
            // Kotlin sequencer that no longer exists — a stored value that was never the user's.

            // ── The device rows that are plain scalars ───────────────────────────────────────────
            json.put("buttonSound",       prefs.getBoolean("button_sound", true))
            json.put("buttonSoundVolume", prefs.getInt("button_sound_volume", 0x80))
            json.put("buttonVibro",       prefs.getBoolean("button_vibro", true))
            json.put("vibroPower",        prefs.getInt("vibro_power", 255))
            json.put("overlayStrength",   prefs.getInt("overlay_strength", 128))

            // ── The device-row SELECTIONS, as STABLE STRINGS (v2) ────────────────────────────────
            // SKIN and OVERLAY resolve against `device_skin.h` / `shell/overlay.h`, so their stored ids
            // move across (`portrait_skin` / `overlay_name`). ⚠️ LAYOUT (`layout_mode`) is not: the
            // shell picks the layout by orientation and controller, so it has nothing to resolve to.
            json.put("portrait_skin", prefs.getString("portrait_skin", DEFAULT_SKIN_ID))
            json.put("overlay_name",  prefs.getString("overlay_name", "OFF"))

            // ── The theme ────────────────────────────────────────────────────────────────────────
            // The palette is the most visible thing here. Both `appTheme` (what the reader prefers)
            // and `theme` (the name, for an older build) are written, as `serialize_settings` emits.
            val storedTheme = prefs.getString("app_theme", null)
            if (storedTheme != null) {
                val parsed = JSONObject(storedTheme)
                json.put("appTheme", parsed)
                json.put("theme", parsed.optString("name", "CLASSIC"))
                // The visualizer is the theme's FIELD but the user's CHOICE — settings.json carries it
                // as a top-level int, so it is translated out of the theme object here exactly as
                // `load_settings` expects to find it.
                json.put("visualizer", visualizerIndex(parsed.optString("visualizerType", "SCOPE")))
            }

            target.parentFile?.mkdirs()
            target.writeText(json.toString(2) + "\n")
            prefs.edit().putInt(IMPORT_VERSION_KEY, SETTINGS_IMPORT_VERSION).apply()
            Log.i(TAG, "settings import: wrote ${target.absolutePath} " +
                       "(${json.length()} keys, theme=${json.optString("theme", "-")}), marked v$SETTINGS_IMPORT_VERSION")
        } catch (e: Exception) {
            // ⚠️ NOT stamped on failure, so the next launch tries again. And deliberately not fatal:
            // losing a migration costs the user their settings, and crashing on the way in costs them
            // the app. The log line is the only thing that says which happened.
            Log.e(TAG, "settings import FAILED - settings will fall back to defaults: ${e.message}", e)
        }
    }

    /** `VisualizerType`'s ordinal, which settings.json stores — the order of `theme.h` and
     *  `settings_store.cpp`'s VISUALIZER_COUNT. */
    private fun visualizerIndex(name: String): Int = when (name) {
        "SCOPE"          -> 0
        "FLAT"           -> 1
        "OCTA"           -> 2
        "OCTA_FULL"      -> 3
        "SPECTRUM"       -> 4
        "SPECTRUM_PEAKS" -> 5
        else             -> 0
    }

    private companion object {
        const val TAG = "PocketTrackerSDL"

        /** How long a rotation the device DID accept has to complete, before [portraitCheck] reads
         *  the configuration back and concludes it was refused. */
        const val ORIENTATION_SETTLE_MS = 2000L

        /**
         * Bump this when there are new keys to migrate, and add an arm for them.
         * v1 — the every-platform rows, RESUME, the button-feedback scalars, overlay STRENGTH, theme.
         * v2 — the SKIN and OVERLAY selections (`portrait_skin` / `overlay_name`). LAYOUT stays out.
         */
        const val SETTINGS_IMPORT_VERSION = 2
        const val IMPORT_VERSION_KEY = "settings_import_version"

        /** The old app's default skin id, read when the user never chose one — also the shell's
         *  fallback for an unknown id (device_skin.h). */
        const val DEFAULT_SKIN_ID = "amiga-2"

        /** [safRequestRoot]'s `startActivityForResult` code, matched in [onActivityResult]. */
        const val REQ_ADD_ROOT = 1001

        /** [askForNotificationsOnce] — the request code, and where "already asked" is remembered. */
        const val REQ_NOTIFICATIONS = 1002
        const val SHELL_PREFS = "pockettracker_shell"
        const val NOTIFICATIONS_ASKED_KEY = "notifications_asked"
    }
}
