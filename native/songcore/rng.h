#ifndef POCKETTRACKER_SONGCORE_RNG_H
#define POCKETTRACKER_SONGCORE_RNG_H

// ─── The scheduler's random source ────────────────────────────────────────────────────────────────
//
// The C++ twin of kotlin.random.Random, for the four random FX: CHA (the chance gate), RND and RNL
// (randomize an FX value, or a note + instrument), and ARP mode 3 (RANDOM). Until S7 these draws
// were stubs — rng_int() returned 0 and rng_range() returned its low bound — so every one of them
// took not merely the same branch each time but the LOWEST value: CHA passed whenever its
// probability nibble was nonzero, RND/RNL always emitted the bottom of their range, and a random
// arpeggio always played the root. Correct-looking, and completely wrong.
//
// WHY THIS IS THE ONE PIECE OF SONGCORE THAT IS NOT GOLDEN-COMPARED.
//
// Every other layer is proven by byte-equality against the Kotlin original. This one cannot be:
// Kotlin's `Random.Default` is seeded from the platform, so the KOTLIN sequencer does not produce
// the same sequence twice either. There is no draw-for-draw ground truth to match, and cloning
// Kotlin's generator ALGORITHM would buy nothing — a stream nobody can predict is not a stream
// anybody can compare. What must match — and what tools/ptrandom does check against the real Kotlin
// implementation, before it is deleted — is the *distribution*:
//
//   • the SUPPORT: the exact set of values a draw can produce. This is where the bugs live. An
//     off-by-one, a closed interval where Kotlin's is half-open, or a stub pinned to the low end,
//     all show up here as a missing or extra value — exactly, and with no statistics involved.
//   • the SHAPE: uniform over that support.
//
// So the API below mirrors kotlin.random.Random's CONTRACT, bound for bound:
//
//     Kotlin                           songcore                  used by
//     Random.nextInt(bound)       →    next_int(bound)           CHA roll: nextInt(15) → 0..14
//     Random.nextInt(from, until) →    next_int(from, until)     RND/RNL value, RNL note/inst offset
//     listOf(a, b, c).random()    →    next_int(3) as an index   ARP RANDOM
//
// Half-open at the top, and `from` may be NEGATIVE — RNL draws its note and instrument offsets as
// nextInt(-range, range + 1), i.e. the inclusive band [-range, +range]. Get either end wrong and the
// FX quietly loses a semitone at one edge, which no ear and no golden would ever catch.
//
// Seeding matches Kotlin's default too: a fresh Sequencer seeds itself from the platform, so two
// runs of the same song differ — as they always have on the Kotlin path. That is why the audio
// golden (g7) is built to avoid random FX entirely, and why a render containing them is not
// reproducible on EITHER engine. seed() exists so the conformance tool can make its own measurement
// repeatable; the app never calls it.
//
// Real-time safe: no allocation, no locks, no syscalls once constructed.

#include <chrono>
#include <cstdint>

namespace songcore {

// PCG-XSH-RR 64/32 (O'Neill 2014): 64 bits of state, 32 out, a handful of instructions, and quality
// far beyond anything a tracker's chance gate can consume. The choice is not load-bearing — any
// generator that is uniform and fast satisfies the contract above — but a named, tested one beats an
// improvised xorshift nobody has looked at.
class Rng {
  public:
    Rng() { seed(platform_entropy()); }
    explicit Rng(uint64_t s) { seed(s); }

    void seed(uint64_t s) {
        state_ = 0u;
        inc_ = (s << 1u) | 1u;   // stream selector: any odd number
        next_u32();
        state_ += s;
        next_u32();
    }

    uint32_t next_u32() {
        uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + inc_;
        uint32_t xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
        uint32_t rot = static_cast<uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }

    /// Uniform over [0, bound). Mirrors kotlin.random.Random.nextInt(bound).
    int next_int(int bound) {
        if (bound <= 1) return 0;   // Kotlin throws on bound <= 0; no call site can reach it
        return static_cast<int>(bounded(static_cast<uint32_t>(bound)));
    }

    /// Uniform over [from, until), `from` may be negative. Mirrors nextInt(from, until).
    int next_int(int from, int until) {
        // Kotlin throws when until <= from. Both call sites already order their bounds (RND/RNL swap
        // min and max when the nibbles are inverted), and RNL guards range > 0 before drawing — so
        // this arm is unreachable, and returning `from` keeps it total rather than undefined.
        if (until <= from) return from;
        int64_t span = static_cast<int64_t>(until) - static_cast<int64_t>(from);
        return from + static_cast<int>(bounded(static_cast<uint32_t>(span)));
    }

  private:
    /// Uniform over [0, bound), bound >= 1. Rejection-sampled, not `% bound`: modulo folds the top of
    /// the 2^32 range onto the low values whenever bound does not divide 2^32, biasing them upward in
    /// frequency. At the sample sizes ptrandom runs, that bias is far below what its chi-square could
    /// resolve — this is correctness for its own sake, and it costs one branch that, for the bounds
    /// the FX actually use (3, 15, ≤256), retries with probability under 6e-8.
    uint32_t bounded(uint32_t bound) {
        uint32_t limit = (0xFFFFFFFFu / bound) * bound;   // largest exact multiple of bound
        uint32_t r;
        do { r = next_u32(); } while (r >= limit);
        return r % bound;
    }

    /// Kotlin seeds Random.Default from the platform; so do we. Mixed rather than raw: a clock read
    /// alone is too coarse if two Sequencers are constructed inside one tick, so it is folded with an
    /// address (ASLR) through splitmix64's finalizer. std::random_device is deliberately not used —
    /// it is deterministic on some MinGW toolchains, which is precisely the failure this must avoid.
    static uint64_t platform_entropy() {
        uint64_t x = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        x ^= static_cast<uint64_t>(
            std::chrono::system_clock::now().time_since_epoch().count()) << 17u;
        static int marker = 0;
        x ^= reinterpret_cast<uintptr_t>(&marker);

        x += 0x9E3779B97F4A7C15ULL;                       // splitmix64 finalizer
        x = (x ^ (x >> 30u)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27u)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31u);
    }

    uint64_t state_ = 0;
    uint64_t inc_ = 1;
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_RNG_H
