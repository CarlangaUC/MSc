#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

// Aligned with uiHRDC `version_packing.h` / `scripts/packed64_layout.py` (default 40+24).
#if __has_include("version_packing.h")
#include "version_packing.h"
#define EDD_PACKED64_FROM_HEADER 1
#else
#ifndef ZDD_MASTER_BITS
#define ZDD_MASTER_BITS 40u
#endif
#ifndef ZDD_REL_BITS
#define ZDD_REL_BITS 24u
#endif
#define EDD_PACKED64_FROM_HEADER 0
#endif

namespace edd {

inline constexpr uint32_t kMasterBits = ZDD_MASTER_BITS;
inline constexpr uint32_t kRelBits = ZDD_REL_BITS;
inline constexpr uint64_t kMasterMask =
    (kMasterBits >= 64) ? ~0ULL : ((1ULL << kMasterBits) - 1ULL);
inline constexpr uint64_t kRelMask = (kRelBits >= 64) ? ~0ULL : ((1ULL << kRelBits) - 1ULL);
inline constexpr uint64_t kMasterShift = kRelBits;

inline uint64_t unpack_master(uint64_t packed) {
    return (packed >> kMasterShift) & kMasterMask;
}

inline uint64_t unpack_relative(uint64_t packed) { return packed & kRelMask; }

inline uint64_t pack_master_rel(uint64_t master, uint64_t rel) {
    if ((master & ~kMasterMask) != 0) {
        throw std::out_of_range("master exceeds ZDD_MASTER_BITS=" + std::to_string(kMasterBits));
    }
    if ((rel & ~kRelMask) != 0) {
        throw std::out_of_range("rel exceeds ZDD_REL_BITS=" + std::to_string(kRelBits));
    }
    return (master << kMasterShift) | rel;
}

inline uint32_t bits_required_u64(uint64_t value) {
    if (value == 0) return 1;
    uint32_t bits = 0;
    while (value) {
        ++bits;
        value >>= 1;
    }
    return bits;
}

}  // namespace edd
