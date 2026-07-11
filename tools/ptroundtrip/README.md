# ptroundtrip — songcore schema round-trip conformance

A host tool (no device / NDK) that proves the C++ songcore serializer is **byte-for-byte
compatible** with the Kotlin `kotlinx.serialization` output. It reads each golden `.ptp` from
`/testdata`, runs the real load path (`parse → normalize → migrate`), re-serializes, and compares
to the **original bytes**. Any difference is schema drift and is reported with the exact offset +
context. Also checks the `.pti` (`InstrumentPreset`) reader/writer via write→read→write idempotence.

This is the bit-exact half of the schema-lock described in `linux-port-plan.md` §4.4; it is destined
to run under `ctest` in CI once the `pt-core` / linux-x86_64 build lands.

## What it exercises

`native/songcore/model.h` (structs + field defaults) and `native/songcore/project_io.h`
(nlohmann parse + the kotlinx-exact emitter + `normalize`/`migrate`). The six goldens collectively
cover populated phrases/notes/FX, chain transposes, tracks with `chainRefs`/`mute`, 128 instruments
(incl. a SOUNDFONT slot), tables, grooves, and EQ presets.

## Build & run (Windows, on-box MSVC)

There is no CMake target yet (arrives with the CI lane). Compile the single TU directly — its
`#include`s are relative, so compile it in place:

```
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cl /std:c++17 /EHsc /O2 /nologo tools\ptroundtrip\main.cpp /Fe:ptroundtrip.exe
ptroundtrip.exe testdata
```

`clang++` / `g++` work equally (`-std=c++17 tools/ptroundtrip/main.cpp -o ptroundtrip`).

Exit code `0` = all green, `1` = any mismatch. Expected output ends in `ALL GREEN`.
