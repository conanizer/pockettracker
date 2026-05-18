# Refactoring Audit Algorithm for Claude Code

A step-by-step process for auditing and refactoring a codebase.
Follow this in order. Do not skip phases. Do not write code until Phase 5.

---

## Phase 1 — Understand the Intended Architecture

Before reading any code, read the architecture documentation.

**Read:**
- Architecture docs (`TECHNICAL_ARCHITECTURE.md`, `CLAUDE.md`, any PLAN_* files)
- Any existing cleanup or refactoring plans

**Answer these questions. Write the answers down:**

1. What are the layers? (e.g., core logic, platform, UI)
2. What is the rule for each layer? What is allowed and what is forbidden in each?
3. What is the intended data flow for the most important operations? (e.g., "user presses button → input handler → controller → audio backend")
4. What are the portability constraints? (e.g., "core must work on Linux too")
5. What interfaces exist to abstract platform-specific behavior?

If you cannot answer these from the docs, ask before proceeding.

---

## Phase 2 — Read the Code

Read every file. No exceptions. Do not form opinions yet — only gather facts.

**For each file, record:**
- Package / folder it lives in
- Its imports (what does it depend on?)
- What it does in one sentence
- Its public interface (what functions/classes does it expose?)

**Start with the largest files.** Large files are the most likely source of mixed responsibilities.

**For each file, also note:**
- Does it contain any `TODO` or `FIXME` comments? What do they say?
- Does the file's location match what it does? (e.g., a file in `core/` that has UI imports)
- Does anything in this file look identical or very similar to something you already read?

Do not stop when you think you understand the codebase. Read everything.

---

## Phase 3 — Map the Actual Architecture

Draw (or write) a dependency map of the actual code.

**For each file:**
- What layer does it actually belong to, based on its imports and behavior?
- What does it depend on?
- What depends on it?

**Find all violations of the intended architecture (from Phase 1):**

1. **Wrong layer placement:** A file lives in one layer but behaves like another.
   - Example: a file in `core/` that imports Android SDK classes
   - Example: a file in a "platform" folder that contains business logic

2. **Wrong direction dependencies:** A lower layer depends on a higher layer.
   - Example: `core/logic/` importing from the root UI package
   - Example: an interface implemented by platform code, but called from UI in a way that bypasses the interface

3. **Bypassed abstractions:** A caller uses a concrete class when an interface exists.
   - Example: calling `ConcreteAudioEngine.specialMethod()` instead of `IAudioBackend.method()`
   - This means the abstraction is incomplete — the caller needs something the interface doesn't provide

4. **Missing abstraction boundary:** Business logic lives inside a component that shouldn't own it.
   - Example: note-to-frequency conversion inside a hardware wrapper class
   - Example: file format parsing inside a UI screen
   - Rule: if the logic would need to be duplicated when porting to another platform, it's in the wrong place

---

## Phase 4 — Find Duplication and Divergence

**Look for the same concept implemented more than once:**

1. **Duplicate constants:** Search for the same value defined in multiple files.
   Any constant with a comment like "must match X" is a guaranteed maintenance problem.

2. **Parallel data structures:** Two classes or data classes with the same or similar fields.
   If they represent the same concept, there should be one.

3. **Parallel logic implementations:** Two functions that do the same thing for different contexts.
   The most dangerous case: one is complete and one is incomplete.
   - How to find: look for similar loop structures, similar `when` dispatch blocks, similar parameter lists
   - Pay special attention to: any place that handles the same input types (notes, effects, steps) in a different file

4. **Multiple code paths for the same operation:** The same feature (e.g., "schedule a note") reached via different function chains in different contexts (e.g., playback vs. export).
   - Trace both paths end-to-end
   - Check if they produce identical output
   - Check if one path is missing features that the other has

**For each duplication found, answer:**
- Are they in sync right now?
- If you add a feature to one, will someone remember to add it to the other?
- If not: this is a divergence time bomb, not just a style problem.

---

## Phase 5 — Find Real Bugs

Separate from architectural problems. These cause observable incorrect behavior now.

**State mutation (most common in Compose/reactive UI):**
- Find all mutable data structures that are observed by the UI
- Check whether they are modified in place (mutation) or replaced with new copies
- In-place mutation on observed state is invisible to the reactivity system

**Feature gaps:**
- For each feature that exists in one code path, verify it exists in all equivalent code paths
- Example: if an effect is handled during live playback, verify it is also handled during export
- Example: if an operation supports undo, verify all variants of that operation support undo

**Timing and ordering bugs:**
- When the same result is calculated in two places with different formulas, they will drift
- Look for hardcoded values that should be dynamic (e.g., hardcoded sample rate, hardcoded tick count)

---

## Phase 6 — Compile and Prioritize Findings

Write a findings list. For every finding, record:

- **What:** exact file and what the problem is
- **Category:** wrong placement / wrong dependency direction / bypassed abstraction / misplaced logic / duplication / divergence / real bug
- **Why it matters:** what breaks or gets harder because of this
- **Fix:** one sentence describing the correct state

**Prioritize in this order:**
1. Real bugs — things that are wrong right now, observable by a user
2. Divergence — two code paths that are out of sync and will get worse over time
3. Bypassed abstractions — things that prevent portable code from being portable
4. Wrong placement — structural debt with no current functional impact
5. Redundancy — unnecessary layers or wrappers

Do not start fixing low-priority items before all high-priority items are understood.
Do not start fixing anything before the full findings list is complete.

---

## Phase 7 — Plan the Refactoring

**Rules for sequencing refactoring steps:**

- Dependencies between changes determine order: if change B requires change A to be done first, do A first
- Logic changes and structural changes must not happen in the same step
  - First move code to the right place, then change its behavior
  - This keeps each step verifiable — a pure move should not change behavior at all
- Fix root causes, not symptoms
  - If a wrong-direction import exists because an abstraction is missing, create the abstraction, don't just move the import
- Highest-risk changes last
  - The safest changes are pure moves with no logic change
  - The riskiest are changes to shared infrastructure used by many callers

**For each step in the plan:**
- State what preconditions must be true before starting
- State exactly what changes (file created, deleted, moved, method signature changed)
- State what to test after the change
- Estimate risk: Low (compile check is enough) / Medium (manual feature test) / High (full regression)

**Never plan to fix more than one thing at a time in a single step.**
Each step should be a coherent, testable unit of work.

---

## Phase 8 — Execute

Work through the plan step by step.

**Before each step:**
- Confirm the precondition from Phase 7 is true
- If anything has changed since the plan was written, re-read the affected code and update the plan

**After each step:**
- Run the test defined in Phase 7 for that step
- If the test fails: stop, investigate, fix before continuing
- Do not proceed to the next step with a failing test

**When something unexpected appears:**
- Stop
- Read the unexpected code fully before deciding what to do
- Update the findings list if it reveals a new problem
- Update the plan if it affects the planned sequence
- Do not improvise fixes to unexpected problems without re-running Phase 6 and 7 for the new finding

---

## What This Process Finds That Ad-Hoc Review Misses

**The most important insight:** the dangerous problems are not the obvious ones. Wrong imports are obvious. The dangerous problems are:

- Code that looks correct but is incomplete relative to another code path (divergence)
- Business logic hiding inside infrastructure components (misplaced logic)
- Abstractions that are bypassed not because someone was lazy, but because the abstraction is incomplete
- State mutation that is invisible to the reactivity system

These require reading the code with a specific question in mind: "is this the only place this operation is performed, and if not, are all places equivalent?"

---

**Version:** 2.0
**Replaces:** Previous version which listed specific known findings rather than a generic process
