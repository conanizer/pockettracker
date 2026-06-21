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
import androidx.core.splashscreen.SplashScreen.Companion.installSplashScreen
import androidx.core.view.WindowCompat
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
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
import com.conanizer.pockettracker.core.logic.RenderController
import com.conanizer.pockettracker.platform.android.OboeAudioBackend
import androidx.compose.runtime.rememberCoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import com.conanizer.pockettracker.platform.android.AndroidResourceLoader
import com.conanizer.pockettracker.platform.android.AndroidFileSystem
import com.conanizer.pockettracker.platform.android.ThemeLoader
import com.conanizer.pockettracker.platform.android.AndroidVideoAudioExtractor
import com.conanizer.pockettracker.platform.android.ButtonSoundManager
import com.conanizer.pockettracker.platform.android.ButtonHapticManager
import com.conanizer.pockettracker.core.storage.FileInfo
import androidx.compose.ui.graphics.asImageBitmap
import com.conanizer.pockettracker.input.AppControllers
import com.conanizer.pockettracker.input.AppInputDispatcher
import com.conanizer.pockettracker.input.AppStateRefs
import com.conanizer.pockettracker.input.InputMapper
import com.conanizer.pockettracker.input.InsertPosition
import com.conanizer.pockettracker.input.LocalButtonEventCallback
import com.conanizer.pockettracker.platform.android.DeviceAdapter
import com.conanizer.pockettracker.ui.FullScreenLayout
import com.conanizer.pockettracker.ui.LandscapeLayoutWithVirtualButtons
import com.conanizer.pockettracker.ui.LocalAppTheme
import com.conanizer.pockettracker.ui.LocalLayoutMode
import com.conanizer.pockettracker.ui.PortraitLayout2WithVirtualButtons
import com.conanizer.pockettracker.ui.PortraitLayoutWithVirtualButtons
import com.conanizer.pockettracker.ui.TrackerScreenParams
import com.conanizer.pockettracker.ui.modules.ChainEditorModule
import com.conanizer.pockettracker.ui.modules.EffectModule
import com.conanizer.pockettracker.ui.modules.EqModule
import com.conanizer.pockettracker.ui.modules.FileBrowserModule
import com.conanizer.pockettracker.ui.modules.GrooveModule
import com.conanizer.pockettracker.ui.modules.InstrumentModule
import com.conanizer.pockettracker.ui.modules.InstrumentPoolModule
import com.conanizer.pockettracker.ui.modules.MixerModule
import com.conanizer.pockettracker.ui.modules.ModulationModule
import com.conanizer.pockettracker.ui.modules.PhraseEditorModule
import com.conanizer.pockettracker.ui.modules.ProjectModule
import com.conanizer.pockettracker.ui.modules.SampleEditorModule
import com.conanizer.pockettracker.ui.modules.SampleEditorState
import com.conanizer.pockettracker.ui.modules.SettingsModule
import com.conanizer.pockettracker.ui.modules.SongEditorModule
import com.conanizer.pockettracker.ui.modules.TableModule
import com.conanizer.pockettracker.ui.modules.ThemeEditorState
import com.conanizer.pockettracker.ui.overlays.EqCallerContext
import com.conanizer.pockettracker.ui.overlays.EqEditorState
import com.conanizer.pockettracker.ui.overlays.FxHelperState
import com.conanizer.pockettracker.ui.overlays.QwertyKeyboardState
import com.conanizer.pockettracker.ui.theme.AppTheme
import com.conanizer.pockettracker.ui.theme.DeviceTheme
import java.io.File

fun File.toFileInfo(): FileInfo = FileInfo(
    path = absolutePath,
    name = name,
    extension = extension,
    isDirectory = isDirectory,
    size = if (isFile) length() else 0L,
    lastModified = lastModified()
)

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
        installSplashScreen()
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

// Crash-recovery autosave debounce: write this long after the last edit, so a burst of edits
// coalesces into a single write (REVIEW-3 5.3, Phase A).
private const val AUTOSAVE_DEBOUNCE_MS = 3000L

@Composable
fun PocketTrackerApp(layoutConfig: DeviceAdapter.LayoutConfig, deviceAdapter: DeviceAdapter) {
    val context = LocalContext.current
    val coroutineScope = rememberCoroutineScope()

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
                    Log.w("Permissions", "App-specific intent failed, trying general intent: ${e.message}")
                    try {
                        val intent = Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)
                        context.startActivity(intent)
                    } catch (e2: Exception) {
                        // Some custom ROMs (e.g. /e/OS) may not expose this settings page at all.
                        // The user will see the "NO STORAGE PERMISSION" message in the file browser instead.
                        Log.e("Permissions", "All Files Access settings unavailable: ${e2.message}")
                    }
                }
            } else {
                Log.d("Permissions", "✅ MANAGE_EXTERNAL_STORAGE already granted")
            }
        }
    }
    val fileSystem = remember { AndroidFileSystem(context) }
    val videoExtractor = remember { AndroidVideoAudioExtractor() }

    val logger = remember { com.conanizer.pockettracker.platform.android.AndroidLogger() }

    // stateVersion.let { } creates a Compose dependency so reads recompose when controllers mutate state.
    var stateVersion by remember { mutableIntStateOf(0) }
    val stateObserver = remember {
        object : com.conanizer.pockettracker.core.logic.StateObserver {
            override fun onStateChanged() {
                stateVersion++  // Trigger Compose recomposition
            }
        }
    }

    val fileController = remember { FileController(fileSystem, logger) }
    val autosaveManager = remember {
        com.conanizer.pockettracker.core.logic.AutosaveManager(fileController, logger)
    }

    val audioBackend = remember { OboeAudioBackend() }
    val resourceLoader = remember { AndroidResourceLoader(context) }
    val audioEngine = remember { AudioEngine(audioBackend, resourceLoader, logger) }

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
    // Native-heap baseline for the PROJECT sample-RAM readout (REVIEW-3 5.1). Captured below right
    // after the engine's fixed DSP is allocated but before any samples load, so the readout can show
    // (current native heap − baseline) ≈ the PCM of the samples/soundfonts the user has loaded.
    // −1 = not captured yet.
    var nativeHeapBaseline by remember { mutableStateOf(-1L) }
    LaunchedEffect(Unit) {
        withContext(Dispatchers.IO) {
            audioEngine.create()
        }
        // Engine DSP is now allocated and no user samples are loaded yet (the template/recovery sample
        // loads are keyed on audioReady, set on the next line) → this is the clean "zero samples" baseline.
        nativeHeapBaseline = android.os.Debug.getNativeHeapAllocatedSize()
        audioReady = true
    }

    val instrumentController = remember {
        InstrumentController(audioEngine, logger, stateObserver, fileController)
    }

    val effectProcessor = remember {
        com.conanizer.pockettracker.core.logic.EffectProcessor(audioBackend, logger)
    }

    val playbackController = remember {
        PlaybackController(audioEngine, effectProcessor, logger, stateObserver)
    }
    LaunchedEffect(playbackController, instrumentController) {
        playbackController.instrumentController = instrumentController
    }

    val clipboardManager = remember {
        com.conanizer.pockettracker.core.logic.ClipboardManager(logger)
    }

    val renderController = remember {
        RenderController(audioEngine, playbackController, fileSystem, logger)
    }

    val _isRendering = remember { mutableStateOf(false) }
    var isRendering by _isRendering
    val _isStemsRendering = remember { mutableStateOf(false) }
    var isStemsRendering by _isStemsRendering
    val _renderProgress = remember { mutableFloatStateOf(0f) }
    var renderProgress by _renderProgress
    // Sample-RAM readout value for the PROJECT screen (REVIEW-3 5.1) — native-heap growth since the
    // startup baseline, refreshed by the poll below while the project screen is visible.
    val _sampleRamBytes = remember { mutableStateOf(0L) }
    var sampleRamBytes by _sampleRamBytes

    val _showCleanDialog = remember { mutableStateOf(false) }
    var showCleanDialog by _showCleanDialog
    val _cleanDialogTarget = remember { mutableStateOf("") }  // "SEQ" or "INST"
    var cleanDialogTarget by _cleanDialogTarget
    val _cleanDialogCursor = remember { mutableIntStateOf(0) }  // 0=YES, 1=NO
    var cleanDialogCursor by _cleanDialogCursor
    val _showNewProjectDialog = remember { mutableStateOf(false) }
    var showNewProjectDialog by _showNewProjectDialog
    val _showInstrTypeDialog = remember { mutableStateOf(false) }
    var showInstrTypeDialog by _showInstrTypeDialog
    val _showRecoveryDialog = remember { mutableStateOf(false) }
    var showRecoveryDialog by _showRecoveryDialog

    // Tracks where the last single A-press inserted into an empty cell (screen, row, col)
    // Used by A+A to decide whether to insert next-unused (only allowed on same cell)
    val _lastAInsertPosition = remember { mutableStateOf<InsertPosition?>(null) }
    var lastAInsertPosition by _lastAInsertPosition

    val inputController = remember {
        com.conanizer.pockettracker.core.logic.InputController(logger, stateObserver)
    }

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

    // Load saved template project on startup (data only; samples loaded after audio is ready below)
    val templateLoaded = remember {
        val result = fileController.loadTemplate()
        if (result is FileController.LoadResult.Success) {
            trackerController.project = result.project
            true
        } else false
    }

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
        audioBackend.setLimiterPreGain(trackerController.project.limiterPreGain)
        Log.d("VolumeSync", "Initial volume sync to audio backend complete")
    }

    val chainEditorModule = remember { ChainEditorModule() }
    val phraseEditorModule = remember { PhraseEditorModule() }
    val songEditorModule = remember { SongEditorModule() }
    val projectModule = remember { ProjectModule() }
    val settingsModule = remember { SettingsModule() }
    val instrumentModule = remember { InstrumentModule() }
    val instrumentPoolModule = remember { InstrumentPoolModule() }
    val mixerModule = remember { MixerModule() }
    val effectModule = remember { EffectModule() }
    val eqModule     = remember { EqModule() }
    val tableModule = remember { TableModule() }
    val grooveModule = remember { GrooveModule() }
    val modulationModule = remember { ModulationModule() }

    val trackPeakBuffer = remember { FloatArray(16) }
    val masterPeakBuffer = remember { FloatArray(2) }
    val sendPeakBuffer = remember { FloatArray(4) }  // [revL, revR, delL, delR]

    val autoLayoutMode = when {
        !layoutConfig.needsVirtualButtons -> DeviceAdapter.LayoutMode.FULL
        layoutConfig.isLandscape          -> DeviceAdapter.LayoutMode.TOUCH_LANDSCAPE
        else                              -> DeviceAdapter.LayoutMode.TOUCH_PORTRAIT2
    }

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
            DeviceAdapter.ScalingMode.entries.firstOrNull { it.name == savedScalingName } ?: DeviceAdapter.ScalingMode.BILINEAR
        } else {
            DeviceAdapter.ScalingMode.BILINEAR
        }
    }

    val _layoutMode  = remember { mutableStateOf(initialLayoutMode) }
    var layoutMode   by _layoutMode
    val _scalingMode = remember { mutableStateOf(initialScalingMode) }
    var scalingMode  by _scalingMode

    LaunchedEffect(layoutMode) {
        prefs.edit().putString("layout_mode", layoutMode.name).apply()
    }
    LaunchedEffect(scalingMode) {
        prefs.edit().putString("scaling_mode", scalingMode.name).apply()
    }

    val _buttonSoundEnabled = remember { mutableStateOf(prefs.getBoolean("button_sound", true)) }
    var buttonSoundEnabled  by _buttonSoundEnabled
    val _buttonSoundVolume  = remember { mutableStateOf(prefs.getInt("button_sound_volume", 255)) }
    var buttonSoundVolume   by _buttonSoundVolume
    val _buttonVibroEnabled = remember { mutableStateOf(prefs.getBoolean("button_vibro", true)) }
    var buttonVibroEnabled  by _buttonVibroEnabled
    val _vibroPower         = remember { mutableStateOf(prefs.getInt("vibro_power", 255)) }
    var vibroPower          by _vibroPower

    val _insertBefore = remember { mutableStateOf(prefs.getBoolean("kb_insert_before", true)) }
    var insertBefore  by _insertBefore

    // Cursor remember mode: REMEMBER=true keeps cursor position between screen switches,
    // REFRESH=false resets cursor to default on every screen switch (persisted)
    val _cursorRemember = remember { mutableStateOf(prefs.getBoolean("cursor_remember", false)) }
    var cursorRemember  by _cursorRemember

    val _notePreviewEnabled = remember { mutableStateOf(prefs.getBoolean("note_preview", true)) }
    var notePreviewEnabled  by _notePreviewEnabled

    // Autosave recovery behaviour (REVIEW-3 5.3 follow-up). ASK (false) shows the RECOVER WORK?
    // prompt on launch; AUTO (true) silently restores the autosave with no prompt. Per-device
    // (SharedPreferences, not the project file) so it can differ between handhelds that kill the
    // app on background (AUTO) and phones that keep it warm (ASK).
    val _autosaveResumeAuto = remember { mutableStateOf(prefs.getBoolean("autosave_resume_auto", false)) }
    var autosaveResumeAuto  by _autosaveResumeAuto

    // Overlay: list files from assets/overlays/, load + process selected bitmap
    val overlayFiles: List<String> = remember {
        try { context.assets.list("overlays")
                ?.filter { it.endsWith(".png") }
                ?.map { it.removeSuffix(".png") }
                ?: emptyList()
        } catch (_: Exception) { emptyList() }
    }
    val _overlayName     = remember { mutableStateOf(prefs.getString("overlay_name", "OFF") ?: "OFF") }
    var overlayName      by _overlayName
    val _overlayStrength = remember { mutableStateOf(prefs.getInt("overlay_strength", 128)) }
    var overlayStrength  by _overlayStrength

    // Load overlay PNG as-is — no processing. The PNG is designed for direct use;
    // alpha = STR/255f at draw time is the only control needed.
    val overlayBitmap: androidx.compose.ui.graphics.ImageBitmap? = remember(overlayName) {
        if (overlayName == "OFF") null
        else try {
            android.graphics.BitmapFactory.decodeStream(
                context.assets.open("overlays/$overlayName.png"))?.asImageBitmap()
        } catch (_: Exception) { null }
    }

    val _qwertyKeyboardState = remember { mutableStateOf(QwertyKeyboardState()) }
    var qwertyKeyboardState  by _qwertyKeyboardState
    val _fxHelperState = remember { mutableStateOf(FxHelperState()) }
    var fxHelperState  by _fxHelperState
    val _eqEditorState      = remember { mutableStateOf(EqEditorState()) }
    var eqEditorState       by _eqEditorState
    val _themeEditorState   = remember { mutableStateOf(ThemeEditorState()) }
    var themeEditorState    by _themeEditorState
    val _eqSpectrumData = remember { mutableStateOf<FloatArray?>(null) }
    var eqSpectrumData  by _eqSpectrumData

    val _appTheme = remember {
        val savedJson = prefs.getString("app_theme", null)
        val initial = if (savedJson != null) {
            try { Json { ignoreUnknownKeys = true }.decodeFromString<AppTheme>(savedJson) }
            catch (_: Exception) { AppTheme.CLASSIC }
        } else { AppTheme.CLASSIC }
        mutableStateOf(initial)
    }
    var appTheme  by _appTheme

    val buttonSoundManager = remember { ButtonSoundManager(context) }
    val buttonHapticManager = remember { ButtonHapticManager(context) }

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
    LaunchedEffect(notePreviewEnabled) {
        prefs.edit().putBoolean("note_preview", notePreviewEnabled).apply()
    }
    LaunchedEffect(autosaveResumeAuto) {
        prefs.edit().putBoolean("autosave_resume_auto", autosaveResumeAuto).apply()
    }
    LaunchedEffect(overlayName) {
        prefs.edit().putString("overlay_name", overlayName).apply()
    }
    LaunchedEffect(overlayStrength) {
        prefs.edit().putInt("overlay_strength", overlayStrength).apply()
    }
    LaunchedEffect(appTheme) {
        prefs.edit().putString("app_theme", Json { prettyPrint = false }.encodeToString(appTheme)).apply()
    }

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

    val projectStatusMessage = stateVersion.let { trackerController.statusMessage }
    val projectStatusSuccess = stateVersion.let { trackerController.statusSuccess }

    // Auto-dismiss status messages after 5 seconds
    LaunchedEffect(trackerController.statusMessage) {
        if (trackerController.statusMessage.isNotEmpty()) {
            kotlinx.coroutines.delay(5000)
            trackerController.clearStatus()
        }
    }

    LaunchedEffect(instrumentController.statusMessage) {
        if (instrumentController.statusMessage.isNotEmpty()) {
            kotlinx.coroutines.delay(5000)
            instrumentController.clearStatus()
        }
    }

    // Crash-recovery autosave (REVIEW-3 5.3, Phase A). Every edit bumps projectVersion, which re-keys
    // this effect: a new edit cancels the pending delay, so a burst of edits coalesces into one write
    // AUTOSAVE_DEBOUNCE_MS after the user pauses. Only writes when dirty (skips load/new/save, which
    // already match a clean file). Playback-agnostic by design — autosave() serializes here on the main
    // thread (tear-free) and writes on IO, and the audio engine is native, so this can't glitch audio.
    LaunchedEffect(projectVersion) {
        if (trackerController.isProjectDirty) {
            kotlinx.coroutines.delay(AUTOSAVE_DEBOUNCE_MS)
            // Re-check: a save during the debounce window clears dirty + deletes the file, and this
            // effect isn't re-keyed by a save (it doesn't bump projectVersion), so without this we'd
            // re-create autosave.ptp after a clean save and trigger a false recovery next launch.
            if (trackerController.isProjectDirty) {
                autosaveManager.autosave(trackerController.project)
            }
        }
    }

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

    val sampleEditorModule = remember { SampleEditorModule() }
    val _sampleEditorState = remember { mutableStateOf(
        SampleEditorState(
            sampleId = 0,
            instrumentId = 0
        )
    ) }
    var sampleEditorState  by _sampleEditorState

    // Reset note/volume combo when leaving PHRASE screen
    // (instrument is kept so quick-insert uses the last instrument you worked with)
    var wasPhraseScreen by remember { mutableStateOf(false) }
    LaunchedEffect(trackerController.currentScreen) {
        val isPhrase = trackerController.currentScreen == ScreenType.PHRASE
        if (wasPhraseScreen && !isPhrase) {
            trackerController.lastEditedNote = com.conanizer.pockettracker.core.data.Note.fromMidi(60) // C-4
            trackerController.lastEditedVolume = 0x7F  // max velocity (VOL column is now 0x00-0x7F)
        }
        wasPhraseScreen = isPhrase
    }

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

    // Poll spectrum magnitudes for EQ visualizer (~20fps while EQ screen is open).
    // The source depends on which EQ context is open so the spectrum reflects the actual signal path.
    LaunchedEffect(eqEditorState.isOpen) {
        if (eqEditorState.isOpen) {
            while (true) {
                val ctx = eqEditorState.callerContext
                val source = when (ctx) {
                    is EqCallerContext.DelayInputEq  -> 1
                    is EqCallerContext.ReverbInputEq -> 2
                    is EqCallerContext.InstrumentEq  -> 3
                    else                             -> 0  // MasterEq, SampleEditorFx → master bus
                }
                val instrId = if (ctx is EqCallerContext.InstrumentEq) ctx.instrId else -1
                eqSpectrumData = audioEngine.getSpectrumMagnitudesForSource(source, instrId, 620)
                delay(50)
            }
        } else {
            eqSpectrumData = null
        }
    }

    // Decay peaks manually when not playing — audio callback is not running so peaks freeze on stop.
    LaunchedEffect(currentScreen) {
        if (currentScreen == ScreenType.MIXER) {
            while (true) {
                if (!trackerController.isPlaying()) {
                    audioBackend.decayPeaks()
                    audioBackend.decayWaveform()
                }
                audioBackend.getTrackPeaks(trackPeakBuffer)
                audioBackend.getMasterPeaks(masterPeakBuffer)
                audioBackend.getSendPeaks(sendPeakBuffer)
                stateVersion++  // Trigger recomposition
                kotlinx.coroutines.delay(60)
            }
        }
    }

    // Sample-RAM readout poll for the PROJECT screen (REVIEW-3 5.1). While the project screen is shown,
    // refresh (current native heap − startup baseline) ≈ loaded sample/soundfont RAM. Android's getter
    // is cheap; the State only recomposes when the MB value actually changes. coerceAtLeast(0) guards
    // the brief window before the baseline is captured (and any native shrink below it).
    LaunchedEffect(currentScreen) {
        if (currentScreen == ScreenType.PROJECT) {
            while (true) {
                val base = if (nativeHeapBaseline >= 0L) nativeHeapBaseline
                           else android.os.Debug.getNativeHeapAllocatedSize()
                sampleRamBytes = (android.os.Debug.getNativeHeapAllocatedSize() - base).coerceAtLeast(0L)
                kotlinx.coroutines.delay(500)
            }
        }
    }

    val appCtrl = remember {
        AppControllers(
            trackerController, audioEngine, audioBackend, instrumentController,
            fileController, renderController, clipboardManager, deviceAdapter,
            videoExtractor, fileSystem, coroutineScope,
            chainEditorModule, phraseEditorModule, songEditorModule, projectModule,
            settingsModule, instrumentModule, instrumentPoolModule, mixerModule, effectModule, eqModule,
            tableModule, grooveModule, modulationModule, fileBrowserModule, sampleEditorModule
        )
    }
    val appState = remember {
        AppStateRefs(
            _fileBrowserState, _sampleEditorState, _qwertyKeyboardState,
            _fxHelperState, _eqEditorState, _eqSpectrumData,
            _layoutMode, _scalingMode, _isRendering, _isStemsRendering, _renderProgress,
            _showCleanDialog, _cleanDialogTarget, _cleanDialogCursor,
            _lastAInsertPosition, _insertBefore, _instrumentFileBrowserAction,
            _previousScreen, _buttonSoundEnabled, _buttonSoundVolume,
            _buttonVibroEnabled, _vibroPower, _cursorRemember, _notePreviewEnabled,
            _overlayName, _overlayStrength, overlayFiles,
            trackPeakBuffer, masterPeakBuffer, sendPeakBuffer, _appTheme, _themeEditorState,
            _showNewProjectDialog, _showInstrTypeDialog, _showRecoveryDialog,
            _autosaveResumeAuto
        )
    }
    val dispatcher = remember { AppInputDispatcher(appCtrl, appState) }

    val buttonHandlers = remember { dispatcher.createButtonHandlers() }

    // Load template samples after audio engine is ready
    LaunchedEffect(audioReady) {
        if (!audioReady || !templateLoaded) return@LaunchedEffect
        dispatcher.reloadProjectSamples()
        dispatcher.syncVolumesToAudioBackend()
    }

    val inputMapper = remember(buttonHandlers) {
        InputMapper(buttonHandlers)
    }
    val focusRequester = remember { FocusRequester() }

    LaunchedEffect(Unit) {
        kotlinx.coroutines.delay(100)
        try {
            focusRequester.requestFocus()
        } catch (e: Exception) { }
    }

    // Crash-recovery at launch (REVIEW-3 5.3 Phase B + RESUME-mode follow-up): an autosave surviving
    // to launch means the last session didn't exit cleanly (a clean save/load/new deletes it). What we
    // do with it depends on the per-device RESUME setting:
    //   • AUTO  → silently restore it, no prompt (for handhelds that kill the app on background, where
    //             a prompt every return is noise). recoverFromAutosave leaves the project dirty so it
    //             keeps re-autosaving and survives the next kill; reload samples since the autosave
    //             stores paths, not PCM. A corrupt autosave is dropped so AUTO can't loop on it.
    //   • ASK   → the recovery prompt. We request focus as the dialog appears: B is KEYCODE_BACK on the
    //             Miyoo, and audioReady can flip after a focus-losing recomposition — without focus, A/B
    //             never reach the app and B falls through to the system (minimizing it) instead of
    //             dismissing the dialog.
    LaunchedEffect(audioReady) {
        if (audioReady && fileController.hasAutosave()) {
            if (autosaveResumeAuto) {
                if (trackerController.recoverFromAutosave()) dispatcher.reloadProjectSamples()
                else fileController.clearAutosave()
            } else {
                showRecoveryDialog = true
                kotlinx.coroutines.delay(50)  // let the surface settle after the audioReady recomposition, as the layout-change re-focus below does
                try { focusRequester.requestFocus() } catch (e: Exception) { }
            }
        }
    }
    // Safety net for the above: if focus still isn't on the input view, the system would treat B
    // (= BACK) as "minimize". While the recovery prompt is up, intercept BACK and make it NO/discard.
    BackHandler(enabled = showRecoveryDialog) {
        showRecoveryDialog = false
        fileController.clearAutosave()
    }

    // Autosave onStop flush (REVIEW-3 5.3 Phase C). The 3 s debounce can lose the last edits if Android
    // kills the backgrounded app (common on the 1 GB Miyoo) before it fires. On ON_STOP, flush
    // synchronously when dirty: onStop runs on the main thread and the process may be killed right
    // after, so we can't await a coroutine — and main is the project's sole mutator, so a direct
    // serialize+write is tear-free.
    DisposableEffect(Unit) {
        val activity = context as? ComponentActivity
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_STOP && trackerController.isProjectDirty) {
                fileController.saveAutosave(trackerController.project)
            }
        }
        activity?.lifecycle?.addObserver(observer)
        onDispose { activity?.lifecycle?.removeObserver(observer) }
    }

    // When the layout mode changes, Compose destroys the old layout composable and builds
    // a new one. Any virtual button that was being held at that moment has its pointerInput
    // coroutine cancelled without firing the RELEASED callback, leaving modifier flags
    // (isAPressed, isLPressed, etc.) permanently stuck in InputMapper. Reset them here so
    // the first input after a layout switch behaves correctly.
    // Also re-request focus so keyboard/gamepad input works in the new layout composable.
    LaunchedEffect(layoutMode) {
        inputMapper.reset()
        kotlinx.coroutines.delay(50)
        try { focusRequester.requestFocus() } catch (e: Exception) { }
    }

    val isPlaying = stateVersion.let { trackerController.isPlaying() }
    val currentInstrument = stateVersion.let { trackerController.currentInstrument }
    val instrumentCursorRow = stateVersion.let { trackerController.instrumentCursorRow }
    val instrumentCursorColumn = stateVersion.let { trackerController.instrumentCursorColumn }
    val instrumentStatusMessage = stateVersion.let { trackerController.statusMessage }
    val instrumentStatusSuccess = stateVersion.let { trackerController.statusSuccess }
    val poolCursorColumn = stateVersion.let { trackerController.poolCursorColumn }
    val instrumentFromPool = stateVersion.let { trackerController.instrumentFromPool }

    val selectionInfo = stateVersion.let { trackerController.inputController.getSelectionInfo() }
    val clipboardInfo = stateVersion.let { clipboardManager.getClipboardInfo() }
    val selectionModeActive = stateVersion.let { trackerController.inputController.selectionMode }
    // Key the cell-selection lambda on the current selection bounds so its IDENTITY changes when
    // the selection is expanded. Post-6.1 the screen only redraws when a param to PixelPerfectTracker
    // changes; in selection mode the cursor is anchored and the scope label ("SEL:CELL") is unchanged,
    // and isCellSelected was a stable (compiler-memoized) lambda — so a growing selection changed no
    // param, the composable was skipped, and the highlight looked frozen (copy still worked blind).
    // remember(selStart, selEnd) hands down a fresh lambda on each expand → recomposition → redraw.
    val selStart = stateVersion.let { trackerController.inputController.selectionStart }
    val selEnd = stateVersion.let { trackerController.inputController.selectionEnd }
    val isCellSelectedFn: (Int, Int) -> Boolean = remember(selStart, selEnd) {
        { row, col -> trackerController.inputController.isCellSelected(row, col) }
    }

    val trackerParams = TrackerScreenParams(
        currentScreen = currentScreen,
        project = project,
        audioEngine = audioEngine,
        playbackController = playbackController,
        cursorRow = cursorRow,
        cursorColumn = cursorColumn,
        isPlaying = isPlaying,
        previousColumn = previousColumn,
        currentChain = currentChain,
        currentPhrase = currentPhrase,
        projectCursorRow = projectCursorRow,
        projectCursorColumn = projectCursorColumn,
        projectStatusMessage = projectStatusMessage,
        projectStatusSuccess = projectStatusSuccess,
        projectVersion = projectVersion,
        currentInstrument = currentInstrument,
        instrumentCursorRow = instrumentCursorRow,
        instrumentCursorColumn = instrumentCursorColumn,
        instrumentStatusMessage = instrumentStatusMessage,
        instrumentStatusSuccess = instrumentStatusSuccess,
        poolCursorColumn = poolCursorColumn,
        instrumentFromPool = instrumentFromPool,
        fileBrowserState = fileBrowserState,
        sampleEditorState = sampleEditorState,
        selectionInfo = selectionInfo,
        clipboardInfo = clipboardInfo,
        selectionMode = selectionModeActive,
        isCellSelected = isCellSelectedFn,
        mixerCursorColumn = trackerController.mixerCursorColumn,
        mixerMasterRow = trackerController.mixerMasterRow,
        trackPeaks = trackPeakBuffer,
        masterPeaks = masterPeakBuffer,
        sendPeaks = sendPeakBuffer,
        currentTable = trackerController.currentTable,
        tableCursorRow = trackerController.tableCursorRow,
        tableCursorColumn = trackerController.tableCursorColumn,
        currentGroove = trackerController.currentGroove,
        grooveCursorRow = trackerController.grooveCursorRow,
        modCursorRow = trackerController.modCursorRow,
        modCursorPair = trackerController.modCursorPair,
        modCursorSide = trackerController.modCursorSide,
        effectsCursorRow = trackerController.effectsCursorRow,
        isRendering = isRendering,
        renderProgress = renderProgress,
        sampleRamBytes = sampleRamBytes,
        showCleanDialog = showCleanDialog,
        cleanDialogTarget = cleanDialogTarget,
        cleanDialogCursor = cleanDialogCursor,
        showNewProjectDialog = showNewProjectDialog,
        showInstrTypeDialog = showInstrTypeDialog,
        showRecoveryDialog = showRecoveryDialog,
        songScrollPosition = stateVersion.let { trackerController.songScrollPosition },
        scalingMode = scalingMode,
        buttonSoundEnabled = buttonSoundEnabled,
        buttonSoundVolume = buttonSoundVolume,
        buttonVibroEnabled = buttonVibroEnabled,
        vibroPower = vibroPower,
        qwertyKeyboardState = qwertyKeyboardState.copy(insertBefore = insertBefore),
        fxHelperState = fxHelperState,
        eqEditorState = eqEditorState,
        eqSpectrumData = eqSpectrumData,
        themeEditorState = themeEditorState,
        settingsCursorRow = stateVersion.let { trackerController.settingsCursorRow },
        settingsCursorColumn = stateVersion.let { trackerController.settingsCursorColumn },
        cursorRemember = cursorRemember,
        notePreviewEnabled = notePreviewEnabled,
        autosaveResumeAuto = autosaveResumeAuto,
        soundfontPresetName = stateVersion.let {
            val inst = project.instruments[currentInstrument]
            val path = inst.soundfontPath
            if (path != null && inst.instrumentType == InstrumentType.SOUNDFONT) {
                val slot = instrumentController.sfSlotMap[path]
                if (slot != null) audioEngine.backend.getSoundfontPresetName(
                    slot,
                    inst.sfBank,
                    inst.sfPreset
                )
                else "---"
            } else "---"
        },
        soundfontPresetCount = stateVersion.let {
            instrumentController.getSoundfontPresetCount(project.instruments[currentInstrument])
        },
        soundfontPresetIndex = stateVersion.let {
            instrumentController.getSoundfontCurrentPresetIndex(project.instruments[currentInstrument])
        },
        overlayBitmap = overlayBitmap,
        overlayStrength = overlayStrength,
        overlayFiles = overlayFiles,
        overlayName = overlayName
    )

    // The Oboe stream opens on a background IO thread; the UI renders immediately rather than behind a
    // loading screen. That first open can take many seconds on some devices (e.g. GammaCoreOS / Miyoo
    // Flip, where AAudio's open triggers C2 codec enumeration), which is exactly why we don't block on
    // it. Audio-dependent effects key off audioReady, the visualizer poll no-ops until
    // audioEngine.isReady, and START/playback is gated the same way — so nothing touches the engine
    // before it exists.

    val hapticView = LocalView.current
    CompositionLocalProvider(
        LocalLayoutMode provides layoutMode,
        LocalAppTheme provides appTheme,
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
            FullScreenLayout(
                layoutConfig = effectiveLayoutConfig,
                scalingMode = scalingMode,
                params = trackerParams,
                inputMapper = inputMapper,
                focusRequester = focusRequester
            )
        } else when (layoutMode) {
            DeviceAdapter.LayoutMode.TOUCH_LANDSCAPE ->
                LandscapeLayoutWithVirtualButtons(
                    layoutConfig = effectiveLayoutConfig,
                    scalingMode = scalingMode,
                    params = trackerParams,
                    inputMapper = inputMapper,
                    focusRequester = focusRequester
                )
            DeviceAdapter.LayoutMode.TOUCH_PORTRAIT2 ->
                PortraitLayout2WithVirtualButtons(
                    layoutConfig = effectiveLayoutConfig,
                    scalingMode = scalingMode,
                    params = trackerParams,
                    inputMapper = inputMapper,
                    focusRequester = focusRequester,
                    theme = theme,
                )
            else -> // TOUCH_PORTRAIT (and FULL fallback — shouldn't normally reach here)
                PortraitLayoutWithVirtualButtons(
                    layoutConfig = effectiveLayoutConfig,
                    scalingMode = scalingMode,
                    params = trackerParams,
                    inputMapper = inputMapper,
                    focusRequester = focusRequester
                )
        }
    } // CompositionLocalProvider
}

