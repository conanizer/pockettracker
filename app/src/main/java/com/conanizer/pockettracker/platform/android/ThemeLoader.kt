// ============================================================================
// ThemeLoader.kt — platform/android/ThemeLoader.kt
//
// Loads PNG files from assets/ folder. Android-specific code lives here.
// ============================================================================

package com.conanizer.pockettracker.platform.android

import android.content.Context
import android.graphics.BitmapFactory
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import com.conanizer.pockettracker.R
import com.conanizer.pockettracker.ui.theme.DeviceTheme

object ThemeLoader {

    // Try to load one PNG from assets/; return null if missing or corrupt
    private fun Context.loadBitmap(path: String) = try {
        assets.open(path)
            .use { BitmapFactory.decodeStream(it) }
            ?.asImageBitmap()
    } catch (e: Exception) { null }

    fun loadAmigaTheme(context: Context): DeviceTheme {
        val helvetica = FontFamily(Font(R.font.helvetica_regular))

        return DeviceTheme.AMIGA.copy(
            buttonFont          = helvetica,
            topPanelImage       = context.loadBitmap("themes/amiga/bg_top_panel.png"),
            screenBezelImage    = context.loadBitmap("themes/amiga/bg_screen_bezel.png"),
            brandingPanelImage  = context.loadBitmap("themes/amiga/bg_branding_panel.png"),
            buttonBackingImage  = context.loadBitmap("themes/amiga/bg_button_backing.png"),
            buttonSquareNormal  = context.loadBitmap("themes/amiga/btn_square_normal.png"),
            buttonSquarePressed = context.loadBitmap("themes/amiga/btn_square_pressed.png"),
            buttonWideNormal    = context.loadBitmap("themes/amiga/btn_wide_normal.png"),
            buttonWidePressed   = context.loadBitmap("themes/amiga/btn_wide_pressed.png"),
        )
    }

    // amiga-2 skin: same layout as amiga, but A/B use slightly darker square PNGs
    // (btn_square_*_dark). The old amiga loader above is kept so we can switch back.
    fun loadAmiga2Theme(context: Context): DeviceTheme {
        val helvetica = FontFamily(Font(R.font.helvetica_regular))

        return DeviceTheme.AMIGA.copy(
            buttonFont              = helvetica,
            buttonLabelColor        = Color.White,        // amiga-2: white labels on the darker A/B buttons
            casingFillColor         = Color(0xFF56606C),  // amiga-2: slate fill instead of beige
            topPanelImage           = context.loadBitmap("themes/amiga-2/bg_top_panel.png"),
            screenBezelImage        = context.loadBitmap("themes/amiga-2/bg_screen_bezel.png"),
            brandingPanelImage      = context.loadBitmap("themes/amiga-2/bg_branding_panel.png"),
            buttonBackingImage      = context.loadBitmap("themes/amiga-2/bg_button_backing.png"),
            buttonSquareNormal      = context.loadBitmap("themes/amiga-2/btn_square_normal.png"),
            buttonSquarePressed     = context.loadBitmap("themes/amiga-2/btn_square_pressed.png"),
            buttonSquareNormalDark  = context.loadBitmap("themes/amiga-2/btn_square_normal_dark.png"),
            buttonSquarePressedDark = context.loadBitmap("themes/amiga-2/btn_square_pressed_dark.png"),
            buttonWideNormal        = context.loadBitmap("themes/amiga-2/btn_wide_normal.png"),
            buttonWidePressed       = context.loadBitmap("themes/amiga-2/btn_wide_pressed.png"),
        )
    }
}
