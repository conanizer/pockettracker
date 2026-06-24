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
import com.conanizer.pockettracker.ui.theme.DeviceSkin
import com.conanizer.pockettracker.ui.theme.DeviceTheme

object ThemeLoader {

    // Decoded themes cached by skin id. Each skin's ~10 PNGs decode to large ARGB_8888 bitmaps
    // (megabytes each); without this cache, every theme switch re-decoded them and left the old
    // bitmaps for the GC to reclaim later — switching back and forth could spike RAM to ~150MB.
    // With only a handful of skins, caching keeps each one resident exactly once, so switching just
    // swaps references with no re-decode and no growth.
    private val skinCache = HashMap<String, DeviceTheme>()

    // Try to load one PNG from assets/; return null if missing or corrupt
    private fun Context.loadBitmap(path: String) = try {
        assets.open(path)
            .use { BitmapFactory.decodeStream(it) }
            ?.asImageBitmap()
    } catch (e: Exception) { null }

    /**
     * Build (or reuse) a fully-loaded DeviceTheme from a skin descriptor. The skin only chooses the
     * asset folder, label color and casing fill; the structural defaults (bezel thickness, backing
     * corner) come from the AMIGA base. Both amiga folders ship dark A/B square PNGs, so they are
     * always loaded — the A/B buttons in VirtualBtnThemed prefer buttonSquare*Dark and fall back to
     * the regular square PNGs. Results are cached per skin id (see skinCache).
     */
    @Synchronized
    fun loadSkin(context: Context, skin: DeviceSkin): DeviceTheme {
        skinCache[skin.id]?.let { return it }

        val helvetica = FontFamily(Font(R.font.helvetica_regular))
        val f = skin.assetFolder

        val theme = DeviceTheme.AMIGA.copy(
            buttonFont              = helvetica,
            buttonLabelColor        = skin.labelColor,
            casingFillColor         = skin.casingFillColor,
            topPanelImage           = context.loadBitmap("$f/bg_top_panel.png"),
            screenBezelImage        = context.loadBitmap("$f/bg_screen_bezel.png"),
            brandingPanelImage      = context.loadBitmap("$f/bg_branding_panel.png"),
            buttonBackingImage      = context.loadBitmap("$f/bg_button_backing.png"),
            buttonSquareNormal      = context.loadBitmap("$f/btn_square_normal.png"),
            buttonSquarePressed     = context.loadBitmap("$f/btn_square_pressed.png"),
            buttonSquareNormalDark  = context.loadBitmap("$f/btn_square_normal_dark.png"),
            buttonSquarePressedDark = context.loadBitmap("$f/btn_square_pressed_dark.png"),
            buttonWideNormal        = context.loadBitmap("$f/btn_wide_normal.png"),
            buttonWidePressed       = context.loadBitmap("$f/btn_wide_pressed.png"),
        )

        skinCache[skin.id] = theme
        return theme
    }
}
