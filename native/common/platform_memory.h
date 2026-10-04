#ifndef POCKETTRACKER_PLATFORM_MEMORY_H
#define POCKETTRACKER_PLATFORM_MEMORY_H

// ─── How much memory this machine will actually give us ──────────────────────────────────────────
//
// The denominator the USED RAM readout never had, and the number a load has to be measured against
// before it is attempted.
//
// ⚠️⚠️ THE ALLOCATOR WILL NOT TELL US. bionic grants requests far beyond the machine (it overcommits
// untouched address space, and the kernel kills the process when the pages are written); glibc
// refuses only above MemTotal + SwapTotal, never against what is free. Only Windows (commit
// accounting) and 32-bit address spaces fail early. So a load asks first.

#include <cstdint>

namespace pt {

/**
 * Physical memory a large allocation could plausibly obtain right now, in bytes. 0 when the platform
 * cannot answer — ⚠️ **callers must treat 0 as "unknown" and NOT as "nothing is free"**, or a probe
 * that fails to read turns into a refusal of every file.
 *
 * ⚠️ **SWAP IS DELIBERATELY NOT COUNTED**, on any platform. Sample and soundfont PCM is touched on
 * every audio callback, so audio that lives in swap does not play — admitting a load that only fits
 * with swap trades a crash for an app that runs and cannot make sound, which is not a better
 * outcome. The number here is RAM.
 *
 * ⚠️ On a 32-bit build this is capped by the address space rather than by the RAM fitted: a device
 * with 4 GB cannot hand a single process more than its ~3 GB of user address space, and the smaller
 * of the two is the one that binds.
 */
int64_t available_memory_bytes();

/** Physical memory fitted, in bytes; 0 when unknown. Same 32-bit address-space cap applies. */
int64_t total_memory_bytes();

/**
 * The budget a load is measured against — `max(available, total / 2)`, and 0 when unknown.
 *
 * ⚠️ **IT IS DELIBERATELY LARGER THAN `available_memory_bytes()`, AND THE REASON IS THE POLICY.**
 * The app REFUSES an over-budget load rather than asking, so an under-estimate does not cost a
 * dialog — it takes a file away from a user whose device could have opened it, with nothing on
 * screen to explain why.
 *
 * `available` alone under-states what a foreground app can have: Android evicts background apps to
 * feed the one in front, so much of the memory counted "in use" will be handed over.
 *
 * Half of total is not a tuned constant and is not meant to be one. It is a floor chosen so a
 * refusal only ever fires on a load that is hopeless under *any* reading of the machine, leaving
 * everything ambiguous to proceed and fail honestly if it must.
 *
 * ⚠️ **Do not "improve" this by shaving a safety margin off it.** A margin is right when the app
 * ASKS and wrong when it REFUSES, and this app refuses.
 */
int64_t load_budget_bytes();

}  // namespace pt

#endif  // POCKETTRACKER_PLATFORM_MEMORY_H
