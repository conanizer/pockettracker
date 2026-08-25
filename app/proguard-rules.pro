# ─── The by-name JNI callbacks from native into Kotlin (convergence Phase E) ──────────────────
#
# The shared C++ shell calls these Kotlin methods on MainActivity by name, resolving each with
# GetMethodID (shell/android-main.cpp, shell/midi-out-android.cpp, shell/midi-in-android.cpp).
# ⚠️ No count in prose — this comment carried three different ones at once. The list below IS the
# count, and CI reads it out of this file rather than out of a sentence.
#     onButtonFeedback(IZZIZI)V     — routes a virtual-button press to the sound/haptic managers
#     hasPhysicalGameButtons()Z     — the SOURCE_GAMEPAD check SDL's looser C joystick API cannot make
#     describeInputDevices()L…/String; — the same enumeration as TEXT, printed at boot into the log file
#                                     a user can send. ⚠️ It is a DIAGNOSTIC, which is exactly why it
#                                     must be kept: it is only ever read when something has gone wrong
#                                     on a device nobody here owns, so a release-only rename would be
#                                     discovered by the report coming back empty.
#     midiDeviceCount()I            — ┐ the EXTERNAL MIDI out port (MIDI plan B2b). MidiManager is a
#     midiDeviceName(I)L…/String;   — │ Java-only API and the ONLY route to USB/virtual/BLE MIDI on
#     midiOpenDevice(I)Z            — │ Android, so these five are the whole platform seam. See
#     midiCloseDevice()V            — │ MidiOutManager.kt for why AMidi does not remove the Java half.
#     midiSend(IIII)Z               — ┘
#     midiInDeviceCount()I          — ┐ the MIDI INPUT port (MIDI plan E5), the same seam in the other
#     midiInDeviceName(I)L…/String; — │ direction. ⚠️ Its device list is `outputPortCount > 0`, the
#     midiInOpenDevice(I)Z          — │ OPPOSITE filter — see MidiInManager.kt. The last one is a READ
#     midiInCloseDevice()V          — │ because the native side POLLS this port once a frame rather
#     midiInRead([B)I               — ┘ than being called from the MIDI service's binder thread.
#     safRootCount()I               — ┐ the Storage Access Framework seam (SAF migration P3).
#     safRootInfo(I)L…/String;      — │ ContentResolver/DocumentsContract are Java-only, so these twelve
#     safListChildren(L…;)L…/String;— │ are the whole of it. ⚠️ `safOpenFd` is `pt_fopen`'s resolver:
#     safReadFile(L…;)[B            — │ a rename would make every sample under a granted folder
#     safOpenFd(L…;L…;)I            — │ fail to open, with the browser still listing it — which
#     safCreateDir(L…;L…;)L…/String;— │ reads as a corrupt file rather than as a stripped method.
#     safCreateFile(L…;L…;)L…/String;─┤ The write half (P3b). ⚠️ A rename here is WORSE than a read
#     safDelete(L…;)Z               — │ one: SAVE reports "SAVE FAILED" and the user's edits are still
#     safRename(L…;L…;)L…/String;   — │ only in RAM, so the failure arrives at the moment the work is
#     safMove(L…;L…;L…;)L…/String;  — ┘ least recoverable.
#     safHomeRootId()L…/String;     — ┐ which granted tree the app's seven folders live in (P4a), and
#                                   — │ the way into the first one. ⚠️ A rename of `safHomeRootId`
#     safRequestRoot()Z             — ┘ answers "" for a device that HAS a grant, which sends the seven
#                                   —   folders to whichever tree sorts lowest; a rename of
#                                   —   `safRequestRoot` leaves ADD FOLDER… as a row that does nothing,
#                                   —   which on a fresh install is the whole app.
#     safSetHomeRoot(L…;)Z          —   the user CHOOSING the home tree (SEL+A in the roots directory).
#                                   —   ⚠️ A rename leaves the choice permanently stuck on the first
#                                   —   folder ever granted, which is the state it exists to escape —
#                                   —   and the row still says it worked, because a missing method and
#                                   —   a refusal are the same `false`.
# Every NAME above is part of the ABI and R8 must not rename or strip them. They carry @Keep as well, but
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
    void setLandscapeAllowed(boolean);
    java.lang.String describeInputDevices();
    int midiDeviceCount();
    java.lang.String midiDeviceName(int);
    boolean midiOpenDevice(int);
    void midiCloseDevice();
    boolean midiSend(int, int, int, int);
    int midiInDeviceCount();
    java.lang.String midiInDeviceName(int);
    boolean midiInOpenDevice(int);
    void midiInCloseDevice();
    int midiInRead(byte[]);
    int safRootCount();
    java.lang.String safRootInfo(int);
    java.lang.String safListChildren(java.lang.String);
    byte[] safReadFile(java.lang.String);
    int safOpenFd(java.lang.String, java.lang.String);
    java.lang.String safCreateDir(java.lang.String, java.lang.String);
    java.lang.String safCreateFile(java.lang.String, java.lang.String);
    boolean safDelete(java.lang.String);
    java.lang.String safRename(java.lang.String, java.lang.String);
    java.lang.String safMove(java.lang.String, java.lang.String, java.lang.String);
    java.lang.String safHomeRootId();
    boolean safSetHomeRoot(java.lang.String);
    boolean safForgetRoot(java.lang.String);
    boolean safRequestRoot();
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