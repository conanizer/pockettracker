import java.io.File
import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
    // Pinned to the Kotlin version via the catalog (was a hard-coded "1.9.0" that only resolved because
    // KGP aligns the compiler plugin) so the serialization plugin can't drift from the Kotlin compiler.
    alias(libs.plugins.kotlin.serialization)
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
        // 920 = v0.9.2; next: 930, … 1000 = 1.0.0). F-Droid's Tags update check and the fastlane
        // changelog filename (changelogs/<versionCode>.txt) both need a literal value, and it
        // outranks any commit-count build ever sideloaded.
        // versionName is bumped by hand per release; tag the matching release in git.
        versionCode = 920
        versionName = "0.9.2"

        // Landscape touch layout is hidden in release (no themed asset for it yet) but kept
        // in debug builds for testing. Gated in MainActivity / SettingsModule on this flag.
        buildConfigField("boolean", "LANDSCAPE_LAYOUT", "false")

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
            // Landscape layout stays available in debug for testing.
            buildConfigField("boolean", "LANDSCAPE_LAYOUT", "true")
        }
        release {
            // R8 + resource shrinking. Smaller dex → faster cold start and less code pinned in
            // RAM on the 1 GB Miyoo. JNI keep rules + kotlinx-serialization keep rules live in
            // proguard-rules.pro. Overlay PNGs are in assets/ (not res/) so resource shrinking
            // leaves them alone.
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
        prefab = true
        compose = true
        buildConfig = true
    }
}

dependencies {
    implementation(libs.oboe)

    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.core.splashscreen)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.ui.text)
    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(platform(libs.androidx.compose.bom))
    androidTestImplementation(libs.androidx.compose.ui.test.junit4)
    debugImplementation(libs.androidx.compose.ui.tooling)
    debugImplementation(libs.androidx.compose.ui.test.manifest)
    implementation(libs.kotlinx.serialization.json)
}