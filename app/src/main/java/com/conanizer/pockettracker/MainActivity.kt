package com.conanizer.pockettracker

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.provider.Settings
import android.util.Log
import android.view.InputDevice
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import androidx.annotation.Keep
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
import java.io.IOException

/**
 * PocketTracker's Android entry point: an `SDLActivity` subclass. Convergence Phase E.
 *
 * This is the whole of the Android-only surface now. The ~15,000-line Compose UI, the input
 * dispatcher and the JNI audio facade are gone, replaced by the shared C++ SDL shell (`shell/`,
 * `native/ui/`) that already drives Windows and the Linux handhelds. All this class still does is the
 * handful of jobs that are genuinely Java's: point SDL at the native libraries and the two roots, own
 * the splash screen and the immersive / edge-to-edge window, ask for storage permission, run the two
 * one-shot migrations (settings values out of SharedPreferences, app files out of shared storage), and
 * route button feedback (sound/haptics) back from the shell over one JNI hook.
 *
 * ⚠️ It began life as `SdlActivity` in `src/debug/` — the second, debug-only activity the app carried
 * beside Compose through phases C and D, so touch could be developed without breaking the shipped UI
 * (see docs/internal/convergence-plan.md §5–7 and the git history at tag `kotlin-ui-final`). Phase E
 * deleted the Compose `MainActivity` and moved this class into `src/main/` under that name. Inline
 * comments below that cite `MainActivity.kt:<line>` refer to that now-deleted Compose activity as the
 * prior art each behaviour was learned from.
 *
 * ⚠️ **C4 added the three things C3 deliberately left out**, and two of them are not here at all —
 * which is the point. The permission request and the system bars are Java's, so they are below. The
 * lifecycle is NOT: the autosave/settings flush is a `SDL_AddEventWatch` watcher in `shell/app.cpp`,
 * shared with every other platform, because `SDL_APP_WILLENTERBACKGROUND` fires on the NATIVE thread
 * inside the frame loop's own `SDL_PollEvent` — not on this thread. The back button is likewise
 * split: the hint is armed in `shell/android-main.cpp` and the key is mapped in `shell/sdl-input.cpp`.
 * Nothing about the lifecycle needs Kotlin.
 */
class MainActivity : SDLActivity() {

    // ── Button feedback (convergence D) ────────────────────────────────────────────────────────────
    //
    // The surviving thin Kotlin the plan keeps: SoundPool clicks and Vibrator pulses are Android system
    // services with no C++ twin, so they stay here and the shared shell reaches them through ONE JNI
    // call (`onButtonFeedback` below). Created in `onCreate`, before `super.onCreate()` starts the SDL
    // thread, so the SoundPool has begun loading its samples before the first tap can arrive.
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
     * The app root, resolved HERE because only Java can resolve it.
     *
     * ⚠️ `ui::default_app_root()` on the C++ side walks `POCKETTRACKER_HOME` → `XDG_DATA_HOME` →
     * `HOME` and every one of them misses on Android — it would fall through to a RELATIVE path and
     * put the user's songs beside whatever the process's cwd happens to be. That is exactly the A1
     * bug, which was found on Windows for the same reason. `Environment` is the only thing that
     * knows the real answer on this device and this OS version, so it answers, and
     * `android-main.cpp` takes it as argv[1].
     *
     * The path matches what `AndroidFileSystem.kt` has always used, which is what makes this activity
     * open the SAME projects the Compose app does rather than a parallel empty world.
     *
     * ⚠️ **argv[2] is a SECOND root, and it is not the same directory.** The media tree above is user
     * storage the app may be refused; `filesDir` is app-private, needs no permission, and cannot be
     * revoked. `settings.json`, `template.ptp` and `autosave.ptp` are read during the native boot,
     * before the user has been asked for anything, so they live there — see `StdFileSystem`'s two-root
     * constructor for the argument, and [migrateAppFilesToPrivateStorage] for the users who already
     * have those three files in the tree. `config.json` is deliberately NOT one of them: it is the one
     * file the user hand-edits, and app-private storage is reachable only over adb.
     */
    /**
     * ⚠️ **argv[3] is SCAFFOLDING WITH A KNOWN DEATH DATE (SAF migration P3).** It selects which
     * `ui::FileSystem` the shell constructs, so one build can be driven down both the old media-tree
     * path and the new SAF one and the two compared while `MANAGE_EXTERNAL_STORAGE` is still granted:
     *
     *     adb shell am start -n …/.MainActivity --es storage saf
     *
     * Absent — every normal launch, including every launcher icon tap — this is empty and the shell
     * takes the `StdFileSystem` branch it has always taken. It is an intent extra rather than a
     * SETTINGS row because the row would be real UI work for something P4 deletes, and rather than a
     * `config.json` key because that file lives in the very tree SAF may not have granted yet.
     * **P4 removes this argument and the branch below it together.**
     */
    override fun getArguments(): Array<String> =
        arrayOf(appRoot(), privateRoot(), intent?.getStringExtra("storage") ?: "")

    private fun appRoot(): String =
        File(
            Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS),
            "PocketTracker"
        ).absolutePath

    private fun privateRoot(): String = filesDir.absolutePath

    /**
     * Hide the status and navigation bars (immersive sticky) — **C4, and NOT cosmetic.**
     *
     * ⚠️⚠️ **THE STATUS BAR COSTS A WHOLE SCALING FACTOR.** With it visible the SDL window is
     * **1280×904**, not 1280×960, because the bar keeps 56 px — and 2× of the 640×480 design needs
     * exactly 960. So `SdlVideo::dest_rect`'s INTEGER scale computes `min(1280/640, 904/480)` =
     * `min(2, 1)` = **1×**, and the tracker draws at a quarter of the area it should with 320 px
     * letterbox bars either side. Nothing is wrong with the scaler; it is doing the right thing with
     * the wrong window. Hidden, the panel is 1280×960 and 2× is pixel-exact and full-screen.
     *
     * ⚠️ This is a lesson this app already paid for once: `MainActivity.kt:158` says in its own
     * comment that reserving inset padding "can drop scale from 2× to 1×". The Compose activity has
     * always hidden the bars; `SdlActivity` simply never inherited the knowledge.
     *
     * Nothing on the C++ side has to be told: `dest_rect()` asks `SDL_GetRendererOutputSize` every
     * frame, so the resize is picked up on the next present with no resize handler at all.
     *
     * ⚠️ `decorView.post` because API 30+ requires the DecorView to be ATTACHED before
     * `insetsController` is non-null — the same reason MainActivity posts it.
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
        // The Compose activity has shown one since long before the port (`MainActivity.kt:151`, plus
        // `Theme.Pockettracker.Splash` on its manifest entry); this activity was given the plain
        // theme in C3 and so came up on a blank window instead. Reported by the user as "the Android
        // build misses the splash screen while opening", and it is the same shape as C4's
        // `hideSystemBars`: knowledge the Compose activity had and the SDL one never inherited.
        //
        // ⚠️ BOTH HALVES ARE REQUIRED. The manifest entry supplies the windowBackground the system
        // draws before any of our code runs (API 31+ builds its splash from the theme alone), and
        // this call is what hands over to `postSplashScreenTheme` afterwards and back-ports the whole
        // thing below API 31. Either one alone leaves a visible gap.
        //
        // ⚠️ FIRST, and ahead of `setDecorFitsSystemWindows` below, because this is the call that
        // swaps the activity's theme — doing it after would apply a theme over a window we have
        // already configured. `MainActivity` has the same call in the same position.
        //
        // ⭐ The colours already agree with no work: `splash_bg` is #0A0A0A and `pt::ui::Theme`'s
        // `background` default is 0xFF0A0A0A, so the splash and the tracker's first frame are the
        // same colour and the handover has no seam in it.
        installSplashScreen()

        // ⚠️ Without MANAGE_EXTERNAL_STORAGE the C++ `StdFileSystem` cannot see /storage/emulated/0
        // and the file browser comes up EMPTY — which looks exactly like "C5's spike says
        // std::filesystem does not work on Android", the single most important open question phase C
        // answered. A wrong answer there would have been recorded as an architectural fact and cost
        // `AndroidFileSystem.kt` its deletion in Phase E. So the state is still LOGGED beside the
        // result, which is this project's standing rule for instruments — read this line before
        // believing an empty browser.
        //
        // C3 logged it and left the granting to the Compose activity. C4 asks.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val granted = Environment.isExternalStorageManager()
            Log.i(TAG, "MANAGE_EXTERNAL_STORAGE granted=$granted  appRoot=${appRoot()}  " +
                       "privateRoot=${privateRoot()}")
            if (!granted) requestAllFilesAccess()
        }

        // ⚠️⚠️ **EDGE-TO-EDGE, AND `hideSystemBars()` ALONE DOES NOT DO IT — MEASURED, NOT ASSUMED.**
        // The first C4 build hid the bars and the window STAYED 1280x904: `dumpsys` reported
        // `statusBars visible=false` while SDL's renderer output was still 904 px tall, so INTEGER
        // scaling was still falling back to 1x. Hiding a bar and letting the content DRAW WHERE IT WAS
        // are two different requests — without this line Android keeps reserving inset padding for a
        // bar that is no longer on screen, and the SurfaceView is laid out inside the reduced area.
        //
        // `MainActivity.kt:156` has carried this call, and a comment naming this exact symptom ("can
        // drop scale from 2x to 1x"), since long before the port. The SDL activity had to learn it the
        // expensive way.
        //
        // ⚠️ BEFORE `super.onCreate()`, which is where SDLActivity builds its layout and surface: set
        // afterwards, the surface is created at the inset size and then resized, and every consumer
        // (including the boot `video:` line) sees the wrong number first. Set here, the FIRST surface
        // is already 1280x960. `getWindow()` is valid from `attach()`, well before onCreate.
        WindowCompat.setDecorFitsSystemWindows(window, false)

        // Draw behind a punch-hole/notch too. In landscape the cutout is on a short edge, so without
        // this the panel gives back less height than it has — the same 2x-becomes-1x arithmetic,
        // arriving through a different subtraction. Harmless on a device with no cutout, like this one.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            window.attributes.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
        }

        // ⚠️ BEFORE `super.onCreate()` for a harder reason than the two above: that call starts the
        // SDL thread, which runs `SDL_main`, which calls `load_settings()`. Anything either of these
        // writes after that point is a file the app has already read past.
        //
        // ⚠️ AND IN THIS ORDER. The relocation moves an existing `settings.json` into `filesDir`;
        // `importLegacySettings` then finds it there and correctly leaves it alone. Reversed, the
        // import would see an empty `filesDir`, write factory-plus-prefs values into it, and the
        // relocation would then decline to overwrite them with the user's real file.
        migrateAppFilesToPrivateStorage()
        importLegacySettings()

        // The feedback managers, before super.onCreate() starts the SDL thread that calls back into
        // onButtonFeedback. Constructing the SoundPool early lets it load the click samples off the
        // critical path, so the first tap is not silent while they decode.
        buttonSound  = ButtonSoundManager(this)
        buttonHaptic = ButtonHapticManager(this)

        // The MIDI port, for the same reason and in the same place: the SDL thread `super.onCreate()`
        // starts calls `boot_midi_port()` during its boot, which re-opens the saved device. Nothing
        // here touches hardware — it only takes the system service — so it costs the splash nothing.
        midiOut = MidiOutManager(this)
        // …and the INPUT port beside it (E5), for the identical reason: `boot_midi_in_port()` runs in
        // the same native boot and re-opens the saved keyboard.
        midiIn = MidiInManager(this)

        super.onCreate(savedInstanceState)
        hideSystemBars()
    }

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
        // ⚠️ The same backstop for the input port (E5), and it matters for a different reason: an open
        // MidiOutputPort holds the DEVICE, so a port left connected after this activity dies is a
        // keyboard no other app on the phone can use until PocketTracker's process is killed.
        midiIn?.close()
        midiIn       = null
        super.onDestroy()
    }

    /**
     * **Called from native (`shell/android-main.cpp`, on the SDL thread) on every virtual-button press
     * and release** — the one outward JNI hook the convergence plan's Phase-E table names. The shared
     * touch layer (`sdl-touch.cpp`) owns the DECISION to fire and passes the live BTN SOUND / BTN VIBRO
     * scalars across; this only routes them to the two managers, which are unchanged from the Compose
     * app. ⚠️ Resolved by name over JNI, so `@Keep` here is LOAD-BEARING now that this class is in
     * `src/main` and R8 runs on release — plus an explicit `-keep` in `proguard-rules.pro`, per the
     * project's standing rule for JNI-by-name callbacks (a renamed member is an `UnsatisfiedLinkError`
     * at runtime in release only, exactly the v0.9.3 DEX bug class).
     *
     * @param button ordinal of the virtual button — matches `VirtualButton`'s order exactly, which is
     *               `pt::ui::Button`'s order (native/ui/buttons.h), so it passes straight through.
     * @param down   true = press feel, false = release (a lift or a slide-off).
     *
     * ⚠️ The haptic is posted to the UI thread; the sound is not. `SoundPool.play` is thread-safe and
     * lowest-latency called straight from here, but `ButtonHapticManager`'s bottom fallback reaches a
     * `View.performHapticFeedback`, which wants the UI thread — and the post costs nothing perceptible
     * on a pulse. The Vibrator itself is thread-safe; posting the whole call is simply the safe default.
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
     * ⚠️ **THIS EXISTS BECAUSE SDL AND ANDROID DISAGREE ABOUT WHAT A CONTROLLER IS.** SDL's
     * `isDeviceSDLJoystick` opens any device with a GAMEPAD *or a bare DPAD* source, so the emulator's
     * built-in keyboard registers as a full game controller — `SdlInput::controller_count()` reads 1 and
     * the app wrongly drops the touch UI (fullscreen frame, empty LAYOUT row). The tests that tell a pad
     * from an impostor are Java-only, which is why the shell asks over JNI at all; they live in
     * [PadClassifier], which also explains why the `sources` bitmask alone is not one of them.
     *
     * ⚠️ Called by name over JNI, so `@Keep` plus an explicit `proguard-rules.pro` `-keep` guard it
     * against R8, which runs on this class in release (`src/main`).
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
     * Everything [hasPhysicalGameButtons] looked at, as text, plus the device identity — the payload
     * of a "it opened without the on-screen buttons" report.
     *
     * ⚠️ **UNCONDITIONAL, and that is the point.** [hasPhysicalGameButtons] logs only when it FINDS a
     * pad, so a wrong `true` and a device that was never enumerated look identical from outside: the
     * one state that needs explaining is the one that leaves no record. Every device is listed with its
     * raw `sources` bitmask AND the per-device reason [PadClassifier] decided on — the same call the
     * verdict itself is made of, so the two cannot drift — and a device that claims to be a pad and is
     * not says so, with its evidence, on its own line.
     *
     * ⚠️ Returned as a STRING rather than logged here, so native can `printf` it through the stdout
     * pipe — which is what tees it into `pockettracker-log.txt`, the copy a user can actually send.
     * A `Log.i` from Kotlin reaches logcat only, and logcat needs a PC. Called by name over JNI, so it
     * needs its `-keep` in `proguard-rules.pro` like every other hook here.
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

    // ── EXTERNAL MIDI out (MIDI plan phase B2b) ────────────────────────────────────────────────────
    //
    // Five more by-name JNI hooks, and the same shape as `onButtonFeedback` above: an Android system
    // service with no C++ twin, reached through the narrowest surface that works. `MidiOutManager`
    // holds the whole of it; these five only forward. See that file for why MidiManager is
    // unavoidable (AMidi does not remove the Java half) and for the direction gotcha — to SEND you
    // open the device's INPUT port.
    //
    // ⚠️ All five are `@Keep` AND listed in `proguard-rules.pro`, per the project's standing rule for
    // JNI-by-name callbacks. A renamed member here is silent in debug and kills MIDI in release only —
    // the v0.9.3 DEX bug class. `shell/midi-out-android.cpp` logs one line at resolve time saying
    // whether it found them, so the failure is visible in logcat instead of looking like "no devices".

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

    // ── MIDI IN (MIDI plan phase E5) ───────────────────────────────────────────────────────────────
    //
    // Five more, and the mirror of the block above in every way but two, both of which are written out
    // in `MidiInManager`: the device list is `outputPortCount > 0` (to RECEIVE you open the device's
    // OUTPUT port — the exact opposite of the block above), and the last hook is a READ rather than a
    // send, because the native side POLLS this port once a frame instead of being called from the
    // binder thread the MIDI service delivers on. `shell/midi-in-android.cpp` says why polling costs
    // nothing here and buys a single JNI direction for the whole app.
    //
    // ⚠️ `@Keep` AND `proguard-rules.pro`, exactly as above. Twelve by-name hooks now, and the count in
    // that file's comment is part of the check — a hook added without updating it is one nobody knows is
    // unprotected until a release APK has shipped with MIDI silently dead.

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

    // ── Storage Access Framework (SAF migration P3) ──────────────────────────────────────────────
    //
    // `ContentResolver` and `DocumentsContract` are Java-only, so `shell/saf-filesystem.cpp` reaches
    // them through these six. Each is a one-line delegate to [SafStorage], which holds the whole of
    // the SAF knowledge; nothing here decides anything.
    //
    // ⚠️ `@Keep` AND an explicit `proguard-rules.pro` rule, exactly as the thirteen above — the count
    // in that file is the count, and CI reads it from there rather than from a sentence.

    private val safStorage: SafStorage by lazy { SafStorage(this) }

    /** How many folders the user has granted. Zero is the fresh-install state, not an error. */
    @Keep
    fun safRootCount(): Int = safStorage.rootCount()

    /** `<id>\t<displayName>\t<docUri>` for granted tree [index], or "" if it is gone. */
    @Keep
    fun safRootInfo(index: Int): String = safStorage.rootInfo(index)

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

    /**
     * Move whatever has arrived since the last frame into [out]; returns how many bytes.
     *
     * ⚠️ The array is allocated ONCE on the native side and reused — no allocation per frame on the one
     * thread that must not stutter. Anything this does not take stays in `MidiInManager`'s ring.
     */
    @Keep
    fun midiInRead(out: ByteArray): Int = midiIn?.read(out) ?: 0

    /**
     * **C6 — the one-time SharedPreferences → settings.json migration.**
     *
     * Android has kept its settings in SharedPreferences since the app existed; `pt-ui` keeps them in
     * `settings.json`. Without this, every existing user's settings silently reset the day the SDL UI
     * becomes the shipping one — their theme included, which is the one they would notice.
     *
     * It is written HERE rather than in C++ because SharedPreferences is a Java API backed by an XML
     * file whose format is Android's business, not ours: `getBoolean` with the same default the
     * Compose app used is a fact, and parsing that XML from C++ would be a guess maintained forever.
     *
     * ⚠️⚠️ **VERSIONED, NOT KEYED OFF "settings.json IS ABSENT" — and that distinction is the whole
     * design.** The obvious guard is *"no settings.json + prefs exist → import"*, and it gets exactly
     * ONE chance: the moment the SDL app runs once, settings.json exists forever after and no later
     * migration can ever fire. Phase D adds SKIN and OVERLAY selections (indices into lists that did not
     * exist at v1 — see the note in `settings_store.cpp`), so a second pass was always coming, and the
     * "absent file" guard would have made it unreachable before it was written. The version counter is
     * what keeps that honest: v2 folds the two new stable-string keys into the SAME fresh-install write
     * (the real upgrade path — a user who only ever ran Compose has no settings.json yet), and stamps v2
     * so it never re-runs. An existing settings.json still WINS (below): a population that already ran
     * the debug SDL activity has SDL-chosen values there, and re-importing older prefs over them would be
     * a regression dressed as a migration.
     *
     * ⚠️ **The defaults below are ANDROID's, not `SettingsValues`'s, and they disagree on purpose.**
     * `button_sound` and `button_vibro` default TRUE in the Compose app while the C++ struct defaults
     * them FALSE; `button_sound_volume` is 0x80 there and 255 here. What must survive a migration is
     * what the user actually EXPERIENCED, and for a row they never touched that is the value the
     * Compose app was using — so the pref's own default is the correct thing to read and write. Taking
     * the C++ defaults instead would silently switch off button sound for every user who had left it
     * alone, which is exactly the class of upgrade bug this whole function exists to prevent.
     *
     * ⚠️ **`app_theme` is passed through VERBATIM, and that is sound rather than lazy.**
     * `theme_io.h`'s `serialize_theme` states in its own comment that it emits kotlinx's bytes, and
     * all 18 colour defaults plus `name` and `visualizerType` were compared field by field against
     * `AppTheme.kt` — they are identical. Since both sides OMIT fields equal to their default, a
     * mismatch anywhere would have silently recoloured a theme, which is why it was checked rather
     * than assumed. Re-serialising it here would add a second format to keep in step for no gain.
     *
     * ⚠️ **Debug and release do NOT share SharedPreferences.** `applicationIdSuffix = ".debug"` gives
     * this build its own data directory, so what this reads today is whatever the *debug* Compose
     * activity wrote — not the songs-and-settings of the real install. That is a testing note, not a
     * defect: in Phase E the SDL activity replaces `MainActivity` inside the one real package and the
     * prefs it reads are the user's own. To exercise it, run "PocketTracker" (debug Compose), change
     * some settings, delete settings.json, then run "PT (SDL)".
     */
    private fun importLegacySettings() {
        val prefs = getSharedPreferences("pockettracker_ui", MODE_PRIVATE)
        val done  = prefs.getInt(IMPORT_VERSION_KEY, 0)
        if (done >= SETTINGS_IMPORT_VERSION) {
            Log.i(TAG, "settings import: already at v$done, nothing to do")
            return
        }

        // ⚠️ `filesDir`, not the media tree: this must write the file the native side will READ, and
        // that moved with [migrateAppFilesToPrivateStorage]. A migration that writes to the old
        // location is a migration whose output nothing opens.
        val target = File(filesDir, "settings.json")

        // ⚠️ An existing settings.json WINS, and the version is still stamped. During phases C and D
        // this activity has already been run by hand, so a settings.json is sitting there with values
        // chosen through the SDL UI itself — clobbering those with older prefs would be a regression
        // dressed as a migration. Stamping the version regardless is what stops this from re-arming
        // later and overwriting a settled file the first time a user clears their prefs.
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

            // ⚠️ `trace` is NOT imported. It is a developer switch, it is off in every shipped build,
            // and `engine_cpp_v2` is not imported either: the converged app has no Kotlin sequencer to
            // switch TO, so the value is not merely stale, it is unanswerable. That is the same call
            // the `engine_cpp` key got in songcore S7 — a stored value that was never the user's
            // choice must be abandoned rather than honoured (see order-of-work.md).

            // ── The device rows that are plain scalars ───────────────────────────────────────────
            json.put("buttonSound",       prefs.getBoolean("button_sound", true))
            json.put("buttonSoundVolume", prefs.getInt("button_sound_volume", 0x80))
            json.put("buttonVibro",       prefs.getBoolean("button_vibro", true))
            json.put("vibroPower",        prefs.getInt("vibro_power", 255))
            json.put("overlayStrength",   prefs.getInt("overlay_strength", 128))

            // ── The device-row SELECTIONS, as STABLE STRINGS (v2) ────────────────────────────────
            // SKIN and OVERLAY are now indices into lists the shell HAS — `device_skin.h` resolves the
            // skin and `shell/overlay.h` the overlay — so their persisted names finally have a consumer
            // and move across with the rest. Written as the ids the Compose app stored (matching
            // `settings_store.cpp`'s `portrait_skin` / `overlay_name` keys). ⚠️ LAYOUT (`layout_mode`)
            // is still absent by design: there is no shell-side layout-mode override to resolve a name
            // against (the shell auto-selects by orientation + controller), so its stored value is
            // unanswerable here, exactly like `trace`/`engine_cpp_v2` above.
            json.put("portrait_skin", prefs.getString("portrait_skin", DEFAULT_SKIN_ID))
            json.put("overlay_name",  prefs.getString("overlay_name", "OFF"))

            // ── The theme ────────────────────────────────────────────────────────────────────────
            // The palette the user dialled in is the single most visible thing in this migration, and
            // the one they could not reconstruct. Both `appTheme` (what the C++ reader prefers) and
            // `theme` (the name, what an older build reads) are written, mirroring what
            // `serialize_settings` itself emits.
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

    /**
     * **The one-time relocation of the app's own files out of shared storage.**
     *
     * `settings.json`, `template.ptp` and `autosave.ptp` used to sit in `Documents/PocketTracker/`
     * beside the user's six folders. They now live in [filesDir]; this copies an existing user's three
     * across on the first launch after the update, while the storage permission is still granted.
     *
     * ⚠️⚠️ **IT MUST SHIP A RELEASE BEFORE THE PERMISSION IS DROPPED.** Removing
     * `MANAGE_EXTERNAL_STORAGE` from the manifest auto-revokes it on update, so a build that both drops
     * the permission and migrates would find the old files already unreadable — and a user's settings,
     * their song template and any unsaved work would be gone with no way back. That is the whole reason
     * this is a phase of its own rather than part of the SAF switch.
     *
     * ⚠️ **VERSIONED, exactly as [importLegacySettings] is versioned, and for the same reason**: the
     * obvious guard is *"filesDir has no settings.json → migrate"*, which gets one chance and is spent
     * the moment the app writes its first settings file. A counter can be bumped for a fourth file
     * later; an artifact check cannot.
     *
     * ⚠️ **Not stamped when the permission is absent, and that distinction is load-bearing.** Without
     * All-files access the old directory reads as empty, which is indistinguishable from a fresh
     * install with nothing to migrate — so stamping there would silently spend the migration on a
     * user who is one Settings toggle away from having files to move. Unstamped, the next launch
     * retries; the cost is three `File.exists` calls.
     *
     * ⚠️ **The originals are left where they are.** They are already invisible to the browser, which
     * lists only the six sub-directories, so they cost three small files and nothing else — and a
     * one-shot migration has no second chance to undo a delete it should not have made.
     */
    private fun migrateAppFilesToPrivateStorage() {
        val prefs = getSharedPreferences("pockettracker_ui", MODE_PRIVATE)
        val done  = prefs.getInt(APP_FILES_MIGRATION_KEY, 0)
        if (done >= APP_FILES_MIGRATION_VERSION) {
            Log.i(TAG, "app-file migration: already at v$done, nothing to do")
            return
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && !Environment.isExternalStorageManager()) {
            Log.i(TAG, "app-file migration: no All-files access - deferring, NOT marking done")
            return
        }

        try {
            var copied = 0
            var kept   = 0
            var absent = 0
            for (name in APP_FILES_TO_RELOCATE) {
                val dst = File(filesDir, name)
                if (dst.exists()) { kept++; continue }      // a newer build already wrote one: it wins
                val src = File(appRoot(), name)
                if (!src.isFile) { absent++; continue }

                src.copyTo(dst, overwrite = false)

                // ⚠️ Read the SIZE BACK OFF THE DESTINATION rather than trusting copyTo to have thrown.
                // A short copy — a full data partition is the realistic one — leaves a file that exists,
                // parses as truncated JSON and takes the user's settings with it. Dropping the partial
                // and failing the whole migration leaves the original intact and the version unstamped,
                // so the next launch tries again.
                if (dst.length() != src.length()) {
                    val short = dst.length()
                    dst.delete()
                    throw IOException("$name copied ${short}B of ${src.length()}B")
                }
                copied++
            }

            prefs.edit().putInt(APP_FILES_MIGRATION_KEY, APP_FILES_MIGRATION_VERSION).apply()
            Log.i(TAG, "app-file migration: $copied copied, $kept already present, $absent not there " +
                       "-> ${filesDir.absolutePath}, marked v$APP_FILES_MIGRATION_VERSION")
        } catch (e: Exception) {
            // Not stamped, and deliberately not fatal — [importLegacySettings]'s reasoning verbatim:
            // losing a migration costs the user their settings, crashing on the way in costs them the
            // app, and this line is the only thing that says which happened.
            Log.e(TAG, "app-file migration FAILED - retrying next launch: ${e.message}", e)
        }
    }

    /** `VisualizerType`'s ordinal, which is what settings.json stores. The order is the enum's, and it
     *  is the same list in `AppTheme.kt`, `theme.h` and `settings_store.cpp`'s VISUALIZER_COUNT. */
    private fun visualizerIndex(name: String): Int = when (name) {
        "SCOPE"          -> 0
        "FLAT"           -> 1
        "OCTA"           -> 2
        "OCTA_FULL"      -> 3
        "SPECTRUM"       -> 4
        "SPECTRUM_PEAKS" -> 5
        else             -> 0
    }

    /**
     * Send the user to the All files access settings page.
     *
     * ⚠️ There is no runtime-permission dialog for `MANAGE_EXTERNAL_STORAGE` — it is granted only
     * through Settings, so this is a `startActivity`, not a permission request, and it cannot be
     * answered inline. `READ_MEDIA_AUDIO` and friends are deliberately NOT requested here: they
     * govern MediaStore, and nothing in the SDL build goes through MediaStore. The native
     * `std::filesystem` path this port rests on is governed by All files access alone.
     *
     * ⚠️ Both intents, in order, and the fallback is not theoretical — `MainActivity` carries the
     * same pair because some custom ROMs (/e/OS was the one that bit us) do not expose the
     * app-specific page at all. If neither resolves, the app still runs; the browser is just empty
     * and the log above says why.
     *
     * ⚠️ Called BEFORE `super.onCreate()`, i.e. before the SDL thread exists. Settings comes up over
     * us and the activity is immediately paused — which is fine, and is in fact the first real
     * exercise of C4's background watcher, on a blank document where every step of it is a no-op.
     */
    private fun requestAllFilesAccess() {
        Log.i(TAG, "requesting MANAGE_EXTERNAL_STORAGE - the file browser is empty without it")
        try {
            startActivity(
                Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION)
                    .setData(Uri.parse("package:$packageName"))
            )
        } catch (e: Exception) {
            Log.w(TAG, "app-specific All-files-access page unavailable: ${e.message}")
            try {
                startActivity(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
            } catch (e2: Exception) {
                Log.e(TAG, "All-files-access settings unavailable entirely: ${e2.message}")
            }
        }
    }

    private companion object {
        const val TAG = "PocketTrackerSDL"

        /**
         * Bump this when a later phase has new keys to migrate, and add an arm for them.
         *
         * **v1 (C6)** — the rows that exist today: the four every-platform ones, RESUME, the four
         * button-feedback scalars, overlay STRENGTH, and the theme.
         * **v2 (Phase D6)** — the SKIN and OVERLAY *selections* (`portrait_skin` / `overlay_name`),
         * now that `device_skin.h` and `shell/overlay.h` give their stored names a list to resolve
         * against. LAYOUT (`layout_mode`) stays out — the shell has no layout-mode override, so there is
         * still nothing to resolve it against.
         */
        const val SETTINGS_IMPORT_VERSION = 2
        const val IMPORT_VERSION_KEY = "settings_import_version"

        /**
         * Its own counter, separate from [SETTINGS_IMPORT_VERSION]: the two migrations answer different
         * questions ("where do the values come from" and "where does the file live") and a user can
         * legitimately be done with one and not the other. Bump this — and add to
         * [APP_FILES_TO_RELOCATE] — if a fourth app file ever has to move.
         *
         * **v1** — `settings.json`, `template.ptp` and `autosave.ptp` out of `Documents/PocketTracker/`.
         */
        const val APP_FILES_MIGRATION_VERSION = 1
        const val APP_FILES_MIGRATION_KEY = "app_files_migration_version"

        /**
         * ⚠️ `config.json` is NOT here, and its absence is a decision (see [getArguments]): it is the
         * one app file the user hand-edits, and `filesDir` is reachable only over adb. It stays in the
         * media tree.
         */
        val APP_FILES_TO_RELOCATE = arrayOf("settings.json", "template.ptp", "autosave.ptp")

        /** The Compose default for the skin pref (`DeviceSkin.AMIGA_DARK.id`), read when the user never
         *  chose one — the shell's own fallback for an unknown id is the same skin (device_skin.h). */
        const val DEFAULT_SKIN_ID = "amiga-2"
    }
}
