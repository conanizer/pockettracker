package com.conanizer.pockettracker

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.provider.Settings
import android.util.Log
import android.view.Window
import android.view.WindowManager
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.View
import androidx.activity.ComponentActivity
import androidx.core.view.WindowCompat
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.sp
import android.content.res.Configuration
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.core.content.ContextCompat
import com.conanizer.pockettracker.ui.theme.PockettrackerTheme
import androidx.compose.ui.focus.FocusRequester
import com.conanizer.pockettracker.core.logic.InstrumentController
import com.conanizer.pockettracker.core.logic.PlaybackController
import com.conanizer.pockettracker.core.logic.FileController
import com.conanizer.pockettracker.core.audio.AudioEngine
import com.conanizer.pockettracker.core.data.InstrumentType
import com.conanizer.pockettracker.core.data.ScreenType
import com.conanizer.pockettracker.core.logic.ClipboardManager
import com.conanizer.pockettracker.core.logic.EffectProcessor
import com.conanizer.pockettracker.core.logic.RenderController
import com.conanizer.pockettracker.platform.android.OboeAudioBackend
import androidx.compose.runtime.rememberCoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import com.conanizer.pockettracker.platform.android.AndroidResourceLoader
import com.conanizer.pockettracker.platform.android.AndroidFileSystem
import com.conanizer.pockettracker.platform.android.ThemeLoader
import com.conanizer.pockettracker.platform.android.AndroidVideoAudioExtractor
import com.conanizer.pockettracker.platform.android.ButtonSoundManager
import com.conanizer.pockettracker.platform.android.ButtonHapticManager
import com.conanizer.pockettracker.core.storage.FileInfo
import java.io.File

// ═══════════════════════════════════════════════════════════════════════════
// HELPER FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Convert java.io.File to platform-agnostic FileInfo.
 * Temporary helper during refactoring - FileBrowserModule will eventually use FileInfo directly.
 */
fun File.toFileInfo(): FileInfo = FileInfo(
    path = absolutePath,
    name = name,
    extension = extension,
    isDirectory = isDirectory,
    size = if (isFile) length() else 0L,
    lastModified = lastModified()
)

// ═══════════════════════════════════════════════════════════════════════════
// MAIN ACTIVITY
// ═══════════════════════════════════════════════════════════════════════════

/**
 * MAIN ACTIVITY
 *
 * This is the entry point of the app. It runs ONCE when the app starts.
 *
 * What it does:
 * 1. Creates a DeviceAdapter to detect what kind of device we're on
 * 2. Calculates the best layout (full screen vs virtual buttons, portrait vs landscape)
 * 3. Logs the detection results so we can see them in Logcat
 * 4. Passes the layout configuration to PocketTrackerApp
 */
class MainActivity : ComponentActivity() {

    /** Hide status bar + navigation bar (immersive sticky). Called on create and focus regain.
     *  Must be called AFTER setContent so the DecorView is attached (API 30+ requirement). */
    private fun hideSystemBars() {
        // API 30+ path: insetsController requires DecorView to be attached
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.decorView.post {
                window.insetsController?.apply {
                    hide(WindowInsets.Type.systemBars())
                    systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                }
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                or View.SYSTEM_UI_FLAG_FULLSCREEN
                or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
            )
        }
    }

    /** Re-apply immersive mode whenever the window regains focus (e.g. after a dialog). */
    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        requestWindowFeature(Window.FEATURE_NO_TITLE)

        // Edge-to-edge: let Compose fill the entire display, behind system bars.
        // Without this, Android reserves inset padding for nav/status bars and the
        // Canvas only sees the reduced area — which can drop scale from 2× to 1× in
        // landscape on phones with a non-gesture navigation bar (e.g. Realme UI 6).
        WindowCompat.setDecorFitsSystemWindows(window, false)

        // Allow content to extend into display-cutout areas (punch-hole camera, notch).
        // In landscape, the camera hole is on a short edge; SHORT_EDGES lets us draw
        // behind it so the Canvas gets the full 1080px height rather than a reduced area.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            window.attributes.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
        }

        // Your existing code...
        val deviceAdapter = DeviceAdapter(this)
        val layout = deviceAdapter.calculateLayout()

        Log.d("DeviceAdapter", "=== DEVICE DETECTION ===")
        Log.d("DeviceAdapter", deviceAdapter.getConfigDescription(layout))
        Log.d("DeviceAdapter", "======================")

        setContent {
            PockettrackerTheme {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = Color.Black
                ) {
                    PocketTrackerApp(layoutConfig = layout, deviceAdapter = deviceAdapter)
                }
            }
        }

        // DecorView is now attached — safe to call on all API levels
        hideSystemBars()
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// POCKET TRACKER APP
// ═══════════════════════════════════════════════════════════════════════════

/**
 * POCKET TRACKER APP
 *
 * This is the main composable function that contains all the app logic.
 * A "composable" is a function that creates UI elements.
 *
 * @param layoutConfig - Information about the device (from DeviceAdapter)
 *                       This tells us if we need virtual buttons and what orientation
 *
 * What it does:
 * 1. Sets up all the app state (cursor position, current screen, etc.)
 * 2. Creates button handlers (what happens when each button is pressed)
 * 3. Chooses which layout to show based on layoutConfig
 */

@Composable
fun PocketTrackerApp(layoutConfig: DeviceAdapter.LayoutConfig, deviceAdapter: DeviceAdapter) {
    // Get Android context (needed for file access, audio, etc.)
    val context = LocalContext.current
    val coroutineScope = rememberCoroutineScope()

    // ═══════════════════════════════════════════════════════════════════════
    // STORAGE PERMISSIONS REQUEST
    // ═══════════════════════════════════════════════════════════════════════

    // Request storage permissions for reading WAV files from Documents folder
    val permissionsToRequest = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
        // Android 13+ (API 33+): Use granular media permissions
        arrayOf(Manifest.permission.READ_MEDIA_AUDIO)
    } else {
        // Android 12 and below: Use legacy storage permissions
        arrayOf(
            Manifest.permission.READ_EXTERNAL_STORAGE,
            Manifest.permission.WRITE_EXTERNAL_STORAGE
        )
    }

    val permissionLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.RequestMultiplePermissions()
    ) { permissions ->
        val allGranted = permissions.values.all { it }
        if (allGranted) {
            Log.d("Permissions", "✅ Storage permissions granted!")
        } else {
            Log.w("Permissions", "❌ Storage permissions denied - some features may not work")
        }
    }

    // Check and request permissions on first composition
    //*
    LaunchedEffect(Unit) {
        val hasPermission = permissionsToRequest.all { permission ->
            ContextCompat.checkSelfPermission(context, permission) == PackageManager.PERMISSION_GRANTED
        }

        if (!hasPermission) {
            Log.d("Permissions", "Requesting storage permissions...")
            permissionLauncher.launch(permissionsToRequest)
        } else {
            Log.d("Permissions", "✅ Storage permissions already granted")
        }

        // Android 11+ (API 30+): Request MANAGE_EXTERNAL_STORAGE for full file access
        // This is needed to see files copied from other devices via USB/file manager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            if (!Environment.isExternalStorageManager()) {
                Log.d("Permissions", "Requesting MANAGE_EXTERNAL_STORAGE permission...")
                try {
                    val intent = Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION).apply {
                        data = Uri.parse("package:${context.packageName}")
                    }
                    context.startActivity(intent)
                } catch (e: Exception) {
                    // Fallback for devices that don't support the app-specific intent
                    Log.w("Permissions", "App-specific intent failed, trying general intent: ${e.message}")
                    val intent = Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)
                    context.startActivity(intent)
                }
            } else {
                Log.d("Permissions", "✅ MANAGE_EXTERNAL_STORAGE already granted")
            }
        }
    }
    // ═══════════════════════════════════════════════════════════════════════
    // FILE SYSTEM SETUP (REFACTORED ARCHITECTURE - Phase 2 COMPLETE!)
    // ═══════════════════════════════════════════════════════════════════════

    // Step 1: Create platform-specific file system backend
    val fileSystem = remember { AndroidFileSystem(context) }

    // Video audio extractor — Android implementation (MediaExtractor + MediaCodec)
    val videoExtractor = remember { AndroidVideoAudioExtractor() }

    // ═══════════════════════════════════════════════════════════════════════
    // PLATFORM BACKENDS (Phase 5: Complete Portability)
    // ═══════════════════════════════════════════════════════════════════════

    // Create logger (used by all controllers)
    val logger = remember { com.conanizer.pockettracker.platform.android.AndroidLogger() }

    // Create state observer (triggers UI recomposition when controller state changes)
    // This is the bridge between platform-agnostic controllers and Compose's reactive UI
    var stateVersion by remember { mutableIntStateOf(0) }
    val stateObserver = remember {
        object : com.conanizer.pockettracker.core.logic.StateObserver {
            override fun onStateChanged() {
                stateVersion++  // Trigger Compose recomposition
            }
        }
    }

    // Step 2/3: Create FileController (coordinates all file operations)
    val fileController = remember { FileController(fileSystem, logger) }

    // ═══════════════════════════════════════════════════════════════════════
    // AUDIO ENGINE SETUP (REFACTORED ARCHITECTURE - Phase 1 COMPLETE!)
    // ═══════════════════════════════════════════════════════════════════════

    // Step 1: Create platform-specific backends
    val audioBackend = remember { OboeAudioBackend() }
    val resourceLoader = remember { AndroidResourceLoader(context) }

    // Step 2: Create platform-agnostic AudioEngine (object only — stream opens below)
    val audioEngine = remember { AudioEngine(audioBackend, resourceLoader, logger) }

    // Step 3: Cleanup when app closes (important to prevent memory leaks)
    DisposableEffect(Unit) {
        onDispose {
            audioEngine.close()
        }
    }

    // Open the Oboe audio stream on an IO thread.
    // On some devices (e.g. Miyoo Flip / GammaCoreOS) opening an AAudio LowLatency/Exclusive
    // stream triggers Android's C2 codec framework to enumerate ~42 codecs, which can take
    // up to 35 seconds and completely freezes the main thread if done synchronously.
    var audioReady by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) {
        withContext(Dispatchers.IO) {
            audioEngine.create()
        }
        audioReady = true
    }

    // InstrumentController: Manages all instrument operations
    // PHASE 4: Extracted from MainActivity to separate business logic
    // PHASE 5: Uses StateObserver for UI reactivity
    val instrumentController = remember {
        InstrumentController(audioEngine, logger, stateObserver, fileController)
    }

    // EffectProcessor: Processes effects (Milestone 2 - Kill effect implemented!)
    // PHASE 4: Extracted from MainActivity to separate business logic
    val effectProcessor = remember {
        com.conanizer.pockettracker.core.logic.EffectProcessor(audioBackend, logger)
    }

    // PlaybackController: Manages all playback operations
    // PHASE 4: Extracted from MainActivity to separate business logic
    // PHASE 5: Uses StateObserver for UI reactivity
    // MILESTONE 2: Now includes EffectProcessor for effects support
    val playbackController = remember {
        PlaybackController(audioEngine, effectProcessor, logger, stateObserver)
    }
    // Wire InstrumentController into PlaybackController for soundfont slot lookups
    LaunchedEffect(playbackController, instrumentController) {
        playbackController.instrumentController = instrumentController
    }

    // ClipboardManager: Handles copy/paste (stub for now, implementation in Milestone 2.5)
    // PHASE 4: Extracted from MainActivity to separate business logic
    val clipboardManager = remember {
        com.conanizer.pockettracker.core.logic.ClipboardManager(logger)
    }

    // RenderController: Handles offline rendering to WAV files
    // MVP EXPANSION Phase 6: WAV Export functionality
    val renderController = remember {
        RenderController(audioEngine, playbackController, fileSystem, logger)
    }

    // Render state for WAV export
    val _isRendering = remember { mutableStateOf(false) }
    var isRendering by _isRendering
    val _renderProgress = remember { mutableFloatStateOf(0f) }
    var renderProgress by _renderProgress

    // Clean dialog state (triggered by A on row 5 in PROJECT screen)
    val _showCleanDialog = remember { mutableStateOf(false) }
    var showCleanDialog by _showCleanDialog
    val _cleanDialogTarget = remember { mutableStateOf("") }  // "SEQ" or "INST"
    var cleanDialogTarget by _cleanDialogTarget
    val _cleanDialogCursor = remember { mutableIntStateOf(0) }  // 0=YES, 1=NO
    var cleanDialogCursor by _cleanDialogCursor

    // Tracks where the last single A-press inserted into an empty cell (screen, row, col)
    // Used by A+A to decide whether to insert next-unused (only allowed on same cell)
    val _lastAInsertPosition = remember { mutableStateOf<InsertPosition?>(null) }
    var lastAInsertPosition by _lastAInsertPosition
    // InputController: Handles button input
    // PHASE 4: Extracted from MainActivity to separate business logic
    // PHASE 5: Uses StateObserver for UI reactivity
    val inputController = remember {
        com.conanizer.pockettracker.core.logic.InputController(logger, stateObserver)
    }

    // TrackerController: Main coordinator that owns state and delegates to controllers
    // PHASE 4: This is the MAIN COORDINATOR for all tracker logic
    // PHASE 5: Uses StateObserver for UI reactivity
    val trackerController = remember {
        com.conanizer.pockettracker.core.logic.TrackerController(
            fileController = fileController,
            playbackController = playbackController,
            instrumentController = instrumentController,
            effectProcessor = effectProcessor,
            clipboardManager = clipboardManager,
            inputController = inputController,
            stateObserver = stateObserver
        )
    }

    // NOTE: GenericInputHandler has been migrated to InputController (Phase 4)
    // All input handling now goes through trackerController.inputController

    // Sync mixer volumes to audio backend once the stream is open.
    // (setTrackVolume/setMasterVolume are no-ops if native engine is null, so this must
    // run after audioReady — i.e., after LaunchedEffect(Unit) above finishes create().)
    LaunchedEffect(audioReady) {
        if (!audioReady) return@LaunchedEffect
        for (i in 0 until 8) {
            val vol = trackerController.project.tracks[i].volume
            audioBackend.setTrackVolume(i, com.conanizer.pockettracker.core.data.VolumeUtils.hexToFloat(vol))
        }
        audioBackend.setMasterVolume(com.conanizer.pockettracker.core.data.VolumeUtils.hexToFloat(trackerController.project.masterVolume))
        audioBackend.setOttDepth(trackerController.project.ottDepth)
        audioBackend.setMasterFx(trackerController.project.masterBusFx)
        audioBackend.setDustDepth(trackerController.project.dustDepth)
        Log.d("VolumeSync", "Initial volume sync to audio backend complete")
    }

    // ChainEditorModule: Used to get cursor context for chain editing
    val chainEditorModule = remember { ChainEditorModule() }

    // PhraseEditorModule: Used to get cursor context for phrase editing
    val phraseEditorModule = remember { PhraseEditorModule() }

    // SongEditorModule: Used to get cursor context for song editing
    val songEditorModule = remember { SongEditorModule() }

    // ProjectModule: Used to get cursor context for project editing
    val projectModule = remember { ProjectModule() }

    // SettingsModule: Used for SETTINGS side menu
    val settingsModule = remember { SettingsModule() }

    // InstrumentModule: Used for instrument editing screen
    val instrumentModule = remember { InstrumentModule() }

    // MixerModule: Used for mixer screen (8 tracks + master)
    val mixerModule = remember { MixerModule() }

    // EffectModule: Used for effects screen (reverb, delay, master EQ)
    val effectModule = remember { EffectModule() }
    val eqModule     = remember { EqModule() }

    // TableModule: Used for table editing screen
    val tableModule = remember { TableModule() }

    // GrooveModule: Used for groove pattern editing screen
    val grooveModule = remember { GrooveModule() }

    // ModulationModule: Used for modulation editing screen
    val modulationModule = remember { ModulationModule() }

    // Peak level buffers for mixer meters (updated periodically)
    val trackPeakBuffer = remember { FloatArray(16) }
    val masterPeakBuffer = remember { FloatArray(2) }
    val sendPeakBuffer = remember { FloatArray(4) }  // [revL, revR, delL, delR]

    // ═══════════════════════════════════════════════════════════════════════
    // LAYOUT MODE — user-selectable, overrides DeviceAdapter auto-detection
    // ═══════════════════════════════════════════════════════════════════════

    // Derive initial mode from the auto-detected layoutConfig (fallback if no saved pref)
    val autoLayoutMode = when {
        !layoutConfig.needsVirtualButtons -> DeviceAdapter.LayoutMode.FULL
        layoutConfig.isLandscape          -> DeviceAdapter.LayoutMode.TOUCH_LANDSCAPE
        else                              -> DeviceAdapter.LayoutMode.TOUCH_PORTRAIT2
    }

    // Load saved preferences (SharedPreferences, persists across app restarts)
    val prefs = remember { context.getSharedPreferences("pockettracker_ui", android.content.Context.MODE_PRIVATE) }

    val savedLayoutName  = remember { prefs.getString("layout_mode", null) }
    val savedScalingName = remember { prefs.getString("scaling_mode", null) }

    val initialLayoutMode = remember {
        val saved = if (savedLayoutName != null) {
            DeviceAdapter.LayoutMode.entries.firstOrNull { it.name == savedLayoutName } ?: autoLayoutMode
        } else {
            autoLayoutMode
        }
        when {
            // TOUCH_PORTRAIT is retired from the active cycle — migrate to AMIGA PORTRAIT
            saved == DeviceAdapter.LayoutMode.TOUCH_PORTRAIT ->
                DeviceAdapter.LayoutMode.TOUCH_PORTRAIT2
            // FULLSCREEN on a touch-only device would trap the user with no virtual buttons
            saved == DeviceAdapter.LayoutMode.FULL && layoutConfig.needsVirtualButtons ->
                autoLayoutMode
            else -> saved
        }
    }
    val initialScalingMode = remember {
        if (savedScalingName != null) {
            DeviceAdapter.ScalingMode.entries.firstOrNull { it.name == savedScalingName } ?: DeviceAdapter.ScalingMode.NEAREST
        } else {
            DeviceAdapter.ScalingMode.NEAREST
        }
    }

    val _layoutMode  = remember { mutableStateOf(initialLayoutMode) }
    var layoutMode   by _layoutMode
    val _scalingMode = remember { mutableStateOf(initialScalingMode) }
    var scalingMode  by _scalingMode

    // Persist whenever either setting changes
    LaunchedEffect(layoutMode) {
        prefs.edit().putString("layout_mode", layoutMode.name).apply()
    }
    LaunchedEffect(scalingMode) {
        prefs.edit().putString("scaling_mode", scalingMode.name).apply()
    }

    // ═══════════════════════════════════════════════════════════════════════
    // BUTTON SOUND & VIBRO — app-level settings persisted in SharedPreferences
    // ═══════════════════════════════════════════════════════════════════════

    val _buttonSoundEnabled = remember { mutableStateOf(prefs.getBoolean("button_sound", true)) }
    var buttonSoundEnabled  by _buttonSoundEnabled
    val _buttonSoundVolume  = remember { mutableStateOf(prefs.getInt("button_sound_volume", 255)) }
    var buttonSoundVolume   by _buttonSoundVolume
    val _buttonVibroEnabled = remember { mutableStateOf(prefs.getBoolean("button_vibro", true)) }
    var buttonVibroEnabled  by _buttonVibroEnabled
    val _vibroPower         = remember { mutableStateOf(prefs.getInt("vibro_power", 255)) }
    var vibroPower          by _vibroPower

    // QWERTY keyboard insert mode (persisted in SharedPreferences)
    val _insertBefore = remember { mutableStateOf(prefs.getBoolean("kb_insert_before", true)) }
    var insertBefore  by _insertBefore

    // Cursor remember mode: REMEMBER=true keeps cursor position between screen switches,
    // REFRESH=false resets cursor to default on every screen switch (persisted)
    val _cursorRemember = remember { mutableStateOf(prefs.getBoolean("cursor_remember", false)) }
    var cursorRemember  by _cursorRemember

    // QWERTY keyboard overlay state (transient — not persisted)
    val _qwertyKeyboardState = remember { mutableStateOf(QwertyKeyboardState()) }
    var qwertyKeyboardState  by _qwertyKeyboardState

    // FX helper overlay state (transient — not persisted)
    val _fxHelperState = remember { mutableStateOf(FxHelperState()) }
    var fxHelperState  by _fxHelperState

    // EQ editor overlay state (transient — not persisted)
    val _eqEditorState  = remember { mutableStateOf(EqEditorState()) }
    var eqEditorState   by _eqEditorState
    val _eqSpectrumData = remember { mutableStateOf<FloatArray?>(null) }
    var eqSpectrumData  by _eqSpectrumData

    val buttonSoundManager = remember { ButtonSoundManager(context) }
    val buttonHapticManager = remember { ButtonHapticManager(context) }

    // Sync enabled flags into managers whenever they change
    LaunchedEffect(buttonSoundEnabled) {
        buttonSoundManager.enabled = buttonSoundEnabled
        prefs.edit().putBoolean("button_sound", buttonSoundEnabled).apply()
    }
    LaunchedEffect(buttonSoundVolume) {
        buttonSoundManager.volume = buttonSoundVolume / 255f
        prefs.edit().putInt("button_sound_volume", buttonSoundVolume).apply()
    }
    LaunchedEffect(buttonVibroEnabled) {
        buttonHapticManager.enabled = buttonVibroEnabled
        prefs.edit().putBoolean("button_vibro", buttonVibroEnabled).apply()
    }
    LaunchedEffect(vibroPower) {
        buttonHapticManager.power = vibroPower
        prefs.edit().putInt("vibro_power", vibroPower).apply()
    }
    LaunchedEffect(insertBefore) {
        prefs.edit().putBoolean("kb_insert_before", insertBefore).apply()
    }
    LaunchedEffect(cursorRemember) {
        prefs.edit().putBoolean("cursor_remember", cursorRemember).apply()
    }

    // Release SoundPool when the composable leaves composition
    DisposableEffect(Unit) {
        onDispose { buttonSoundManager.release() }
    }

    // Theme — starts as DARK (immediate), loads AMIGA PNGs in background
    var theme by remember { mutableStateOf<DeviceTheme>(DeviceTheme.AMIGA) }
    LaunchedEffect(Unit) {
        withContext(Dispatchers.IO) {
            val loaded = ThemeLoader.loadAmigaTheme(context)
            withContext(Dispatchers.Main) { theme = loaded }
        }
    }

    // Track orientation so layout recalculates when device flips (Activity survives
    // rotation thanks to android:configChanges in the manifest, but device dimensions
    // swap so we need a fresh LayoutConfig).
    val configuration = LocalConfiguration.current

    // Recompute layout config whenever the user changes the mode OR device flips
    val effectiveLayoutConfig = remember(layoutMode, configuration.orientation) {
        deviceAdapter.calculateLayout(layoutMode)
    }

    // Auto-switch between portrait/landscape virtual-button modes on device flip
    LaunchedEffect(configuration.orientation) {
        when {
            (layoutMode == DeviceAdapter.LayoutMode.TOUCH_PORTRAIT ||
             layoutMode == DeviceAdapter.LayoutMode.TOUCH_PORTRAIT2) &&
                    configuration.orientation == Configuration.ORIENTATION_LANDSCAPE ->
                layoutMode = DeviceAdapter.LayoutMode.TOUCH_LANDSCAPE

            layoutMode == DeviceAdapter.LayoutMode.TOUCH_LANDSCAPE &&
                    configuration.orientation == Configuration.ORIENTATION_PORTRAIT ->
                layoutMode = DeviceAdapter.LayoutMode.TOUCH_PORTRAIT2
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // STATE ALIASES (read from TrackerController, triggered by stateVersion)
    // ═══════════════════════════════════════════════════════════════════════
    //
    // All core state is now owned by TrackerController. These aliases create
    // a dependency on stateVersion so Compose recomposes when state changes.
    // For writes, use trackerController.xxx = value directly.

    // Core state aliases (depend on stateVersion for recomposition)
    // The stateVersion.let { } pattern creates a dependency on stateVersion
    val project = stateVersion.let { trackerController.project }
    val currentScreen = stateVersion.let { trackerController.currentScreen }
    val previousColumn = stateVersion.let { trackerController.previousColumn }
    val cursorRow = stateVersion.let { trackerController.cursorRow }
    val cursorColumn = stateVersion.let { trackerController.cursorColumn }
    val projectCursorRow = stateVersion.let { trackerController.projectCursorRow }
    val projectCursorColumn = stateVersion.let { trackerController.projectCursorColumn }
    val currentChain = stateVersion.let { trackerController.currentChain }
    val currentPhrase = stateVersion.let { trackerController.currentPhrase }
    val lastEditedPhrase = stateVersion.let { trackerController.lastEditedPhrase }
    val lastEditedChain = stateVersion.let { trackerController.lastEditedChain }
    val lastEditedNote = stateVersion.let { trackerController.lastEditedNote }
    val lastEditedVolume = stateVersion.let { trackerController.lastEditedVolume }
    val lastEditedTranspose = stateVersion.let { trackerController.lastEditedTranspose }
    val projectVersion = stateVersion.let { trackerController.projectVersion }

    // Status message aliases - use TrackerController's unified status
    val projectStatusMessage = stateVersion.let { trackerController.statusMessage }
    val projectStatusSuccess = stateVersion.let { trackerController.statusSuccess }

    // Auto-dismiss status messages after 5 seconds
    LaunchedEffect(trackerController.statusMessage) {
        if (trackerController.statusMessage.isNotEmpty()) {
            kotlinx.coroutines.delay(5000)
            trackerController.clearStatus()
        }
    }

    // Auto-dismiss instrument status (from InstrumentController)
    LaunchedEffect(instrumentController.statusMessage) {
        if (instrumentController.statusMessage.isNotEmpty()) {
            kotlinx.coroutines.delay(5000)
            instrumentController.clearStatus()
        }
    }

    // File browser module and state
    val fileBrowserModule = remember { FileBrowserModule() }
    val _fileBrowserState = remember {
        mutableStateOf(
            FileBrowserModule.State(
                currentDirectory = File(fileController.getProjectsDirectory()),
                items = emptyList(),
                fileExtension = "ptp"  // Only show .ptp project files
            )
        )
    }
    var fileBrowserState by _fileBrowserState
    val _previousScreen = remember { mutableStateOf(ScreenType.PROJECT) }
    var previousScreen  by _previousScreen
    // Tracks what action triggered the file browser from the INSTRUMENT screen
    // Values: "LOAD_SOURCE", "LOAD_PRESET", "SAVE_PRESET"
    val _instrumentFileBrowserAction = remember { mutableStateOf("") }
    var instrumentFileBrowserAction  by _instrumentFileBrowserAction

    // Sample editor module and state
    val sampleEditorModule = remember { SampleEditorModule() }
    val _sampleEditorState = remember { mutableStateOf(SampleEditorState(sampleId = 0, instrumentId = 0)) }
    var sampleEditorState  by _sampleEditorState

    // Reset note/volume combo when leaving PHRASE screen
    // (instrument is kept so quick-insert uses the last instrument you worked with)
    var wasPhraseScreen by remember { mutableStateOf(false) }
    LaunchedEffect(trackerController.currentScreen) {
        val isPhrase = trackerController.currentScreen == ScreenType.PHRASE
        if (wasPhraseScreen && !isPhrase) {
            trackerController.lastEditedNote = com.conanizer.pockettracker.core.data.Note.fromMidi(60) // C-4
            trackerController.lastEditedVolume = 0xFF
        }
        wasPhraseScreen = isPhrase
    }

    // Initialize file browser item list when directory changes
    LaunchedEffect(fileBrowserState.currentDirectory, fileBrowserState.sortMode) {
        val items = fileBrowserModule.buildItemList(
            fileBrowserState.currentDirectory,
            fileBrowserState.fileExtension,
            fileBrowserState.fileExtensions
        )
        fileBrowserState = fileBrowserState.copy(
            items = fileBrowserModule.sortItems(items, fileBrowserState.sortMode)
        )
    }

    // Load waveform data whenever the sample editor becomes the active screen or instrument changes
    LaunchedEffect(trackerController.currentScreen, sampleEditorState.instrumentId) {
        if (trackerController.currentScreen == ScreenType.SAMPLE_EDITOR) {
            val instId      = sampleEditorState.instrumentId
            val totalFrames = audioEngine.getSampleLength(instId)
            val sampleRate  = audioEngine.getOriginalSampleRate(instId)
            val hasStereo   = audioEngine.hasStereoData(instId)
            val channel     = when (sampleEditorState.sourceMode) { 0 -> 0; 1 -> 1; else -> 2 }
            val waveformData = if (hasStereo) {
                audioEngine.getSampleWaveformRangeSource(instId, 0, totalFrames, 620, channel)
            } else {
                audioEngine.getSampleWaveform(instId, 620)
            }
            val inst = trackerController.project.instruments[instId]

            // Use instrument.sliceMarkers as the display source for the editor.
            // These are already loaded from the WAV cue chunk by loadSampleFromFile,
            // so no extra file I/O is needed here and there are no race conditions.
            // instrument.sliceMarkers is only updated when the user saves from the editor.
            val cuePoints = inst.sliceMarkers.map { it.toInt() }.toIntArray()

            sampleEditorState = sampleEditorState.copy(
                totalFrames   = totalFrames,
                waveformData  = waveformData,
                sampleRate    = sampleRate,
                hasStereoData = hasStereo,
                selectionStart   = (inst.sampleStart.toLong() * totalFrames) / 255L,
                selectionEnd     = (inst.sampleEnd.toLong()   * totalFrames) / 255L,
                transientMarkers = cuePoints,
                // sliceMethod intentionally not changed: stays at 2 (OFF) on fresh open,
                // or preserves the user's current choice on navigate-back.
                sliceIndex       = 0
            )
        }
    }

    // Reload zoomed waveform when zoom level, view window, or source mode changes
    val sampleEditorZoom       = sampleEditorState.zoomLevel
    val sampleEditorViewStart  = sampleEditorState.viewStart
    val sampleEditorSourceMode = sampleEditorState.sourceMode
    LaunchedEffect(sampleEditorZoom, sampleEditorViewStart, sampleEditorSourceMode) {
        if (trackerController.currentScreen == ScreenType.SAMPLE_EDITOR && sampleEditorState.totalFrames > 0) {
            val instId      = sampleEditorState.instrumentId
            val totalFrames = sampleEditorState.totalFrames
            val hasStereo   = sampleEditorState.hasStereoData
            val channel     = when (sampleEditorSourceMode) { 0 -> 0; 1 -> 1; else -> 2 }
            // Compute viewEnd from the captured keys to stay consistent
            val viewStart = sampleEditorViewStart.toInt()
            val viewEnd   = (if (sampleEditorZoom == 0) totalFrames.toLong()
                             else (sampleEditorViewStart + (totalFrames.toLong() ushr sampleEditorZoom))
                                     .coerceAtMost(totalFrames.toLong())).toInt()
            val waveformData = if (hasStereo) {
                audioEngine.getSampleWaveformRangeSource(instId, viewStart, viewEnd, 620, channel)
            } else if (sampleEditorZoom == 0) {
                audioEngine.getSampleWaveform(instId, 620)
            } else {
                audioEngine.getSampleWaveformRange(instId, viewStart, viewEnd, 620)
            }
            sampleEditorState = sampleEditorState.copy(waveformData = waveformData)
        }
    }

    // Run transient detection when TRANSIENT mode is active and sensitivity or sample changes
    val seSliceMethod      = sampleEditorState.sliceMethod
    val seSliceSensitivity = sampleEditorState.sliceSensitivity
    val seTotalFrames      = sampleEditorState.totalFrames
    LaunchedEffect(seSliceMethod, seSliceSensitivity, seTotalFrames) {
        if (seSliceMethod == 0 && seTotalFrames > 0 && sampleEditorState.transientMarkers.isEmpty()) {
            // Auto-detect when switching to TRANSIENT with no markers yet.
            // handleInput clears transientMarkers when user switches TO TRANSIENT,
            // so this fires on that transition and on sensitivity changes.
            val markers = audioEngine.detectTransients(sampleEditorState.instrumentId, seSliceSensitivity)
            val firstSliceEnd = markers.firstOrNull()?.toLong() ?: seTotalFrames.toLong()
            sampleEditorState = sampleEditorState.copy(
                transientMarkers = markers,
                sliceIndex       = 0,
                selectionStart   = 0L,
                selectionEnd     = firstSliceEnd
            )
            // Markers stay in sampleEditorState only until saved — not synced to instrument here.
        }
    }

    // Poll playback position for real-time waveform marker (~30fps)
    LaunchedEffect(Unit) {
        while (true) {
            if (trackerController.currentScreen == ScreenType.SAMPLE_EDITOR) {
                val pollSlot = if (sampleEditorState.hasStereoData && sampleEditorState.sourceMode != 2) 254
                               else sampleEditorState.sampleId
                val pos = audioEngine.getSamplePlaybackPosition(pollSlot)
                if (pos != sampleEditorState.playbackPosition) {
                    sampleEditorState = sampleEditorState.copy(playbackPosition = pos)
                }
            }
            delay(33)
        }
    }

    // (Audio engine cleanup moved to line 168-172 with new architecture)

    // Poll spectrum magnitudes for EQ visualizer (~20fps while EQ screen is open)
    LaunchedEffect(eqEditorState.isOpen) {
        if (eqEditorState.isOpen) {
            while (true) {
                eqSpectrumData = audioEngine.getSpectrumMagnitudes(620)
                delay(50)
            }
        } else {
            eqSpectrumData = null
        }
    }

    // Update peak levels for mixer meters (every ~60ms = ~16fps update rate)
    // When not playing, manually decay peaks and waveform (fixes freeze on stop bug)
    LaunchedEffect(currentScreen) {
        if (currentScreen == ScreenType.MIXER) {
            while (true) {
                // When not playing, manually decay peaks (audio callback not running)
                if (!trackerController.isPlaying()) {
                    audioBackend.decayPeaks()
                    audioBackend.decayWaveform()
                }
                // Always read current peak values for display
                audioBackend.getTrackPeaks(trackPeakBuffer)
                audioBackend.getMasterPeaks(masterPeakBuffer)
                audioBackend.getSendPeaks(sendPeakBuffer)
                stateVersion++  // Trigger recomposition
                kotlinx.coroutines.delay(60)
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // DISPATCHER WIRING
    // ═══════════════════════════════════════════════════════════════════════

    val appCtrl = remember {
        AppControllers(
            trackerController, audioEngine, audioBackend, instrumentController,
            fileController, renderController, clipboardManager, deviceAdapter,
            videoExtractor, fileSystem, coroutineScope,
            chainEditorModule, phraseEditorModule, songEditorModule, projectModule,
            settingsModule, instrumentModule, mixerModule, effectModule, eqModule,
            tableModule, grooveModule, modulationModule, fileBrowserModule, sampleEditorModule
        )
    }
    val appState = remember {
        AppStateRefs(
            _fileBrowserState, _sampleEditorState, _qwertyKeyboardState,
            _fxHelperState, _eqEditorState, _eqSpectrumData,
            _layoutMode, _scalingMode, _isRendering, _renderProgress,
            _showCleanDialog, _cleanDialogTarget, _cleanDialogCursor,
            _lastAInsertPosition, _insertBefore, _instrumentFileBrowserAction,
            _previousScreen, _buttonSoundEnabled, _buttonSoundVolume,
            _buttonVibroEnabled, _vibroPower, _cursorRemember,
            trackPeakBuffer, masterPeakBuffer, sendPeakBuffer
        )
    }
    val dispatcher = remember { AppInputDispatcher(appCtrl, appState) }

    // (All input helpers and button handlers are in AppInputDispatcher)

    val buttonHandlers = remember { dispatcher.createButtonHandlers() }

// ═══════════════════════════════════════════════════════════════════════
// KEYBOARD INPUT MAPPING
// ═══════════════════════════════════════════════════════════════════════

// Create the input mapper to handle keyboard input
// This maps WASD/JK/UI/Shift/Space to game buttons
    val inputMapper = remember(buttonHandlers) {
        InputMapper(buttonHandlers)
    }

// Focus requester to auto-focus the app for keyboard input
    val focusRequester = remember { FocusRequester() }

    //Auto-focus when app starts so keyboard works immediately
    LaunchedEffect(Unit) {
        // Small delay to ensure view is ready before requesting focus
        kotlinx.coroutines.delay(100)
        try {
            focusRequester.requestFocus()
        } catch (e: Exception) {
            // Ignore focus request errors
        }
    }

// ═══════════════════════════════════════════════════════════════════════
// CHOOSE LAYOUT BASED ON DEVICE DETECTION
// ═══════════════════════════════════════════════════════════════════════

// This is where the magic happens!
// Based on what DeviceAdapter detected, we show different layouts:

// Re-read controller properties when stateVersion changes
// This ensures layout functions recompose when controller state changes
    val isPlaying = stateVersion.let { trackerController.isPlaying() }
    val currentInstrument = stateVersion.let { trackerController.currentInstrument }
    val instrumentCursorRow = stateVersion.let { trackerController.instrumentCursorRow }
    val instrumentCursorColumn = stateVersion.let { trackerController.instrumentCursorColumn }
    val instrumentStatusMessage = stateVersion.let { trackerController.statusMessage }
    val instrumentStatusSuccess = stateVersion.let { trackerController.statusSuccess }

    // Selection/clipboard state for copy/paste
    val selectionInfo = stateVersion.let { trackerController.inputController.getSelectionInfo() }
    val clipboardInfo = stateVersion.let { clipboardManager.getClipboardInfo() }
    val selectionModeActive = stateVersion.let { trackerController.inputController.selectionMode }
    val isCellSelectedFn: (Int, Int) -> Boolean = { row, col ->
        trackerController.inputController.isCellSelected(row, col)
    }

    // Build the shared tracker params bundle once — all layout modes use the same values.
    val trackerParams = TrackerScreenParams(
        currentScreen           = currentScreen,
        project                 = project,
        audioEngine             = audioEngine,
        playbackController      = playbackController,
        cursorRow               = cursorRow,
        cursorColumn            = cursorColumn,
        isPlaying               = isPlaying,
        previousColumn          = previousColumn,
        currentChain            = currentChain,
        currentPhrase           = currentPhrase,
        projectCursorRow        = projectCursorRow,
        projectCursorColumn     = projectCursorColumn,
        projectStatusMessage    = projectStatusMessage,
        projectStatusSuccess    = projectStatusSuccess,
        projectVersion          = projectVersion,
        currentInstrument       = currentInstrument,
        instrumentCursorRow     = instrumentCursorRow,
        instrumentCursorColumn  = instrumentCursorColumn,
        instrumentStatusMessage = instrumentStatusMessage,
        instrumentStatusSuccess = instrumentStatusSuccess,
        fileBrowserState        = fileBrowserState,
        sampleEditorState       = sampleEditorState,
        selectionInfo           = selectionInfo,
        clipboardInfo           = clipboardInfo,
        selectionMode           = selectionModeActive,
        isCellSelected          = isCellSelectedFn,
        mixerCursorColumn       = trackerController.mixerCursorColumn,
        mixerMasterRow          = trackerController.mixerMasterRow,
        trackPeaks              = trackPeakBuffer,
        masterPeaks             = masterPeakBuffer,
        sendPeaks               = sendPeakBuffer,
        currentTable            = trackerController.currentTable,
        tableCursorRow          = trackerController.tableCursorRow,
        tableCursorColumn       = trackerController.tableCursorColumn,
        currentGroove           = trackerController.currentGroove,
        grooveCursorRow         = trackerController.grooveCursorRow,
        modCursorRow            = trackerController.modCursorRow,
        modCursorPair           = trackerController.modCursorPair,
        modCursorSide           = trackerController.modCursorSide,
        effectsCursorRow        = trackerController.effectsCursorRow,
        isRendering             = isRendering,
        renderProgress          = renderProgress,
        showCleanDialog         = showCleanDialog,
        cleanDialogTarget       = cleanDialogTarget,
        cleanDialogCursor       = cleanDialogCursor,
        songScrollPosition      = stateVersion.let { trackerController.songScrollPosition },
        scalingMode             = scalingMode,
        buttonSoundEnabled      = buttonSoundEnabled,
        buttonSoundVolume       = buttonSoundVolume,
        buttonVibroEnabled      = buttonVibroEnabled,
        vibroPower              = vibroPower,
        qwertyKeyboardState     = qwertyKeyboardState.copy(insertBefore = insertBefore),
        fxHelperState           = fxHelperState,
        eqEditorState           = eqEditorState,
        eqSpectrumData          = eqSpectrumData,
        settingsCursorRow       = stateVersion.let { trackerController.settingsCursorRow },
        settingsCursorColumn    = stateVersion.let { trackerController.settingsCursorColumn },
        cursorRemember          = cursorRemember,
        soundfontPresetName     = stateVersion.let {
            val inst = project.instruments[currentInstrument]
            val path = inst.soundfontPath
            if (path != null && inst.instrumentType == InstrumentType.SOUNDFONT) {
                val slot = instrumentController.sfSlotMap[path]
                if (slot != null) audioEngine.backend.getSoundfontPresetName(slot, inst.sfBank, inst.sfPreset)
                else "---"
            } else "---"
        },
        soundfontPresetCount    = stateVersion.let {
            instrumentController.getSoundfontPresetCount(project.instruments[currentInstrument])
        },
        soundfontPresetIndex    = stateVersion.let {
            instrumentController.getSoundfontCurrentPresetIndex(project.instruments[currentInstrument])
        }
    )

    // Show a loading screen until the Oboe stream is open.
    // Audio init can take a long time on some devices (e.g. GammaCoreOS / Miyoo Flip)
    // because AAudio's first-open triggers C2 codec enumeration. Running it on Dispatchers.IO
    // keeps the UI thread free so this screen is visible immediately.
    if (!audioReady) {
        Box(
            modifier = Modifier.fillMaxSize().background(Color.Black),
            contentAlignment = Alignment.Center
        ) {
            Text(
                text = "AUDIO LOADING...",
                color = Color(0xFF00CC00),
                fontSize = 14.sp
            )
        }
        return
    }

    val hapticView = LocalView.current
    CompositionLocalProvider(
        LocalLayoutMode provides layoutMode,
        LocalButtonEventCallback provides { button, isPress ->
            if (isPress) {
                buttonSoundManager.onPress(button)
                buttonHapticManager.onPress(hapticView)
            } else {
                buttonSoundManager.onRelease(button)
                buttonHapticManager.onRelease(hapticView)
            }
        }
    ) {
        if (!effectiveLayoutConfig.needsVirtualButtons) {
            // FULL SCREEN — physical buttons or user-selected FULL mode
            FullScreenLayout(
                layoutConfig  = effectiveLayoutConfig,
                scalingMode   = scalingMode,
                params        = trackerParams,
                inputMapper   = inputMapper,
                focusRequester = focusRequester
            )
        } else when (layoutMode) {
            DeviceAdapter.LayoutMode.TOUCH_LANDSCAPE ->
                LandscapeLayoutWithVirtualButtons(
                    layoutConfig  = effectiveLayoutConfig,
                    scalingMode   = scalingMode,
                    params        = trackerParams,
                    inputMapper   = inputMapper,
                    focusRequester = focusRequester
                )
            DeviceAdapter.LayoutMode.TOUCH_PORTRAIT2 ->
                PortraitLayout2WithVirtualButtons(
                    layoutConfig   = effectiveLayoutConfig,
                    scalingMode    = scalingMode,
                    params         = trackerParams,
                    inputMapper    = inputMapper,
                    focusRequester = focusRequester,
                    theme          = theme,
                )
            else -> // TOUCH_PORTRAIT (and FULL fallback — shouldn't normally reach here)
                PortraitLayoutWithVirtualButtons(
                    layoutConfig  = effectiveLayoutConfig,
                    scalingMode   = scalingMode,
                    params        = trackerParams,
                    inputMapper   = inputMapper,
                    focusRequester = focusRequester
                )
        }
    } // CompositionLocalProvider
}

// ═══════════════════════════════════════════════════════════════════════════
// NAVIGATION HELPER FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════
// NOTE: Navigation functions migrated to TrackerController (Phase 4 cleanup - January 2025)
// All navigation logic now in core/logic/TrackerController.kt

// ═══════════════════════════════════════════════════════════════════════════
// EDITOR HELPER FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════
// NOTE: Editor helpers migrated to EditorHelpers.kt (Phase 4 cleanup - January 2025)
// All phrase/chain/song/project editing helpers now in EditorHelpers.kt
