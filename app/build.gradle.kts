import java.io.File
import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    // Convergence Phase E deleted the Compose UI and every @Serializable model along with the Kotlin
    // tracker, so the kotlin.compose and kotlin.serialization compiler plugins are both gone. The
    // surviving Kotlin (MainActivity + the two feedback managers) is plain Android View/JNI glue.
}

android {
    namespace = "com.conanizer.pockettracker"
    compileSdk {
        version = release(36)
    }
    // Must match `ndk:` in the fdroiddata recipe (metadata/com.conanizer.pockettracker.yml) —
    // F-Droid's offline builder provisions exactly this version. Without the pin, AGP silently
    // resolves its own default NDK, which changes with AGP upgrades.
    ndkVersion = "27.0.12077973"

    // Release signing reads a gitignored keystore.properties from the repo root. When it's
    // absent (fresh clone, CI without secrets) the release build falls back to the debug key,
    // so the build never breaks — see signingConfigs / buildTypes.release below.
    val keystorePropertiesFile = rootProject.file("keystore.properties")
    val keystoreProperties = Properties().apply {
        if (keystorePropertiesFile.exists()) keystorePropertiesFile.inputStream().use { load(it) }
    }

    defaultConfig {
        applicationId = "com.conanizer.pockettracker"
        minSdk = 26
        targetSdk = 34
        // versionCode is hardcoded per release (900 = v0.9.0, 910 = v0.9.1 [F-Droid hotfix],
        // 920 = v0.9.2, 930 = v0.9.3; next: 940, … 1000 = 1.0.0). F-Droid's Tags update check and
        // the fastlane changelog filename (changelogs/<versionCode>.txt) both need a literal value,
        // and it outranks any commit-count build ever sideloaded.
        // versionName is bumped by hand per release; tag the matching release in git.
        versionCode = 930
        versionName = "0.9.3"

        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                // Use shared STL to match Oboe's requirements
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }
    }

    externalNativeBuild {
        cmake {
            // Shared C++ core lives at repo-root /native (moved from app/src/main/cpp,
            // Linux-port plan §4.2/§6). file() is relative to this module dir (app/).
            path = file("../native/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    // SDL2's Java half, compiled straight out of the vendored source tree (convergence plan C1).
    //
    // ⚠️ NOT copied into app/src/main/java, and that is the whole point. SDL's Android support is
    // half C and half Java: SDLActivity hardcodes the SDL version and refuses to start against a
    // libSDL2.so reporting a different one. Pointing the sourceSet at the vendored tree means the
    // Java compiled here and the C compiled by native/CMakeLists.txt are the same release BY
    // CONSTRUCTION — one directory, updated only by native/vendor/revendor-sdl2.sh. A copy would
    // reintroduce exactly the drift the version check exists to catch. (native/CMakeLists.txt
    // asserts the two halves agree at configure time anyway — search SDL_VERSION_LOCK — because a
    // structural guarantee is worth having a guard on.)
    sourceSets {
        getByName("main") {
            java.srcDir("../native/vendor/SDL2/android/java")
        }
    }

    // Strip AGP's Google "dependency metadata" blob from the APK signing block. F-Droid's
    // scanner rejects any extra signing block ("Found extra signing block 'Dependency
    // metadata'"), so its build fails without this. No runtime effect; also trims the APK.
    dependenciesInfo {
        includeInApk = false
        includeInBundle = false
    }

    signingConfigs {
        // Only declared when keystore.properties exists; otherwise the release build
        // below stays on the debug key.
        if (keystorePropertiesFile.exists()) {
            create("release") {
                storeFile = rootProject.file(keystoreProperties.getProperty("storeFile"))
                storePassword = keystoreProperties.getProperty("storePassword")
                keyAlias = keystoreProperties.getProperty("keyAlias")
                keyPassword = keystoreProperties.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        debug {
            // A SEPARATE PACKAGE (`.debug`) so a dev build coexists with — and does not evict — a
            // real install being used to make music. Debug and release are signed with different
            // keys, so a debug APK cannot install over a release one; without the suffix the only way
            // to test on a device with a real install is to uninstall it, taking its SharedPreferences
            // with it. Songs are NOT package-scoped (they live in public external storage at
            // /storage/emulated/0/Documents/PocketTracker), so both packages open the SAME projects.
            // ⚠️ Each package needs its OWN MANAGE_EXTERNAL_STORAGE grant (permissions are per-package),
            // or its file browser comes up empty for a reason unrelated to std::filesystem.
            applicationIdSuffix = ".debug"
        }
        release {
            // R8 + resource shrinking. Smaller dex → faster cold start and less code pinned in
            // RAM on the 1 GB Miyoo. The JNI keep rules (MainActivity's onButtonFeedback /
            // hasPhysicalGameButtons, called by name from native) live in proguard-rules.pro.
            // Overlay PNGs are in assets/ (not res/) so resource shrinking leaves them alone.
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            // Real release key when keystore.properties is present; debug key otherwise so
            // the build never breaks without the secrets.
            signingConfig = if (keystorePropertiesFile.exists())
                signingConfigs.getByName("release")
            else
                signingConfigs.getByName("debug")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    kotlinOptions {
        jvmTarget = "11"
    }
    buildFeatures {
        prefab = true          // Oboe arrives as a Prefab package
        // compose and buildConfig both gone with Phase E: no Compose UI, and no surviving Kotlin
        // reads BuildConfig (the debug/release split the C++ side needs comes from NDEBUG, not here).
    }
}

// Convergence Phase E: the dependency list is now the whole cost of the Kotlin shim. Oboe (the audio
// backend, via Prefab), core-ktx (WindowCompat), and the splash-screen compat lib are all that the
// surviving MainActivity + feedback managers use. Compose (BOM, ui, material3, activity-compose),
// lifecycle-runtime, kotlinx-serialization and the JUnit/Espresso/Compose test stack all left with
// the ~15k lines of Kotlin they supported — the APK-size win the convergence plan projected.
dependencies {
    implementation(libs.oboe)
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.core.splashscreen)
}

// ─── The licence payload that ships INSIDE the APK ───────────────────────────────────────────────
//
// PocketTracker is GPL-3.0-or-later and statically links a dozen third-party components, so their
// notices have to travel with the BINARY and not merely with the source tree that built it — GPL-3.0
// §4 and BSD-3-Clause both word the obligation around what the recipient of the ARTIFACT receives.
// The Windows zip, the Linux tarball and the PortMaster zip each stage these three files into a
// `licenses/` folder beside the executable. The APK is the one artifact that has no "beside the
// executable", and it carried none of them.
//
// They are COPIED from the repo root at build time rather than checked in under `src/main/assets/`,
// so `licenses/THIRD-PARTY-NOTICES.md` stays the single source of truth and no second copy exists to
// drift from it. Assets — unlike `res/` — are untouched by `isShrinkResources`, so the release APK
// keeps all three.
//
// ⚠️ The wiring is `addGeneratedSourceDirectory`, NOT `sourceSets.assets.srcDir(...)` plus a
// hand-written `dependsOn`: it makes the asset-merge task consume this task's output directory by
// construction, so no ordering can put the merge first.
//
// ⚠️ Nothing READS these files — they are payload, and their whole job is to exist. So a missing one
// breaks no build and throws nothing at run time: it would ship silently, and a green build is no
// evidence at all. The only evidence is the finished APK, which the release build verifies by reading
// all three back out of it and comparing the bytes (a truncated copy unzips just as cleanly).
abstract class StageLicenseAssets : DefaultTask() {
    /** The repo-root files to ship, taken verbatim. */
    @get:InputFiles abstract val notices: ConfigurableFileCollection

    /** Becomes `assets/` in the APK, so the files land under `assets/licenses/`. */
    @get:OutputDirectory abstract val outputDir: DirectoryProperty

    @TaskAction
    fun stage() {
        val dir = outputDir.get().asFile.resolve("licenses")
        // Wipe first: an entry dropped from `notices` must leave the APK too, and Gradle's stale-output
        // cleanup does not reach inside a directory this task wrote by hand.
        dir.deleteRecursively()
        dir.mkdirs()
        notices.forEach { src -> src.copyTo(dir.resolve(src.name), overwrite = true) }
    }
}

val stageLicenseAssets = tasks.register<StageLicenseAssets>("stageLicenseAssets") {
    notices.from(
        rootProject.file("LICENSE"),                         // GPL-3.0-or-later — PocketTracker's own
        rootProject.file("licenses/THIRD-PARTY-NOTICES.md"), // every statically linked component
        rootProject.file("CREDITS.md"),                      // the Gradle-resolved Android dependencies
        // The notices file cites this one by path for the Linux Biolinum font, so shipping the
        // citation without the text would leave a dangling pointer for anyone holding only the APK.
        rootProject.file("licenses/OFL-1.1-LinuxBiolinum.txt"),
    )
    outputDir.set(layout.buildDirectory.dir("generated/licenseAssets"))
}

androidComponents {
    onVariants { variant ->
        variant.sources.assets?.addGeneratedSourceDirectory(
            stageLicenseAssets, StageLicenseAssets::outputDir
        )
    }
}
