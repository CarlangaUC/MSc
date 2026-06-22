// =============================================================================
// VARIANTE 2 PISOS — ZDD(M_t) + Phi externo + ZDD pool CUDD
// =============================================================================
// Objetivo:
//   Comparar contra blueprint_doble_piso_cudd.cpp.
//
//   En vez de codificar el Piso 1 como una cadena sparse multi-terminal
//   (master -> puntero), esta variante vuelve al primer piso conjuntista:
//
//      Piso 1:  term -> ZDD(M_t), donde M_t = masters donde aparece t.
//      Phi:     (term, master) -> pattern_id.
//      Piso 2:  pattern_id -> ZDD CUDD R_{t,d}.
//
// Matemáticamente:
//   PL(t) = {(d,v): d in M_t and v in R_{t,d}}.
//
// Esto permite medir el costo/beneficio de separar:
//   - compresión de conjuntos de documentos (Piso 1 ZDD real),
//   - compresión temporal compartida (Piso 2 CUDD),
//   - mapeo Phi externo (overhead explícito).
//
// Compilar (desde la raíz MAGISTER):
//   g++ -O2 -std=c++17 -o scripts/cpp/variant_zdd_mt_phi_cudd scripts/cpp/variant_zdd_mt_phi_cudd.cpp \
//       -I ./TdZdd/include -I ./cudd/cudd -I ./cudd \
//       -L ./cudd/cudd/.libs -Wl,-rpath,'$ORIGIN/cudd/cudd/.libs' \
//       -lcudd -lpthread
//
// Ejecutar:
//   ./scripts/cpp/variant_zdd_mt_phi_cudd \
//     resultados_test/wiki_100mb_uihrdc_packed.docs \
//     uiHRDC/uiHRDC/data/texts/page_mapping_wiki_100mb.bin \
//     uiHRDC/uiHRDC/data/texts/index_wiki_100mb_named.voc \
//     Abraham Website 13 180 0 2000 resultados_test/variant_zdd_mt_phi_metrics.csv
// =============================================================================

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

#include "cudd.h"

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

struct DocsMeta {
    bool loaded = false;
    std::unordered_map<std::string, std::string> kv;
};

struct Vocabulary {
    bool loaded = false;
    uint32_t nwords = 0;
    std::vector<std::string> words;
    std::unordered_map<std::string, uint32_t> word2id;
};

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

static uint32_t bitread32(const uint32_t* e, uint32_t p, uint32_t len) {
    e += p / 32u;
    p %= 32u;
    uint64_t answ = static_cast<uint64_t>(*e) >> p;
    if (p + len > 32u) answ |= static_cast<uint64_t>(*(e + 1)) << (32u - p);
    if (len < 32u) answ &= ((static_cast<uint64_t>(1) << len) - 1u);
    return static_cast<uint32_t>(answ);
}

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

static bool toDdLevel(uint64_t v, int& out) {
    if (v == 0 || v > static_cast<uint64_t>(std::numeric_limits<int>::max())) return false;
    out = static_cast<int>(v);
    return true;
}

static std::string patternKey(const std::vector<uint32_t>& rels) {
    std::string key;
    key.resize(rels.size() * sizeof(uint32_t));
    if (!rels.empty()) std::memcpy(&key[0], rels.data(), key.size());
    return key;
}

static DdNode* buildSingletonZdd(DdManager* dd, const std::vector<uint32_t>& rels) {
    DdNode* p = Cudd_ReadOne(dd);
    Cudd_Ref(p);
    for (auto rit = rels.rbegin(); rit != rels.rend(); ++rit) {
        DdNode* tmp = Cudd_zddChange(dd, p, static_cast<int>(*rit));
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

static std::vector<uint32_t> relsOfPattern(DdNode* root) {
    std::vector<uint32_t> rels;
    DdNode* cur = root;
    while (!Cudd_IsConstant(cur)) {
        rels.push_back(static_cast<uint32_t>(Cudd_NodeReadIndex(cur)));
        cur = Cudd_T(cur);
    }
    std::sort(rels.begin(), rels.end());
    return rels;
}

static std::vector<uint32_t> mastersOf(const tdzdd::DdStructure<2>& z) {
    std::set<uint32_t> acc;
    for (auto const& s : z) {
        for (int lvl : s) {
            if (lvl >= 1) acc.insert(static_cast<uint32_t>(lvl - 1));
        }
    }
    return std::vector<uint32_t>(acc.begin(), acc.end());
}

static std::vector<uint32_t> intersectSorted(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    std::vector<uint32_t> out;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}

static std::vector<uint32_t> unionSorted(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    std::vector<uint32_t> out;
    std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}

static void printSet(const std::string& label, const std::vector<uint32_t>& v, size_t maxShow = 24) {
    std::cout << label << " (|set|=" << v.size() << "): { ";
    size_t show = std::min(maxShow, v.size());
    for (size_t i = 0; i < show; ++i) {
        std::cout << v[i];
        if (i + 1 < show) std::cout << ", ";
    }
    if (v.size() > show) std::cout << " ...";
    std::cout << " }\n";
}

static bool findPhi(const std::vector<std::pair<uint32_t, uint32_t>>& phiTerm,
                    uint32_t master,
                    uint32_t& patternId) {
    auto it = std::lower_bound(
        phiTerm.begin(), phiTerm.end(), master,
        [](const std::pair<uint32_t, uint32_t>& p, uint32_t value) { return p.first < value; });
    if (it == phiTerm.end() || it->first != master) return false;
    patternId = it->second;
    return true;
}

int main(int argc, char** argv) {
    std::string packedDocsPath = "/root/MAGISTER/resultados_test/wiki_100mb_uihrdc_packed.docs";
    std::string mappingPath = "/root/MAGISTER/uiHRDC/uiHRDC/data/texts/page_mapping_wiki_100mb.bin";
    std::string vocabPath = "/root/MAGISTER/uiHRDC/uiHRDC/data/texts/index_wiki_100mb_named.voc";
    std::string wordA = "Abraham";
    std::string wordB = "Website";
    int queryMaster = 13;
    int queryRel = 180;
    uint32_t maxTerms = 0;
    uint32_t progressEvery = 1000;
    std::string metricsCsvPath = "resultados_test/variant_zdd_mt_phi_metrics.csv";

    if (argc >= 2) packedDocsPath = argv[1];
    if (argc >= 3) mappingPath = argv[2];
    if (argc >= 4) vocabPath = argv[3];
    if (argc >= 5) wordA = argv[4];
    if (argc >= 6) wordB = argv[5];
    if (argc >= 7) queryMaster = std::atoi(argv[6]);
    if (argc >= 8) queryRel = std::atoi(argv[7]);
    if (argc >= 9) maxTerms = static_cast<uint32_t>(std::strtoul(argv[8], nullptr, 10));
    if (argc >= 10) progressEvery = static_cast<uint32_t>(std::strtoul(argv[9], nullptr, 10));
    if (argc >= 11) metricsCsvPath = argv[10];

    std::cout << "=== VARIANTE: ZDD(M_t) + Phi externo + ZDD pool CUDD ===\n";
    std::cout << "[CFG] packed_docs = " << packedDocsPath << "\n";
    std::cout << "[CFG] page_map    = " << mappingPath << "\n";
    std::cout << "[CFG] vocab       = " << vocabPath << "\n";
    std::cout << "[CFG] wordA/wordB = " << wordA << " / " << wordB << "\n";
    std::cout << "[CFG] query pair  = (master=" << queryMaster << ", rel=" << queryRel << ")\n";
    std::cout << "[CFG] max_terms   = " << maxTerms << "\n";
    std::cout << "[CFG] metrics_csv = " << metricsCsvPath << "\n";
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
        std::cerr << "Error: split incompatible.\n";
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
                std::cerr << "Error leyendo posting en term " << term << ".\n";
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
    std::cout << "[OK] max rel = " << maxRelSeen << " -> variables ZDD = " << numZddVars << "\n\n";

    DdManager* dd = Cudd_Init(0, numZddVars, CUDD_UNIQUE_SLOTS, CUDD_CACHE_SLOTS, 0);
    if (dd == nullptr) {
        std::cerr << "Error: no se pudo inicializar CUDD.\n";
        return 1;
    }

    auto wallStart = std::chrono::steady_clock::now();

    std::unordered_map<std::string, uint32_t> patternIdByKey;
    std::vector<DdNode*> patterns;
    std::vector<size_t> patternCards;
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> phiByTerm(termsToProcess);
    std::vector<tdzdd::DdStructure<2>> termDocZdds;
    termDocZdds.reserve(termsToProcess);
    tdzdd::DdStructure<2> docsForest;

    uint64_t totalPairs = 0;
    uint64_t masterLevelSkips = 0;

    for (uint32_t term = 0; term < termsToProcess; ++term) {
        const std::vector<uint64_t>& posting = postings[term];
        std::unordered_map<uint32_t, std::vector<uint32_t>> relsByMaster;
        for (uint64_t packed : posting) {
            relsByMaster[static_cast<uint32_t>(ZDD_UNPACK_MASTER(packed))]
                .push_back(static_cast<uint32_t>(ZDD_UNPACK_REL(packed)));
        }

        std::set<int> masterLevels;
        std::vector<std::pair<uint32_t, uint32_t>>& phi = phiByTerm[term];
        phi.reserve(relsByMaster.size());

        for (auto& kv : relsByMaster) {
            uint32_t master = kv.first;
            std::vector<uint32_t>& rels = kv.second;
            std::sort(rels.begin(), rels.end());
            rels.erase(std::unique(rels.begin(), rels.end()), rels.end());

            std::string key = patternKey(rels);
            uint32_t pid = 0;
            auto it = patternIdByKey.find(key);
            if (it != patternIdByKey.end()) {
                pid = it->second;
            } else {
                DdNode* pat = buildSingletonZdd(dd, rels);
                if (pat == nullptr) {
                    std::cerr << "Error construyendo patron (term " << term << ").\n";
                    for (DdNode* p : patterns) Cudd_RecursiveDerefZdd(dd, p);
                    Cudd_Quit(dd);
                    return 1;
                }
                pid = static_cast<uint32_t>(patterns.size());
                patternIdByKey.emplace(std::move(key), pid);
                patterns.push_back(pat);
                patternCards.push_back(rels.size());
            }

            int levelMaster = 0;
            if (toDdLevel(static_cast<uint64_t>(master) + 1u, levelMaster)) {
                masterLevels.insert(levelMaster);
            } else {
                ++masterLevelSkips;
            }
            phi.emplace_back(master, pid);
            ++totalPairs;
        }

        std::sort(phi.begin(), phi.end(),
                  [](const std::pair<uint32_t, uint32_t>& a, const std::pair<uint32_t, uint32_t>& b) {
                      return a.first < b.first;
                  });

        FastPathSpec spec(masterLevels);
        tdzdd::DdStructure<2> termZdd(spec);
        termZdd.zddReduce();
        docsForest = tdzdd::DdStructure<2>(tdzdd::zddUnion(docsForest, termZdd));
        termDocZdds.push_back(termZdd);

        if ((term + 1) % progressEvery == 0 || term + 1 == termsToProcess) {
            std::cout << "[PROGRESS] term " << (term + 1) << "/" << termsToProcess
                      << " | docsForest_nodes=" << docsForest.size()
                      << " | patrones=" << patterns.size()
                      << " | pares=" << totalPairs << "\n";
            std::cout.flush();
        }
    }

    size_t docsForestPre = docsForest.size();
    docsForest.zddReduce();
    size_t docsForestPost = docsForest.size();

    auto wallBuild = std::chrono::steady_clock::now();
    double buildS = std::chrono::duration_cast<std::chrono::duration<double>>(wallBuild - wallStart).count();

    long poolLiveNodes = Cudd_zddReadNodeCount(dd);
    unsigned long memInUse = Cudd_ReadMemoryInUse(dd);
    uint64_t phiBytesApprox = totalPairs * sizeof(std::pair<uint32_t, uint32_t>);

    std::cout << "\n=== RESUMEN BUILD ===\n";
    std::cout << "terms processed             = " << termsToProcess << "\n";
    std::cout << "values packed               = " << totalPackedValues << "\n";
    std::cout << "pairs (t,d)                 = " << totalPairs << "\n";
    std::cout << "max rel                     = " << maxRelSeen << "\n";
    std::cout << "build time (s)              = " << std::fixed << std::setprecision(2) << buildS << "\n";
    std::cout << "master level skips          = " << masterLevelSkips << "\n";

    std::cout << "\n--- PISO 1: ZDD(M_t) ---\n";
    std::cout << "term ZDDs stored            = " << termDocZdds.size() << "\n";
    std::cout << "docsForest nodes pre/post   = " << docsForestPre << " / " << docsForestPost << "\n";

    std::cout << "\n--- PHI externo ---\n";
    std::cout << "Phi entries                 = " << totalPairs << "\n";
    std::cout << "Phi bytes aprox             = " << phiBytesApprox << " B ("
              << std::fixed << std::setprecision(2) << phiBytesApprox / 1024.0 / 1024.0 << " MB)\n";

    std::cout << "\n--- PISO 2: ZDD pool CUDD ---\n";
    std::cout << "patrones distintos          = " << patterns.size() << "\n";
    std::cout << "factor dedup (pares/patrones)= "
              << (patterns.empty() ? 0.0 : static_cast<double>(totalPairs) / patterns.size()) << "x\n";
    std::cout << "nodos vivos manager (pool)  = " << poolLiveNodes << "\n";
    std::cout << "memoria CUDD en uso         = " << memInUse << " B ("
              << std::fixed << std::setprecision(2) << memInUse / 1024.0 / 1024.0 << " MB)\n";

    auto resolveTerm = [&](const std::string& w, int& outTid) -> bool {
        // Si es numerico, priorizarlo como term_id. Esto permite consultar subsets.
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

    std::cout << "\n=== CONSULTA 1: PUNTO VERSIONADO ===\n";
    if (!okA) {
        std::cout << "[INFO] wordA='" << wordA << "' no resuelta en rango procesado.\n";
    } else {
        std::cout << "wordA='" << wordA << "' -> term_id=" << tidA << "\n";
        uint32_t pid = 0;
        if (!findPhi(phiByTerm[tidA], static_cast<uint32_t>(queryMaster), pid)) {
            std::cout << "  master " << queryMaster << " -> NO presente en Phi/M_t.\n";
        } else {
            std::vector<uint32_t> rels = relsOfPattern(patterns[pid]);
            bool found = std::binary_search(rels.begin(), rels.end(), static_cast<uint32_t>(queryRel));
            printSet("  R_{t,master}", rels, 16);
            std::cout << "  contains(" << wordA << ", master=" << queryMaster << ", rel=" << queryRel
                      << ") -> " << (found ? "FOUND" : "NOT FOUND") << "\n";
            if (!rels.empty()) {
                uint32_t present = rels.front();
                uint32_t absent = rels.back() + 1;
                bool okPresent = std::binary_search(rels.begin(), rels.end(), present);
                bool okAbsent = std::binary_search(rels.begin(), rels.end(), absent);
                std::cout << "  [self-test] contains(rel=" << present << ")="
                          << (okPresent ? "FOUND" : "NOT FOUND")
                          << "; contains(rel=" << absent << ")="
                          << (okAbsent ? "FOUND" : "NOT FOUND") << " -> "
                          << ((okPresent && !okAbsent) ? "OK" : "FALLA") << "\n";
            }
        }
    }

    std::cout << "\n=== CONSULTA 2: M_t y M_tA ∩ M_tB ===\n";
    std::vector<uint32_t> mA, mB, common;
    if (okA) {
        mA = mastersOf(termDocZdds[tidA]);
        printSet("  M_wordA", mA);
    }
    if (okB) {
        std::cout << "wordB='" << wordB << "' -> term_id=" << tidB << "\n";
        mB = mastersOf(termDocZdds[tidB]);
        printSet("  M_wordB", mB);
    }
    if (okA && okB) {
        common = intersectSorted(mA, mB);
        printSet("  M_wordA ∩ M_wordB", common);
    }

    std::cout << "\n=== CONSULTA 3: INTERSECCION/UNION VERSIONADA EN MASTER QUERY ===\n";
    if (okA && okB) {
        uint32_t pidA = 0, pidB = 0;
        bool hasA = findPhi(phiByTerm[tidA], static_cast<uint32_t>(queryMaster), pidA);
        bool hasB = findPhi(phiByTerm[tidB], static_cast<uint32_t>(queryMaster), pidB);
        if (hasA && hasB) {
            std::vector<uint32_t> ra = relsOfPattern(patterns[pidA]);
            std::vector<uint32_t> rb = relsOfPattern(patterns[pidB]);
            std::vector<uint32_t> inter = intersectSorted(ra, rb);
            std::vector<uint32_t> uni = unionSorted(ra, rb);
            printSet("  R_A ∩ R_B", inter, 16);
            printSet("  R_A ∪ R_B", uni, 16);
            std::cout << "  same pattern_id? " << (pidA == pidB ? "SI" : "no")
                      << " (pidA=" << pidA << ", pidB=" << pidB << ")\n";
        } else {
            std::cout << "  master " << queryMaster << " no comun a ambos terminos.\n";
        }
    }

    std::ofstream metrics(metricsCsvPath, std::ios::app);
    if (metrics.good()) {
        if (metrics.tellp() == 0) {
            metrics << "packed_docs,terms,values_packed,pairs_td,distinct_patterns,"
                    << "docs_forest_pre,docs_forest_post,pool_live_nodes,phi_bytes,"
                    << "cudd_mem_bytes,max_rel,build_s\n";
        }
        metrics << packedDocsPath << "," << termsToProcess << "," << totalPackedValues << ","
                << totalPairs << "," << patterns.size() << "," << docsForestPre << ","
                << docsForestPost << "," << poolLiveNodes << "," << phiBytesApprox << ","
                << memInUse << "," << maxRelSeen << "," << std::fixed << std::setprecision(6)
                << buildS << "\n";
        std::cout << "\n[OK] metrics csv: " << metricsCsvPath << "\n";
    }

    for (DdNode* p : patterns) Cudd_RecursiveDerefZdd(dd, p);
    int leaked = Cudd_CheckZeroRef(dd);
    if (leaked != 0) std::cout << "[WARN] CUDD refs no liberadas: " << leaked << "\n";
    Cudd_Quit(dd);

    std::cout << "\nFinalizado (variant ZDD(M_t)+Phi+CUDD).\n";
    return 0;
}
