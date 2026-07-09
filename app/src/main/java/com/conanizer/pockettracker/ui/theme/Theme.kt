package com.conanizer.pockettracker.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable

// Fixed dark Material scheme. The app's real theming is the .ptt AppTheme system (pixel-art
// palettes drawn by the renderer); Material only backs the few material3 widgets around it
// (virtual-button Text labels, Surface). This replaced the stock Android Studio template,
// whose dynamicColor=true restyled those widgets from the DEVICE WALLPAPER on Android 12+ —
// a device-varying look for an app whose identity is a fixed pixel-perfect one — plus dead
// purple palettes and Typography defaults.
@Composable
fun PockettrackerTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = darkColorScheme(),
        content = content
    )
}
