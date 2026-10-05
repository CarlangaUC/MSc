// EDD time-first metatrie for versioned packed64 .docs (adapted from BGPs CLTJ, no 18-trie graph).

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

namespace edd {

// Count serialized bytes without materializing the buffer (many per-term WMs).
class counting_streambuf : public std::streambuf {
    std::streamsize n_ = 0;

protected:
    std::streamsize xsputn(const char*, std::streamsize n) override {
        n_ += n;
        return n;
    }
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

inline uint64_t wm_serialized_bytes(const temporal_wm<>& wm) {
    counting_ostream oss;
    return static_cast<uint64_t>(wm.serialize(oss));
}

struct wm_size_breakdown {
    uint64_t bytes_total = 0;
    uint64_t total() const { return bytes_total; }
};

struct triple_tm {
    uint32_t term;
    uint64_t master;
    bool operator<(const triple_tm& o) const {
        if (term != o.term) return term < o.term;
        return master < o.master;
    }
    bool operator==(const triple_tm& o) const { return term == o.term && master == o.master; }
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
    sdsl::int_vector<> m_tempint_left;
    sdsl::int_vector<> m_tempint_right;
    sdsl::bit_vector m_last_update_per_int;
    sdsl::select_support_mcl<1> m_last_update_select1;
    sdsl::int_vector<> m_last_update_pos;
    temporal_wm<> m_temporal_ds;
    size_type m_root_degree = 0;
    uint32_t m_payload_components = 1;

    void rebind_supports() {
        sdsl::util::init_support(m_last_update_select1, &m_last_update_per_int);
    }

    void copy_from(const time_first_trie& o) {
        m_tempint_left = o.m_tempint_left;
        m_tempint_right = o.m_tempint_right;
        m_last_update_per_int = o.m_last_update_per_int;
        m_last_update_pos = o.m_last_update_pos;
        m_temporal_ds = o.m_temporal_ds;
        m_root_degree = o.m_root_degree;
        m_payload_components = o.m_payload_components;
        rebind_supports();
    }

    void move_from(time_first_trie&& o) noexcept {
        m_tempint_left = std::move(o.m_tempint_left);
        m_tempint_right = std::move(o.m_tempint_right);
        m_last_update_per_int = std::move(o.m_last_update_per_int);
        m_last_update_pos = std::move(o.m_last_update_pos);
        m_temporal_ds = std::move(o.m_temporal_ds);
        m_root_degree = o.m_root_degree;
        m_payload_components = o.m_payload_components;
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

    time_first_trie(std::vector<temporal_interval>& interval_seq, temporal_wm<>& temp_ds, sdsl::bit_vector& last_update_bv,
                    const std::vector<size_type>& last_update_pos, uint32_t payload_components)
        : m_payload_components(payload_components) {
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

    size_type root_degree() const { return m_root_degree > 0 ? m_root_degree - 1 : 0; }
    size_type interval_count() const { return m_root_degree; }
    uint32_t payload_components() const { return m_payload_components; }

    std::pair<size_type, size_type> get_temporal_root() const { return m_temporal_ds.get_root(); }

    size_type get_last_update_of_interval(size_type pos) const {
        if (pos >= m_last_update_pos.size()) return 0;
        return m_last_update_pos[pos];
    }

    std::pair<value_type, value_type> get_interval_at_pos(size_type pos) const {
        return std::make_pair(static_cast<value_type>(m_tempint_left[pos]),
                              static_cast<value_type>(m_tempint_right[pos]));
    }

    size_type temporal_successor(size_type depth, std::pair<size_type, size_type>& node_interval, int64_t pos,
                                 value_type val,
                                 std::pair<std::pair<size_type, size_type>, size_type>& node_pair) {
        return m_temporal_ds.leap(depth * m_temporal_ds.get_n_bits(), node_interval.first, node_interval.second, pos,
                                  val, m_temporal_ds.get_n_bits(), node_pair);
    }

    uint32_t wm_n_bits() const { return static_cast<uint32_t>(m_temporal_ds.get_n_bits()); }

    // Active payload at integer version (half-open ground interval membership).
    std::vector<triple_tm> values_at_version(uint32_t version) {
        std::vector<triple_tm> result;
        if (m_root_degree == 0) return result;
        const size_type pos = interval_seek(version);
        if (!version_in_interval(static_cast<value_type>(pos), version)) return result;
        const size_type l_update = get_last_update_of_interval(pos);
        if (l_update == std::numeric_limits<size_type>::max()) return result;
        std::pair<size_type, size_type> cur_node = get_temporal_root();
        if (l_update > cur_node.second) return result;
        const value_type infinity = std::numeric_limits<value_type>::max();
        if (m_payload_components == 1) {
            value_type cand = 0;
            while (true) {
                std::pair<std::pair<size_type, size_type>, size_type> node_pair;
                const value_type master = static_cast<value_type>(
                    temporal_successor(0, cur_node, static_cast<int64_t>(l_update), cand, node_pair));
                if (master == infinity) break;
                result.push_back({0, master});
                if (master == infinity - 1) break;
                cand = master + 1;
            }
        } else {
            value_type term_cand = 0;
            while (true) {
                std::pair<std::pair<size_type, size_type>, size_type> first_node;
                const value_type term = static_cast<value_type>(
                    temporal_successor(0, cur_node, static_cast<int64_t>(l_update), term_cand, first_node));
                if (term == infinity) break;
                value_type master_cand = 0;
                while (true) {
                    std::pair<std::pair<size_type, size_type>, size_type> second_node;
                    const value_type master = static_cast<value_type>(temporal_successor(
                        1, first_node.first, static_cast<int64_t>(first_node.second), master_cand, second_node));
                    if (master == infinity) break;
                    result.push_back({static_cast<uint32_t>(term), master});
                    if (master == infinity - 1) break;
                    master_cand = master + 1;
                }
                if (term == infinity - 1) break;
                term_cand = term + 1;
            }
        }
        return result;
    }

    // Half-open [left,right): find ground interval index containing version v, or interval_count if none.
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

    bool version_in_interval(size_type pos, value_type v) const {
        if (pos >= m_root_degree) return false;
        return m_tempint_left[pos] <= v && v < m_tempint_right[pos];
    }

    // Compact arrays behind the logical trie (for the Graphviz legend).
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
           << ", payload_components=" << m_payload_components;
        return os.str();
    }

    size_breakdown size_bytes_breakdown() const {
        size_breakdown b;
        b.bytes_tempint_left = sdsl::size_in_bytes(m_tempint_left);
        b.bytes_tempint_right = sdsl::size_in_bytes(m_tempint_right);
        b.bytes_last_update = sdsl::size_in_bytes(m_last_update_per_int) + sdsl::size_in_bytes(m_last_update_pos);
        b.bytes_last_select = sdsl::size_in_bytes(m_last_update_select1);
        b.wm.bytes_total = wm_serialized_bytes(m_temporal_ds);
        return b;
    }

    size_type serialize(std::ostream& out, sdsl::structure_tree_node* v = nullptr, std::string name = "") const {
        sdsl::structure_tree_node* child = sdsl::structure_tree::add_child(v, name, sdsl::util::class_name(*this));
        size_type written = 0;
        written += sdsl::write_member(m_payload_components, out, child, "payload_components");
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

    void load(std::istream& in) {
        sdsl::read_member(m_payload_components, in);
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

// Line sweep on interval endpoints -> ground intervals and insert/delete updates.
// Same logic as cltj_build_compact_tries.hpp, with bounds check when the last
// batch of equal endpoints falls at the maximum coordinate (OOB on endpoints[i]).
std::vector<std::pair<temporal_interval, std::vector<update_type>>> generate_list_of_updates(
    const std::vector<spot_quad>& D) {
    std::vector<interval_endpoint_info> endpoints;
    endpoints.reserve(2 * D.size());
    for (uint64_t i = 0; i < D.size(); ++i) {
        endpoints.emplace_back(D[i][3], false, i);
        endpoints.emplace_back(D[i][4], true, i);
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

struct comparator_time_first_payload {
    const std::vector<spot_quad>& data;
    bool operator()(size_t a, size_t b) const {
        const spot_quad& t1 = data[a];
        const spot_quad& t2 = data[b];
        if (t1[3] != t2[3]) return t1[3] < t2[3];
        if (t1[0] != t2[0]) return t1[0] < t2[0];
        if (t1[1] != t2[1]) return t1[1] < t2[1];
        return t1[2] < t2[2];
    }
};

// Build time-first trie from ground intervals and sorted update stream.
time_first_trie create_time_first_trie(std::vector<spot_quad>& D,
                                       std::vector<std::pair<temporal_interval, std::vector<update_type>>>& D_T,
                                       uint64_t n_bits, uint64_t n_tuple_components, bool is_partial,
                                       uint32_t payload_components) {
    std::vector<temporal_interval> interval_list;
    sdsl::bit_vector last_update_per_interval;
    uint64_t n_updates = 0;
    for (uint64_t i = 0; i < D_T.size(); ++i) {
        interval_list.push_back(D_T[i].first);
        n_updates += D_T[i].second.size();
    }
    last_update_per_interval = sdsl::bit_vector(n_updates, 0);
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
    for (uint64_t i = 0; i < D_T.size(); ++i) {
        for (uint64_t k = 0; k < D_T[i].second.size(); ++k) {
            uint64_t cur_tuple = D_T[i].second[k].tuple_index;
            new_D.push_back({D[cur_tuple][0], D[cur_tuple][1], D[cur_tuple][2], D_T[i].first.first, D_T[i].first.second});
            is_delete.push_back(D_T[i].second[k].is_delete);
        }
    }

    std::vector<uint64_t> idx(new_D.size());
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), comparator_time_first_payload{new_D});
    std::vector<spot_quad> sorted_D(new_D.size());
    std::vector<bool> sorted_del(new_D.size());
    for (uint64_t i = 0; i < idx.size(); ++i) {
        sorted_D[i] = new_D[idx[i]];
        sorted_del[i] = is_delete[idx[i]];
    }
    temporal_wm<> temp_ds(sorted_D, sorted_del, n_tuple_components, n_bits, is_partial);
    return time_first_trie(interval_list, temp_ds, last_update_per_interval, last_update_pos, payload_components);
}

// --- .docs I/O (from build-versioned-op.cpp) ---

struct docs_index {
    uint32_t nlists = 0;
    uint64_t postings = 0;
    uint64_t max_master = 0;
    uint64_t max_relative = 0;
    std::vector<uint64_t> offsets;
};

bool read_u32(std::istream& in, uint32_t& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(value));
    return in.good();
}

bool read_posting_list(std::ifstream& in, uint64_t offset, std::vector<uint64_t>& out) {
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    uint32_t length = 0;
    if (!read_u32(in, length)) return false;
    out.resize(length);
    if (length) in.read(reinterpret_cast<char*>(out.data()), sizeof(uint64_t) * length);
    return in.good();
}

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

struct pair_mr {
    uint64_t master;
    uint64_t rel;
    bool operator<(const pair_mr& o) const {
        if (master != o.master) return master < o.master;
        return rel < o.rel;
    }
    bool operator==(const pair_mr& o) const { return master == o.master && rel == o.rel; }
};

// RLE on consecutive rel per master -> half-open quads [term, master, 0, start, end).
void postings_to_quads_rle(uint32_t term, const std::vector<uint64_t>& postings, std::vector<spot_quad>& out) {
    std::vector<pair_mr> pairs;
    pairs.reserve(postings.size());
    for (uint64_t p : postings) pairs.push_back({unpack_master(p), unpack_relative(p)});
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    size_t i = 0;
    while (i < pairs.size()) {
        const uint64_t master = pairs[i].master;
        uint64_t start = pairs[i].rel;
        uint64_t prev = start;
        ++i;
        while (i < pairs.size() && pairs[i].master == master) {
            if (pairs[i].rel == prev + 1) {
                prev = pairs[i].rel;
                ++i;
                continue;
            }
            out.push_back({term, master, 0, start, prev + 1});
            start = prev = pairs[i].rel;
            ++i;
        }
        out.push_back({term, master, 0, start, prev + 1});
    }
}

// One half-open quad per posting (no RLE).
void postings_to_quads_point(uint32_t term, const std::vector<uint64_t>& postings, std::vector<spot_quad>& out) {
    for (uint64_t p : postings) {
        const uint64_t master = unpack_master(p);
        const uint64_t rel = unpack_relative(p);
        out.push_back({term, master, 0, rel, rel + 1});
    }
}

void postings_to_quads_rle_per_term(const std::vector<uint64_t>& postings, std::vector<spot_quad>& out) {
    const size_t base = out.size();
    postings_to_quads_rle(0, postings, out);
    for (size_t i = base; i < out.size(); ++i) out[i] = {out[i][1], 0, 0, out[i][3], out[i][4]};
}

void postings_to_quads_point_per_term(const std::vector<uint64_t>& postings, std::vector<spot_quad>& out) {
    const size_t base = out.size();
    postings_to_quads_point(0, postings, out);
    for (size_t i = base; i < out.size(); ++i) out[i] = {out[i][1], 0, 0, out[i][3], out[i][4]};
}

enum class build_mode { global, per_term };

struct docs_metrics {
    uint64_t n_raw = 0;
    uint64_t n_pairs_uniq = 0;
    uint64_t n_snap_elems = 0;
};

// Deep scan .docs for BPI denominators (aligned with plus_t/utils/bpi.h).
docs_metrics scan_docs_metrics(const std::string& path, uint32_t max_terms) {
    docs_metrics m;
    std::ifstream in(path.c_str(), std::ios::binary);
    uint32_t nlists = 0;
    if (!in.is_open() || !read_u32(in, nlists)) return m;
    const uint32_t limit = (max_terms && max_terms < nlists) ? max_terms : nlists;
    std::vector<pair_mr> pairs;
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

    void build_global(const std::string& docs_path, uint32_t max_terms, bool use_rle) {
        mode = build_mode::global;
        const docs_index meta = inspect_docs(docs_path, max_terms);
        if (meta.offsets.empty()) return;
        n_terms = meta.nlists;
        std::ifstream docs(docs_path.c_str(), std::ios::binary);
        std::vector<spot_quad> D;
        std::vector<uint64_t> postings;
        const uint32_t bits =
            std::max(bits_required_u64(meta.nlists ? meta.nlists - 1 : 0), bits_required_u64(meta.max_master));
        for (uint32_t term = 0; term < meta.nlists; ++term) {
            if (!read_posting_list(docs, meta.offsets[term], postings)) continue;
            if (use_rle)
                postings_to_quads_rle(term, postings, D);
            else
                postings_to_quads_point(term, postings, D);
        }
        raw_quads = D.size();
        if (D.empty()) return;
        auto D_T = generate_list_of_updates(D);
        for (const auto& p : D_T) update_events += p.second.size();
        global = create_time_first_trie(D, D_T, bits, 2, false, 2);
    }

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
            per_term[term] = create_time_first_trie(D, D_T, bits, 1, false, 1);
        }
        (void)use_rle;
    }

    std::vector<triple_tm> values_at(uint32_t version) {
        if (mode == build_mode::global) {
            std::vector<triple_tm> got = global.values_at_version(version);
            std::sort(got.begin(), got.end());
            return got;
        }
        std::vector<triple_tm> all;
        for (uint32_t term = 0; term < n_terms && term < per_term.size(); ++term) {
            if (per_term[term].interval_count() == 0) continue;
            for (const triple_tm& t : per_term[term].values_at_version(version))
                all.push_back({term, t.master});
        }
        std::sort(all.begin(), all.end());
        return all;
    }

    std::vector<triple_tm> values_at(uint32_t term, uint32_t version) {
        if (mode == build_mode::global) {
            std::vector<triple_tm> got;
            for (const triple_tm& t : global.values_at_version(version))
                if (t.term == term) got.push_back(t);
            std::sort(got.begin(), got.end());
            return got;
        }
        if (term >= per_term.size() || per_term[term].interval_count() == 0) return {};
        std::vector<triple_tm> got;
        for (const triple_tm& t : per_term[term].values_at_version(version)) got.push_back({term, t.master});
        std::sort(got.begin(), got.end());
        return got;
    }

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

static double bpi_from_bytes(uint64_t bytes, uint64_t n) {
    if (n == 0) return 0.0;
    return (static_cast<double>(bytes) * 8.0) / static_cast<double>(n);
}

// --- Graphviz dump of the LOGICAL time-first trie (demo only, small inputs) ---
// Walks the compact structure itself (interval array + temporal_wm via values_at_version).
bool dump_trie_dot(time_first_trie& trie, const std::string& out_path, const std::string& title,
                   uint64_t max_intervals, uint64_t max_payload) {
    std::ofstream out(out_path.c_str());
    if (!out.is_open()) return false;
    const uint64_t n_int = trie.interval_count();
    const uint64_t shown = std::min(n_int, max_intervals);
    const bool two_levels = trie.payload_components() == 2;

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
        out << "  I" << i << " [label=\"[" << iv.first << ", " << iv.second << ")\\nlast_update=" << lu
            << "\", shape=box, style=filled, fillcolor=\"#bfdbfe\", color=\"#1d4ed8\"];\n";
        out << "  root -> I" << i << " [label=\"" << i << "\"];\n";

        std::vector<triple_tm> payload = trie.values_at_version(iv.first);
        if (payload.empty()) {
            out << "  E" << i << " [label=\"(vacio)\", shape=plaintext, fontcolor=\"#94a3b8\"];\n";
            out << "  I" << i << " -> E" << i << ";\n";
            continue;
        }
        const uint64_t p_shown = std::min<uint64_t>(payload.size(), max_payload);
        if (two_levels) {
            uint64_t k = 0;
            while (k < p_shown) {
                const uint32_t term = payload[k].term;
                out << "  T" << i << "_" << term << " [label=\"term " << term
                    << "\", shape=box, style=filled, fillcolor=\"#fef3c7\", color=\"#d97706\"];\n";
                out << "  I" << i << " -> T" << i << "_" << term << ";\n";
                while (k < p_shown && payload[k].term == term) {
                    out << "  M" << i << "_" << term << "_" << payload[k].master << " [label=\"u="
                        << payload[k].master << "\", shape=ellipse, style=filled, fillcolor=\"#dcfce7\", "
                           "color=\"#15803d\"];\n";
                    out << "  T" << i << "_" << term << " -> M" << i << "_" << term << "_" << payload[k].master
                        << ";\n";
                    ++k;
                }
            }
        } else {
            for (uint64_t k = 0; k < p_shown; ++k) {
                out << "  M" << i << "_" << payload[k].master << " [label=\"u=" << payload[k].master
                    << "\", shape=ellipse, style=filled, fillcolor=\"#dcfce7\", color=\"#15803d\"];\n";
                out << "  I" << i << " -> M" << i << "_" << payload[k].master << ";\n";
            }
        }
        if (p_shown < payload.size()) {
            out << "  X" << i << " [label=\"... +" << (payload.size() - p_shown)
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

void print_size_report(const index_size_report& rep, const docs_metrics& dm, build_mode mode) {
    const uint64_t bits_total = rep.bytes_total * 8;
    std::cout << "mode=" << (mode == build_mode::global ? "global" : "per-term") << "\n";
    std::cout << "bytes_total=" << rep.bytes_total << "\n";
    std::cout << "bits_total=" << bits_total << "\n";
    std::cout << "n_raw=" << dm.n_raw << "\n";
    std::cout << "n_pairs_uniq=" << dm.n_pairs_uniq << "\n";
    std::cout << "n_snap_elems=" << dm.n_snap_elems << "\n";
    std::cout << std::setprecision(8) << "bpi_file=" << bpi_from_bytes(rep.bytes_total, dm.n_raw) << "\n";
    std::cout << "bpi_over_pairs=" << bpi_from_bytes(rep.bytes_total, dm.n_pairs_uniq) << "\n";
    std::cout << "bpi_over_stored=" << bpi_from_bytes(rep.bytes_total, dm.n_snap_elems) << "\n";
    std::cout << "bpi_total=" << bpi_from_bytes(rep.bytes_total, dm.n_raw) << "\n";
    if (mode == build_mode::global) {
        const auto& b = rep.global_bd;
        std::cout << "bytes_wm_total=" << b.wm.bytes_total << "\n";
        std::cout << "bytes_tempint=" << (b.bytes_tempint_left + b.bytes_tempint_right) << "\n";
    } else {
        std::cout << "per_term_built=" << rep.per_term_count << "\n";
    }
}

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

std::vector<uint64_t> expected_masters(const std::vector<uint64_t>& postings, uint32_t relative) {
    std::vector<pair_mr> pairs;
    for (uint64_t p : postings) pairs.push_back({unpack_master(p), unpack_relative(p)});
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    std::vector<uint64_t> masters;
    size_t i = 0;
    const uint64_t rel = relative;
    while (i < pairs.size()) {
        const uint64_t master = pairs[i].master;
        uint64_t start = pairs[i].rel;
        uint64_t prev = start;
        ++i;
        while (i < pairs.size() && pairs[i].master == master) {
            if (pairs[i].rel == prev + 1) {
                prev = pairs[i].rel;
                ++i;
                continue;
            }
            if (start <= rel && rel <= prev) masters.push_back(master);
            start = prev = pairs[i].rel;
            ++i;
        }
        if (start <= rel && rel <= prev) masters.push_back(master);
    }
    std::sort(masters.begin(), masters.end());
    masters.erase(std::unique(masters.begin(), masters.end()), masters.end());
    return masters;
}

static std::vector<triple_tm> expected_triples_at(const std::vector<uint64_t>& postings, uint32_t term,
                                                  uint32_t relative) {
    std::vector<triple_tm> exp;
    for (uint64_t m : expected_masters(postings, relative)) exp.push_back({term, m});
    std::sort(exp.begin(), exp.end());
    return exp;
}

static void print_mismatch(uint32_t term, uint32_t rel, const std::vector<triple_tm>& got,
                           const std::vector<triple_tm>& exp) {
    std::cerr << "MISMATCH term=" << term << " rel=" << rel << " got={";
    for (size_t i = 0; i < got.size(); ++i) std::cerr << (i ? "," : "") << got[i].master;
    std::cerr << "} exp={";
    for (size_t i = 0; i < exp.size(); ++i) std::cerr << (i ? "," : "") << exp[i].master;
    std::cerr << "}\n";
}

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
        for (uint32_t rel : times) {
            const std::vector<triple_tm> exp = expected_triples_at(postings, term, rel);
            const std::vector<triple_tm> got = index.values_at(term, rel);
            if (got != exp) {
                if (mismatches < debug_limit) print_mismatch(term, rel, got, exp);
                ++mismatches;
            }
        }
    }
    (void)mode;
    return mismatches;
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
    uint64_t dump_max_payload = 16;
    uint32_t debug_mismatches = 0;
};

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
        } else if (a == "--dump-max-payload" && i + 1 < argc) {
            opt.dump_max_payload = std::strtoull(argv[++i], nullptr, 10);
        } else if (a == "--debug-mismatches" && i + 1 < argc) {
            opt.debug_mismatches = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace edd

int main(int argc, char** argv) {
    edd::cli_options opt;
    if (!edd::parse_args(argc, argv, opt)) {
        std::cerr << "Usage: " << argv[0]
                  << " <global|per-term> <input.docs> [--max-terms N] [--validate-terms N] [--no-rle] [--csv path] "
                     "[--serialize out.emt] [--dump-dot out.dot [--dump-term T] [--dump-max-intervals N] "
                     "[--dump-max-payload N]] [--debug-mismatches N]\n";
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

    if (!opt.dump_dot_path.empty()) {
        std::ostringstream title;
        bool ok = false;
        if (opt.mode == edd::build_mode::global) {
            title << "EDD metatrie (global) — payload (term, master) — " << opt.docs_path;
            ok = edd::dump_trie_dot(index.global, opt.dump_dot_path, title.str(), opt.dump_max_intervals,
                                    opt.dump_max_payload);
        } else if (opt.dump_term < index.per_term.size()) {
            title << "EDD metatrie (per-term) — term " << opt.dump_term << " — " << opt.docs_path;
            ok = edd::dump_trie_dot(index.per_term[opt.dump_term], opt.dump_dot_path, title.str(),
                                    opt.dump_max_intervals, opt.dump_max_payload);
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
        const double bpi_file = edd::bpi_from_bytes(rep.bytes_total, dm.n_raw);
        csv << (opt.mode == edd::build_mode::global ? "global" : "per-term") << ','
            << rep.bytes_total << ',' << dm.n_raw << ',' << std::setprecision(8) << bpi_file << ','
            << edd::bpi_from_bytes(rep.bytes_total, dm.n_pairs_uniq) << ','
            << edd::bpi_from_bytes(rep.bytes_total, dm.n_snap_elems) << ',' << bpi_file << ',' << build_s
            << '\n';
    }
    return 0;
}
