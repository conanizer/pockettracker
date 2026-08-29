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
        // 920 = v0.9.2, 930 = v0.9.3, 940 = v0.9.4, 950 = v0.9.5, 960 = v0.9.6;
        // next: 970, … 1000 = 1.0.0).
        // F-Droid's Tags update check and the fastlane changelog filename
        // (changelogs/<versionCode>.txt) both need a literal value, and it outranks any
        // commit-count build ever sideloaded.
        // versionName is bumped by hand per release; tag the matching release in git.
        versionCode = 970
        versionName = "0.9.7"

        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                // Use shared STL to match Oboe's requirements
                //
                // ⚠️ A device with 16 KB memory pages (Android 15+) REFUSES TO LOAD a shared object
                // whose LOAD segments are aligned to 4 KB, so without the second argument the app
                // does not start there at all — and Android 16 already names the three offenders in
                // a dialog on launch. It expands to `-Wl,-z,max-page-size=16384` (NDK r27's
                // android-legacy.toolchain.cmake), and it is spelled as the NDK's own option rather
                // than as raw linker flags so the NDK keeps owning what the value is.
                //
                // Only the libraries CMake builds here need it: `libc++_shared.so` and `liboboe.so`
                // arrive prebuilt and already aligned. The APK's own 16 KB zip alignment is AGP's
                // and needs nothing — `zipalign -c -P 16` is what says so.
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",
                )
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
// The Windows zip, the Linux tarball and the PortMaster zip each stage this set into a `licenses/`
// folder beside the executable. The APK is the one artifact with no "beside the executable", so the
// same set goes into `assets/licenses/`.
//
// They are COPIED at build time rather than checked in under `src/main/assets/`, so
// `docs/licenses/THIRD-PARTY-NOTICES.md` stays the single source of truth and no second copy exists
// to drift from it. Assets — unlike `res/` — are untouched by `isShrinkResources`, so the release APK
// keeps every one.
//
// ⚠️ The wiring is `addGeneratedSourceDirectory`, NOT `sourceSets.assets.srcDir(...)` plus a
// hand-written `dependsOn`: it makes the asset-merge task consume this task's output directory by
// construction, so no ordering can put the merge first.
//
// ⚠️ Nothing READS these files — they are payload, and their whole job is to exist. So a missing one
// breaks no build and throws nothing at run time: it would ship silently, and a green build is no
// evidence at all. The only evidence is the finished APK, which the release build verifies by reading
// every one back out of it and comparing the bytes (a truncated copy unzips just as cleanly).
abstract class StageLicenseAssets : DefaultTask() {
    /**
     * Name the file gets in the artifact -> its repo-relative source path.
     *
     * ⚠️ The destination name is declared rather than taken from the source file, because two of the
     * sources are both called `COPYING` (`native/vendor/ogg/` and `native/vendor/opus/`) and would
     * overwrite each other in one output directory. Repo-relative, not absolute, so the value is a
     * portable task input.
     */
    @get:Input abstract val payload: MapProperty<String, String>

    /** The same sources again, so a change to a licence's CONTENT re-runs the task. */
    @get:InputFiles abstract val sources: ConfigurableFileCollection

    @get:Internal abstract val repoRoot: DirectoryProperty

    /** Becomes `assets/` in the APK, so the files land under `assets/licenses/`. */
    @get:OutputDirectory abstract val outputDir: DirectoryProperty

    @TaskAction
    fun stage() {
        val dir = outputDir.get().asFile.resolve("licenses")
        // Wipe first: an entry dropped from `payload` must leave the APK too, and Gradle's stale-output
        // cleanup does not reach inside a directory this task wrote by hand.
        dir.deleteRecursively()
        dir.mkdirs()
        val root = repoRoot.get().asFile
        payload.get().forEach { (name, rel) -> root.resolve(rel).copyTo(dir.resolve(name), overwrite = true) }
    }
}

// The set every artifact ships. The three desktop packaging scripts stage exactly this, from exactly
// these sources, into a `licenses/` folder beside the executable — keep them in step.
//
// Everything after the first three is cited BY PATH inside THIRD-PARTY-NOTICES.md, and shipping the
// citation without the text leaves a dangling pointer for anyone holding only the artifact — the
// breach the notices file exists to prevent. The two vendored `COPYING` files come from the SOURCE
// THAT WAS COMPILED rather than a copy kept elsewhere in the repo, so the licence that ships is the
// licence of the code that shipped, by construction, and no second copy can drift.
val licensePayload = mapOf(
    "LICENSE"                         to "LICENSE",                                    // GPL-3.0-or-later — PocketTracker's own
    "THIRD-PARTY-NOTICES.md"          to "docs/licenses/THIRD-PARTY-NOTICES.md",       // every statically linked component
    "CREDITS.md"                      to "CREDITS.md",                                 // the Gradle-resolved Android dependencies
    "OFL-1.1-LinuxBiolinum.txt"       to "docs/licenses/OFL-1.1-LinuxBiolinum.txt",    // Linux Biolinum font
    "libogg-COPYING"                  to "native/vendor/ogg/COPYING",                  // BSD-3-Clause
    "libopus-COPYING"                 to "native/vendor/opus/COPYING",                 // BSD-3-Clause
    "libopus-LICENSE_PLEASE_READ.txt" to "native/vendor/opus/LICENSE_PLEASE_READ.txt", // upstream's patent note
)

val stageLicenseAssets = tasks.register<StageLicenseAssets>("stageLicenseAssets") {
    payload.set(licensePayload)
    sources.from(licensePayload.values.map { rootProject.file(it) })
    repoRoot.set(rootProject.layout.projectDirectory)
    outputDir.set(layout.buildDirectory.dir("generated/licenseAssets"))
}

androidComponents {
    onVariants { variant ->
        variant.sources.assets?.addGeneratedSourceDirectory(
            stageLicenseAssets, StageLicenseAssets::outputDir
        )
    }
}
