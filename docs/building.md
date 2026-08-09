# Building PocketTracker from source

The engine, the sequencer and the whole UI are portable C++17 under `native/`. Each platform adds a
thin shell around them — Android through Gradle and the NDK, everything else through CMake in
`shell/`. There is no platform-specific engine and no platform-specific UI; the same code produces the
same 640×480 design and the same sound everywhere.

The version number lives in exactly one place: `versionName` in `app/build.gradle.kts`.
`shell/CMakeLists.txt` parses it out of that file, so the desktop and handheld builds cannot drift
from the Android one.

---

## Android

A standard Android + NDK project — the native side builds via CMake as part of the normal Gradle
build.

1. Clone the repo and open it in a recent **Android Studio**.
2. Let Gradle sync; it provisions the SDK and the pinned **NDK `27.0.12077973`**.
3. Run the **app** configuration on a device or emulator, or build an APK with `./gradlew assembleDebug`.

Debug builds need no signing setup — release builds fall back to the debug key when
`keystore.properties` is absent.

---

## Linux / Windows desktop

Needs CMake and a C++17 compiler. SDL2 is used from the system if present, and fetched and built if
not.

```bash
cmake -S shell -B shell/build -DCMAKE_BUILD_TYPE=Release
cmake --build shell/build
```

The redistributable packages are built by `shell/build-linux.sh` and `shell/build-windows.ps1`. Those
scripts do more than archive the binary: they verify the artifact and read the finished archive back
out, so a package cannot ship missing a file that no compiler would have complained about.

---

## PortMaster (aarch64 handheld)

Cross-compiled inside a container. The container image is the glibc floor every Tier-1 CFW is at or
above — a host cross compiler produces a binary no handheld can load.

```bash
docker build -t pockettracker-build -f shell/Dockerfile.portmaster shell/
docker run --rm -v "$PWD:/src" pockettracker-build bash /src/shell/build-portmaster.sh
```

---

## Tests

The engine, sequencer, input layer and renderer are covered by a suite of headless tools that compare
them against recorded goldens and hand-written invariants. That suite is developed separately and is
not part of this repository, so `git clone` gives you the application and everything needed to build
it, and no test target.

What CI runs here is the build itself, on every platform: the Android APK (debug and a minified
release, whose DEX is checked for the JNI keep rules), and the SDL shell on both Linux and Windows —
which covers both branches of `shell/CMakeLists.txt`'s SDL2 lookup, the system library and the
built-from-source one.
