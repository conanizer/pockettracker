package com.conanizer.pockettracker

import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.util.Log
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
 * What this class does NOT do is as important as what it does. There is no permission request, no
 * lifecycle handling and no back-button mapping here: those are **C4**, they are subtle
 * (`SDL_AddEventWatch` rather than the frame loop, because SDL freezes the native thread on pause),
 * and writing a half version now would be a thing to un-write. This activity boots the tracker and
 * gets out of the way.
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

    override fun onCreate(savedInstanceState: Bundle?) {
        // ⚠️ Logged, not requested, and not fixed. Without MANAGE_EXTERNAL_STORAGE the C++
        // `StdFileSystem` cannot see /storage/emulated/0 and the file browser comes up EMPTY — which
        // looks exactly like "C5's spike says std::filesystem does not work on Android", the single
        // most important open question this phase answers. A wrong answer there would be recorded as
        // an architectural fact and cost `AndroidFileSystem.kt` its deletion in Phase E.
        //
        // So the state is printed beside the result, which is this project's standing rule for
        // instruments: the permission is granted by the Compose activity (or by hand in Settings),
        // and C4 moves the request here. Until then, read this line before believing an empty browser.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val granted = Environment.isExternalStorageManager()
            Log.i(TAG, "MANAGE_EXTERNAL_STORAGE granted=$granted  appRoot=${appRoot()}")
            if (!granted) {
                Log.w(
                    TAG,
                    "storage permission NOT granted - the file browser will be empty and that is " +
                        "NOT a C5 result. Grant it (launch the Compose activity once, or " +
                        "Settings > Apps > PocketTracker > All files access) and relaunch."
                )
            }
        }
        super.onCreate(savedInstanceState)
    }

    private companion object {
        const val TAG = "PocketTrackerSDL"
    }
}
