import java.io.File

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
    id("org.jetbrains.kotlin.plugin.serialization") version "1.9.0"
}

fun String.runCommand(workingDir: File = rootDir): String? =
    ProcessBuilder(split(" "))
        .directory(workingDir)
        .redirectErrorStream(true)
        .start()
        .inputStream
        .bufferedReader()
        .readText()
        .takeIf { it.isNotBlank() }

android {
    namespace = "com.conanizer.pockettracker"
    compileSdk {
        version = release(36)
    }
    val gitCommitCount = "git rev-list --count HEAD".runCommand()?.trim()?.toIntOrNull() ?: 1

    defaultConfig {
        applicationId = "com.conanizer.pockettracker"
        minSdk = 26
        targetSdk = 34
        // versionCode = git commit count (monotonic, drives update ordering).
        // versionName is bumped by hand per release; tag the matching release in git.
        versionCode = gitCommitCount
        versionName = "0.9.0"

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
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
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
            signingConfig = signingConfigs.getByName("debug")
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
    }
}

dependencies {
    //noinspection UseTomlInstead
    implementation("com.google.oboe:oboe:1.10.0")

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
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.6.0")
}