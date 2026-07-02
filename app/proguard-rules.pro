# Add project specific ProGuard rules here.
# You can control the set of applied configuration files using the
# proguardFiles setting in build.gradle.
#
# For more details, see
#   http://developer.android.com/guide/developing/tools/proguard.html

# If your project uses WebView with JS, uncomment the following
# and specify the fully qualified class name to the JavaScript interface
# class:
#-keepclassmembers class fqcn.of.javascript.interface.for.webview {
#   public *;
#}

# Uncomment this to preserve the line number information for
# debugging stack traces.
#-keepattributes SourceFile,LineNumberTable

# If you keep the line number information, uncomment this to
# hide the original source file name.
#-renamesourcefileattribute SourceFile

# Keep JNI-accessible native methods (required when minification is enabled)
-keep class com.conanizer.pockettracker.platform.android.OboeAudioBackend {
    native <methods>;
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