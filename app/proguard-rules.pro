# Keep JNI-accessible native methods (required when minification is enabled)
-keep class com.conanizer.pockettracker.platform.android.OboeAudioBackend {
    native <methods>;
}

# The one place C++ calls back INTO Kotlin, and the only by-name JNI lookup in the codebase:
# songcore's render loop reports progress through this interface, resolving the method with
#     GetMethodID(cls, "onProgress", "(F)V")     (native/songcore-jni.cpp)
# so the NAME is part of the ABI and R8 must not touch it.
#
# The default proguard-android-optimize.txt rule
#     -keepclasseswithmembernames,includedescriptorclasses class * { native <methods>; }
# is a trap here: `includedescriptorclasses` keeps the *class names* appearing in a native
# method's signature — AndroidSongcore, native_renderToWav and this interface all kept theirs —
# but it does NOT keep those classes' MEMBERS. So `onProgress` was renamed on the SAM lambda
# RenderController.progressSlice builds, and every render in a release build died with
#     NoSuchMethodError: no non-static method "Lh1/o1;.onProgress(F)V"
# Debug builds don't run R8, which is why only a release APK ever showed it.
#
# Keeping the interface method pins the name for every implementation of it (R8 names virtual
# methods per override group); the second rule states that for the implementors outright.
-keep interface com.conanizer.pockettracker.core.audio.ISongcore$RenderProgress {
    void onProgress(float);
}
-keepclassmembers class * implements com.conanizer.pockettracker.core.audio.ISongcore$RenderProgress {
    void onProgress(float);
}

# Strip debug/verbose/info logging from release builds. Every logger.d / Log.d call site
# (including its string building) is removed by R8, so the scheduling path and UI layer
# don't build emoji strings or cross into liblog on the Miyoo. Log.w / Log.e are kept.
-assumenosideeffects class android.util.Log {
    public static int v(...);
    public static int d(...);
    public static int i(...);
}

# kotlinx.serialization — keep generated serializers for @Serializable model classes
# (project save/load, instrument presets, themes). kotlinx-serialization-json 1.6.0 ships its own
# consumer R8 rules, but keep these explicitly since save/load correctness is critical.
-keepattributes RuntimeVisibleAnnotations,AnnotationDefault

# Keep `Companion` object fields of serializable classes.
-if @kotlinx.serialization.Serializable class **
-keepclassmembers class <1> {
    static <1>$Companion Companion;
}

# Keep `serializer()` on companion objects (both default and named) of serializable classes.
-if @kotlinx.serialization.Serializable class ** {
    static **$* *;
}
-keepclassmembers class <2>$<3> {
    kotlinx.serialization.KSerializer serializer(...);
}

# Keep `INSTANCE.serializer()` of serializable objects.
-if @kotlinx.serialization.Serializable class ** {
    public static ** INSTANCE;
}
-keepclassmembers class <1> {
    public static <1> INSTANCE;
    kotlinx.serialization.KSerializer serializer(...);
}

# Google AutoService (a build-time annotation processor) references javax.annotation.processing.* —
# classes that exist only at compile time, never in the Android runtime. The code is never reached
# on-device, so silence R8's missing-class errors for them.
# (These mirror app/build/outputs/mapping/release/missing_rules.txt.)
-dontwarn javax.annotation.processing.**
-dontwarn com.google.auto.service.**