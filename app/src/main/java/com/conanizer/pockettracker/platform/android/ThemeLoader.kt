// ============================================================================
// ThemeLoader.kt — platform/android/ThemeLoader.kt
//
// Loads PNG files from assets/ folder. Android-specific code lives here.
// ============================================================================

package com.conanizer.pockettracker.platform.android

import android.content.Context
import android.graphics.BitmapFactory
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import com.conanizer.pockettracker.R
import com.conanizer.pockettracker.DeviceTheme

object ThemeLoader {

    fun loadAmigaTheme(context: Context): DeviceTheme {

        // Try to load one PNG from assets/; return null if missing or corrupt
        fun load(path: String) = try {
            context.assets.open(path)
                .use { BitmapFactory.decodeStream(it) }
                ?.asImageBitmap()
        } catch (e: Exception) { null }

        val helvetica = FontFamily(Font(R.font.helvetica_regular))

        return DeviceTheme.AMIGA.copy(
            buttonFont          = helvetica,
            topPanelImage       = load("themes/amiga/bg_top_panel.png"),
            screenBezelImage    = load("themes/amiga/bg_screen_bezel.png"),
            brandingPanelImage  = load("themes/amiga/bg_branding_panel.png"),
            buttonBackingImage  = load("themes/amiga/bg_button_backing.png"),
            buttonSquareNormal  = load("themes/amiga/btn_square_normal.png"),
            buttonSquarePressed = load("themes/amiga/btn_square_pressed.png"),
            buttonWideNormal    = load("themes/amiga/btn_wide_normal.png"),
            buttonWidePressed   = load("themes/amiga/btn_wide_pressed.png"),
        )
    }
}
