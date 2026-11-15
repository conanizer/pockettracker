# Vendored Braids (the MACRO instrument's oscillators)

**Upstream:** https://github.com/pichenettes/eurorack (the `braids/` folder) and its submodule https://github.com/pichenettes/stmlib

**Pinned commits:** eurorack `f4cfa7697095a28d292bff758beb51deb49b5a86` (2026-10-12), stmlib `e3bd7c9cc00e4364166f9905c0509b6ffd0535ec` (2023-05-30, the commit eurorack's submodule points to)

**Vendored:** 2026-11-15

**Licence:** MIT, copyright Emilie Gillet. Each source file carries the notice, and `stmlib/LICENSE` is upstream's file. MIT is compatible with PocketTracker's GPL-3.0.

**Name:** upstream's README says "Mutable Instruments" is a registered trademark that derivative works must not use, and recommends not keeping the module's name. PocketTracker calls the instrument MACRO. Use "Braids" only in credits and in documentation that says where the code comes from.

## What is vendored

Only what the macro-oscillator needs to render audio:

- `braids/macro_oscillator.{h,cc}`, `analog_oscillator.{h,cc}`, `digital_oscillator.{h,cc}`: the oscillators
- `braids/resources.{h,cc}`: the lookup tables and wavetables
- `braids/settings.h`: included for the `MacroOscillatorShape` enum; `settings.cc` is not needed
- `braids/parameter_interpolation.h`, `excitation.h`, `svf.h`
- `stmlib/stmlib.h`, `stmlib/utils/dsp.h`, `stmlib/utils/random.{h,cc}`
- `stmlib/LICENSE`

Not vendored: the module's firmware (`braids.cc`, `ui.cc`, `drivers/`, `bootloader/`), its envelope, quantizer, waveshaper and jitter source, the hardware design files, and the rest of stmlib (`system/`, `third_party/STM/` and so on, which is the STM32 code).

`pt-msvc.h` and this file are PocketTracker's, not upstream's.

## How it is built

`native/CMakeLists.txt` builds these files as the `braids` static library. Three settings matter:

- `TEST` is defined. It is stmlib's own switch for building off the module. Without it, `stmlib.h` places functions in a linker section that only the firmware has.
- `-fwrapv` is passed to GCC and Clang. The code multiplies and shifts 32-bit signed values past their range in many places and relies on them wrapping, as they do on the module's ARM core. Without the flag that is undefined behaviour.
- On MSVC, `pt-msvc.h` is included ahead of every file (`/FI`). It defines `__attribute__` away, because `dsp.h` marks its inline functions `__attribute__((always_inline))`. MSVC has no `-fwrapv`. As of 2026-11-15 the MSVC build is untested.

## What a caller must do

Keep the pitch passed to `set_pitch` within 0..16383 (MIDI note × 128, so notes 0 to just under 128). The module's firmware does, and the code relies on it: above 16383 the fluted model reads past its 128-entry body-filter table, and far below 0 the phase-increment shift is undefined. Within the range, every model runs clean under the sanitizers.

## Local changes

Two, both in `braids/digital_oscillator.cc`, both marked with a `PocketTracker:` comment. Re-apply them on any update.

1. `wave_line` has one more entry (the last wave, 182, repeated). `RenderWaveLine` reads `wave_line[(scan >> 10) + 1]`, which is index 64 of a 64-entry table when the first parameter is near its top. Upstream reads whatever byte follows the table.
2. `ComputeDelay` returns 0 when the pitch is more than 12 octaves below the table, where upstream shifts by a negative count. The comb model reaches this with a low note and its first parameter low. On the module's ARM core that shift gives 0, so the sound is the module's.

A run of every model under GCC's address and undefined-behaviour sanitizers, with `-fwrapv`, reports nothing after these two changes.

## Checking the files against upstream

The folder has its own `.gitattributes` (`* -text`), so git stores the bytes as upstream published them on every platform.

On 2026-11-15, `git hash-object` of each vendored file matched the blob in the pinned commits, except `braids/digital_oscillator.cc`, which carries the two changes above.

## Updating

```sh
git -c core.autocrlf=false clone https://github.com/pichenettes/eurorack.git
git -C eurorack submodule update --init stmlib
# Copy the files listed above into native/vendor/braids/, keeping the braids/ and stmlib/ folders.
# Re-apply the two local changes, then compare `git hash-object <file>` with
# `git -C eurorack rev-parse HEAD:<file>` (stmlib files: `git -C eurorack/stmlib rev-parse HEAD:<file>`).
```

Then render every model again and compare levels and timing with the numbers in the pull request that added MACRO.
