# Contributing to PocketTracker

## Current status

PocketTracker is approaching its first public release and is **not yet accepting external pull requests**. Once released:

- **Bug reports** → [GitHub Issues](https://github.com/conanizer/pockettracker/issues)
- **Feature requests** → [GitHub Discussions](https://github.com/conanizer/pockettracker/discussions)
- **Code / docs / example projects** → pull requests welcome

---

## Build environment

| Tool | Version | Install via |
|---|---|---|
| Android Studio | Hedgehog (2023.1.1)+ | [developer.android.com](https://developer.android.com/studio) |
| NDK | 25.1+ | SDK Manager → SDK Tools |
| CMake | 3.22.1+ | SDK Manager → SDK Tools |
| JDK | 17+ | Bundled with Android Studio |

**Minimum target device:** Android 8.0 (API 26), 64-bit, 640×480 screen, ~512 MB RAM.

---

## Building

```bash
git clone https://github.com/conanizer/pockettracker.git
cd pockettracker

./gradlew assembleDebug          # build debug APK
./gradlew installDebug           # build + install to connected device
./gradlew test                   # run unit tests
```

Output APK: `app/build/outputs/apk/debug/`

The project uses CMake for the native C++ audio engine. NDK and CMake must be installed before the first build.

---

## Architecture overview

PocketTracker uses a **portable core + platform adapters** layered architecture:

```
core/           Platform-agnostic Kotlin — NO Android imports allowed here
  audio/        IAudioBackend interface + AudioEngine coordinator
  logic/        TrackerController, PlaybackController, EffectProcessor, …
  storage/      IFileSystem interface + FileInfo
  resources/    IResourceLoader interface

platform/android/   Android-specific implementations
  MainActivity.kt         Thin entry point (~1000 lines)
  AppInputDispatcher.kt   All button handler logic
  OboeAudioBackend.kt     Oboe/JNI implementation of IAudioBackend
  AndroidFileSystem.kt    Scoped storage implementation

app/src/main/cpp/   C++ audio engine (portable, shared with future Linux port)
  audio-engine.cpp    processAudioBlock — all DSP lives here
  effects/            Three-layer DSP module system (primitives / modules / chains)
```

**Critical rule:** `core/` must never import `android.*` or `androidx.*`. Business logic is written once; platform adapters handle the Android specifics.

See [`docs/technical-architecture.md`](docs/technical-architecture.md) for the full architecture.

---

## Code style

### Kotlin

- Classes: `PascalCase` — Functions: `camelCase` — Constants: `SCREAMING_SNAKE_CASE`
- No Android imports in `core/` — use the interfaces (`IAudioBackend`, `IFileSystem`, etc.)
- Compose state: always copy, never mutate (`state.copy(...)`, not `state.field = value`)
- Hex display: use `.toHex2()` / `.toHex1()` from `EditorHelpers.kt`, not inline format strings
- Row background in list editors: use `rowBgColor()` from `EditorHelpers.kt`

### C++

- Classes: `PascalCase` — Functions/members: `camelCase` — Constants: `SCREAMING_SNAKE_CASE`
- All audio DSP goes in `processAudioBlock()` — never add processing to `onAudioReady` or `renderOffline`
- Thread safety: use `std::lock_guard<std::mutex>` for queue access

### Comments

Only comment when the **why** is non-obvious: a hidden constraint, a surprising invariant, a specific bug workaround. Code that describes what it does via well-named identifiers needs no comment.

---

## Commit message format

```
[Category] Brief description (imperative mood)

- What changed and why
- What was tested
```

**Categories:** `[Feature]` `[Fix]` `[Audio]` `[UI]` `[Refactor]` `[Docs]`

---

## Testing

Before submitting a PR:

1. App compiles without errors
2. App runs on a real device or emulator without crashing
3. Changed feature works as intended
4. No regressions in other screens — verify phrase/chain/song playback, save/load

If you can't test on a real device, note that in the PR description.

---

## Linux port

A Linux port is planned post-MVP. The C++ audio engine and all `core/` logic are already platform-agnostic for this reason. If you're working on the Linux port, `platform/linux/` is the right place for new platform-specific code.
