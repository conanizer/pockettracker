# Vendored FAAD2 (AAC decoder)

**Upstream:** https://github.com/knik0/faad2
**Pinned commit:** `b743105de73f31f214f824e72d2001a3736da768` (master, PACKAGE_VERSION 2.11.2)
**Vendored:** 2026-07-27, for the mp4/AAC in-place sample decoder (convergence media-unification).
**Licence:** GPL-2.0-or-later (see `COPYING`). Compatible with PocketTracker's GPL-3.0 — this is
*why* FAAD2 was chosen over fdk-aac (whose licence FSF considers GPL-incompatible). Recorded in
`licenses/THIRD-PARTY-NOTICES.md`.

## What is vendored, and what is not

Only the decode library and its public header — the subset needed to build `libfaad` as a static
archive:

- `libfaad/*.c` `*.h` (38 `.c`, plus headers) and `libfaad/codebook/*.h` (13 Huffman tables)
- `include/neaacdec.h` — the public API (`NeAACDecOpen`/`Init2`/`Decode`/`Close`)
- `COPYING` — the licence

Deliberately NOT vendored: `frontend/`, `fuzz/`, `docs/`, the Bazel files, the top-level
`CMakeLists.txt` and `properties.json`. We do **not** use FAAD2's own CMake — `native/CMakeLists.txt`
builds `libfaad` with an explicit `add_library(faad2 STATIC …)` target, exactly as it does for
`opusfile`, so there is no autotools/`config.h` generation step (which is what the F-Droid buildserver
would trip over). `common.h` guards its `config.h` include behind `HAVE_CONFIG_H`, which we never
define; the library uses its own compile-time defaults (floating-point, not `FIXED_POINT`).

## Byte-identity to upstream (the C1 lesson)

The tree carries its own `.gitattributes` (`* -text`) so git stores these bytes verbatim on every
platform, Windows included — a CRLF-converted copy would compile perfectly and quietly stop being what
upstream published. Verified at vendor time by comparing `git hash-object` of every file against the
upstream commit's own blob OIDs: **100/100 identical.** A local `diff` against the clone is NOT a valid
check here (both sides share any EOL conversion); the blob-OID comparison is the independent invariant.

## Re-vendoring

Clone with EOL conversion OFF, copy the subset, re-verify blob OIDs:

```sh
git -c core.autocrlf=false clone https://github.com/knik0/faad2.git
git -C faad2 checkout b743105de73f31f214f824e72d2001a3736da768   # or a newer pin
# copy libfaad/**, include/neaacdec.h, COPYING into native/vendor/faad2/
```
