# PocketTracker Development Session History

This file tracks development sessions with Claude Code to maintain context across conversations.

## Purpose
- Document what was attempted in each session
- Record lessons learned and successful patterns
- Track current state and next steps
- Help Claude understand context when resuming work

---

## Session 2024-12-22: State Refactoring Attempt & Rollback

### Context
- Continued from previous session that ran out of context
- Previous session had created a refactoring plan (see `.claude/plans/unified-napping-gosling.md`)
- Goal: Refactor MainActivity.kt state management (1,852 lines → ~800 lines)

### What Was Attempted
1. **State Consolidation**: Convert 26+ scattered state variables to unified `AppState` data class
2. **Immutable Updates**: Replace direct mutation + `projectVersion++` with immutable `.copy()` pattern
3. **NavigationController**: Extract 175 lines of navigation logic into separate class

### What Happened
1. Started refactoring by migrating handlers to use `appState`
2. **Breakage Discovered**: Most button combos stopped working
   - A+DPad combos not working
   - B+DPad editing wrong cell (row 0, col 0 regardless of cursor position)
   - File browser combos broken
3. **Root Causes Identified**:
   - Mixed state: `appState` and old variables independent, not synced
   - ButtonHandlers wrapped in `remember()` captured stale initial state
   - Layout functions still reading old variables
4. **Attempted Fixes**: Added sync code, removed `remember` wrapper → **More breakage**

### The Rollback
1. Decided to rollback to working state before refactoring
2. **Git Rollback Issues**:
   - `git reset --hard 532b0a2` → **Compilation errors** (commit was broken)
   - `git reset --hard 1ac1edc` → Too far back, missing file browser
3. **Discovery**: Commit 532b0a2 had never compiled - was committed incomplete
4. **Solution**: User restored working version from **Android Studio Local History** ✅
5. Pushed restored version to GitHub (commit `22ffee0`)

### Key Lessons Learned

#### ❌ What NOT to Do
- **Big-bang refactoring** - Don't change core architecture all at once
- **Mixed patterns** - Don't have old and new state systems coexisting without clear migration path
- **Commit broken code** - Even WIP commits should compile
- **Fix forward when fundamentally broken** - Rollback first, analyze second

#### ✅ What WORKS
- **Android Studio Local History** - Excellent safety net for recent changes
- **Small incremental changes** - Test after each logical step
- **Compile-Test-Commit cycle** - Verify app works before committing
- **Git tags for known-good states** - Tag commits verified to work

#### ✅ Better Approaches for Future Refactoring

**1. Incremental Migration Pattern**
```
Phase 1: Add new system alongside old (read-only observation)
Phase 2: Migrate ONE screen to new system
Phase 3: Test thoroughly (all buttons, navigation, editing)
Phase 4: Repeat for other screens
Phase 5: Remove old system when migration complete
```

**2. Feature Flag Pattern**
```kotlin
const val USE_NEW_STATE = false

if (USE_NEW_STATE) {
    // New architecture
} else {
    // Old architecture (working)
}
```

**3. Compile-Test-Commit Workflow**
```
1. Make change
2. ./gradlew assembleDebug (must succeed)
3. Smoke test: Launch app, move cursor, press A/B, navigate screens
4. Git commit if all working
5. Git tag for verified working state
```

### Current State (End of Session)
- ✅ Working version restored with complete file browser functionality
- ✅ Commit: `22ffee0` on branch `claude/keyboard-input-layout-01KisvUqQtDHG9cSjHA353c8`
- ✅ All button combos working correctly
- ⏸️ State refactoring plan shelved - current architecture works fine
- 📁 File browser fully functional with navigation, rename, delete modes

### Files Modified Today (Restored Version)
- CursorContext.kt - Added `browserLine()` and `character()` methods
- FileBrowserModule.kt - Complete file browser implementation
- FileManager.kt - File operations
- InputHandler.kt - Generic input handling
- NavigationMapModule.kt - Added FILE_BROWSER cases
- ScreenLayouts.kt - Added fileBrowserState parameter
- ScreenType.kt - Added FILE_BROWSER enum
- SongEditorModule.kt - Updates
- TrackerAudioEngine.kt - Updates

### Next Steps (Recommendations)
1. **Don't refactor state management yet** - Current system works, not worth the risk
2. **Focus on features** - Add new screens/functionality using existing patterns
3. **If refactoring needed later**:
   - Use incremental migration pattern
   - Test after EVERY small change
   - Keep old system working throughout migration
   - Use feature flags to toggle between old/new

### Notes for Future Claude Sessions
- Read this file at session start to understand what's been tried
- The current state management (scattered vars + `projectVersion++`) works fine despite being "antipattern"
- **CursorContext system** (CursorContext.kt, GenericInputHandler) is excellent - DON'T change it
- **ButtonHandlers data class** works well - DON'T wrap in `remember()`
- Android Studio Local History is better than git for recent recovery
- User prefers working code over "clean" architecture

---

## Session Template for Future Entries

```markdown
## Session YYYY-MM-DD: [Brief Description]

### Context
- What was the starting state?
- What problem were we trying to solve?

### What Was Attempted
- List of changes/features attempted

### What Happened
- Results (success/failure)
- Issues encountered

### Lessons Learned
- What worked well
- What didn't work
- Better approaches discovered

### Current State (End of Session)
- Working/broken?
- Latest commit/branch
- Files modified

### Next Steps
- Recommendations for next session
```

---

## Quick Reference: Current Architecture (Don't Change These!)

### ✅ Keep As-Is (Working Well)
- **CursorContext system** - Generic input handling based on data type
- **GenericInputHandler** - Converts cursor context to input actions
- **ButtonHandlers data class** - Contains all button press lambdas
- **TrackerModule interface** - Screen module system
- **State variables + projectVersion** - Yes it's an antipattern, but it WORKS

### 📋 Current State Management Pattern
```kotlin
// In MainActivity.kt:
var project by remember { mutableStateOf(Project()) }
var cursorRow by remember { mutableIntStateOf(0) }
var projectVersion by remember { mutableIntStateOf(0) }
// ... 20+ more variables

// When data changes:
project.phrases[i].steps[j].note = newNote
projectVersion++  // Force recomposition
```

**Why it works**: Predictable, simple, every screen uses same pattern

**Why not refactor**: High risk, low reward - would need to change ALL screens simultaneously

### 🎯 Development Best Practices (Learned from Experience)

1. **Test Frequently**
   - After any change, run `./gradlew assembleDebug`
   - Basic smoke test: Launch → Move cursor → Press A/B → Navigate screens

2. **Commit Working Code**
   - Only commit code that compiles
   - Add descriptive commit messages
   - Tag verified working states: `git tag working-YYYY-MM-DD`

3. **Use Local History**
   - Android Studio → Right-click file → Local History → Show History
   - Better than git for recovering recent working states

4. **Incremental Changes**
   - One logical change at a time
   - Test before moving to next change
   - Don't batch unrelated changes

5. **When Things Break**
   - Don't try to "fix forward" complex breakage
   - Rollback to last working state
   - Analyze what went wrong
   - Try again with smaller changes

---

*Last Updated: 2024-12-22*
