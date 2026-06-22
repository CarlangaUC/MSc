// =============================================================================
// N-ZDD VERSIONADO (TdZdd) — familia de snapshots de documentos por palabra
// =============================================================================
// Transposicion del modelo de dos pisos: en vez de (documento -> versiones),
// aqui modelamos (version -> documentos).
//
//   Por cada palabra t y cada version i (1..T_max):
//     S_t^i = { master : t aparece en (master, version i) }   (snapshot)
//
//   - ZDD de la palabra = familia de conjuntos { S_t^1, ..., S_t^T } (documentos
//     puros; snapshots identicos se deduplican). La VERSION NO entra al ZDD.
//   - Indice lateral por palabra: version -> snapshot (puntos de cambio sobre la
//     linea de tiempo 1..T_max; snapshot vacio = id 0). Recupera la version.
//   - Backbone 1: vector de N familias ZDD materializadas (ZDD^j por palabra j).
//   - Backbone 2: meta-ZDD sobre variables de indice (altura ceil(log2 N)) cuya
//     familia es un singleton por term_id; enruta a ZDD^j (terminales logicos).
//   - Indice lateral version->snapshot (WordTimeline) para consultas temporales.
//
//   Pool de snapshots: dedup de conjuntos S_t^i + DdStructure<2> cacheado por id.
//
// Compilar (desde la raíz MAGISTER):
//   g++ -O2 -std=c++17 -o scripts/cpp/nzdd_versionado scripts/cpp/nzdd_versionado.cpp \
//       -I ./TdZdd/include -lpthread
//
// Ejecutar (wiki_100mb):
//   ./scripts/cpp/nzdd_versionado \
//     resultados_test/wiki_100mb_uihrdc_packed64.docs \
//     uiHRDC/uiHRDC/data/texts/page_mapping_wiki_100mb.bin \
//     uiHRDC/uiHRDC/data/texts/index_wiki_100mb_named.voc \
//     Abraham Website 1730 0 1000 resultados_test/nzdd_metrics.csv
// =============================================================================

#include <tdzdd/DdSpec.hpp>
#include <tdzdd/DdSpecOp.hpp>
#include <tdzdd/DdStructure.hpp>
#include <tdzdd/util/ResourceUsage.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "uiHRDC/uiHRDC/indexes/NOPOS/II_docs/src/utils/version_packing.h"

/*
 * Eje documentos en el ZDD (Backbone 1):
 *   nivel_ZDD = master + NZDD_ZDD_DOC_LEVEL_OFFSET
 *
 * Por que +1 (default):
 *   - Los masters del motor son 0-indexed (doc 0, 1, 2, ...).
 *   - TdZdd numera variables desde nivel 1 (nivel 0 no es variable de conjunto).
 *   - El offset biyectivo evita colision doc0/doc1 y permite S = {} (vacio).
 *
 * Lectura de diagramas: S{4,6} se dibuja en niveles 5 y 7; masters = nivel - offset.
 * Override: -DNZDD_ZDD_DOC_LEVEL_OFFSET=1 (no cambiar salvo alinear con otro binario).
 */
#ifndef NZDD_ZDD_DOC_LEVEL_OFFSET
#define NZDD_ZDD_DOC_LEVEL_OFFSET 1
#endif

#if NZDD_ZDD_DOC_LEVEL_OFFSET < 1
#error "NZDD_ZDD_DOC_LEVEL_OFFSET must be >= 1 (TdZdd variables start at level 1)"
#endif

// -----------------------------------------------------------------------------
// Spec TdZdd: construye el ZDD de UN conjunto (single-subset).
// Los niveles en sortedItems ya vienen con offset aplicado (master + OFFSET).
// -----------------------------------------------------------------------------
class FastPathSpec : public tdzdd::StatelessDdSpec<FastPathSpec, 2> {
    std::vector<int> sortedItems;

public:
    explicit FastPathSpec(std::set<int> const& s) {
        sortedItems.reserve(s.size());
        for (int v : s) sortedItems.push_back(v);
        std::sort(sortedItems.rbegin(), sortedItems.rend());
    }

    int getRoot() const {
        if (sortedItems.empty()) return -1;  // terminal-1: acepta el conjunto vacio
        return sortedItems.front();
    }

    int getChild(int level, int value) const {
        if (value == 0) return 0;
        auto it = std::upper_bound(sortedItems.begin(), sortedItems.end(), level, std::greater<int>());
        if (it == sortedItems.end()) return -1;
        return *it;
    }
};

// -----------------------------------------------------------------------------
// Lectores .docs / .meta / page_map / .voc (espejo de test_tdzdd_piso1).
// -----------------------------------------------------------------------------
static bool readPisaHeader(std::ifstream& in, uint32_t& nlists) {
    in.read(reinterpret_cast<char*>(&nlists), sizeof(nlists));
    return in.good();
}

// Motor 64 bits: el header (nlists) y el largo de cada lista siguen siendo
// uint32, pero cada valor empaquetado es uint64 (8 bytes).
static bool readPisaPostingList(std::ifstream& in, std::vector<uint64_t>& out) {
    out.clear();
    uint32_t len = 0;
    in.read(reinterpret_cast<char*>(&len), sizeof(len));
    if (!in.good()) return false;
    out.resize(len);
    if (len == 0) return true;
    in.read(reinterpret_cast<char*>(out.data()), sizeof(uint64_t) * len);
    return in.good();
}

static bool loadPageMapping(const std::string& path, std::vector<uint32_t>& mapping) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    mapping.clear();
    uint32_t x = 0;
    while (in.read(reinterpret_cast<char*>(&x), sizeof(x))) mapping.push_back(x);
    return !mapping.empty();
}

struct DocsMeta {
    bool loaded = false;
    std::unordered_map<std::string, std::string> kv;
};

static bool loadDocsMeta(const std::string& docsPath, DocsMeta& out) {
    out = DocsMeta{};
    std::ifstream in(docsPath + ".meta");
    if (!in.is_open()) return false;
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        out.kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    out.loaded = true;
    return true;
}

static bool metaGetU32(const DocsMeta& m, const std::string& key, uint32_t& out) {
    auto it = m.kv.find(key);
    if (it == m.kv.end()) return false;
    out = static_cast<uint32_t>(std::stoul(it->second));
    return true;
}

static std::string metaGetStr(const DocsMeta& m, const std::string& key, const std::string& def = "") {
    auto it = m.kv.find(key);
    if (it == m.kv.end()) return def;
    return it->second;
}

static uint32_t bitread32(const uint32_t* e, uint32_t p, uint32_t len) {
    e += p / 32u;
    p %= 32u;
    uint64_t answ = static_cast<uint64_t>(*e) >> p;
    if (p + len > 32u) answ |= static_cast<uint64_t>(*(e + 1)) << (32u - p);
    if (len < 32u) answ &= ((static_cast<uint64_t>(1) << len) - 1u);
    return static_cast<uint32_t>(answ);
}

struct Vocabulary {
    bool loaded = false;
    uint32_t nwords = 0;
    std::vector<std::string> words;
    std::unordered_map<std::string, uint32_t> word2id;
};

static bool loadVocabulary(const std::string& path, Vocabulary& voc) {
    voc = Vocabulary{};
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    uint32_t nwords = 0, elemSize = 0, zoneSize = 0;
    in.read(reinterpret_cast<char*>(&nwords), sizeof(nwords));
    in.read(reinterpret_cast<char*>(&elemSize), sizeof(elemSize));
    in.read(reinterpret_cast<char*>(&zoneSize), sizeof(zoneSize));
    if (!in.good() || nwords == 0 || elemSize == 0 || elemSize > 32u) return false;
    std::vector<unsigned char> zone(zoneSize);
    if (zoneSize > 0) {
        in.read(reinterpret_cast<char*>(zone.data()), zoneSize);
        if (!in.good()) return false;
    }
    size_t nOffsets = static_cast<size_t>(nwords) + 1u;
    size_t nPacked = (((nOffsets * elemSize) + 32u - 1u) / 32u);
    std::vector<uint32_t> packed(nPacked + 1u, 0u);
    in.read(reinterpret_cast<char*>(packed.data()), nPacked * sizeof(uint32_t));
    voc.words.resize(nwords);
    for (uint32_t i = 0; i < nwords; ++i) {
        uint32_t off = bitread32(packed.data(), i * elemSize, elemSize);
        uint32_t nxt = bitread32(packed.data(), (i + 1) * elemSize, elemSize);
        if (nxt < off || nxt > zoneSize) return false;
        voc.words[i].assign(reinterpret_cast<const char*>(zone.data()) + off, nxt - off);
        voc.word2id.emplace(voc.words[i], i);
    }
    voc.nwords = nwords;
    voc.loaded = true;
    return true;
}

static int masterToZddLevel(uint32_t master) {
    return static_cast<int>(master) + NZDD_ZDD_DOC_LEVEL_OFFSET;
}

static bool zddLevelToMaster(int level, uint32_t& masterOut) {
    if (level < NZDD_ZDD_DOC_LEVEL_OFFSET) return false;
    masterOut = static_cast<uint32_t>(level - NZDD_ZDD_DOC_LEVEL_OFFSET);
    return true;
}

// Enumera masters desde un ZDD: nivel - OFFSET (ver NZDD_ZDD_DOC_LEVEL_OFFSET).
static std::vector<uint32_t> mastersOf(const tdzdd::DdStructure<2>& z) {
    std::set<uint32_t> acc;
    for (auto const& s : z) {
        for (int lvl : s) {
            uint32_t m = 0;
            if (zddLevelToMaster(lvl, m)) acc.insert(m);
        }
    }
    return std::vector<uint32_t>(acc.begin(), acc.end());
}

static std::string snapshotKey(const std::vector<uint32_t>& masters) {
    std::string key;
    key.resize(masters.size() * sizeof(uint32_t));
    if (!masters.empty()) std::memcpy(&key[0], masters.data(), key.size());
    return key;
}

static std::set<int> masterLevelsOf(const std::vector<uint32_t>& masters) {
    std::set<int> levels;
    for (uint32_t m : masters) levels.insert(masterToZddLevel(m));
    return levels;
}

static tdzdd::DdStructure<2> zddOfMasterSet(const std::vector<uint32_t>& masters) {
    FastPathSpec spec(masterLevelsOf(masters));
    tdzdd::DdStructure<2> z(spec);
    z.zddReduce();
    return z;
}

// -----------------------------------------------------------------------------
// Pool de snapshots: dedup + ZDD cacheado por id. Id 0 = snapshot vacio.
// -----------------------------------------------------------------------------
class SnapshotPool {
public:
    SnapshotPool() {
        masters_.push_back({});
        snapZdds_.emplace_back(zddOfMasterSet(masters_.back()));
        keyToId_.emplace(std::string(), 0u);
    }

    uint32_t intern(const std::vector<uint32_t>& sortedMasters) {
        std::string key = snapshotKey(sortedMasters);
        auto it = keyToId_.find(key);
        if (it != keyToId_.end()) return it->second;

        uint32_t id = static_cast<uint32_t>(masters_.size());
        masters_.push_back(sortedMasters);
        snapZdds_.push_back(zddOfMasterSet(sortedMasters));
        keyToId_.emplace(std::move(key), id);
        return id;
    }

    size_t distinctCount() const { return masters_.size(); }
    const std::vector<uint32_t>& mastersOfId(uint32_t id) const { return masters_[id]; }
    const tdzdd::DdStructure<2>& zddOfId(uint32_t id) const { return snapZdds_[id]; }

    uint64_t totalSnapZddNodes() const {
        uint64_t nodes = 0;
        for (const auto& z : snapZdds_) nodes += z.size();
        return nodes;
    }

private:
    std::vector<std::vector<uint32_t>> masters_;
    std::vector<tdzdd::DdStructure<2>> snapZdds_;
    std::unordered_map<std::string, uint32_t> keyToId_;
};

// Familia ZDD^t = union de snapshots distintos de la palabra (eje documentos).
static tdzdd::DdStructure<2> buildWordFamilyZdd(const SnapshotPool& pool,
                                                const std::vector<uint32_t>& snapIds) {
    tdzdd::DdStructure<2> family;
    bool any = false;
    for (uint32_t sid : snapIds) {
        if (sid == 0) continue;
        if (!any) {
            family = pool.zddOfId(sid);
            any = true;
        } else {
            family = tdzdd::DdStructure<2>(tdzdd::zddUnion(family, pool.zddOfId(sid)));
        }
    }
    if (any) family.zddReduce();
    return family;
}

static std::set<std::string> snapshotKeysFromZdd(const tdzdd::DdStructure<2>& z) {
    std::set<std::string> keys;
    for (auto const& s : z) {
        std::set<uint32_t> masters;
        for (int lvl : s) {
            uint32_t m = 0;
            if (zddLevelToMaster(lvl, m)) masters.insert(m);
        }
        std::vector<uint32_t> v(masters.begin(), masters.end());
        keys.insert(snapshotKey(v));
    }
    return keys;
}

static bool validateWordFamilyZdd(const tdzdd::DdStructure<2>& wordZdd,
                                  const SnapshotPool& pool,
                                  const std::vector<uint32_t>& snapIds) {
    std::set<std::string> expected;
    for (uint32_t sid : snapIds) {
        if (sid == 0) continue;
        expected.insert(snapshotKey(pool.mastersOfId(sid)));
    }
    return snapshotKeysFromZdd(wordZdd) == expected;
}

static std::vector<uint32_t> mastersOfWord(const SnapshotPool& pool,
                                           const std::vector<uint32_t>& snapIds) {
    std::set<uint32_t> acc;
    for (uint32_t sid : snapIds) {
        for (uint32_t m : pool.mastersOfId(sid)) acc.insert(m);
    }
    return std::vector<uint32_t>(acc.begin(), acc.end());
}

// -----------------------------------------------------------------------------
// Indice lateral version->snapshot por palabra: puntos de cambio sobre la linea
// de tiempo 1..T_max. Cada punto (version, snapId) indica que desde esa version
// (inclusive) y hasta el siguiente punto el snapshot es snapId. snapId 0 = vacio.
// Consulta por version: ultimo punto con version <= v (busqueda binaria).
// -----------------------------------------------------------------------------
struct WordTimeline {
    std::vector<std::pair<uint32_t, uint32_t>> changes;  // (version, snapId), ordenado

    uint32_t snapAt(uint32_t version) const {
        if (changes.empty()) return 0u;
        // ultimo change con version <= version
        auto it = std::upper_bound(
            changes.begin(), changes.end(), version,
            [](uint32_t v, const std::pair<uint32_t, uint32_t>& c) { return v < c.first; });
        if (it == changes.begin()) return 0u;  // antes del primer cambio: vacio
        return std::prev(it)->second;
    }

    size_t changePoints() const { return changes.size(); }
};

// Construye los puntos de cambio a partir del mapa denso-disperso
// (version presente -> snapId). Inserta corridas vacias en los huecos y colapsa
// versiones consecutivas con el mismo snapId.
static WordTimeline buildTimeline(const std::map<uint32_t, uint32_t>& versionToSnap,
                                  uint32_t tMax) {
    WordTimeline tl;
    if (versionToSnap.empty()) return tl;
    uint32_t prevSnap = 0u;  // estado inicial = vacio
    uint32_t expected = 1u;
    for (const auto& kv : versionToSnap) {
        uint32_t v = kv.first;
        uint32_t snap = kv.second;
        // hueco antes de v -> vacio (si el estado previo no era ya vacio)
        if (v > expected && prevSnap != 0u) {
            tl.changes.emplace_back(expected, 0u);
            prevSnap = 0u;
        }
        if (snap != prevSnap) {
            tl.changes.emplace_back(v, snap);
            prevSnap = snap;
        }
        expected = v + 1u;
    }
    // Cerrar la ultima corrida: tras la ultima version presente el estado vuelve
    // a vacio (sin esto, la corrida final se extenderia erroneamente hasta T_max).
    if (prevSnap != 0u && expected <= tMax + 1u) {
        tl.changes.emplace_back(expected, 0u);
    }
    return tl;
}

static constexpr size_t kPrintMax = 24;

static void printSet(const std::string& label, const std::vector<uint32_t>& v, size_t maxShow = kPrintMax) {
    std::cout << label << " (|set|=" << v.size() << "): { ";
    size_t show = std::min(maxShow, v.size());
    for (size_t i = 0; i < show; ++i) {
        std::cout << v[i];
        if (i + 1 < show) std::cout << ", ";
    }
    if (v.size() > show) std::cout << " ...";
    std::cout << " }\n";
}

static std::vector<uint32_t> intersectSorted(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    std::vector<uint32_t> out;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}

static uint32_t wordIndexHeight(uint32_t nWords) {
    if (nWords == 0) return 0;
    uint32_t h = 0;
    while ((1u << h) < nWords) ++h;
    return h;
}

// Niveles ZDD para el indice de palabra: bit b (MSB..LSB) -> nivel (height-b).
static std::set<int> indexLevelsForTerm(uint32_t tid, uint32_t height) {
    std::set<int> levels;
    for (int b = static_cast<int>(height) - 1; b >= 0; --b) {
        if ((tid >> b) & 1u) levels.insert(static_cast<int>(height - b));
    }
    return levels;
}

static uint32_t termFromIndexLevels(const std::set<int>& levels, uint32_t height) {
    uint32_t tid = 0;
    for (int b = static_cast<int>(height) - 1; b >= 0; --b) {
        tid <<= 1;
        if (levels.count(static_cast<int>(height - b))) tid |= 1u;
    }
    return tid;
}

// -----------------------------------------------------------------------------
// Backbone 1 + 2: familias ZDD por palabra y meta-ZDD de enrutamiento log N.
// -----------------------------------------------------------------------------
class WordMetaIndex {
public:
  void build(uint32_t nWords, std::vector<tdzdd::DdStructure<2>> families) {
    nWords_ = nWords;
    height_ = wordIndexHeight(nWords);
    families_ = std::move(families);

    tdzdd::DdStructure<2> indexZdd;
    bool any = false;
    for (uint32_t tid = 0; tid < nWords; ++tid) {
      FastPathSpec idxSpec(indexLevelsForTerm(tid, height_));
      tdzdd::DdStructure<2> one(idxSpec);
      if (!any) {
        indexZdd = one;
        any = true;
      } else {
        indexZdd = tdzdd::DdStructure<2>(tdzdd::zddUnion(indexZdd, one));
      }
    }
    if (any) indexZdd.zddReduce();
    indexZdd_ = std::move(indexZdd);
  }

  uint32_t height() const { return height_; }
  size_t familyCount() const { return families_.size(); }
  const tdzdd::DdStructure<2>& familyOf(uint32_t tid) const { return families_.at(tid); }
  const tdzdd::DdStructure<2>& indexZdd() const { return indexZdd_; }

  uint32_t navigateBits(uint32_t tid, uint32_t& steps) const {
    steps = 0;
    uint32_t pos = 0;
    for (int b = static_cast<int>(height_) - 1; b >= 0; --b) {
      pos = (pos << 1) | ((tid >> b) & 1u);
      ++steps;
    }
    return pos;
  }

  bool resolveViaIndex(uint32_t tid) const {
    if (tid >= nWords_) return false;
    std::set<int> want = indexLevelsForTerm(tid, height_);
    for (auto const& s : indexZdd_) {
      std::set<int> got(s.begin(), s.end());
      if (got == want) return termFromIndexLevels(got, height_) == tid;
    }
    return false;
  }

  uint64_t totalFamilyNodes() const {
    uint64_t nodes = 0;
    for (const auto& z : families_) nodes += z.size();
    return nodes;
  }

  tdzdd::DdStructure<2> singletonIndexZdd(uint32_t tid) const {
    FastPathSpec idxSpec(indexLevelsForTerm(tid, height_));
    tdzdd::DdStructure<2> one(idxSpec);
    one.zddReduce();
    return one;
  }

  tdzdd::DdStructure<2> demoIndexZdd(uint32_t maxTerms) const {
    tdzdd::DdStructure<2> demo;
    bool any = false;
    uint32_t n = std::min(maxTerms, nWords_);
    for (uint32_t tid = 0; tid < n; ++tid) {
      tdzdd::DdStructure<2> one = singletonIndexZdd(tid);
      if (!any) {
        demo = one;
        any = true;
      } else {
        demo = tdzdd::DdStructure<2>(tdzdd::zddUnion(demo, one));
      }
    }
    if (any) demo.zddReduce();
    return demo;
  }

private:
  uint32_t nWords_ = 0;
  uint32_t height_ = 0;
  std::vector<tdzdd::DdStructure<2>> families_;
  tdzdd::DdStructure<2> indexZdd_;
};

// -----------------------------------------------------------------------------
// Export DOT + PNG (Graphviz) y reporte de validacion.
// -----------------------------------------------------------------------------
static bool exportZddDotPng(const tdzdd::DdStructure<2>& z, const std::string& dotPath,
                            const std::string& label) {
    std::ofstream dot(dotPath);
    if (!dot.good()) return false;
    z.dumpDot(dot, label.c_str());
    dot.close();
    std::string pngPath = dotPath.substr(0, dotPath.size() - 4) + ".png";
    std::string cmd = "dot -Tpng \"" + dotPath + "\" -o \"" + pngPath + "\" 2>/dev/null";
    int rc = std::system(cmd.c_str());
    return rc == 0;
}

static std::string mastersLabel(const std::vector<uint32_t>& masters) {
    std::ostringstream oss;
    oss << "{";
    for (size_t i = 0; i < masters.size(); ++i) {
        if (i) oss << ",";
        oss << masters[i];
    }
    oss << "}";
    return oss.str();
}

// Ejemplo pedagogico del enunciado: hola -> S^1={1}, S^2={1,2}, S^3={2}.
static tdzdd::DdStructure<2> buildToyHolaFamilyZdd() {
    auto snap = [](std::initializer_list<uint32_t> ms) {
        std::set<int> levels;
        for (uint32_t m : ms) levels.insert(masterToZddLevel(m));
        FastPathSpec spec(levels);
        tdzdd::DdStructure<2> z(spec);
        z.zddReduce();
        return z;
    };
    tdzdd::DdStructure<2> z1 = snap({1});
    tdzdd::DdStructure<2> z2 = snap({1, 2});
    tdzdd::DdStructure<2> z3 = snap({2});
    tdzdd::DdStructure<2> family = tdzdd::DdStructure<2>(tdzdd::zddUnion(z1, z2));
    family = tdzdd::DdStructure<2>(tdzdd::zddUnion(family, z3));
    family.zddReduce();
    return family;
}

struct ValidationReport {
    uint64_t zddFamilyMismatches = 0;
    uint64_t metaRouteFails = 0;
    uint64_t timelineMismatches = 0;
    uint64_t mtZddVsPoolMismatches = 0;
    uint64_t metaBijectionFails = 0;
    uint64_t packRoundtripFails = 0;
    uint64_t changePointSpotFails = 0;
    bool allOk() const {
        return zddFamilyMismatches == 0 && metaRouteFails == 0 && timelineMismatches == 0 &&
               mtZddVsPoolMismatches == 0 && metaBijectionFails == 0 && packRoundtripFails == 0 &&
               changePointSpotFails == 0;
    }
};

static void writeValidationReport(const std::string& path, const ValidationReport& r,
                                  uint32_t terms, const std::string& packedDocs) {
    std::ofstream out(path);
    if (!out.good()) return;
    out << "# N-ZDD validation report\n";
    out << "packed_docs=" << packedDocs << "\n";
    out << "terms=" << terms << "\n";
    out << "V1_zdd_family_vs_pool_mismatches=" << r.zddFamilyMismatches << "\n";
    out << "V2_meta_route_fails=" << r.metaRouteFails << "\n";
    out << "V3_timeline_reconstruction_mismatches=" << r.timelineMismatches << "\n";
    out << "V4_Mt_zdd_vs_pool_mismatches=" << r.mtZddVsPoolMismatches << "\n";
    out << "V5_meta_index_bijection_fails=" << r.metaBijectionFails << "\n";
    out << "V6_pack_unpack_roundtrip_fails=" << r.packRoundtripFails << "\n";
    out << "V7_timeline_changepoint_spot_fails=" << r.changePointSpotFails << "\n";
    out << "overall=" << (r.allOk() ? "PASS" : "FAIL") << "\n";
}

static uint32_t findSnapshotWithMinMasters(const SnapshotPool& pool, size_t minMasters) {
    for (uint32_t id = 1; id < pool.distinctCount(); ++id) {
        if (pool.mastersOfId(id).size() >= minMasters) return id;
    }
    return 1;
}

static uint32_t findTermWithMostSnapshots(const std::vector<std::vector<uint32_t>>& wordSnapIds) {
    uint32_t best = 0;
    size_t bestN = 0;
    for (uint32_t t = 0; t < wordSnapIds.size(); ++t) {
        size_t n = 0;
        for (uint32_t sid : wordSnapIds[t]) if (sid != 0) ++n;
        if (n > bestN) {
            bestN = n;
            best = t;
        }
    }
    return best;
}

int main(int argc, char** argv) {
    std::string packedDocsPath = "/root/MAGISTER/resultados_test/wiki_100mb_uihrdc_packed64.docs";
    std::string mappingPath = "/root/MAGISTER/uiHRDC/uiHRDC/data/texts/page_mapping_wiki_100mb.bin";
    std::string vocabPath = "/root/MAGISTER/uiHRDC/uiHRDC/data/texts/index_wiki_100mb_named.voc";
    std::string wordA = "Abraham";
    std::string wordB = "Website";
    int queryVersion = 1730;
    uint32_t maxTerms = 0;
    uint32_t progressEvery = 1000;
    std::string metricsCsvPath = "resultados_test/nzdd_metrics.csv";
    std::string vizDir = "resultados_test/nzdd_viz";
    std::string validationReportPath = "resultados_test/nzdd_validacion_report.txt";

    if (argc >= 2) packedDocsPath = argv[1];
    if (argc >= 3) mappingPath = argv[2];
    if (argc >= 4) vocabPath = argv[3];
    if (argc >= 5) wordA = argv[4];
    if (argc >= 6) wordB = argv[5];
    if (argc >= 7) queryVersion = std::atoi(argv[6]);
    if (argc >= 8) maxTerms = static_cast<uint32_t>(std::strtoul(argv[7], nullptr, 10));
    if (argc >= 9) progressEvery = static_cast<uint32_t>(std::strtoul(argv[8], nullptr, 10));
    if (argc >= 10) metricsCsvPath = argv[9];
    if (argc >= 11) vizDir = argv[10];
    if (argc >= 12) validationReportPath = argv[11];

    std::cout << "=== N-ZDD VERSIONADO (TdZdd, version -> documentos) ===\n";
    std::cout << "[CFG] packed_docs = " << packedDocsPath << "\n";
    std::cout << "[CFG] page_map    = " << mappingPath << "\n";
    std::cout << "[CFG] vocab       = " << vocabPath << "\n";
    std::cout << "[CFG] wordA/wordB = " << wordA << " / " << wordB << "\n";
    std::cout << "[CFG] query_ver   = " << queryVersion << "\n";
    std::cout << "[CFG] max_terms   = " << maxTerms << "\n";
    std::cout << "[CFG] metrics_csv = " << metricsCsvPath << "\n";
    std::cout << "[CFG] viz_dir     = " << vizDir << "\n";
    std::cout << "[CFG] val_report  = " << validationReportPath << "\n";
    std::cout << "[CFG] bit_split   = " << ZDD_MASTER_BITS << "/" << ZDD_REL_BITS << "\n\n";

    DocsMeta meta;
    if (!loadDocsMeta(packedDocsPath, meta)) {
        std::cerr << "Error: falta metadata (" << packedDocsPath << ".meta).\n";
        return 1;
    }
    uint32_t metaMasterBits = 0, metaRelBits = 0;
    if (!metaGetU32(meta, "master_bits", metaMasterBits) || !metaGetU32(meta, "rel_bits", metaRelBits)) {
        std::cerr << "Error: meta sin master_bits/rel_bits.\n";
        return 1;
    }
    if (metaMasterBits != ZDD_MASTER_BITS || metaRelBits != ZDD_REL_BITS) {
        std::cerr << "Error: split incompatible. meta=" << metaMasterBits << "/" << metaRelBits
                  << " binario=" << ZDD_MASTER_BITS << "/" << ZDD_REL_BITS << "\n";
        return 1;
    }
    if (metaGetStr(meta, "tuple_output", "") != "packed") {
        std::cerr << "Error: tuple_output != packed.\n";
        return 1;
    }
    std::cout << "[META] tuple_output=packed; split validado.\n";

    std::ifstream packedIn(packedDocsPath, std::ios::binary);
    if (!packedIn.is_open()) {
        std::cerr << "Error: no se pudo abrir packed_docs.\n";
        return 1;
    }
    uint32_t nlistsPacked = 0;
    if (!readPisaHeader(packedIn, nlistsPacked)) {
        std::cerr << "Error: header PISA invalido.\n";
        return 1;
    }
    std::cout << "[OK] header packed lists = " << nlistsPacked << "\n";

    std::vector<uint32_t> mapping;
    if (loadPageMapping(mappingPath, mapping))
        std::cout << "[OK] page_map entries = " << mapping.size() << "\n";
    else
        std::cout << "[WARN] page_map no cargado (no bloquea).\n";

    Vocabulary voc;
    bool hasVocab = loadVocabulary(vocabPath, voc);
    if (hasVocab)
        std::cout << "[OK] vocab cargado: " << voc.nwords << " palabras\n";
    else
        std::cout << "[WARN] vocab no disponible.\n";

    if (progressEvery == 0) progressEvery = 1000;

    uint32_t termsToProcess = nlistsPacked;
    if (maxTerms > 0 && maxTerms < nlistsPacked) termsToProcess = maxTerms;

    // -------------------- Primera pasada: cargar postings + T_max -------------
    std::vector<std::vector<uint64_t>> postings;
    postings.reserve(termsToProcess);
    uint32_t tMax = 0;
    uint32_t maxMaster = 0;
    uint64_t totalPackedValues = 0;
    {
        std::vector<uint64_t> posting;
        for (uint32_t term = 0; term < termsToProcess; ++term) {
            if (!readPisaPostingList(packedIn, posting)) {
                std::cerr << "Error leyendo posting en term " << term << ".\n";
                return 1;
            }
            totalPackedValues += posting.size();
            for (uint64_t packed : posting) {
                uint32_t master = static_cast<uint32_t>(ZDD_UNPACK_MASTER(packed));
                uint32_t rel = static_cast<uint32_t>(ZDD_UNPACK_REL(packed));
                if (master > maxMaster) maxMaster = master;
                if (rel > tMax) tMax = rel;
            }
            postings.push_back(posting);
        }
    }
    std::cout << "[OK] T_max (version global maxima) = " << tMax << "\n";
    std::cout << "[OK] max_master (doc maestro maximo) = " << maxMaster << "\n";
    std::cout << "[OK] zdd_doc_level_offset = " << NZDD_ZDD_DOC_LEVEL_OFFSET
              << " (nivel_ZDD = master + offset; TdZdd parte en 1, masters 0-index)\n\n";

    auto wallStart = std::chrono::steady_clock::now();
    tdzdd::ResourceUsage usageStart;

    // -------------------- Construccion N-ZDD ----------------------------------
    SnapshotPool pool;
    // Por palabra: ids de snapshots distintos (evita copiar DdStructure por termino).
    std::vector<std::vector<uint32_t>> wordSnapIds(termsToProcess);
    std::vector<WordTimeline> timelines(termsToProcess);
    std::vector<std::map<uint32_t, uint32_t>> versionToSnapByTerm(termsToProcess);

    uint64_t totalSnapshotInstances = 0;  // (palabra, version) con ocurrencia
    uint64_t totalChangePoints = 0;

    for (uint32_t term = 0; term < termsToProcess; ++term) {
        const std::vector<uint64_t>& posting = postings[term];

        // Agrupar por version (rel) -> conjunto de masters (S_t^i).
        std::map<uint32_t, std::set<uint32_t>> docsByVersion;
        for (uint64_t packed : posting) {
            uint32_t master = static_cast<uint32_t>(ZDD_UNPACK_MASTER(packed));
            uint32_t rel = static_cast<uint32_t>(ZDD_UNPACK_REL(packed));
            docsByVersion[rel].insert(master);
        }

        // Internar cada snapshot en el pool (dedup) y armar version->snapId.
        std::map<uint32_t, uint32_t> versionToSnap;
        std::set<uint32_t> snapIdsThisWord;
        for (const auto& kv : docsByVersion) {
            uint32_t version = kv.first;
            std::vector<uint32_t> masters(kv.second.begin(), kv.second.end());
            uint32_t snapId = pool.intern(masters);
            versionToSnap[version] = snapId;
            snapIdsThisWord.insert(snapId);
            ++totalSnapshotInstances;
        }

        // Snapshots distintos de la palabra (familia = union logica via pool, sin copiar ZDD).
        wordSnapIds[term].assign(snapIdsThisWord.begin(), snapIdsThisWord.end());
        std::sort(wordSnapIds[term].begin(), wordSnapIds[term].end());

        versionToSnapByTerm[term] = versionToSnap;
        timelines[term] = buildTimeline(versionToSnap, tMax);
        totalChangePoints += timelines[term].changePoints();

        if ((term + 1) % progressEvery == 0 || term + 1 == termsToProcess) {
            std::cout << "[PROGRESS] term " << (term + 1) << "/" << termsToProcess
                      << " | snapshots distintos=" << pool.distinctCount()
                      << " | change_points=" << totalChangePoints << "\n";
            std::cout.flush();
        }
    }

    // -------------------- Backbone 1: ZDD^j materializado por palabra ---------
    std::vector<tdzdd::DdStructure<2>> wordFamilyZdds;
    wordFamilyZdds.reserve(termsToProcess);
    for (uint32_t term = 0; term < termsToProcess; ++term) {
        wordFamilyZdds.push_back(buildWordFamilyZdd(pool, wordSnapIds[term]));
        if ((term + 1) % progressEvery == 0 || term + 1 == termsToProcess) {
            std::cout << "[PROGRESS-ZDD] term " << (term + 1) << "/" << termsToProcess
                      << " | word_zdd_nodes=" << wordFamilyZdds.back().size() << "\n";
            std::cout.flush();
        }
    }

    // -------------------- Backbone 2: meta-ZDD indice palabras (log N) ----------
    WordMetaIndex wordIndex;
    wordIndex.build(termsToProcess, std::move(wordFamilyZdds));

    auto wallBuild = std::chrono::steady_clock::now();
    double buildS = std::chrono::duration_cast<std::chrono::duration<double>>(wallBuild - wallStart).count();

    // -------------------- Metricas --------------------------------------------
    size_t distinctSnaps = pool.distinctCount();
    size_t distinctNonEmpty = distinctSnaps > 0 ? distinctSnaps - 1 : 0;
    double snapDedup = distinctNonEmpty > 0
                           ? static_cast<double>(totalSnapshotInstances) / distinctNonEmpty
                           : 0.0;
    uint64_t poolZddNodes = pool.totalSnapZddNodes();
    uint64_t wordZddNodes = wordIndex.totalFamilyNodes();

    std::cout << "\n=== RESUMEN BUILD ===\n";
    std::cout << "terms processed             = " << termsToProcess << "\n";
    std::cout << "values packed               = " << totalPackedValues << "\n";
    std::cout << "T_max                       = " << tMax << "\n";
    std::cout << "build time (s)              = " << std::fixed << std::setprecision(2) << buildS << "\n";
    std::cout << "\n--- SNAPSHOTS (eje documentos) ---\n";
    std::cout << "instancias (palabra,version)= " << totalSnapshotInstances << "\n";
    std::cout << "snapshots distintos (no vac)= " << distinctNonEmpty << "\n";
    std::cout << "factor dedup snapshots      = " << std::fixed << std::setprecision(2) << snapDedup << "x\n";
    std::cout << "nodos ZDD pool snapshots    = " << poolZddNodes << "\n";
    std::cout << "\n--- INDICE LATERAL version->snapshot ---\n";
    std::cout << "change_points totales       = " << totalChangePoints << "\n";
    double cpRatio = totalSnapshotInstances > 0
                         ? static_cast<double>(totalSnapshotInstances) / static_cast<double>(totalChangePoints == 0 ? 1 : totalChangePoints)
                         : 0.0;
    std::cout << "compresion temporal (inst/cp)= " << std::fixed << std::setprecision(2) << cpRatio << "x\n";
    std::cout << "\n--- BACKBONE 1: ZDD^j por palabra ---\n";
    std::cout << "familias materializadas     = " << wordIndex.familyCount() << "\n";
    std::cout << "nodos ZDD familias (suma)   = " << wordZddNodes << "\n";
    std::cout << "\n--- BACKBONE 2: meta-ZDD indice palabras ---\n";
    std::cout << "meta-ZDD nodos              = " << wordIndex.indexZdd().size() << "\n";
    std::cout << "altura indice (log2 N)      = " << wordIndex.height() << "\n";

    // -------------------- Resolver palabras -----------------------------------
    auto resolveTerm = [&](const std::string& w, int& outTid) -> bool {
        char* end = nullptr;
        long v = std::strtol(w.c_str(), &end, 10);
        if (end != w.c_str() && *end == '\0' && v >= 0 && static_cast<uint32_t>(v) < termsToProcess) {
            outTid = static_cast<int>(v);
            return true;
        }
        if (hasVocab) {
            auto it = voc.word2id.find(w);
            if (it != voc.word2id.end()) {
                outTid = static_cast<int>(it->second);
                return outTid >= 0 && static_cast<uint32_t>(outTid) < termsToProcess;
            }
        }
        return false;
    };

    int tidA = -1, tidB = -1;
    bool okA = resolveTerm(wordA, tidA);
    bool okB = resolveTerm(wordB, tidB);

    // -------------------- CONSULTAS -------------------------------------------
    // Consulta 1: S_t^version = documentos de la palabra en una version dada.
    std::cout << "\n=== CONSULTA 1: S_t^version (docs de la palabra en una version) ===\n";
    if (!okA) {
        std::cout << "[INFO] wordA='" << wordA << "' no resuelta.\n";
    } else {
        std::cout << "wordA='" << wordA << "' -> term_id=" << tidA << "\n";
        uint32_t snapId = timelines[tidA].snapAt(static_cast<uint32_t>(queryVersion));
        const std::vector<uint32_t>& docs = pool.mastersOfId(snapId);
        std::cout << "  version=" << queryVersion << " -> snapId=" << snapId
                  << (snapId == 0 ? " (vacio)" : "") << "\n";
        printSet("  S_t^version", docs, 16);
    }

    // Consulta 2: M_t desde ZDD^t materializado (union de snapshots de la familia).
    std::cout << "\n=== CONSULTA 2: M_t via ZDD^t (documentos en cualquier version) ===\n";
    if (okA) {
        std::vector<uint32_t> mA = mastersOf(wordIndex.familyOf(static_cast<uint32_t>(tidA)));
        printSet("  M_wordA (ZDD)", mA);
    }
    if (okB) {
        std::cout << "wordB='" << wordB << "' -> term_id=" << tidB << "\n";
        std::vector<uint32_t> mB = mastersOf(wordIndex.familyOf(static_cast<uint32_t>(tidB)));
        printSet("  M_wordB (ZDD)", mB);
    }

    // Consulta 3: interseccion de snapshots en una version (docs donde ambos en v).
    std::cout << "\n=== CONSULTA 3: S_tA^version ∩ S_tB^version ===\n";
    if (okA && okB) {
        uint32_t sa = timelines[tidA].snapAt(static_cast<uint32_t>(queryVersion));
        uint32_t sb = timelines[tidB].snapAt(static_cast<uint32_t>(queryVersion));
        std::vector<uint32_t> inter = intersectSorted(pool.mastersOfId(sa), pool.mastersOfId(sb));
        std::cout << "  snapId(A)=" << sa << " snapId(B)=" << sb
                  << "  same_snapshot? " << (sa == sb && sa != 0 ? "SI" : "no") << "\n";
        printSet("  S_A^v ∩ S_B^v", inter, 16);
    } else {
        std::cout << "[INFO] requiere ambas palabras resueltas.\n";
    }

    // -------------------- Backbone 2: enrutamiento meta-ZDD --------------------
    std::cout << "\n=== BACKBONE 2: meta-ZDD indice palabras (log N) ===\n";
    if (okA) {
        uint32_t steps = 0;
        uint32_t leaf = wordIndex.navigateBits(static_cast<uint32_t>(tidA), steps);
        bool viaMeta = wordIndex.resolveViaIndex(static_cast<uint32_t>(tidA));
        std::cout << "  term_id=" << tidA << " -> ruta bits en " << steps
                  << " pasos (hoja=" << leaf << "); meta-ZDD enruta a ZDD^"
                  << tidA << "? " << (viaMeta ? "SI" : "NO") << "\n";
        const auto& fam = wordIndex.familyOf(static_cast<uint32_t>(tidA));
        size_t subsets = 0;
        for (auto const& _ : fam) { (void)_; ++subsets; }
        std::cout << "  ZDD^" << tidA << " nodos=" << fam.size()
                  << " snapshots_en_familia=" << subsets << "\n";
    }

    // -------------------- VALIDACIONES COMPLETAS --------------------------------
    ValidationReport val;
    std::cout << "\n=== VALIDACIONES N-ZDD ===\n";

    // V1: ZDD^j enumera exactamente los snapshots del pool para cada palabra.
    std::cout << "\n[V1] Backbone 1: ZDD^j vs snapshots del pool\n";
    for (uint32_t term = 0; term < termsToProcess; ++term) {
        if (!validateWordFamilyZdd(wordIndex.familyOf(term), pool, wordSnapIds[term])) {
            ++val.zddFamilyMismatches;
            if (val.zddFamilyMismatches <= 5) {
                std::cout << "  [FAIL] term " << term << ": familia ZDD != snapshots\n";
            }
        }
    }
    std::cout << "  mismatches=" << val.zddFamilyMismatches << " -> "
              << (val.zddFamilyMismatches == 0 ? "PASS" : "FAIL") << "\n";

    // V2: meta-ZDD enruta cada term_id a su ZDD^j.
    std::cout << "\n[V2] Backbone 2: meta-ZDD enruta term_id -> ZDD^j\n";
    for (uint32_t term = 0; term < termsToProcess; ++term) {
        if (!wordIndex.resolveViaIndex(term)) ++val.metaRouteFails;
    }
    std::cout << "  routed=" << (termsToProcess - val.metaRouteFails) << "/" << termsToProcess
              << " fails=" << val.metaRouteFails << " -> "
              << (val.metaRouteFails == 0 ? "PASS" : "FAIL") << "\n";

    // V3: timeline reconstruye posting (version,master) exacta.
    std::cout << "\n[V3] Timeline: reconstruccion (version,master) vs .docs\n";
    for (uint32_t term = 0; term < termsToProcess; ++term) {
        std::set<std::pair<uint32_t, uint32_t>> expected;
        for (uint64_t packed : postings[term]) {
            expected.emplace(static_cast<uint32_t>(ZDD_UNPACK_REL(packed)),
                             static_cast<uint32_t>(ZDD_UNPACK_MASTER(packed)));
        }
        std::set<std::pair<uint32_t, uint32_t>> rebuilt;
        const WordTimeline& tl = timelines[term];
        for (size_t i = 0; i < tl.changes.size(); ++i) {
            uint32_t vStart = tl.changes[i].first;
            uint32_t snapId = tl.changes[i].second;
            uint32_t vEnd = (i + 1 < tl.changes.size()) ? tl.changes[i + 1].first - 1u : tMax;
            if (snapId == 0) continue;
            const std::vector<uint32_t>& docs = pool.mastersOfId(snapId);
            for (uint32_t v = vStart; v <= vEnd; ++v)
                for (uint32_t d : docs) rebuilt.emplace(v, d);
        }
        if (rebuilt != expected) {
            ++val.timelineMismatches;
            if (val.timelineMismatches <= 5) {
                std::cout << "  [FAIL] term " << term << " exp=" << expected.size()
                          << " got=" << rebuilt.size() << "\n";
            }
        }
    }
    std::cout << "  mismatches=" << val.timelineMismatches << " -> "
              << (val.timelineMismatches == 0 ? "PASS" : "FAIL") << "\n";

    // V4: M_t desde ZDD^j == union pool (mastersOfWord).
    std::cout << "\n[V4] M_t: masters(ZDD^j) vs union snapshots pool\n";
    for (uint32_t term = 0; term < termsToProcess; ++term) {
        std::vector<uint32_t> fromZdd = mastersOf(wordIndex.familyOf(term));
        std::vector<uint32_t> fromPool = mastersOfWord(pool, wordSnapIds[term]);
        if (fromZdd != fromPool) ++val.mtZddVsPoolMismatches;
    }
    std::cout << "  mismatches=" << val.mtZddVsPoolMismatches << " -> "
              << (val.mtZddVsPoolMismatches == 0 ? "PASS" : "FAIL") << "\n";

    // V5: meta-ZDD es biyeccion term_id <-> singleton del indice.
    std::cout << "\n[V5] Meta-ZDD: biyeccion term_id <-> singleton indice\n";
  {
    std::unordered_set<uint32_t> seen;
    size_t count = 0;
    for (auto const& s : wordIndex.indexZdd()) {
        ++count;
        std::set<int> got(s.begin(), s.end());
        uint32_t tid = termFromIndexLevels(got, wordIndex.height());
        if (tid >= termsToProcess || !seen.insert(tid).second) ++val.metaBijectionFails;
    }
    if (count != termsToProcess) ++val.metaBijectionFails;
    std::cout << "  subsets_in_meta=" << count << " terms=" << termsToProcess
              << " bijection_fails=" << val.metaBijectionFails << " -> "
              << (val.metaBijectionFails == 0 ? "PASS" : "FAIL") << "\n";
  }

    // V6: roundtrip ZDD_PACK / ZDD_UNPACK en todas las postings.
    std::cout << "\n[V6] Motor 64-bit: roundtrip PACK/UNPACK en postings\n";
    for (uint32_t term = 0; term < termsToProcess; ++term) {
        for (uint64_t packed : postings[term]) {
            uint32_t m = static_cast<uint32_t>(ZDD_UNPACK_MASTER(packed));
            uint32_t r = static_cast<uint32_t>(ZDD_UNPACK_REL(packed));
            if (ZDD_PACK(m, r) != packed) ++val.packRoundtripFails;
        }
    }
    std::cout << "  fails=" << val.packRoundtripFails << " -> "
              << (val.packRoundtripFails == 0 ? "PASS" : "FAIL") << "\n";

    // V7: snapAt en cada punto de cambio coincide con versionToSnap original.
    std::cout << "\n[V7] Timeline: snapAt en puntos de cambio\n";
    for (uint32_t term = 0; term < termsToProcess; ++term) {
        for (const auto& kv : versionToSnapByTerm[term]) {
            if (timelines[term].snapAt(kv.first) != kv.second) ++val.changePointSpotFails;
        }
    }
    std::cout << "  fails=" << val.changePointSpotFails << " -> "
              << (val.changePointSpotFails == 0 ? "PASS" : "FAIL") << "\n";

    std::cout << "\n=== RESULTADO GLOBAL VALIDACION ===\n";
    std::cout << "  overall: " << (val.allOk() ? "PASS (7/7)" : "FAIL") << "\n";
    writeValidationReport(validationReportPath, val, termsToProcess, packedDocsPath);
    std::cout << "[OK] reporte: " << validationReportPath << "\n";

    // -------------------- VISUALIZACIONES (>=2 PNG por backbone) ----------------
    std::cout << "\n=== VISUALIZACIONES (DOT + PNG) ===\n";
    std::string mkdirCmd = "mkdir -p \"" + vizDir + "\"";
    (void)std::system(mkdirCmd.c_str());

    int pngB1 = 0;
    int pngB2 = 0;

    // Backbone 1 — imagen 1: snapshot del pool (multi-doc).
    {
        uint32_t snapId = findSnapshotWithMinMasters(pool, 2);
        const auto& masters = pool.mastersOfId(snapId);
        std::string label = "B1_snapshot_id" + std::to_string(snapId) + "_S" + mastersLabel(masters);
        std::string dot = vizDir + "/b1_snapshot_pool.dot";
        if (exportZddDotPng(pool.zddOfId(snapId), dot, label)) {
            ++pngB1;
            std::cout << "  [B1] " << dot << " -> " << vizDir << "/b1_snapshot_pool.png\n";
        }
    }

    // Backbone 1 — imagen 2: ZDD^j de palabra con mas snapshots distintos.
    {
        uint32_t richTerm = findTermWithMostSnapshots(wordSnapIds);
        std::string wname = hasVocab && richTerm < voc.words.size() ? voc.words[richTerm] : ("term" + std::to_string(richTerm));
        std::string label = "B1_ZDD_" + wname + "_tid" + std::to_string(richTerm);
        std::string dot = vizDir + "/b1_word_family_richest.dot";
        if (exportZddDotPng(wordIndex.familyOf(richTerm), dot, label)) {
            ++pngB1;
            std::cout << "  [B1] " << dot << " -> " << vizDir << "/b1_word_family_richest.png"
                      << " (" << wname << ", tid=" << richTerm << ")\n";
        }
    }

    // Backbone 1 — imagen 3: ejemplo toy "hola" del enunciado.
    {
        tdzdd::DdStructure<2> toy = buildToyHolaFamilyZdd();
        std::string dot = vizDir + "/b1_toy_hola.dot";
        if (exportZddDotPng(toy, dot, "B1_toy_hola S1={1} S2={1,2} S3={2}")) {
            ++pngB1;
            std::cout << "  [B1] " << dot << " -> " << vizDir << "/b1_toy_hola.png (ejemplo pedagogico)\n";
        }
    }

    // Backbone 2 — imagen 1: meta-ZDD completo (indice de palabras).
    {
        std::string dot = vizDir + "/b2_meta_index_full.dot";
        std::string label = "B2_meta_index N=" + std::to_string(termsToProcess) + " h=" + std::to_string(wordIndex.height());
        if (exportZddDotPng(wordIndex.indexZdd(), dot, label)) {
            ++pngB2;
            std::cout << "  [B2] " << dot << " -> " << vizDir << "/b2_meta_index_full.png\n";
        }
    }

    // Backbone 2 — imagen 2: singleton de enrutamiento para wordA.
    if (okA) {
        tdzdd::DdStructure<2> route = wordIndex.singletonIndexZdd(static_cast<uint32_t>(tidA));
        std::string dot = vizDir + "/b2_route_wordA.dot";
        std::string label = "B2_route_" + wordA + "_tid" + std::to_string(tidA);
        if (exportZddDotPng(route, dot, label)) {
            ++pngB2;
            std::cout << "  [B2] " << dot << " -> " << vizDir << "/b2_route_wordA.png\n";
        }
    }

    // Backbone 2 — imagen 3: demo legible (primeros 8 terminos).
    {
        tdzdd::DdStructure<2> demo = wordIndex.demoIndexZdd(8);
        std::string dot = vizDir + "/b2_meta_demo8.dot";
        if (exportZddDotPng(demo, dot, "B2_meta_demo first_8_terms")) {
            ++pngB2;
            std::cout << "  [B2] " << dot << " -> " << vizDir << "/b2_meta_demo8.png\n";
        }
    }

    std::cout << "  PNG backbone 1: " << pngB1 << " | PNG backbone 2: " << pngB2 << "\n";
    std::cout << "  directorio: " << vizDir << "/\n";

    auto wallEnd = std::chrono::steady_clock::now();
    tdzdd::ResourceUsage usageEnd;
    tdzdd::ResourceUsage usageDiff = usageEnd - usageStart;
    double elapsedS = std::chrono::duration_cast<std::chrono::duration<double>>(wallEnd - wallStart).count();

    // -------------------- Metricas CSV ----------------------------------------
    std::ofstream metrics(metricsCsvPath, std::ios::app);
    if (metrics.good()) {
        if (metrics.tellp() == 0) {
            metrics << "packed_docs,terms,values_packed,t_max,snapshot_instances,"
                    << "distinct_snapshots,snapshot_dedup,pool_zdd_nodes,word_zdd_nodes,"
                    << "meta_zdd_nodes,change_points,word_index_height,"
                    << "zdd_family_mismatches,meta_route_fails,timeline_mismatches,"
                    << "build_s,elapsed_s,peak_mem_kb\n";
        }
        metrics << packedDocsPath << "," << termsToProcess << "," << totalPackedValues << ","
                << tMax << "," << totalSnapshotInstances << "," << distinctNonEmpty << ","
                << std::fixed << std::setprecision(4) << snapDedup << "," << poolZddNodes << ","
                << wordZddNodes << "," << wordIndex.indexZdd().size() << ","
                << totalChangePoints << "," << wordIndex.height() << ","
                << val.zddFamilyMismatches << "," << val.metaRouteFails << "," << val.timelineMismatches << ","
                << std::setprecision(6) << buildS << "," << elapsedS << ","
                << usageDiff.maxrss << "\n";
        std::cout << "\n[OK] metrics csv: " << metricsCsvPath << "\n";
    }

    std::cout << "\nFinalizado (N-ZDD versionado TdZdd).\n";
    return 0;
}
