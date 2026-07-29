// ptalsa — does shell/midi-out-alsa.h agree with the REAL libasound headers?
//
// ⚠️ THIS TOOL EXISTS BECAUSE THE THING IT CHECKS IS CHECKED BY NOTHING ELSE. The Linux MIDI backend
// reaches libasound through `dlopen`/`dlsym` (midi-out-alsa.cpp says why: the PortMaster build
// container has no libasound-dev, and a link-time dependency turns a CFW without the library into an
// app that will not start). The price of dlopen is that every prototype in `AlsaApi` is HAND-COPIED,
// and a hand-copied C signature is verified by no compiler and no linker: get one argument's type
// wrong and the call site still compiles, still links, and corrupts the stack at runtime — on a
// handheld, not on this desk.
//
// So the check is not "does the header agree with itself". It is the guardrails' rule about
// comparing against an INDEPENDENT invariant: this TU includes ALSA's own <alsa/asoundlib.h> and
// ASSIGNS the real functions into `AlsaApi`'s fields. C++ will not convert between incompatible
// function-pointer types, so a mismatched argument, return type or arity is a COMPILE ERROR naming
// the field. Upstream's declarations are the authority; midi-out-alsa.h is the thing on trial.
//
// It also pins the two integer constants the backend hard-codes, which no assignment could catch:
// `SND_RAWMIDI_STREAM_OUTPUT` in particular is load-bearing in a way that is easy to miss — it is the
// only thing keeping input-only ports off the OUTPUT row, and if it were wrong it would ALSO hide
// every output-only port, i.e. the failure looks like "no MIDI devices" rather than like a bug.
//
// ── HOW IT IS BUILT AND WHEN IT RUNS ─────────────────────────────────────────────────────────────
//
// It is the one tool in this directory with a build-time dependency (libasound2-dev), so
// tools/CMakeLists.txt adds it ONLY when <alsa/asoundlib.h> is present, and prints a STATUS line
// saying so when it is not. That means it does not run everywhere — a limitation stated rather than
// hidden. Run it by hand on any box with the headers after touching `AlsaApi`:
//
//     sudo apt-get install libasound2-dev
//     cmake -S tools -B tools/build-linux -DCMAKE_BUILD_TYPE=Release && \
//         cmake --build tools/build-linux --target ptalsa && ./tools/build-linux/ptalsa
//
// ⚠️ It does NOT link libasound and does not open a device: it is a compile-time conformance check
// with a runtime report, so it is safe on a machine with no sound card at all.

#include <cstdio>

#if !defined(__linux__) || defined(__ANDROID__)
int main() {
    std::printf("ptalsa: not Linux - nothing to check\n");
    return 0;
}
#else

#include <alsa/asoundlib.h>

#include <type_traits>

#include "../../shell/midi-out-alsa.h"

using ptshell::alsa_detail::AlsaApi;

// ── The check, in two halves, and BOTH are needed ────────────────────────────────────────────────
//
// AlsaApi cannot be compared to libasound directly: its opaque handles are `void*` (they are
// pointer-to-incomplete in ALSA's headers too, so the ABI is identical, but the TYPES differ). So:
//
//   half 1 — a `Real_*` alias per function, written out by hand, then BOUND to the real symbol.
//            If upstream's signature is not what the alias says, that initializer is illegal and
//            this file does not compile. This is the half anchored to something outside our tree.
//   half 2 — a `static_assert` per function comparing `Real_*` to AlsaApi's field once the opaque
//            pointers are erased to void*, which is the only difference the backend may have.
//
// Half 1 without half 2 would check nothing about the backend; half 2 without half 1 would be the
// header agreeing with itself, which the guardrails call not a check at all.
namespace {

// Upstream's types, named. This is the half that fails to compile when a signature is wrong.
using Real_card_next                = int  (*)(int*);
using Real_ctl_open                 = int  (*)(snd_ctl_t**, const char*, int);
using Real_ctl_close                = int  (*)(snd_ctl_t*);
using Real_ctl_rawmidi_next_device  = int  (*)(snd_ctl_t*, int*);
using Real_ctl_rawmidi_info         = int  (*)(snd_ctl_t*, snd_rawmidi_info_t*);
using Real_rawmidi_info_malloc      = int  (*)(snd_rawmidi_info_t**);
using Real_rawmidi_info_free        = void (*)(snd_rawmidi_info_t*);
using Real_rawmidi_info_set_device  = void (*)(snd_rawmidi_info_t*, unsigned int);
using Real_rawmidi_info_set_subdev  = void (*)(snd_rawmidi_info_t*, unsigned int);
using Real_rawmidi_info_set_stream  = void (*)(snd_rawmidi_info_t*, snd_rawmidi_stream_t);
using Real_rawmidi_info_get_name    = const char* (*)(const snd_rawmidi_info_t*);
using Real_rawmidi_open             = int  (*)(snd_rawmidi_t**, snd_rawmidi_t**, const char*, int);
using Real_rawmidi_close            = int  (*)(snd_rawmidi_t*);
using Real_rawmidi_write            = ssize_t (*)(snd_rawmidi_t*, const void*, size_t);
using Real_rawmidi_drain            = int  (*)(snd_rawmidi_t*);
using Real_strerror                 = const char* (*)(int);

// ⚠️ THE ACTUAL CHECK. Each initializer takes the address of the real libasound function and binds it
// to upstream's declared type. A changed or mis-remembered signature makes the initializer illegal
// and this file does not build — which is the pass/fail, and it happens before main() ever runs.
[[maybe_unused]] constexpr Real_card_next               k1  = &snd_card_next;
[[maybe_unused]] constexpr Real_ctl_open                k2  = &snd_ctl_open;
[[maybe_unused]] constexpr Real_ctl_close               k3  = &snd_ctl_close;
[[maybe_unused]] constexpr Real_ctl_rawmidi_next_device k4  = &snd_ctl_rawmidi_next_device;
[[maybe_unused]] constexpr Real_ctl_rawmidi_info        k5  = &snd_ctl_rawmidi_info;
[[maybe_unused]] constexpr Real_rawmidi_info_malloc     k6  = &snd_rawmidi_info_malloc;
[[maybe_unused]] constexpr Real_rawmidi_info_free       k7  = &snd_rawmidi_info_free;
[[maybe_unused]] constexpr Real_rawmidi_info_set_device k8  = &snd_rawmidi_info_set_device;
[[maybe_unused]] constexpr Real_rawmidi_info_set_subdev k9  = &snd_rawmidi_info_set_subdevice;
[[maybe_unused]] constexpr Real_rawmidi_info_set_stream k10 = &snd_rawmidi_info_set_stream;
[[maybe_unused]] constexpr Real_rawmidi_info_get_name   k11 = &snd_rawmidi_info_get_name;
[[maybe_unused]] constexpr Real_rawmidi_open            k12 = &snd_rawmidi_open;
[[maybe_unused]] constexpr Real_rawmidi_close           k13 = &snd_rawmidi_close;
[[maybe_unused]] constexpr Real_rawmidi_write           k14 = &snd_rawmidi_write;
[[maybe_unused]] constexpr Real_rawmidi_drain           k15 = &snd_rawmidi_drain;
[[maybe_unused]] constexpr Real_strerror                k16 = &snd_strerror;

// ── And now the same signatures as AlsaApi spells them, with the opaque handles substituted. ─────
//
// The pairs below are what actually compares the two. Each `static_assert` asks whether upstream's
// type and AlsaApi's field type are the same ONCE the opaque pointers are erased to void* — which is
// the only difference the backend is entitled to have. Anything else (an argument's width, a
// missing argument, a return type) makes the two differ and fires the assert BY NAME.
template <typename T> struct Erase { using type = T; };
template <> struct Erase<snd_ctl_t*>            { using type = void*; };
template <> struct Erase<snd_ctl_t**>           { using type = void**; };
template <> struct Erase<snd_rawmidi_t*>        { using type = void*; };
template <> struct Erase<snd_rawmidi_t**>       { using type = void**; };
template <> struct Erase<snd_rawmidi_info_t*>   { using type = void*; };
template <> struct Erase<snd_rawmidi_info_t**>  { using type = void**; };
template <> struct Erase<const snd_rawmidi_info_t*> { using type = const void*; };
template <> struct Erase<snd_rawmidi_stream_t>  { using type = int; };
template <> struct Erase<ssize_t>               { using type = ptrdiff_t; };

template <typename R, typename... A>
struct Erased { using type = typename Erase<R>::type (*)(typename Erase<A>::type...); };

template <typename F> struct Signature;
template <typename R, typename... A>
struct Signature<R (*)(A...)> { using type = typename Erased<R, A...>::type; };

template <typename Real, typename Field>
constexpr bool same = std::is_same<typename Signature<Real>::type, Field>::value;

static_assert(same<Real_card_next,               decltype(AlsaApi::card_next)>,               "card_next");
static_assert(same<Real_ctl_open,                decltype(AlsaApi::ctl_open)>,                "ctl_open");
static_assert(same<Real_ctl_close,               decltype(AlsaApi::ctl_close)>,               "ctl_close");
static_assert(same<Real_ctl_rawmidi_next_device, decltype(AlsaApi::ctl_rawmidi_next_device)>, "ctl_rawmidi_next_device");
static_assert(same<Real_ctl_rawmidi_info,        decltype(AlsaApi::ctl_rawmidi_info)>,        "ctl_rawmidi_info");
static_assert(same<Real_rawmidi_info_malloc,     decltype(AlsaApi::rawmidi_info_malloc)>,     "rawmidi_info_malloc");
static_assert(same<Real_rawmidi_info_free,       decltype(AlsaApi::rawmidi_info_free)>,       "rawmidi_info_free");
static_assert(same<Real_rawmidi_info_set_device, decltype(AlsaApi::rawmidi_info_set_device)>, "rawmidi_info_set_device");
static_assert(same<Real_rawmidi_info_set_subdev, decltype(AlsaApi::rawmidi_info_set_subdevice)>, "rawmidi_info_set_subdevice");
static_assert(same<Real_rawmidi_info_set_stream, decltype(AlsaApi::rawmidi_info_set_stream)>, "rawmidi_info_set_stream");
static_assert(same<Real_rawmidi_info_get_name,   decltype(AlsaApi::rawmidi_info_get_name)>,   "rawmidi_info_get_name");
static_assert(same<Real_rawmidi_open,            decltype(AlsaApi::rawmidi_open)>,            "rawmidi_open");
static_assert(same<Real_rawmidi_close,           decltype(AlsaApi::rawmidi_close)>,           "rawmidi_close");
static_assert(same<Real_rawmidi_write,           decltype(AlsaApi::rawmidi_write)>,           "rawmidi_write");
static_assert(same<Real_rawmidi_drain,           decltype(AlsaApi::rawmidi_drain)>,           "rawmidi_drain");
static_assert(same<Real_strerror,                decltype(AlsaApi::strerror_fn)>,             "strerror");

// ── The constants, which no assignment could catch ───────────────────────────────────────────────
//
// ⚠️ STREAM_OUTPUT is the filter that keeps input-only ports off the OUTPUT row (scan() asks
// snd_ctl_rawmidi_info for the OUTPUT stream and treats -ENXIO as "skip"). Wrong, and the row lists
// the user's MIDI keyboard as a destination while hiding their synth.
static_assert(ptshell::alsa_detail::STREAM_OUTPUT == SND_RAWMIDI_STREAM_OUTPUT, "STREAM_OUTPUT");
static_assert(ptshell::alsa_detail::NONBLOCK == SND_RAWMIDI_NONBLOCK, "NONBLOCK");

}  // namespace

int main() {
    // Everything above is compile-time; reaching here IS the pass. The report exists so that a green
    // run prints the number beside the verdict rather than an unqualified "OK" — 16 prototypes and 2
    // constants is the count a future reader should compare against AlsaApi's field list, because a
    // field ADDED to AlsaApi and not added here would otherwise pass in silence.
    std::printf("ptalsa: 16 prototypes and 2 constants checked against <alsa/asoundlib.h>\n");
    std::printf("ptalsa: symbols the backend dlsym()s      = 16\n");
    std::printf("ptalsa: fields in AlsaApi                 = %d\n",
                static_cast<int>(sizeof(AlsaApi) / sizeof(void (*)())));
    if (sizeof(AlsaApi) / sizeof(void (*)()) != 16) {
        std::printf("ptalsa: FAIL - AlsaApi has fields this check does not cover\n");
        return 1;
    }
    std::printf("ptalsa: ALL GREEN\n");
    return 0;
}

#endif  // __linux__ && !__ANDROID__
