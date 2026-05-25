# PocketTracker Development Setup Guide

This guide explains the development environment setup for working on PocketTracker with Claude Code.

## ✅ What's Been Set Up

### 1. **JAVA_HOME Configuration**
- **File**: `~/.bashrc`
- **What it does**: Automatically sets JAVA_HOME for Gradle builds
- **No more**: Manually setting `export JAVA_HOME="..."` before every build
- **Verify**: Run `echo $JAVA_HOME` in Git Bash - should show Android Studio JBR path

### 2. **Git Pre-Commit Hook**
- **File**: `.git/hooks/pre-commit`
- **What it does**: Automatically compiles code before allowing commits
- **Prevents**: Committing broken code (would have prevented today's issue!)
- **Test**: Try `git commit` with a syntax error - it will block the commit

### 3. **Helper Build Scripts**
Quick commands for common tasks:

```bash
./build.sh   # Full debug build → generates APK
./test.sh    # Compile check only (fast verification)
./clean.sh   # Clean build artifacts
```

All scripts automatically set JAVA_HOME, so you can just run them directly.

### 4. **Improved .gitignore**
- Added `nul` (Windows temp file)
- Added `*.tmp` files
- Added `.vscode/` directory
- Ensured build scripts are tracked (not ignored)

### 5. **Claude Code Settings**
- **File**: `.claude/settings.json`
- **Contains**:
  - Ignore patterns (don't read build/, .gradle/, etc.)
  - Development reminders (read SESSION_HISTORY.md, test before commit, etc.)
  - Quick command references

### 6. **Gradle Performance Optimizations**
- **File**: `gradle.properties`
- **Enabled**:
  - Parallel builds (`org.gradle.parallel=true`)
  - Build cache (`org.gradle.caching=true`)
  - Gradle daemon (`org.gradle.daemon=true`)
  - Configuration on demand (`org.gradle.configureondemand=true`)
- **Result**: Faster incremental builds

### 7. **Session History Tracking**
- **File**: `SESSION_HISTORY.md`
- **Purpose**: Maintain context across Claude Code sessions
- **Contains**: What was tried, what worked, what didn't, lessons learned
- **Update**: Add new entries after each significant development session

## 🚀 Quick Start Workflow

### First Time Setup (One-time)
1. ✅ JAVA_HOME is set in ~/.bashrc (already done)
2. ✅ Pre-commit hook is installed (already done)
3. ✅ Build scripts are ready (already done)

### Daily Development Workflow

```bash
# 1. Start work - check current state
git status

# 2. Make changes in Android Studio
# ... edit code ...

# 3. Quick compile check
./test.sh

# 4. Test in app
# - Launch app
# - Move cursor (D-pad)
# - Test A/B buttons
# - Navigate screens (R+D-pad)

# 5. Commit (pre-commit hook runs automatically)
git add .
git commit -m "Your message"

# 6. Push to GitHub
git push
```

## 🔧 Common Tasks

### Build Full APK
```bash
./build.sh
# Output: app/build/outputs/apk/debug/pockettracker-debug.apk
```

### Quick Compile Check (Fastest)
```bash
./test.sh
```

### Clean Build (When Things Get Weird)
```bash
./clean.sh
./build.sh
```

### Manual Gradle Commands
```bash
# Compile Kotlin only (fast check)
./gradlew compileDebugKotlin

# Full debug build
./gradlew assembleDebug

# Install to connected device
./gradlew installDebug

# Clean
./gradlew clean
```

## 🛡️ Safety Mechanisms

### 1. Pre-Commit Hook
**What it prevents**: Committing code that doesn't compile

**How to bypass** (not recommended):
```bash
git commit --no-verify -m "Message"
```

### 2. Android Studio Local History
**What it is**: IDE tracks all file changes automatically

**How to use**:
1. Right-click file in Android Studio
2. Local History → Show History
3. Restore any previous version

**When to use**: Saved us today when git rollback didn't work!

### 3. Session History
**What it is**: `SESSION_HISTORY.md` documents what's been tried

**Before starting work**: Read it to understand context

**After major changes**: Add new session entry

## 📋 Development Best Practices

### ✅ Do This
1. **Small incremental changes** - One feature/fix at a time
2. **Test after each change** - Run `./test.sh` frequently
3. **Commit working code** - Pre-commit hook enforces this
4. **Use descriptive commit messages** - Future you will thank you
5. **Read SESSION_HISTORY.md** - Learn from past mistakes

### ❌ Don't Do This
1. **Big-bang refactoring** - Change too much at once
2. **Skip testing** - "It probably works" → famous last words
3. **Commit broken code** - Pre-commit hook will stop you anyway
4. **Try to fix forward** when majorly broken - rollback first
5. **Ignore Local History** - It's your safety net

## 🎯 When Working with Claude Code

### At Session Start
Claude should:
1. Read `SESSION_HISTORY.md` for context
2. Read `CLAUDE.md` for architecture overview
3. Check `git status` to see current state

### During Development
1. Make small changes
2. Run `./test.sh` after each change
3. Test basic functionality
4. Commit when working

### When Things Break
1. **First**: Rollback to last working state (git or Local History)
2. **Second**: Analyze what went wrong
3. **Third**: Try again with smaller changes
4. **Document**: Add to SESSION_HISTORY.md what was learned

## 🔍 Troubleshooting

### "JAVA_HOME is not set"
```bash
# Check if it's set
echo $JAVA_HOME

# If not, reload bashrc
source ~/.bashrc

# Verify
echo $JAVA_HOME
```

### "Permission denied" on build scripts
```bash
chmod +x build.sh test.sh clean.sh
```

### "Pre-commit hook failed"
- Fix compilation errors shown in output
- Run `./test.sh` to verify fixes
- Try commit again

### Builds are slow
- Check if Gradle daemon is running: `./gradlew --status`
- Try clean build: `./clean.sh && ./build.sh`
- Restart Gradle daemon: `./gradlew --stop` then build again

## 📚 Additional Resources

- **Architecture**: See `CLAUDE.md` for project architecture details
- **Session History**: See `SESSION_HISTORY.md` for development timeline
- **Git History**: `git log --oneline` to see recent changes
- **Claude Settings**: `.claude/settings.json` for tool configuration

---

*Last Updated: 2026-05-18*
*Note: SESSION_HISTORY.md mentioned throughout this document is no longer part of the active workflow. Current session context is maintained via CLAUDE.md and the auto-memory system.*
