# Vendored minimp4 (MP4 / ISO-BMFF demuxer)

**Upstream:** https://github.com/lieff/minimp4
**File:** `minimp4.h` (single header, 3502 lines)
**Vendored:** 2026-07-27, for the mp4/AAC in-place sample decoder (convergence media-unification).
**Licence:** CC0-1.0 / public domain (see the header comment). Recorded in
`docs/licenses/THIRD-PARTY-NOTICES.md`.

Single-header, drops into the existing `dr_mp3` vendor style: the implementation is compiled exactly
once, inside `audio-decoders.cpp`, by defining `MINIMP4_IMPLEMENTATION` before the include. Only the
**demuxer** (`MP4D_*`) is used — the muxer half is unused and dropped by the linker. minimp4 parses the
container and hands us each AAC access unit plus the track's AudioSpecificConfig (DSI); FAAD2 decodes
the AAC. One demuxer covers `.m4a`/`.mp4`/`.m4b`/`.mov`/`.3gp` — they are all ISO-BMFF.

## Byte-identity

`.gitattributes` (`* -text`) stores the bytes verbatim. Verified upstream-identical at vendor time by
raw-byte md5 across the original download, a fresh independent re-download, and the vendored copy
(all `d4b0f3ef6222436a72ab4b13e267454e`). Note: `git hash-object` is filter-subject and gave a
misleading mismatch outside the `* -text` guard — the raw md5 is the independent invariant, not the
git blob hash.

## Re-vendoring

```sh
curl -L https://raw.githubusercontent.com/lieff/minimp4/master/minimp4.h -o native/vendor/minimp4/minimp4.h
```
