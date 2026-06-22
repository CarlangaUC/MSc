#include <tdzdd/DdSpec.hpp>
#include <tdzdd/DdSpecOp.hpp>
#include <tdzdd/DdStructure.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "uiHRDC/uiHRDC/indexes/NOPOS/II_docs/src/utils/version_packing.h"

// =============================================================================
// DOBLE PISO INTEGRADO — M_t (documentos) + pool R_{t,d} (versiones) + Phi
// =============================================================================
// Implementacion unificada de la logica de tesis (TdZdd):
//   - Piso 1: ZDD de masters M_t por termino.
//   - Piso 2: pool de patrones de versiones R_{t,d} con deduplicacion.
//   - Phi(t,d): mapeo disperso termino-master -> patron en el pool.
//   - Incluye intersecciones demo entre terminos (M_t y R_{t,d}).
//
// Compilar (desde la raíz MAGISTER):
//   g++ -O2 -std=c++17 -o scripts/cpp/test_tdzdd_doble_piso scripts/cpp/test_tdzdd_doble_piso.cpp \
//       -I ./TdZdd/include -lpthread
//
// Ejecutar:
//   ./scripts/cpp/test_tdzdd_doble_piso \
//     resultados_test/wiki_100mb_uihrdc_packed.docs \
//     uiHRDC/uiHRDC/data/texts/page_mapping_wiki_100mb.bin \
//     1729 4633 13 1730 500 0 resultados_test/doble_piso_metrics.csv
// =============================================================================

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

struct DocsMeta {
    bool loaded = false;
    std::unordered_map<std::string, std::string> kv;
};

struct PatternEntry {
    tdzdd::DdStructure<2> zdd;
    uint64_t useCount = 0;
    size_t cardinality = 0;
};

struct BuildStats {
    uint64_t listsProcessed = 0;
    uint64_t valuesPacked = 0;
    uint64_t totalPairsTD = 0;
    uint64_t packedDuplicates = 0;
    uint64_t packedUnsortedLists = 0;
    uint64_t ddLevelSkipsMaster = 0;
    uint64_t ddLevelSkipsRel = 0;
};

static constexpr size_t kPrintMax = 24;

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

static bool toDdLevel(uint64_t v, int& out) {
    if (v == 0 || v > static_cast<uint64_t>(std::numeric_limits<int>::max())) return false;
    out = static_cast<int>(v);
    return true;
}

static void auditSortedDedup(const std::vector<uint64_t>& v, bool& unsorted, uint64_t& duplicates) {
    unsorted = false;
    if (v.size() < 2) return;
    for (size_t i = 1; i < v.size(); ++i) {
        if (v[i] < v[i - 1]) unsorted = true;
        if (v[i] == v[i - 1]) ++duplicates;
    }
}

static std::string patternKey(const std::vector<uint32_t>& rels) {
    std::string key;
    key.resize(rels.size() * sizeof(uint32_t));
    if (!rels.empty()) std::memcpy(&key[0], rels.data(), key.size());
    return key;
}

static std::vector<uint32_t> elementsOf(const tdzdd::DdStructure<2>& z) {
    std::set<uint32_t> acc;
    for (auto const& s : z) {
        for (int lvl : s) {
            if (lvl >= 1) acc.insert(static_cast<uint32_t>(lvl - 1));
        }
    }
    return std::vector<uint32_t>(acc.begin(), acc.end());
}

static void printSetSample(const std::string& label, const std::vector<uint32_t>& v, size_t maxShow = kPrintMax) {
    std::cout << label << " (|set|=" << v.size() << "): { ";
    size_t show = std::min(maxShow, v.size());
    for (size_t i = 0; i < show; ++i) {
        std::cout << v[i];
        if (i + 1 < show) std::cout << ", ";
    }
    if (v.size() > show) std::cout << " ...";
    std::cout << " }\n";
}

static bool findPatternIdForMaster(
    const std::vector<std::pair<uint32_t, uint32_t>>& phiTerm,
    uint32_t master,
    uint32_t& outPid) {
    for (const auto& md : phiTerm) {
        if (md.first == master) {
            outPid = md.second;
            return true;
        }
    }
    return false;
}

int main(int argc, char** argv) {
    std::string packedDocsPath = "/root/MAGISTER/resultados_test/wiki_100mb_uihrdc_packed.docs";
    std::string mappingPath = "/root/MAGISTER/uiHRDC/uiHRDC/data/texts/page_mapping_wiki_100mb.bin";
    int termA = 1729;
    int termB = 4633;
    int queryMaster = 13;
    int queryRel = 1730;
    uint32_t progressEvery = 500;
    uint32_t maxTerms = 0;
    std::string metricsCsvPath = "resultados_test/add_pool_metrics.csv";

    if (argc >= 2) packedDocsPath = argv[1];
    if (argc >= 3) mappingPath = argv[2];
    if (argc >= 4) termA = std::atoi(argv[3]);
    if (argc >= 5) termB = std::atoi(argv[4]);
    if (argc >= 6) queryMaster = std::atoi(argv[5]);
    if (argc >= 7) queryRel = std::atoi(argv[6]);
    if (argc >= 8) progressEvery = static_cast<uint32_t>(std::strtoul(argv[7], nullptr, 10));
    if (argc >= 9) maxTerms = static_cast<uint32_t>(std::strtoul(argv[8], nullptr, 10));
    if (argc >= 10) metricsCsvPath = argv[9];

    std::cout << "=== DOBLE PISO INTEGRADO (TdZdd) ===\n";
    std::cout << "[CFG] packed_docs = " << packedDocsPath << "\n";
    std::cout << "[CFG] page_map    = " << mappingPath << "\n";
    std::cout << "[CFG] termA/termB = " << termA << "/" << termB << "\n";
    std::cout << "[CFG] query pair  = (master=" << queryMaster << ", rel=" << queryRel << ")\n";
    std::cout << "[CFG] progress    = " << progressEvery << "\n";
    std::cout << "[CFG] max_terms   = " << maxTerms << "\n";
    std::cout << "[CFG] metrics_csv = " << metricsCsvPath << "\n";
    std::cout << "[CFG] bit_split   = " << ZDD_MASTER_BITS << "/" << ZDD_REL_BITS << "\n\n";

    DocsMeta meta;
    if (!loadDocsMeta(packedDocsPath, meta)) {
        std::cerr << "Error: falta metadata (" << packedDocsPath << ".meta).\n";
        return 1;
    }
    uint32_t metaMasterBits = 0, metaRelBits = 0;
    if (!metaGetU32(meta, "master_bits", metaMasterBits) ||
        !metaGetU32(meta, "rel_bits", metaRelBits)) {
        std::cerr << "Error: meta sin master_bits/rel_bits.\n";
        return 1;
    }
    if (metaMasterBits != ZDD_MASTER_BITS || metaRelBits != ZDD_REL_BITS) {
        std::cerr << "Error: split incompatible. meta=" << metaMasterBits << "/" << metaRelBits
                  << " binario=" << ZDD_MASTER_BITS << "/" << ZDD_REL_BITS << "\n";
        return 1;
    }
    std::string tupleOutput = metaGetStr(meta, "tuple_output", "");
    if (tupleOutput != "packed") {
        std::cerr << "Error: tuple_output != packed (actual='" << tupleOutput << "').\n";
        return 1;
    }
    std::cout << "[META] tuple_output=packed; split validado.\n\n";

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
        std::cout << "[OK] page_map entries = " << mapping.size() << "\n\n";
    else
        std::cout << "[WARN] no se pudo cargar page_map (opcional para este build).\n\n";

    uint32_t termsToProcess = nlistsPacked;
    if (maxTerms > 0 && maxTerms < nlistsPacked) termsToProcess = maxTerms;

    auto wallStart = std::chrono::steady_clock::now();

    BuildStats stats;
    uint32_t maxRelSeen = 0;
    std::vector<tdzdd::DdStructure<2>> docsByTerm;
    docsByTerm.reserve(termsToProcess);

    std::unordered_map<std::string, uint32_t> patternKeyToId;
    std::vector<PatternEntry> patternPool;
    patternPool.reserve(4096);

    // Phi por termino: lista dispersa de (master, pattern_id).
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> phiByTerm;
    phiByTerm.reserve(termsToProcess);

    std::vector<uint64_t> posting;
    for (uint32_t term = 0; term < termsToProcess; ++term) {
        if (!readPisaPostingList(packedIn, posting)) {
            std::cerr << "Error leyendo posting en term " << term << ".\n";
            return 1;
        }

        ++stats.listsProcessed;
        stats.valuesPacked += posting.size();
        bool unsorted = false;
        auditSortedDedup(posting, unsorted, stats.packedDuplicates);
        if (unsorted) ++stats.packedUnsortedLists;

        std::set<int> masterSet;
        std::unordered_map<uint32_t, std::vector<uint32_t>> relsByMaster;
        for (uint64_t packed : posting) {
            uint32_t master = static_cast<uint32_t>(ZDD_UNPACK_MASTER(packed));
            uint32_t rel = static_cast<uint32_t>(ZDD_UNPACK_REL(packed));
            if (rel > maxRelSeen) maxRelSeen = rel;

            int lvlMaster = 0;
            if (toDdLevel(static_cast<uint64_t>(master) + 1u, lvlMaster))
                masterSet.insert(lvlMaster);
            else
                ++stats.ddLevelSkipsMaster;

            relsByMaster[master].push_back(rel);
        }

        // Piso 1: M_t.
        FastPathSpec docsSpec(masterSet);
        tdzdd::DdStructure<2> docsZdd(docsSpec);
        docsZdd.zddReduce();
        docsByTerm.push_back(docsZdd);

        // Piso 2 + Phi(t,d): R_{t,d} -> pattern_id.
        std::vector<std::pair<uint32_t, uint32_t>> phiTerm;
        phiTerm.reserve(relsByMaster.size());

        for (auto& kv : relsByMaster) {
            uint32_t master = kv.first;
            std::vector<uint32_t>& rels = kv.second;
            std::sort(rels.begin(), rels.end());
            rels.erase(std::unique(rels.begin(), rels.end()), rels.end());

            std::string key = patternKey(rels);
            uint32_t pid = 0;
            auto it = patternKeyToId.find(key);
            if (it != patternKeyToId.end()) {
                pid = it->second;
                patternPool[pid].useCount += 1;
            } else {
                std::set<int> relSet;
                for (uint32_t r : rels) {
                    int lvlRel = 0;
                    if (toDdLevel(static_cast<uint64_t>(r) + 1u, lvlRel))
                        relSet.insert(lvlRel);
                    else
                        ++stats.ddLevelSkipsRel;
                }
                FastPathSpec relSpec(relSet);
                tdzdd::DdStructure<2> relZdd(relSpec);
                relZdd.zddReduce();

                PatternEntry pe;
                pe.zdd = relZdd;
                pe.useCount = 1;
                pe.cardinality = rels.size();

                pid = static_cast<uint32_t>(patternPool.size());
                patternPool.push_back(pe);
                patternKeyToId.emplace(std::move(key), pid);
            }

            phiTerm.emplace_back(master, pid);
            ++stats.totalPairsTD;
        }
        phiByTerm.push_back(std::move(phiTerm));

        if ((term + 1) % progressEvery == 0 || term + 1 == termsToProcess) {
            std::cout << "[PROGRESS] term " << (term + 1) << "/" << termsToProcess
                      << " | patrones_distintos=" << patternPool.size()
                      << " | pares=" << stats.totalPairsTD << "\n";
        }
    }

    // Bosque 1: union de M_t.
    tdzdd::DdStructure<2> docsForest;
    for (const auto& z : docsByTerm) docsForest = tdzdd::DdStructure<2>(tdzdd::zddUnion(docsForest, z));
    docsForest.zddReduce();

    // Bosque 2: union de patrones R.
    tdzdd::DdStructure<2> versionPoolForest;
    for (const auto& p : patternPool)
        versionPoolForest = tdzdd::DdStructure<2>(tdzdd::zddUnion(versionPoolForest, p.zdd));
    versionPoolForest.zddReduce();

    // ----------------------------- Reporte principal --------------------------
    std::cout << "\n=== RESUMEN BUILD ===\n";
    std::cout << "terms processed             = " << stats.listsProcessed << "\n";
    std::cout << "values packed               = " << stats.valuesPacked << "\n";
    std::cout << "pairs (t,d)                 = " << stats.totalPairsTD << "\n";
    std::cout << "distinct version patterns   = " << patternPool.size() << "\n";
    if (!patternPool.empty()) {
        double dedup = static_cast<double>(stats.totalPairsTD) / static_cast<double>(patternPool.size());
        std::cout << "dedup factor (pairs/pattern)= " << std::fixed << std::setprecision(2) << dedup << "x\n";
    }
    std::cout << "max rel observed            = " << maxRelSeen << "\n";
    std::cout << "docsForest nodes            = " << docsForest.size() << "\n";
    std::cout << "versionPool nodes           = " << versionPoolForest.size() << "\n";
    std::cout << "unsorted packed lists       = " << stats.packedUnsortedLists << "\n";
    std::cout << "duplicate packed values     = " << stats.packedDuplicates << "\n";
    std::cout << "level skips master/rel      = " << stats.ddLevelSkipsMaster
              << "/" << stats.ddLevelSkipsRel << "\n";

    // ----------------------------- Consultas demo -----------------------------
    std::cout << "\n=== CONSULTAS DEMO (DOS PISOS) ===\n";
    if (termA < 0 || termB < 0 ||
        static_cast<uint32_t>(termA) >= docsByTerm.size() ||
        static_cast<uint32_t>(termB) >= docsByTerm.size()) {
        std::cout << "[WARN] termA o termB fuera de rango procesado; omitiendo consultas demo.\n";
    } else {
        std::vector<uint32_t> docsA = elementsOf(docsByTerm[static_cast<size_t>(termA)]);
        std::vector<uint32_t> docsB = elementsOf(docsByTerm[static_cast<size_t>(termB)]);
        printSetSample("M_termA", docsA);
        printSetSample("M_termB", docsB);

        // Interseccion de documentos entre terminos.
        tdzdd::DdStructure<2> docsInter(
            tdzdd::zddIntersection(docsByTerm[static_cast<size_t>(termA)],
                                   docsByTerm[static_cast<size_t>(termB)]));
        docsInter.zddReduce();
        std::vector<uint32_t> docsAB = elementsOf(docsInter);
        printSetSample("M_termA ∩ M_termB", docsAB);

        // Interseccion de versiones en el mismo master para termA y termB.
        uint32_t pidA = 0, pidB = 0;
        bool hasA = findPatternIdForMaster(phiByTerm[static_cast<size_t>(termA)],
                                           static_cast<uint32_t>(queryMaster), pidA);
        bool hasB = findPatternIdForMaster(phiByTerm[static_cast<size_t>(termB)],
                                           static_cast<uint32_t>(queryMaster), pidB);
        if (!hasA || !hasB) {
            std::cout << "R intersection on master=" << queryMaster
                      << " -> N/A (uno de los terminos no aparece en ese master)\n";
        } else {
            const auto& zA = patternPool[pidA].zdd;
            const auto& zB = patternPool[pidB].zdd;
            std::vector<uint32_t> rA = elementsOf(zA);
            std::vector<uint32_t> rB = elementsOf(zB);
            printSetSample("R_termA,master", rA, 16);
            printSetSample("R_termB,master", rB, 16);

            tdzdd::DdStructure<2> relInter(tdzdd::zddIntersection(zA, zB));
            relInter.zddReduce();
            std::vector<uint32_t> rAB = elementsOf(relInter);
            printSetSample("R_termA,master ∩ R_termB,master", rAB, 16);

            bool foundRelA = std::binary_search(rA.begin(), rA.end(), static_cast<uint32_t>(queryRel));
            bool foundRelB = std::binary_search(rB.begin(), rB.end(), static_cast<uint32_t>(queryRel));
            bool foundRelAB = std::binary_search(rAB.begin(), rAB.end(), static_cast<uint32_t>(queryRel));
            std::cout << "lookup rel=" << queryRel << " in termA -> " << (foundRelA ? "FOUND" : "NOT FOUND")
                      << ", termB -> " << (foundRelB ? "FOUND" : "NOT FOUND")
                      << ", intersection -> " << (foundRelAB ? "FOUND" : "NOT FOUND") << "\n";
        }
    }

    auto wallEnd = std::chrono::steady_clock::now();
    double elapsedS =
        std::chrono::duration_cast<std::chrono::duration<double>>(wallEnd - wallStart).count();

    std::ofstream metrics(metricsCsvPath, std::ios::app);
    if (metrics.good()) {
        if (metrics.tellp() == 0) {
            metrics << "packed_docs,terms_processed,values_packed,pairs_td,distinct_patterns,"
                    << "docs_forest_nodes,versions_forest_nodes,max_rel,elapsed_s\n";
        }
        metrics << packedDocsPath << "," << stats.listsProcessed << "," << stats.valuesPacked << ","
                << stats.totalPairsTD << "," << patternPool.size() << ","
                << docsForest.size() << "," << versionPoolForest.size() << ","
                << maxRelSeen << "," << std::setprecision(6) << elapsedS << "\n";
        std::cout << "\n[OK] metrics csv actualizado: " << metricsCsvPath << "\n";
    } else {
        std::cout << "\n[WARN] no se pudo escribir metrics csv: " << metricsCsvPath << "\n";
    }

    std::cout << "\nFinalizado (doble piso integrado).\n";
    return 0;
}
