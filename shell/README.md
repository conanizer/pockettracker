# The SDL shell — `pockettracker-sdl`

**Linux-port plan Phase 2 (the sound) + Phase 3 (the UI).** A build of PocketTracker with no Kotlin in
it: it opens an audio device, hands songcore a `.ptp`, draws the tracker, and plays it.

> ⚠️ **This directory was `linux/` until convergence C0.3.** It was named for the platform that made
> it necessary, and it stopped being true in the same phase that made it matter: `platform_caps.h`
> had already argued the point for its own reasons (its profile is `sdl()`, not `linux()`), and
> convergence Phase C gives Android this same shell. Windows has shipped out of here since A3.
>
> Since C0.2 the split inside the directory says the same thing: **`app.{h,cpp}` is the shared shell**
> — the boot sequence, the frame loop, the teardown, identical on every platform — and **`main.cpp` is
> the desktop/handheld residue** beside it (argv, the signal handler, `SDL_Init`/`SDL_Quit`, and the
> choice of audio backend, root and caps). C1's Android entry point is a second file next to
> `main.cpp` linking the same `app.cpp`.

## Why this directory is small

Almost nothing here is new code, and that is the point.

Everything that decides how a song *sounds* — the sequencer, effect resolution, voices, modulation,
the whole DSP chain, the offline render — is the same C++ the Android APK ships, reached through
exactly one class:

```
songcore::SongcoreHost          native/songcore/host.h
```

which is the same class `songcore-jni.cpp` marshals for Android. Since Phase 3, everything that
decides how the app *looks* is shared too: `pt-ui` (`native/ui/`) draws every screen into a 640×480
framebuffer and contains no SDL, no POSIX and no window.

So what is left in here is only, and exactly, the shell:

| | Android | shell/SDL |
|---|---|---|
| Audio backend | `native/oboe-audio-engine.cpp` | `shell/sdl-audio-engine.cpp` (both are an `AudioBackend`, C0.2) |
| Video / window | Compose `Canvas` | `shell/sdl-video.cpp` |
| Input source | `InputMapper` (Android keys) | `shell/sdl-input.cpp` — the physical half only |
| Combo matrix | `InputMapper.handleButtonAction` | `native/ui/button_mapper.h` — **shared** since C0.1 |
| Entry point | `native/songcore-jni.cpp` (JNI) | `shell/main.cpp` |
| Boot · frame loop · exit | `PixelPerfectRenderer` | `shell/app.cpp` — **shared** since C0.2 |
| **Engine · songcore · UI** | **shared — the same files** | **shared — the same files** |

Both audio backends do the same one thing in their callback: hand the device buffer to
`AudioEngine::processLiveBlock()`. **No DSP may ever be added to either.**

Because `pt-ui` has no display dependency, a screen can be drawn with no window at all — which is what
`tools/ptshot` does, and a green ptshot is the standing proof that the seam is real:

```sh
tools/build/ptshot testdata/g1-basics.ptp phrase.png --screen=PHRASE --cursor=3,1 --scale=2
```

## The UI edits the live project

There is exactly **one** `Project` in the process: the one `SongcoreHost` owns and the `Sequencer`
reads. The UI edits it in place through `host.edit_project()`, so an edit is live the instant it is
made, and there is no second copy to desync.

(Android needs a second copy — Compose requires an observable object graph to recompose against — and
pushes the whole thing down to songcore as a JSON blob whenever it changes. There is no Kotlin here,
so there is no reason to pay for that round trip on every keystroke.)

## What is *not* here yet

**Most of the screens.** One is real (PHRASE). The rest draw the "COMING SOON" placeholder that the
Android app itself used while its own screens were being written, and they land session by session:
the other grid editors and the oscilloscope/navigation furniture, then the full input dispatcher
(`AppInputDispatcher` is ~3200 lines of Kotlin — the combos, selection, clipboard and screen
navigation all live there), then instruments, mixer, files and settings.

Also missing, and deliberately so: the POSIX filesystem layer, `settings.json` prefs, `.ptt` theme
loading, SIGTERM autosave and the EXIT action. The app currently takes its project on the command
line and quits with F10 or the window close button.

## Build

SDL2 is taken **from the system if it is there**, and fetched only if it isn't. On a handheld that
is a requirement rather than a preference: the PortMaster CFWs (muOS, ROCKNIX, ArkOS, Knulli,
AmberELEC…) ship their own `libSDL2`, and a port links the one the OS provides. The FetchContent
fallback exists so a Windows dev box needs no setup at all.

```sh
cmake -S shell -B shell/build -DCMAKE_BUILD_TYPE=Release
cmake --build shell/build --config Release
```

Pass `CMAKE_BUILD_TYPE` explicitly. The usual "default to Release if it's empty" guard is not
portable — MSVC's platform module pre-seeds it to `Debug` while GCC/Clang leave it unset — and here,
unlike the conformance tools, a Debug engine may not keep up with a real-time audio callback.

**On this Windows box specifically**, CMake 3.22 (the one in the Android SDK) predates the installed
Visual Studio, so it cannot generate for it. Use Ninja, from a `vcvars64` shell:

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set CMBIN=%LOCALAPPDATA%\Android\Sdk\cmake\3.22.1\bin
%CMBIN%\cmake -S shell -B shell\build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM=%CMBIN%\ninja.exe
%CMBIN%\cmake --build shell\build
```

Linux needs SDL2's dev package (`apt install libsdl2-dev`) to use the system one, or it will fetch.

### Cross-compiling for aarch64 (the PortMaster target)

The handhelds are `aarch64`. Cross-compile from an amd64 Linux (the CI runners, or WSL) with the ARM
toolchain — on Ubuntu, `apt install crossbuild-essential-arm64` — and point CMake at the toolchain
file:

```sh
cmake -S shell -B build/aarch64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=shell/toolchain-aarch64.cmake
cmake --build build/aarch64
file build/aarch64/pockettracker-sdl        # -> ELF 64-bit ... ARM aarch64
```

With no arm64 SDL2 on the host this FetchContent-builds SDL2 static, giving a self-contained binary
proven to boot, decode media, run the frame loop and exit on SIGTERM under `qemu-aarch64` emulation —
a build/CI validation, **not a device artifact**. For one of those, use the packaging build below; it
is the same toolchain file with the two things the header warns about actually done.

### The PortMaster package

```sh
docker build -t pockettracker-build -f shell/Dockerfile.portmaster shell/
docker run --rm -v "$PWD:/src" pockettracker-build bash /src/shell/build-portmaster.sh
# -> build/portmaster/pockettracker.zip
```

Unzip it into the SD card's `ports/` folder, or install it through PortMaster.

**Do not shortcut the container**, even though a modern dev box has a perfectly good
`aarch64-linux-gnu-gcc` and will build this in one command. That build links, produces a valid ARM
ELF, and cannot be loaded by any handheld: glibc symbols are versioned, so a binary demands whatever
versions its host's libc offered, and nothing in a green build says which. `ubuntu:20.04` is glibc
2.31, at or below every Tier-1 CFW. The build script asserts it **on the artifact** rather than
trusting the recipe, and refuses to package a binary that fails.

Two decisions worth knowing before changing anything here:

- **The SDL2 the script builds is a link-time SDK and is never shipped.** The port loads the
  *device's* `libSDL2` — the copy its CFW patched for that hardware's KMSDRM display and ALSA audio.
  Its version is pinned at `release-2.0.18` deliberately: that is the compatibility floor
  (`SDL_GetTicks64` is the one post-2.0.0 symbol the shell needs), and pinning it low means the
  linker *cannot* let a newer SDL call through — a mistake fails here instead of on a stranger's
  device as `undefined symbol`.
- **The launch script must never run `gptokeyb`.** The shell reads the pad itself *and* reads the
  keyboard, so gptokeyb's injected keystrokes arrive as a second, disagreeing copy of every press —
  its default `start = enter` against `sdl-input.cpp`'s `Enter -> Button::A` made START insert a
  chain and stopped playback from stopping. The full table is at the top of `portmaster/`
  `PocketTracker.sh`, and `build-portmaster.sh` fails the build if it reappears.

### The Windows package

```bat
:: build first (see above), then:
powershell -ExecutionPolicy Bypass -File shell\build-windows.ps1
:: -> build\windows\PocketTracker-<version>-windows-x64.zip
```

Unzip anywhere and double-click `PocketTracker.exe`. **Nothing to install** — one self-contained
exe, a README and the licences. CI builds it on every push (`.github/workflows/build.yml`, the
`shell` job's windows-x64 leg).

Unlike `build-portmaster.sh` this script only packages; it does not build, because the compiler
lives behind whichever `vcvars64.bat` the machine happens to have and guessing at that is worse than
failing loudly. It refuses to package an exe older than the sources.

Three decisions worth knowing:

- **SDL2 is linked statically, INTO the exe** — the exact opposite of the PortMaster package, and
  correct for the same reason: a handheld has a CFW-patched `libSDL2` that knows its display, and a
  Windows box has no system SDL2 at all. ⚠️ That makes this zip a *binary distribution of SDL2*, so
  it ships SDL's licence — copied out of the source tree that was actually compiled. The
  `native/vendor/*/`-derived notice guard **cannot see SDL2** (it is fetched, never vendored), which
  is why `build-windows.ps1` checks for it by name.
- **The MSVC runtime is linked in too** (`CMAKE_MSVC_RUNTIME_LIBRARY`). Without it the exe imports
  `VCRUNTIME140.dll` / `MSVCP140.dll`, which every machine that can *build* this has and a stranger's
  machine does not — a failure invisible on any box you would test it on. The package script asserts
  the imports are gone.
- **The icon is a resource, and that is also the window icon.** SDL takes the first `RT_GROUP_ICON`
  out of the exe when no hint is set, so there is no `SDL_SetWindowIcon` call and no PNG decoder in
  the C++ tree — which convergence D2 does not want pulled forward. `shell/windows/make-icon.ps1`
  regenerates the `.ico` from `docs/images/logo-plain.png`; no build runs it.

## Run

```sh
shell/build/pockettracker-sdl testdata/g7-audio.ptp testdata
```

`g7-audio` is the golden project built for the DSP rather than the sequencer — both send buses, the
master bus (OTT, limiter, master EQ), a per-instrument EQ, drive, a resonant filter under an LFO, a
SoundFont voice and a resampled stereo pad — so if it sounds right, most of the engine is right.

```
pockettracker-sdl <project.ptp> [media-base-dir]
```

`media-base-dir` defaults to the project's own directory. Portable projects (the `/testdata`
goldens, anything the Linux build ships) store sample paths **relative** to the project file; a
project saved on a device stores absolute paths, and both resolve correctly
(`engine_setup.h: resolve_media_path`).

### Environment

| variable | |
|---|---|
| `POCKETTRACKER_HOME` | the app root (`Projects/`, `Samples/`…). Overrides the platform default on every platform; a PortMaster launch script exports it to point at the SD card. |
| `POCKETTRACKER_LOG` | **`=1` turns the engine's `LOGD` chatter back on.** Off by default. |
| `POCKETTRACKER_AUDIO_PROFILE` | the audio-callback profiler (`sdl-audio-engine.cpp`). |

⚠️ **`POCKETTRACKER_LOG` is off by default and that is a deliberate reversal (2026-07-20).** On
Android `LOGD` is a debug-priority line nobody sees; off Android there is no logcat, so the same 35
call sites went straight to stderr — meaning the shipped desktop console filled with
`[D/NativeAudio] 🔊 Track 0 volume set to 1.00` on every boot, emoji mojibaked on any non-UTF-8
console. **The PortMaster build had always done this too**; it went unnoticed because a handheld's
stderr goes nowhere anyone looks. During a bring-up, set the variable — that is what it is for.
`LOGE` is **not** gated: an error is not spam, and the console is only worth keeping if a user can
paste it back.

### Controls

The keyboard map is copied key-for-key from the Android one (`InputMapper.keyboardMapping`), so
muscle memory transfers and a bug report about "the K key" means the same thing on both builds.

| Key | Button | |
|---|---|---|
| `WASD` / arrows | D-PAD | move the cursor |
| `K` / `Enter` | **A** | |
| `J` / `Esc` | **B** | |
| `U` / `I` | L / R | |
| `LShift` | SELECT | |
| `Space` | START | play / stop |
| `F10` | — | quit (**dev only** — not a real button; the handheld's EXIT action lands with the PROJECT screen) |

Editing is the standard tracker gesture set, and it is driven entirely by the cursor's *context*
(`native/ui/cursor.h`) rather than by which screen is up:

| | |
|---|---|
| **A** + `UP`/`DOWN` | step the value under the cursor by one |
| **A** + `LEFT`/`RIGHT` | step it by the large step (16 for a hex byte, an octave for a note) |
| **A** on an empty cell | insert the default (an empty note becomes C-4) |
| **A**+**B** | delete the value — or reset it to its default, for cells that cannot be empty |

A gamepad works too (`SDL_GameController`): D-pad, A/B (X and Y aliased onto them), the shoulders,
BACK = SELECT and START. The L2/R2 triggers and the analog stick are **not** mapped yet — both are
axes, both vary per CFW, and neither can be verified without a device (Phase 4 bring-up).

## Three things that will bite you

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
- **`SDL_RENDERER_ACCELERATED` means *require*, not *prefer*.** `SDL_CreateRenderer` FAILS outright
  when no driver offers acceleration — so asking for it unconditionally means the app does not start
  on exactly the devices the port plan warns about (TrimUI's GE8300, whose 32-bit GL blobs are
  missing; any CFW booted without a GPU driver). `sdl-video.cpp` therefore tries accelerated+vsync,
  then accelerated, then **anything** — and the software renderer that catches the fall is not a
  degraded mode: blitting one 640×480 texture is trivial on a CPU, which is *why* the UI draws into a
  framebuffer instead of using shaders. Without vsync, `present()` also has to pace the frame itself
  or the loop spins a core flat.

## Next

Phase 3 continues — the remaining screen modules, the input dispatcher, the filesystem/prefs layer.
Phase 4 — the handheld bring-up. Phase 5 — PortMaster packaging.
