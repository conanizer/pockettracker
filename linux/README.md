# The SDL shell — `pockettracker-sdl`

**Linux-port plan Phase 2.** The first build of PocketTracker with no Kotlin in it: it opens an audio
device, hands songcore a `.ptp`, and plays it.

## Why this is small

Almost nothing here is new code, and that is the point. Everything that decides how a song *sounds* —
the sequencer, effect resolution, voices, modulation, the whole DSP chain, the offline render — is
the same C++ the Android APK ships. This directory links it (`add_subdirectory(../native)`) and
reaches it through exactly one class:

```
songcore::SongcoreHost          native/songcore/host.h
```

which is the same class `songcore-jni.cpp` marshals for Android. There is no second implementation of
anything, and no seam where the two platforms can drift.

The call sequence was proven before this shell existed — `tools/ptrender` already does

```
push_project(.ptp)  →  load_media(samples + SoundFonts)  →  render_song_to_wav()
```

with zero app code, on gcc/x86-64, MSVC/x86-64 and clang/arm64 in CI. This shell is the same
sequence with a **live audio device** where ptrender has a render loop. So what is genuinely new is
only the shell: a device, a window, a clock.

| | Android | Linux/SDL |
|---|---|---|
| Audio backend | `native/oboe-audio-engine.cpp` | `linux/sdl-audio-engine.cpp` |
| Entry point | `native/songcore-jni.cpp` (JNI) | `linux/main.cpp` |
| Engine, songcore, DSP | **shared — the same files** | **shared — the same files** |

Both backends do the same one thing in their audio callback: hand the device buffer to
`AudioEngine::processLiveBlock()`. **No DSP may ever be added to either.**

## What is *not* here

**The UI.** The ~20 screen modules, input mapping, the file browser and the 5×5 bitmap font are
**Phase 3** of the port plan. What `main.cpp` draws is a deliberate smoke-test readout — eight track
blocks lit by the notes actually sounding, a phrase-step strip, a song-row bar — with no font and no
editing. It answers "is it actually playing?" at a glance and nothing else should be built on it; it
is meant to be deleted the day the real UI lands.

## Build

SDL2 is taken **from the system if it is there**, and fetched only if it isn't. On a handheld that
is a requirement rather than a preference: the PortMaster CFWs (muOS, ROCKNIX, ArkOS, Knulli,
AmberELEC…) ship their own `libSDL2`, and a port links the one the OS provides. The FetchContent
fallback exists so a Windows dev box needs no setup at all.

```sh
cmake -S linux -B linux/build -DCMAKE_BUILD_TYPE=Release
cmake --build linux/build --config Release
```

Pass `CMAKE_BUILD_TYPE` explicitly. The usual "default to Release if it's empty" guard is not
portable — MSVC's platform module pre-seeds it to `Debug` while GCC/Clang leave it unset — and here,
unlike the conformance tools, a Debug engine may not keep up with a real-time audio callback.

**On this Windows box specifically**, CMake 3.22 (the one in the Android SDK) predates the installed
Visual Studio, so it cannot generate for it. Use Ninja, from a `vcvars64` shell:

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set CMBIN=%LOCALAPPDATA%\Android\Sdk\cmake\3.22.1\bin
%CMBIN%\cmake -S linux -B linux\build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM=%CMBIN%\ninja.exe
%CMBIN%\cmake --build linux\build
```

Linux needs SDL2's dev package (`apt install libsdl2-dev`) to use the system one, or it will fetch.

## Run

```sh
linux/build/pockettracker-sdl testdata/g7-audio.ptp testdata
```

`g7-audio` is the golden project built for the DSP rather than the sequencer — both send buses, the
master bus (OTT, limiter, master EQ), a per-instrument EQ, drive, a resonant filter under an LFO, a
SoundFont voice and a resampled stereo pad — so if it sounds right, most of the engine is right.

```
pockettracker-sdl <project.ptp> [media-base-dir]

  SPACE   play / stop
  ESC     quit
```

`media-base-dir` defaults to the project's own directory. Portable projects (the `/testdata`
goldens, anything the Linux build ships) store sample paths **relative** to the project file; a
project saved on a device stores absolute paths, and both resolve correctly
(`engine_setup.h: resolve_media_path`).

## Two things that will bite you

- **`AudioEngine` must be heap-allocated.** Its per-block DSP scratch, spectrum rings and 256-slot
  table pool are members and blow a 1 MB stack instantly (`0xC00000FD`). `main.cpp` uses
  `make_unique`; so does ptrender, for the same reason.
- **This target is compiled `-fno-fast-math -ffp-contract=off`** (`/fp:precise` on MSVC), and that is
  a *correctness* requirement, not an optimisation choice. `main.cpp` includes `songcore/host.h`, so
  the **sequencer** is compiled into this target — and aarch64 clang contracts `a + b*c` into an fma
  **by default**, which rounds once where the JVM rounds twice. Without those flags the handheld
  build would quietly sequence differently from the APK the conformance goldens were recorded
  against, on the exact architecture we ship to. `tools/CMakeLists.txt` states the same rule for the
  same reason; see event-schema §5/§6.

## Next

Phase 3 — port the ~20 screen modules and the input layer to C++ against this shell. Phase 4 — the
handheld bring-up. Phase 5 — PortMaster packaging.
