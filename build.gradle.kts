// Top-level build file where you can add configuration options common to all sub-projects/modules.
// ⚠️ No Compose plugin. `apply false` still resolves a plugin's marker artifact, so declaring one
// nothing applies makes the build fetch a compiler plugin it will never run — which matters more than
// usual here, because F-Droid's builder fetches dependencies and fewer artifacts is strictly better
// for that review. Same reason `gradle/libs.versions.toml` carries only what is referenced.
plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.kotlin.android) apply false
}
