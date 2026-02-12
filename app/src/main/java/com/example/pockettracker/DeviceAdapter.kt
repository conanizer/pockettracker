// ============================================================================
// DeviceAdapter.kt - CORRECT with 200px portrait spacer
// ============================================================================

package com.example.pockettracker

import android.content.Context
import android.content.res.Configuration
import android.view.InputDevice
import android.view.KeyEvent
import android.graphics.Point
import android.view.WindowManager

class DeviceAdapter(private val context: Context) {

    companion object {
        const val SCREEN_WIDTH = 640
        const val SCREEN_HEIGHT = 480
        const val BUTTON_PATTERN_WIDTH = 3.4f
        const val BUTTON_PATTERN_HEIGHT = 5.1f
        const val PORTRAIT_PATTERN_WIDTH = 6.8f
        const val PORTRAIT_PATTERN_HEIGHT = 5.1f

        // Portrait spacer height (fixed)
        const val PORTRAIT_SPACER_HEIGHT = 200
    }

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

    private fun hasPhysicalGameButtons(): Boolean {
        val deviceIds = InputDevice.getDeviceIds()
        for (deviceId in deviceIds) {
            val device = InputDevice.getDevice(deviceId) ?: continue

            if (device.name == "Virtual") {
                android.util.Log.d("DeviceAdapter", "Skipping: Virtual (emulator UI)")
                continue
            }

            val sources = device.sources

            // Check if device has ANY gamepad/joystick capability
            val hasGamepad = (sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
            val hasJoystick = (sources and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK

            if (hasGamepad || hasJoystick) {
                android.util.Log.d("DeviceAdapter", "Found gaming device: ${device.name}")
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
                    android.util.Log.d("DeviceAdapter", "Treating as gamepad (keyboard-like): ${device.name}")
                    return true
                }
            }
        }

        android.util.Log.d("DeviceAdapter", "No gaming controls found - virtual buttons needed")
        return false
    }
    private fun isLandscapeOrientation(): Boolean {
        val orientation = context.resources.configuration.orientation
        val isLandscape = orientation == Configuration.ORIENTATION_LANDSCAPE
        android.util.Log.d("DeviceAdapter", "Orientation: ${if (isLandscape) "LANDSCAPE" else "PORTRAIT"}")
        return isLandscape
    }

    fun calculateLayout(): LayoutConfig {
        val hasButtons = hasPhysicalGameButtons()

        // Get the real display size (not affected by system UI)
        val windowManager = context.getSystemService(Context.WINDOW_SERVICE) as WindowManager
        val display = windowManager.defaultDisplay
        val realSize = Point()
        display.getRealSize(realSize)

        val deviceWidth = realSize.x
        val deviceHeight = realSize.y

        // Alternative: Use DisplayMetrics with a different method
        // val displayMetrics = DisplayMetrics()
        // display.getRealMetrics(displayMetrics)
        // val deviceWidth = displayMetrics.widthPixels
        // val deviceHeight = displayMetrics.heightPixels

        android.util.Log.d("DeviceAdapter", "Device PHYSICAL screen: ${deviceWidth}×${deviceHeight}")

        val needsVirtual = !hasButtons
        val isLandscape = if (needsVirtual) isLandscapeOrientation() else true

        if (!needsVirtual) {
            val scaleByWidth = deviceWidth / SCREEN_WIDTH
            val scaleByHeight = deviceHeight / SCREEN_HEIGHT
            val scale = minOf(scaleByWidth, scaleByHeight)
            val finalScale = maxOf(1, scale)

            android.util.Log.d("DeviceAdapter", "Mode: FULL SCREEN")
            android.util.Log.d("DeviceAdapter", "Scale factors: width=${scaleByWidth}, height=${scaleByHeight}")

            return LayoutConfig(
                needsVirtualButtons = false,
                isLandscape = true,
                screenScale = finalScale,
                scaledScreenWidth = SCREEN_WIDTH * finalScale,
                scaledScreenHeight = SCREEN_HEIGHT * finalScale,
                virtualButtonsHeight = 0,
                virtualButtonsWidth = 0,
                deviceWidth = deviceWidth,
                deviceHeight = deviceHeight
            )
        }

        // Rest of the function remains the same...

        if (isLandscape) {
            return calculateLandscapeLayout(deviceWidth, deviceHeight)
        } else {
            return calculatePortraitLayout(deviceWidth, deviceHeight)
        }
    }

    private fun calculateLandscapeLayout(deviceWidth: Int, deviceHeight: Int): LayoutConfig {
        android.util.Log.d("DeviceAdapter", "=== LANDSCAPE CALCULATION ===")

        for (scale in 4 downTo 1) {
            val scaledScreenWidth = SCREEN_WIDTH * scale
            val scaledScreenHeight = SCREEN_HEIGHT * scale

            android.util.Log.d("DeviceAdapter", "")
            android.util.Log.d("DeviceAdapter", "Testing scale ${scale}x:")
            android.util.Log.d("DeviceAdapter", "  Scaled screen: ${scaledScreenWidth}×${scaledScreenHeight}")

            if (scaledScreenHeight > deviceHeight) {
                android.util.Log.d("DeviceAdapter", "  ✗ Screen too tall")
                continue
            }

            // Available space for EACH button panel
            // Formula: (deviceWidth - scaledScreenWidth) / 2
            val availableButtonWidth = (deviceWidth - scaledScreenWidth) / 2
            val availableButtonHeight = deviceHeight  // FULL device height

            android.util.Log.d("DeviceAdapter", "  Remaining width: ${deviceWidth - scaledScreenWidth}px")
            android.util.Log.d("DeviceAdapter", "  Button panel: ${availableButtonWidth}px each")

            val boxRatio = availableButtonWidth.toFloat() / availableButtonHeight.toFloat()
            val patternRatio = BUTTON_PATTERN_WIDTH / BUTTON_PATTERN_HEIGHT

            android.util.Log.d("DeviceAdapter", "  Box ratio: ${"%.3f".format(boxRatio)}")
            android.util.Log.d("DeviceAdapter", "  Pattern ratio: ${"%.3f".format(patternRatio)}")

            val requiredHeight: Int

            if (boxRatio < patternRatio) {
                // Width limits
                val X = availableButtonWidth / BUTTON_PATTERN_WIDTH
                requiredHeight = (X * BUTTON_PATTERN_HEIGHT).toInt()
                android.util.Log.d("DeviceAdapter", "  WIDTH limits → X = ${"%.2f".format(X)}px")
            } else {
                // Height limits
                requiredHeight = availableButtonHeight
                android.util.Log.d("DeviceAdapter", "  HEIGHT limits")
            }

            android.util.Log.d("DeviceAdapter", "  Need ${requiredHeight}px height (have ${availableButtonHeight}px)")

            if (requiredHeight <= availableButtonHeight) {
                android.util.Log.d("DeviceAdapter", "  ✓ FITS! Using ${scale}x")

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
        android.util.Log.d("DeviceAdapter", "=== PORTRAIT CALCULATION ===")

        for (scale in 4 downTo 1) {
            val scaledScreenWidth = SCREEN_WIDTH * scale
            val scaledScreenHeight = SCREEN_HEIGHT * scale

            android.util.Log.d("DeviceAdapter", "")
            android.util.Log.d("DeviceAdapter", "Testing scale ${scale}x:")

            if (scaledScreenWidth > deviceWidth) {
                android.util.Log.d("DeviceAdapter", "  ✗ Screen too wide")
                continue
            }

            // Available space for buttons
            // Formula: deviceHeight - PORTRAIT_SPACER_HEIGHT - scaledScreenHeight
            val availableButtonHeight = deviceHeight - PORTRAIT_SPACER_HEIGHT - scaledScreenHeight
            val availableButtonWidth = deviceWidth  // Full width

            android.util.Log.d("DeviceAdapter", "  Available for buttons: ${availableButtonWidth}×${availableButtonHeight}")

            val boxRatio = availableButtonWidth.toFloat() / availableButtonHeight.toFloat()
            val patternRatio = PORTRAIT_PATTERN_WIDTH / PORTRAIT_PATTERN_HEIGHT

            android.util.Log.d("DeviceAdapter", "  Box ratio: ${"%.3f".format(boxRatio)}")
            android.util.Log.d("DeviceAdapter", "  Pattern ratio: ${"%.3f".format(patternRatio)}")

            val requiredHeight: Int

            if (boxRatio < patternRatio) {
                // Width limits
                val X = availableButtonWidth / PORTRAIT_PATTERN_WIDTH
                requiredHeight = (X * PORTRAIT_PATTERN_HEIGHT).toInt()
                android.util.Log.d("DeviceAdapter", "  WIDTH limits → X = ${"%.2f".format(X)}px")
                android.util.Log.d("DeviceAdapter", "  Required height: ${requiredHeight}px")
            } else {
                // Height limits
                requiredHeight = availableButtonHeight
                android.util.Log.d("DeviceAdapter", "  HEIGHT limits")
            }

            if (requiredHeight <= availableButtonHeight) {
                android.util.Log.d("DeviceAdapter", "  ✓ FITS! Using ${scale}x")

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