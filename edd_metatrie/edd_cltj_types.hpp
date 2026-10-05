#pragma once

#include <array>
#include <cstdint>
#include <utility>

namespace edd {

// Same semantics as cltj::spot_quad, but 64-bit fields (masters up to 40-bit layout).
using spot_quad = std::array<uint64_t, 5>;
using temporal_endpoint_type = uint64_t;
using temporal_interval = std::pair<temporal_endpoint_type, temporal_endpoint_type>;

}  // namespace edd
