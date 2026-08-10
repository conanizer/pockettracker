#ifndef POCKETTRACKER_BYTE_SOURCE_H
#define POCKETTRACKER_BYTE_SOURCE_H

// ─── pt_fopen and friends — the one place a path string becomes a file operation ─────────────────
//
// Every touch of a user file below the UI goes through here: samples, soundfonts, projects, `.pti`
// presets, the render's own read-back, the "is this file still there?" probe — and the render's
// publish step, which deletes a stale target and renames a temp over it.
//
// **Why an indirection at all, when every one of them could just call `fopen`.** A path is not
// always a path. A directory a user grants through a system file picker is addressed by a URI with
// a scheme, and no libc call can touch one; it is resolved by the host. Deciding *which kind of
// string this is* has to happen somewhere, and the one thing it must not be is a check at each of
// the call sites: correctness would then rest on every future caller remembering to write it. So it
// is derived from the data, once, below all of them.
//
// **On a host that installs no hooks** every function here takes its plain libc branch and this is
// a rename. A URI with no hook fails, which is the right answer on a desktop.
//
// ⚠️ **The hooks arrive as ONE struct, and are installed by one call.** A host does not get to
// resolve opens and leave deletes on libc: half-hooked is the state where a render writes its temp
// through the host and then asks libc to rename a string libc cannot even see, which fails as
// "the export did not appear" with nothing in the log about storage.
//
// ⚠️ **They live in `byte_source.cpp`, not as an inline variable in this header, and that is
// deliberate.** On Android the engine (`libpockettracker.so`) reads them while the shell
// (`libpockettracker-sdl.so`) installs them. An inline variable is one COMDAT symbol per binary,
// kept single only by ELF interposition — which a `-fvisibility=hidden` added for size, at any point
// in the future, would silently break: the shell would install into its copy and the engine would
// keep reading its own null one, and the failure is a file that will not open with nothing in the
// log to say why. One definition in one translation unit does not depend on a flag.

#include <cstdio>
#include <string>

/**
 * Resolve a URI to an open OS file descriptor, or -1 if it cannot be resolved.
 *
 * ⚠️ **Ownership transfers to the caller** — `pt_fopen` wraps the returned descriptor in a `FILE*`
 * and closes it. The hook must therefore hand over a descriptor it has itself stopped tracking
 * (Android: `ParcelFileDescriptor.detachFd()`, not `getFd()`).
 *
 * `mode` is the stdio mode string as given to `pt_fopen`. A write mode may CREATE, because the
 * strings that reach here are composable — the host can derive the parent and the name from the
 * path (`shell/saf-filesystem.h`). ⚠️ **Append is not required of a hook**: nothing below the UI
 * opens for append, and a mode nobody exercises is a mode nobody notices breaking.
 */
using PtOpenHook = int (*)(const char* path, const char* mode);

/** Delete a URI. 0 on success, non-zero otherwise — `std::remove`'s convention, so the sites match. */
using PtRemoveHook = int (*)(const char* path);

/**
 * Rename a URI to another URI. 0 on success, non-zero otherwise (`std::rename`'s convention).
 *
 * ⚠️ **Both ends are URIs or neither is** — `pt_rename` never hands a hook one of each. A caller
 * that wants to cross the boundary is copying, not renaming, and that is a UI-layer act.
 */
using PtRenameHook = int (*)(const char* from, const char* to);

/** What a host installs. A null member means "this host cannot do that to a URI". */
struct PtFileHooks {
    PtOpenHook   open   = nullptr;
    PtRemoveHook remove = nullptr;
    PtRenameHook rename = nullptr;
};

/** Install the resolvers. Called once at boot by hosts that have them; never called elsewhere. */
void pt_set_file_hooks(const PtFileHooks& hooks);

/** What is installed. Exposed so a caller can tell "no host" from "the host said no". */
const PtFileHooks& pt_file_hooks();

/**
 * True when `path` begins with a URI scheme (`scheme://`, RFC 3986 scheme characters).
 *
 * Anchored at the front, so it cannot be tripped by a `://` inside a filename, and it does not
 * name a particular scheme — `content://` is not the only one a host may hand down.
 * A Windows drive letter (`C:\…`) has no `//` and is a plain path here, as it must be.
 */
bool pt_path_is_uri(const char* path);

/**
 * `std::fopen` for a plain path; the host hook for a URI. Null on failure, in both branches.
 *
 * The returned handle is an ordinary `FILE*` — seekable, and `fclose`d by the caller in the usual
 * way. ⚠️ Several decoders take the handle and **own it from then on** (`stb_vorbis_open_file`
 * closes it, and is corrupted by anyone else seeking it); the caller must not keep a second
 * reference to one it has passed on.
 */
FILE* pt_fopen(const char* path, const char* mode);

/**
 * Whole file into `out`, binary. False if it cannot be opened or is not read to its end; `out` is
 * only meaningful on true.
 */
bool pt_read_file(const char* path, std::string& out);

/**
 * `std::remove` for a plain path; the host hook for a URI. 0 on success, as `std::remove` returns.
 *
 * Deleting something that is not there is a failure here exactly as it is in libc — every caller
 * below the UI uses it to clear a stale target and ignores the result.
 */
int pt_remove(const char* path);

/**
 * `std::rename` for two plain paths; the host hook when EITHER is a URI. 0 on success.
 *
 * ⚠️ **One URI and one plain path fails**, rather than silently copying: the two live in different
 * storage domains, moving bytes between them is a read plus a write, and a caller that thought it
 * was doing a metadata operation would be doing whole-file I/O without knowing it.
 */
int pt_rename(const char* from, const char* to);

#endif  // POCKETTRACKER_BYTE_SOURCE_H
