#ifndef POCKETTRACKER_BYTE_SOURCE_H
#define POCKETTRACKER_BYTE_SOURCE_H

// Every user-file operation below the UI goes through here. A path may be a URI (a folder granted
// through a system picker), which only the host's hooks can open; without hooks a URI fails.
// ⚠️ The hooks are installed as one struct: half-hooked, a render would write its temp through the
// host and then ask libc to rename it.
// ⚠️ They live in the .cpp: on Android the engine and shell are separate libraries, and an inline
// variable would give each its own copy.

#include <cstdio>
#include <string>

// A URI → an open file descriptor, or −1. `mode` is `pt_fopen`'s; a write mode may create.
// ⚠️ The caller takes ownership, so the hook must let go of it (Android: `detachFd()`, not `getFd()`).
// Append need not be supported — nothing opens for append.
using PtOpenHook = int (*)(const char* path, const char* mode);

// Delete a URI. 0 on success, as `std::remove`.
using PtRemoveHook = int (*)(const char* path);

// Rename a URI to another URI. 0 on success, as `std::rename`. Never handed a mixed pair.
using PtRenameHook = int (*)(const char* from, const char* to);

// What a host installs. A null member: this host cannot do that to a URI.
struct PtFileHooks {
    PtOpenHook   open   = nullptr;
    PtRemoveHook remove = nullptr;
    PtRenameHook rename = nullptr;
};

// Called once at boot by hosts that have hooks.
void pt_set_file_hooks(const PtFileHooks& hooks);

const PtFileHooks& pt_file_hooks();

// True when `path` starts with `scheme://`. A drive letter (`C:\…`) is a plain path.
bool pt_path_is_uri(const char* path);

// `std::fopen`, or the host hook for a URI. Null on failure.
// ⚠️ Some decoders own the handle once given it (`stb_vorbis_open_file` closes it); keep no copy.
FILE* pt_fopen(const char* path, const char* mode);

// The whole file into `out`. False if it cannot be opened or read to its end.
bool pt_read_file(const char* path, std::string& out);

// `std::remove`, or the host hook for a URI. 0 on success.
int pt_remove(const char* path);

// `std::rename`, or the host hook when either end is a URI. 0 on success.
// ⚠️ A URI and a plain path fail rather than quietly becoming a whole-file copy.
int pt_rename(const char* from, const char* to);

#endif  // POCKETTRACKER_BYTE_SOURCE_H
