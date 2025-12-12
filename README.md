# PocketTracker

Android music tracker application inspired by M8 and LSDJ.

## Project Structure
- **TrackerData.kt**: Core data structures (Note, Phrase, Chain, etc.)
- **TrackerAudioEngine.kt**: JNI bridge to C++ audio engine
- **native-audio.cpp**: C++ audio engine with voice management
- **PhraseEditorModule.kt**: Main phrase editing screen
- **PixelPerfectRenderer.kt**: Custom Canvas-based rendering

## Current Development Phase
Phase A: UI completion without physical hardware
- Implementing mouse navigation
- Building file management system
- Connecting data flow between screens

## Build Instructions
1. Open in Android Studio
2. Sync Gradle
3. Run on emulator or device

## Target Devices
- Android gaming handhelds (640×480 resolution)
- Budget Android devices
- Minimum Android 8.0 (API 26)