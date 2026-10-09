// EDD metatrie time-first sobre .docs versionados (packed64).
// Adaptado de BGPs/CLTJ: un solo trie (o uno por término), sin el grafo de 18 tries.
// Hecho versionado = spot_quad (term, master_doc, unused, version_start, version_end).

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <cstdlib>
#include <sstream>
#include <streambuf>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <sdsl/int_vector.hpp>
#include <sdsl/io.hpp>
#include <sdsl/rank_support_v.hpp>
#include <sdsl/select_support_mcl.hpp>
#include <sdsl/sdsl_concepts.hpp>

#include "edd_cltj_types.hpp"
#include "packed64_io.hpp"
#include "cltj_temporal_wm_u64.hpp"

// Misma fórmula BPI que el pipeline ZDD (plus_t).
#include "utils/bpi.h"

namespace edd {

// Count serialized bytes without materializing the buffer (many per-term WMs).
class counting_streambuf : public std::streambuf {
    std::streamsize n_ = 0;

protected:
    // Suma bytes escritos sin guardar el buffer.
    std::streamsize xsputn(const char*, std::streamsize n) override {
        n_ += n;
        return n;
    }
    // Cuenta un byte suelto que no entró por xsputn.
    int overflow(int c) override {
        if (c != traits_type::eof()) ++n_;
        return traits_type::not_eof(c);
    }

public:
    uint64_t bytes() const { return static_cast<uint64_t>(n_); }
};

struct counting_ostream : std::ostream {
    counting_streambuf buf;
    counting_ostream() : std::ostream(&buf) {}
    uint64_t bytes() const { return buf.bytes(); }
};

// Bytes que ocuparía el WM al serializarlo.
inline uint64_t wm_serialized_bytes(const temporal_wm<>& wm) {
    counting_ostream oss;
    return static_cast<uint64_t>(wm.serialize(oss));
}

struct wm_size_breakdown {
    uint64_t bytes_total = 0;
    uint64_t total() const { return bytes_total; }
};

// Par (término, documento) activo en un snapshot S^τ.
struct term_master {
    uint32_t term;    // t — id de posting list / término
    uint64_t master;  // u — documento (master id del packed64)
    bool operator<(const term_master& o) const {
        if (term != o.term) return term < o.term;
        return master < o.master;
    }
    bool operator==(const term_master& o) const { return term == o.term && master == o.master; }
};

// --- time-first trie (TIME_FIRST_FULL_INTERVALS only, from compact_trie_v3) ---

class time_first_trie {
public:
    using size_type = uint64_t;
    using value_type = uint64_t;

    struct size_breakdown {
        uint64_t bytes_tempint_left = 0;
        uint64_t bytes_tempint_right = 0;
        uint64_t bytes_last_update = 0;
        uint64_t bytes_last_select = 0;
        wm_size_breakdown wm;
        uint64_t total() const {
            return bytes_tempint_left + bytes_tempint_right + bytes_last_update + bytes_last_select + wm.total();
        }
    };

private:
    sdsl::int_vector<> m_tempint_left;   // τ_a de cada intervalo suelo
    sdsl::int_vector<> m_tempint_right;  // τ_b exclusive de cada intervalo suelo
    sdsl::bit_vector m_last_update_per_int;  // 1 en la última update de cada suelo
    sdsl::select_support_mcl<1> m_last_update_select1;
    sdsl::int_vector<> m_last_update_pos;  // prefijo p_l: índice de la última update del suelo
    temporal_wm<> m_temporal_ds;  // wavelet matrix de inserts/deletes
    size_type m_root_degree = 0;  // cantidad de intervalos suelo
    // Cuántos campos no-temporales indexa el wavelet matrix:
    //   1 = solo master_doc (modo per-term); 2 = term_id → master_doc (modo global).
    uint32_t m_n_components = 1;

    // Reengancha select1 al bitvector de last-update.
    void rebind_supports() {
        sdsl::util::init_support(m_last_update_select1, &m_last_update_per_int);
    }

    // Copia suelos, last-update y el WM.
    void copy_from(const time_first_trie& o) {
        m_tempint_left = o.m_tempint_left;
        m_tempint_right = o.m_tempint_right;
        m_last_update_per_int = o.m_last_update_per_int;
        m_last_update_pos = o.m_last_update_pos;
        m_temporal_ds = o.m_temporal_ds;
        m_root_degree = o.m_root_degree;
        m_n_components = o.m_n_components;
        rebind_supports();
    }

    // Mueve el trie y deja el origen vacío.
    void move_from(time_first_trie&& o) noexcept {
        m_tempint_left = std::move(o.m_tempint_left);
        m_tempint_right = std::move(o.m_tempint_right);
        m_last_update_per_int = std::move(o.m_last_update_per_int);
        m_last_update_pos = std::move(o.m_last_update_pos);
        m_temporal_ds = std::move(o.m_temporal_ds);
        m_root_degree = o.m_root_degree;
        m_n_components = o.m_n_components;
        o.m_root_degree = 0;
        rebind_supports();
    }

public:
    time_first_trie() = default;

    time_first_trie(const time_first_trie& o) { copy_from(o); }
    time_first_trie(time_first_trie&& o) noexcept { move_from(std::move(o)); }
    time_first_trie& operator=(const time_first_trie& o) {
        if (this != &o) copy_from(o);
        return *this;
    }
    time_first_trie& operator=(time_first_trie&& o) noexcept {
        if (this != &o) move_from(std::move(o));
        return *this;
    }
    ~time_first_trie() = default;

    // Empaqueta suelos, WM y p_l por suelo.
    time_first_trie(std::vector<temporal_interval>& interval_seq, temporal_wm<>& temp_ds, sdsl::bit_vector& last_update_bv,
                    const std::vector<size_type>& last_update_pos, uint32_t n_components)
        : m_n_components(n_components) {
        m_temporal_ds = temp_ds;
        m_last_update_per_int = last_update_bv;
        sdsl::util::init_support(m_last_update_select1, &m_last_update_per_int);
        m_last_update_pos = sdsl::int_vector<>(last_update_pos.size());
        for (size_type i = 0; i < last_update_pos.size(); ++i) m_last_update_pos[i] = last_update_pos[i];
        sdsl::util::bit_compress(m_last_update_pos);

        m_tempint_left = sdsl::int_vector<>(interval_seq.size());
        m_tempint_right = sdsl::int_vector<>(interval_seq.size());

        for (size_type i = 0; i < interval_seq.size(); ++i) {
            m_tempint_left[i] = interval_seq[i].first;
            m_tempint_right[i] = interval_seq[i].second;
        }
        sdsl::util::bit_compress(m_tempint_left);
        sdsl::util::bit_compress(m_tempint_right);
        m_root_degree = interval_seq.size();
    }

    // Cantidad de intervalos suelo.
    size_type interval_count() const { return m_root_degree; }
    // 1 = solo master_doc (per-term); 2 = (term_id, master_doc) (global).
    uint32_t n_components() const { return m_n_components; }

    // Rango raíz del WM de updates.
    std::pair<size_type, size_type> get_temporal_root() const { return m_temporal_ds.get_root(); }

    // Prefijo last_update (p_l): hasta dónde apply updates del WM en el suelo `pos`.
    size_type get_last_update_of_interval(size_type pos) const {
        if (pos >= m_last_update_pos.size()) return 0;
        return m_last_update_pos[pos];
    }

    // Intervalo suelo half-open [version_start, version_end) en la posición `pos`.
    std::pair<value_type, value_type> get_interval_at_pos(size_type pos) const {
        return std::make_pair(static_cast<value_type>(m_tempint_left[pos]),
                              static_cast<value_type>(m_tempint_right[pos]));
    }

    // Leap del WM: siguiente valor ≥ val en el componente `depth` (0=term o master, 1=master).
    size_type temporal_successor(size_type depth, std::pair<size_type, size_type>& node_interval, int64_t pos,
                                 value_type val,
                                 std::pair<std::pair<size_type, size_type>, size_type>& node_pair) {
        return m_temporal_ds.leap(depth * m_temporal_ds.get_n_bits(), node_interval.first, node_interval.second, pos,
                                  val, m_temporal_ds.get_n_bits(), node_pair);
    }

    // Dada una versión τ, obtiene (last_update_prefix p_l, raíz del WM).
    // False si no hay intervalo suelo que cubra τ o el estado está vacío.
    bool resolve_at_tau(uint32_t tau, size_type& last_update_prefix,
                        std::pair<size_type, size_type>& root) const {
        if (m_root_degree == 0) return false;
        const size_type pos = interval_seek(tau);
        if (!version_in_interval(pos, tau)) return false;
        last_update_prefix = get_last_update_of_interval(pos);
        if (last_update_prefix == std::numeric_limits<size_type>::max()) return false;
        root = get_temporal_root();
        return last_update_prefix <= root.second;
    }

    // Enumera documentos (master_doc) ≥ 0 bajo un nodo del WM en el componente `depth`,
    // aplicando updates solo hasta last_update_prefix.
    void append_masters_at(size_type depth, std::pair<size_type, size_type> node,
                           int64_t last_update_prefix, uint32_t term, std::vector<term_master>& out) {
        const value_type infinity = std::numeric_limits<value_type>::max();
        value_type cand = 0;
        while (true) {
            std::pair<std::pair<size_type, size_type>, size_type> node_pair;
            const value_type master = static_cast<value_type>(
                temporal_successor(depth, node, last_update_prefix, cand, node_pair));
            if (master == infinity) break;
            out.push_back({term, master});
            if (master == infinity - 1) break;
            cand = master + 1;
        }
    }

    // Snapshot S^τ completo: todos los (term, master) activos, o solo masters si per-term.
    std::vector<term_master> values_at_version(uint32_t tau) {
        std::vector<term_master> result;
        size_type last_update_prefix = 0;
        std::pair<size_type, size_type> root;
        if (!resolve_at_tau(tau, last_update_prefix, root)) return result;
        if (m_n_components == 1) {
            append_masters_at(0, root, static_cast<int64_t>(last_update_prefix), 0, result);
            return result;
        }
        const value_type infinity = std::numeric_limits<value_type>::max();
        value_type term_cand = 0;
        while (true) {
            std::pair<std::pair<size_type, size_type>, size_type> term_node;
            const value_type term = static_cast<value_type>(temporal_successor(
                0, root, static_cast<int64_t>(last_update_prefix), term_cand, term_node));
            if (term == infinity) break;
            append_masters_at(1, term_node.first, static_cast<int64_t>(term_node.second),
                              static_cast<uint32_t>(term), result);
            if (term == infinity - 1) break;
            term_cand = term + 1;
        }
        return result;
    }

    // Índice del suelo que contiene τ, o interval_count si no hay.
    size_type interval_seek(value_type v) const {
        if (m_root_degree == 0) return 0;
        size_type i = 0, f = m_root_degree - 1;
        const size_type orig_i = i;
        const size_type orig_f = f;
        if (m_tempint_right[f] <= v) return m_root_degree;
        if (m_tempint_left[i] > v) return i;
        while (i < f) {
            const size_type mid = (i + f) / 2;
            if (m_tempint_left[mid] < v)
                i = mid + 1;
            else
                f = mid;
        }
        if (i == orig_f && m_tempint_left[i] <= v && v < m_tempint_right[i]) return i;
        if (i > orig_i && v < m_tempint_right[i - 1]) return i - 1;
        return i;
    }

    // True si τ cae en [left, right) del suelo `pos`.
    bool version_in_interval(size_type pos, value_type v) const {
        if (pos >= m_root_degree) return false;
        return m_tempint_left[pos] <= v && v < m_tempint_right[pos];
    }

    // Texto corto de suelos y last-update para la leyenda Graphviz.
    std::string arrays_debug(uint64_t max_items = 32) const {
        std::ostringstream os;
        const uint64_t n = std::min<uint64_t>(m_root_degree, max_items);
        os << "tempint_left  = [";
        for (uint64_t i = 0; i < n; ++i) os << (i ? ", " : "") << m_tempint_left[i];
        if (n < m_root_degree) os << ", ...";
        os << "]\\ntempint_right = [";
        for (uint64_t i = 0; i < n; ++i) os << (i ? ", " : "") << m_tempint_right[i];
        if (n < m_root_degree) os << ", ...";
        os << "]\\nlast_update_pos = [";
        for (uint64_t i = 0; i < n && i < m_last_update_pos.size(); ++i)
            os << (i ? ", " : "") << m_last_update_pos[i];
        if (n < m_last_update_pos.size()) os << ", ...";
        os << "]\\nlast_update_bv (|B|=" << m_last_update_per_int.size() << ") = ";
        const uint64_t nb = std::min<uint64_t>(m_last_update_per_int.size(), max_items * 2);
        for (uint64_t i = 0; i < nb; ++i) os << m_last_update_per_int[i];
        if (nb < m_last_update_per_int.size()) os << "...";
        if (m_root_degree > 0) {
            const uint64_t last = m_root_degree - 1;
            os << "\\nultimo intervalo = [" << m_tempint_left[last] << ", " << m_tempint_right[last] << ")";
        }
        os << "\\ntemporal_wm: n_bits=" << m_temporal_ds.get_n_bits()
           << ", n_components=" << m_n_components << " (1=u, 2=(t,u))";
        return os.str();
    }

    // Tamaño para BPI (no se usa en consultas). Por defecto: solo WM serializado (B,E,rank).
    // Índice completo: descomentar los cuatro sdsl::size_in_bytes y quitar los = 0.
    size_breakdown size_bytes_breakdown() const {
        size_breakdown b;
        b.bytes_tempint_left = 0;
        b.bytes_tempint_right = 0;
        b.bytes_last_update = 0;
        b.bytes_last_select = 0;
        // b.bytes_tempint_left = sdsl::size_in_bytes(m_tempint_left);
        // b.bytes_tempint_right = sdsl::size_in_bytes(m_tempint_right);
        // b.bytes_last_update =
        //     sdsl::size_in_bytes(m_last_update_per_int) + sdsl::size_in_bytes(m_last_update_pos);
        // b.bytes_last_select = sdsl::size_in_bytes(m_last_update_select1);
        b.wm.bytes_total = wm_serialized_bytes(m_temporal_ds);
        return b;
    }

    // Serializa el trie (suelos, WM, last-update).
    size_type serialize(std::ostream& out, sdsl::structure_tree_node* v = nullptr, std::string name = "") const {
        sdsl::structure_tree_node* child = sdsl::structure_tree::add_child(v, name, sdsl::util::class_name(*this));
        size_type written = 0;
        written += sdsl::write_member(m_n_components, out, child, "n_components");
        written += sdsl::write_member(m_root_degree, out, child, "root_degree");
        written += m_tempint_left.serialize(out, child, "tempint_left");
        written += m_tempint_right.serialize(out, child, "tempint_right");
        written += m_temporal_ds.serialize(out, child, "temporal_ds");
        written += m_last_update_pos.serialize(out, child, "last_update_pos");
        written += m_last_update_per_int.serialize(out, child, "last_update");
        written += m_last_update_select1.serialize(out, child, "last_select");
        sdsl::structure_tree::add_size(child, written);
        return written;
    }

    // Carga el trie y reengancha el select de last-update.
    void load(std::istream& in) {
        sdsl::read_member(m_n_components, in);
        sdsl::read_member(m_root_degree, in);
        m_tempint_left.load(in);
        m_tempint_right.load(in);
        m_temporal_ds.load(in);
        m_last_update_pos.load(in);
        m_last_update_per_int.load(in);
        m_last_update_select1.load(in, &m_last_update_per_int);
    }
};

// --- build helpers (from cltj_build_compact_tries.hpp) ---

struct interval_endpoint_info {
    temporal_endpoint_type ep;
    bool is_right_endpoint;
    uint64_t quad;
    interval_endpoint_info(temporal_endpoint_type _endpoint, bool _is_right_endpoint, uint64_t _quad)
        : ep(_endpoint), is_right_endpoint(_is_right_endpoint), quad(_quad) {}
};

struct CompareEndpoints {
    bool operator()(const interval_endpoint_info& a, const interval_endpoint_info& b) const { return a.ep < b.ep; }
};

struct update_type {
    uint64_t tuple_index = 0;
    bool is_delete = false;
    update_type() = default;
    update_type(uint64_t _tuple_index, bool _is_delete) : tuple_index(_tuple_index), is_delete(_is_delete) {}
};

// Barrido de extremos version_start/version_end → intervalos suelo y updates insert/delete.
std::vector<std::pair<temporal_interval, std::vector<update_type>>> generate_list_of_updates(

    const std::vector<spot_quad>& D) {
    std::vector<interval_endpoint_info> endpoints;

    endpoints.reserve(2 * D.size());
    for (uint64_t i = 0; i < D.size(); ++i) {
        endpoints.emplace_back(D[i][QUAD_VERSION_START], false, i);  // start = insert
        endpoints.emplace_back(D[i][QUAD_VERSION_END], true, i);     // end = delete
    }

    if (endpoints.size() < 2) return {};
    std::sort(endpoints.begin(), endpoints.end(), CompareEndpoints());

    std::vector<std::pair<temporal_interval, std::vector<update_type>>> ground_intervals;
    for (uint64_t i = 1; i < endpoints.size();) {
        ground_intervals.emplace_back(temporal_interval(endpoints[i - 1].ep, endpoints[i].ep),
                                      std::vector<update_type>());
        auto& interval = ground_intervals.back();
        bool batch_at_left = false;
        while (i < endpoints.size() && endpoints[i].ep == endpoints[i - 1].ep) {
            batch_at_left = true;
            interval.second.emplace_back(endpoints[i - 1].quad, endpoints[i - 1].is_right_endpoint);
            ++i;
        }
        if (batch_at_left) {
            const temporal_endpoint_type right =
                (i < endpoints.size()) ? endpoints[i].ep : endpoints[i - 1].ep;
            interval.first.second = right;
            interval.second.emplace_back(endpoints[i - 1].quad, endpoints[i - 1].is_right_endpoint);
            if (i < endpoints.size()) ++i;
        } else {
            interval.second.emplace_back(endpoints[i - 1].quad, endpoints[i - 1].is_right_endpoint);
            ++i;
        }
    }
    return ground_intervals;
}

// Orden time-first del stream de updates: version_start, luego term_id, master_doc, unused.
struct comparator_time_first {
    const std::vector<spot_quad>& data;
    bool operator()(size_t a, size_t b) const {
        const spot_quad& q1 = data[a];
        const spot_quad& q2 = data[b];
        if (q1[QUAD_VERSION_START] != q2[QUAD_VERSION_START])
            return q1[QUAD_VERSION_START] < q2[QUAD_VERSION_START];
        if (q1[QUAD_TERM] != q2[QUAD_TERM]) return q1[QUAD_TERM] < q2[QUAD_TERM];
        if (q1[QUAD_MASTER] != q2[QUAD_MASTER]) return q1[QUAD_MASTER] < q2[QUAD_MASTER];
        return q1[QUAD_UNUSED] < q2[QUAD_UNUSED];
    }
};

// Arma el trie: intervalos suelo, last_update_prefix, stream ordenado y temporal_wm.
// n_components: 1 = master_doc (per-term); 2 = (term_id, master_doc) (global).
// is_partial de BGPs no se usa en este fork.
time_first_trie create_time_first_trie(std::vector<spot_quad>& D,
                                       std::vector<std::pair<temporal_interval, std::vector<update_type>>>& D_T,
                                       uint64_t n_bits, uint32_t n_components) {

    std::vector<temporal_interval> interval_list;
    uint64_t n_updates = 0;
    for (uint64_t i = 0; i < D_T.size(); ++i) {
        interval_list.push_back(D_T[i].first);
        n_updates += D_T[i].second.size();
    }
    sdsl::bit_vector last_update_per_interval(n_updates, 0);
    static constexpr uint64_t kNoUpdateState = std::numeric_limits<uint64_t>::max();
    std::vector<uint64_t> last_update_pos(D_T.size(), kNoUpdateState);
    uint64_t j = 0;
    for (uint64_t i = 0; i < D_T.size(); ++i) {
        if (!D_T[i].second.empty()) {
            j += D_T[i].second.size();
            last_update_per_interval[j - 1] = 1;
            last_update_pos[i] = j - 1;
        } else if (i > 0) {
            last_update_pos[i] = last_update_pos[i - 1];
        }
    }

    std::vector<spot_quad> new_D;
    std::vector<bool> is_delete;
    new_D.reserve(n_updates);
    is_delete.reserve(n_updates);
    for (uint64_t i = 0; i < D_T.size(); ++i) {
        for (uint64_t k = 0; k < D_T[i].second.size(); ++k) {
            const uint64_t cur_tuple = D_T[i].second[k].tuple_index;
            new_D.push_back({D[cur_tuple][QUAD_TERM], D[cur_tuple][QUAD_MASTER], D[cur_tuple][QUAD_UNUSED],
                             D_T[i].first.first, D_T[i].first.second});
            is_delete.push_back(D_T[i].second[k].is_delete);
        }
    }

    std::vector<uint64_t> idx(new_D.size());
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), comparator_time_first{new_D});

    std::vector<spot_quad> sorted_D(new_D.size());
    std::vector<bool> sorted_del(new_D.size());

    for (uint64_t i = 0; i < idx.size(); ++i) {
        sorted_D[i] = new_D[idx[i]];
        sorted_del[i] = is_delete[idx[i]];
    }
    temporal_wm<> temp_ds(sorted_D, sorted_del, n_components, n_bits, /*is_partial=*/false);
    return time_first_trie(interval_list, temp_ds, last_update_per_interval, last_update_pos, n_components);
}

// --- .docs I/O (from build-versioned-op.cpp) ---

struct docs_index {
    uint32_t nlists = 0;
    uint64_t postings = 0;
    uint64_t max_master = 0;
    uint64_t max_relative = 0;
    std::vector<uint64_t> offsets;
};

// Lee un u32 little-endian del .docs.
bool read_u32(std::istream& in, uint32_t& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(value));
    return in.good();
}

// Posting list packed64 en `offset`: [u32 len][u64...].
bool read_posting_list(std::ifstream& in, uint64_t offset, std::vector<uint64_t>& out) {
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    uint32_t length = 0;
    if (!read_u32(in, length)) return false;
    out.resize(length);
    if (length) in.read(reinterpret_cast<char*>(out.data()), sizeof(uint64_t) * length);
    return in.good();
}

// Offsets por término y máximos de master_doc / version (acota a max_terms).
docs_index inspect_docs(const std::string& path, uint32_t max_terms) {
    docs_index result;
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in.is_open() || !read_u32(in, result.nlists)) return result;
    const uint32_t limit = (max_terms && max_terms < result.nlists) ? max_terms : result.nlists;
    result.offsets.reserve(limit);
    for (uint32_t term = 0; term < limit; ++term) {
        result.offsets.push_back(static_cast<uint64_t>(in.tellg()));
        uint32_t length = 0;
        if (!read_u32(in, length)) {
            result.offsets.clear();
            return result;
        }
        result.postings += length;
        for (uint32_t i = 0; i < length; ++i) {
            uint64_t packed = 0;
            in.read(reinterpret_cast<char*>(&packed), sizeof(packed));
            if (!in.good()) {
                result.offsets.clear();
                return result;
            }
            const uint64_t master = unpack_master(packed);
            const uint64_t rel = unpack_relative(packed);
            result.max_master = std::max(result.max_master, master);
            result.max_relative = std::max(result.max_relative, rel);
        }
    }
    result.nlists = limit;
    return result;
}

struct master_version {
    uint64_t master;   // documento (u)
    uint64_t version;  // versión relativa τ del packed64
    // Orden (master, version) para el RLE.
    bool operator<(const master_version& o) const {
        if (master != o.master) return master < o.master;
        return version < o.version;
    }
    bool operator==(const master_version& o) const {
        return master == o.master && version == o.version;
    }
};

// RLE por documento: versiones consecutivas del mismo master → un quad global.
// Resultado: (term_id, master_doc, 0, version_start, version_end).
void postings_to_quads_rle(uint32_t term, const std::vector<uint64_t>& postings, std::vector<spot_quad>& out) {
    std::vector<master_version> pairs;
    pairs.reserve(postings.size());
    for (uint64_t p : postings) pairs.push_back({unpack_master(p), unpack_relative(p)});
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    size_t i = 0;
    while (i < pairs.size()) {
        const uint64_t master = pairs[i].master;
        uint64_t version_start = pairs[i].version;
        uint64_t version_prev = version_start;
        ++i;
        while (i < pairs.size() && pairs[i].master == master) {
            if (pairs[i].version == version_prev + 1) {
                version_prev = pairs[i].version;
                ++i;
                continue;
            }
            // [(1,2), (1,3), (1,4)] → [(1,2), (1,4)] = [ (doc_global, relative_version), ....]
            out.push_back(make_global_quad(term, master, version_start, version_prev + 1));
            version_start = version_prev = pairs[i].version;
            ++i;
        }
        out.push_back(make_global_quad(term, master, version_start, version_prev + 1));
    }
}

// Un quad puntual [version, version+1) por cada posting, sin fusionar.
void postings_to_quads_point(uint32_t term, const std::vector<uint64_t>& postings, std::vector<spot_quad>& out) {
    for (uint64_t p : postings) {
        const uint64_t master = unpack_master(p);
        const uint64_t version = unpack_relative(p);
        out.push_back(make_global_quad(term, master, version, version + 1));
    }
}

// RLE per-term: el término vive en el trie; el quad es (master_doc, 0, 0, version_start, version_end).
void postings_to_quads_rle_per_term(const std::vector<uint64_t>& postings, std::vector<spot_quad>& out) {
    const size_t base = out.size();
    postings_to_quads_rle(0, postings, out);
    for (size_t i = base; i < out.size(); ++i) {
        out[i] = make_per_term_quad(out[i][QUAD_MASTER], out[i][QUAD_VERSION_START], out[i][QUAD_VERSION_END]);
    }
}

// Punto per-term: (master_doc, 0, 0, version, version+1).
void postings_to_quads_point_per_term(const std::vector<uint64_t>& postings, std::vector<spot_quad>& out) {
    const size_t base = out.size();
    postings_to_quads_point(0, postings, out);
    for (size_t i = base; i < out.size(); ++i) {
        out[i] = make_per_term_quad(out[i][QUAD_MASTER], out[i][QUAD_VERSION_START], out[i][QUAD_VERSION_END]);
    }
}

enum class build_mode { global, per_term };

struct docs_metrics {
    uint64_t n_raw = 0;
    uint64_t n_pairs_uniq = 0;
    uint64_t n_snap_elems = 0;
};

// n_raw y pares (master_doc, version) únicos: denominadores del BPI.
docs_metrics scan_docs_metrics(const std::string& path, uint32_t max_terms) {
    docs_metrics m;
    std::ifstream in(path.c_str(), std::ios::binary);
    uint32_t nlists = 0;
    if (!in.is_open() || !read_u32(in, nlists)) return m;
    const uint32_t limit = (max_terms && max_terms < nlists) ? max_terms : nlists;
    std::vector<master_version> pairs;
    for (uint32_t term = 0; term < limit; ++term) {
        uint32_t length = 0;
        if (!read_u32(in, length)) break;
        m.n_raw += length;
        for (uint32_t i = 0; i < length; ++i) {
            uint64_t packed = 0;
            in.read(reinterpret_cast<char*>(&packed), sizeof(packed));
            pairs.push_back({unpack_master(packed), unpack_relative(packed)});
        }
        (void)term;
    }
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    m.n_pairs_uniq = pairs.size();
    m.n_snap_elems = m.n_pairs_uniq;
    return m;
}

struct index_size_report {
    uint64_t bytes_total = 0;
    time_first_trie::size_breakdown global_bd;
    std::vector<time_first_trie::size_breakdown> per_term_bd;
    uint64_t per_term_count = 0;
};

class meta_trie_edd {
public:
    typedef uint64_t size_type;
    build_mode mode = build_mode::global;
    time_first_trie global;
    std::vector<time_first_trie> per_term;
    uint32_t n_terms = 0;
    uint64_t raw_quads = 0;
    uint64_t update_events = 0;

    // Un trie global: quads (term, master, 0, version_start, version_end); WM con 2 componentes (term→master).
    void build_global(const std::string& docs_path, uint32_t max_terms, bool use_rle) {

        mode = build_mode::global;
        const docs_index meta = inspect_docs(docs_path, max_terms);
        if (meta.offsets.empty()) return;
        n_terms = meta.nlists;
        std::ifstream docs(docs_path.c_str(), std::ios::binary);
        std::vector<spot_quad> D;
        D.reserve(static_cast<size_t>(std::min<uint64_t>(meta.postings ? meta.postings : 5000000ull, 5000000ull)));
        std::vector<uint64_t> postings;

        const uint32_t bits =
            std::max(bits_required_u64(meta.nlists ? meta.nlists - 1 : 0), bits_required_u64(meta.max_master));

        for (uint32_t term = 0; term < meta.nlists; ++term) {
            if (!read_posting_list(docs, meta.offsets[term], postings)) continue;
            if (use_rle)
                postings_to_quads_rle(term, postings, D);
            else
                postings_to_quads_point(term, postings, D);
            if ((term + 1) % 10000u == 0u || term + 1 == meta.nlists) {
                std::cerr << "[build_global] terms=" << (term + 1) << "/" << meta.nlists
                          << " quads=" << D.size() << std::endl;
            }
        }
        raw_quads = D.size();
        if (D.empty()) return;
        std::cerr << "[build_global] generate_list_of_updates quads=" << D.size() << std::endl;
        auto D_T = generate_list_of_updates(D);
        for (const auto& p : D_T) update_events += p.second.size();
        std::cerr << "[build_global] create_time_first_trie intervals=" << D_T.size()
                  << " updates=" << update_events << std::endl;
        global = create_time_first_trie(D, D_T, bits, /*n_components=*/2);
        std::cerr << "[build_global] done\n";

        
    }

    // Un trie por término: quads (master, 0, 0, version_start, version_end); WM con 1 componente (master).
    void build_per_term(const std::string& docs_path, uint32_t max_terms, bool use_rle) {
        mode = build_mode::per_term;
        const docs_index meta = inspect_docs(docs_path, max_terms);
        if (meta.offsets.empty()) return;
        n_terms = meta.nlists;
        std::ifstream docs(docs_path.c_str(), std::ios::binary);
        per_term.clear();
        per_term.resize(meta.nlists);
        const uint32_t bits = bits_required_u64(meta.max_master);
        std::vector<uint64_t> postings;
        for (uint32_t term = 0; term < meta.nlists; ++term) {
            if (!read_posting_list(docs, meta.offsets[term], postings)) continue;
            std::vector<spot_quad> D;
            if (use_rle)
                postings_to_quads_rle_per_term(postings, D);
            else
                postings_to_quads_point_per_term(postings, D);
            raw_quads += D.size();
            if (D.empty()) continue;
            auto D_T = generate_list_of_updates(D);
            for (const auto& p : D_T) update_events += p.second.size();
            per_term[term] = create_time_first_trie(D, D_T, bits, /*n_components=*/1);
        }
    }

    // Consulta Q_τ(term, version) → S^τ: documentos activos del término en esa versión.
    // Global: leap al term_id en el WM; per-term: consulta el trie de ese término.
    std::vector<term_master> values_at(uint32_t term, uint32_t tau) {
        if (mode == build_mode::global) {
            std::vector<term_master> got;
            time_first_trie::size_type last_update_prefix = 0;
            std::pair<time_first_trie::size_type, time_first_trie::size_type> root;
            if (!global.resolve_at_tau(tau, last_update_prefix, root)) return got;
            std::pair<std::pair<time_first_trie::size_type, time_first_trie::size_type>,
                      time_first_trie::size_type>
                term_node;
            const auto term_v = static_cast<time_first_trie::value_type>(
                global.temporal_successor(0, root, static_cast<int64_t>(last_update_prefix),
                                          static_cast<time_first_trie::value_type>(term), term_node));
            if (term_v != static_cast<time_first_trie::value_type>(term)) return got;
            global.append_masters_at(1, term_node.first, static_cast<int64_t>(term_node.second), term, got);
            std::sort(got.begin(), got.end());
            return got;
        }
        if (term >= per_term.size() || per_term[term].interval_count() == 0) return {};
        std::vector<term_master> got;
        for (const term_master& row : per_term[term].values_at_version(tau)) got.push_back({term, row.master});
        std::sort(got.begin(), got.end());
        return got;
    }

    // Bytes del índice (un trie o la suma per-term).
    index_size_report size_report() const {
        index_size_report r;
        if (mode == build_mode::global) {
            r.global_bd = global.size_bytes_breakdown();
            r.bytes_total = r.global_bd.total();
        } else {
            for (const time_first_trie& t : per_term) {
                if (t.interval_count() == 0) continue;
                r.per_term_bd.push_back(t.size_bytes_breakdown());
                r.bytes_total += r.per_term_bd.back().total();
                ++r.per_term_count;
            }
        }
        return r;
    }

    // Serializa modo, contadores y el o los tries (.emt).
    size_type serialize(std::ostream& out, sdsl::structure_tree_node* v = nullptr, std::string name = "") const {
        sdsl::structure_tree_node* child = sdsl::structure_tree::add_child(v, name, sdsl::util::class_name(*this));
        uint8_t m = static_cast<uint8_t>(mode);
        size_type w = sdsl::write_member(m, out, child, "mode");
        w += sdsl::write_member(raw_quads, out, child, "raw_quads");
        w += sdsl::write_member(update_events, out, child, "update_events");
        if (mode == build_mode::global) {
            w += global.serialize(out, child, "global");
        } else {
            const uint64_t n = per_term.size();
            w += sdsl::write_member(n, out, child, "n_terms");
            for (uint64_t i = 0; i < n; ++i) w += per_term[i].serialize(out, child, "term");
        }
        sdsl::structure_tree::add_size(child, w);
        return w;
    }

    // Carga un .emt (global o vector per-term).
    void load(std::istream& in) {
        uint8_t m = 0;
        sdsl::read_member(m, in);
        sdsl::read_member(raw_quads, in);
        sdsl::read_member(update_events, in);
        mode = static_cast<build_mode>(m);
        if (mode == build_mode::global) {
            global.load(in);
        } else {
            uint64_t n = 0;
            sdsl::read_member(n, in);
            per_term.resize(n);
            for (uint64_t i = 0; i < n; ++i) per_term[i].load(in);
        }
    }
};

// DOT del trie lógico (suelos + S^τ); solo inputs chicos.
bool dump_trie_dot(time_first_trie& trie, const std::string& out_path, const std::string& title,
                   uint64_t max_intervals, uint64_t max_answer) {
    std::ofstream out(out_path.c_str());
    if (!out.is_open()) return false;
    const uint64_t n_int = trie.interval_count();
    const uint64_t shown = std::min(n_int, max_intervals);
    const bool has_term = trie.n_components() == 2;

    out << "digraph metatrie {\n";
    out << "  rankdir=TB;\n  bgcolor=\"white\";\n";
    out << "  node [fontname=\"Helvetica\", fontsize=10];\n";
    out << "  edge [fontname=\"Helvetica\", fontsize=9, color=\"#475569\"];\n";
    out << "  labelloc=\"t\";\n  label=\"" << title << "\";\n  fontsize=13;\n";

    out << "  root [label=\"time-first trie\\nraiz: " << n_int
        << " intervalos suelo\", shape=box, style=\"filled,rounded\", fillcolor=\"#1e3a8a\", "
           "fontcolor=\"white\"];\n";

    for (uint64_t i = 0; i < shown; ++i) {
        const std::pair<uint32_t, uint32_t> iv = trie.get_interval_at_pos(i);
        const uint64_t lu = trie.get_last_update_of_interval(i);
        out << "  I" << i << " [label=\"[" << iv.first << ", " << iv.second << ")\\nlast_update(p_l)=" << lu
            << "\", shape=box, style=filled, fillcolor=\"#bfdbfe\", color=\"#1d4ed8\"];\n";
        out << "  root -> I" << i << " [label=\"" << i << "\"];\n";

        std::vector<term_master> snap = trie.values_at_version(iv.first);
        if (snap.empty()) {
            out << "  E" << i << " [label=\"(vacio)\", shape=plaintext, fontcolor=\"#94a3b8\"];\n";
            out << "  I" << i << " -> E" << i << ";\n";
            continue;
        }
        const uint64_t p_shown = std::min<uint64_t>(snap.size(), max_answer);
        if (has_term) {
            uint64_t k = 0;
            while (k < p_shown) {
                const uint32_t term = snap[k].term;
                out << "  T" << i << "_" << term << " [label=\"t=" << term
                    << "\", shape=box, style=filled, fillcolor=\"#fef3c7\", color=\"#d97706\"];\n";
                out << "  I" << i << " -> T" << i << "_" << term << ";\n";
                while (k < p_shown && snap[k].term == term) {
                    out << "  M" << i << "_" << term << "_" << snap[k].master << " [label=\"u="
                        << snap[k].master << "\", shape=ellipse, style=filled, fillcolor=\"#dcfce7\", "
                           "color=\"#15803d\"];\n";
                    out << "  T" << i << "_" << term << " -> M" << i << "_" << term << "_" << snap[k].master
                        << ";\n";
                    ++k;
                }
            }
        } else {
            for (uint64_t k = 0; k < p_shown; ++k) {
                out << "  M" << i << "_" << snap[k].master << " [label=\"u=" << snap[k].master
                    << "\", shape=ellipse, style=filled, fillcolor=\"#dcfce7\", color=\"#15803d\"];\n";
                out << "  I" << i << " -> M" << i << "_" << snap[k].master << ";\n";
            }
        }
        if (p_shown < snap.size()) {
            out << "  X" << i << " [label=\"... +" << (snap.size() - p_shown)
                << "\", shape=plaintext, fontcolor=\"#94a3b8\"];\n";
            out << "  I" << i << " -> X" << i << ";\n";
        }
    }
    if (shown < n_int) {
        out << "  more [label=\"... +" << (n_int - shown)
            << " intervalos\", shape=plaintext, fontcolor=\"#94a3b8\"];\n";
        out << "  root -> more;\n";
    }

    const time_first_trie::size_breakdown bd = trie.size_bytes_breakdown();
    out << "  legend [shape=note, style=filled, fillcolor=\"#f1f5f9\", color=\"#64748b\", "
           "fontsize=9, label=\""
        << trie.arrays_debug() << "\\n---\\nbytes trie=" << bd.total() << " (wm=" << bd.wm.bytes_total
        << ", tempint=" << (bd.bytes_tempint_left + bd.bytes_tempint_right) << ")\"];\n";
    out << "  { rank=sink; legend; }\n";
    out << "}\n";
    return true;
}

// stdout BPI tras build. bytes_total = suma size_bytes_breakdown (hoy ≈ solo WM).
// Ver: meta_trie_edd <global|per-term> file.docs [--validate-terms N] [--csv path]
void print_size_report(const index_size_report& rep, const docs_metrics& dm, build_mode mode) {
    std::cout << "mode=" << (mode == build_mode::global ? "global" : "per-term") << "\n";
    std::cout << "bytes_total=" << rep.bytes_total << "\n";
    std::cout << "n_raw=" << dm.n_raw << "\n";
    std::cout << "n_pairs_uniq=" << dm.n_pairs_uniq << "\n";
    std::cout << "n_snap_elems=" << dm.n_snap_elems << "\n";
    // NzddBpi::bpiFromBytes (plus_t/utils/bpi.h): (bytes_total×8)/n. Canónico: bpi_file / n_raw.

    std::cout << std::setprecision(8) << "bpi_file=" << NzddBpi::bpiFromBytes(rep.bytes_total, dm.n_raw)
              << "\n";
    std::cout << "bpi_over_pairs=" << NzddBpi::bpiFromBytes(rep.bytes_total, dm.n_pairs_uniq) << "\n";
    std::cout << "bpi_over_stored=" << NzddBpi::bpiFromBytes(rep.bytes_total, dm.n_snap_elems) << "\n";
    std::cout << "bpi_total=" << NzddBpi::bpiFromBytes(rep.bytes_total, dm.n_raw) << "\n";
    if (mode == build_mode::global) {
        const auto& b = rep.global_bd;
        std::cout << "bytes_wm_total=" << b.wm.bytes_total << "\n";
        std::cout << "bytes_tempint=" << (b.bytes_tempint_left + b.bytes_tempint_right) << "\n";
    } else {
        std::cout << "per_term_built=" << rep.per_term_count << "\n";
    }
}

// Versiones de borde (y vecinas ±1) alrededor de cada posting, para validar snapshots.
std::vector<uint32_t> validation_times(const std::vector<uint64_t>& postings) {
    std::vector<uint32_t> times;
    times.push_back(0);
    uint32_t max_rel = 0;
    for (uint64_t packed : postings) {
        const uint32_t rel = unpack_relative(packed);
        max_rel = std::max(max_rel, rel);
        times.push_back(rel);
        if (rel != std::numeric_limits<uint32_t>::max()) times.push_back(rel + 1);
        if (rel > 0) times.push_back(rel - 1);
    }
    if (max_rel < std::numeric_limits<uint32_t>::max() - 2) times.push_back(max_rel + 2);
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    return times;
}

// Documentos activos en `version` según el RLE del .docs (ground truth).
std::vector<uint64_t> expected_masters(const std::vector<uint64_t>& postings, uint32_t version) {
    std::vector<master_version> pairs;
    for (uint64_t p : postings) pairs.push_back({unpack_master(p), unpack_relative(p)});
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    std::vector<uint64_t> masters;
    size_t i = 0;
    const uint64_t ver = version;
    while (i < pairs.size()) {
        const uint64_t master = pairs[i].master;
        uint64_t version_start = pairs[i].version;
        uint64_t version_prev = version_start;
        ++i;
        while (i < pairs.size() && pairs[i].master == master) {
            if (pairs[i].version == version_prev + 1) {
                version_prev = pairs[i].version;
                ++i;
                continue;
            }
            if (version_start <= ver && ver <= version_prev) masters.push_back(master);
            version_start = version_prev = pairs[i].version;
            ++i;
        }
        if (version_start <= ver && ver <= version_prev) masters.push_back(master);
    }
    std::sort(masters.begin(), masters.end());
    masters.erase(std::unique(masters.begin(), masters.end()), masters.end());
    return masters;
}

// Ground truth como pares (term, master) activos en `version`.
static std::vector<term_master> expected_term_masters_at(const std::vector<uint64_t>& postings, uint32_t term,
                                                        uint32_t version) {
    std::vector<term_master> exp;
    for (uint64_t m : expected_masters(postings, version)) exp.push_back({term, m});
    std::sort(exp.begin(), exp.end());
    return exp;
}

// Imprime un mismatch (term, version) got vs esperado.
static void print_mismatch(uint32_t term, uint32_t version, const std::vector<term_master>& got,
                           const std::vector<term_master>& exp) {
    std::cerr << "MISMATCH term=" << term << " version=" << version << " got={";
    for (size_t i = 0; i < got.size(); ++i) std::cerr << (i ? "," : "") << got[i].master;
    std::cerr << "} exp={";
    for (size_t i = 0; i < exp.size(); ++i) std::cerr << (i ? "," : "") << exp[i].master;
    std::cerr << "}\n";
}

// Compara values_at contra el .docs en los primeros K términos.
uint64_t validate_index(meta_trie_edd& index, const std::string& docs_path, const docs_index& meta,
                        uint32_t validate_terms, build_mode mode, uint32_t debug_limit) {
    uint64_t mismatches = 0;
    std::ifstream docs(docs_path.c_str(), std::ios::binary);
    std::vector<uint64_t> postings;
    const uint32_t limit = std::min(validate_terms, meta.nlists);
    for (uint32_t term = 0; term < limit; ++term) {
        if (!read_posting_list(docs, meta.offsets[term], postings)) {
            ++mismatches;
            continue;
        }
        const std::vector<uint32_t> times = validation_times(postings);
        for (uint32_t version : times) {
            const std::vector<term_master> exp = expected_term_masters_at(postings, term, version);
            const std::vector<term_master> got = index.values_at(term, version);
            if (got != exp) {
                if (mismatches < debug_limit) print_mismatch(term, version, got, exp);
                ++mismatches;
            }
        }
    }
    (void)mode;
    return mismatches;
}

// Consulta Q_τ: término t en versión τ.
struct term_version_query {
    uint32_t term = 0;     // t — id de posting list
    uint32_t version = 0;  // τ — versión relativa a consultar
};

struct bench_result {
    uint64_t n_queries = 0;
    uint64_t reps = 0;
    uint64_t mismatches = 0;
    uint64_t total_answer_size = 0;
    double docs_scan_s = 0.0;
    double metatrie_s = 0.0;
};

// Arma hasta n_queries consultas (term, version) en round-robin:
// cicla términos 0..term_limit-1 y, en cada pasada, avanza al siguiente tiempo
// de validación de ese término (evita sesgar el bench a los primeros términos).
std::vector<term_version_query> sample_queries(const std::string& docs_path, const docs_index& meta,
                                       uint32_t term_limit, uint32_t n_queries) {
    std::vector<term_version_query> out;
    if (n_queries == 0 || term_limit == 0) return out;
    out.reserve(n_queries);
    std::ifstream docs(docs_path.c_str(), std::ios::binary);
    std::vector<std::vector<uint32_t>> times_per_term(term_limit);
    std::vector<uint64_t> postings;
    for (uint32_t term = 0; term < term_limit; ++term) {
        if (!read_posting_list(docs, meta.offsets[term], postings)) continue;
        times_per_term[term] = validation_times(postings);
    }
    uint64_t guard = 0;
    while (out.size() < n_queries && guard < static_cast<uint64_t>(n_queries) * 8u) {
        const uint32_t term = static_cast<uint32_t>(out.size() % term_limit);
        const auto& times = times_per_term[term];
        if (!times.empty()) {
            const uint32_t version = times[(out.size() / term_limit) % times.size()];
            out.push_back({term, version});
        }
        ++guard;
        if (times.empty() && term + 1 >= term_limit && out.empty()) break;
    }
    return out;
}

// Cronometra docs_scan vs values_at y cuenta mismatches.
bench_result run_query_bench(meta_trie_edd& index, const std::string& docs_path, const docs_index& meta,
                             const std::vector<term_version_query>& queries, uint32_t reps) {
    bench_result br;
    br.n_queries = queries.size();
    br.reps = reps == 0 ? 1 : reps;
    if (queries.empty()) return br;

    uint32_t max_term = 0;
    for (const term_version_query& q : queries) max_term = std::max(max_term, q.term);
    const uint32_t term_limit = std::min(max_term + 1, meta.nlists);

    std::ifstream docs(docs_path.c_str(), std::ios::binary);
    std::vector<std::vector<uint64_t>> postings_by_term(term_limit);
    for (uint32_t t = 0; t < term_limit; ++t) {
        if (!read_posting_list(docs, meta.offsets[t], postings_by_term[t])) {
            postings_by_term[t].clear();
        }
    }

    // Warm-up (untimed): touch both paths once.
    for (const term_version_query& q : queries) {
        if (q.term >= term_limit) continue;
        (void)expected_masters(postings_by_term[q.term], q.version);
        (void)index.values_at(q.term, q.version);
    }

    const auto t_docs0 = std::chrono::steady_clock::now();
    for (uint32_t r = 0; r < br.reps; ++r) {
        for (const term_version_query& q : queries) {
            if (q.term >= term_limit) continue;
            const std::vector<uint64_t> got = expected_masters(postings_by_term[q.term], q.version);
            br.total_answer_size += got.size();
        }
    }
    br.docs_scan_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_docs0).count();

    const auto t_mt0 = std::chrono::steady_clock::now();
    for (uint32_t r = 0; r < br.reps; ++r) {
        for (const term_version_query& q : queries) {
            const std::vector<term_master> got = index.values_at(q.term, q.version);
            if (r == 0) {
                if (q.term >= term_limit) {
                    ++br.mismatches;
                    continue;
                }
                const std::vector<uint64_t> exp = expected_masters(postings_by_term[q.term], q.version);
                if (got.size() != exp.size()) {
                    ++br.mismatches;
                } else {
                    for (size_t i = 0; i < got.size(); ++i) {
                        if (got[i].master != exp[i]) {
                            ++br.mismatches;
                            break;
                        }
                    }
                }
            }
        }
    }
    br.metatrie_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_mt0).count();
    // total_answer_size counted only on docs path × reps; normalize to one pass for reporting.
    if (br.reps > 0) br.total_answer_size /= br.reps;
    return br;
}

struct cli_options {
    build_mode mode = build_mode::global;
    std::string docs_path;
    uint32_t max_terms = 0;
    uint32_t validate_terms = 100;
    bool use_rle = true;
    std::string csv_path;
    std::string serialize_path;
    std::string dump_dot_path;
    uint32_t dump_term = 0;
    uint64_t dump_max_intervals = 40;
    uint64_t dump_max_answer = 16;
    uint32_t debug_mismatches = 0;
    uint32_t bench_queries = 0;
    uint32_t bench_reps = 5;
    std::string bench_csv_path;
    std::string bench_queries_out;
};

// CLI: modo, .docs y flags de build, validación, bench y dump.
bool parse_args(int argc, char** argv, cli_options& opt) {
    if (argc < 3) return false;
    const std::string mode_str = argv[1];
    if (mode_str == "global")
        opt.mode = build_mode::global;
    else if (mode_str == "per-term")
        opt.mode = build_mode::per_term;
    else
        return false;
    opt.docs_path = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--no-rle") {
            opt.use_rle = false;
        } else if (a == "--max-terms" && i + 1 < argc) {
            opt.max_terms = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (a == "--validate-terms" && i + 1 < argc) {
            opt.validate_terms = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (a == "--csv" && i + 1 < argc) {
            opt.csv_path = argv[++i];
        } else if (a == "--serialize" && i + 1 < argc) {
            opt.serialize_path = argv[++i];
        } else if (a == "--dump-dot" && i + 1 < argc) {
            opt.dump_dot_path = argv[++i];
        } else if (a == "--dump-term" && i + 1 < argc) {
            opt.dump_term = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (a == "--dump-max-intervals" && i + 1 < argc) {
            opt.dump_max_intervals = std::strtoull(argv[++i], nullptr, 10);
        } else if ((a == "--dump-max-answer" || a == "--dump-max-payload") && i + 1 < argc) {
            opt.dump_max_answer = std::strtoull(argv[++i], nullptr, 10);
        } else if (a == "--debug-mismatches" && i + 1 < argc) {
            opt.debug_mismatches = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (a == "--bench-queries" && i + 1 < argc) {
            opt.bench_queries = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (a == "--bench-reps" && i + 1 < argc) {
            opt.bench_reps = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (a == "--bench-csv" && i + 1 < argc) {
            opt.bench_csv_path = argv[++i];
        } else if (a == "--bench-queries-out" && i + 1 < argc) {
            opt.bench_queries_out = argv[++i];
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace edd

// Build, métricas, validacióo y bench/dump/serialize opcionales.
int main(int argc, char** argv) {
    edd::cli_options opt;
    if (!edd::parse_args(argc, argv, opt)) {
        std::cerr << "Usage: " << argv[0]
                  << " <global|per-term> <input.docs> [--max-terms N] [--validate-terms N] [--no-rle] [--csv path] "
                     "[--serialize out.emt] [--dump-dot out.dot [--dump-term T] [--dump-max-intervals N] "
                     "[--dump-max-answer N]] [--debug-mismatches N] "
                     "[--bench-queries N [--bench-reps R] [--bench-csv path] [--bench-queries-out path]]\n";
        return 1;
    }

    const edd::docs_index meta = edd::inspect_docs(opt.docs_path, opt.max_terms);
    if (meta.offsets.empty()) {
        std::cerr << "ERROR: invalid or empty .docs\n";
        return 1;
    }

    edd::meta_trie_edd index;
    const auto t0 = std::chrono::steady_clock::now();

    if (opt.mode == edd::build_mode::global)
        index.build_global(opt.docs_path, opt.max_terms, opt.use_rle);
    else
        index.build_per_term(opt.docs_path, opt.max_terms, opt.use_rle);

    const double build_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const edd::docs_metrics dm = edd::scan_docs_metrics(opt.docs_path, opt.max_terms);
    const edd::index_size_report rep = index.size_report();
    edd::print_size_report(rep, dm, opt.mode);


    std::cout << "raw_quads=" << index.raw_quads << "\n";
    std::cout << "update_events=" << index.update_events << "\n";
    std::cout << "build_s=" << build_s << "\n";

    const uint64_t mismatches =
        edd::validate_index(index, opt.docs_path, meta, opt.validate_terms, opt.mode, opt.debug_mismatches);
    std::cout << "validation_mismatches=" << mismatches << "\n";
    if (mismatches != 0) return 2;

    if (opt.bench_queries > 0) {
        const uint32_t term_cap =
            std::min(opt.validate_terms == 0 ? meta.nlists : opt.validate_terms, meta.nlists);
        const std::vector<edd::term_version_query> queries =
            edd::sample_queries(opt.docs_path, meta, term_cap, opt.bench_queries);
        const edd::bench_result br =
            edd::run_query_bench(index, opt.docs_path, meta, queries, opt.bench_reps);
        const double n_ops = static_cast<double>(br.n_queries) * static_cast<double>(br.reps);
        const double docs_ns = (n_ops > 0.0) ? (br.docs_scan_s * 1e9 / n_ops) : 0.0;
        const double mt_ns = (n_ops > 0.0) ? (br.metatrie_s * 1e9 / n_ops) : 0.0;
        std::cout << "bench_n_queries=" << br.n_queries << "\n";
        std::cout << "bench_reps=" << br.reps << "\n";
        std::cout << "bench_mismatches=" << br.mismatches << "\n";
        std::cout << "bench_avg_answer_size="
                  << (br.n_queries ? (static_cast<double>(br.total_answer_size) / br.n_queries) : 0.0)
                  << "\n";
        std::cout << "bench_docs_scan_s=" << br.docs_scan_s << "\n";
        std::cout << "bench_metatrie_s=" << br.metatrie_s << "\n";
        std::cout << "bench_docs_scan_ns_per_query=" << docs_ns << "\n";
        std::cout << "bench_metatrie_ns_per_query=" << mt_ns << "\n";

        if (!opt.bench_queries_out.empty()) {
            std::ofstream qf(opt.bench_queries_out.c_str());
            qf << "term,rel\n";
            for (const edd::term_version_query& q : queries) qf << q.term << ',' << q.version << '\n';
            std::cout << "bench_queries_out=" << opt.bench_queries_out << "\n";
        }
        if (!opt.bench_csv_path.empty()) {
            std::ofstream csv(opt.bench_csv_path.c_str());
            csv << "structure,mode,docs,n_queries,reps,mismatches,avg_answer_size,"
                   "total_s,ns_per_query\n";
            const char* mode_s = (opt.mode == edd::build_mode::global ? "global" : "per-term");
            csv << "docs_scan," << mode_s << ',' << opt.docs_path << ',' << br.n_queries << ','
                << br.reps << ',' << br.mismatches << ','
                << (br.n_queries ? (static_cast<double>(br.total_answer_size) / br.n_queries) : 0.0)
                << ',' << br.docs_scan_s << ',' << docs_ns << '\n';
            csv << "metatrie," << mode_s << ',' << opt.docs_path << ',' << br.n_queries << ','
                << br.reps << ',' << br.mismatches << ','
                << (br.n_queries ? (static_cast<double>(br.total_answer_size) / br.n_queries) : 0.0)
                << ',' << br.metatrie_s << ',' << mt_ns << '\n';
            std::cout << "bench_csv=" << opt.bench_csv_path << "\n";
        }
        if (br.mismatches != 0) return 3;
    }

    if (!opt.dump_dot_path.empty()) {
        std::ostringstream title;
        bool ok = false;
        if (opt.mode == edd::build_mode::global) {
            title << "EDD metatrie (global) — S^τ = (t,u) — " << opt.docs_path;
            ok = edd::dump_trie_dot(index.global, opt.dump_dot_path, title.str(), opt.dump_max_intervals,
                                    opt.dump_max_answer);
        } else if (opt.dump_term < index.per_term.size()) {
            title << "EDD metatrie (per-term) — t=" << opt.dump_term << " — " << opt.docs_path;
            ok = edd::dump_trie_dot(index.per_term[opt.dump_term], opt.dump_dot_path, title.str(),
                                    opt.dump_max_intervals, opt.dump_max_answer);
        } else {
            std::cerr << "ERROR: --dump-term " << opt.dump_term << " fuera de rango\n";
            return 1;
        }
        if (!ok) {
            std::cerr << "ERROR writing " << opt.dump_dot_path << "\n";
            return 1;
        }
        std::cout << "dumped_dot=" << opt.dump_dot_path << "\n";
    }

    if (!opt.serialize_path.empty()) {
        if (!sdsl::store_to_file(index, opt.serialize_path)) {
            std::cerr << "ERROR writing " << opt.serialize_path << "\n";
            return 1;
        }
        std::cout << "serialized=" << opt.serialize_path << "\n";
    }

    if (!opt.csv_path.empty()) {
        std::ofstream csv(opt.csv_path.c_str());
        csv << "mode,bytes_total,n_raw,bpi_file,bpi_over_pairs,bpi_over_stored,bpi_total,build_s\n";
        const double bpi_file = NzddBpi::bpiFromBytes(rep.bytes_total, dm.n_raw);
        csv << (opt.mode == edd::build_mode::global ? "global" : "per-term") << ','
            << rep.bytes_total << ',' << dm.n_raw << ',' << std::setprecision(8) << bpi_file << ','
            << NzddBpi::bpiFromBytes(rep.bytes_total, dm.n_pairs_uniq) << ','
            << NzddBpi::bpiFromBytes(rep.bytes_total, dm.n_snap_elems) << ',' << bpi_file << ',' << build_s
            << '\n';
    }
    return 0;
}
