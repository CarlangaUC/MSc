
// The implementation of the persistent successor data structure

#ifndef EDD_CLTJ_TEMPORAL_WM_U64_HPP
#define EDD_CLTJ_TEMPORAL_WM_U64_HPP

#include <limits>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include "edd_cltj_types.hpp"
#include <sdsl/sdsl_concepts.hpp>
#include <sdsl/int_vector.hpp>
#include <sdsl/rank_support_v.hpp>

using namespace sdsl;

namespace edd {

template<class t_bitvector   = sdsl::bit_vector,
         class t_rank        = typename t_bitvector::rank_1_type> 
class temporal_wm {

public: 
        typedef int_vector<>::size_type              size_type;
        typedef int_vector<>::value_type             value_type;
        typedef t_bitvector                          bit_vector_type;
        typedef t_rank                               rank_1_type;

protected:
    size_type m_n_updates;  // number of updates in the sequence of events
    size_type m_tuple_comp;
    size_type m_n_bits;

    bit_vector_type m_B; // bit vector B in the paper
    rank_1_type m_B_rank;

    bit_vector_type m_E; // bit vector E in the paper
    rank_1_type m_E_rank;

//    bit_vector_type m_B_aux; // bit vector B in the paper
//    rank_1_type m_B_aux_rank;

//    bit_vector_type m_E_aux; // bit vector E in the paper
//    rank_1_type m_E_aux_rank;


    // Copia B/E y reengancha los rank al vector propio.
    void copy(const temporal_wm& wm) {
            m_n_updates  = wm.m_n_updates;
            m_tuple_comp = wm.m_tuple_comp;
	        m_n_bits     = wm.m_n_bits;
            m_B          = wm.m_B;
            m_E          = wm.m_E;
            m_B_rank     = wm.m_B_rank;
            m_B_rank.set_vector(&m_B);
            m_E_rank     = wm.m_E_rank;
            m_E_rank.set_vector(&m_E);
            // m_B_aux      = wm.m_B_aux;
            // m_E_aux      = wm.m_E_aux;
            // m_B_aux_rank = wm.m_B_aux_rank;
            // m_E_aux_rank = wm.m_E_aux_rank;
    }

    // Campo i del spot_quad: 0=term_id, 1=master_doc, 2=unused, 3=version_start, 4=version_end.
    static uint64_t get_component(const spot_quad& t, size_type component) {
        return t[component];
    }


    // Hash for std::tuple<uint64_t,uint64_t,uint64_t>
    struct TripleHash {
        // Hash de prefijo (p0,p1,p2) para contar símbolos iguales al armar B.
        std::size_t operator()(const std::tuple<uint64_t,uint64_t,uint64_t> &t) const noexcept {
            auto [a,b,c] = t;
            std::size_t h = std::hash<uint64_t>{}(a);
            h ^= std::hash<uint64_t>{}(b) + 0x9e3779b9 + (h<<6) + (h>>2);
            h ^= std::hash<uint64_t>{}(c) + 0x9e3779b9 + (h<<6) + (h>>2);
            return h;
        }
    };

    // Extracts bits_to_use most-significant bits from val, which has total_bits bits
    // Los `bits_to_use` bits más significativos de un componente.
    static inline uint64_t top_bits(uint64_t val, int bits_to_use, int total_bits) {
        if (bits_to_use <= 0) return 0;
        if (bits_to_use >= total_bits) return val;
        return val >> (total_bits - bits_to_use);
    }


    // Bit i del valor (term_id / master_doc), leído de MSB a LSB por componente.
    static bool get_accumulated_bit(const spot_quad & t, int i, int bits_per_component) {
        int comp = i / bits_per_component;        // determines the component
        int bit_in_comp = i % bits_per_component; // position within the component

        int shift = bits_per_component - 1 - bit_in_comp;

        uint64_t val;
        if (comp == 0) val = t[0];
        else if (comp == 1) val = t[1];
        else val = t[2];
        
        return (val >> shift) & 1ULL;
    }

    struct PrefixComparator {
        const std::vector<spot_quad> & data;
        int total_bits_used; 
        int bits_per_component; 
        int num_components;   

        PrefixComparator(const std::vector<spot_quad> & d,
                        int total_bits, int bits_per_comp, int n_comp)
                        : data(d), total_bits_used(total_bits),
                        bits_per_component(bits_per_comp), num_components(n_comp) {}

        // Orden estable por el prefijo de bits ya emitidos.
        bool operator() (size_type a, size_type b) const {
            int bits_left = total_bits_used;
            for (int c = 0; c < num_components && bits_left > 0; ++c) {
                int use = std::min(bits_left, bits_per_component);
                uint64_t va = top_bits(get_component(data[a], c), use, bits_per_component);
                uint64_t vb = top_bits(get_component(data[b], c), use, bits_per_component);
                if (va != vb) return va < vb;
                bits_left -= use;
            }
            return false;
        }
    };

    // Construye bitvectors B y E (paper §6) con sort estable por prefijo.
    void generate_B_E_stable_prefix_sort(const std::vector<spot_quad>& data, std::vector<bool>& delete_flags,
                                         int bits) {
        size_type n = data.size();
        std::vector<size_type> idx(n);
        for (size_type i = 0; i < n; ++i)
            idx[i] = i;

        size_type bit_in_B = 0, bit_in_E = 0;
        size_type diff_delete = 0;
        for (size_type i = 0; i < n; ++i) {
            diff_delete += (delete_flags[i])?-1:1;
            m_E[bit_in_E++] = (diff_delete == 0) ? 0 : 1;
        }

        for (size_type cur_bit = 1; cur_bit <= m_tuple_comp * bits; ++cur_bit) { 
            
            for (size_type k = 0; k < n; ++k) {
	            m_B[bit_in_B] = get_accumulated_bit(data[idx[k]], cur_bit-1, bits); 
                ++bit_in_B;
            }
        
            PrefixComparator cmp(data, cur_bit, bits, m_tuple_comp);
            std::stable_sort(idx.begin(), idx.end(), cmp);

            int cur_comp = (int)((cur_bit - 1) / bits);          // component of the i-th bit (0-based)
            int bit_in_comp = (int)((cur_bit - 1) % bits);       // index within that component (0..bits-1)
            int bits_in_this_comp = bit_in_comp + 1;        // number of bits from that component

            // hash table for (prefix0, prefix1, prefix2) keys
            std::unordered_map<std::tuple<uint64_t,uint64_t,uint64_t>, int, TripleHash> prefix_count;
            prefix_count.reserve(2*n);

            for (size_type k = 0; k < n; ++k) {
                const spot_quad &t = data[idx[k]];

                uint64_t p0 = 0, p1 = 0, p2 = 0;

                // component 0
                if (0 < cur_comp) {
                    // component 0 already processed -> use all of it
                    p0 = get_component(t, 0);
                } else if (0 == cur_comp) {
                    // component 0 partially processed (currently processing it within this pass)
                    p0 = top_bits(get_component(t, 0), bits_in_this_comp, bits);
                } else {
                    p0 = 0;
                }

                // component 1
                if (1 < cur_comp) {
                    p1 = get_component(t, 1);
                } else if (1 == cur_comp) {
                    p1 = top_bits(get_component(t, 1), bits_in_this_comp, bits);
                } else {
                    p1 = 0;
                }

                // component 2
                if (2 < cur_comp) {
                    p2 = get_component(t, 2);
                } else if (2 == cur_comp) {
                    p2 = top_bits(get_component(t, 2), bits_in_this_comp, bits);
                } else {
                    p2 = 0;
                }

                auto key = std::make_tuple(p0, p1, p2);

                if (delete_flags[idx[k]]) 
                    prefix_count[key]--;
                else 
                    prefix_count[key]++;

                // mark m_E_aux (1 if prefix_count > 0, 0 if <= 0)
                m_E[bit_in_E++] = (prefix_count[key] == 0) ? 0 : 1;
            }
        }
        if (bit_in_B > m_B.size() || bit_in_E > m_E.size()) {
            std::cerr << "edd::temporal_wm: B/E size mismatch wrote B=" << bit_in_B << "/" << m_B.size()
                      << " E=" << bit_in_E << "/" << m_E.size() << " n=" << n << " bits=" << bits
                      << " tuple_comp=" << m_tuple_comp << "\n";
            std::abort();
        }
    }

    // rank1(B) en el rango [s,e] del nivel `depth`.
    int64_t rank_range_B(size_type depth, size_type s, size_type e) {
        return m_B_rank(depth*m_n_updates+e+1) - m_B_rank(depth*m_n_updates + s);
    }

    // rank1(E) en el rango [s,e] del nivel `depth`.
    int64_t rank_range_E(size_type depth, size_type s, size_type e) {
        return m_E_rank(depth*m_n_updates+e+1) - m_E_rank(depth*m_n_updates + s);
    }

    // No choca con masters válidos (hasta 40 bits); callers usan uint64_t::max como ∞.
    static constexpr uint64_t INFTY = std::numeric_limits<uint64_t>::max();

    // Menor símbolo ≥ en el subárbol (hijo 0, si no el hijo 1).
    size_type leftmost(size_type depth, int64_t s, int64_t e, int64_t p, size_type h,
                       std::pair<std::pair<size_type, size_type>, size_type>& node_pair) {
        if (p < 0 || s > e) return INFTY;

        if (h == 0) {
            node_pair.second = p;
            node_pair.first.first = s;
            node_pair.first.second = e;
            return 0;
        }

        const int64_t r = rank_range_B(depth, s, e);
        const int64_t p_prime = rank_range_B(depth, s, s + p);
        const int64_t p_left = p - p_prime;
        if (p_left >= 0 && s <= e - r && m_E[(depth + 1) * m_n_updates + s + p_left] == 1) {
            const size_type t = leftmost(depth + 1, s, e - r, p_left, h - 1, node_pair);
            if (t != INFTY) return t;
        }
        if (p_prime <= 0 || r == 0) return INFTY;
        const size_type t = leftmost(depth + 1, e - r + 1, e, p_prime - 1, h - 1, node_pair);
        return (t == INFTY) ? INFTY : (uint64_t(1) << (h - 1)) + t;
    }

public:
 
    temporal_wm() = default;

    // Arma B/E desde el stream de updates ya ordenado time-first.
    temporal_wm(const std::vector<spot_quad>& update_tuples, std::vector<bool> is_delete,
                size_type n_tuple_components, size_type n_bits, bool is_partial = false) {

        m_n_updates = update_tuples.size();
        m_tuple_comp = n_tuple_components;
        m_n_bits = n_bits;

        const size_type b_size = m_n_bits * m_n_updates * m_tuple_comp;
        const size_type e_size = m_n_bits * m_n_updates * m_tuple_comp + m_n_updates;
        if (m_n_updates > 50000000 || m_n_bits > 64 || m_tuple_comp > 8 || b_size > 500000000ULL) {
            std::cerr << "edd::temporal_wm: refuse alloc n_updates=" << m_n_updates << " n_bits=" << m_n_bits
                      << " tuple_comp=" << m_tuple_comp << " b_size=" << b_size << "\n";
            std::abort();
        }

        m_B = bit_vector_type(b_size, 0);
        m_E = bit_vector_type(e_size, 0);

        generate_B_E_stable_prefix_sort(update_tuples, is_delete, n_bits);

        if (is_partial) {
            // we only store only the middle component, as the firt one
            // is shared with another trie via the meta trie
            m_tuple_comp = 1; // a single component
            bit_vector_type m_B_aux(m_n_bits*m_n_updates, 0);
            bit_vector_type m_E_aux((m_n_bits+1)*m_n_updates, 0);
            
            uint64_t l, r, i, i_aux;
            l = m_n_bits*m_n_updates;
            r = 2*m_n_bits*m_n_updates;
            for (i_aux = 0, i = l; i < r; i++, i_aux++) {
                m_B_aux[i_aux] = m_B[i];
                m_E_aux[i_aux] = m_E[i];
            }

            for (uint64_t k = 0; k < m_n_updates; k++) {
                m_E_aux[i_aux++] = m_E[i++];
            }

            m_B.swap(m_B_aux);
            m_E.swap(m_E_aux);
        }

        sdsl::util::init_support(m_B_rank, &m_B);
        sdsl::util::init_support(m_E_rank, &m_E);
    }

    //! Copy constructor
    temporal_wm(const temporal_wm& wm) {
        copy(wm);
    }

    //! Copy constructor
    temporal_wm(temporal_wm&& wm) {
        *this = std::move(wm);
    }

    //! Assignment operator
    temporal_wm& operator=(const temporal_wm& wm) {
        if (this != &wm) {
            copy(wm);
        }
        return *this;
    }

    //! Assignment move operator
    temporal_wm& operator=(temporal_wm&& wm) {
        if (this != &wm) {
            m_n_updates  = wm.m_n_updates;
            m_tuple_comp = wm.m_tuple_comp;
	        m_n_bits     = wm.m_n_bits;
            m_B          = std::move(wm.m_B);
            m_E          = std::move(wm.m_E);
            m_B_rank     = std::move(wm.m_B_rank);
            m_B_rank.set_vector(&m_B);
            m_E_rank     = std::move(wm.m_E_rank);
            m_E_rank.set_vector(&m_E);
            // m_B_aux          = std::move(wm.m_B_aux);
            // m_E_aux          = std::move(wm.m_E_aux);
            // m_B_aux_rank     = std::move(wm.m_B_aux_rank);
            // m_B_aux_rank.set_vector(&m_B_aux);
            // m_E_aux_rank     = std::move(wm.m_E_aux_rank);
            // m_E_aux_rank.set_vector(&m_E_aux);
        }
        return *this;
    }

    //! Swap operator
    // Intercambia dos WM y reengancha los rank supports.
    void swap(temporal_wm& wm) {
        if (this != &wm) {
            std::swap(m_n_updates, wm.m_n_updates);
            std::swap(m_tuple_comp, wm.m_tuple_comp);
	        std::swap(m_n_bits, wm.m_n_bits);
            m_B.swap(wm.m_B); 
            m_E.swap(wm.m_E);            
            sdsl::util::swap_support(m_B_rank, wm.m_B_rank, &m_B, &(wm.m_B));
            sdsl::util::swap_support(m_E_rank, wm.m_E_rank, &m_E, &(wm.m_E));
            // m_B_aux.swap(wm.m_B_aux); 
            // m_E_aux.swap(wm.m_E_aux);            
            // sdsl::util::swap_support(m_B_aux_rank, wm.m_B_aux_rank, &m_B_aux, &(wm.m_B_aux));
            // sdsl::util::swap_support(m_E_aux_rank, wm.m_E_aux_rank, &m_E_aux, &(wm.m_E_aux));
        }
    }

    // Ancho en bits de cada componente (t y/o u).
    size_type get_n_bits() const { return m_n_bits; }

    // Intervalo raíz [0, n_updates).
    std::pair<size_type, size_type> get_root() const {
        return std::make_pair(0, m_n_updates - 1);
    }

    // Sucesor ≥ x (t o u) en el WM; si el bit es 0 y no hay match, cae a leftmost.
    size_type leap(size_type depth, int64_t s, int64_t e, int64_t p, value_type x, size_type h,
                   std::pair<std::pair<size_type, size_type>, size_type> &  node_pair) {

        if (x > ((1ULL<<m_n_bits)-1)) return INFTY;

        if ((int64_t)p < 0 || s > e || m_E[depth*m_n_updates + s + p] == 0) return INFTY; 

        if (h == 0) {
            node_pair.second = p;
            node_pair.first.first = s;
            node_pair.first.second = e; 
            return 0;
        }

        int64_t r = rank_range_B(depth, s, e);
        int64_t p_prime = rank_range_B(depth, s, s+p);   // p is relative to s

        if ((x & (uint64_t(1)<<(h-1)))!= 0) {
            x = leap(depth+1, e-r+1, e, p_prime-1, x-(uint64_t(1)<<(h-1)), h-1, node_pair);
            if (x != INFTY) return x + (uint64_t(1)<<(h-1));
            else return INFTY;
        } else {
            x = leap(depth + 1, s, e - r, p - p_prime, x, h - 1, node_pair);
            if (x != INFTY) return x;
            if (r == 0 || p_prime <= 0 || m_E[(depth + 1) * m_n_updates + e - r + p_prime] == 0) return INFTY;
            x = leftmost(depth + 1, e - r + 1, e, p_prime - 1, h - 1, node_pair);
            if (x == INFTY) return INFTY;
            return (uint64_t(1) << (h - 1)) + x;
        }
    }
    
    //! Serializes the data structure into the given ostream
    // Escribe n_updates, bits, B, E y ranks.
    size_type serialize(std::ostream& out, structure_tree_node* v=nullptr, std::string name="") const {
        structure_tree_node* child = structure_tree::add_child(v, name, sdsl::util::class_name(*this));
        size_type written_bytes = 0;
        written_bytes += write_member(m_n_updates, out, child, "n_updates");
        written_bytes += write_member(m_tuple_comp, out, child, "tuple_comp");
	    written_bytes += write_member(m_n_bits, out, child, "n_bits");
        written_bytes += m_B.serialize(out, child, "B");
        written_bytes += m_E.serialize(out, child, "E");
        written_bytes += m_B_rank.serialize(out, child, "B_rank");
        written_bytes += m_E_rank.serialize(out, child, "E_rank");
        // written_bytes += m_B_aux.serialize(out, child, "B");
        // written_bytes += m_E_aux.serialize(out, child, "E");
        // written_bytes += m_B_aux_rank.serialize(out, child, "B_aux_rank");
        // written_bytes += m_E_aux_rank.serialize(out, child, "E_aux_rank");
        structure_tree::add_size(child, written_bytes);
        return written_bytes;
    }

    //! Loads the data structure from the given istream.
    // Carga el WM y reengancha ranks a B y E.
    void load(std::istream& in) {
        read_member(m_n_updates, in);
        read_member(m_tuple_comp, in);
	    read_member(m_n_bits, in);
        m_B.load(in);
        m_E.load(in);
        m_B_rank.load(in, &m_B);
        m_E_rank.load(in, &m_E);
        // m_B_aux.load(in);
        // m_E_aux.load(in);
        // m_B_aux_rank.load(in, &m_B_aux);
        // m_E_aux_rank.load(in, &m_E_aux);
    }
};


}  // namespace edd
#endif
