# The SDL shell — `pockettracker-sdl`

**Linux-port plan Phase 2 (the sound) + Phase 3 (the UI).** A build of PocketTracker with no Kotlin in
it: it opens an audio device, hands songcore a `.ptp`, draws the tracker, and plays it.

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

| | Android | Linux/SDL |
|---|---|---|
| Audio backend | `native/oboe-audio-engine.cpp` | `linux/sdl-audio-engine.cpp` |
| Video / window | Compose `Canvas` | `linux/sdl-video.cpp` |
| Input source | `InputMapper` (Android keys) | `linux/sdl-input.cpp` |
| Entry point | `native/songcore-jni.cpp` (JNI) | `linux/main.cpp` |
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
```

`media-base-dir` defaults to the project's own directory. Portable projects (the `/testdata`
goldens, anything the Linux build ships) store sample paths **relative** to the project file; a
project saved on a device stores absolute paths, and both resolve correctly
(`engine_setup.h: resolve_media_path`).

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
