package com.conanizer.pockettracker.platform.android

import android.content.Context
import android.content.res.Configuration
import android.graphics.Point
import android.os.Build
import android.util.Log
import android.view.InputDevice
import android.view.WindowManager

class DeviceAdapter(private val context: Context) {

    companion object {
        // Verbose per-scale layout tracing (fires on every layout change) — same TRACE-gate
        // convention as the scheduler/backend layers. Errors/oddities still use Log.d directly.
        private const val TRACE = false
        private const val TAG = "DeviceAdapter"

        const val SCREEN_WIDTH = 640
        const val SCREEN_HEIGHT = 480
        const val BUTTON_PATTERN_WIDTH = 3.4f
        const val BUTTON_PATTERN_HEIGHT = 5.1f
        const val PORTRAIT_PATTERN_WIDTH = 6.8f
        const val PORTRAIT_PATTERN_HEIGHT = 5.1f

        // Portrait spacer height — 0 means screen starts at the very top,
        // maximising the button panel area below.
        const val PORTRAIT_SPACER_HEIGHT = 0

        // Minimum pixels per landscape button panel. If the chosen scale leaves less
        // than this on each side, drop to the next lower scale so buttons are usable.
        const val MIN_BUTTON_PANEL_PX = 150
    }

    /** User-selectable layout modes. Persisted as UI state in PocketTrackerApp. */
    // Three real layouts: FULL (handheld fullscreen), TOUCH_LANDSCAPE, TOUCH_PORTRAIT2 (the retro
    // portrait skin). TOUCH_PORTRAIT is a retired legacy layout kept only for save migration.
    // The portrait skin's NORMAL/DARK look is now a separate DeviceSkin theme, not a layout mode.
    enum class LayoutMode { FULL, TOUCH_PORTRAIT, TOUCH_LANDSCAPE, TOUCH_PORTRAIT2 }

    /** Scaling mode for the game screen. INTEGER = crisp pixel-perfect, BILINEAR = fill screen. */
    enum class ScalingMode { INTEGER, BILINEAR }

    data class LayoutConfig(
        val needsVirtualButtons: Boolean,
        val isLandscape: Boolean,
        val screenScale: Int,
        val scaledScreenWidth: Int,
        val scaledScreenHeight: Int,
        val virtualButtonsHeight: Int = 0,
        val virtualButtonsWidth: Int = 0,
        val deviceWidth: Int = 0,
        val deviceHeight: Int = 0
    )

    private fun logt(msg: String) { if (TRACE) Log.d(TAG, msg) }

    /**
     * Returns (width, height) for the current window/screen in the current orientation.
     * Uses the modern WindowMetrics API on API 30+ to avoid the deprecated
     * Display.getRealSize() which can return stale or incorrect values on Android 15.
     */
    private fun getDeviceDimensions(): Pair<Int, Int> {
        val windowManager = context.getSystemService(Context.WINDOW_SERVICE) as WindowManager
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val bounds = windowManager.currentWindowMetrics.bounds
            Pair(bounds.width(), bounds.height())
        } else {
            @Suppress("DEPRECATION")
            val display = windowManager.defaultDisplay
            val realSize = Point()
            @Suppress("DEPRECATION")
            display.getRealSize(realSize)
            Pair(realSize.x, realSize.y)
        }
    }

    fun hasPhysicalGameButtons(): Boolean {
        val deviceIds = InputDevice.getDeviceIds()
        for (deviceId in deviceIds) {
            val device = InputDevice.getDevice(deviceId) ?: continue

            if (device.name == "Virtual") {
                continue  // emulator UI pseudo-device — not a real controller
            }

            val sources = device.sources

            // Check if device has ANY gamepad/joystick capability
            val hasGamepad = (sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
            val hasJoystick = (sources and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK

            if (hasGamepad || hasJoystick) {
                logt("Found gaming device: ${device.name}")
                return true
            }

            // If device looks like a keyboard BUT has "Xbox" or "Controller" in the name,
            // treat it as a gamepad (this catches Xbox controllers that report as keyboards)
            val isKeyboard = (sources and InputDevice.SOURCE_KEYBOARD) == InputDevice.SOURCE_KEYBOARD
            if (isKeyboard) {
                val lowerName = device.name.lowercase()
                if (lowerName.contains("xbox") ||
                    lowerName.contains("controller") ||
                    lowerName.contains("gamepad")) {
                    logt("Treating as gamepad (keyboard-like): ${device.name}")
                    return true
                }
            }
        }

        return false  // no gamepad/joystick — virtual buttons needed
    }
    private fun isLandscapeOrientation(): Boolean {
        val orientation = context.resources.configuration.orientation
        return orientation == Configuration.ORIENTATION_LANDSCAPE
    }

    /** Auto-detect: FULL when physical game buttons exist, else touch layout by orientation. */
    fun calculateLayout(): LayoutConfig {
        val mode = when {
            hasPhysicalGameButtons() -> LayoutMode.FULL
            isLandscapeOrientation() -> LayoutMode.TOUCH_LANDSCAPE
            else                     -> LayoutMode.TOUCH_PORTRAIT
        }
        return calculateLayout(mode)
    }

    /** Calculate layout for a forced mode (used when user overrides auto-detection). */
    fun calculateLayout(mode: LayoutMode): LayoutConfig {
        val (deviceWidth, deviceHeight) = getDeviceDimensions()
        logt("Device PHYSICAL screen: ${deviceWidth}×${deviceHeight}, mode=$mode")

        return when (mode) {
            LayoutMode.FULL -> {
                val scaleByWidth = deviceWidth / SCREEN_WIDTH
                val scaleByHeight = deviceHeight / SCREEN_HEIGHT
                val finalScale = maxOf(1, minOf(scaleByWidth, scaleByHeight))
                LayoutConfig(
                    needsVirtualButtons = false,
                    isLandscape = true,
                    screenScale = finalScale,
                    scaledScreenWidth = SCREEN_WIDTH * finalScale,
                    scaledScreenHeight = SCREEN_HEIGHT * finalScale,
                    deviceWidth = deviceWidth,
                    deviceHeight = deviceHeight
                )
            }
            LayoutMode.TOUCH_PORTRAIT  -> calculatePortraitLayout(deviceWidth, deviceHeight)
            LayoutMode.TOUCH_LANDSCAPE -> calculateLandscapeLayout(deviceWidth, deviceHeight)
            LayoutMode.TOUCH_PORTRAIT2 -> calculatePortrait2Layout(deviceWidth, deviceHeight)
        }
    }

    private fun calculateLandscapeLayout(deviceWidth: Int, deviceHeight: Int): LayoutConfig {
        logt("=== LANDSCAPE CALCULATION ===")

        for (scale in 4 downTo 1) {
            val scaledScreenWidth = SCREEN_WIDTH * scale
            val scaledScreenHeight = SCREEN_HEIGHT * scale

            logt("")
            logt("Testing scale ${scale}x:")
            logt("  Scaled screen: ${scaledScreenWidth}×${scaledScreenHeight}")

            if (scaledScreenHeight > deviceHeight || scaledScreenWidth > deviceWidth) {
                logt("  ✗ Screen too tall/wide")
                continue
            }

            // Available space for EACH button panel
            // Formula: (deviceWidth - scaledScreenWidth) / 2
            val availableButtonWidth = (deviceWidth - scaledScreenWidth) / 2

            if (availableButtonWidth < MIN_BUTTON_PANEL_PX) {
                logt("  ✗ Button panel too narrow (${availableButtonWidth}px < ${MIN_BUTTON_PANEL_PX}px)")
                continue
            }
            val availableButtonHeight = deviceHeight  // FULL device height

            logt("  Remaining width: ${deviceWidth - scaledScreenWidth}px")
            logt("  Button panel: ${availableButtonWidth}px each")

            val boxRatio = availableButtonWidth.toFloat() / availableButtonHeight.toFloat()
            val patternRatio = BUTTON_PATTERN_WIDTH / BUTTON_PATTERN_HEIGHT

            logt("  Box ratio: ${"%.3f".format(boxRatio)}")
            logt("  Pattern ratio: ${"%.3f".format(patternRatio)}")

            val requiredHeight: Int

            if (boxRatio < patternRatio) {
                // Width limits
                val X = availableButtonWidth / BUTTON_PATTERN_WIDTH
                requiredHeight = (X * BUTTON_PATTERN_HEIGHT).toInt()
                logt("  WIDTH limits → X = ${"%.2f".format(X)}px")
            } else {
                // Height limits
                requiredHeight = availableButtonHeight
                logt("  HEIGHT limits")
            }

            logt("  Need ${requiredHeight}px height (have ${availableButtonHeight}px)")

            if (requiredHeight <= availableButtonHeight) {
                logt("  ✓ FITS! Using ${scale}x")

                return LayoutConfig(
                    needsVirtualButtons = true,
                    isLandscape = true,
                    screenScale = scale,
                    scaledScreenWidth = scaledScreenWidth,
                    scaledScreenHeight = scaledScreenHeight,
                    virtualButtonsHeight = 0,
                    virtualButtonsWidth = availableButtonWidth,  // Just pass the width
                    deviceWidth = deviceWidth,
                    deviceHeight = deviceHeight
                )
            }
        }

        // Fallback
        val remainingWidth = deviceWidth - SCREEN_WIDTH
        val buttonPanelWidth = maxOf(remainingWidth / 2, 100)

        return LayoutConfig(
            needsVirtualButtons = true,
            isLandscape = true,
            screenScale = 1,
            scaledScreenWidth = SCREEN_WIDTH,
            scaledScreenHeight = SCREEN_HEIGHT,
            virtualButtonsHeight = 0,
            virtualButtonsWidth = buttonPanelWidth,
            deviceWidth = deviceWidth,
            deviceHeight = deviceHeight
        )
    }

    private fun calculatePortraitLayout(deviceWidth: Int, deviceHeight: Int): LayoutConfig {
        logt("=== PORTRAIT CALCULATION ===")

        for (scale in 4 downTo 1) {
            val scaledScreenWidth = SCREEN_WIDTH * scale
            val scaledScreenHeight = SCREEN_HEIGHT * scale

            logt("")
            logt("Testing scale ${scale}x:")

            if (scaledScreenWidth > deviceWidth) {
                logt("  ✗ Screen too wide")
                continue
            }

            // Available space for buttons
            // Formula: deviceHeight - PORTRAIT_SPACER_HEIGHT - scaledScreenHeight
            val availableButtonHeight = deviceHeight - PORTRAIT_SPACER_HEIGHT - scaledScreenHeight
            val availableButtonWidth = deviceWidth  // Full width

            logt("  Available for buttons: ${availableButtonWidth}×${availableButtonHeight}")

            val boxRatio = availableButtonWidth.toFloat() / availableButtonHeight.toFloat()
            val patternRatio = PORTRAIT_PATTERN_WIDTH / PORTRAIT_PATTERN_HEIGHT

            logt("  Box ratio: ${"%.3f".format(boxRatio)}")
            logt("  Pattern ratio: ${"%.3f".format(patternRatio)}")

            val requiredHeight: Int

            if (boxRatio < patternRatio) {
                // Width limits
                val X = availableButtonWidth / PORTRAIT_PATTERN_WIDTH
                requiredHeight = (X * PORTRAIT_PATTERN_HEIGHT).toInt()
                logt("  WIDTH limits → X = ${"%.2f".format(X)}px")
                logt("  Required height: ${requiredHeight}px")
            } else {
                // Height limits
                requiredHeight = availableButtonHeight
                logt("  HEIGHT limits")
            }

            if (requiredHeight <= availableButtonHeight) {
                logt("  ✓ FITS! Using ${scale}x")

                return LayoutConfig(
                    needsVirtualButtons = true,
                    isLandscape = false,
                    screenScale = scale,
                    scaledScreenWidth = scaledScreenWidth,
                    scaledScreenHeight = scaledScreenHeight,
                    virtualButtonsHeight = availableButtonHeight,  // Pass AVAILABLE height, not required!
                    virtualButtonsWidth = 0,
                    deviceWidth = deviceWidth,
                    deviceHeight = deviceHeight
                )
            }
        }

        // Fallback
        val availableButtonHeight = deviceHeight - PORTRAIT_SPACER_HEIGHT - SCREEN_HEIGHT

        return LayoutConfig(
            needsVirtualButtons = true,
            isLandscape = false,
            screenScale = 1,
            scaledScreenWidth = SCREEN_WIDTH,
            scaledScreenHeight = SCREEN_HEIGHT,
            virtualButtonsHeight = maxOf(availableButtonHeight, 400),
            virtualButtonsWidth = 0,
            deviceWidth = deviceWidth,
            deviceHeight = deviceHeight
        )
    }

    /**
     * Portrait2 layout: compact 4×4 button grid with 1-unit spacer above.
     * Grid is 4 units wide × 4 units tall, spacer is 1 unit tall → 5 units total height.
     * Screen sits above the button area, centered horizontally.
     */
    private fun calculatePortrait2Layout(deviceWidth: Int, deviceHeight: Int): LayoutConfig {
        logt("=== PORTRAIT2 CALCULATION ===")

        for (scale in 4 downTo 1) {
            val scaledScreenWidth = SCREEN_WIDTH * scale
            val scaledScreenHeight = SCREEN_HEIGHT * scale

            if (scaledScreenWidth > deviceWidth) {
                logt("  ✗ Scale ${scale}x: screen too wide")
                continue
            }

            // Button area with outer spacers:
            // Width:  4.5X (2×0.1X outer + 4 cells + 3×0.1X inner col spacers)
            // Height: 5.2X (0.8X spacer above + 4 cells + 3×0.1X row spacers + 0.1X bottom)
            val remainingHeight = deviceHeight - scaledScreenHeight
            if (remainingHeight <= 0) {
                logt("  ✗ Scale ${scale}x: no space for buttons")
                continue
            }

            val xByWidth  = (deviceWidth / 4.5f).toInt()
            val xByHeight = (remainingHeight / 5.2f).toInt()
            val X = minOf(xByWidth, xByHeight)

            if (X <= 0) continue

            val buttonAreaHeight = (X * 5.2f).toInt()  // 0.8X spacer + 4 button rows + 3×0.1X row spacers + 0.1X bottom
            val buttonAreaWidth  = (X * 4.5f).toInt()  // 2×0.1X outer + 4 button cols + 3×0.1X col spacers

            logt("  ✓ Scale ${scale}x: X=$X, btnArea=${buttonAreaWidth}×${buttonAreaHeight}")

            return LayoutConfig(
                needsVirtualButtons = true,
                isLandscape = false,
                screenScale = scale,
                scaledScreenWidth = scaledScreenWidth,
                scaledScreenHeight = scaledScreenHeight,
                virtualButtonsHeight = buttonAreaHeight,
                virtualButtonsWidth = buttonAreaWidth,
                deviceWidth = deviceWidth,
                deviceHeight = deviceHeight
            )
        }

        // Fallback
        val X = minOf((deviceWidth / 4.5f).toInt(), 80)
        return LayoutConfig(
            needsVirtualButtons = true,
            isLandscape = false,
            screenScale = 1,
            scaledScreenWidth = SCREEN_WIDTH,
            scaledScreenHeight = SCREEN_HEIGHT,
            virtualButtonsHeight = (X * 5.2f).toInt(),
            virtualButtonsWidth = (X * 4.5f).toInt(),
            deviceWidth = deviceWidth,
            deviceHeight = deviceHeight
        )
    }

    fun getConfigDescription(config: LayoutConfig): String {
        val scenario = when {
            !config.needsVirtualButtons -> "Gaming Handheld"
            config.isLandscape -> "Touchscreen LANDSCAPE"
            else -> "Touchscreen PORTRAIT"
        }

        return """
            $scenario
            Orientation: ${if (config.isLandscape) "Landscape" else "Portrait"}
            Scale: ${config.screenScale}x
            Screen: ${config.scaledScreenWidth}×${config.scaledScreenHeight}
            Virtual Buttons: ${if (config.needsVirtualButtons) "YES" else "NO"}
            Buttons: ${if (config.isLandscape) "${config.virtualButtonsWidth}px each" else "${config.virtualButtonsHeight}px"}
            Device: ${config.deviceWidth}×${config.deviceHeight}
        """.trimIndent()
    }
}