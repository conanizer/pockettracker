import java.io.File
import java.util.Properties

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
    buildFeatures {
        prefab = true
    }

    val gitCommitCount = "git rev-list --count HEAD".runCommand()?.trim()?.toIntOrNull() ?: 1
    val gitShortHash = "git rev-parse --short HEAD".runCommand()?.trim() ?: "unknown"

    val localProps = Properties()
    val localPropsFile = rootProject.file("local.properties")
    if (localPropsFile.exists()) localProps.load(localPropsFile.inputStream())
    val githubToken: String = localProps.getProperty("github.token", "")

    defaultConfig {
        applicationId = "com.conanizer.pockettracker"
        minSdk = 26
        targetSdk = 34
        versionCode = gitCommitCount
        versionName = "0.9.$gitCommitCount ($gitShortHash)"

        buildConfigField("String", "GITHUB_REPO_OWNER", "\"conanizer\"")
        buildConfigField("String", "GITHUB_REPO_NAME", "\"pockettracker.\"")

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

    buildFeatures {
        prefab = true
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        debug {
            buildConfigField("String", "GITHUB_TOKEN", "\"$githubToken\"")
        }
        release {
            buildConfigField("String", "GITHUB_TOKEN", "\"\"")  // No token in release builds
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
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
        compose = true
        buildConfig = true
    }
}

dependencies {
    //noinspection UseTomlInstead
    implementation("com.google.oboe:oboe:1.10.0")

    val acraVersion = "5.11.3"
    implementation("ch.acra:acra-core:$acraVersion")
    implementation(libs.androidx.core.ktx)
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