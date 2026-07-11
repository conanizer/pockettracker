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

## Build & run

The four conformance tools are one CMake project (`tools/CMakeLists.txt`), wired to ctest. CI runs
exactly this on every push, on gcc/x86-64, MSVC/x86-64 and clang/arm64:

```
cmake -S tools -B tools/build -DCMAKE_BUILD_TYPE=Release
cmake --build tools/build --config Release
ctest --test-dir tools/build --output-on-failure -C Release
```

This tool alone is the **`s2-project-io`** test — `ctest --test-dir tools/build -R s2-project-io
--output-on-failure` — or invoke the built binary directly with the goldens directory as `argv[1]`
(`ptroundtrip testdata`).

Exit code `0` = all green, `1` = any mismatch. Expected output ends in `ALL GREEN`.
