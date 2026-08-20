// =============================================================================
// PISO 2 — pool de versiones R_{t,d} (elegir backend: tdzdd | cudd)
// =============================================================================
//
// Primer piso incluido: S_t (masters) por termino.
// Segundo piso: pool de patrones R_{t,d} = { rel : t aparece en (master, rel) }.
//
// Backends:
//   tdzdd (alias 2a): dedup exacta + pool unificado TdZdd.
//   cudd  (alias 2b): pool unico CUDD (unique table compartida).
//
// Compilar (desde la raíz MAGISTER, solo TdZdd):
//   g++ -O2 -std=c++17 -o scripts/cpp/test_tdzdd_piso2 scripts/cpp/test_tdzdd_piso2.cpp \
//       -I ./TdZdd/include -lpthread
//
// Compilar (TdZdd + CUDD):
//   g++ -O2 -std=c++17 -DHAVE_CUDD -o scripts/cpp/test_tdzdd_piso2 scripts/cpp/test_tdzdd_piso2.cpp \
//       -I ./TdZdd/include -I ./cudd/cudd -I ./cudd \
//       -L ./cudd/cudd/.libs -Wl,-rpath,'$ORIGIN/cudd/cudd/.libs' -lcudd -lpthread
//
// Ejecutar:
//   ./scripts/cpp/test_tdzdd_piso2 tdzdd \
//     resultados_test/wiki_100mb_uihrdc_packed.docs \
//     uiHRDC/uiHRDC/data/texts/page_mapping_wiki_100mb.bin \
//     1729 13 1730 500 0 resultados_test/piso2_tdzdd_metrics.csv 0 \
//     uiHRDC/uiHRDC/data/texts/index_wiki_100mb_named.voc Abraham
//
//   LD_LIBRARY_PATH=./cudd/cudd/.libs ./scripts/cpp/test_tdzdd_piso2 cudd \
//     resultados_test/wiki_100mb_uihrdc_packed.docs \
//     uiHRDC/uiHRDC/data/texts/page_mapping_wiki_100mb.bin \
//     1729 13 1730 500 0 resultados_test/piso2_cudd_metrics.csv 0 \
//     uiHRDC/uiHRDC/data/texts/index_wiki_100mb_named.voc Abraham
//
// Compatibilidad: si el primer argumento es *.docs (sin prefijo backend), asume tdzdd.
// =============================================================================

#include <tdzdd/DdSpec.hpp>
#include <tdzdd/DdSpecOp.hpp>
#include <tdzdd/DdStructure.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <chrono>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <tdzdd/DdEval.hpp>
#include <tdzdd/util/ResourceUsage.hpp>

#include "uiHRDC/uiHRDC/indexes/NOPOS/II_docs/src/utils/version_packing.h"

#ifdef HAVE_CUDD
#include "cudd.h"
#endif

enum class Piso2Backend { TDZDD, CUDD };

static bool isBackendToken(const std::string& s) {
    return s == "tdzdd" || s == "2a" || s == "cudd" || s == "2b";
}

static Piso2Backend parseBackendToken(const std::string& s) {
    if (s == "cudd" || s == "2b") return Piso2Backend::CUDD;
    return Piso2Backend::TDZDD;
}

static void printPiso2Usage() {
    std::cerr
        << "Uso:\n"
        << "  test_tdzdd_piso2 <tdzdd|cudd> <packed.docs> <page_map.bin> "
        << "[query_term query_master query_rel progress dump_dot metrics_csv max_terms vocab query_word]\n\n"
        << "  test_tdzdd_piso2 <packed.docs> ...   (legacy, asume tdzdd)\n\n"
        << "Backends:\n"
        << "  tdzdd | 2a  -> pool dedup exacta + union TdZdd\n"
        << "  cudd  | 2b  -> pool unico CUDD (requiere compilar con -DHAVE_CUDD)\n";
}

// -----------------------------------------------------------------------------
// Spec para construir una ZDD a partir de un conjunto de enteros (camino).
// Identico al del binario base.
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
        if (sortedItems.empty()) return -1;
        return sortedItems.front();
    }

    int getChild(int level, int value) const {
        if (value == 0) return 0;
        auto it = std::upper_bound(sortedItems.begin(), sortedItems.end(), level, std::greater<int>());
        if (it == sortedItems.end()) return -1;
        return *it;
    }
};

struct AuditStats {
    uint64_t totalLists = 0;
    uint64_t totalPackedValues = 0;
    uint64_t packedUnsortedLists = 0;
    uint64_t packedDuplicates = 0;
    uint64_t ddLevelSkipsPacked = 0;
    uint64_t masterLevelSkips = 0;
    uint64_t relLevelSkips = 0;
};

// -----------------------------------------------------------------------------
// Auditoria de memoria (igual que el binario base).
// -----------------------------------------------------------------------------
namespace ZddAudit {

struct MemoryStats {
    size_t totalBytes = 0;
    size_t nodePayload = 0;
    size_t indexPayload = 0;
    size_t vectorHeaders = 0;
    size_t classOverhead = 0;
};

template <int ARITY>
MemoryStats auditarMemoria(const tdzdd::DdStructure<ARITY>& dd) {
    MemoryStats stats{};

    const auto& handler = dd.getDiagram();
    const auto& entity = *handler;
    int numRows = entity.numRows();

    size_t sizeStructure = sizeof(dd);
    size_t sizeEntity = sizeof(tdzdd::NodeTableEntity<ARITY>);
    size_t sizeRef = sizeof(unsigned);

    size_t sizeObjectHeap = sizeRef + sizeEntity;
    if (sizeObjectHeap % 8 != 0) sizeObjectHeap += (8 - (sizeObjectHeap % 8));
    stats.classOverhead = sizeStructure + sizeObjectHeap;

    size_t sizeMyVectorHeader = sizeof(tdzdd::MyVector<int>);
    stats.vectorHeaders = static_cast<size_t>(numRows) * sizeMyVectorHeader * 3;

    for (int i = 0; i < numRows; ++i) {
        const auto& rowVector = entity[i];
        stats.nodePayload += rowVector.capacity() * sizeof(tdzdd::Node<ARITY>);
    }

    for (int i = 0; i < numRows; ++i) {
        const auto& highVec = entity.higherLevels(i);
        stats.indexPayload += highVec.capacity() * sizeof(int);

        const auto& lowVec = entity.lowerLevels(i);
        stats.indexPayload += lowVec.capacity() * sizeof(int);
    }

    stats.totalBytes =
        stats.nodePayload + stats.indexPayload + stats.vectorHeaders + stats.classOverhead;
    return stats;
}

}  // namespace ZddAudit

static void imprimirReporteMemoria(const std::string& titulo, const tdzdd::DdStructure<2>& dd) {
    ZddAudit::MemoryStats s = ZddAudit::auditarMemoria(dd);

    double totalKB = static_cast<double>(s.totalBytes) / 1024.0;
    double totalMB = totalKB / 1024.0;
    double bytesPerNode = (dd.size() > 0) ? static_cast<double>(s.totalBytes) / dd.size() : 0.0;

    std::cout << "\n=== " << titulo << " ===\n";
    std::cout << "Nodos logicos (dd.size): " << dd.size() << "\n";
    std::cout << "Niveles fisicos (rows):  " << dd.getDiagram()->numRows() << "\n";
    std::cout << "-------------------------------------------------\n";
    std::cout << " [1] Nodos ZDD (datos):  " << s.nodePayload << " B\n";
    std::cout << " [2] Indices (ints):     " << s.indexPayload << " B (higher/lower)\n";
    std::cout << " [3] Headers vectores:   " << s.vectorHeaders << " B (MyVector)\n";
    std::cout << " [4] Overhead clases:    " << s.classOverhead << " B\n";
    std::cout << "-------------------------------------------------\n";
    std::cout << " TOTAL FISICO:           " << s.totalBytes << " Bytes\n";
    std::cout << "                         " << std::fixed << std::setprecision(2) << totalKB << " KB\n";
    std::cout << "                         " << std::fixed << std::setprecision(2) << totalMB << " MB\n";
    std::cout << "-------------------------------------------------\n";
    std::cout << " Costo real:             " << std::fixed << std::setprecision(2) << bytesPerNode
              << " bytes/nodo\n";
    std::cout << "=================================================\n";
}

static bool toDdLevel(uint64_t v, int& out) {
    if (v == 0 || v > static_cast<uint64_t>(std::numeric_limits<int>::max())) return false;
    out = static_cast<int>(v);
    return true;
}

static bool readPisaHeader(std::ifstream& in, uint32_t& nlists) {
    in.read(reinterpret_cast<char*>(&nlists), sizeof(nlists));
    return in.good();
}

// Motor 64 bits: header/largos uint32; cada valor empaquetado uint64 (8 bytes).
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

static void auditSortedDedup(const std::vector<uint64_t>& v, bool& unsorted, uint64_t& duplicates) {
    unsorted = false;
    if (v.size() < 2) return;
    for (size_t i = 1; i < v.size(); ++i) {
        if (v[i] < v[i - 1]) unsorted = true;
        if (v[i] == v[i - 1]) ++duplicates;
    }
}

static void printVectorSample(const std::vector<uint64_t>& v, size_t maxShow, const std::string& tag) {
    std::cout << tag << " (len=" << v.size() << "): ";
    size_t show = std::min(maxShow, v.size());
    for (size_t i = 0; i < show; ++i) {
        std::cout << v[i];
        if (i + 1 < show) std::cout << " ";
    }
    if (v.size() > show) std::cout << " ...";
    std::cout << "\n";
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

// Lectura bit-packed espejo de bitread() de uiHRDC (basics.c), W=32, LSB-first.
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

// Enumera el conjunto (masters o rels) a partir del handle ZDD.
// Cada nivel guardado es valor+1, por lo que se resta 1 al listar.
static std::vector<uint32_t> elementsOf(const tdzdd::DdStructure<2>& z) {
    std::set<uint32_t> acc;
    for (auto const& s : z) {
        for (int lvl : s) {
            if (lvl >= 1) acc.insert(static_cast<uint32_t>(lvl - 1));
        }
    }
    return std::vector<uint32_t>(acc.begin(), acc.end());
}

// Evita bloquear la terminal imprimiendo posting lists / R enormes.
static constexpr size_t kMaxPrintElems = 24;

static void printSetSample(const char* label, const std::vector<uint32_t>& v, size_t maxShow = kMaxPrintElems) {
    std::cout << label << " (|set|=" << v.size() << "): { ";
    size_t show = std::min(maxShow, v.size());
    for (size_t i = 0; i < show; ++i) {
        std::cout << v[i];
        if (i + 1 < show) std::cout << ", ";
    }
    if (v.size() > show) std::cout << " ...";
    std::cout << " }\n";
    std::cout.flush();
}

// Clave canonica de un patron R (rels ya ordenados y unicos): bytes crudos.
static std::string patternKey(const std::vector<uint32_t>& rels) {
    std::string key;
    key.resize(rels.size() * sizeof(uint32_t));
    std::memcpy(&key[0], rels.data(), key.size());
    return key;
}

static int run_piso2_tdzdd(int argc, char** argv) {
    std::string globalDocsPath = "N/A";
    std::string packedDocsPath = "/root/MAGISTER/resultados_test/wiki_100mb_uihrdc_packed.docs";
    std::string mappingPath = "/root/MAGISTER/uiHRDC/uiHRDC/data/texts/page_mapping_wiki_100mb.bin";
    int queryTerm = 1;
    int queryMaster = 13;
    int queryRel = 1730;
    uint32_t progressEvery = 500;
    bool dumpDot = false;
    std::string metricsCsvPath = "resultados_test/piso2_tdzdd_metrics.csv";
    uint32_t maxTerms = 0;
    std::string vocabPath = "/root/MAGISTER/uiHRDC/uiHRDC/data/texts/index_wiki_100mb_named.voc";
    std::string queryWord = "";

    auto endsWith = [](const std::string& s, const std::string& suf) {
        return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
    };

    // Mismo contrato CLI que test_tdzdd_two_floor.cpp.
    bool legacyCli = (argc >= 3) && endsWith(argv[2], ".docs");
    if (legacyCli) {
        if (argc >= 3) { globalDocsPath = argv[1]; packedDocsPath = argv[2]; }
        if (argc >= 4) mappingPath = argv[3];
        if (argc >= 5) queryTerm = std::atoi(argv[4]);
        if (argc >= 6) queryMaster = std::atoi(argv[5]);
        if (argc >= 7) queryRel = std::atoi(argv[6]);
        if (argc >= 8) progressEvery = static_cast<uint32_t>(std::strtoul(argv[7], nullptr, 10));
        if (argc >= 9) dumpDot = (std::atoi(argv[8]) != 0);
        if (argc >= 10) metricsCsvPath = argv[9];
        if (argc >= 11) maxTerms = static_cast<uint32_t>(std::strtoul(argv[10], nullptr, 10));
        if (argc >= 12) vocabPath = argv[11];
        if (argc >= 13) queryWord = argv[12];
    } else {
        if (argc >= 2) packedDocsPath = argv[1];
        if (argc >= 3) mappingPath = argv[2];
        if (argc >= 4) queryTerm = std::atoi(argv[3]);
        if (argc >= 5) queryMaster = std::atoi(argv[4]);
        if (argc >= 6) queryRel = std::atoi(argv[5]);
        if (argc >= 7) progressEvery = static_cast<uint32_t>(std::strtoul(argv[6], nullptr, 10));
        if (argc >= 8) dumpDot = (std::atoi(argv[7]) != 0);
        if (argc >= 9) metricsCsvPath = argv[8];
        if (argc >= 10) maxTerms = static_cast<uint32_t>(std::strtoul(argv[9], nullptr, 10));
        if (argc >= 11) vocabPath = argv[10];
        if (argc >= 12) queryWord = argv[11];
    }

    auto wallStart = std::chrono::steady_clock::now();
    tdzdd::ResourceUsage usageStart;

    std::cout << "=== PISO 2 (backend=tdzdd) ===\n";
    std::cout << "[CFG] cli_mode    = " << (legacyCli ? "legacy(global+packed)" : "packed_only") << "\n";
    std::cout << "[CFG] packed_docs = " << packedDocsPath << "\n";
    std::cout << "[CFG] page_map    = " << mappingPath << "\n";
    std::cout << "[CFG] query_term  = " << queryTerm << "\n";
    std::cout << "[CFG] query_pair  = (" << queryMaster << "," << queryRel << ")\n";
    std::cout << "[CFG] progress    = " << progressEvery << "\n";
    std::cout << "[CFG] metrics_csv = " << metricsCsvPath << "\n";
    std::cout << "[CFG] max_terms   = " << maxTerms << "\n";
    std::cout << "[CFG] vocab       = " << vocabPath << "\n";
    std::cout << "[CFG] query_word  = " << (queryWord.empty() ? "(ninguna)" : queryWord) << "\n";
    std::cout << "[CFG] bit_split   = " << ZDD_MASTER_BITS << "/" << ZDD_REL_BITS << "\n\n";

    DocsMeta packedMeta;
    if (!loadDocsMeta(packedDocsPath, packedMeta)) {
        std::cerr << "Error: falta metadata (" << packedDocsPath << ".meta).\n";
        return 1;
    }
    std::cout << "[META] packed meta = FOUND\n";

    uint32_t metaMasterBits = 0, metaRelBits = 0;
    if (!metaGetU32(packedMeta, "master_bits", metaMasterBits) ||
        !metaGetU32(packedMeta, "rel_bits", metaRelBits)) {
        std::cerr << "Error: packed meta no trae master_bits/rel_bits.\n";
        return 1;
    }
    if (metaMasterBits != ZDD_MASTER_BITS || metaRelBits != ZDD_REL_BITS) {
        std::cerr << "Error: split incompatible. meta=" << metaMasterBits << "/" << metaRelBits
                  << " binario=" << ZDD_MASTER_BITS << "/" << ZDD_REL_BITS << "\n";
        return 1;
    }
    std::string packedTupleOutput = metaGetStr(packedMeta, "tuple_output", "");
    if (packedTupleOutput != "packed") {
        std::cerr << "Error: tuple_output != packed (='" << packedTupleOutput << "').\n";
        return 1;
    }
    std::cout << "[META] tuple_output = packed; split validado.\n\n";

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
        std::cout << "[WARN] no se pudo cargar page_map (no bloquea master-only).\n";
    std::cout << "\n";

    // ----------------------- PRIMER PISO (igual al base) ---------------------
    tdzdd::DdStructure<2> dd;  // union global de S_t
    AuditStats stats;
    std::vector<tdzdd::DdStructure<2>> termZdds;  // handle por termino (masters)
    termZdds.reserve(nlistsPacked);

    // ----------------------- SEGUNDO PISO (pool 2a) --------------------------
    // patternKeyToId: clave canonica de rels -> pattern_id (dedup exacto).
    std::unordered_map<std::string, uint32_t> patternKeyToId;
    std::vector<tdzdd::DdStructure<2>> patternPool;  // pattern_id -> ZDD sobre rel+1
    std::vector<size_t> patternCardinality;          // |R| de cada patron distinto
    // Phi compacto: por termino, lista de (master, pattern_id).
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> phiByTerm;
    phiByTerm.reserve(nlistsPacked);
    uint64_t totalPairs = 0;   // total de pares (t,d)
    uint32_t maxRelSeen = 0;

    std::vector<uint64_t> packedPosting;
    std::vector<uint64_t> queryPackedPosting;

    uint32_t termsToProcess = nlistsPacked;
    if (maxTerms > 0 && maxTerms < nlistsPacked) termsToProcess = maxTerms;

    for (uint32_t term = 0; term < termsToProcess; ++term) {
        if (!readPisaPostingList(packedIn, packedPosting)) {
            std::cerr << "Error leyendo posting packed en term " << term << ".\n";
            return 1;
        }

        ++stats.totalLists;
        stats.totalPackedValues += packedPosting.size();
        bool unsortedPacked = false;
        auditSortedDedup(packedPosting, unsortedPacked, stats.packedDuplicates);
        if (unsortedPacked) ++stats.packedUnsortedLists;

        // ---- primer piso: S_t (masters) + agrupacion de rels por master ----
        std::set<int> masterSet;
        std::unordered_map<uint32_t, std::vector<uint32_t>> relsByMaster;

        for (uint64_t packed : packedPosting) {
            uint32_t master = static_cast<uint32_t>(ZDD_UNPACK_MASTER(packed));
            uint32_t rel = static_cast<uint32_t>(ZDD_UNPACK_REL(packed));
            if (rel > maxRelSeen) maxRelSeen = rel;

            int levelMaster = 0;
            if (toDdLevel(static_cast<uint64_t>(master) + 1u, levelMaster))
                masterSet.insert(levelMaster);
            else
                ++stats.masterLevelSkips;

            relsByMaster[master].push_back(rel);
        }

        FastPathSpec termSpec(masterSet);
        tdzdd::DdStructure<2> termZdd(termSpec);
        termZdd.zddReduce();
        termZdds.push_back(termZdd);
        dd = tdzdd::DdStructure<2>(tdzdd::zddUnion(dd, termZdd));

        // ---- segundo piso: por cada master, patron R_{t,d} con dedup -------
        std::vector<std::pair<uint32_t, uint32_t>> phiTerm;
        phiTerm.reserve(relsByMaster.size());
        for (auto& kv : relsByMaster) {
            uint32_t master = kv.first;
            std::vector<uint32_t>& rels = kv.second;
            std::sort(rels.begin(), rels.end());
            rels.erase(std::unique(rels.begin(), rels.end()), rels.end());

            std::string key = patternKey(rels);
            uint32_t pid;
            auto it = patternKeyToId.find(key);
            if (it != patternKeyToId.end()) {
                pid = it->second;
            } else {
                std::set<int> relLevels;
                for (uint32_t r : rels) {
                    int lvl = 0;
                    if (toDdLevel(static_cast<uint64_t>(r) + 1u, lvl)) relLevels.insert(lvl);
                    else ++stats.relLevelSkips;
                }
                FastPathSpec relSpec(relLevels);
                tdzdd::DdStructure<2> relZdd(relSpec);
                relZdd.zddReduce();
                pid = static_cast<uint32_t>(patternPool.size());
                patternPool.push_back(relZdd);
                patternCardinality.push_back(rels.size());
                patternKeyToId.emplace(std::move(key), pid);
            }
            phiTerm.emplace_back(master, pid);
            ++totalPairs;
        }
        phiByTerm.push_back(std::move(phiTerm));

        if (static_cast<int>(term) == queryTerm) queryPackedPosting = packedPosting;

        if ((term + 1) % progressEvery == 0 || term + 1 == termsToProcess) {
            std::cout << "[PROGRESS] term " << (term + 1) << "/" << termsToProcess
                      << " | nodes(dd)=" << dd.size()
                      << " | patrones_distintos=" << patternPool.size()
                      << " | pares=" << totalPairs << "\n";
            std::cout.flush();
        }
    }

    std::cout << "\n=== PRIMER PISO: REDUCE GLOBAL ===\n";
    size_t ddPreNodes = dd.size();
    std::cout << "dd nodes (pre)  = " << ddPreNodes << "\n";
    dd.zddReduce();
    size_t ddPostNodes = dd.size();
    std::cout << "dd nodes (post) = " << ddPostNodes << "\n";

    // ---------------- SEGUNDO PISO: metricas del pool ------------------------
    std::cout << "\n=== SEGUNDO PISO: POOL DE PATRONES R_{t,d} (2a) ===\n";
    uint64_t distinctPatterns = patternPool.size();
    uint64_t poolIndepNodes = 0;
    size_t poolIndepBytes = 0;
    for (const auto& z : patternPool) {
        poolIndepNodes += z.size();
        poolIndepBytes += ZddAudit::auditarMemoria(z).totalBytes;
    }

    std::cout << "pares (t,d) totales         = " << totalPairs << "\n";
    std::cout << "patrones DISTINTOS          = " << distinctPatterns << "\n";
    if (distinctPatterns > 0) {
        double dedup = static_cast<double>(totalPairs) / static_cast<double>(distinctPatterns);
        std::cout << "factor de deduplicacion     = " << std::fixed << std::setprecision(2)
                  << dedup << "x  (pares / patron distinto)\n";
    }
    std::cout << "max rel observado           = " << maxRelSeen << "\n";
    std::cout << "[POOL INDEPENDIENTE] suma de nodos de patrones = " << poolIndepNodes << "\n";
    std::cout << "[POOL INDEPENDIENTE] bytes fisicos aprox       = " << poolIndepBytes
              << " B (" << std::fixed << std::setprecision(2)
              << poolIndepBytes / 1024.0 / 1024.0 << " MB)\n";

    // Pool unificado en TdZdd: union de todos los patrones distintos en un solo
    // diagrama -> mide el sharing inter-patron DENTRO de TdZdd (comparable a 2b).
    std::cout << "\n[POOL UNIFICADO TdZdd] uniendo " << distinctPatterns << " patrones distintos...\n";
    tdzdd::DdStructure<2> unifiedPool;
    for (const auto& z : patternPool)
        unifiedPool = tdzdd::DdStructure<2>(tdzdd::zddUnion(unifiedPool, z));
    unifiedPool.zddReduce();
    std::cout << "[POOL UNIFICADO TdZdd] nodos = " << unifiedPool.size() << "\n";

    std::cout << "\n=== AUDIT SUMMARY ===\n";
    std::cout << "lists processed             = " << stats.totalLists << "\n";
    std::cout << "values packed               = " << stats.totalPackedValues << "\n";
    std::cout << "unsorted packed             = " << stats.packedUnsortedLists << "\n";
    std::cout << "duplicates packed           = " << stats.packedDuplicates << "\n";
    std::cout << "dd level skips master       = " << stats.masterLevelSkips << "\n";
    std::cout << "dd level skips rel          = " << stats.relLevelSkips << "\n";

    // ---------------- Auditoria de termino (igual al base) -------------------
    if (queryTerm >= 0 && static_cast<uint32_t>(queryTerm) < nlistsPacked) {
        std::cout << "\n=== TERM QUERY AUDIT ===\n";
        std::cout << "term_id = " << queryTerm << "\n";
        printVectorSample(queryPackedPosting, 24, "packed posting sample");
        std::cout << "decoded packed sample (master,rel): ";
        size_t show = std::min<size_t>(12, queryPackedPosting.size());
        for (size_t i = 0; i < show; ++i) {
            uint64_t packed = queryPackedPosting[i];
            std::cout << "(" << ZDD_UNPACK_MASTER(packed) << "," << ZDD_UNPACK_REL(packed) << ")";
            if (i + 1 < show) std::cout << " ";
        }
        if (queryPackedPosting.size() > show) std::cout << " ...";
        std::cout << "\n";
        if (queryMaster >= 0 && queryRel >= 0) {
            uint64_t needle = ZDD_PACK(static_cast<uint32_t>(queryMaster), static_cast<uint32_t>(queryRel));
            std::unordered_set<uint64_t> ps(queryPackedPosting.begin(), queryPackedPosting.end());
            std::cout << "lookup packed(" << queryMaster << "," << queryRel << ") -> "
                      << (ps.count(needle) ? "FOUND" : "NOT FOUND") << "\n";
        }
    }

    // ---------------- Vocabulario + consultas dobles -------------------------
    Vocabulary voc;
    bool hasVocab = loadVocabulary(vocabPath, voc);
    std::cout << "\n=== VOCABULARIO ===\n";
    if (hasVocab) {
        std::cout << "[OK] vocab cargado: " << voc.nwords << " palabras\n";
        if (voc.nwords != nlistsPacked)
            std::cout << "[WARN] nwords(" << voc.nwords << ") != listas(" << nlistsPacked << ").\n";
    } else {
        std::cout << "[WARN] vocab no disponible en " << vocabPath << "\n";
    }

    auto printDobleConsulta = [&](uint32_t tid) {
        if (tid >= termZdds.size()) {
            std::cout << "[INFO] term_id fuera de rango procesado.\n";
            return;
        }
        std::vector<uint32_t> masters = elementsOf(termZdds[tid]);
        printSetSample("PRIMER PISO  S_t", masters);
        std::cout << "SEGUNDO PISO R_{t,d} por master (muestra):\n";
        const auto& phiTerm = phiByTerm[tid];
        size_t show = std::min<size_t>(8, phiTerm.size());
        for (size_t i = 0; i < show; ++i) {
            uint32_t master = phiTerm[i].first;
            uint32_t pid = phiTerm[i].second;
            std::vector<uint32_t> rels = elementsOf(patternPool[pid]);
            std::cout << "   master " << master << " -> patron#" << pid;
            printSetSample(" R", rels, 16);
        }
        if (phiTerm.size() > show)
            std::cout << "   ... (" << phiTerm.size() << " masters en total)\n";
    };

    if (!queryWord.empty()) {
        std::cout << "\n=== CONSULTA DOBLE POR PALABRA ===\n";
        std::cout << "palabra = \"" << queryWord << "\"\n";
        if (!hasVocab) {
            std::cout << "[ERR] vocab no disponible.\n";
        } else {
            auto it = voc.word2id.find(queryWord);
            if (it == voc.word2id.end()) std::cout << "[INFO] palabra no encontrada.\n";
            else {
                std::cout << "term_id = " << it->second << "\n";
                printDobleConsulta(it->second);
            }
        }
    }

    if (queryTerm >= 0 && static_cast<size_t>(queryTerm) < termZdds.size()) {
        std::cout << "\n[OK] Indices construidos; mostrando consulta (salida truncada).\n";
        std::cout.flush();
        std::cout << "\n=== CONSULTA DOBLE POR TERM_ID ===\n";
        std::cout << "term_id = " << queryTerm;
        if (hasVocab && static_cast<size_t>(queryTerm) < voc.words.size())
            std::cout << " -> palabra \"" << voc.words[queryTerm] << "\"";
        std::cout << "\n";
        printDobleConsulta(static_cast<uint32_t>(queryTerm));
    }

    if (dumpDot) {
        std::ofstream o1("resultados_test/piso2_tdzdd_primer_piso.dot");
        if (o1.good()) dd.dumpDot(o1, "Piso2TdzddPrimerPiso");
        std::ofstream o2("resultados_test/piso2_tdzdd_pool_unificado.dot");
        if (o2.good()) unifiedPool.dumpDot(o2, "Piso2TdzddPoolUnificado");
        std::cout << "\n[OK] DOT escritos en resultados_test/piso2_tdzdd_*.dot\n";
    }

    // ---------------- Metricas CSV -------------------------------------------
    auto wallEnd = std::chrono::steady_clock::now();
    tdzdd::ResourceUsage usageEnd;
    tdzdd::ResourceUsage usageDiff = usageEnd - usageStart;
    double elapsedS = std::chrono::duration_cast<std::chrono::duration<double>>(wallEnd - wallStart).count();

    std::ofstream metrics(metricsCsvPath, std::ios::app);
    if (metrics.good()) {
        if (metrics.tellp() == 0) {
            metrics << "packed_docs,lists_processed,dd_nodes_pre,dd_nodes_post,values_packed,"
                    << "pairs_td,distinct_patterns,pool_indep_nodes,pool_indep_bytes,"
                    << "pool_unified_tdzdd_nodes,max_rel,elapsed_s,cpu_s,peak_mem_kb\n";
        }
        metrics << packedDocsPath << "," << stats.totalLists << "," << ddPreNodes << ","
                << ddPostNodes << "," << stats.totalPackedValues << "," << totalPairs << ","
                << distinctPatterns << "," << poolIndepNodes << "," << poolIndepBytes << ","
                << unifiedPool.size() << "," << maxRelSeen << "," << elapsedS << ","
                << usageDiff.utime << "," << usageDiff.maxrss << "\n";
        std::cout << "\n[OK] metrics csv actualizado: " << metricsCsvPath << "\n";
    } else {
        std::cout << "\n[WARN] no se pudo escribir metrics csv: " << metricsCsvPath << "\n";
    }

    imprimirReporteMemoria("AUDITORIA MEMORIA - PRIMER PISO (union global)", dd);
    imprimirReporteMemoria("AUDITORIA MEMORIA - POOL UNIFICADO 2do PISO (TdZdd)", unifiedPool);

    std::cout << "\nFinalizado (piso2, backend=tdzdd).\n";
    return 0;
}

#ifdef HAVE_CUDD

static DdNode* buildSingletonZddCudd(DdManager* dd, const std::vector<uint32_t>& rels) {
    DdNode* p = Cudd_ReadOne(dd);
    Cudd_Ref(p);
    for (uint32_t r : rels) {
        DdNode* tmp = Cudd_zddChange(dd, p, static_cast<int>(r));
        if (tmp == nullptr) {
            Cudd_RecursiveDerefZdd(dd, p);
            return nullptr;
        }
        Cudd_Ref(tmp);
        Cudd_RecursiveDerefZdd(dd, p);
        p = tmp;
    }
    return p;
}

static int run_piso2_cudd(int argc, char** argv) {
    std::string packedDocsPath = "/root/MAGISTER/resultados_test/wiki_100mb_uihrdc_packed.docs";
    std::string mappingPath = "/root/MAGISTER/uiHRDC/uiHRDC/data/texts/page_mapping_wiki_100mb.bin";
    int queryTerm = 1;
    int queryMaster = 13;
    int queryRel = 1730;
    uint32_t progressEvery = 500;
    bool dumpDot = false;
    std::string metricsCsvPath = "resultados_test/piso2_cudd_metrics.csv";
    uint32_t maxTerms = 0;
    std::string vocabPath = "/root/MAGISTER/uiHRDC/uiHRDC/data/texts/index_wiki_100mb_named.voc";
    std::string queryWord = "";

    auto endsWith = [](const std::string& s, const std::string& suf) {
        return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
    };

    bool legacyCli = (argc >= 3) && endsWith(argv[2], ".docs");
    if (legacyCli) {
        if (argc >= 3) packedDocsPath = argv[2];
        if (argc >= 4) mappingPath = argv[3];
        if (argc >= 5) queryTerm = std::atoi(argv[4]);
        if (argc >= 6) queryMaster = std::atoi(argv[5]);
        if (argc >= 7) queryRel = std::atoi(argv[6]);
        if (argc >= 8) progressEvery = static_cast<uint32_t>(std::strtoul(argv[7], nullptr, 10));
        if (argc >= 9) dumpDot = (std::atoi(argv[8]) != 0);
        if (argc >= 10) metricsCsvPath = argv[9];
        if (argc >= 11) maxTerms = static_cast<uint32_t>(std::strtoul(argv[10], nullptr, 10));
        if (argc >= 12) vocabPath = argv[11];
        if (argc >= 13) queryWord = argv[12];
    } else {
        if (argc >= 2) packedDocsPath = argv[1];
        if (argc >= 3) mappingPath = argv[2];
        if (argc >= 4) queryTerm = std::atoi(argv[3]);
        if (argc >= 5) queryMaster = std::atoi(argv[4]);
        if (argc >= 6) queryRel = std::atoi(argv[5]);
        if (argc >= 7) progressEvery = static_cast<uint32_t>(std::strtoul(argv[6], nullptr, 10));
        if (argc >= 8) dumpDot = (std::atoi(argv[7]) != 0);
        if (argc >= 9) metricsCsvPath = argv[8];
        if (argc >= 10) maxTerms = static_cast<uint32_t>(std::strtoul(argv[9], nullptr, 10));
        if (argc >= 11) vocabPath = argv[10];
        if (argc >= 12) queryWord = argv[11];
    }

    auto wallStart = std::chrono::steady_clock::now();

    std::cout << "=== PISO 2 (backend=cudd) ===\n";
    std::cout << "[CFG] cli_mode    = " << (legacyCli ? "legacy(global+packed)" : "packed_only") << "\n";
    std::cout << "[CFG] packed_docs = " << packedDocsPath << "\n";
    std::cout << "[CFG] page_map    = " << mappingPath << "\n";
    std::cout << "[CFG] query_term  = " << queryTerm << "\n";
    std::cout << "[CFG] query_pair  = (" << queryMaster << "," << queryRel << ")\n";
    std::cout << "[CFG] progress    = " << progressEvery << "\n";
    std::cout << "[CFG] metrics_csv = " << metricsCsvPath << "\n";
    std::cout << "[CFG] max_terms   = " << maxTerms << "\n";
    std::cout << "[CFG] vocab       = " << vocabPath << "\n";
    std::cout << "[CFG] query_word  = " << (queryWord.empty() ? "(ninguna)" : queryWord) << "\n";
    std::cout << "[CFG] bit_split   = " << ZDD_MASTER_BITS << "/" << ZDD_REL_BITS << "\n\n";

    DocsMeta packedMeta;
    if (!loadDocsMeta(packedDocsPath, packedMeta)) {
        std::cerr << "Error: falta metadata (" << packedDocsPath << ".meta).\n";
        return 1;
    }
    uint32_t metaMasterBits = 0, metaRelBits = 0;
    if (!metaGetU32(packedMeta, "master_bits", metaMasterBits) ||
        !metaGetU32(packedMeta, "rel_bits", metaRelBits)) {
        std::cerr << "Error: packed meta no trae master_bits/rel_bits.\n";
        return 1;
    }
    if (metaMasterBits != ZDD_MASTER_BITS || metaRelBits != ZDD_REL_BITS) {
        std::cerr << "Error: split incompatible.\n";
        return 1;
    }
    if (metaGetStr(packedMeta, "tuple_output", "") != "packed") {
        std::cerr << "Error: tuple_output != packed.\n";
        return 1;
    }
    std::cout << "[META] packed meta = FOUND; split validado.\n\n";

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
        std::cout << "[WARN] no se pudo cargar page_map (no bloquea master-only).\n";

    uint32_t termsToProcess = nlistsPacked;
    if (maxTerms > 0 && maxTerms < nlistsPacked) termsToProcess = maxTerms;

    std::vector<std::vector<uint64_t>> postings;
    postings.reserve(termsToProcess);
    uint32_t maxRelSeen = 0;
    uint64_t totalPackedValues = 0;
    {
        std::vector<uint64_t> posting;
        for (uint32_t term = 0; term < termsToProcess; ++term) {
            if (!readPisaPostingList(packedIn, posting)) {
                std::cerr << "Error leyendo posting packed en term " << term << ".\n";
                return 1;
            }
            totalPackedValues += posting.size();
            for (uint64_t packed : posting) {
                uint32_t rel = static_cast<uint32_t>(ZDD_UNPACK_REL(packed));
                if (rel > maxRelSeen) maxRelSeen = rel;
            }
            postings.push_back(posting);
        }
    }
    int numZddVars = static_cast<int>(maxRelSeen) + 1;
    std::cout << "[OK] max rel observado = " << maxRelSeen
              << " -> variables ZDD = " << numZddVars << "\n\n";

    DdManager* dd = Cudd_Init(0, numZddVars, CUDD_UNIQUE_SLOTS, CUDD_CACHE_SLOTS, 0);
    if (dd == nullptr) {
        std::cerr << "Error: no se pudo inicializar CUDD.\n";
        return 1;
    }

    DdNode* pool = Cudd_ReadZero(dd);
    Cudd_Ref(pool);

    std::vector<uint32_t> masterCard(termsToProcess, 0);
    std::unordered_map<uint32_t, DdNode*> queryTermPatterns;
    uint64_t totalPairs = 0;

    for (uint32_t term = 0; term < termsToProcess; ++term) {
        const std::vector<uint64_t>& posting = postings[term];
        std::unordered_map<uint32_t, std::vector<uint32_t>> relsByMaster;
        for (uint64_t packed : posting) {
            uint32_t master = static_cast<uint32_t>(ZDD_UNPACK_MASTER(packed));
            uint32_t rel = static_cast<uint32_t>(ZDD_UNPACK_REL(packed));
            relsByMaster[master].push_back(rel);
        }
        masterCard[term] = static_cast<uint32_t>(relsByMaster.size());
        bool isQueryTerm = (static_cast<int>(term) == queryTerm);

        for (auto& kv : relsByMaster) {
            uint32_t master = kv.first;
            std::vector<uint32_t>& rels = kv.second;
            std::sort(rels.begin(), rels.end());
            rels.erase(std::unique(rels.begin(), rels.end()), rels.end());

            DdNode* patt = buildSingletonZddCudd(dd, rels);
            if (patt == nullptr) {
                std::cerr << "Error construyendo patron ZDD.\n";
                Cudd_Quit(dd);
                return 1;
            }

            DdNode* u = Cudd_zddUnion(dd, pool, patt);
            if (u == nullptr) {
                std::cerr << "Error en Cudd_zddUnion.\n";
                Cudd_Quit(dd);
                return 1;
            }
            Cudd_Ref(u);
            Cudd_RecursiveDerefZdd(dd, pool);
            pool = u;

            if (isQueryTerm) queryTermPatterns[master] = patt;
            else Cudd_RecursiveDerefZdd(dd, patt);
            ++totalPairs;
        }

        if ((term + 1) % progressEvery == 0 || term + 1 == termsToProcess) {
            std::cout << "[PROGRESS] term " << (term + 1) << "/" << termsToProcess
                      << " | pool_nodes=" << Cudd_zddDagSize(pool)
                      << " | pares=" << totalPairs
                      << " | live_nodes=" << Cudd_ReadZddSize(dd) << "\n";
            std::cout.flush();
        }
    }

    int poolNodes = Cudd_zddDagSize(pool);
    double distinctPatterns = Cudd_zddCountDouble(dd, pool);
    unsigned long memInUse = Cudd_ReadMemoryInUse(dd);
    long totalManagerNodes = Cudd_ReadNodeCount(dd);

    std::cout << "\n=== SEGUNDO PISO: POOL UNICO CUDD ===\n";
    std::cout << "pares (t,d) totales         = " << totalPairs << "\n";
    std::cout << "patrones DISTINTOS          = " << std::fixed << std::setprecision(0)
              << distinctPatterns << "\n";
    if (distinctPatterns > 0) {
        std::cout << "factor de deduplicacion     = " << std::fixed << std::setprecision(2)
                  << static_cast<double>(totalPairs) / distinctPatterns << "x\n";
    }
    std::cout << "[POOL UNICO] nodos ZDD      = " << poolNodes << "\n";
    std::cout << "[POOL UNICO] nodos manager  = " << totalManagerNodes << "\n";
    std::cout << "[POOL UNICO] memoria CUDD   = " << memInUse << " B\n";
    std::cout << "max rel observado           = " << maxRelSeen << "\n";

    Vocabulary voc;
    bool hasVocab = loadVocabulary(vocabPath, voc);
    std::cout << "\n=== VOCABULARIO ===\n";
    if (hasVocab) std::cout << "[OK] vocab cargado: " << voc.nwords << " palabras\n";
    else std::cout << "[WARN] vocab no disponible.\n";

    auto relsOfPattern = [&](DdNode* root) {
        std::vector<uint32_t> rels;
        DdNode* cur = root;
        while (!Cudd_IsConstant(cur)) {
            int idx = Cudd_NodeReadIndex(cur);
            rels.push_back(static_cast<uint32_t>(idx));
            cur = Cudd_T(cur);
        }
        std::sort(rels.begin(), rels.end());
        return rels;
    };

    if (queryTerm >= 0 && static_cast<uint32_t>(queryTerm) < termsToProcess) {
        std::cout << "\n=== CONSULTA DOBLE POR TERM_ID ===\n";
        std::cout << "term_id = " << queryTerm;
        if (hasVocab && static_cast<size_t>(queryTerm) < voc.words.size())
            std::cout << " -> palabra \"" << voc.words[queryTerm] << "\"";
        std::cout << "\n";
        std::cout << "PRIMER PISO  |S_t| (masters) = " << masterCard[queryTerm] << "\n";
        size_t shown = 0;
        for (auto& kv : queryTermPatterns) {
            if (shown >= 8) break;
            std::vector<uint32_t> rels = relsOfPattern(kv.second);
            std::cout << "   master " << kv.first;
            printSetSample(" -> R", rels, 16);
            ++shown;
        }
        if (queryMaster >= 0 && queryRel >= 0) {
            auto it = queryTermPatterns.find(static_cast<uint32_t>(queryMaster));
            bool found = false;
            if (it != queryTermPatterns.end()) {
                std::vector<uint32_t> rels = relsOfPattern(it->second);
                found = std::binary_search(rels.begin(), rels.end(), static_cast<uint32_t>(queryRel));
            }
            std::cout << "lookup (master=" << queryMaster << ", rel=" << queryRel << ") -> "
                      << (found ? "FOUND" : "NOT FOUND") << "\n";
        }
    }

    if (!queryWord.empty() && hasVocab) {
        auto it = voc.word2id.find(queryWord);
        if (it != voc.word2id.end()) {
            std::cout << "\n=== CONSULTA POR PALABRA ===\n";
            std::cout << "palabra = \"" << queryWord << "\" -> term_id = " << it->second << "\n";
        }
    }

    if (dumpDot) {
        FILE* f = fopen("resultados_test/piso2_cudd_pool_unico.dot", "w");
        if (f) {
            DdNode* roots[1] = {pool};
            Cudd_zddDumpDot(dd, 1, roots, nullptr, nullptr, f);
            fclose(f);
            std::cout << "\n[OK] DOT escrito en resultados_test/piso2_cudd_pool_unico.dot\n";
        }
    }

    auto wallEnd = std::chrono::steady_clock::now();
    double elapsedS =
        std::chrono::duration_cast<std::chrono::duration<double>>(wallEnd - wallStart).count();

    std::ofstream metrics(metricsCsvPath, std::ios::app);
    if (metrics.good()) {
        if (metrics.tellp() == 0) {
            metrics << "backend,packed_docs,lists_processed,values_packed,pairs_td,distinct_patterns,"
                    << "pool_nodes,manager_live_nodes,cudd_mem_bytes,max_rel,elapsed_s\n";
        }
        metrics << "cudd," << packedDocsPath << "," << termsToProcess << "," << totalPackedValues << ","
                << totalPairs << "," << std::fixed << std::setprecision(0) << distinctPatterns << ","
                << poolNodes << "," << totalManagerNodes << "," << memInUse << ","
                << maxRelSeen << "," << std::setprecision(6) << elapsedS << "\n";
        std::cout << "\n[OK] metrics csv actualizado: " << metricsCsvPath << "\n";
    }

    for (auto& kv : queryTermPatterns) Cudd_RecursiveDerefZdd(dd, kv.second);
    Cudd_RecursiveDerefZdd(dd, pool);
    Cudd_Quit(dd);

    std::cout << "\nFinalizado (piso2, backend=cudd).\n";
    return 0;
}

#endif  // HAVE_CUDD

int main(int argc, char** argv) {
    if (argc < 2) {
        printPiso2Usage();
        return 1;
    }

    std::string first = argv[1];
    Piso2Backend backend = Piso2Backend::TDZDD;
    int argOff = 0;

    if (isBackendToken(first)) {
        backend = parseBackendToken(first);
        argOff = 1;
        if (argc < 3) {
            printPiso2Usage();
            return 1;
        }
    } else if (first.find(".docs") == std::string::npos) {
        printPiso2Usage();
        return 1;
    }

    if (backend == Piso2Backend::CUDD) {
#ifndef HAVE_CUDD
        std::cerr << "Error: backend cudd no disponible. Recompila con -DHAVE_CUDD -lcudd\n";
        return 1;
#else
        return run_piso2_cudd(argc - argOff, argv + argOff);
#endif
    }

    return run_piso2_tdzdd(argc - argOff, argv + argOff);
}
