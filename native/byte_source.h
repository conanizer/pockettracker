#ifndef POCKETTRACKER_BYTE_SOURCE_H
#define POCKETTRACKER_BYTE_SOURCE_H

// ─── pt_fopen — the one place a path string becomes an open file ─────────────────────────────────
//
// Every read of a user file below the UI goes through here: samples, soundfonts, projects, `.pti`
// presets, the render's own read-back, and the "is this file still there?" probe. Thirteen call
// sites across the engine, the decoders and songcore.
//
// **Why an indirection at all, when every one of them could just call `fopen`.** A path is not
// always a path. A directory a user grants through a system file picker is addressed by a URI with
// a scheme — `content://…` — and no libc call can open one; it is resolved by the host and handed
// back as an already-open descriptor. Deciding *which kind of string this is* has to happen
// somewhere, and the one thing it must not be is a check at each of the thirteen sites: correctness
// would then rest on every future caller remembering to write it. So it is derived from the data,
// once, below all of them.
//
// **Today no host installs a hook**, so every platform takes the plain `std::fopen` branch and this
// is a rename. A URI with no hook fails to open, which is the right answer on a desktop.
//
// ⚠️ **The hook lives in `byte_source.cpp`, not as an inline variable in this header, and that is
// deliberate.** On Android the engine (`libpockettracker.so`) reads the hook while the shell
// (`libpockettracker-sdl.so`) installs it. An inline variable is one COMDAT symbol per binary, kept
// single only by ELF interposition — which a `-fvisibility=hidden` added for size, at any point in
// the future, would silently break: the shell would install into its copy and the engine would keep
// reading its own null one, and the failure is a file that will not open with nothing in the log to
// say why. One definition in one translation unit does not depend on a flag.

#include <cstdio>
#include <string>

/**
 * Resolve a URI to an open OS file descriptor, or -1 if it cannot be resolved.
 *
 * ⚠️ **Ownership transfers to the caller** — `pt_fopen` wraps the returned descriptor in a `FILE*`
 * and closes it. The hook must therefore hand over a descriptor it has itself stopped tracking
 * (Android: `ParcelFileDescriptor.detachFd()`, not `getFd()`).
 *
 * `mode` is the stdio mode string as given to `pt_fopen`. ⚠️ **`"w"`/`"a"` cannot create anything**:
 * a document has to exist before it can be opened for write, and creating one is a UI-layer act
 * (`ui::FileSystem`) that knows about names and parent directories. A hook asked to open a URI that
 * does not exist returns -1.
 */
using PtOpenHook = int (*)(const char* path, const char* mode);

/** Install the resolver. Called once at boot by hosts that have one; never called elsewhere. */
void pt_set_open_hook(PtOpenHook hook);

/** The installed resolver, or null. Exposed so a caller can tell "no host" from "host said no". */
PtOpenHook pt_get_open_hook();

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

#endif  // POCKETTRACKER_BYTE_SOURCE_H
