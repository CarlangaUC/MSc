#pragma once

#include <array>
#include <cstdint>
#include <utility>

namespace edd {

// Intervalo temporal half-open [version_start, version_end).
using temporal_endpoint_type = uint64_t;
using temporal_interval = std::pair<temporal_endpoint_type, temporal_endpoint_type>;

// ---------------------------------------------------------------------------
// spot_quad: un hecho versionado (misma semántica que cltj::spot_quad, campos u64).
//
// Layout fijo de 5 enteros:
//   [0] term_id         — término del vocabulario (t). En modo per-term se reutiliza
//                         este slot para el master (el término vive fuera del quad).
//   [1] master_doc      — documento / id en el universo (u). En per-term queda en 0.
//   [2] unused_slot     — siempre 0 (hueco S/P/O de CLTJ no usado en IR).
//   [3] version_start   — τ_a inclusive (versión relativa packed64).
//   [4] version_end     — τ_b exclusive; vigencia en [τ_a, τ_b).
//
// Global:   (term_id, master_doc, 0, version_start, version_end)
// Per-term: (master_doc, 0, 0, version_start, version_end)
// ---------------------------------------------------------------------------
using spot_quad = std::array<uint64_t, 5>;

enum spot_quad_field : size_t {
    QUAD_TERM = 0,            // term_id (global) o master_doc (per-term)
    QUAD_MASTER = 1,          // master_doc (global); 0 en per-term
    QUAD_UNUSED = 2,          // siempre 0
    QUAD_VERSION_START = 3,   // τ_a
    QUAD_VERSION_END = 4,     // τ_b exclusive
};

inline spot_quad make_global_quad(uint64_t term_id, uint64_t master_doc, uint64_t version_start,
                                  uint64_t version_end) {
    return {term_id, master_doc, 0, version_start, version_end};
}

inline spot_quad make_per_term_quad(uint64_t master_doc, uint64_t version_start, uint64_t version_end) {
    return {master_doc, 0, 0, version_start, version_end};
}

}  // namespace edd
