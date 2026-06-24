package com.conanizer.pockettracker.ui.theme

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.text.font.FontFamily

data class DeviceTheme(

    // Fallback colors (visible immediately, even before PNGs load)
    val casingColor: Color,
    val buttonBackingColor: Color,
    val screenBezelColor: Color,
    val buttonNormalColor: Color,
    val buttonPressedColor: Color,
    val buttonLabelColor: Color,

    // Solid fill shown on the case sides / bottom gap behind the skin panels (portrait-2 layout).
    // Defaults to the original hardcoded beige so existing skins look unchanged.
    val casingFillColor: Color = Color(0xFFBF9971),

    // Bezel border thickness.
    // Use screenBezelThicknessX (in skin X-units) when a bezel PNG is present — this keeps
    // the padding proportional to the scaled PNG border on every device/density.
    // Fall back to screenBezelThicknessDp (absolute dp) for solid-color bezels.
    // Amiga bezel PNG: 24px border on 1080px image = 24/1080 of width = X * (135 * 24/1080) = X * 3
    val screenBezelThicknessX:  Float = 0f,   // >0 → use X-unit scaling; 0 → use dp below
    val screenBezelThicknessDp: Float = 9f,   // used when screenBezelThicknessX == 0

    // Corner radius of the black button backing rectangle
    val buttonBackingCornerDp: Float = 12f,

    // Button label font (null = FontFamily.SansSerif system fallback)
    val buttonFont: FontFamily = FontFamily.Companion.SansSerif,

    // PNG image assets (null = use color fallback)
    val topPanelImage:       ImageBitmap? = null,
    val screenBezelImage:    ImageBitmap? = null,
    val brandingPanelImage:  ImageBitmap? = null,
    val buttonBackingImage:  ImageBitmap? = null,
    val buttonSquareNormal:  ImageBitmap? = null,
    val buttonSquarePressed: ImageBitmap? = null,
    // Optional darker square variant — used for A/B only (e.g. amiga-2 skin).
    // Null → fall back to the regular square images above.
    val buttonSquareNormalDark:  ImageBitmap? = null,
    val buttonSquarePressedDark: ImageBitmap? = null,
    val buttonWideNormal:    ImageBitmap? = null,
    val buttonWidePressed:   ImageBitmap? = null,

    ) {
    companion object {

        // AMIGA theme: warm beige/tan, Commodore Amiga-inspired
        // buttonFont is loaded at runtime by ThemeLoader (res/font/helvetica_regular.otf)
        val AMIGA = DeviceTheme(
            casingColor            = Color(0xFFD4B896),
            buttonBackingColor     = Color(0xFF1A1A1A),
            screenBezelColor       = Color(0xFFB8A07A),
            buttonNormalColor      = Color(0xFFBFA882),
            buttonPressedColor     = Color(0xFF8A7A5A),
            buttonLabelColor       = Color(0xFF0D0D0D),   // near-black
            screenBezelThicknessX  = 3f,  // 24px border / 1080px PNG width * 135 X-units = 3X
            buttonBackingCornerDp  = 16f,
        )

        // DARK theme: current look, always available as safe fallback
        val DARK = DeviceTheme(
            casingColor        = Color(0xFF1A1A1A),
            buttonBackingColor = Color(0xFF0D0D0D),
            screenBezelColor   = Color(0xFF222222),
            buttonNormalColor  = Color(0xFF3D5A80),
            buttonPressedColor = Color(0xFF98C1D9),
            buttonLabelColor   = Color.Companion.White,
        )
    }
}

/**
 * A selectable device skin (theme) for a layout. Pure data — describes WHICH assets/colors fill the
 * layout's fixed slots, not how the layout is structured. ThemeLoader.loadSkin() turns one of these
 * into a fully-loaded DeviceTheme. Add a new skin = add a folder under assets/themes/ + one entry here.
 *
 * soundSet is reserved for future per-theme button sounds; it is unused today (all skins share the
 * res/raw button sounds loaded by ButtonSoundManager).
 */
data class DeviceSkin(
    val id: String,              // persisted key, e.g. "amiga"
    val displayName: String,     // shown in Settings, e.g. "NORMAL"
    val assetFolder: String,     // assets path, e.g. "themes/amiga"
    val labelColor: Color,       // button label color
    val casingFillColor: Color,  // solid fill shown behind/around the skin panels
    val soundSet: String? = null
) {
    companion object {
        // NORM = beige amiga skin (bg r225 g208 b186), near-black labels, dark A/B buttons.
        val AMIGA_NORMAL = DeviceSkin(
            id = "amiga", displayName = "NORM", assetFolder = "themes/amiga",
            labelColor = Color(0xFF0D0D0D), casingFillColor = Color(0xFFE1D0BA)
        )
        // DARK = slate amiga-2 skin, white labels, dark A/B buttons.
        val AMIGA_DARK = DeviceSkin(
            id = "amiga-2", displayName = "DARK", assetFolder = "themes/amiga-2",
            labelColor = Color.White, casingFillColor = Color(0xFF56606C)
        )
        val ALL = listOf(AMIGA_NORMAL, AMIGA_DARK)
        fun byId(id: String?): DeviceSkin? = ALL.firstOrNull { it.id == id }
    }
}