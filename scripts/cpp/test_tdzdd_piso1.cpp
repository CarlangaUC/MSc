// =============================================================================
// PISO 1 — ZDD de documentos (masters M_t por termino)
// =============================================================================
// Binario base validado: term_id -> S_t (conjunto de masters), consulta por .voc.
//
// Compilar (desde la raíz MAGISTER):
//   g++ -O2 -std=c++17 -o scripts/cpp/test_tdzdd_piso1 scripts/cpp/test_tdzdd_piso1.cpp \
//       -I ./TdZdd/include -lpthread
//
// Ejecutar (packed-only):
//   ./scripts/cpp/test_tdzdd_piso1 \
//     resultados_test/wiki_100mb_uihrdc_packed.docs \
//     uiHRDC/uiHRDC/data/texts/page_mapping_wiki_100mb.bin \
//     1729 13 1730 2000 0 resultados_test/piso1_metrics.csv 0 \
//     uiHRDC/uiHRDC/data/texts/index_wiki_100mb_named.voc Abraham
// =============================================================================

#include <tdzdd/DdSpec.hpp>
#include <tdzdd/DdSpecOp.hpp>
#include <tdzdd/DdStructure.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
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
};

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

static void imprimirReporteMemoriaFinal(const tdzdd::DdStructure<2>& dd) {
    ZddAudit::MemoryStats s = ZddAudit::auditarMemoria(dd);

    double totalKB = static_cast<double>(s.totalBytes) / 1024.0;
    double totalMB = totalKB / 1024.0;
    double bytesPerNode = (dd.size() > 0) ? static_cast<double>(s.totalBytes) / dd.size() : 0.0;

    std::cout << "\n=== AUDITORIA PROFUNDA DE MEMORIA (FINAL) ===\n";
    std::cout << "Nodos logicos (dd.size): " << dd.size() << "\n";
    std::cout << "Niveles fisicos (rows):  " << dd.getDiagram()->numRows() << "\n";
    std::cout << "-------------------------------------------------\n";
    std::cout << " [1] Nodos ZDD (datos):  " << s.nodePayload << " B\n";
    std::cout << " [2] Indices (ints):     " << s.indexPayload << " B (higher/lower)\n";
    std::cout << " [3] Headers vectores:   " << s.vectorHeaders << " B (MyVector)\n";
    std::cout << " [4] Overhead clases:    " << s.classOverhead << " B\n";
    std::cout << "-------------------------------------------------\n";
    std::cout << " TOTAL FISICO:           " << s.totalBytes << " Bytes\n";
    std::cout << "                         " << std::fixed << std::setprecision(2) << totalKB
              << " KB\n";
    std::cout << "                         " << std::fixed << std::setprecision(2) << totalMB
              << " MB\n";
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

struct TermHandle {
    uint32_t termId = 0;
    size_t packedCardinality = 0;
    size_t masterCardinality = 0;
};

static bool loadDocsMeta(const std::string& docsPath, DocsMeta& out) {
    out = DocsMeta{};
    std::ifstream in(docsPath + ".meta");
    if (!in.is_open()) return false;
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        out.kv[k] = v;
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
    if (p + len > 32u) {
        answ |= static_cast<uint64_t>(*(e + 1)) << (32u - p);
    }
    if (len < 32u) {
        answ &= ((static_cast<uint64_t>(1) << len) - 1u);
    }
    return static_cast<uint32_t>(answ);
}

// Vocabulario uiHRDC (.voc): term_id -> palabra y palabra -> term_id.
// Formato (ver ii.c saveVocabulary): nwords(uint32), elemSize(uint32),
// zoneSize(uint32), zona ascii (zoneSize bytes), offsets bit-packed (nwords+1).
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
        if (nxt < off || nxt > zoneSize) {
            return false;
        }
        voc.words[i].assign(reinterpret_cast<const char*>(zone.data()) + off, nxt - off);
        voc.word2id.emplace(voc.words[i], i);
    }
    voc.nwords = nwords;
    voc.loaded = true;
    return true;
}

// Enumera S_t (masters) a partir del handle ZDD de un termino.
// Cada nivel guardado es master+1, por lo que se resta 1 al imprimir/listar.
static std::vector<uint32_t> mastersOf(const tdzdd::DdStructure<2>& z) {
    std::set<uint32_t> acc;
    for (auto const& s : z) {
        for (int lvl : s) {
            if (lvl >= 1) acc.insert(static_cast<uint32_t>(lvl - 1));
        }
    }
    return std::vector<uint32_t>(acc.begin(), acc.end());
}

int main(int argc, char** argv) {
    // Config por defecto (se puede sobreescribir por CLI).
    // Por ahora se usa SOLO packed.docs para construir una EDD de 1 nivel (GLOBAL_VERSION).
    std::string globalDocsPath = "N/A";
    std::string packedDocsPath = "/root/MAGISTER/resultados_test/wiki_100mb_uihrdc_packed.docs";
    std::string mappingPath = "/root/MAGISTER/uiHRDC/uiHRDC/data/texts/page_mapping_wiki_100mb.bin";
    int queryTerm = 1;      // termino para auditoria fina
    int queryMaster = 13;   // consulta especifica (master,rel)
    int queryRel = 1730;
    uint32_t progressEvery = 500;
    bool dumpDot = false;  // solo DOT final post-reduce; nunca por termino/paso
    std::string metricsCsvPath = "resultados_test/two_floor_metrics.csv";
    uint32_t maxTerms = 0;  // 0 = todos los terminos
    bool singleFloorMasterOnly = true;
    // Vocabulario uiHRDC para mapear term_id <-> palabra y consultar por texto.
    std::string vocabPath = "/root/MAGISTER/uiHRDC/uiHRDC/data/texts/index_wiki_100mb_named.voc";
    std::string queryWord = "";  // si no vacio: "dame docs donde aparece <palabra>"
    auto endsWith = [](const std::string& s, const std::string& suf) {
        return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
    };

    // CLI nuevo (recomendado): packed.docs primero, sin global.docs.
    // CLI legado: global.docs packed.docs page_map ... (se mantiene compatibilidad).
    bool legacyCli = (argc >= 3) && endsWith(argv[2], ".docs");
    if (legacyCli) {
        if (argc >= 3) {
            globalDocsPath = argv[1];
            packedDocsPath = argv[2];
        }
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

    std::cout << "=== TDZDD TWO FLOOR BUILDER (PISA x2) ===\n";
    std::cout << "[CFG] cli_mode    = " << (legacyCli ? "legacy(global+packed)" : "packed_only") << "\n";
    std::cout << "[CFG] global_docs = " << globalDocsPath << "\n";
    std::cout << "[CFG] packed_docs = " << packedDocsPath << "\n";
    std::cout << "[CFG] page_map    = " << mappingPath << "\n";
    std::cout << "[CFG] query_term  = " << queryTerm << "\n";
    std::cout << "[CFG] query_pair  = (" << queryMaster << "," << queryRel << ")\n";
    std::cout << "[CFG] progress    = " << progressEvery << "\n";
    std::cout << "[CFG] dump_dot    = " << (dumpDot ? "1" : "0") << "\n";
    std::cout << "[CFG] metrics_csv = " << metricsCsvPath << "\n";
    std::cout << "[CFG] max_terms   = " << maxTerms << "\n";
    std::cout << "[CFG] mode        = "
              << (singleFloorMasterOnly ? "single_floor_master_only" : "two_floor") << "\n";
    std::cout << "[CFG] vocab       = " << vocabPath << "\n";
    std::cout << "[CFG] query_word  = " << (queryWord.empty() ? "(ninguna)" : queryWord) << "\n";
    std::cout << "[CFG] bit_split   = " << ZDD_MASTER_BITS << "/" << ZDD_REL_BITS << "\n\n";

    DocsMeta packedMeta;
    bool hasPackedMeta = loadDocsMeta(packedDocsPath, packedMeta);
    std::cout << "[META] packed meta = " << (hasPackedMeta ? "FOUND" : "MISSING") << "\n";
    if (!hasPackedMeta) {
        std::cerr << "Error: falta metadata para packed docs (" << packedDocsPath << ".meta).\n"
                  << "Regenera con el script convertir_versionado_input_uiHRDC.py.\n";
        return 1;
    }

    uint32_t metaMasterBits = 0;
    uint32_t metaRelBits = 0;
    bool hasMetaMasterBits = metaGetU32(packedMeta, "master_bits", metaMasterBits);
    bool hasMetaRelBits = metaGetU32(packedMeta, "rel_bits", metaRelBits);
    std::string packedTupleOutput = metaGetStr(packedMeta, "tuple_output", "");

    if (!hasMetaMasterBits || !hasMetaRelBits) {
        std::cerr << "Error: packed meta no trae master_bits/rel_bits.\n";
        return 1;
    }
    if (metaMasterBits != ZDD_MASTER_BITS || metaRelBits != ZDD_REL_BITS) {
        std::cerr << "Error: split incompatible.\n"
                  << "  packed meta  = " << metaMasterBits << "/" << metaRelBits << "\n"
                  << "  binario actual= " << ZDD_MASTER_BITS << "/" << ZDD_REL_BITS << "\n"
                  << "Regenera packed docs o recompila test_tdzdd_piso1 con el split correcto.\n";
        return 1;
    }
    if (packedTupleOutput != "packed") {
        std::cerr << "Error: packed docs no fue generado con tuple_output=packed (valor='"
                  << packedTupleOutput << "').\n";
        return 1;
    }
    std::cout << "[META] packed tuple_output = " << packedTupleOutput << "\n";
    std::cout << "[META] split validated against binary.\n\n";
    std::ifstream packedIn(packedDocsPath, std::ios::binary);
    if (!packedIn.is_open()) {
        std::cerr << "Error: no se pudo abrir packed_docs.\n";
        return 1;
    }

    uint32_t nlistsPacked = 0;
    if (!readPisaHeader(packedIn, nlistsPacked)) {
        std::cerr << "Error: no se pudo leer header PISA de packed_docs.\n";
        return 1;
    }
    std::cout << "[OK] header packed lists = " << nlistsPacked << "\n";

    std::vector<uint32_t> mapping;
    bool hasMapping = loadPageMapping(mappingPath, mapping);
    if (hasMapping) {
        std::cout << "[OK] page_map entries = " << mapping.size() << "\n";
    } else {
        std::cout << "[WARN] no se pudo cargar page_map (no bloquea el modo master-only).\n";
    }
    std::cout << "\n";

    tdzdd::DdStructure<2> dd;
    AuditStats stats;

    std::vector<uint64_t> packedPosting;
    std::vector<uint64_t> queryPackedPosting;
    std::vector<TermHandle> termHandles;
    termHandles.reserve(nlistsPacked);
    // handle: t -> ZDD propia de S_t (masters). Permite consultas O(1) de acceso
    // al conjunto del termino y O(k) para enumerar sus documentos maestros.
    std::vector<tdzdd::DdStructure<2>> termZdds;
    termZdds.reserve(nlistsPacked);

    uint32_t termsToProcess = nlistsPacked;
    if (maxTerms > 0 && maxTerms < nlistsPacked) {
        termsToProcess = maxTerms;
    }

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

        std::set<int> masterSet;
        std::set<int> packedSet;

        for (uint64_t packed : packedPosting) {
            int levelPacked = 0;
            if (toDdLevel(packed + 1u, levelPacked)) {
                packedSet.insert(levelPacked);
            } else {
                ++stats.ddLevelSkipsPacked;
            }

            uint32_t master = static_cast<uint32_t>(ZDD_UNPACK_MASTER(packed));
            int levelMaster = 0;
            if (toDdLevel(static_cast<uint64_t>(master) + 1u, levelMaster)) {
                masterSet.insert(levelMaster);
            } else {
                ++stats.masterLevelSkips;
            }
        }

        // ZDD propia del termino (S_t sobre masters), reducida: este es el handle.
        FastPathSpec termSpec(masterSet);
        tdzdd::DdStructure<2> termZdd(termSpec);
        termZdd.zddReduce();
        termZdds.push_back(termZdd);

        // Acumular en la ZDD global compartida (familia de conjuntos).
        dd = tdzdd::DdStructure<2>(tdzdd::zddUnion(dd, termZdd));

        TermHandle handle;
        handle.termId = term;
        handle.packedCardinality = packedSet.size();
        handle.masterCardinality = masterSet.size();
        termHandles.push_back(handle);

        if (static_cast<int>(term) == queryTerm) {
            queryPackedPosting = packedPosting;
        }

        if ((term + 1) % progressEvery == 0 || term + 1 == termsToProcess) {
            std::cout << "[PROGRESS] term " << (term + 1) << "/" << termsToProcess
                      << " | nodes(dd)=" << dd.size() << "\n";
        }
    }

    std::cout << "\n=== PRE-REDUCE ===\n";
    size_t ddPreNodes = dd.size();
    std::cout << "dd nodes = " << ddPreNodes << "\n";

    dd.zddReduce();

    std::cout << "\n=== POST-REDUCE ===\n";
    size_t ddPostNodes = dd.size();
    std::cout << "dd nodes = " << ddPostNodes << "\n";

    std::cout << "\n=== AUDIT SUMMARY ===\n";
    std::cout << "lists processed             = " << stats.totalLists << "\n";
    std::cout << "values packed               = " << stats.totalPackedValues << "\n";
    std::cout << "unsorted packed             = " << stats.packedUnsortedLists << "\n";
    std::cout << "duplicates packed           = " << stats.packedDuplicates << "\n";
    std::cout << "dd level skips packed       = " << stats.ddLevelSkipsPacked << "\n";
    std::cout << "dd level skips master       = " << stats.masterLevelSkips << "\n";

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
            uint64_t packedNeedle = ZDD_PACK(static_cast<uint32_t>(queryMaster), static_cast<uint32_t>(queryRel));
            std::unordered_set<uint64_t> packedSet(queryPackedPosting.begin(), queryPackedPosting.end());
            bool foundPacked = packedSet.find(packedNeedle) != packedSet.end();
            std::cout << "lookup packed(" << queryMaster << "," << queryRel
                      << ") -> " << (foundPacked ? "FOUND" : "NOT FOUND") << "\n";
        }
    } else {
        std::cout << "\n[WARN] query_term fuera de rango; se omite auditoria de termino.\n";
    }

    std::cout << "\n=== TERM->HANDLE SAMPLE ===\n";
    std::cout << "handles almacenados = " << termHandles.size() << "\n";
    size_t handleShow = std::min<size_t>(5, termHandles.size());
    for (size_t i = 0; i < handleShow; ++i) {
        const TermHandle& h = termHandles[i];
        std::cout << "term " << h.termId
                  << " -> (master_card=" << h.masterCardinality
                  << ", packed_card=" << h.packedCardinality << ")\n";
    }

    // Vocabulario uiHRDC: term_id <-> palabra (para consultas por texto).
    Vocabulary voc;
    bool hasVocab = loadVocabulary(vocabPath, voc);
    std::cout << "\n=== VOCABULARIO ===\n";
    if (hasVocab) {
        std::cout << "[OK] vocab cargado: " << voc.nwords << " palabras\n";
        if (voc.nwords != nlistsPacked) {
            std::cout << "[WARN] nwords(" << voc.nwords << ") != listas("
                      << nlistsPacked << "); el mapeo term_id<->palabra podria no alinear.\n";
        }
        size_t vocShow = std::min<size_t>(5, voc.words.size());
        for (size_t i = 0; i < vocShow; ++i) {
            std::cout << "  term_id " << i << " -> \"" << voc.words[i] << "\"\n";
        }
    } else {
        std::cout << "[WARN] no se pudo cargar vocab en " << vocabPath
                  << " (las consultas por palabra quedan deshabilitadas).\n";
    }

    // Consulta directa: "dame todos los documentos (masters) donde aparece <palabra>".
    // Camino: palabra -> term_id (O(1)) -> handle[term_id] -> S_t -> enumerar (O(k)).
    if (!queryWord.empty()) {
        std::cout << "\n=== CONSULTA POR PALABRA ===\n";
        std::cout << "palabra = \"" << queryWord << "\"\n";
        if (!hasVocab) {
            std::cout << "[ERR] vocab no disponible; no se puede resolver la palabra.\n";
        } else {
            auto it = voc.word2id.find(queryWord);
            if (it == voc.word2id.end()) {
                std::cout << "[INFO] palabra no encontrada en el vocabulario.\n";
            } else {
                uint32_t tid = it->second;
                std::cout << "term_id = " << tid << " (acceso al handle O(1))\n";
                if (tid < termZdds.size()) {
                    std::vector<uint32_t> docs = mastersOf(termZdds[tid]);
                    std::cout << "documentos maestros donde aparece (|S_t|=" << docs.size() << "): { ";
                    for (size_t i = 0; i < docs.size(); ++i) {
                        std::cout << docs[i];
                        if (i + 1 < docs.size()) std::cout << ", ";
                    }
                    std::cout << " }\n";
                } else {
                    std::cout << "[INFO] term_id fuera del rango procesado (max_terms="
                              << termsToProcess << "); no hay handle construido.\n";
                }
            }
        }
    }

    // Consulta equivalente por term_id (usa el mismo handle ZDD).
    if (queryTerm >= 0 && static_cast<size_t>(queryTerm) < termZdds.size()) {
        std::cout << "\n=== CONSULTA POR TERM_ID (via handle) ===\n";
        std::cout << "term_id = " << queryTerm;
        if (hasVocab && static_cast<size_t>(queryTerm) < voc.words.size()) {
            std::cout << " -> palabra \"" << voc.words[queryTerm] << "\"";
        }
        std::cout << "\n";
        std::vector<uint32_t> docs = mastersOf(termZdds[queryTerm]);
        std::cout << "documentos maestros (|S_t|=" << docs.size() << "): { ";
        for (size_t i = 0; i < docs.size(); ++i) {
            std::cout << docs[i];
            if (i + 1 < docs.size()) std::cout << ", ";
        }
        std::cout << " }\n";
    }

    // Un unico volcado al terminar (post-reduce). No hay DOTs intermedios por termino.
    if (dumpDot) {
        std::ofstream outDd("resultados_test/one_floor_master.dot");
        if (outDd.good()) dd.dumpDot(outDd, "OneFloorMaster");
        std::cout << "\n[OK] DOT final escrito en resultados_test/one_floor_master.dot\n";
    }

    std::cout << "\n=== ZDD POR TERMINO (S_t / master_id) ===\n";
    std::cout << "ZDD global unificada: " << dd.size() << " nodos logicos\n";
    std::cout << "Cada linea es S_t = masters unicos donde aparece el termino t.\n";
    std::cout << "Valores = master_id (GLOBAL_VERSION); NO docid global page_map[master]+rel.\n";

    const size_t maxPrintSets = 30;
    std::vector<uint32_t> allTermIds(termHandles.size());
    std::iota(allTermIds.begin(), allTermIds.end(), 0u);

    const size_t sampleCount = std::min(maxPrintSets, allTermIds.size());
    std::vector<uint32_t> sampleTerms(sampleCount);
    std::random_device rd;
    std::mt19937 rng(rd());
    std::sample(allTermIds.begin(), allTermIds.end(), sampleTerms.begin(), sampleCount, rng);
    std::sort(sampleTerms.begin(), sampleTerms.end());
    for (uint32_t t : sampleTerms) {
        std::vector<uint32_t> docs = mastersOf(termZdds[t]);
        std::cout << "  term_id=" << t;
        if (hasVocab && t < voc.words.size()) {
            std::cout << " (\"" << voc.words[t] << "\")";
        }
        std::cout << ": { ";
        for (size_t i = 0; i < docs.size(); ++i) {
            std::cout << docs[i];
            if (i + 1 < docs.size()) std::cout << ", ";
        }
        std::cout << " }\n";
    }
    if (sampleTerms.empty()) {
        std::cout << "  (sin terminos procesados)\n";
    } else {
        std::cout << "Mostrando " << sampleTerms.size() << " terminos (muestra aleatoria de S_t).\n";
    }

    auto wallEnd = std::chrono::steady_clock::now();
    tdzdd::ResourceUsage usageEnd;
    tdzdd::ResourceUsage usageDiff = usageEnd - usageStart;
    double elapsedS = std::chrono::duration_cast<std::chrono::duration<double>>(wallEnd - wallStart).count();

    std::ofstream metrics(metricsCsvPath, std::ios::app);
    if (metrics.good()) {
        if (metrics.tellp() == 0) {
            metrics << "global_docs,packed_docs,lists_processed,dd_nodes_pre,dd_nodes_post,"
                    << "values_packed,dd_skips_packed,dd_skips_master,elapsed_s,cpu_s,peak_mem_kb\n";
        }
        metrics << globalDocsPath << "," << packedDocsPath << "," << stats.totalLists << ","
                << ddPreNodes << "," << ddPostNodes << ","
                << stats.totalPackedValues << ","
                << stats.ddLevelSkipsPacked << "," << stats.masterLevelSkips << ","
                << elapsedS << "," << usageDiff.utime << "," << usageDiff.maxrss << "\n";
        std::cout << "[OK] metrics csv actualizado: " << metricsCsvPath << "\n";
    } else {
        std::cout << "[WARN] no se pudo escribir metrics csv: " << metricsCsvPath << "\n";
    }

    imprimirReporteMemoriaFinal(dd);

    std::cout << "\nFinalizado.\n";
    return 0;
}
