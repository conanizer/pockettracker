# PocketTracker Development Setup Guide

How to set up and work on PocketTracker. The project builds with **Android Studio + the Gradle
wrapper (`./gradlew`)** — there are no custom build scripts.

**Last Updated:** 2026-06-22

---

## Requirements

- **Android Studio** Hedgehog or newer (bundles a compatible JBR)
- **Android NDK 25.1+** and **CMake 3.22.1+** (native audio engine is compiled via CMake)
- **JDK**: use the JBR bundled with Android Studio (`<Android Studio>/jbr`)

---

## One-time setup

### JAVA_HOME (for command-line Gradle)

Android Studio runs Gradle with its bundled JBR automatically. For command-line builds in Git Bash,
point `JAVA_HOME` at the same JBR (add to `~/.bashrc` so it persists):

```bash
export JAVA_HOME="/c/Program Files/Android/Android Studio/jbr"
```

Verify:

```bash
echo "$JAVA_HOME"        # should print the JBR path
./gradlew --version      # should run without "JAVA_HOME is not set"
```

### Pre-commit hook (already in the repo)

`.git/hooks/pre-commit` blocks a commit if staged Kotlin changes don't compile. It:

1. Sets `JAVA_HOME` to the Android Studio JBR if it isn't already set.
2. If any staged file matches `*.kt`, runs `./gradlew compileDebugKotlin --quiet`.
3. Aborts the commit (exit 1) if compilation fails.

It only runs the Kotlin compile check — it does **not** build the APK or compile the native C++.

Bypass (not recommended): `git commit --no-verify`.

---

## Build & run

All commands use the Gradle wrapper from the project root.

```bash
./gradlew compileDebugKotlin   # fast Kotlin-only compile check (what the pre-commit hook runs)
./gradlew assembleDebug        # full debug build → APK
./gradlew installDebug         # build + install to a connected device/emulator
./gradlew test                 # JVM unit tests
./gradlew clean                # remove build artifacts
./gradlew assembleRelease      # release APK (R8 minify + resource shrink, debug-signed for sideload)
```

Debug APK output: `app/build/outputs/apk/debug/`.

> **Note:** the native audio engine is compiled by CMake as part of `assembleDebug` (not by
> `compileDebugKotlin`). A green pre-commit hook means Kotlin compiles — it does not guarantee the
> native build or runtime behavior. Always test on a real device before committing (see below).

---

## Daily workflow

```bash
git status                     # 1. check current state
# 2. edit code in Android Studio
./gradlew compileDebugKotlin   # 3. quick compile check
./gradlew installDebug         # 4. build + push to device, then test by hand
git add .                      # 5. stage
git commit -m "..."            #    (pre-commit hook runs the Kotlin compile check)
git push                       # 6. push
```

**Manual test pass after a change:** launch the app, move the cursor (D-pad), test A/B, navigate
screens (R + D-pad), and play something. Per `CLAUDE.md`, code is verified on a real device
(Miyoo Flip / Ayaneo) before committing — compilation success ≠ working code.

---

## Gradle performance (already configured)

`gradle.properties` enables faster incremental builds:

- `org.gradle.parallel=true`
- `org.gradle.caching=true`
- `org.gradle.daemon=true`
- `org.gradle.configureondemand=true`
- `org.gradle.jvmargs=-Xmx2048m`

---

## Safety nets

1. **Pre-commit hook** — stops you committing Kotlin that doesn't compile (see above).
2. **Android Studio Local History** — the IDE records every file change. Right-click a file →
   *Local History → Show History* to restore an earlier version, even when `git` can't help.
3. **Git** — `git log --oneline`, `git restore`, branch before risky work.

---

## Troubleshooting

**"JAVA_HOME is not set"**
```bash
echo "$JAVA_HOME"      # check
source ~/.bashrc       # reload if you just added the export
```

**Pre-commit hook failed**
- Run `./gradlew compileDebugKotlin` to see the errors, fix them, commit again.

**Builds are slow / acting weird**
```bash
./gradlew --status     # is the daemon alive?
./gradlew --stop       # restart the daemon
./gradlew clean assembleDebug
```

**Native (C++) changes not taking effect**
- Confirm NDK + CMake are installed (SDK Manager). A clean build forces a CMake reconfigure:
  `./gradlew clean assembleDebug`.

---

## When working with Claude Code

- **Do not run builds automatically** — per `CLAUDE.md`, the developer builds and tests manually.
  Claude writes the code; the developer verifies on-device.
- **Context lives in** `CLAUDE.md` (architecture + rules), `docs/development-status.md` (what's done /
  remaining), and the auto-memory system — not in any session-log file.
- `.claude/settings.json` holds tool configuration (ignore patterns, permissions).

---

## Additional resources

- **Architecture:** `CLAUDE.md`, `docs/technical-architecture.md`
- **Status & known issues:** `docs/development-status.md`
- **Controls:** `docs/input-system.md`
- **Git history:** `git log --oneline`
