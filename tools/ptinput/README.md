# ptinput — the input layer, byte-compared against Kotlin

The ninth conformance tool, and the one that measures **what a button press does**.

```
cmake -S tools -B tools/build -DCMAKE_BUILD_TYPE=Release
cmake --build tools/build --config Release
ctest --test-dir tools/build -R p3-input --output-on-failure -C Release
```

It reads `testdata/units/p3-input.txt` — emitted by the JVM `P3InputGoldenTest` from the **real**
Kotlin `InputController`, the five screen modules, `ClipboardManager` and `FxHelperOverlay` — and for
every `<inputs> => <outputs>` line it re-parses the inputs, recomputes the right-hand side through the
C++ port (`native/ui/{cursor,selection,clipboard,fx_helper}.h` + `native/ui/modules/*`), and
byte-compares. **1647 cases.** Exit 0 = green, 1 = any mismatch, and a mismatch names the line.

## What an `EDIT` line claims, and why it takes three parts

```
EDIT scr=PHRASE col=1 step=-1.0:7F:00:00/00:00/00:00/00 btn=A
  => ctx=NOTE|.....I.E|0|12|127|1|12|-1|0|-  act=INSERT  step=0.4:7F:00:00/00:00/00:00/00
```

1. the **cursor context** — "the cursor is on a NOTE, it is empty, it can be inserted, its range is
   12..127, a small step is 1 and a large step is 12"
2. the **resolved action** — what `handleAButton(ctx)` and its four siblings return
3. the **resulting cell** — what the module's `handleInput` actually wrote

The third one is the one that earns its keep, and this is not a theoretical worry — it was **measured**.
Change `delete_phrase_steps`'s velocity clear from `0x7F` to `0x00` (a plausible "clearing means zero"
bug; a phrase velocity has no empty state, so clearing it means *full*) and:

* **every context matches. Every action matches.** A tool comparing only those two is **completely
  blind** to it.
* the **cell** diverges in 8 places, and the tool names the byte.

A module that resolves A on a velocity to `SET_VALUE(0x40)` and then writes 0x40 into the *instrument*
field satisfies (1) and (2). Only (3) catches it. So every line carries the cell before **and** after,
and every `CLIP` case is followed by a dump of the whole 16-row grid it operated on — because a
mis-anchored paste writes the right bytes into the wrong cells.

## The caps string

`ctx=<TYPE>|<caps>|<cur>|<min>|<max>|<small>|<large>|<empty>|<fxSlot>|<default or ->`, where `caps` is
the eight capability flags in declaration order, `.` for false:

| | | | | | | | |
|---|---|---|---|---|---|---|---|
| `+` | `-` | `>` | `<` | `D` | `I` | `C` | `E` |
| canIncrement | canDecrement | fast + | fast − | canDelete | canInsert | canCreate | isEmpty |

So `+-><D...` reads as "steps both ways, fast both ways, deletable, not empty".

## The four line kinds

| kind | cases | what it pins |
|---|---|---|
| `EDIT` | 1385 | context + action + mutation, over all five screens × every column × a value ladder × the five buttons |
| `FXH` | 170 | the 6×5 effect grid — above all its **centred last row**, whose edge cells are empty and unreachable, and every move that could land on one rounds inward |
| `SEL` | 64 | the L+B multi-tap CELL→ROW→SCREEN machine, D-pad edge-dragging, and the clamps |
| `CLIP` | 28 | copy / cut / paste / delete, incl. the cross-screen rejection and the paste re-anchoring |

## ⚠️ The clock

`SEL` scripts spell a tap as `F` (inside the 500 ms multi-tap window) or `S` (outside it) rather than
as a timestamp, because the **JVM side cannot be handed a fake clock** —
`InputController.handleSelectB` reads `System.currentTimeMillis()` itself. The C++ port takes `now_ms`
as a *parameter* precisely so that it can, and this tool advances a fake clock by 10 ms for `F` and
600 ms for `S`. What is under test is "tap fast" versus "tap slow", not "tap at t=137 ms", and the
encoding says so. (Kotlin realises `S` with one `Thread.sleep` — the only sleep in the suite, and it
buys the one transition that cannot be observed any other way.)

## ⚠️ What it does NOT cover

The **dispatcher** (`native/ui/input_dispatcher.cpp`) — the *composition* of these pieces. ptinput
proves each unit matches Kotlin; nothing here proves they are wired together correctly. A bug like
"B+RIGHT cycles the TABLE pool at 256 instead of 128" leaves every case above green, because ptinput
never calls `cycle_current_item` at all. That gap was closed for S3 by a throwaway harness driving the
real dispatcher (85 checks), in the shape S1's edit-path check and S2's `navcheck` took — and its
negative control is exactly the pool-size bug above: **ptinput ALL GREEN, harness RED.**

A permanent golden for the dispatcher would have to be recorded from Kotlin's `AppInputDispatcher`,
which is entangled with ~60 Compose `mutableStateOf` refs; that is a session of its own, and worth
opening only if the dispatcher starts regressing.

## Regenerating the golden

Deliberate change to the Kotlin input layer → delete `testdata/units/p3-input.txt`, run
`./gradlew :app:testDebugUnitTest --tests "*P3InputGoldenTest"`, review the diff, commit. `ptinput`
then holds C++ to the new behaviour. Left in place, the JVM test byte-compares instead and names the
first line that drifted.
