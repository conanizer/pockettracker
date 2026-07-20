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
import androidx.core.splashscreen.SplashScreen.Companion.installSplashScreen
import androidx.core.view.WindowCompat
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

        super.onCreate(savedInstanceState)
        hideSystemBars()
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
    }
}
