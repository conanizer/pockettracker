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
import com.conanizer.pockettracker.core.logic.InputAction
import com.conanizer.pockettracker.core.audio.AudioEngine
import com.conanizer.pockettracker.core.data.MAIN_ROW_SCREENS
import com.conanizer.pockettracker.core.data.InstrumentType
import com.conanizer.pockettracker.core.data.Note
import com.conanizer.pockettracker.core.data.Project
import com.conanizer.pockettracker.core.data.ScreenType
import com.conanizer.pockettracker.core.logic.ClipboardManager
import com.conanizer.pockettracker.core.logic.EffectProcessor
import com.conanizer.pockettracker.core.logic.RenderController
import com.conanizer.pockettracker.platform.android.OboeAudioBackend
import androidx.compose.runtime.rememberCoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import com.conanizer.pockettracker.platform.android.AndroidResourceLoader
import com.conanizer.pockettracker.platform.android.AndroidFileSystem
import com.conanizer.pockettracker.platform.android.ThemeLoader
import com.conanizer.pockettracker.core.ui.DeviceTheme
import com.conanizer.pockettracker.platform.android.AndroidVideoAudioExtractor
import com.conanizer.pockettracker.platform.android.ButtonSoundManager
import com.conanizer.pockettracker.platform.android.ButtonHapticManager
import com.conanizer.pockettracker.core.storage.FileInfo
import com.conanizer.pockettracker.core.storage.FileSortMode
import com.conanizer.pockettracker.core.storage.WavWriter
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
/** Tracks where a single A-press inserted into an empty cell (screen, row, col).
 *  Used by A+A to allow "next unused" only on the same cell the first A just filled. */
private data class InsertPosition(val screen: ScreenType, val row: Int, val col: Int)

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

    // Step 2: Create platform-agnostic FileManager
    // ✅ No more Context dependency - fully portable!
    val fileManager = remember { FileManager(fileSystem) }

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

    // Step 3: Create FileController (coordinates save/load operations)
    val fileController = remember { FileController(fileManager, logger) }

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
        RenderController(audioEngine, playbackController, fileSystem)
    }

    // Render state for WAV export
    var isRendering by remember { mutableStateOf(false) }
    var renderProgress by remember { mutableFloatStateOf(0f) }

    // Clean dialog state (triggered by A on row 5 in PROJECT screen)
    var showCleanDialog by remember { mutableStateOf(false) }
    var cleanDialogTarget by remember { mutableStateOf("") }  // "SEQ" or "INST"
    var cleanDialogCursor by remember { mutableIntStateOf(0) }  // 0=YES, 1=NO

    // Tracks where the last single A-press inserted into an empty cell (screen, row, col)
    // Used by A+A to decide whether to insert next-unused (only allowed on same cell)
    var lastAInsertPosition by remember { mutableStateOf<InsertPosition?>(null) }
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

    var layoutMode  by remember { mutableStateOf(initialLayoutMode) }
    var scalingMode by remember { mutableStateOf(initialScalingMode) }

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

    var buttonSoundEnabled by remember { mutableStateOf(prefs.getBoolean("button_sound", true)) }
    var buttonSoundVolume  by remember { mutableStateOf(prefs.getInt("button_sound_volume", 255)) }
    var buttonVibroEnabled by remember { mutableStateOf(prefs.getBoolean("button_vibro", true)) }
    var vibroPower         by remember { mutableStateOf(prefs.getInt("vibro_power", 255)) }

    // QWERTY keyboard insert mode (persisted in SharedPreferences)
    var insertBefore by remember { mutableStateOf(prefs.getBoolean("kb_insert_before", true)) }

    // Cursor remember mode: REMEMBER=true keeps cursor position between screen switches,
    // REFRESH=false resets cursor to default on every screen switch (persisted)
    var cursorRemember by remember { mutableStateOf(prefs.getBoolean("cursor_remember", false)) }

    // QWERTY keyboard overlay state (transient — not persisted)
    var qwertyKeyboardState by remember { mutableStateOf(QwertyKeyboardState()) }

    // FX helper overlay state (transient — not persisted)
    var fxHelperState by remember { mutableStateOf(FxHelperState()) }

    // EQ editor overlay state (transient — not persisted)
    var eqEditorState by remember { mutableStateOf(EqEditorState()) }

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
    var fileBrowserState by remember {
        mutableStateOf(
            FileBrowserModule.State(
                currentDirectory = File(fileManager.getProjectsDirectory()),
                items = emptyList(),
                fileExtension = "ptp"  // Only show .ptp project files
            )
        )
    }
    var previousScreen by remember { mutableStateOf(ScreenType.PROJECT) }
    // Tracks what action triggered the file browser from the INSTRUMENT screen
    // Values: "LOAD_SOURCE", "LOAD_PRESET", "SAVE_PRESET"
    var instrumentFileBrowserAction by remember { mutableStateOf("") }

    // Sample editor module and state
    val sampleEditorModule = remember { SampleEditorModule() }
    var sampleEditorState by remember { mutableStateOf(SampleEditorState(sampleId = 0, instrumentId = 0)) }

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
            val instId = sampleEditorState.instrumentId
            val totalFrames = audioEngine.getSampleLength(instId)
            val waveformData = audioEngine.getSampleWaveform(instId, 620)
            val sampleRate   = audioEngine.getOriginalSampleRate(instId)
            val inst = trackerController.project.instruments[instId]
            sampleEditorState = sampleEditorState.copy(
                totalFrames  = totalFrames,
                waveformData = waveformData,
                sampleRate   = sampleRate,
                selectionStart = (inst.sampleStart.toLong() * totalFrames) / 255L,
                selectionEnd   = (inst.sampleEnd.toLong()   * totalFrames) / 255L
            )
        }
    }

    // Reload zoomed waveform when zoom level or view window changes
    val sampleEditorZoom      = sampleEditorState.zoomLevel
    val sampleEditorViewStart = sampleEditorState.viewStart
    LaunchedEffect(sampleEditorZoom, sampleEditorViewStart) {
        if (trackerController.currentScreen == ScreenType.SAMPLE_EDITOR && sampleEditorState.totalFrames > 0) {
            val instId     = sampleEditorState.instrumentId
            val totalFrames = sampleEditorState.totalFrames
            // Compute viewEnd from the captured keys to stay consistent
            val viewEnd = if (sampleEditorZoom == 0) totalFrames.toLong()
                          else (sampleEditorViewStart + (totalFrames.toLong() ushr sampleEditorZoom))
                                  .coerceAtMost(totalFrames.toLong())
            val waveformData = if (sampleEditorZoom == 0) {
                audioEngine.getSampleWaveform(instId, 620)
            } else {
                audioEngine.getSampleWaveformRange(
                    instId,
                    sampleEditorViewStart.toInt(),
                    viewEnd.toInt(),
                    620
                )
            }
            sampleEditorState = sampleEditorState.copy(waveformData = waveformData)
        }
    }

    // Poll playback position for real-time waveform marker (~30fps)
    LaunchedEffect(Unit) {
        while (true) {
            if (trackerController.currentScreen == ScreenType.SAMPLE_EDITOR) {
                val pos = audioEngine.getSamplePlaybackPosition(sampleEditorState.sampleId)
                if (pos != sampleEditorState.playbackPosition) {
                    sampleEditorState = sampleEditorState.copy(playbackPosition = pos)
                }
            }
            delay(33)
        }
    }

    // (Audio engine cleanup moved to line 168-172 with new architecture)

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
    // INPUT ACTION HELPERS
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Apply InputAction to phrase step
     *
     * Handles value changes for phrase editing (notes, volume, instrument)
     */
    /**
     * Apply input action to file browser (character editing in RENAME/CREATE mode)
     */
    fun applyFileBrowserInputAction(action: InputAction) {
        when (action) {
            is InputAction.SET_VALUE -> {
                // Set character at renameCursor position
                val char = action.value.toChar()
                val buffer = fileBrowserState.renameBuffer.padEnd(12, ' ')
                val sb = StringBuilder(buffer)
                if (fileBrowserState.renameCursor < sb.length) {
                    sb.setCharAt(fileBrowserState.renameCursor, char)
                    fileBrowserState = fileBrowserState.copy(
                        renameBuffer = sb.toString().trimEnd()
                    )
                }
            }
            is InputAction.DELETE -> {
                // Delete character at cursor (replace with space, trim end)
                val buffer = fileBrowserState.renameBuffer.padEnd(12, ' ')
                val sb = StringBuilder(buffer)
                if (fileBrowserState.renameCursor < sb.length) {
                    sb.setCharAt(fileBrowserState.renameCursor, ' ')
                    fileBrowserState = fileBrowserState.copy(
                        renameBuffer = sb.toString().trimEnd()
                    )
                }
            }
            else -> { /* Other actions not applicable */ }
        }
    }

    /**
     * Sync all track and master volumes to the audio backend.
     *
     * This ensures the C++ real-time volume array matches the project data.
     * Called on project load and app initialization.
     */
    fun syncVolumesToAudioBackend() {
        val project = trackerController.project

        // Sync all 8 track volumes
        for (i in 0 until 8) {
            val vol = project.tracks[i].volume
            audioBackend.setTrackVolume(i, com.conanizer.pockettracker.core.data.VolumeUtils.hexToFloat(vol))
        }

        // Sync master volume
        audioBackend.setMasterVolume(com.conanizer.pockettracker.core.data.VolumeUtils.hexToFloat(project.masterVolume))

        // Sync OTT depth and master bus FX selection
        audioBackend.setOttDepth(project.ottDepth)
        audioBackend.setMasterFx(project.masterBusFx)
        audioBackend.setDustDepth(project.dustDepth)

        Log.d("VolumeSync", "Synced all track/master volumes to audio backend")
    }

    /**
     * Reload all custom samples from a loaded project
     *
     * When a project is loaded from JSON, the sampleFilePath is preserved but the actual
     * WAV files aren't in memory. This function iterates through all instruments and
     * reloads any custom samples (instruments with non-null sampleFilePath).
     *
     * Also restores all instrument parameters (ROOT, DETUNE, START, END, REVERSE, LOOP).
     *
     * Should be called immediately after loading a project.
     */
    fun reloadProjectSamples() {
        var loadedCount = 0
        var failedCount = 0

        // IMPORTANT: Use trackerController.project directly, not the local 'project' val
        // The local 'project' is captured at composition time and may be stale
        trackerController.project.instruments.forEach { instrument ->
            if (instrument.instrumentType == com.conanizer.pockettracker.core.data.InstrumentType.SOUNDFONT &&
                instrument.soundfontPath != null) {
                // Reload soundfont and repopulate sfSlotMap
                val path = instrument.soundfontPath!!
                val slot = audioEngine.backend.loadSoundfont(instrument.id, path)
                if (slot >= 0) {
                    instrumentController.sfSlotMap[path] = slot
                    // If bank/preset was never saved (both 0) verify first preset exists; init if not
                    val firstPreset = audioEngine.backend.getSoundfontFirstBankPreset(slot)
                    if (firstPreset[0] >= 0 &&
                        audioEngine.backend.getSoundfontPresetName(slot, instrument.sfBank, instrument.sfPreset) == "---") {
                        instrument.sfBank   = firstPreset[0]
                        instrument.sfPreset = firstPreset[1]
                    }
                    loadedCount++
                    Log.d("ProjectLoad", "✅ Reloaded soundfont for instrument ${instrument.id.toString(16).padStart(2, '0')}: $path")
                } else {
                    failedCount++
                    Log.e("ProjectLoad", "❌ Failed to reload soundfont for instrument ${instrument.id.toString(16).padStart(2, '0')}: $path")
                }
            } else if (instrument.sampleFilePath != null) {
                val filePath = instrument.sampleFilePath!!
                val success = audioEngine.loadSampleFromFile(instrument.id, filePath)

                if (success) {
                    // Restore instrument parameters after loading sample
                    audioEngine.updateInstrumentBaseFrequency(instrument)
                    audioEngine.updateInstrumentPlaybackParams(instrument)

                    loadedCount++
                    Log.d("ProjectLoad", "✅ Reloaded sample for instrument ${instrument.id.toString(16).padStart(2, '0')}: $filePath")
                } else {
                    failedCount++
                    Log.e("ProjectLoad", "❌ Failed to reload sample for instrument ${instrument.id.toString(16).padStart(2, '0')}: $filePath")
                }
            }
        }

        if (loadedCount > 0 || failedCount > 0) {
            Log.d("ProjectLoad", "Sample reload complete: $loadedCount loaded, $failedCount failed")
        }

        // Sync all track/master volumes to audio backend
        syncVolumesToAudioBackend()
    }

    // ─────────────────────────────────────────────────────────────────────────
    // FX HELPER OVERLAY HELPERS
    // ─────────────────────────────────────────────────────────────────────────

    /** True if cursor is currently on an FX type column in PHRASE or TABLE. */
    fun isOnFxTypeColumn(): Boolean = when (trackerController.currentScreen) {
        ScreenType.PHRASE -> trackerController.cursorColumn == 4 ||
                             trackerController.cursorColumn == 6 ||
                             trackerController.cursorColumn == 8
        ScreenType.TABLE  -> trackerController.tableCursorColumn == 3 ||
                             trackerController.tableCursorColumn == 5 ||
                             trackerController.tableCursorColumn == 7
        else -> false
    }

    /** Returns the EFFECT_TYPES index for the FX type currently under the cursor. */
    fun getCurrentFxTypeIndex(): Int {
        val code = when (trackerController.currentScreen) {
            ScreenType.PHRASE -> {
                val step = trackerController.project.phrases[trackerController.currentPhrase]
                    .steps[trackerController.cursorRow]
                when (trackerController.cursorColumn) {
                    4 -> step.fx1Type
                    6 -> step.fx2Type
                    8 -> step.fx3Type
                    else -> 0
                }
            }
            ScreenType.TABLE -> {
                val row = trackerController.project.tables[trackerController.currentTable]
                    .rows[trackerController.tableCursorRow]
                when (trackerController.tableCursorColumn) {
                    3 -> row.fx1Type
                    5 -> row.fx2Type
                    7 -> row.fx3Type
                    else -> 0
                }
            }
            else -> 0
        }
        val idx = EffectProcessor.EFFECT_TYPES.indexOf(code)
        return if (idx < 0) 0 else idx
    }

    /** Writes [effectCode] into the FX type column currently under cursor. */
    fun applyFxTypeChange(effectCode: Int) {
        when (trackerController.currentScreen) {
            ScreenType.PHRASE -> {
                val step = trackerController.project.phrases[trackerController.currentPhrase]
                    .steps[trackerController.cursorRow]
                when (trackerController.cursorColumn) {
                    4 -> step.fx1Type = effectCode
                    6 -> step.fx2Type = effectCode
                    8 -> step.fx3Type = effectCode
                }
                trackerController.projectVersion++
            }
            ScreenType.TABLE -> {
                val row = trackerController.project.tables[trackerController.currentTable]
                    .rows[trackerController.tableCursorRow]
                when (trackerController.tableCursorColumn) {
                    3 -> row.fx1Type = effectCode
                    5 -> row.fx2Type = effectCode
                    7 -> row.fx3Type = effectCode
                }
                audioEngine.invalidateTable(trackerController.currentTable)
                trackerController.projectVersion++
            }
            else -> {}
        }
    }

    /**
     * Generic input handler that routes button presses through cursor context
     *
     * This helper eliminates code duplication by:
     * 1. Getting the cursor context for the current screen
     * 2. Calling the provided handler function (handleAButton, handleBButton, etc.)
     * 3. Applying the resulting action to the appropriate screen
     *
     * Usage example:
     *   onAUp = { handleGenericInput { ctx -> trackerController.inputController.handleAButton(ctx) } }
     *   onADown = { handleGenericInput { ctx -> trackerController.inputController.handleBButton(ctx) } }
     *
     * @param handlerFunction Lambda that takes CursorContext and returns InputAction
     */
    fun handleGenericInput(handlerFunction: (CursorContext) -> InputAction) {
        // EQ editor takes priority over all screens
        if (eqEditorState.isOpen) {
            val eqState = EqState(
                project       = trackerController.project,
                slotIndex     = eqEditorState.slotIndex,
                cursorRow     = eqEditorState.cursorRow,
                callerContext = eqEditorState.callerContext
            )
            val context = eqModule.getCursorContext(eqState)
            val action  = handlerFunction(context)
            val result  = eqModule.handleInput(eqState, action) { trackerController.projectVersion++ }
            if (result.eqBandChanged) {
                val slot    = eqEditorState.slotIndex
                val bandIdx = eqEditorState.cursorBand
                val band    = trackerController.project.eqPresets[slot].bands[bandIdx]
                audioBackend.setEqBand(slot, bandIdx, band.type, band.freq, band.gain, band.q)
                // Re-apply preset to the DSP processor that's using this slot
                when (val ctx = eqEditorState.callerContext) {
                    is EqCallerContext.MasterEq      -> audioBackend.setMasterEqSlot(slot)
                    is EqCallerContext.ReverbInputEq -> audioBackend.setReverbInputEq(slot)
                    is EqCallerContext.DelayInputEq  -> audioBackend.setDelayInputEq(slot)
                    is EqCallerContext.InstrumentEq  -> audioBackend.setInstrumentEqSlot(ctx.instrId, slot)
                }
                trackerController.projectVersion++
            }
            return
        }

        // Read directly from trackerController to avoid stale captured values
        when (trackerController.currentScreen) {
            ScreenType.CHAIN -> {
                val chainState = ChainEditorState(
                    trackerController.project.chains[trackerController.currentChain],
                    trackerController.cursorRow,
                    trackerController.cursorColumn
                )
                val context = chainEditorModule.getCursorContext(chainState)
                val action = handlerFunction(context)
                val result = chainEditorModule.handleInput(chainState, action)
                if (result.modified) {
                    result.lastEditedPhrase?.let { trackerController.lastEditedPhrase = it }
                    result.lastEditedTranspose?.let { trackerController.lastEditedTranspose = it }
                    trackerController.projectVersion++
                }
            }
            ScreenType.PHRASE -> {
                val phraseState = PhraseEditorState(
                    trackerController.project.phrases[trackerController.currentPhrase],
                    trackerController.cursorRow,
                    trackerController.cursorColumn,
                    playbackRow = 0,
                    isPlaying = trackerController.isPlaying()
                )
                val context = phraseEditorModule.getCursorContext(phraseState)
                val action = handlerFunction(context)
                val result = phraseEditorModule.handleInput(phraseState, action, instrumentController)
                if (result.modified) {
                    // When any of note/vol/inst changes, capture the whole step combo
                    // so quick-insert always reflects what was actually on that step
                    if (result.lastEditedNote != null || result.lastEditedVolume != null || result.lastEditedInstrument != null) {
                        val step = trackerController.project.phrases[trackerController.currentPhrase].steps[trackerController.cursorRow]
                        if (step.note != com.conanizer.pockettracker.core.data.Note.EMPTY) {
                            trackerController.lastEditedNote = step.note
                            trackerController.lastEditedVolume = step.volume
                            trackerController.lastEditedInstrument = step.instrument
                        }
                    }
                    trackerController.projectVersion++
                }
            }
            ScreenType.SONG -> {
                val songState = SongEditorState(
                    trackerController.project,
                    trackerController.cursorRow,
                    cursorTrack = trackerController.cursorColumn,
                    scrollPosition = trackerController.songScrollPosition
                )
                val context = songEditorModule.getCursorContext(songState)
                val action = handlerFunction(context)
                val result = songEditorModule.handleInput(songState, action)
                if (result.modified) {
                    result.lastEditedChain?.let { trackerController.lastEditedChain = it }
                    trackerController.projectVersion++
                }
            }
            ScreenType.PROJECT -> {
                val projectState = ProjectState(
                    project = trackerController.project,
                    cursorRow = trackerController.projectCursorRow,
                    cursorColumn = trackerController.projectCursorColumn,
                    statusMessage = trackerController.statusMessage,
                    isSuccess = trackerController.statusSuccess,
                    isRendering = isRendering,
                    renderProgress = renderProgress
                )
                val context = projectModule.getCursorContext(projectState)
                val action = handlerFunction(context)
                val result = projectModule.handleInput(projectState, action)
                if (result.modified) {
                    trackerController.projectVersion++
                }
            }
            ScreenType.SETTINGS -> {
                val settingsState = SettingsState(
                    cursorRow = trackerController.settingsCursorRow,
                    cursorColumn = trackerController.settingsCursorColumn,
                    layoutMode = layoutMode,
                    scalingMode = scalingMode,
                    buttonSoundEnabled = buttonSoundEnabled,
                    buttonSoundVolume = buttonSoundVolume,
                    buttonVibroEnabled = buttonVibroEnabled,
                    vibroPower = vibroPower,
                    insertBefore = insertBefore,
                    cursorRemember = cursorRemember
                )
                val context = settingsModule.getCursorContext(settingsState)
                val action = handlerFunction(context)
                val result = settingsModule.handleInput(settingsState, action)
                if (result.modified) {
                    result.buttonSoundEnabled?.let { buttonSoundEnabled = it }
                    result.buttonSoundVolume?.let  { buttonSoundVolume  = it }
                    result.buttonVibroEnabled?.let { buttonVibroEnabled = it }
                    result.vibroPower?.let         { vibroPower         = it }
                    result.insertBefore?.let       { insertBefore       = it }
                    result.cursorRemember?.let     { cursorRemember     = it }
                    trackerController.projectVersion++
                }
            }
            ScreenType.FILE_BROWSER -> {
                // Only handle generic input when in RENAME or CREATE mode (character editing)
                if (fileBrowserState.mode == FileBrowserModule.BrowserMode.RENAME ||
                    fileBrowserState.mode == FileBrowserModule.BrowserMode.CREATE) {
                    val context = fileBrowserModule.getCursorContext(fileBrowserState)
                    val action = handlerFunction(context)
                    applyFileBrowserInputAction(action)
                }
            }
            ScreenType.SAMPLE_EDITOR -> {
                val context = sampleEditorModule.getCursorContext(sampleEditorState)
                val action = handlerFunction(context)
                val result = sampleEditorModule.handleInput(sampleEditorState, action)
                // View settings (zoom, source, snap, pitch, slice params, fx type/value) do
                // NOT set isModified — only destructive buffer operations do.
                if (result.modified) {
                    val prevRateMode = sampleEditorState.rateMode
                    sampleEditorState = sampleEditorState.applyResult(result)
                    // RATE change: re-derive buffer from cached original so toggling back to HIGH
                    // is always lossless. No stepFactor arithmetic needed — C++ handles it all.
                    if (result.rateMode != null && sampleEditorState.rateMode != prevRateMode) {
                        val newFactor = when (sampleEditorState.rateMode) { 1 -> 2; 2 -> 4; else -> 1 }
                        val instId = sampleEditorState.instrumentId
                        audioEngine.applyRateMode(instId, newFactor)
                        sampleEditorState = sampleEditorState.copy(
                            totalFrames  = audioEngine.getSampleLength(instId),
                            waveformData = audioEngine.getSampleWaveform(instId, 620),
                            isModified   = true
                        )
                    }
                }
            }
            ScreenType.INSTRUMENT -> {
                val inst = trackerController.project.instruments[trackerController.currentInstrument]
                val instrumentState = InstrumentState(
                    instrument = inst,
                    cursorRow = trackerController.instrumentCursorRow,
                    cursorColumn = trackerController.instrumentCursorColumn,
                    statusMessage = trackerController.statusMessage,
                    isSuccess = trackerController.statusSuccess,
                    soundfontPresetName  = instrumentController.getSoundfontPresetName(trackerController.project),
                    soundfontPresetCount = instrumentController.getSoundfontPresetCount(inst),
                    soundfontPresetIndex = instrumentController.getSoundfontCurrentPresetIndex(inst)
                )
                val context = instrumentModule.getCursorContext(instrumentState)
                val action = handlerFunction(context)
                val result = instrumentModule.handleInput(instrumentState, action, instrumentController)
                if (result.modified) {
                    trackerController.projectVersion++
                }
            }
            ScreenType.MIXER -> {
                val mixerState = MixerState(
                    project        = trackerController.project,
                    cursorColumn   = trackerController.mixerCursorColumn,
                    mixerMasterRow = trackerController.mixerMasterRow,
                    trackPeaks     = trackPeakBuffer,
                    masterPeaks    = masterPeakBuffer,
                    reverbPeaks    = floatArrayOf(sendPeakBuffer[0], sendPeakBuffer[1]),
                    delayPeaks     = floatArrayOf(sendPeakBuffer[2], sendPeakBuffer[3])
                )
                val context = mixerModule.getCursorContext(mixerState)
                val action = handlerFunction(context)
                val result = mixerModule.handleInput(mixerState, action) {
                    trackerController.projectVersion++
                }
                // Sync real-time audio params to backend if modified
                if (result.modified) {
                    val proj = trackerController.project
                    val cursorCol = trackerController.mixerCursorColumn
                    val masterRow = trackerController.mixerMasterRow
                    when {
                        result.masterEqChanged -> {
                            val slot = proj.masterEqSlot
                            if (slot >= 0) audioBackend.setMasterEqSlot(slot)
                            trackerController.projectVersion++
                        }
                        result.ottDepthChanged -> {
                            audioBackend.setOttDepth(proj.ottDepth)
                            trackerController.projectVersion++
                        }
                        result.dustDepthChanged -> {
                            audioBackend.setDustDepth(proj.dustDepth)
                            trackerController.projectVersion++
                        }
                        result.reverbWetChanged -> {
                            audioBackend.setReverbParams(proj.reverbFeedback, proj.reverbDamp, proj.reverbWet)
                            trackerController.projectVersion++
                        }
                        result.delayWetChanged -> {
                            audioBackend.setDelayParams(proj.delayTime, proj.delayFeedback, proj.delaySync, proj.tempo.toFloat(), proj.delayWet)
                            trackerController.projectVersion++
                        }
                        masterRow == 0 && cursorCol < 8 -> {
                            val vol = proj.tracks[cursorCol].volume
                            audioBackend.setTrackVolume(cursorCol, com.conanizer.pockettracker.core.data.VolumeUtils.hexToFloat(vol))
                        }
                        masterRow == 0 -> {
                            audioBackend.setMasterVolume(com.conanizer.pockettracker.core.data.VolumeUtils.hexToFloat(proj.masterVolume))
                        }
                    }
                }
            }
            ScreenType.EFFECTS -> {
                val effectState = EffectState(
                    project   = trackerController.project,
                    cursorRow = trackerController.effectsCursorRow
                )
                val context = effectModule.getCursorContext(effectState)
                val action  = handlerFunction(context)
                val result  = effectModule.handleInput(effectState, action) {
                    trackerController.projectVersion++
                }
                if (result.modified) {
                    val proj = trackerController.project
                    when {
                        result.masterFxChanged -> {
                            audioBackend.setMasterFx(proj.masterBusFx)
                            trackerController.projectVersion++
                        }
                        result.reverbParamsChanged -> {
                            audioBackend.setReverbParams(proj.reverbFeedback, proj.reverbDamp, proj.reverbWet)
                            if (proj.reverbInputEq >= 0) audioBackend.setReverbInputEq(proj.reverbInputEq)
                            trackerController.projectVersion++
                        }
                        result.delayReverbSendChanged -> {
                            audioBackend.setDelayReverbSend(proj.delayReverbSend)
                            trackerController.projectVersion++
                        }
                        result.delayParamsChanged -> {
                            audioBackend.setDelayParams(
                                proj.delayTime, proj.delayFeedback,
                                proj.delaySync, proj.tempo.toFloat(), proj.delayWet
                            )
                            if (proj.delayInputEq >= 0) audioBackend.setDelayInputEq(proj.delayInputEq)
                            trackerController.projectVersion++
                        }
                        result.masterEqChanged -> {
                            if (proj.masterEqSlot >= 0) audioBackend.setMasterEqSlot(proj.masterEqSlot)
                            trackerController.projectVersion++
                        }
                    }
                }
            }
            ScreenType.TABLE -> {
                val tableState = TableState(
                    trackerController.project.tables[trackerController.currentTable],
                    trackerController.tableCursorRow,
                    trackerController.tableCursorColumn,
                    playbackRow = null,  // TODO: Table playback row tracking
                    ticRate = trackerController.project.instruments.getOrNull(trackerController.currentInstrument)?.tableTicRate ?: 0x06,
                    selectionMode = trackerController.inputController.isSelectionModeActive(),
                    isCellSelected = { row, col -> trackerController.inputController.isCellSelected(row, col) }
                )
                val context = tableModule.getCursorContext(tableState)
                val action = handlerFunction(context)
                val result = tableModule.handleInput(tableState, action)
                if (result.modified) {
                    trackerController.projectVersion++
                    // Invalidate table cache so changes are reloaded to native on next preview/playback
                    audioEngine.invalidateTable(trackerController.currentTable)
                }
            }
            ScreenType.GROOVE -> {
                val grooveState = GrooveState(
                    groove = trackerController.project.grooves[trackerController.currentGroove],
                    cursorRow = trackerController.grooveCursorRow,
                    cursorColumn = 1
                )
                val context = grooveModule.getCursorContext(grooveState)
                val action = handlerFunction(context)
                val result = grooveModule.handleInput(grooveState, action)
                if (result.modified) {
                    trackerController.projectVersion++
                }
            }
            ScreenType.MODS -> {
                val modState = ModulationState(
                    instrument = trackerController.project.instruments[trackerController.currentInstrument],
                    cursorRow = trackerController.modCursorRow,
                    cursorPair = trackerController.modCursorPair,
                    cursorSide = trackerController.modCursorSide
                )
                val context = modulationModule.getCursorContext(modState)
                val action = handlerFunction(context)
                val result = modulationModule.handleInput(modState, action)
                if (result.modified) {
                    trackerController.projectVersion++
                }
            }
            else -> { /* Other screens not yet implemented */ }
        }
    }

    /**
     * Handle A+DPAD with selection awareness.
     *
     * When selection is active, applies increment/decrement to ALL selected rows
     * in the cursor's column. When no selection, delegates to handleGenericInput.
     */
    fun handleSelectionOrSingleIncrement(handlerFunction: (CursorContext) -> InputAction) {
        if (!trackerController.inputController.isSelectionModeActive()) {
            handleGenericInput(handlerFunction)
            return
        }

        val bounds = trackerController.inputController.getSelectionBounds() ?: return

        // Determine which cursor row property to use based on current screen
        when (trackerController.currentScreen) {
            ScreenType.PHRASE, ScreenType.CHAIN, ScreenType.SONG -> {
                val savedRow = trackerController.cursorRow
                for (row in bounds.topLeftRow..bounds.bottomRightRow) {
                    trackerController.cursorRow = row
                    handleGenericInput(handlerFunction)
                }
                trackerController.cursorRow = savedRow
            }
            ScreenType.TABLE -> {
                val savedRow = trackerController.tableCursorRow
                for (row in bounds.topLeftRow..bounds.bottomRightRow) {
                    trackerController.tableCursorRow = row
                    handleGenericInput(handlerFunction)
                }
                trackerController.tableCursorRow = savedRow
            }
            else -> handleGenericInput(handlerFunction)
        }
    }

    /**
     * Apply an InputAction based on current screen and context
     *
     * This dispatcher handles InputAction objects returned by InputController.
     * It bridges the gap between input interpretation and state management.
     *
     * Routes NAVIGATE actions to appropriate TrackerController methods.
     */
    fun applyInputAction(action: InputAction) {
        when (action) {
            is InputAction.NAVIGATE_UP -> {
                when (trackerController.currentScreen) {
                    ScreenType.FILE_BROWSER -> {
                        // File browser: move cursor up
                        if (fileBrowserState.items.isNotEmpty()) {
                            val newCursor = if (fileBrowserState.cursor > 0) {
                                fileBrowserState.cursor - 1
                            } else {
                                fileBrowserState.items.size - 1
                            }
                            val newScroll = when {
                                newCursor < fileBrowserState.scroll -> newCursor
                                newCursor >= fileBrowserState.scroll + FileBrowserModule.VISIBLE_ROWS ->
                                    (newCursor - FileBrowserModule.VISIBLE_ROWS + 1).coerceAtLeast(0)
                                else -> fileBrowserState.scroll
                            }
                            fileBrowserState = fileBrowserState.copy(cursor = newCursor, scroll = newScroll)
                        }
                    }
                    ScreenType.SAMPLE_EDITOR -> {
                        val newRow = SampleEditorModule.rowAbove(sampleEditorState.cursorRow, sampleEditorState.sliceMethod)
                        sampleEditorState = sampleEditorState.copy(
                            cursorRow = newRow,
                            cursorCol = sampleEditorState.cursorCol.coerceAtMost(
                                SampleEditorModule.maxColForRow(newRow, sampleEditorState.sliceMethod))
                        )
                    }
                    else -> trackerController.moveCursorUp()
                }
            }
            is InputAction.NAVIGATE_DOWN -> {
                when (trackerController.currentScreen) {
                    ScreenType.FILE_BROWSER -> {
                        // File browser: move cursor down
                        if (fileBrowserState.items.isNotEmpty()) {
                            val newCursor = if (fileBrowserState.cursor < fileBrowserState.items.size - 1) {
                                fileBrowserState.cursor + 1
                            } else {
                                0
                            }
                            val newScroll = when {
                                newCursor >= fileBrowserState.scroll + FileBrowserModule.VISIBLE_ROWS ->
                                    newCursor - FileBrowserModule.VISIBLE_ROWS + 1
                                newCursor < fileBrowserState.scroll -> newCursor
                                else -> fileBrowserState.scroll
                            }
                            fileBrowserState = fileBrowserState.copy(cursor = newCursor, scroll = newScroll)
                        }
                    }
                    ScreenType.SAMPLE_EDITOR -> {
                        val newRow = SampleEditorModule.rowBelow(sampleEditorState.cursorRow, sampleEditorState.sliceMethod)
                        sampleEditorState = sampleEditorState.copy(
                            cursorRow = newRow,
                            cursorCol = sampleEditorState.cursorCol.coerceAtMost(
                                SampleEditorModule.maxColForRow(newRow, sampleEditorState.sliceMethod))
                        )
                    }
                    else -> trackerController.moveCursorDown()
                }
            }
            is InputAction.NAVIGATE_LEFT -> {
                when (trackerController.currentScreen) {
                    ScreenType.FILE_BROWSER -> {
                        // File browser: page up or edit character position
                        when (fileBrowserState.mode) {
                            FileBrowserModule.BrowserMode.RENAME, FileBrowserModule.BrowserMode.CREATE -> {
                                if (fileBrowserState.renameCursor > 0) {
                                    fileBrowserState = fileBrowserState.copy(renameCursor = fileBrowserState.renameCursor - 1)
                                }
                            }
                            else -> {
                                if (fileBrowserState.items.isNotEmpty()) {
                                    val newCursor = (fileBrowserState.cursor - FileBrowserModule.VISIBLE_ROWS).coerceAtLeast(0)
                                    val newScroll = when {
                                        newCursor < fileBrowserState.scroll -> newCursor
                                        newCursor >= fileBrowserState.scroll + FileBrowserModule.VISIBLE_ROWS ->
                                            (newCursor - FileBrowserModule.VISIBLE_ROWS + 1).coerceAtLeast(0)
                                        else -> fileBrowserState.scroll
                                    }
                                    fileBrowserState = fileBrowserState.copy(cursor = newCursor, scroll = newScroll)
                                }
                            }
                        }
                    }
                    ScreenType.SAMPLE_EDITOR -> {
                        val row = sampleEditorState.cursorRow
                        val maxCol = SampleEditorModule.maxColForRow(row, sampleEditorState.sliceMethod)
                        val newCol = if (row in 13..14) {
                            if (sampleEditorState.cursorCol == 0) maxCol else sampleEditorState.cursorCol - 1
                        } else {
                            (sampleEditorState.cursorCol - 1).coerceAtLeast(0)
                        }
                        sampleEditorState = sampleEditorState.copy(cursorCol = newCol)
                    }
                    else -> trackerController.moveCursorLeft()
                }
            }
            is InputAction.NAVIGATE_RIGHT -> {
                when (trackerController.currentScreen) {
                    ScreenType.FILE_BROWSER -> {
                        // File browser: page down or edit character position
                        when (fileBrowserState.mode) {
                            FileBrowserModule.BrowserMode.RENAME, FileBrowserModule.BrowserMode.CREATE -> {
                                if (fileBrowserState.renameCursor < 11) {
                                    fileBrowserState = fileBrowserState.copy(renameCursor = fileBrowserState.renameCursor + 1)
                                }
                            }
                            else -> {
                                if (fileBrowserState.items.isNotEmpty()) {
                                    val newCursor = (fileBrowserState.cursor + FileBrowserModule.VISIBLE_ROWS)
                                        .coerceAtMost(fileBrowserState.items.size - 1)
                                    val newScroll = when {
                                        newCursor >= fileBrowserState.scroll + FileBrowserModule.VISIBLE_ROWS ->
                                            newCursor - FileBrowserModule.VISIBLE_ROWS + 1
                                        newCursor < fileBrowserState.scroll -> newCursor
                                        else -> fileBrowserState.scroll
                                    }
                                    fileBrowserState = fileBrowserState.copy(cursor = newCursor, scroll = newScroll)
                                }
                            }
                        }
                    }
                    ScreenType.SAMPLE_EDITOR -> {
                        val row = sampleEditorState.cursorRow
                        val maxCol = SampleEditorModule.maxColForRow(row, sampleEditorState.sliceMethod)
                        val newCol = if (row in 13..14) {
                            (sampleEditorState.cursorCol + 1) % (maxCol + 1)
                        } else {
                            (sampleEditorState.cursorCol + 1).coerceAtMost(maxCol)
                        }
                        sampleEditorState = sampleEditorState.copy(cursorCol = newCol)
                    }
                    else -> trackerController.moveCursorRight()
                }
            }
            // Other actions are handled by handleGenericInput or specific handlers
            else -> { /* No action needed for other types */ }
        }
    }

    /**
     * DPAD navigation handler - gets proper context and dispatches NAVIGATE action
     * 
     * Unlike handleGenericInput, this directly calls applyInputAction() because
     * modules don't understand NAVIGATE_* actions - those are screen-level navigation.
     *
     * @param handlerFunction Lambda that takes CursorContext and returns InputAction (typically a NAVIGATE_* action)
     */
    fun handleDPadNavigation(handlerFunction: () -> InputAction) {
        // Check if in selection mode first - expand selection instead of navigating
        if (trackerController.inputController.isSelectionModeActive()) {
            val action = handlerFunction()
            val direction = when (action) {
                InputAction.NAVIGATE_UP -> "UP"
                InputAction.NAVIGATE_DOWN -> "DOWN"
                InputAction.NAVIGATE_LEFT -> "LEFT"
                InputAction.NAVIGATE_RIGHT -> "RIGHT"
                else -> null
            }
            if (direction != null) {
                val maxColumn = when (trackerController.currentScreen) {
                    ScreenType.PHRASE -> 9
                    ScreenType.CHAIN -> 2
                    ScreenType.SONG -> 8
                    else -> 1
                }
                trackerController.inputController.expandSelection(direction, 15, maxColumn)
                return
            }
        }

        // DPAD navigation: Dispatch NAVIGATE_* actions directly via applyInputAction()
        // (NOT to module.handleInput() since modules don't understand NAVIGATE actions)
        //
        // Note: DPAD movement is screen-aware (handled by trackerController),
        // not cursor-state-aware. Movement bounds are enforced per-screen, not per-context.
        val action = handlerFunction()
        applyInputAction(action)
    }

    // ═══════════════════════════════════════════════════════════════════════
    // EQ EDITOR HELPERS
    // ═══════════════════════════════════════════════════════════════════════

    fun openEqEditor(slot: Int, caller: EqCallerContext) {
        eqEditorState = EqEditorState(
            isOpen = true,
            slotIndex = slot.coerceIn(0, 127),
            callerContext = caller
        )
    }

    fun applyCallerEqSlotChange(newSlot: Int) {
        val proj = trackerController.project
        when (val ctx = eqEditorState.callerContext) {
            is EqCallerContext.MasterEq      -> { proj.masterEqSlot = newSlot; audioBackend.setMasterEqSlot(newSlot) }
            is EqCallerContext.ReverbInputEq -> { proj.reverbInputEq = newSlot; audioBackend.setReverbInputEq(newSlot) }
            is EqCallerContext.DelayInputEq  -> { proj.delayInputEq = newSlot; audioBackend.setDelayInputEq(newSlot) }
            is EqCallerContext.InstrumentEq  -> { proj.instruments[ctx.instrId].eqSlot = newSlot; audioBackend.setInstrumentEqSlot(ctx.instrId, newSlot) }
        }
        trackerController.projectVersion++
    }

    // ═══════════════════════════════════════════════════════════════════════
    // BUTTON HANDLERS
    // ═══════════════════════════════════════════════════════════════════════

    // Create a ButtonHandlers object that contains all button press logic
    // Remove dependencies - handlers can access current values directly from scope
    // This prevents InputMapper from being recreated and losing modifier states
    val buttonHandlers = remember {
        ButtonHandlers(
            onDPadUp = {
                if (qwertyKeyboardState.isOpen) { qwertyKeyboardState = qwertyKeyboardState.moveCursorUp() }
                else if (showCleanDialog) { cleanDialogCursor = 0 }
                else if (eqEditorState.isOpen) { eqEditorState = eqEditorState.copy(cursorRow = (eqEditorState.cursorRow - 1).coerceAtLeast(0)) }
                else handleDPadNavigation { trackerController.inputController.handleDPadUp() }
            },

            // ───────────────────────────────────────────────────────────────
            // D-PAD DOWN
            // ───────────────────────────────────────────────────────────────
            onDPadDown = {
                if (qwertyKeyboardState.isOpen) { qwertyKeyboardState = qwertyKeyboardState.moveCursorDown() }
                else if (showCleanDialog) { cleanDialogCursor = 1 }
                else if (eqEditorState.isOpen) { eqEditorState = eqEditorState.copy(cursorRow = (eqEditorState.cursorRow + 1).coerceAtMost(EqModule.MAX_CURSOR_ROW)) }
                else handleDPadNavigation { trackerController.inputController.handleDPadDown() }
            },

            // ───────────────────────────────────────────────────────────────
            // D-PAD LEFT
            // ───────────────────────────────────────────────────────────────
            onDPadLeft = {
                if (qwertyKeyboardState.isOpen) { qwertyKeyboardState = qwertyKeyboardState.moveCursorLeft() }
                else if (eqEditorState.isOpen) { /* no-op: B+LEFT handles preset cycling */ }
                else handleDPadNavigation { trackerController.inputController.handleDPadLeft() }
            },

            // ───────────────────────────────────────────────────────────────
            // D-PAD RIGHT
            // ───────────────────────────────────────────────────────────────
            onDPadRight = {
                if (qwertyKeyboardState.isOpen) { qwertyKeyboardState = qwertyKeyboardState.moveCursorRight() }
                else if (eqEditorState.isOpen) { /* no-op: B+RIGHT handles preset cycling */ }
                else handleDPadNavigation { trackerController.inputController.handleDPadRight() }
            },

// ───────────────────────────────────────────────────────────────
// BUTTON A - Primary action (insert/increment)
// ───────────────────────────────────────────────────────────────
            onButtonA = { run buttonA@{
                // ── QWERTY keyboard takes priority ──────────────────────────
                if (qwertyKeyboardState.isOpen) {
                    qwertyKeyboardState = qwertyKeyboardState.insertCurrentKey()
                    return@buttonA
                }

                // ── Clean dialog takes priority ──────────────────────────
                if (showCleanDialog) {
                    if (cleanDialogCursor == 0) {
                        // YES: Execute clean
                        val target = cleanDialogTarget
                        showCleanDialog = false
                        if (target == "SEQ") trackerController.cleanUnusedSeq()
                        else trackerController.cleanUnusedInst()
                    } else {
                        // NO: dismiss
                        showCleanDialog = false
                    }
                    return@buttonA
                }

                // SAMPLE EDITOR confirm-close dialog: A = YES (close without saving)
                if (trackerController.currentScreen == ScreenType.SAMPLE_EDITOR && sampleEditorState.showConfirmClose) {
                    sampleEditorState = sampleEditorState.copy(showConfirmClose = false, isModified = false)
                    trackerController.currentScreen = previousScreen
                    return@buttonA
                }

                // Read directly from trackerController to avoid stale captured values
                when (trackerController.currentScreen) {
                    // FILE BROWSER: Open folder, load file, or confirm actions
                    ScreenType.FILE_BROWSER -> {
                        when (fileBrowserState.mode) {
                            FileBrowserModule.BrowserMode.NORMAL -> {
                                // Open folder or load file
                                val item = fileBrowserState.items.getOrNull(fileBrowserState.cursor)
                                when (item) {
                                    is FileBrowserModule.BrowserItem.Parent -> {
                                        // Navigate to parent folder
                                        fileBrowserState = fileBrowserModule.navigateToParent(fileBrowserState)
                                    }
                                    is FileBrowserModule.BrowserItem.Folder -> {
                                        // Navigate into folder
                                        fileBrowserState = fileBrowserModule.navigateToFolder(
                                            fileBrowserState,
                                            item.file
                                        )
                                    }
                                    is FileBrowserModule.BrowserItem.FileItem -> {
                                        // Check which screen opened the file browser
                                        when (previousScreen) {
                                            ScreenType.PROJECT -> {
                                                // Load project file
                                                val result = trackerController.loadProject(item.file.toFileInfo())
                                                when (result) {
                                                    is FileController.LoadResult.Success -> {
                                                        // Sync project name from filename (handles renamed files)
                                                        result.project.name = item.file.nameWithoutExtension.take(20)
                                                        trackerController.project = result.project
                                                        // Reload all custom samples from the loaded project
                                                        reloadProjectSamples()
                                                        // Clear table cache so new project tables are loaded
                                                        audioEngine.clearLoadedTables()
                                                        trackerController.statusMessage = "LOADED: ${item.file.nameWithoutExtension}"
                                                        trackerController.statusSuccess = true
                                                        trackerController.projectVersion++
                                                        trackerController.currentScreen = previousScreen
                                                    }
                                                    is FileController.LoadResult.Error -> {
                                                        fileBrowserState = fileBrowserState.copy(
                                                            statusMessage = "LOAD FAILED",
                                                            statusSuccess = false
                                                        )
                                                    }
                                                }
                                            }
                                            ScreenType.SAMPLE_EDITOR -> {
                                                // Load a new WAV for the sample editor's instrument
                                                if (item.file.extension.lowercase() == "wav") {
                                                    val result = instrumentController.loadSampleFromFile(
                                                        trackerController.project, item.file.absolutePath
                                                    )
                                                    if (result is com.conanizer.pockettracker.core.logic.LoadResult.Success) {
                                                        trackerController.projectVersion++
                                                        // Trigger waveform reload by bumping the instrumentId key
                                                        sampleEditorState = sampleEditorState.copy(
                                                            sampleFilePath = item.file.absolutePath,
                                                            isModified = false
                                                        )
                                                        trackerController.currentScreen = ScreenType.SAMPLE_EDITOR
                                                    } else {
                                                        fileBrowserState = fileBrowserState.copy(statusMessage = "LOAD FAILED", statusSuccess = false)
                                                    }
                                                }
                                            }
                                            ScreenType.INSTRUMENT -> {
                                                when (instrumentFileBrowserAction) {
                                                    "LOAD_PRESET" -> {
                                                        instrumentController.loadPreset(trackerController.project, item.file.absolutePath)
                                                        trackerController.projectVersion++
                                                        trackerController.currentScreen = previousScreen
                                                    }
                                                    "LOAD_SOURCE" -> {
                                                        val ext = item.file.extension.lowercase()
                                                        if (ext == "sf2" || ext == "sf3") {
                                                            instrumentController.loadSoundfont(trackerController.project, item.file.absolutePath)
                                                            trackerController.projectVersion++
                                                            trackerController.currentScreen = previousScreen
                                                        } else {
                                                            val result = if (ext == "wav") {
                                                                instrumentController.loadSampleFromFile(trackerController.project, item.file.absolutePath)
                                                            } else if (videoExtractor.isSupportedVideo(item.file.absolutePath)) {
                                                                instrumentController.loadSampleFromVideo(trackerController.project, item.file.absolutePath, videoExtractor, fileSystem)
                                                            } else {
                                                                com.conanizer.pockettracker.core.logic.LoadResult.Error("Unsupported format")
                                                            }
                                                            if (result is com.conanizer.pockettracker.core.logic.LoadResult.Success) {
                                                                trackerController.projectVersion++
                                                                trackerController.currentScreen = previousScreen
                                                            } else {
                                                                fileBrowserState = fileBrowserState.copy(statusMessage = "LOAD FAILED", statusSuccess = false)
                                                            }
                                                        }
                                                    }
                                                    "LOAD_SAMPLE_EDITOR" -> {
                                                        if (item.file.extension.lowercase() == "wav") {
                                                            val result = instrumentController.loadSampleFromFile(
                                                                trackerController.project, item.file.absolutePath
                                                            )
                                                            if (result is com.conanizer.pockettracker.core.logic.LoadResult.Success) {
                                                                trackerController.projectVersion++
                                                                sampleEditorState = sampleEditorState.copy(
                                                                    sampleFilePath = item.file.absolutePath,
                                                                    isModified = false
                                                                )
                                                                trackerController.currentScreen = ScreenType.SAMPLE_EDITOR
                                                            } else {
                                                                fileBrowserState = fileBrowserState.copy(statusMessage = "LOAD FAILED", statusSuccess = false)
                                                            }
                                                        }
                                                    }
                                                    else -> {
                                                        // Legacy fallback: treat as LOAD_SOURCE (WAV/video)
                                                        val ext = item.file.extension.lowercase()
                                                        val result = if (ext == "wav") {
                                                            instrumentController.loadSampleFromFile(trackerController.project, item.file.absolutePath)
                                                        } else if (videoExtractor.isSupportedVideo(item.file.absolutePath)) {
                                                            instrumentController.loadSampleFromVideo(trackerController.project, item.file.absolutePath, videoExtractor, fileSystem)
                                                        } else {
                                                            com.conanizer.pockettracker.core.logic.LoadResult.Error("Unsupported format")
                                                        }
                                                        if (result is com.conanizer.pockettracker.core.logic.LoadResult.Success) {
                                                            trackerController.projectVersion++
                                                            trackerController.currentScreen = previousScreen
                                                        } else {
                                                            fileBrowserState = fileBrowserState.copy(statusMessage = "LOAD FAILED", statusSuccess = false)
                                                        }
                                                    }
                                                }
                                            }
                                            else -> {
                                                // Unknown previous screen - try loading as project
                                                val result = trackerController.loadProject(item.file.toFileInfo())
                                                when (result) {
                                                    is FileController.LoadResult.Success -> {
                                                        trackerController.project = result.project
                                                        // Reload all custom samples from the loaded project
                                                        reloadProjectSamples()
                                                        // Clear table cache so new project tables are loaded
                                                        audioEngine.clearLoadedTables()
                                                        trackerController.projectVersion++
                                                        trackerController.currentScreen = previousScreen
                                                    }
                                                    is FileController.LoadResult.Error -> {
                                                        fileBrowserState = fileBrowserState.copy(
                                                            statusMessage = "LOAD FAILED",
                                                            statusSuccess = false
                                                        )
                                                    }
                                                }
                                            }
                                        }
                                    }
                                    null -> { /* Empty list */ }
                                }
                            }
                            FileBrowserModule.BrowserMode.DELETE -> {
                                // Confirm delete
                                val item = fileBrowserState.items.getOrNull(fileBrowserState.cursor)
                                if (item != null && item !is FileBrowserModule.BrowserItem.Parent) {
                                    val deleted = fileManager.deleteFileOrFolder(item.file.absolutePath)
                                    if (deleted) {
                                        // Refresh list
                                        val newItems = fileBrowserModule.buildItemList(
                                            fileBrowserState.currentDirectory,
                                            fileBrowserState.fileExtension,
                                            fileBrowserState.fileExtensions
                                        )
                                        val sortedItems = fileBrowserModule.sortItems(newItems, fileBrowserState.sortMode)

                                        // Adjust cursor if needed
                                        val newCursor = fileBrowserState.cursor.coerceAtMost(
                                            (newItems.size - 1).coerceAtLeast(0)
                                        )

                                        fileBrowserState = fileBrowserState.copy(
                                            items = sortedItems,
                                            cursor = newCursor,
                                            mode = FileBrowserModule.BrowserMode.NORMAL,
                                            statusMessage = "Deleted: ${item.displayName}",
                                            statusSuccess = true
                                        )
                                    } else {
                                        fileBrowserState = fileBrowserState.copy(
                                            mode = FileBrowserModule.BrowserMode.NORMAL,
                                            statusMessage = "Delete failed",
                                            statusSuccess = false
                                        )
                                    }
                                }
                            }
                            FileBrowserModule.BrowserMode.RENAME -> {
                                // Confirm rename (will implement in Phase 6)
                                fileBrowserState = fileBrowserState.copy(mode = FileBrowserModule.BrowserMode.NORMAL)
                            }
                            FileBrowserModule.BrowserMode.CREATE -> {
                                // Confirm create folder (will implement in Phase 6)
                                fileBrowserState = fileBrowserState.copy(mode = FileBrowserModule.BrowserMode.NORMAL)
                            }
                        }
                    }

                    // PROJECT SCREEN: Only handle action buttons (LOAD/SAVE/NEW)
                    // Value editing (TEMPO/TRANSPOSE/NAME) is handled by A+direction combos
                    ScreenType.PROJECT -> {
                        Log.d("ProjectScreen", "A pressed: row=${trackerController.projectCursorRow}, col=${trackerController.projectCursorColumn}")
                        when (trackerController.projectCursorRow) {
                            // ROW 3: PROJECT actions (LOAD/SAVE/NEW)
                            3 -> {
                                Log.d("ProjectScreen", "Row 3 action: column=${trackerController.projectCursorColumn}")
                                when (trackerController.projectCursorColumn) {
                                    1 -> {  // LOAD - Show file browser
                                        Log.d("ProjectScreen", "LOAD button pressed - switching to FILE_BROWSER")
                                        previousScreen = trackerController.currentScreen
                                        trackerController.currentScreen = ScreenType.FILE_BROWSER
                                        // Navigate to folder directly so items list is always rebuilt
                                        val projectsDir = File(fileManager.getProjectsDirectory())
                                        fileBrowserState = fileBrowserModule.navigateToFolder(
                                            fileBrowserState.copy(
                                                fileExtension = "ptp",
                                                fileExtensions = null,
                                                mode = FileBrowserModule.BrowserMode.NORMAL,
                                                statusMessage = ""
                                            ),
                                            projectsDir
                                        )
                                        Log.d("ProjectScreen", "File browser opened for .ptp files")
                                    }
                                    2 -> {  // SAVE
                                        val result = trackerController.saveProject(trackerController.project.name)
                                        when (result) {
                                            is FileController.SaveResult.Success -> {
                                                trackerController.statusMessage = "SAVED"
                                                trackerController.statusSuccess = true
                                            }
                                            is FileController.SaveResult.Error -> {
                                                trackerController.statusMessage = "SAVE FAILED"
                                                trackerController.statusSuccess = false
                                            }
                                        }
                                    }
                                    3 -> {  // NEW
                                        trackerController.newProject()
                                        // Clear table cache so old tables don't persist
                                        audioEngine.clearLoadedTables()
                                    }
                                }
                            }
                            // ROW 4: EXPORT actions (WAV MIX)
                            4 -> {
                                Log.d("ProjectScreen", "Row 4 action: column=${trackerController.projectCursorColumn}")
                                when (trackerController.projectCursorColumn) {
                                    1 -> {  // WAV MIX - Render song to WAV
                                        if (!isRendering) {
                                            Log.d("RenderWav", "Starting WAV render...")
                                            isRendering = true
                                            renderProgress = 0f
                                            trackerController.statusMessage = "RENDERING..."
                                            trackerController.statusSuccess = true

                                            // Stop playback before rendering
                                            trackerController.stopPlayback()

                                            // Run render in background
                                            coroutineScope.launch(Dispatchers.Default) {
                                                val result = renderController.renderSongToWav(
                                                    project = trackerController.project,
                                                    progressCallback = object : RenderController.ProgressCallback {
                                                        override fun onProgress(progress: Float, message: String) {
                                                            renderProgress = progress
                                                            Log.d("RenderWav", "Progress: ${(progress * 100).toInt()}% - $message")
                                                        }
                                                    }
                                                )

                                                withContext(Dispatchers.Main) {
                                                    isRendering = false
                                                    renderProgress = 0f

                                                    when (result) {
                                                        is RenderController.RenderResult.Success -> {
                                                            Log.d("RenderWav", "Render complete: ${result.filename}")
                                                            trackerController.statusMessage = "EXPORTED!"
                                                            trackerController.statusSuccess = true
                                                        }
                                                        is RenderController.RenderResult.Error -> {
                                                            Log.e("RenderWav", "Render failed: ${result.message}")
                                                            trackerController.statusMessage = "EXPORT FAILED"
                                                            trackerController.statusSuccess = false
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            // ROW 5: CLEAN — SEQ (col 1) or INST (col 2) buttons
                            5 -> {
                                val col = trackerController.projectCursorColumn
                                if (col == 1 || col == 2) {
                                    cleanDialogTarget = if (col == 1) "SEQ" else "INST"
                                    cleanDialogCursor = 0
                                    showCleanDialog = true
                                }
                            }
                            // ROW 6: SETTINGS — navigate to SETTINGS side menu
                            6 -> {
                                previousScreen = trackerController.currentScreen
                                trackerController.currentScreen = ScreenType.SETTINGS
                            }
                        }
                    }

                    // SETTINGS: Handle LAYOUT (row 0) and SCALING (row 1) cycle on A press
                    ScreenType.SETTINGS -> {
                        when (trackerController.settingsCursorRow) {
                            0 -> {  // LAYOUT — cycle through layout modes
                                val hasPhysical = deviceAdapter.hasPhysicalGameButtons()
                                layoutMode = when (layoutMode) {
                                    DeviceAdapter.LayoutMode.FULL            -> DeviceAdapter.LayoutMode.TOUCH_LANDSCAPE
                                    DeviceAdapter.LayoutMode.TOUCH_LANDSCAPE -> DeviceAdapter.LayoutMode.TOUCH_PORTRAIT2
                                    DeviceAdapter.LayoutMode.TOUCH_PORTRAIT2 -> if (hasPhysical) DeviceAdapter.LayoutMode.FULL else DeviceAdapter.LayoutMode.TOUCH_LANDSCAPE
                                    DeviceAdapter.LayoutMode.TOUCH_PORTRAIT  -> DeviceAdapter.LayoutMode.TOUCH_PORTRAIT2
                                }
                            }
                            1 -> {  // SCALING — cycle through scaling modes
                                scalingMode = when (scalingMode) {
                                    DeviceAdapter.ScalingMode.INTEGER  -> DeviceAdapter.ScalingMode.BILINEAR
                                    DeviceAdapter.ScalingMode.BILINEAR -> DeviceAdapter.ScalingMode.NEAREST
                                    DeviceAdapter.ScalingMode.NEAREST  -> DeviceAdapter.ScalingMode.INTEGER
                                }
                            }
                            // Rows 2-6: value editing handled via A+direction combos (getCursorContext)
                        }
                    }

                    // INSTRUMENT: Handle button rows
                    ScreenType.INSTRUMENT -> {
                        instrumentController.currentInstrument = trackerController.currentInstrument
                        val instrument = trackerController.project.instruments[trackerController.currentInstrument]
                        when (trackerController.instrumentCursorRow) {
                            0 -> {
                                when (trackerController.instrumentCursorColumn) {
                                    1 -> {  // TYPE toggle: SAMPLER ↔ SOUNDFONT
                                        val newType = if (instrument.instrumentType == InstrumentType.SOUNDFONT)
                                            InstrumentType.SAMPLER else InstrumentType.SOUNDFONT
                                        instrumentController.setInstrumentType(trackerController.project, newType)
                                        trackerController.projectVersion++
                                    }
                                    2 -> {  // LOAD .pti
                                        instrumentFileBrowserAction = "LOAD_PRESET"
                                        previousScreen = trackerController.currentScreen
                                        trackerController.currentScreen = ScreenType.FILE_BROWSER
                                        val instrumentsDir = File(fileManager.getInstrumentsDirectory())
                                        fileBrowserState = fileBrowserModule.navigateToFolder(
                                            fileBrowserState.copy(
                                                fileExtensions = listOf("pti"),
                                                mode = FileBrowserModule.BrowserMode.NORMAL,
                                                statusMessage = ""
                                            ),
                                            instrumentsDir
                                        )
                                    }
                                    3 -> {  // SAVE .pti — open QWERTY keyboard for filename
                                        val instrumentsDir = fileManager.getInstrumentsDirectory()
                                        java.io.File(instrumentsDir).mkdirs()
                                        val defaultName = instrument.name.ifEmpty {
                                            "INST${instrument.id.toString(16).padStart(2,'0').uppercase()}"
                                        }
                                        qwertyKeyboardState = QwertyKeyboardState(
                                            isOpen = true,
                                            text = defaultName,
                                            maxLength = 20,
                                            textCursor = defaultName.length,
                                            fieldLabel = "SAVE PRESET:",
                                            originalText = defaultName,
                                            clearOnFirstB = true,
                                            context = QwertyContext.INSTRUMENT_SAVE,
                                            contextExtra = instrumentsDir
                                        )
                                    }
                                }
                            }
                            3 -> {  // SOURCE section: LOAD (col 2), EDIT placeholder (col 3)
                                when (trackerController.instrumentCursorColumn) {
                                    2 -> {  // LOAD WAV or SF2
                                        instrumentFileBrowserAction = "LOAD_SOURCE"
                                        previousScreen = trackerController.currentScreen
                                        trackerController.currentScreen = ScreenType.FILE_BROWSER
                                        if (instrument.instrumentType == InstrumentType.SOUNDFONT) {
                                            val soundfontsDir = File(fileManager.getSoundfontsDirectory())
                                            fileBrowserState = fileBrowserModule.navigateToFolder(
                                                fileBrowserState.copy(
                                                    fileExtensions = listOf("sf2", "sf3"),
                                                    mode = FileBrowserModule.BrowserMode.NORMAL,
                                                    statusMessage = ""
                                                ),
                                                soundfontsDir
                                            )
                                        } else {
                                            val samplesDir = File(fileManager.getSamplesDirectory())
                                            val sampleExtensions = listOf("wav") + FileBrowserModule.VIDEO_EXTENSIONS
                                            fileBrowserState = fileBrowserModule.navigateToFolder(
                                                fileBrowserState.copy(
                                                    fileExtensions = sampleExtensions,
                                                    mode = FileBrowserModule.BrowserMode.NORMAL,
                                                    statusMessage = ""
                                                ),
                                                samplesDir
                                            )
                                        }
                                    }
                                    3 -> {  // EDIT — open sample editor
                                        val inst = trackerController.project.instruments[trackerController.currentInstrument]
                                        val sampleId = trackerController.currentInstrument
                                        sampleEditorState = SampleEditorState(
                                            sampleId     = sampleId,
                                            instrumentId = trackerController.currentInstrument,
                                            sampleName   = inst.sampleFilePath?.substringAfterLast('/')?.substringBeforeLast('.') ?: "",
                                            sampleFilePath = inst.sampleFilePath,
                                            cursorRow    = 1,
                                            cursorCol    = 0,
                                            isModified   = false
                                        )
                                        previousScreen = trackerController.currentScreen
                                        trackerController.currentScreen = ScreenType.SAMPLE_EDITOR
                                    }
                                }
                            }
                            // Other rows use A+direction combos for value editing
                        }
                    }

                    // SAMPLE EDITOR: Execute operations on action rows 13, 14, 19
                    ScreenType.SAMPLE_EDITOR -> {
                        val s = sampleEditorState
                        val instId = s.instrumentId
                        fun doDestructiveOp(op: () -> Unit) {
                            audioEngine.backupSample(instId)
                            op()
                            sampleEditorState = sampleEditorState.copy(
                                totalFrames  = audioEngine.getSampleLength(instId),
                                waveformData = audioEngine.getSampleWaveform(instId, 620),
                                isModified   = true
                            )
                        }
                        val startF = s.selectionStart.toInt()
                        val endF   = s.selectionEnd.toInt()
                        // Helper to refresh waveform + reset selection to full sample after resize
                        fun afterResize() {
                            val newLen = audioEngine.getSampleLength(instId)
                            sampleEditorState = sampleEditorState.copy(
                                totalFrames  = newLen,
                                waveformData = audioEngine.getSampleWaveform(instId, 620),
                                selectionStart = 0L,
                                selectionEnd   = newLen.toLong(),
                                isModified   = true
                            )
                        }
                        when (s.cursorRow) {
                            13 -> when (s.cursorCol) {
                                0 -> { // CROP — destructively trim sample to selection
                                    if (startF < endF) {
                                        audioEngine.backupSample(instId)
                                        audioEngine.cropSample(instId, startF, endF)
                                        afterResize()
                                    }
                                }
                                1 -> { // COPY — copy selection to clipboard (non-destructive)
                                    audioEngine.copyRegion(instId, startF, endF)
                                }
                                2 -> { // CUT — copy + delete selection
                                    if (startF < endF) {
                                        audioEngine.backupSample(instId)
                                        audioEngine.copyRegion(instId, startF, endF)
                                        audioEngine.deleteSampleRegion(instId, startF, endF)
                                        afterResize()
                                    }
                                }
                                3 -> { // DUPL — duplicate selection (insert copy at end)
                                    if (startF < endF) {
                                        audioEngine.backupSample(instId)
                                        audioEngine.copyRegion(instId, startF, endF)
                                        audioEngine.pasteRegion(instId, sampleEditorState.totalFrames)
                                        afterResize()
                                    }
                                }
                                4 -> { // PASTE — insert clipboard at selection start
                                    if (audioEngine.getClipboardLength() > 0) {
                                        audioEngine.backupSample(instId)
                                        audioEngine.pasteRegion(instId, startF)
                                        afterResize()
                                    }
                                }
                                5 -> { // DEL — delete selection region
                                    if (startF < endF) {
                                        audioEngine.backupSample(instId)
                                        audioEngine.deleteSampleRegion(instId, startF, endF)
                                        afterResize()
                                    }
                                }
                            }
                            14 -> when (s.cursorCol) {
                                0 -> doDestructiveOp { audioEngine.normalizeSample(instId, startF, endF) }
                                1 -> doDestructiveOp { audioEngine.fadeInSample(instId, startF, endF) }
                                2 -> doDestructiveOp { audioEngine.fadeOutSample(instId, startF, endF) }
                                3 -> doDestructiveOp { audioEngine.silenceRegion(instId, startF, endF) }
                                4 -> doDestructiveOp { audioEngine.reverseSample(instId, startF, endF) }
                                5 -> { // UNDO
                                    audioEngine.undoSample(instId)
                                    sampleEditorState = sampleEditorState.copy(
                                        totalFrames  = audioEngine.getSampleLength(instId),
                                        waveformData = audioEngine.getSampleWaveform(instId, 620)
                                    )
                                }
                            }
                            16 -> if (s.cursorCol == 2) { // APPLY FX — not yet implemented
                            }
                            18 -> { // NAME — open QWERTY keyboard for renaming
                                val currentName = s.sampleName
                                qwertyKeyboardState = QwertyKeyboardState(
                                    isOpen       = true,
                                    text         = currentName,
                                    maxLength    = 20,
                                    textCursor   = currentName.length.coerceAtMost(20),
                                    keyCursorRow = 0,
                                    keyCursorCol = 0,
                                    layout       = 0,
                                    fieldLabel   = "SAMPLE NAME:",
                                    originalText = currentName,
                                    insertBefore = insertBefore,
                                    context      = QwertyContext.SAMPLE_NAME,
                                    contextExtra = fileManager.getSamplesDirectory()
                                )
                            }
                            19 -> when (s.cursorCol) {
                                0 -> { // LOAD: open file browser for WAV
                                    instrumentFileBrowserAction = "LOAD_SAMPLE_EDITOR"
                                    // previousScreen intentionally NOT changed — keeps the INSTRUMENT
                                    // reference so closing the editor still returns to the right screen
                                    fileBrowserState = fileBrowserState.copy(
                                        fileExtension = "wav",
                                        fileExtensions = listOf("wav")
                                    )
                                    trackerController.currentScreen = ScreenType.FILE_BROWSER
                                }
                                1 -> { // SAVE: direct save if name is free, QWERTY only on collision
                                    val baseName = s.sampleName.ifEmpty { "SAMPLE" }
                                    val samplesDir = fileManager.getSamplesDirectory()
                                    java.io.File(samplesDir).mkdirs()
                                    val targetPath = "$samplesDir/$baseName.wav"
                                    if (!java.io.File(targetPath).exists()) {
                                        // No collision — save directly and close
                                        coroutineScope.launch(Dispatchers.Default) {
                                            val floats   = audioEngine.getSampleData(instId)
                                            val origRate = audioEngine.getOriginalSampleRate(instId)
                                            val success  = WavWriter.writeWav(
                                                fileSystem   = fileSystem,
                                                path         = targetPath,
                                                leftChannel  = floats,
                                                rightChannel = floats,
                                                sampleRate   = origRate
                                            )
                                            withContext(Dispatchers.Main) {
                                                if (success) {
                                                    trackerController.project.instruments[instId].sampleFilePath = targetPath
                                                    sampleEditorState = sampleEditorState.copy(
                                                        sampleFilePath = targetPath,
                                                        sampleName     = baseName,
                                                        isModified     = false
                                                    )
                                                    trackerController.currentScreen = previousScreen
                                                }
                                            }
                                        }
                                    } else {
                                        // Collision — open QWERTY with auto-incremented suggestion
                                        var suggestedName = baseName
                                        var n = 1
                                        while (java.io.File("$samplesDir/$suggestedName.wav").exists()) {
                                            suggestedName = "${baseName}_${n.toString().padStart(4, '0')}"
                                            n++
                                        }
                                        qwertyKeyboardState = QwertyKeyboardState(
                                            isOpen        = true,
                                            text          = suggestedName,
                                            maxLength     = 24,
                                            textCursor    = suggestedName.length.coerceAtMost(24),
                                            keyCursorRow  = 0,
                                            keyCursorCol  = 0,
                                            layout        = 0,
                                            fieldLabel    = "SAVE AS:",
                                            originalText  = suggestedName,
                                            insertBefore  = insertBefore,
                                            clearOnFirstB = true,
                                            context       = QwertyContext.SAMPLE_SAVE,
                                            contextExtra  = samplesDir
                                        )
                                    }
                                }
                                2 -> { // OVERWRITE: write sample buffer back to original WAV
                                    val filePath = trackerController.project.instruments[instId].sampleFilePath
                                    if (filePath != null) {
                                        coroutineScope.launch(Dispatchers.Default) {
                                            val floats   = audioEngine.getSampleData(instId)
                                            val origRate = audioEngine.getOriginalSampleRate(instId)
                                            val success = WavWriter.writeWav(
                                                fileSystem   = fileSystem,
                                                path         = filePath,
                                                leftChannel  = floats,
                                                rightChannel = floats,
                                                sampleRate   = origRate
                                            )
                                            withContext(Dispatchers.Main) {
                                                if (success) {
                                                    sampleEditorState = sampleEditorState.copy(isModified = false)
                                                    trackerController.currentScreen = previousScreen
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // PHRASE: Quick insert last-used values on empty row
                    ScreenType.PHRASE -> {
                        // Quick-insert only works on Note column (column 1)
                        // FX columns (4-9) should not trigger note insertion
                        if (trackerController.cursorColumn == 1) {
                            // Build phrase state for the current cursor so we can get a CursorContext
                            val phraseState = PhraseEditorState(
                                trackerController.project.phrases[trackerController.currentPhrase],
                                trackerController.cursorRow,
                                trackerController.cursorColumn,
                                playbackRow = 0,
                                isPlaying = trackerController.isPlaying()
                            )
                            // Get cursor context to check if cell is empty via architecture
                            val context = phraseEditorModule.getCursorContext(phraseState)
                            if (context.capabilities.isEmpty) {
                                // Insert last-used values on empty note
                                val step = trackerController.project.phrases[trackerController.currentPhrase].steps[trackerController.cursorRow]
                                step.note = trackerController.lastEditedNote
                                step.instrument = trackerController.lastEditedInstrument
                                step.volume = trackerController.lastEditedVolume
                                trackerController.projectVersion++
                                Log.d("QuickInsert", "Inserted phrase step: note=${trackerController.lastEditedNote}, inst=${trackerController.lastEditedInstrument}, vol=${trackerController.lastEditedVolume}")
                            }
                        }
                        // For FX columns, A button behavior is handled by A+direction combos via handleGenericInput
                    }

                    // CHAIN: Quick insert last-used phrase + transpose on empty row
                    ScreenType.CHAIN -> {
                        val chain = trackerController.project.chains[trackerController.currentChain]
                        if (chain.isEmpty(trackerController.cursorRow)) {
                            // Insert last-used phrase and transpose
                            chain.phraseRefs[trackerController.cursorRow] = trackerController.lastEditedPhrase
                            chain.transposeValues[trackerController.cursorRow] = trackerController.lastEditedTranspose
                            trackerController.projectVersion++
                            lastAInsertPosition = InsertPosition(ScreenType.CHAIN, trackerController.cursorRow, trackerController.cursorColumn)
                            Log.d("QuickInsert", "Inserted chain row: phrase=${trackerController.lastEditedPhrase}, transpose=${trackerController.lastEditedTranspose}")
                        } else {
                            lastAInsertPosition = null
                        }
                    }

                    // SONG: Quick insert last-used chain on empty row
                    // Disabled in selection mode to avoid accidentally inserting while incrementing selections
                    ScreenType.SONG -> {
                        if (!trackerController.inputController.isSelectionModeActive()) {
                            val track = trackerController.project.tracks[trackerController.cursorColumn - 1]
                            // Ensure track has enough rows
                            while (track.chainRefs.size <= trackerController.cursorRow) {
                                track.chainRefs.add(-1)
                            }
                            if (track.chainRefs[trackerController.cursorRow] == -1) {
                                // Insert last-used chain
                                track.chainRefs[trackerController.cursorRow] = trackerController.lastEditedChain
                                trackerController.projectVersion++
                                lastAInsertPosition = InsertPosition(ScreenType.SONG, trackerController.cursorRow, trackerController.cursorColumn)
                                Log.d("QuickInsert", "Inserted song chain: chain=${trackerController.lastEditedChain} at track=${trackerController.cursorColumn-1}, row=${trackerController.cursorRow}")
                            } else {
                                lastAInsertPosition = null
                            }
                        }
                    }

                    else -> { /* Other screens not implemented yet */ }
                }
            } },  // end run buttonA@

// ───────────────────────────────────────────────────────────────
// BUTTON B - Secondary action
// ───────────────────────────────────────────────────────────────
            onButtonB = { run buttonB@{
                // ── QWERTY keyboard: B = delete ──────────────────────────────
                if (qwertyKeyboardState.isOpen) {
                    qwertyKeyboardState = qwertyKeyboardState.deleteChar()
                    return@buttonB
                }

                // Clean dialog: B = cancel (NO)
                if (showCleanDialog) {
                    showCleanDialog = false
                    return@buttonB
                }

                // Check if in selection mode first - B = COPY
                if (trackerController.inputController.isSelectionModeActive()) {
                    val bounds = trackerController.inputController.getSelectionBounds()
                    if (bounds != null) {
                        when (trackerController.currentScreen) {
                            ScreenType.PHRASE -> {
                                clipboardManager.copyPhraseSteps(
                                    trackerController.project,
                                    trackerController.currentPhrase,
                                    bounds.topLeftRow, bounds.topLeftColumn,
                                    bounds.bottomRightRow, bounds.bottomRightColumn
                                )
                                Log.d("CopyPaste", "Copied phrase selection: ${bounds.width}x${bounds.height}")
                            }
                            ScreenType.CHAIN -> {
                                clipboardManager.copyChainRows(
                                    trackerController.project,
                                    trackerController.currentChain,
                                    bounds.topLeftRow, bounds.topLeftColumn,
                                    bounds.bottomRightRow, bounds.bottomRightColumn
                                )
                                Log.d("CopyPaste", "Copied chain selection: ${bounds.width}x${bounds.height}")
                            }
                            ScreenType.SONG -> {
                                clipboardManager.copySongCells(
                                    trackerController.project,
                                    bounds.topLeftRow, bounds.topLeftColumn,
                                    bounds.bottomRightRow, bounds.bottomRightColumn
                                )
                                Log.d("CopyPaste", "Copied song selection: ${bounds.width}x${bounds.height}")
                            }
                            ScreenType.TABLE -> {
                                clipboardManager.copyTableRows(
                                    trackerController.project,
                                    trackerController.currentTable,
                                    bounds.topLeftRow, bounds.topLeftColumn,
                                    bounds.bottomRightRow, bounds.bottomRightColumn
                                )
                                Log.d("CopyPaste", "Copied table selection: ${bounds.width}x${bounds.height}")
                            }
                            else -> { }
                        }
                        trackerController.inputController.exitSelectionMode()
                    }
                    return@buttonB
                }

                when (trackerController.currentScreen) {
                    // SETTINGS: B returns to previous screen (usually PROJECT)
                    ScreenType.SETTINGS -> {
                        trackerController.currentScreen = previousScreen
                    }

                    // SAMPLE EDITOR: confirm close only if modified
                    ScreenType.SAMPLE_EDITOR -> {
                        if (sampleEditorState.showConfirmClose) {
                            // B = NO — dismiss dialog
                            sampleEditorState = sampleEditorState.copy(showConfirmClose = false)
                        } else if (sampleEditorState.isModified) {
                            sampleEditorState = sampleEditorState.copy(showConfirmClose = true)
                        } else {
                            trackerController.currentScreen = previousScreen
                        }
                        return@buttonB
                    }

                    // FILE BROWSER: Cancel operation or go back
                    ScreenType.FILE_BROWSER -> {
                        when (fileBrowserState.mode) {
                            FileBrowserModule.BrowserMode.NORMAL -> {
                                // Go back to previous screen
                                Log.d("FileBrowser", "Returning to $previousScreen")
                                trackerController.currentScreen = previousScreen
                            }
                            else -> {
                                // Cancel current mode (DELETE/RENAME/CREATE)
                                Log.d("FileBrowser", "Cancelled ${fileBrowserState.mode} mode")
                                fileBrowserState = fileBrowserState.copy(
                                    mode = FileBrowserModule.BrowserMode.NORMAL,
                                    renameBuffer = "",
                                    renameCursor = 0
                                )
                            }
                        }
                    }

                    // SONG SCREEN:
                    ScreenType.SONG -> {
                        // All song value editing is now handled via generic input handler
                        // No need for manual cycling here
                    }

                    // CHAIN SCREEN:
                    ScreenType.CHAIN -> {
                        // No need for manual cycling here
                    }

                    // PHRASE SCREEN: Now handled by generic input (A+direction combos)
                    ScreenType.PHRASE -> {
                        // All phrase value editing is now handled via generic input handler
                        // No need for manual cycling here
                    }

                    // PROJECT SCREEN:
                    ScreenType.PROJECT -> {
                        when (trackerController.projectCursorRow) {
                            // ROW 0: TEMPO (decrease by 1)
                            0 -> {
                                // No need for manual cycling here
                            }
                        }
                    }

                    else -> { }
                }
            } },  // end run buttonB@

// ───────────────────────────────────────────────────────────────
// SELECT BUTTON - Clear/Delete or quick navigation
// ───────────────────────────────────────────────────────────────
            onSelect = { run selectHandler@{
                // ── QWERTY keyboard: SELECT = cancel (discard changes) ────────
                if (qwertyKeyboardState.isOpen) {
                    qwertyKeyboardState = QwertyKeyboardState()  // close without saving
                    return@selectHandler
                }

                // EQ editor: SELECT = close
                if (eqEditorState.isOpen) {
                    eqEditorState = EqEditorState()
                    return@selectHandler
                }

                // Read directly from trackerController to avoid stale captured values
                when (trackerController.currentScreen) {
                    // SONG SCREEN: Clear chain reference
                    ScreenType.SONG -> {
                        val trackIndex = getTrackIndex(trackerController.cursorColumn)
                        clearSongChainRef(trackerController.project.tracks[trackIndex], trackerController.cursorRow)
                        trackerController.projectVersion++
                    }

                    // CHAIN SCREEN: Use generic input system (DELETE action)
                    ScreenType.CHAIN -> {
                        // Get cursor context from chain editor
                        val chainState = ChainEditorState(
                            trackerController.project.chains[trackerController.currentChain],
                            trackerController.cursorRow,
                            trackerController.cursorColumn
                        )
                        val context = chainEditorModule.getCursorContext(chainState)

                        // Handle SELECT as delete
                        val action = trackerController.inputController.handleSelect(context)

                        // Apply the action
                        when (action) {
                            is InputAction.DELETE -> {
                                // Clear the value at cursor position
                                if (trackerController.cursorColumn == 1) {
                                    clearChainSlot(trackerController.project.chains[trackerController.currentChain], trackerController.cursorRow)
                                    trackerController.projectVersion++
                                }
                            }
                            else -> { }
                        }
                    }

                    // PHRASE SCREEN: Clear step at cursor
                    ScreenType.PHRASE -> {
                        val step = trackerController.project.phrases[trackerController.currentPhrase].steps[trackerController.cursorRow]
                        when (trackerController.cursorColumn) {
                            1 -> {
                                // Clear note
                                step.note = Note.EMPTY
                                trackerController.projectVersion++
                            }
                            2 -> {
                                // Clear volume (set to max = FF)
                                step.volume = 0xFF
                                trackerController.projectVersion++
                            }
                            3 -> {
                                // Clear instrument (set to 0)
                                step.instrument = 0
                                trackerController.projectVersion++
                            }
                            4, 5 -> {
                                // Clear FX1
                                step.fx1Type = 0
                                step.fx1Value = 0
                                trackerController.projectVersion++
                            }
                            6, 7 -> {
                                // Clear FX2
                                step.fx2Type = 0
                                step.fx2Value = 0
                                trackerController.projectVersion++
                            }
                            8, 9 -> {
                                // Clear FX3
                                step.fx3Type = 0
                                step.fx3Value = 0
                                trackerController.projectVersion++
                            }
                        }
                    }

                    // PROJECT SCREEN: SELECT on NAME row (row 2) opens QWERTY keyboard
                    ScreenType.PROJECT -> {
                        val row = trackerController.projectCursorRow
                        val col = trackerController.projectCursorColumn
                        if (row == 2 && col >= 1) {
                            // Open QWERTY keyboard for project name editing
                            val currentName = trackerController.project.name.trimEnd()
                            qwertyKeyboardState = QwertyKeyboardState(
                                isOpen = true,
                                text = currentName,
                                maxLength = 20,
                                textCursor = currentName.length.coerceAtMost(20),
                                keyCursorRow = 0,
                                keyCursorCol = 0,
                                layout = 0,
                                fieldLabel = "PROJECT NAME:",
                                originalText = currentName,
                                insertBefore = insertBefore,
                                context = QwertyContext.PROJECT_NAME
                            )
                        }
                    }

                    // SAMPLE_EDITOR: SELECT on NAME row opens keyboard; otherwise blocked
                    ScreenType.SAMPLE_EDITOR -> {
                        if (sampleEditorState.cursorRow == 18) {
                            val currentName = sampleEditorState.sampleName
                            qwertyKeyboardState = QwertyKeyboardState(
                                isOpen       = true,
                                text         = currentName,
                                maxLength    = 20,
                                textCursor   = currentName.length.coerceAtMost(20),
                                keyCursorRow = 0,
                                keyCursorCol = 0,
                                layout       = 0,
                                fieldLabel   = "SAMPLE NAME:",
                                originalText = currentName,
                                insertBefore = insertBefore,
                                context      = QwertyContext.SAMPLE_NAME,
                                contextExtra = fileManager.getSamplesDirectory()
                            )
                        }
                        return@selectHandler
                    }

                    // FILE_BROWSER: SELECT button does nothing (combos handled separately)
                    ScreenType.FILE_BROWSER -> {
                        // Do nothing - SELECT combos (SELECT+A, SELECT+B, etc.) are handled in InputMapper
                    }

                    // EFFECTS: SELECT on TIME row toggles sync; SELECT on EQ rows opens EQ editor
                    ScreenType.EFFECTS -> {
                        when (trackerController.effectsCursorRow) {
                            EffectModule.ROW_DLY_TIME -> {
                                val proj = trackerController.project
                                proj.delaySync = !proj.delaySync
                                if (proj.delaySync) proj.delayTime = proj.delayTime.coerceIn(0, 11)
                                audioBackend.setDelayParams(
                                    proj.delayTime, proj.delayFeedback,
                                    proj.delaySync, proj.tempo.toFloat(), proj.delayWet
                                )
                                trackerController.projectVersion++
                            }
                            EffectModule.ROW_REV_EQ -> {
                                val slot = trackerController.project.reverbInputEq
                                openEqEditor(if (slot < 0) 0 else slot, EqCallerContext.ReverbInputEq)
                            }
                            EffectModule.ROW_DLY_EQ -> {
                                val slot = trackerController.project.delayInputEq
                                openEqEditor(if (slot < 0) 0 else slot, EqCallerContext.DelayInputEq)
                            }
                        }
                    }

                    // MIXER: SELECT on master EQ row opens EQ editor
                    ScreenType.MIXER -> {
                        if (trackerController.mixerMasterRow == 1) {
                            val slot = trackerController.project.masterEqSlot
                            openEqEditor(if (slot < 0) 0 else slot, EqCallerContext.MasterEq)
                        }
                    }

                    // INSTRUMENT: SELECT on EQ value opens EQ editor
                    ScreenType.INSTRUMENT -> {
                        val instr = trackerController.project.instruments[trackerController.currentInstrument]
                        val isSF  = instr.instrumentType == com.conanizer.pockettracker.core.data.InstrumentType.SOUNDFONT
                        val row   = trackerController.instrumentCursorRow
                        val col   = trackerController.instrumentCursorColumn
                        val onEq  = (!isSF && row == 13 && col == 3) || (isSF && row == 14 && col == 1)
                        if (onEq) {
                            val slot = instr.eqSlot
                            openEqEditor(if (slot < 0) 0 else slot, EqCallerContext.InstrumentEq(trackerController.currentInstrument))
                        }
                    }

                    // OTHER SCREENS: Quick jump to main screen
                    else -> {
                        if (trackerController.currentScreen !in MAIN_ROW_SCREENS) {
                            trackerController.currentScreen = when (trackerController.previousColumn) {
                                0 -> ScreenType.SONG
                                1 -> ScreenType.CHAIN
                                2 -> ScreenType.PHRASE
                                3 -> ScreenType.INSTRUMENT
                                4 -> ScreenType.TABLE
                                else -> ScreenType.PHRASE
                            }
                        }
                    }
                }
            } },  // end run selectHandler@

// ───────────────────────────────────────────────────────────────
// START BUTTON - Preview sample or toggle playback
// ───────────────────────────────────────────────────────────────
            onStart = { run startHandler@{
                // ── QWERTY keyboard: START = apply and close ─────────────────
                if (qwertyKeyboardState.isOpen) {
                    val typedText = qwertyKeyboardState.text.trimEnd()
                    when (qwertyKeyboardState.context) {
                        QwertyContext.PROJECT_NAME -> {
                            trackerController.project.name = typedText
                            trackerController.projectVersion++
                        }
                        QwertyContext.FILE_RENAME -> {
                            val oldPath = qwertyKeyboardState.contextExtra
                            val newBaseName = typedText.ifEmpty {
                                java.io.File(oldPath).nameWithoutExtension
                            }
                            // fileSystem.renameFile handles sanitization and preserves extension
                            val renamed = fileSystem.renameFile(oldPath, newBaseName)
                            if (renamed) {
                                fileBrowserState = fileBrowserModule.navigateToFolder(
                                    fileBrowserState, fileBrowserState.currentDirectory
                                )
                            }
                        }
                        QwertyContext.FOLDER_CREATE -> {
                            val parentPath = qwertyKeyboardState.contextExtra
                            val folderName = typedText.ifEmpty { "NewFolder" }
                            // fileSystem.createFolder handles sanitization and mkdirs
                            val created = fileSystem.createFolder(parentPath, folderName)
                            if (created != null) {
                                fileBrowserState = fileBrowserModule.navigateToFolder(
                                    fileBrowserState, fileBrowserState.currentDirectory
                                )
                            }
                        }
                        QwertyContext.INSTRUMENT_SAVE -> {
                            val name = typedText.ifEmpty { "PRESET" }
                            val dir = qwertyKeyboardState.contextExtra
                            val filePath = "$dir/$name.pti"
                            instrumentController.currentInstrument = trackerController.currentInstrument
                            instrumentController.savePreset(trackerController.project, filePath)
                            trackerController.projectVersion++
                        }
                        QwertyContext.SAMPLE_NAME -> {
                            val newName = typedText.ifEmpty { sampleEditorState.sampleName }
                            val instId = sampleEditorState.instrumentId
                            sampleEditorState = sampleEditorState.copy(sampleName = newName)
                            trackerController.project.instruments[instId].name = newName
                            trackerController.projectVersion++
                        }
                        QwertyContext.SAMPLE_SAVE -> {
                            val name = typedText.ifEmpty { "SAMPLE" }
                            val instId = sampleEditorState.instrumentId
                            val samplesDir = qwertyKeyboardState.contextExtra  // capture before state reset
                            coroutineScope.launch(Dispatchers.Default) {
                                java.io.File(samplesDir).mkdirs()
                                var path = "$samplesDir/$name.wav"
                                var counter = 1
                                while (java.io.File(path).exists()) {
                                    path = "$samplesDir/${name}_${counter.toString().padStart(4, '0')}.wav"
                                    counter++
                                }
                                val floats   = audioEngine.getSampleData(instId)
                                val origRate = audioEngine.getOriginalSampleRate(instId)
                                val success  = WavWriter.writeWav(
                                    fileSystem   = fileSystem,
                                    path         = path,
                                    leftChannel  = floats,
                                    rightChannel = floats,
                                    sampleRate   = origRate
                                )
                                withContext(Dispatchers.Main) {
                                    if (success) {
                                        val savedName = java.io.File(path).nameWithoutExtension
                                        trackerController.project.instruments[instId].sampleFilePath = path
                                        sampleEditorState = sampleEditorState.copy(
                                            sampleFilePath = path,
                                            sampleName     = savedName,
                                            isModified     = false
                                        )
                                        trackerController.currentScreen = previousScreen
                                    }
                                }
                            }
                        }
                        QwertyContext.RESAMPLE -> {
                            val customName = typedText.ifEmpty { null }
                            val bounds = trackerController.inputController.getSelectionBounds()
                            if (bounds != null && !isRendering) {
                                val selectedTracks = (bounds.topLeftColumn - 1..bounds.bottomRightColumn - 1)
                                    .filter { it in 0..7 }.toSet()
                                isRendering = true
                                renderProgress = 0f
                                trackerController.stopPlayback()

                                coroutineScope.launch(Dispatchers.Default) {
                                    val result = renderController.renderSelectionToWav(
                                        project = trackerController.project,
                                        startRow = bounds.topLeftRow,
                                        endRow = bounds.bottomRightRow,
                                        selectedTrackIds = selectedTracks,
                                        progressCallback = object : RenderController.ProgressCallback {
                                            override fun onProgress(progress: Float, message: String) {
                                                renderProgress = progress
                                            }
                                        },
                                        customBaseName = customName
                                    )

                                    withContext(Dispatchers.Main) {
                                        isRendering = false
                                        renderProgress = 0f
                                        when (result) {
                                            is RenderController.RenderResult.Success -> {
                                                val instId = instrumentController.createResampledInstrument(
                                                    trackerController.project, result.filename
                                                )
                                                if (instId >= 0) {
                                                    trackerController.statusMessage =
                                                        "RESAMPLED → INST ${instId.toString(16).padStart(2,'0').uppercase()}"
                                                    trackerController.statusSuccess = true
                                                    trackerController.projectVersion++
                                                }
                                            }
                                            is RenderController.RenderResult.Error -> {
                                                trackerController.statusMessage = "RESAMPLE FAILED"
                                                trackerController.statusSuccess = false
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    qwertyKeyboardState = QwertyKeyboardState()  // close keyboard
                    return@startHandler
                }

                // Read directly from trackerController to avoid stale captured values
                when (trackerController.currentScreen) {
                    // File browser: Preview selected WAV file
                    ScreenType.FILE_BROWSER -> {
                        Log.d("FileBrowser", "START pressed - previousScreen=$previousScreen, items=${fileBrowserState.items.size}")
                        if (previousScreen == ScreenType.INSTRUMENT && fileBrowserState.items.isNotEmpty()) {
                            val selectedItem = fileBrowserState.items[fileBrowserState.cursor]
                            val selectedFile = selectedItem.file
                            Log.d("FileBrowser", "Selected: ${selectedFile.name}, isFile=${selectedFile.isFile}, ext=${selectedFile.extension}")
                            if (selectedFile.isFile && selectedFile.extension.lowercase() == "wav") {
                                Log.d("FileBrowser", "Previewing sample: ${selectedFile.absolutePath}")
                                trackerController.previewSampleFile(selectedFile.absolutePath)
                            }
                        }
                    }

                    // Sample editor: Preview selection region with pitch applied
                    ScreenType.SAMPLE_EDITOR -> {
                        val instId = sampleEditorState.instrumentId
                        val total  = sampleEditorState.totalFrames
                        val inst   = trackerController.project.instruments[instId]
                        val savedStart = inst.sampleStart
                        val savedEnd   = inst.sampleEnd
                        val savedRoot  = inst.root
                        // Set start/end to selection boundaries so the voice plays only the selection
                        if (total > 0 && sampleEditorState.selectionEnd > sampleEditorState.selectionStart) {
                            inst.sampleStart = ((sampleEditorState.selectionStart * 255L) / total).toInt().coerceIn(0, 255)
                            inst.sampleEnd   = ((sampleEditorState.selectionEnd   * 255L) / total).toInt().coerceIn(0, 255)
                            audioEngine.updateInstrumentPlaybackParams(inst)
                        }
                        // Apply pitch offset — shift root note so preview plays at adjusted pitch
                        val pitchSemitones = sampleEditorState.pitchSemitones
                        if (pitchSemitones != 0) {
                            val shiftedMidi = (inst.root.toMidi() + pitchSemitones).coerceIn(0, 119)
                            inst.root = Note.fromMidi(shiftedMidi)
                        }
                        val savedInstrument = trackerController.currentInstrument
                        trackerController.currentInstrument = instId
                        trackerController.previewInstrument()
                        trackerController.currentInstrument = savedInstrument
                        // Root is captured via frequency at schedule time — safe to restore immediately
                        inst.root = savedRoot
                        // start/end are read from InstrumentParams at audio-callback time, NOT at schedule
                        // time, so restore them after the callback has had a chance to fire (~5ms).
                        coroutineScope.launch {
                            delay(100)
                            inst.sampleStart = savedStart
                            inst.sampleEnd   = savedEnd
                            audioEngine.updateInstrumentPlaybackParams(inst)
                        }
                    }

                    // Instrument screen: Preview instrument with all parameters
                    ScreenType.INSTRUMENT -> {
                        trackerController.previewInstrument()
                    }

                    // Table screen: Preview instrument using this table
                    ScreenType.TABLE -> {
                        // Preview instrument with same ID as current table
                        val instrumentId = trackerController.currentTable
                        trackerController.previewInstrumentWithTable(instrumentId, trackerController.currentTable)
                    }

                    // MODS screen: Preview current instrument (same as INSTRUMENT screen)
                    ScreenType.MODS -> {
                        trackerController.previewInstrument()
                    }

                    // Other screens: Toggle playback USING TrackerController
                    else -> {
                        if (isRendering) return@startHandler  // Block playback during WAV render
                        Log.d("Playback", "▶️ START pressed on ${trackerController.currentScreen}, isPlaying=${trackerController.isPlaying()}")
                        // Use TrackerController instead of direct PlaybackController
                        if (trackerController.isPlaying()) {
                            Log.d("Playback", "  → Stopping playback")
                            trackerController.stopPlayback()
                        } else {
                            when (trackerController.currentScreen) {
                                ScreenType.PHRASE -> {
                                    Log.d("Playback", "  → Starting phrase ${trackerController.currentPhrase}")
                                    trackerController.playPhrase(trackerController.currentPhrase)
                                    Log.d("Playback", "  → After playPhrase: isPlaying=${trackerController.isPlaying()}")
                                }

                                ScreenType.CHAIN -> {
                                    Log.d("Playback", "  → Starting chain ${trackerController.currentChain}")
                                    trackerController.playChain(trackerController.currentChain)
                                }

                                ScreenType.SONG -> {
                                    Log.d("Playback", "  → Starting song")
                                    trackerController.playSong()
                                }

                                ScreenType.MIXER -> {
                                    Log.d("Playback", "  → Starting song (from mixer)")
                                    trackerController.playSong()
                                }

                                ScreenType.EFFECTS -> {
                                    Log.d("Playback", "  → Starting song (from effects)")
                                    trackerController.playSong()
                                }

                                ScreenType.PROJECT -> {
                                    Log.d("Playback", "  → Starting song (from project)")
                                    trackerController.playSong()
                                }

                                else -> {
                                    Log.d("Playback", "  → Default togglePlayback")
                                    // Default to phrase playback
                                    trackerController.togglePlayback()  // NEW: Single method call
                                }
                            }
                        }
                    }
                }
            } },  // end run startHandler@

// ───────────────────────────────────────────────────────────────
// L BUTTON - Now only a hold modifier (L+R exits selection mode)
// ───────────────────────────────────────────────────────────────
            onL = {
                // L alone: Reserved as hold modifier only
                // Selection mode exit moved to L+R combo (fixes L+A cut combo bug)
                // Combinations like L+A, L+B, L+R are handled in InputMapper
            },

// ───────────────────────────────────────────────────────────────
// R BUTTON - Hold modifier (tracked by InputMapper)
// ───────────────────────────────────────────────────────────────
            onR = {
                // R button alone
                // R is tracked as a hold modifier by InputMapper
                // Combinations like R+arrows for screen navigation handled in InputMapper
            },

// ───────────────────────────────────────────────────────────────
// A + DIRECTION COMBINATIONS (M8-style value editing)
// ───────────────────────────────────────────────────────────────
            onAUp = {
                if (eqEditorState.isOpen) {
                    handleGenericInput { ctx -> trackerController.inputController.handleAButton(ctx) }
                } else if (fxHelperState.isOpen) {
                    fxHelperState = fxHelperState.fxMoveCursorUp()
                } else if (trackerController.currentScreen == ScreenType.SAMPLE_EDITOR && sampleEditorState.cursorRow in 3..8) {
                    // A+UP in waveform/selection rows: fine step forward / increment (1/256 of visible window)
                    val step = maxOf(1L, sampleEditorState.totalFrames.toLong() / (256L shl sampleEditorState.zoomLevel))
                    val maxFrame = sampleEditorState.totalFrames.toLong()
                    fun snapFrame(f: Long) = if (sampleEditorState.snapEnabled)
                        audioEngine.findZeroCrossing(sampleEditorState.instrumentId, f.toInt()).toLong() else f
                    sampleEditorState = if (sampleEditorState.cursorCol == 0) {
                        val raw = (sampleEditorState.selectionStart + step).coerceAtMost(sampleEditorState.selectionEnd - 1L)
                        sampleEditorState.copy(selectionStart = snapFrame(raw).coerceAtMost(sampleEditorState.selectionEnd - 1L))
                    } else {
                        val raw = (sampleEditorState.selectionEnd + step).coerceAtMost(maxFrame)
                        sampleEditorState.copy(selectionEnd = snapFrame(raw).coerceAtLeast(sampleEditorState.selectionStart + 1L).coerceAtMost(maxFrame))
                    }
                } else if (isOnFxTypeColumn()) {
                    val idx = getCurrentFxTypeIndex()
                    fxHelperState = FxHelperState(isOpen = true, cursorRow = idx / 5, cursorCol = idx % 5)
                } else {
                    // A+UP: Small increment (selection-aware)
                    handleSelectionOrSingleIncrement { context -> trackerController.inputController.handleAButton(context) }
                }
            },

            onADown = {
                if (eqEditorState.isOpen) {
                    handleGenericInput { ctx -> trackerController.inputController.handleBButton(ctx) }
                } else if (fxHelperState.isOpen) {
                    fxHelperState = fxHelperState.fxMoveCursorDown()
                } else if (trackerController.currentScreen == ScreenType.SAMPLE_EDITOR && sampleEditorState.cursorRow in 3..8) {
                    // A+DOWN in waveform/selection rows: fine step backward / decrement (1/256 of visible window)
                    val step = maxOf(1L, sampleEditorState.totalFrames.toLong() / (256L shl sampleEditorState.zoomLevel))
                    fun snapFrame(f: Long) = if (sampleEditorState.snapEnabled)
                        audioEngine.findZeroCrossing(sampleEditorState.instrumentId, f.toInt()).toLong() else f
                    sampleEditorState = if (sampleEditorState.cursorCol == 0) {
                        val raw = (sampleEditorState.selectionStart - step).coerceAtLeast(0L)
                        sampleEditorState.copy(selectionStart = snapFrame(raw).coerceAtMost(sampleEditorState.selectionEnd - 1L))
                    } else {
                        val raw = (sampleEditorState.selectionEnd - step).coerceAtLeast(sampleEditorState.selectionStart + 1L)
                        sampleEditorState.copy(selectionEnd = snapFrame(raw).coerceAtLeast(sampleEditorState.selectionStart + 1L))
                    }
                } else if (isOnFxTypeColumn()) {
                    val idx = getCurrentFxTypeIndex()
                    fxHelperState = FxHelperState(isOpen = true, cursorRow = idx / 5, cursorCol = idx % 5)
                } else {
                    // A+DOWN: Small decrement (selection-aware)
                    handleSelectionOrSingleIncrement { context -> trackerController.inputController.handleBButton(context) }
                }
            },

            onALeft = {
                if (eqEditorState.isOpen) {
                    handleGenericInput { ctx -> trackerController.inputController.handleALeft(ctx) }
                } else if (fxHelperState.isOpen) {
                    fxHelperState = fxHelperState.fxMoveCursorLeft()
                } else if (trackerController.currentScreen == ScreenType.SAMPLE_EDITOR && sampleEditorState.cursorRow in 3..8) {
                    // A+LEFT in waveform/selection rows: fast step backward / decrement (1/16 of visible window)
                    val step = maxOf(1L, sampleEditorState.totalFrames.toLong() / (16L shl sampleEditorState.zoomLevel))
                    fun snapFrame(f: Long) = if (sampleEditorState.snapEnabled)
                        audioEngine.findZeroCrossing(sampleEditorState.instrumentId, f.toInt()).toLong() else f
                    sampleEditorState = if (sampleEditorState.cursorCol == 0) {
                        val raw = (sampleEditorState.selectionStart - step).coerceAtLeast(0L)
                        sampleEditorState.copy(selectionStart = snapFrame(raw).coerceAtMost(sampleEditorState.selectionEnd - 1L))
                    } else {
                        val raw = (sampleEditorState.selectionEnd - step).coerceAtLeast(sampleEditorState.selectionStart + 1L)
                        sampleEditorState.copy(selectionEnd = snapFrame(raw).coerceAtLeast(sampleEditorState.selectionStart + 1L))
                    }
                } else {
                    // A+LEFT: Large decrement (selection-aware)
                    handleSelectionOrSingleIncrement { context -> trackerController.inputController.handleALeft(context) }
                }
            },

            onARight = {
                if (eqEditorState.isOpen) {
                    handleGenericInput { ctx -> trackerController.inputController.handleARight(ctx) }
                } else if (fxHelperState.isOpen) {
                    fxHelperState = fxHelperState.fxMoveCursorRight()
                } else if (trackerController.currentScreen == ScreenType.SAMPLE_EDITOR && sampleEditorState.cursorRow in 3..8) {
                    // A+RIGHT in waveform/selection rows: fast step forward / increment (1/16 of visible window)
                    val step = maxOf(1L, sampleEditorState.totalFrames.toLong() / (16L shl sampleEditorState.zoomLevel))
                    val maxFrame = sampleEditorState.totalFrames.toLong()
                    fun snapFrame(f: Long) = if (sampleEditorState.snapEnabled)
                        audioEngine.findZeroCrossing(sampleEditorState.instrumentId, f.toInt()).toLong() else f
                    sampleEditorState = if (sampleEditorState.cursorCol == 0) {
                        val raw = (sampleEditorState.selectionStart + step).coerceAtMost(sampleEditorState.selectionEnd - 1L)
                        sampleEditorState.copy(selectionStart = snapFrame(raw).coerceAtMost(sampleEditorState.selectionEnd - 1L))
                    } else {
                        val raw = (sampleEditorState.selectionEnd + step).coerceAtMost(maxFrame)
                        sampleEditorState.copy(selectionEnd = snapFrame(raw).coerceAtLeast(sampleEditorState.selectionStart + 1L).coerceAtMost(maxFrame))
                    }
                } else {
                    // A+RIGHT: Large increment (selection-aware)
                    handleSelectionOrSingleIncrement { context -> trackerController.inputController.handleARight(context) }
                }
            },

// ───────────────────────────────────────────────────────────────
// A + B COMBINATION (Delete selection or single cell)
// ───────────────────────────────────────────────────────────────
            onAB = {
                // A+B in selection mode: Delete selection (no clipboard)
                if (trackerController.inputController.isSelectionModeActive()) {
                    val bounds = trackerController.inputController.getSelectionBounds()
                    if (bounds != null) {
                        when (trackerController.currentScreen) {
                            ScreenType.PHRASE -> {
                                clipboardManager.deletePhraseSteps(
                                    trackerController.project,
                                    trackerController.currentPhrase,
                                    bounds.topLeftRow,
                                    bounds.topLeftColumn,
                                    bounds.bottomRightRow,
                                    bounds.bottomRightColumn
                                )
                                Log.d("Selection", "A+B: Deleted phrase selection")
                            }
                            ScreenType.CHAIN -> {
                                clipboardManager.deleteChainRows(
                                    trackerController.project,
                                    trackerController.currentChain,
                                    bounds.topLeftRow,
                                    bounds.topLeftColumn,
                                    bounds.bottomRightRow,
                                    bounds.bottomRightColumn
                                )
                                Log.d("Selection", "A+B: Deleted chain selection")
                            }
                            ScreenType.SONG -> {
                                clipboardManager.deleteSongCells(
                                    trackerController.project,
                                    bounds.topLeftRow,
                                    bounds.topLeftColumn,
                                    bounds.bottomRightRow,
                                    bounds.bottomRightColumn
                                )
                                Log.d("Selection", "A+B: Deleted song selection")
                            }
                            ScreenType.TABLE -> {
                                clipboardManager.deleteTableRows(
                                    trackerController.project,
                                    trackerController.currentTable,
                                    bounds.topLeftRow,
                                    bounds.topLeftColumn,
                                    bounds.bottomRightRow,
                                    bounds.bottomRightColumn
                                )
                                audioEngine.invalidateTable(trackerController.currentTable)
                                Log.d("Selection", "A+B: Deleted table selection")
                            }
                            else -> { }
                        }
                        trackerController.inputController.exitSelectionMode()
                    }
                } else {
                    // A+B outside selection: Delete/clear single value at cursor
                    handleGenericInput { context -> trackerController.inputController.handleABCombo(context) }
                }
            },

// ───────────────────────────────────────────────────────────────
// B + DIRECTION COMBINATIONS (Item cycling: chain/phrase/instrument)
// ───────────────────────────────────────────────────────────────
            onBLeft = {
                if (eqEditorState.isOpen) {
                    val newSlot = (eqEditorState.slotIndex - 1).coerceIn(0, 127)
                    eqEditorState = eqEditorState.copy(slotIndex = newSlot)
                    applyCallerEqSlotChange(newSlot)
                } else {
                // B+LEFT: Navigate to previous chain/phrase/instrument
                Log.d("Navigation", "B+LEFT: currentScreen=${trackerController.currentScreen}")
                when (trackerController.currentScreen) {
                    ScreenType.CHAIN -> {
                        // Previous chain (wrap around)
                        trackerController.currentChain = if (trackerController.currentChain > 0) trackerController.currentChain - 1 else 255
                        trackerController.lastEditedChain = trackerController.currentChain
                        Log.d("Navigation", "  -> Changed to chain ${trackerController.currentChain}")
                    }
                    ScreenType.PHRASE -> {
                        // Previous phrase (wrap around)
                        trackerController.currentPhrase = if (trackerController.currentPhrase > 0) trackerController.currentPhrase - 1 else 255
                        trackerController.lastEditedPhrase = trackerController.currentPhrase
                        Log.d("Navigation", "  -> Changed to phrase ${trackerController.currentPhrase}")
                    }
                    ScreenType.INSTRUMENT -> {
                        // Previous instrument (wrap around)
                        val newInst = if (trackerController.currentInstrument > 0)
                            trackerController.currentInstrument - 1 else 255
                        trackerController.currentInstrument = newInst
                        trackerController.lastEditedInstrument = newInst
                        instrumentController.currentInstrument = newInst  // Keep in sync!
                        Log.d("Navigation", "  -> Changed to instrument $newInst")
                    }
                    ScreenType.MODS -> {
                        val newInst = if (trackerController.currentInstrument > 0)
                            trackerController.currentInstrument - 1 else 255
                        trackerController.currentInstrument = newInst
                        trackerController.lastEditedInstrument = newInst
                        instrumentController.currentInstrument = newInst
                        Log.d("Navigation", "  -> MODS changed to instrument $newInst")
                    }
                    ScreenType.TABLE -> {
                        // Previous table (wrap around)
                        val newTable = if (trackerController.currentTable > 0)
                            trackerController.currentTable - 1 else 255
                        trackerController.currentTable = newTable
                        trackerController.lastEditedTable = newTable
                        Log.d("Navigation", "  -> Changed to table $newTable")
                    }
                    ScreenType.GROOVE -> {
                        // Previous groove (wrap around)
                        trackerController.currentGroove = if (trackerController.currentGroove > 0)
                            trackerController.currentGroove - 1 else 255
                        Log.d("Navigation", "  -> Changed to groove ${trackerController.currentGroove}")
                    }
                    else -> { Log.d("Navigation", "  -> No action for screen ${trackerController.currentScreen}") }
                }
                } // end else (EQ editor not open)
            },

            onBRight = {
                if (eqEditorState.isOpen) {
                    val newSlot = (eqEditorState.slotIndex + 1).coerceIn(0, 127)
                    eqEditorState = eqEditorState.copy(slotIndex = newSlot)
                    applyCallerEqSlotChange(newSlot)
                } else {
                // B+RIGHT: Navigate to next chain/phrase/instrument
                Log.d("Navigation", "B+RIGHT: currentScreen=${trackerController.currentScreen}")
                when (trackerController.currentScreen) {
                    ScreenType.CHAIN -> {
                        // Next chain (wrap around)
                        trackerController.currentChain = if (trackerController.currentChain < 255) trackerController.currentChain + 1 else 0
                        trackerController.lastEditedChain = trackerController.currentChain
                        Log.d("Navigation", "  -> Changed to chain ${trackerController.currentChain}")
                    }
                    ScreenType.PHRASE -> {
                        // Next phrase (wrap around)
                        trackerController.currentPhrase = if (trackerController.currentPhrase < 255) trackerController.currentPhrase + 1 else 0
                        trackerController.lastEditedPhrase = trackerController.currentPhrase
                        Log.d("Navigation", "  -> Changed to phrase ${trackerController.currentPhrase}")
                    }
                    ScreenType.INSTRUMENT -> {
                        // Next instrument (wrap around)
                        val newInst = if (trackerController.currentInstrument < 255)
                            trackerController.currentInstrument + 1 else 0
                        trackerController.currentInstrument = newInst
                        trackerController.lastEditedInstrument = newInst
                        instrumentController.currentInstrument = newInst  // Keep in sync!
                        Log.d("Navigation", "  -> Changed to instrument $newInst")
                    }
                    ScreenType.MODS -> {
                        val newInst = if (trackerController.currentInstrument < 255)
                            trackerController.currentInstrument + 1 else 0
                        trackerController.currentInstrument = newInst
                        trackerController.lastEditedInstrument = newInst
                        instrumentController.currentInstrument = newInst
                        Log.d("Navigation", "  -> MODS changed to instrument $newInst")
                    }
                    ScreenType.TABLE -> {
                        // Next table (wrap around)
                        val newTable = if (trackerController.currentTable < 255)
                            trackerController.currentTable + 1 else 0
                        trackerController.currentTable = newTable
                        trackerController.lastEditedTable = newTable
                        Log.d("Navigation", "  -> Changed to table $newTable")
                    }
                    ScreenType.GROOVE -> {
                        // Next groove (wrap around)
                        trackerController.currentGroove = if (trackerController.currentGroove < 255)
                            trackerController.currentGroove + 1 else 0
                        Log.d("Navigation", "  -> Changed to groove ${trackerController.currentGroove}")
                    }
                    else -> { Log.d("Navigation", "  -> No action for screen ${trackerController.currentScreen}") }
                }
                } // end else (EQ editor not open)
            },

// ───────────────────────────────────────────────────────────────
// B + UP/DOWN (Song screen page jump)
// ───────────────────────────────────────────────────────────────
            onBUp = {
                // B+UP: Song screen — page up 16 rows
                if (trackerController.currentScreen == ScreenType.SONG) {
                    trackerController.moveSongBigUp()
                }
            },

            onBDown = {
                // B+DOWN: Song screen — page down 16 rows
                if (trackerController.currentScreen == ScreenType.SONG) {
                    trackerController.moveSongBigDown()
                }
            },

// ───────────────────────────────────────────────────────────────
// R + DIRECTION COMBINATIONS (Screen navigation)
// ───────────────────────────────────────────────────────────────
            onRUp = {
                // R+UP: QWERTY keyboard — switch to letters layout
                if (qwertyKeyboardState.isOpen) {
                    qwertyKeyboardState = qwertyKeyboardState.copy(layout = 0).withClampedCol()
                } else if (eqEditorState.isOpen) { /* block map navigation while EQ editor open */
                } else if (trackerController.currentScreen == ScreenType.SAMPLE_EDITOR) { /* blocked in sample editor */
                } else
                // R+UP: Navigate to screen above in 5×5 grid OR cycle sort mode up in FILE_BROWSER
                if (trackerController.currentScreen == ScreenType.FILE_BROWSER) {
                    // File browser: cycle sort mode up
                    val modes = FileSortMode.values()
                    val currentIndex = modes.indexOf(fileBrowserState.sortMode)
                    val nextMode = modes[(currentIndex + 1) % modes.size]
                    fileBrowserState = fileBrowserState.copy(sortMode = nextMode)
                    Log.d("Navigation", "R+UP: Sort mode changed to $nextMode")
                } else {
                    val (newScreen, newCol) = trackerController.navigateUp(trackerController.currentScreen, trackerController.previousColumn)
                    if (newScreen != trackerController.currentScreen) {
                        // Screen changed - save current cursor, restore/reset for new screen, exit selection mode
                        trackerController.saveCursorForScreen(trackerController.currentScreen)
                        trackerController.restoreCursorForScreen(newScreen, cursorRemember)
                        trackerController.inputController.exitSelectionMode()
                    }
                    trackerController.currentScreen = newScreen
                    trackerController.previousColumn = newCol
                }
            },

            onRDown = {
                // R+DOWN: QWERTY keyboard — switch to numbers/symbols layout
                if (qwertyKeyboardState.isOpen) {
                    qwertyKeyboardState = qwertyKeyboardState.copy(layout = 1).withClampedCol()
                } else if (eqEditorState.isOpen) { /* block map navigation while EQ editor open */
                } else if (trackerController.currentScreen == ScreenType.SAMPLE_EDITOR) { /* blocked in sample editor */
                } else
                // R+DOWN: Navigate to screen below in 5×5 grid OR cycle sort mode down in FILE_BROWSER
                if (trackerController.currentScreen == ScreenType.FILE_BROWSER) {
                    // File browser: cycle sort mode down
                    val modes = FileSortMode.values()
                    val currentIndex = modes.indexOf(fileBrowserState.sortMode)
                    val prevMode = modes[(currentIndex - 1 + modes.size) % modes.size]
                    fileBrowserState = fileBrowserState.copy(sortMode = prevMode)
                    Log.d("Navigation", "R+DOWN: Sort mode changed to $prevMode")
                } else {
                    val (newScreen, newCol) = trackerController.navigateDown(trackerController.currentScreen, trackerController.previousColumn)
                    if (newScreen != trackerController.currentScreen) {
                        // Screen changed - save current cursor, restore/reset for new screen, exit selection mode
                        trackerController.saveCursorForScreen(trackerController.currentScreen)
                        trackerController.restoreCursorForScreen(newScreen, cursorRemember)
                        trackerController.inputController.exitSelectionMode()
                    }
                    trackerController.currentScreen = newScreen
                    trackerController.previousColumn = newCol
                }
            },

            onRLeft = {
                // R+LEFT: QWERTY keyboard — move text cursor left
                if (qwertyKeyboardState.isOpen) {
                    qwertyKeyboardState = qwertyKeyboardState.moveTextCursorLeft()
                } else if (eqEditorState.isOpen) { /* block map navigation while EQ editor open */
                } else if (trackerController.currentScreen == ScreenType.SAMPLE_EDITOR) { /* blocked in sample editor */
                } else
                // R+LEFT: Navigate to screen on left OR go to parent folder in FILE_BROWSER
                if (trackerController.currentScreen == ScreenType.FILE_BROWSER) {
                    // File browser: navigate to parent folder
                    fileBrowserState = fileBrowserModule.navigateToParent(fileBrowserState)
                    Log.d("Navigation", "R+LEFT: Navigated to parent: ${fileBrowserState.currentDirectory.absolutePath}")
                } else {
                    val (newScreen, newCol) = trackerController.navigateLeft(trackerController.currentScreen, trackerController.previousColumn)
                    if (newScreen != trackerController.currentScreen) {
                        // Capture cursor value from current screen before leaving
                        when (trackerController.currentScreen) {
                            ScreenType.PHRASE -> {
                                // Leaving PHRASE: remember instrument under cursor if not empty
                                val phraseState = PhraseEditorState(
                                    trackerController.project.phrases[trackerController.currentPhrase],
                                    trackerController.cursorRow,
                                    trackerController.cursorColumn,
                                    playbackRow = 0,
                                    isPlaying = trackerController.isPlaying()
                                )
                                val context = phraseEditorModule.getCursorContext(phraseState)
                                if (!context.capabilities.isEmpty) {
                                    val step = trackerController.project.phrases[trackerController.currentPhrase].steps[trackerController.cursorRow]
                                    trackerController.lastEditedInstrument = step.instrument
                                }
                            }
                            ScreenType.CHAIN -> {
                                // Leaving CHAIN: remember phrase under cursor
                                val phraseRef = trackerController.project.chains[trackerController.currentChain].phraseRefs[trackerController.cursorRow]
                                if (phraseRef >= 0) {
                                    trackerController.lastEditedPhrase = phraseRef
                                }
                            }
                            ScreenType.SONG -> {
                                // Leaving SONG: remember chain under cursor
                                val track = trackerController.project.tracks[trackerController.cursorColumn - 1]
                                if (trackerController.cursorRow < track.chainRefs.size && track.chainRefs[trackerController.cursorRow] >= 0) {
                                    trackerController.lastEditedChain = track.chainRefs[trackerController.cursorRow]
                                }
                            }
                            else -> {}
                        }

                        // Sync cursor state based on which screen we're entering
                        when (newScreen) {
                            ScreenType.PHRASE -> {
                                // Entering PHRASE from CHAIN/INSTRUMENT: sync to last phrase
                                trackerController.currentPhrase = trackerController.lastEditedPhrase
                            }
                            ScreenType.CHAIN -> {
                                // Entering CHAIN from PHRASE/SONG: sync to last chain
                                trackerController.currentChain = trackerController.lastEditedChain
                            }
                            ScreenType.INSTRUMENT -> {
                                // Entering INSTRUMENT from PHRASE: sync to last instrument
                                trackerController.currentInstrument = trackerController.lastEditedInstrument
                            }
                            else -> {}
                        }

                        // Screen changed - save current cursor, restore/reset for new screen, exit selection mode
                        trackerController.saveCursorForScreen(trackerController.currentScreen)
                        trackerController.restoreCursorForScreen(newScreen, cursorRemember)
                        trackerController.inputController.exitSelectionMode()
                    }
                    trackerController.currentScreen = newScreen
                    trackerController.previousColumn = newCol
                }
            },

            onRRight = {
                // R+RIGHT: QWERTY keyboard — move text cursor right
                if (qwertyKeyboardState.isOpen) {
                    qwertyKeyboardState = qwertyKeyboardState.moveTextCursorRight()
                } else if (eqEditorState.isOpen) { /* block map navigation while EQ editor open */
                } else if (trackerController.currentScreen != ScreenType.FILE_BROWSER &&
                           trackerController.currentScreen != ScreenType.SAMPLE_EDITOR) {
                // R+RIGHT: Navigate to screen on right in main row (disabled in FILE_BROWSER and SAMPLE_EDITOR)
                    val (newScreen, newCol) = trackerController.navigateRight(trackerController.currentScreen, trackerController.previousColumn)
                    if (newScreen != trackerController.currentScreen) {
                        // Capture cursor value from current screen before leaving
                        when (trackerController.currentScreen) {
                            ScreenType.PHRASE -> {
                                // Leaving PHRASE: remember instrument under cursor if not empty
                                val phraseState = PhraseEditorState(
                                    trackerController.project.phrases[trackerController.currentPhrase],
                                    trackerController.cursorRow,
                                    trackerController.cursorColumn,
                                    playbackRow = 0,
                                    isPlaying = trackerController.isPlaying()
                                )
                                val context = phraseEditorModule.getCursorContext(phraseState)
                                if (!context.capabilities.isEmpty) {
                                    val step = trackerController.project.phrases[trackerController.currentPhrase].steps[trackerController.cursorRow]
                                    trackerController.lastEditedInstrument = step.instrument
                                }
                            }
                            ScreenType.CHAIN -> {
                                // Leaving CHAIN: remember phrase under cursor
                                val phraseRef = trackerController.project.chains[trackerController.currentChain].phraseRefs[trackerController.cursorRow]
                                if (phraseRef >= 0) {
                                    trackerController.lastEditedPhrase = phraseRef
                                }
                            }
                            ScreenType.SONG -> {
                                // Leaving SONG: remember chain under cursor
                                val track = trackerController.project.tracks[trackerController.cursorColumn - 1]
                                if (trackerController.cursorRow < track.chainRefs.size && track.chainRefs[trackerController.cursorRow] >= 0) {
                                    trackerController.lastEditedChain = track.chainRefs[trackerController.cursorRow]
                                }
                            }
                            else -> {}
                        }

                        // Sync cursor state based on which screen we're entering
                        when (newScreen) {
                            ScreenType.INSTRUMENT -> {
                                // Entering INSTRUMENT from PHRASE: sync to last instrument
                                trackerController.currentInstrument = trackerController.lastEditedInstrument
                            }
                            ScreenType.PHRASE -> {
                                // Entering PHRASE from CHAIN: sync to last phrase
                                trackerController.currentPhrase = trackerController.lastEditedPhrase
                            }
                            ScreenType.CHAIN -> {
                                // Entering CHAIN from SONG: sync to last chain
                                trackerController.currentChain = trackerController.lastEditedChain
                            }
                            else -> {}
                        }

                        // Screen changed - save current cursor, restore/reset for new screen, exit selection mode
                        trackerController.saveCursorForScreen(trackerController.currentScreen)
                        trackerController.restoreCursorForScreen(newScreen, cursorRemember)
                        trackerController.inputController.exitSelectionMode()
                    }
                    trackerController.currentScreen = newScreen
                    trackerController.previousColumn = newCol
                }
            },

// ───────────────────────────────────────────────────────────────
// L + DIRECTION COMBINATIONS (Reserved for future use)
// Item cycling moved to B+direction, file browser actions moved to R+direction
// ───────────────────────────────────────────────────────────────
            onLLeft = {
                // L+LEFT: Reserved for future use (selection expand?)
            },

            onLRight = {
                // L+RIGHT: Reserved for future use (selection expand?)
            },

            onLUp = {
                // L+UP: Reserved for future use
            },

            onLDown = {
                // L+DOWN: Reserved for future use
            },

// ─────────────────────────────────────────────────────────────────────
// L+A: Cut (in selection) / Paste (outside selection)
// ─────────────────────────────────────────────────────────────────────
            onLA = {
                when (trackerController.currentScreen) {
                    ScreenType.PHRASE -> {
                        val action = trackerController.inputController.handleSelectA()
                        when (action) {
                            is InputAction.CUT -> {
                                // Cut selection
                                val bounds = trackerController.inputController.getSelectionBounds()
                                if (bounds != null) {
                                    clipboardManager.cutPhraseSteps(
                                        trackerController.project,
                                        trackerController.currentPhrase,
                                        bounds.topLeftRow, bounds.topLeftColumn,
                                        bounds.bottomRightRow, bounds.bottomRightColumn
                                    )
                                    trackerController.projectVersion++
                                    trackerController.inputController.exitSelectionMode()
                                    Log.d("CopyPaste", "Cut phrase selection")
                                }
                            }
                            is InputAction.PASTE -> {
                                // Paste at cursor
                                val result = clipboardManager.paste(
                                    trackerController.project,
                                    "PHRASE",
                                    trackerController.currentPhrase,
                                    trackerController.cursorRow,
                                    trackerController.cursorColumn
                                )
                                if (result is ClipboardManager.PasteResult.Success) {
                                    trackerController.projectVersion++
                                    Log.d("CopyPaste", "Pasted ${result.itemsPasted} items to phrase")
                                }
                            }
                            else -> { }
                        }
                    }
                    ScreenType.CHAIN -> {
                        val action = trackerController.inputController.handleSelectA()
                        when (action) {
                            is InputAction.CUT -> {
                                // Cut selection
                                val bounds = trackerController.inputController.getSelectionBounds()
                                if (bounds != null) {
                                    clipboardManager.cutChainRows(
                                        trackerController.project,
                                        trackerController.currentChain,
                                        bounds.topLeftRow, bounds.topLeftColumn,
                                        bounds.bottomRightRow, bounds.bottomRightColumn
                                    )
                                    trackerController.projectVersion++
                                    trackerController.inputController.exitSelectionMode()
                                    Log.d("CopyPaste", "Cut chain selection")
                                }
                            }
                            is InputAction.PASTE -> {
                                // Paste at cursor
                                val result = clipboardManager.paste(
                                    trackerController.project,
                                    "CHAIN",
                                    trackerController.currentChain,
                                    trackerController.cursorRow,
                                    trackerController.cursorColumn
                                )
                                if (result is ClipboardManager.PasteResult.Success) {
                                    trackerController.projectVersion++
                                    Log.d("CopyPaste", "Pasted ${result.itemsPasted} items to chain")
                                }
                            }
                            else -> { }
                        }
                    }
                    ScreenType.SONG -> {
                        val action = trackerController.inputController.handleSelectA()
                        when (action) {
                            is InputAction.CUT -> {
                                // Cut selection
                                val bounds = trackerController.inputController.getSelectionBounds()
                                if (bounds != null) {
                                    clipboardManager.cutSongCells(
                                        trackerController.project,
                                        bounds.topLeftRow, bounds.topLeftColumn,
                                        bounds.bottomRightRow, bounds.bottomRightColumn
                                    )
                                    trackerController.projectVersion++
                                    trackerController.inputController.exitSelectionMode()
                                    Log.d("CopyPaste", "Cut song selection")
                                }
                            }
                            is InputAction.PASTE -> {
                                // Paste at cursor
                                val result = clipboardManager.paste(
                                    trackerController.project,
                                    "SONG",
                                    0,  // Not used for song paste
                                    trackerController.cursorRow,
                                    trackerController.cursorColumn
                                )
                                if (result is ClipboardManager.PasteResult.Success) {
                                    trackerController.projectVersion++
                                    Log.d("CopyPaste", "Pasted ${result.itemsPasted} items to song")
                                }
                            }
                            else -> { }
                        }
                    }
                    ScreenType.TABLE -> {
                        val action = trackerController.inputController.handleSelectA()
                        when (action) {
                            is InputAction.CUT -> {
                                // Cut selection
                                val bounds = trackerController.inputController.getSelectionBounds()
                                if (bounds != null) {
                                    clipboardManager.cutTableRows(
                                        trackerController.project,
                                        trackerController.currentTable,
                                        bounds.topLeftRow, bounds.topLeftColumn,
                                        bounds.bottomRightRow, bounds.bottomRightColumn
                                    )
                                    trackerController.projectVersion++
                                    audioEngine.invalidateTable(trackerController.currentTable)
                                    trackerController.inputController.exitSelectionMode()
                                    Log.d("CopyPaste", "Cut table selection")
                                }
                            }
                            is InputAction.PASTE -> {
                                // Paste at cursor
                                val result = clipboardManager.paste(
                                    trackerController.project,
                                    "TABLE",
                                    trackerController.currentTable,
                                    trackerController.tableCursorRow,
                                    trackerController.tableCursorColumn
                                )
                                if (result is ClipboardManager.PasteResult.Success) {
                                    trackerController.projectVersion++
                                    audioEngine.invalidateTable(trackerController.currentTable)
                                    Log.d("CopyPaste", "Pasted ${result.itemsPasted} items to table")
                                }
                            }
                            else -> { }
                        }
                    }
                    else -> { }
                }
            },

// ─────────────────────────────────────────────────────────────────────
// L+B: Enter/cycle selection mode (CELL → ROW → SCREEN)
// ─────────────────────────────────────────────────────────────────────
            onLB = {
                when (trackerController.currentScreen) {
                    ScreenType.PHRASE -> {
                        // Enter/cycle selection mode (CELL → ROW → SCREEN)
                        trackerController.inputController.handleSelectB(
                            trackerController.cursorRow,
                            trackerController.cursorColumn,
                            9  // Max column for phrase (FX3 value)
                        )
                        Log.d("CopyPaste", "Selection: ${trackerController.inputController.getSelectionInfo()}")
                    }
                    ScreenType.CHAIN -> {
                        // Enter/cycle selection mode
                        trackerController.inputController.handleSelectB(
                            trackerController.cursorRow,
                            trackerController.cursorColumn,
                            2  // Max column for chain (transpose)
                        )
                        Log.d("CopyPaste", "Selection: ${trackerController.inputController.getSelectionInfo()}")
                    }
                    ScreenType.SONG -> {
                        // Enter/cycle selection mode
                        trackerController.inputController.handleSelectB(
                            trackerController.cursorRow,
                            trackerController.cursorColumn,
                            8  // Max column for song (track 8)
                        )
                        Log.d("CopyPaste", "Selection: ${trackerController.inputController.getSelectionInfo()}")
                    }
                    ScreenType.TABLE -> {
                        // Enter/cycle selection mode
                        trackerController.inputController.handleSelectB(
                            trackerController.tableCursorRow,
                            trackerController.tableCursorColumn,
                            8  // Max column for table (fx3 value)
                        )
                        Log.d("CopyPaste", "Selection: ${trackerController.inputController.getSelectionInfo()}")
                    }
                    else -> { }
                }
            },

// ─────────────────────────────────────────────────────────────────────
// SELECT+A: Rename file/folder (file browser only)
// ─────────────────────────────────────────────────────────────────────
            onSelectA = {
                if (trackerController.currentScreen == ScreenType.FILE_BROWSER &&
                    fileBrowserState.mode == FileBrowserModule.BrowserMode.NORMAL) {
                    val item = fileBrowserState.items.getOrNull(fileBrowserState.cursor)
                    if (item != null && item !is FileBrowserModule.BrowserItem.Parent) {
                        val file = item.file
                        val isWav = file.extension.lowercase() == "wav"
                        val isPtp = file.extension.lowercase() == "ptp"
                        val fieldLabel = when {
                            file.isDirectory -> "FOLDER NAME:"
                            isWav -> "SAMPLE NAME:"
                            isPtp -> "PROJECT NAME:"
                            else -> "FILE NAME:"
                        }
                        val baseName = file.nameWithoutExtension
                        qwertyKeyboardState = QwertyKeyboardState(
                            isOpen = true,
                            text = baseName,
                            textCursor = baseName.length.coerceAtMost(20),
                            fieldLabel = fieldLabel,
                            originalText = baseName,
                            insertBefore = insertBefore,
                            context = QwertyContext.FILE_RENAME,
                            contextExtra = file.absolutePath
                        )
                    }
                }
            },

// ─────────────────────────────────────────────────────────────────────
// SELECT+B: Delete file/folder (file browser only)
// ─────────────────────────────────────────────────────────────────────
            onSelectB = {
                if (trackerController.currentScreen == ScreenType.FILE_BROWSER &&
                    fileBrowserState.mode == FileBrowserModule.BrowserMode.NORMAL) {
                    val item = fileBrowserState.items.getOrNull(fileBrowserState.cursor)
                    if (item != null && item !is FileBrowserModule.BrowserItem.Parent) {
                        fileBrowserState = fileBrowserState.copy(
                            mode = FileBrowserModule.BrowserMode.DELETE,
                            statusMessage = "",
                            statusSuccess = true
                        )
                    }
                }
            },

// ─────────────────────────────────────────────────────────────────────
// SELECT+R: Create new folder
// ─────────────────────────────────────────────────────────────────────
            onSelectR = {
                if (trackerController.currentScreen == ScreenType.FILE_BROWSER &&
                    fileBrowserState.mode == FileBrowserModule.BrowserMode.NORMAL) {
                    val currentDir = fileBrowserState.currentDirectory
                    val initialText = "NEW FOLDER"
                    qwertyKeyboardState = QwertyKeyboardState(
                        isOpen = true,
                        text = initialText,
                        textCursor = initialText.length.coerceAtMost(20),
                        fieldLabel = "FOLDER NAME:",
                        originalText = initialText,
                        insertBefore = insertBefore,
                        context = QwertyContext.FOLDER_CREATE,
                        contextExtra = currentDir.absolutePath
                    )
                }
            },

// ─────────────────────────────────────────────────────────────────────
// A,A: Insert next unused chain/phrase/note
// ─────────────────────────────────────────────────────────────────────
            onAA = { run aaHandler@{
                val currentScreen = trackerController.currentScreen

                // Song + selection mode → open QWERTY for resample name
                if (currentScreen == ScreenType.SONG && trackerController.inputController.isSelectionModeActive()) {
                    val suggestedName = renderController.generateResampledBaseName()
                    qwertyKeyboardState = QwertyKeyboardState(
                        isOpen = true,
                        text = suggestedName,
                        textCursor = suggestedName.length.coerceAtMost(20),
                        fieldLabel = "SAMPLE NAME:",
                        originalText = suggestedName,
                        insertBefore = insertBefore,
                        clearOnFirstB = true,
                        context = QwertyContext.RESAMPLE
                    )
                    return@aaHandler
                }

                // For SONG/CHAIN/PHRASE: only act if the previous single-A press
                // inserted into an empty cell at the current position
                val currentPos = InsertPosition(currentScreen, trackerController.cursorRow, trackerController.cursorColumn)
                if (lastAInsertPosition != currentPos) return@aaHandler
                lastAInsertPosition = null  // consume the position

                when (currentScreen) {
                    ScreenType.SONG -> {
                        val track = trackerController.project.tracks[trackerController.cursorColumn - 1]
                        // The first-A value is already in the cell; find next unused (won't include it
                        // since it's already referenced in the song)
                        val usedChains = trackerController.project.tracks
                            .flatMap { it.chainRefs }
                            .filter { it != -1 }
                            .toSet()
                        val nextUnused = (0..255).firstOrNull { it !in usedChains }
                        if (nextUnused != null) {
                            track.chainRefs[trackerController.cursorRow] = nextUnused
                            trackerController.lastEditedChain = nextUnused
                            trackerController.projectVersion++
                            Log.d("AA", "Inserted next unused chain $nextUnused")
                        }
                    }
                    ScreenType.CHAIN -> {
                        val chain = trackerController.project.chains[trackerController.currentChain]
                        // First-A already filled the slot; find next phrase not in chain
                        val usedPhrases = chain.phraseRefs.filter { it != -1 }.toSet()
                        val nextUnused = (0..255).firstOrNull { it !in usedPhrases }
                        if (nextUnused != null) {
                            chain.phraseRefs[trackerController.cursorRow] = nextUnused
                            chain.transposeValues[trackerController.cursorRow] = trackerController.lastEditedTranspose
                            trackerController.lastEditedPhrase = nextUnused
                            trackerController.projectVersion++
                            Log.d("AA", "Inserted next unused phrase $nextUnused")
                        }
                    }
                    else -> {}
                }
            } },

// ─────────────────────────────────────────────────────────────────────
// L+B+A: Clone current item to next unused slot
// ─────────────────────────────────────────────────────────────────────
            onLBA = {
                when (trackerController.currentScreen) {
                    ScreenType.SONG -> {
                        val track = trackerController.project.tracks[trackerController.cursorColumn - 1]
                        val currentChainId = track.chainRefs.getOrNull(trackerController.cursorRow) ?: -1
                        if (currentChainId != -1) {
                            val usedChains = trackerController.project.tracks
                                .flatMap { it.chainRefs }
                                .filter { it != -1 }
                                .toSet()
                            val nextUnused = (0..255).firstOrNull { it !in usedChains }
                            if (nextUnused != null) {
                                val src = trackerController.project.chains[currentChainId]
                                val dst = trackerController.project.chains[nextUnused]
                                src.phraseRefs.copyInto(dst.phraseRefs)
                                src.transposeValues.copyInto(dst.transposeValues)
                                track.chainRefs[trackerController.cursorRow] = nextUnused
                                trackerController.lastEditedChain = nextUnused
                                trackerController.projectVersion++
                                Log.d("LBA", "Cloned chain $currentChainId → $nextUnused")
                            }
                        }
                    }
                    ScreenType.CHAIN -> {
                        val chain = trackerController.project.chains[trackerController.currentChain]
                        val currentPhraseId = chain.phraseRefs[trackerController.cursorRow]
                        if (currentPhraseId != -1) {
                            val usedPhrases = chain.phraseRefs.filter { it != -1 }.toSet()
                            val nextUnused = (0..255).firstOrNull { it !in usedPhrases }
                            if (nextUnused != null) {
                                val src = trackerController.project.phrases[currentPhraseId]
                                val dst = trackerController.project.phrases[nextUnused]
                                src.steps.forEachIndexed { i, step -> dst.steps[i] = step.copy() }
                                chain.phraseRefs[trackerController.cursorRow] = nextUnused
                                trackerController.lastEditedPhrase = nextUnused
                                trackerController.projectVersion++
                                Log.d("LBA", "Cloned phrase $currentPhraseId → $nextUnused")
                            }
                        }
                    }
                    ScreenType.PHRASE -> {
                        val srcPhraseId = trackerController.currentPhrase
                        val usedPhrases = trackerController.project.chains
                            .flatMap { it.phraseRefs.toList() }
                            .filter { it != -1 }
                            .toSet()
                        val nextUnused = (0..255).firstOrNull { it !in usedPhrases }
                        if (nextUnused != null) {
                            val src = trackerController.project.phrases[srcPhraseId]
                            val dst = trackerController.project.phrases[nextUnused]
                            src.steps.forEachIndexed { i, step -> dst.steps[i] = step.copy() }
                            trackerController.currentPhrase = nextUnused
                            trackerController.projectVersion++
                            Log.d("LBA", "Cloned phrase $srcPhraseId → $nextUnused, navigating there")
                        }
                    }
                    else -> {}
                }
                // Always exit selection mode after cloning
                if (trackerController.inputController.isSelectionModeActive()) {
                    trackerController.inputController.exitSelectionMode()
                }
            },

// ─────────────────────────────────────────────────────────────────────
// L+R: Exit selection mode (fixes L+A cut combo bug)
// ─────────────────────────────────────────────────────────────────────
            onLR = {
                if (trackerController.inputController.isSelectionModeActive()) {
                    trackerController.inputController.exitSelectionMode()
                    Log.d("Selection", "L+R: Exited selection mode")
                }
            },

// ─────────────────────────────────────────────────────────────────────
// A RELEASED: close FX helper overlay and apply selected effect
// ─────────────────────────────────────────────────────────────────────
            onAReleased = {
                if (fxHelperState.isOpen) {
                    applyFxTypeChange(fxHelperState.selectedEffectCode())
                    fxHelperState = FxHelperState()
                }
            }
        )
    }

// ═══════════════════════════════════════════════════════════════════════
// KEYBOARD INPUT MAPPING
// ═══════════════════════════════════════════════════════════════════════

// Create the input mapper to handle keyboard input
// This maps WASD/JK/UI/Shift/Space to game buttons
    val inputMapper = remember(buttonHandlers) {
        InputMapper(buttonHandlers, logInput = true)  // Enable logging for debugging
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