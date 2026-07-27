# ─── The by-name JNI callbacks from native into Kotlin (convergence Phase E) ──────────────────
#
# The shared C++ shell calls exactly two Kotlin methods on MainActivity by name, resolving each with
# GetMethodID (shell/android-main.cpp):
#     onButtonFeedback(IZZIZI)V     — routes a virtual-button press to the sound/haptic managers
#     hasPhysicalGameButtons()Z     — the SOURCE_GAMEPAD check SDL's looser C joystick API cannot make
# so both NAMES are part of the ABI and R8 must not rename or strip them. They carry @Keep as well, but
# an explicit rule is this project's standing requirement for a native-by-name callback: @Keep alone
# once let a renamed SAM member (onProgress, the old songcore render-progress hook — since deleted with
# the JNI facade) kill every render in a release APK. MainActivity now runs R8 in release, having left
# src/debug in Phase E, so this is load-bearing, not belt-and-braces. Debug never runs R8, which is why
# a slip here is invisible until a release build.
#
# The class itself is a keep root already (it is the launcher activity in the manifest); only its
# members need pinning, hence -keepclassmembers.
-keepclassmembers class com.conanizer.pockettracker.MainActivity {
    void onButtonFeedback(int, boolean, boolean, int, boolean, int);
    boolean hasPhysicalGameButtons();
}

# ─── SDL2's Java glue (convergence plan C1) ───────────────────────────────────────────────────
#
# org.libsdl.app.* is the Java half of SDL's Android support, compiled out of the vendored tree
# (native/vendor/SDL2/android/java). It is JNI on both sides at once, and BOTH directions are
# by-name:
#   - libSDL2.so registers Java_org_libsdl_app_SDLActivity_* natives against these exact classes;
#   - SDL's C calls back INTO them by name through GetStaticMethodID — setActivityTitle,
#     setWindowStyle, getContext, the whole SDLAudioManager/SDLControllerManager surface, and the
#     HIDDevice* stack — none of which any Kotlin in this app references.
#
# So to R8 almost all of it is unreachable code with unreachable members: it would strip or rename
# it and the app would die at launch, in RELEASE ONLY, because debug never runs R8. That is
# precisely the failure that killed every render in the first v0.9.3 APK, and the reason the
# `includedescriptorclasses` note above is written the way it is — keeping a CLASS is not keeping
# its MEMBERS.
#
# ⚠️ It is added HERE, in the commit that vendors SDL, rather than in the phase that first launches
# an SDLActivity: at C1 nothing loads SDL yet, so the breakage would be invisible until C3 and would
# then look like an SDL bug rather than a keep rule nobody wrote. `{ *; }` deliberately — this is
# upstream's code, we do not get to decide which of its members its own C half calls.
-keep class org.libsdl.app.** { *; }
-keepclassmembers class org.libsdl.app.** { *; }

# Strip debug/verbose/info logging from release builds. Every logger.d / Log.d call site
# (including its string building) is removed by R8, so the scheduling path and UI layer
# don't build emoji strings or cross into liblog on the Miyoo. Log.w / Log.e are kept.
-assumenosideeffects class android.util.Log {
    public static int v(...);
    public static int d(...);
    public static int i(...);
}

# kotlinx.serialization's keep rules left with Phase E: the @Serializable models (project save/load,
# instrument presets, themes) are all C++ now (native/songcore/project_io, native/ui/theme_io), so
# there is no generated serializer left in the APK to protect.

# Google AutoService (a build-time annotation processor) references javax.annotation.processing.* —
# classes that exist only at compile time, never in the Android runtime. The code is never reached
# on-device, so silence R8's missing-class errors for them.
# (These mirror app/build/outputs/mapping/release/missing_rules.txt.)
-dontwarn javax.annotation.processing.**
-dontwarn com.google.auto.service.**