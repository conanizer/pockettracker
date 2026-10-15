#ifndef POCKETTRACKER_PLATFORM_MEMORY_H
#define POCKETTRACKER_PLATFORM_MEMORY_H

// How much memory this machine will give us — what a load is measured against before it starts.
// ⚠️ The allocator will not say: Linux and Android overcommit and kill the process when the pages are
// touched. So a load asks first.

#include <cstdint>

namespace pt {

// RAM free for a large allocation now, in bytes, capped by a 32-bit address space.
// ⚠️ 0 means unknown, not "nothing free". Swap is not counted: audio in swap does not play.
int64_t available_memory_bytes();

// RAM fitted, in bytes; 0 when unknown. Same 32-bit cap.
int64_t total_memory_bytes();

// The budget a load is measured against: `max(available, total / 2)`, 0 when unknown.
// ⚠️ Deliberately generous — an over-budget load is refused, not asked about, so an under-estimate
// silently takes away a file the device could open. Do not shave a safety margin off it.
int64_t load_budget_bytes();

}  // namespace pt

#endif  // POCKETTRACKER_PLATFORM_MEMORY_H
