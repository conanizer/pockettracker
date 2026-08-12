# The SDL shell — `pockettracker-sdl`

The shell PocketTracker runs on. It opens an audio device, hands songcore a `.ptp`, draws the tracker,
and plays it — on Windows, Linux, the handhelds and Android alike.

**`app.{h,cpp}` is the shared shell**: the boot sequence, the frame loop, the teardown, identical on
every platform. Beside it sit the two entry points that supply what only the platform knows —
`main.cpp` for desktop and handheld (argv, the signal handler, `SDL_Init`/`SDL_Quit`) and
`android-main.cpp` for Android — both linking the same `app.cpp`.

## Why this directory is small

Almost nothing here is new code, and that is the point.

Everything that decides how a song *sounds* — the sequencer, effect resolution, voices, modulation,
the whole DSP chain, the offline render — is reached through exactly one class:

```
songcore::SongcoreHost          native/songcore/host.h
```

Everything that decides how the app *looks* is shared too: `pt-ui` (`native/ui/`) draws every screen
into a 640×480 framebuffer and contains no SDL, no POSIX and no window.

So the platforms differ in exactly three things, and this directory is where those three live:

| | Android | desktop · handheld |
|---|---|---|
| Entry point | `shell/android-main.cpp` | `shell/main.cpp` |
| Audio backend | `native/oboe-audio-engine.cpp` | `shell/sdl-audio-engine.cpp` |
| Filesystem | `shell/saf-filesystem.cpp` — the app holds no storage permission | `native/ui/std_filesystem.cpp` |
| Video · input · frame loop | *shared* — `sdl-video.cpp`, `sdl-input.cpp`, `app.cpp` | *the same files* |
| **Engine · songcore · UI** | **shared — the same files** | **shared — the same files** |

Both audio backends do the same one thing in their callback: hand the device buffer to
`AudioEngine::processLiveBlock()`. **No DSP may ever be added to either.**

Because `pt-ui` has no display dependency, a screen can be drawn with no window at all: a host program
that implements `Canvas`'s four primitives over a plain pixel buffer gets any screen of any project as
an image, with no SDL and no engine. That is what keeps the seam honest — the day the UI reaches for
SDL, such a program stops linking.

## The UI edits the live project

There is exactly **one** `Project` in the process: the one `SongcoreHost` owns and the `Sequencer`
reads. The UI edits it in place through `host.edit_project()`, so an edit is live the instant it is
made, and there is no second copy to desync.

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
portable — MSVC's platform module pre-seeds it to `Debug` while GCC/Clang leave it unset — and a Debug
engine may not keep up with a real-time audio callback. Offline consumers of the same engine can
afford Debug; this target cannot.

**On a Windows box where CMake predates the installed Visual Studio** it cannot generate for it. Use
Ninja, from a `vcvars64` shell:

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\<ver>\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake -S shell -B shell\build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build shell\build
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

To put it on a device, drop the zip into the `autoinstall` folder inside the device's PortMaster folder
and open PortMaster. That is the path a user takes, and it is the one that guarantees the executable
bit: `PortMaster.sh` hands each zip there to harbourmaster, whose `_install_port` ends in
`chmod -R 777`. The zip does carry `0755` on the launch script and the binary (`unzip -Z` on the
artifact), but a desktop unzip is free to throw that away, and the port then silently does nothing.
`PocketTracker.sh` repairs the binary's bit at launch; nothing can repair its own.

⚠️ **Verify a package by installing it this way, not by unzipping it into `ports/`.** harbourmaster is
not a plain extraction: it stamps a `# PORTMASTER: <zip>, <script>` line into the launch script (so the
installed `.sh` is 50 bytes longer than the built one and will never match it by hash), and it is what
the catalog will do to this zip later. ⚠️ Two things the Flip cannot check for you — its ports live on
a **vfat** mount (`fmask=0022`, so every file reads `rwxr-xr-x` whatever the archive said, and a
permission test there passes by construction), and **spruce replaces PortMaster's own script** with an
older copy that scans only the PortMaster folder's `autoinstall`, never `ports/autoinstall/`.

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
  its default `start = enter` against `sdl-input.cpp`'s `Enter -> Button::A` makes START insert a
  chain and stops playback from stopping. The full table is at the top of `portmaster/`
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
  the C++ tree. `shell/windows/make-icon.ps1` regenerates the `.ico` from
  `docs/images/logo-plain.png`; no build runs it.

## Run

```
pockettracker-sdl [project.ptp] [media-base-dir] [app-root]
```

⚠️ **Every argument is optional, and on the shipping target there are none**: PortMaster launches a
port by running its `.sh`, which invokes this binary bare. With no project the app opens the same
blank document NEW PROJECT makes, and the file browser is how a handheld user reaches their songs.
`app-root` is where `Projects/ Samples/ Soundfonts/ Instruments/` live and defaults to
`$POCKETTRACKER_HOME`, else the platform's own location.

`media-base-dir` defaults to the project's own directory. A **portable** project — anything the Linux
build ships — stores sample paths **relative** to the project file, while a project saved on a device
stores absolute ones; both resolve correctly (`engine_setup.h: resolve_media_path`). The second
argument is the root those relative paths are resolved against, so it is only needed when a project
and its media have been separated.

### Environment

| variable | |
|---|---|
| `POCKETTRACKER_HOME` | the app root (`Projects/`, `Samples/`…). Overrides the platform default on every platform; a PortMaster launch script exports it to point at the SD card. |
| `POCKETTRACKER_LOG` | **`=1` turns the engine's `LOGD` chatter back on.** Off by default. |
| `POCKETTRACKER_AUDIO_PROFILE` | the audio-callback profiler (`sdl-audio-engine.cpp`). |

⚠️ **`POCKETTRACKER_LOG` is off by default**, because off Android there is no logcat and the same 35
`LOGD` call sites go straight to stderr — which fills a shipped desktop console with
`[D/NativeAudio] 🔊 Track 0 volume set to 1.00` on every boot, emoji mojibaked on any non-UTF-8
console. During a bring-up, set the variable — that is what it is for. `LOGE` is **not** gated: an
error is not spam, and the console is only worth keeping if a user can paste it back.

### Controls

| Key | Button | |
|---|---|---|
| `WASD` / arrows | D-PAD | move the cursor |
| `K` / `Enter` | **A** | |
| `J` / `Esc` | **B** | |
| `U` / `I` | L / R | |
| `LShift` | SELECT | |
| `Space` | START | play / stop |
| `F10` | — | quit (**dev only** — not a real button; the handheld's EXIT action lands with the PROJECT screen) |

Every one of these is rebindable through `config.json`'s `keyboard` section; the table above is what
the app seeds into a fresh one.

Editing is the standard tracker gesture set, and it is driven entirely by the cursor's *context*
(`native/ui/cursor.h`) rather than by which screen is up:

| | |
|---|---|
| **A** + `UP`/`DOWN` | step the value under the cursor by one |
| **A** + `LEFT`/`RIGHT` | step it by the large step (16 for a hex byte, an octave for a note) |
| **A** on an empty cell | insert the default (an empty note becomes C-4) |
| **A**+**B** | delete the value — or reset it to its default, for cells that cannot be empty |

A gamepad works too (`SDL_GameController`): D-pad, A/B (X and Y aliased onto them), the shoulders,
BACK = SELECT and START. `config.json`'s `controller` section overrides the face-button layout for a
pad that misreports itself. The L2/R2 triggers and the analog stick are **not** mapped — both are
axes and both vary per CFW.

## Three things that will bite you

- **`AudioEngine` must be heap-allocated.** Its per-block DSP scratch, spectrum rings and 256-slot
  table pool are members and blow a 1 MB stack instantly (`0xC00000FD`). `main.cpp` uses
  `make_unique`.
- **This target is compiled `-fno-fast-math -ffp-contract=off`** (`/fp:precise` on MSVC), and that is
  a *correctness* requirement, not an optimisation choice. `main.cpp` includes `songcore/host.h`, so
  the **sequencer** is compiled into this target — and aarch64 clang contracts `a + b*c` into an fma
  **by default**, which rounds once where a separate multiply and add round twice. Without those
  flags the handheld build would quietly sequence differently from the APK, on the exact architecture
  we ship to. Every target that compiles songcore carries the same two flags for the same reason.
- **`SDL_RENDERER_ACCELERATED` means *require*, not *prefer*.** `SDL_CreateRenderer` FAILS outright
  when no driver offers acceleration — so asking for it unconditionally means the app does not start
  on exactly the devices the port has to run on (TrimUI's GE8300, whose 32-bit GL blobs are missing;
  any CFW booted without a GPU driver). `sdl-video.cpp` therefore tries accelerated+vsync,
  then accelerated, then **anything** — and the software renderer that catches the fall is not a
  degraded mode: blitting one 640×480 texture is trivial on a CPU, which is *why* the UI draws into a
  framebuffer instead of using shaders. Without vsync, `present()` also has to pace the frame itself
  or the loop spins a core flat.
