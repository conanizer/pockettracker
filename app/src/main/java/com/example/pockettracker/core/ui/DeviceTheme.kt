// ============================================================================
// DeviceTheme.kt — core/ui/DeviceTheme.kt
//
// Holds ALL visual settings for the device skin (PORTRAIT2 layout).
// No Android imports — platform-agnostic for future Linux port.
// ============================================================================

package com.example.pockettracker.core.ui

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.text.font.FontFamily

data class DeviceTheme(

    // Fallback colors (visible immediately, even before PNGs load)
    val casingColor:        Color,
    val buttonBackingColor: Color,
    val screenBezelColor:   Color,
    val buttonNormalColor:  Color,
    val buttonPressedColor: Color,
    val buttonLabelColor:   Color,

    // Bezel border thickness in dp (24px / ~2.67 density ≈ 9dp at 1080p)
    val screenBezelThicknessDp: Float = 9f,

    // Corner radius of the black button backing rectangle
    val buttonBackingCornerDp: Float = 12f,

    // Button label font (null = FontFamily.SansSerif system fallback)
    val buttonFont: FontFamily = FontFamily.SansSerif,

    // PNG image assets (null = use color fallback)
    val topPanelImage:       ImageBitmap? = null,
    val screenBezelImage:    ImageBitmap? = null,
    val brandingPanelImage:  ImageBitmap? = null,
    val buttonBackingImage:  ImageBitmap? = null,
    val buttonSquareNormal:  ImageBitmap? = null,
    val buttonSquarePressed: ImageBitmap? = null,
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
            screenBezelThicknessDp = 9f,
            buttonBackingCornerDp  = 16f,
        )

        // DARK theme: current look, always available as safe fallback
        val DARK = DeviceTheme(
            casingColor        = Color(0xFF1A1A1A),
            buttonBackingColor = Color(0xFF0D0D0D),
            screenBezelColor   = Color(0xFF222222),
            buttonNormalColor  = Color(0xFF3D5A80),
            buttonPressedColor = Color(0xFF98C1D9),
            buttonLabelColor   = Color.White,
        )
    }
}
