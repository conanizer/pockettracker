// PocketTracker's, not upstream's. MSVC has no __attribute__, which stmlib/utils/dsp.h puts on its
// inline functions (always_inline). Included ahead of the vendored headers, on MSVC only, so they need
// no change.
#ifndef PT_VENDOR_BRAIDS_MSVC_H_
#define PT_VENDOR_BRAIDS_MSVC_H_

#if defined(_MSC_VER) && !defined(__clang__)
#define __attribute__(x)
#endif

#endif  // PT_VENDOR_BRAIDS_MSVC_H_
