package com.conanizer.pockettracker

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.provider.Settings
import android.util.Log
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import androidx.annotation.Keep
import androidx.core.splashscreen.SplashScreen.Companion.installSplashScreen
import androidx.core.view.WindowCompat
import com.conanizer.pockettracker.input.VirtualButton
import com.conanizer.pockettracker.platform.android.ButtonHapticManager
import com.conanizer.pockettracker.platform.android.ButtonSoundManager
import org.json.JSONObject
import org.libsdl.app.SDLActivity
import java.io.File

/**
 * The SDL app, as a second activity beside the Compose one. Convergence C3.
 *
 * ⚠️ **THIS FILE IS IN `src/debug/`, WHICH IS THE WHOLE OF HOW RELEASE STAYS UNTOUCHED.** The
 * convergence plan requires the APK to carry BOTH UIs for the length of phases C and D: the moment
 * the app becomes an `SDLActivity`, Compose is gone and touch does not work — and touch is Phase D.
 * So release ships `MainActivity` (Compose) until Phase D is device-proven, and this is what we
 * develop against meanwhile. Being in the debug source set means the release APK does not contain
 * this class, its manifest entry, or its launcher icon; there is no flag to remember to flip and no
 * way for a release build to reach it. Phase E deletes `MainActivity` and this moves to `src/main/`.
 *
 * It appears as a SECOND launcher icon on a debug install ("PT (SDL)"), which is deliberate — the
 * alternative is `adb shell am start` every time, and a device round trip is the expensive part of
 * this phase.
 *
 * ⚠️ **C4 (2026-07-20) added the three things C3 deliberately left out**, and two of them are not
 * here at all — which is the point. The permission request and the system bars are Java's, so they
 * are below. The lifecycle is NOT: the autosave/settings flush is a `SDL_AddEventWatch` watcher in
 * `shell/app.cpp`, shared with every other platform, because `SDL_APP_WILLENTERBACKGROUND` turns out
 * to fire on the NATIVE thread inside the frame loop's own `SDL_PollEvent` — not on this thread, as
 * the plan assumed. The back button is likewise split: the hint is armed in `shell/android-main.cpp`
 * and the key is mapped in `shell/sdl-input.cpp`. Nothing about the lifecycle needs Kotlin.
 */
class SdlActivity : SDLActivity() {

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
     */
    override fun getArguments(): Array<String> = arrayOf(appRoot())

    private fun appRoot(): String =
        File(
            Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS),
            "PocketTracker"
        ).absolutePath

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
            Log.i(TAG, "MANAGE_EXTERNAL_STORAGE granted=$granted  appRoot=${appRoot()}")
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
        // SDL thread, which runs `SDL_main`, which calls `load_settings()`. Anything this writes after
        // that point is a file the app has already read past.
        importLegacySettings()

        // The feedback managers, before super.onCreate() starts the SDL thread that calls back into
        // onButtonFeedback. Constructing the SoundPool early lets it load the click samples off the
        // critical path, so the first tap is not silent while they decode.
        buttonSound  = ButtonSoundManager(this)
        buttonHaptic = ButtonHapticManager(this)

        super.onCreate(savedInstanceState)
        hideSystemBars()
    }

    override fun onDestroy() {
        buttonSound?.release()
        buttonSound  = null
        buttonHaptic = null
        super.onDestroy()
    }

    /**
     * **Called from native (`shell/android-main.cpp`, on the SDL thread) on every virtual-button press
     * and release** — the one outward JNI hook the convergence plan's Phase-E table names. The shared
     * touch layer (`sdl-touch.cpp`) owns the DECISION to fire and passes the live BTN SOUND / BTN VIBRO
     * scalars across; this only routes them to the two managers, which are unchanged from the Compose
     * app. Kept resolvable by name across R8 with `@Keep` (belt-and-braces: this class is debug-only
     * today, and R8 does not run on debug, but Phase E moves it to `src/main`).
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
        val vb = VirtualButton.values().getOrNull(button) ?: return

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
     * migration can ever fire. Phase D adds LAYOUT, SKIN and OVERLAY (all three are INDICES into lists
     * that do not exist yet — see the note in `settings_store.cpp`), so a second pass is already known
     * to be coming, and the "absent file" guard would have made it unreachable before it was written.
     * A version counter costs one integer and makes Phase D a `if (version < 2)` arm.
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

        val target = File(appRoot(), "settings.json")

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
            // LAYOUT / SKIN / OVERLAY are absent by design — they are indices into Phase D's lists.
            json.put("buttonSound",       prefs.getBoolean("button_sound", true))
            json.put("buttonSoundVolume", prefs.getInt("button_sound_volume", 0x80))
            json.put("buttonVibro",       prefs.getBoolean("button_vibro", true))
            json.put("vibroPower",        prefs.getInt("vibro_power", 255))
            json.put("overlayStrength",   prefs.getInt("overlay_strength", 128))

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
         * **v2 (Phase D, expected)** — LAYOUT, SKIN and OVERLAY selection, once the lists those
         * indices point into exist and a stored name can be resolved to one.
         */
        const val SETTINGS_IMPORT_VERSION = 1
        const val IMPORT_VERSION_KEY = "settings_import_version"
    }
}
