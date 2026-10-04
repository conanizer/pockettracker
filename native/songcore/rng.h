#ifndef POCKETTRACKER_SONGCORE_RNG_H
#define POCKETTRACKER_SONGCORE_RNG_H

// ─── The scheduler's random source ────────────────────────────────────────────────────────────────
//
// For the four random FX: CHA (chance gate), RND and RNL (randomize an FX value, or note +
// instrument), and ARP mode 3 (RANDOM).
//
// Not golden-compared: the sequence is seeded from the platform, so no draw-for-draw truth exists.
// What the tests check is the DISTRIBUTION — the exact SUPPORT (where off-by-one bugs show, with no
// statistics) and a uniform shape:
//
//     next_int(bound)         CHA roll: next_int(15) → 0..14
//     next_int(from, until)   RND/RNL: the amount added, 0..XY
//     next_int(3) as index    ARP RANDOM
//
// ⚠️ Half-open at the top: RND/RNL draw next_int(0, range + 1) so `range` itself can come out. Get it
// wrong and the FX silently never reaches its top value.
// Two runs of the same song differ, so a render with random FX is never reproducible.
// seed() is for the tests only. Real-time safe once constructed.

#include <chrono>
#include <cstdint>

namespace songcore {

// PCG-XSH-RR 64/32 (O'Neill 2014): small, fast, far beyond what a chance gate needs. Any uniform fast
// generator would satisfy the contract; a named, tested one beats an improvised one.
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

    /// Uniform over [0, bound).
    int next_int(int bound) {
        if (bound <= 1) return 0;   // no call site passes bound <= 0
        return static_cast<int>(bounded(static_cast<uint32_t>(bound)));
    }

    /// Uniform over [from, until); `from` may be negative.
    int next_int(int from, int until) {
        // Every call site draws only over a non-empty range; returning `from` keeps this total.
        if (until <= from) return from;
        int64_t span = static_cast<int64_t>(until) - static_cast<int64_t>(from);
        return from + static_cast<int>(bounded(static_cast<uint32_t>(span)));
    }

  private:
    /// Uniform over [0, bound), bound >= 1. Rejection-sampled, not `% bound`, which would bias the low
    /// values whenever bound does not divide 2^32. For the bounds in use it retries with probability
    /// under 6e-8.
    uint32_t bounded(uint32_t bound) {
        uint32_t limit = (0xFFFFFFFFu / bound) * bound;   // largest exact multiple of bound
        uint32_t r;
        do { r = next_u32(); } while (r >= limit);
        return r % bound;
    }

    /// Seeded from the platform, mixed: a clock read is too coarse for two Sequencers built in one
    /// tick, so it is folded with an address (ASLR) through splitmix64's finalizer.
    /// ⚠️ Not std::random_device — it is deterministic on some MinGW toolchains.
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
