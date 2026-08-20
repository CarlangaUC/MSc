// =============================================================================
// BLUEPRINT DOBLE PISO — MTZDD (Piso 1) + ZDD pool CUDD (Piso 2)
// =============================================================================
// Indice invertido versionado de dos niveles:
//
//   Diccionario:  word -> raiz MTZDD del termino (puntero, O(1)).
//
//   PISO 1 (MTZDD, enrutador espacial):
//     - Universo de nodos = IDs de Documentos Maestros (master).
//     - Cadena descendente por master (root = master mayor).
//     - Rama BAJA  (0): siguiente master menor (o terminal-0 = reject).
//     - Rama ALTA  (1): terminal-PUNTERO al historial R_{t,d} en el Piso 2.
//     - Hash-consing: terminos con la misma "cola" (mismos pares
//       (master, puntero) en los masters menores) comparten nodos fisicos.
//
//   PISO 2 (ZDD pool unico, compresion temporal, CUDD):
//     - Universo de variables = versiones relativas (rel).
//     - Terminales estrictos {0,1}.
//     - R_{t,d} = { rel : t aparece en (master d, version rel) }.
//     - Unique table compartida: historiales identicos o con sub-DAGs
//       isomorfos comparten nodos automaticamente (sharing intra/inter).
//
//   Phi(t,d) NO es un arreglo externo: vive embebido en el terminal de la
//   rama alta del nodo master d del MTZDD (puntero a la raiz ZDD del Piso 2).
//
// Consulta versionada (t, master, rel):
//   1. root = vocab[word]
//   2. nav Piso 1 hasta el nodo master -> rama alta -> puntero R_{t,master}
//   3. membership de rel en el ZDD R_{t,master} (Piso 2)
//
// Compilar (desde la raíz MAGISTER):
//   g++ -O2 -std=c++17 -o scripts/cpp/blueprint_doble_piso_cudd scripts/cpp/blueprint_doble_piso_cudd.cpp \
//       -I ./cudd/cudd -I ./cudd -L ./cudd/cudd/.libs \
//       -Wl,-rpath,'$ORIGIN/cudd/cudd/.libs' -lcudd
//
// Ejecutar (wiki_100mb):
//   ./scripts/cpp/blueprint_doble_piso_cudd \
//     resultados_test/wiki_100mb_uihrdc_packed.docs \
//     uiHRDC/uiHRDC/data/texts/page_mapping_wiki_100mb.bin \
//     uiHRDC/uiHRDC/data/texts/index_wiki_100mb_named.voc \
//     Abraham Website 13 1730 0 1000 resultados_test/blueprint_metrics.csv
// =============================================================================

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
#include <vector>

#include "cudd.h"

#include "uiHRDC/uiHRDC/indexes/NOPOS/II_docs/src/utils/version_packing.h"

// -----------------------------------------------------------------------------
// Lectores .docs PISA-like, metadata y vocabulario (espejo de test_tdzdd_piso2).
// -----------------------------------------------------------------------------
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

// -----------------------------------------------------------------------------
// PISO 2: construccion de un patron R (single-subset ZDD) en CUDD.
// Base {{}} (Cudd_ReadOne) + Cudd_zddChange por cada rel. Devuelve nodo
// referenciado (el llamador es responsable de Cudd_RecursiveDerefZdd).
// -----------------------------------------------------------------------------
static DdNode* buildSingletonZdd(DdManager* dd, const std::vector<uint32_t>& rels) {
    DdNode* p = Cudd_ReadOne(dd);
    Cudd_Ref(p);
    // Insertar variables en orden DESCENDENTE: cada nueva var es menor (mas
    // cerca del top) y crea un nodo raiz nuevo en O(1), evitando O(k^2).
    for (auto rit = rels.rbegin(); rit != rels.rend(); ++rit) {
        uint32_t r = *rit;
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

// Enumera el conjunto R a partir del root ZDD (single-subset: spine de "then").
static std::vector<uint32_t> relsOfPattern(DdManager* dd, DdNode* root) {
    (void)dd;
    std::vector<uint32_t> rels;
    DdNode* cur = root;
    while (!Cudd_IsConstant(cur)) {
        rels.push_back(static_cast<uint32_t>(Cudd_NodeReadIndex(cur)));
        cur = Cudd_T(cur);
    }
    std::sort(rels.begin(), rels.end());
    return rels;
}

static std::string patternKey(const std::vector<uint32_t>& rels) {
    std::string key;
    key.resize(rels.size() * sizeof(uint32_t));
    if (!rels.empty()) std::memcpy(&key[0], rels.data(), key.size());
    return key;
}

// -----------------------------------------------------------------------------
// PISO 1: MTZDD hash-consed. Cada nodo representa un master de una cadena
// descendente. rama alta -> puntero ZDD (Piso 2); rama baja -> master menor.
// -----------------------------------------------------------------------------
struct MtNode {
    uint32_t master;      // nivel/documento
    DdNode* pattern;      // terminal de la rama ALTA: puntero al Piso 2
    const MtNode* low;    // rama BAJA: siguiente master menor (nullptr = reject)
};

struct MtKey {
    uint32_t master;
    DdNode* pattern;
    const MtNode* low;
    bool operator==(const MtKey& o) const {
        return master == o.master && pattern == o.pattern && low == o.low;
    }
};

struct MtKeyHash {
    size_t operator()(const MtKey& k) const {
        size_t h = std::hash<uint32_t>()(k.master);
        h ^= std::hash<const void*>()(k.pattern) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<const void*>()(k.low) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

class MtzddPool {
public:
    // Construye la cadena descendente para un termino a partir de pares
    // (master, pattern) ya ordenados ascendentemente por master.
    // Devuelve la raiz (nodo del master mayor) o nullptr si no hay masters.
    const MtNode* buildChain(const std::vector<std::pair<uint32_t, DdNode*>>& sortedAsc) {
        const MtNode* low = nullptr;  // terminal-0 = reject
        for (const auto& pr : sortedAsc) {
            low = getOrCreate(pr.first, pr.second, low);
        }
        return low;  // ultimo creado = master mayor = raiz
    }

    size_t nodeCount() const { return table_.size(); }

    ~MtzddPool() {
        for (MtNode* n : storage_) delete n;
    }

private:
    const MtNode* getOrCreate(uint32_t master, DdNode* pattern, const MtNode* low) {
        MtKey key{master, pattern, low};
        auto it = table_.find(key);
        if (it != table_.end()) return it->second;
        MtNode* node = new MtNode{master, pattern, low};
        storage_.push_back(node);
        table_.emplace(key, node);
        return node;
    }

    std::unordered_map<MtKey, MtNode*, MtKeyHash> table_;
    std::vector<MtNode*> storage_;
};

// Navega el Piso 1: devuelve el nodo cuyo master == target (rama alta -> pattern),
// o nullptr si el termino no aparece en ese master.
static const MtNode* mtNavigate(const MtNode* root, uint32_t target) {
    const MtNode* cur = root;
    while (cur != nullptr) {
        if (cur->master == target) return cur;
        if (target < cur->master) cur = cur->low;  // bajar hacia masters menores
        else return nullptr;                        // target > master mayor restante
    }
    return nullptr;
}

// M_t: todos los masters del termino (recorre la cadena baja).
static std::vector<uint32_t> mastersOf(const MtNode* root) {
    std::vector<uint32_t> out;
    for (const MtNode* cur = root; cur != nullptr; cur = cur->low) out.push_back(cur->master);
    std::sort(out.begin(), out.end());
    return out;
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

// Interseccion element-wise de dos conjuntos ordenados.
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

int main(int argc, char** argv) {
    std::string packedDocsPath = "/root/MAGISTER/resultados_test/wiki_100mb_uihrdc_packed.docs";
    std::string mappingPath = "/root/MAGISTER/uiHRDC/uiHRDC/data/texts/page_mapping_wiki_100mb.bin";
    std::string vocabPath = "/root/MAGISTER/uiHRDC/uiHRDC/data/texts/index_wiki_100mb_named.voc";
    std::string wordA = "Abraham";
    std::string wordB = "Website";
    int queryMaster = 13;
    int queryRel = 1730;
    uint32_t maxTerms = 0;
    uint32_t progressEvery = 1000;
    std::string metricsCsvPath = "resultados_test/blueprint_metrics.csv";

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

    std::cout << "=== BLUEPRINT DOBLE PISO (MTZDD + ZDD pool CUDD) ===\n";
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

    uint32_t termsToProcess = nlistsPacked;
    if (maxTerms > 0 && maxTerms < nlistsPacked) termsToProcess = maxTerms;

    // ----------------------- Primera pasada: cargar postings + maxRel ---------
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

    // ----------------------- Manager CUDD (Piso 2) ----------------------------
    DdManager* dd = Cudd_Init(0, numZddVars, CUDD_UNIQUE_SLOTS, CUDD_CACHE_SLOTS, 0);
    if (dd == nullptr) {
        std::cerr << "Error: no se pudo inicializar CUDD.\n";
        return 1;
    }

    auto wallStart = std::chrono::steady_clock::now();

    // pattern dedup: rels-key -> root ZDD (referenciado, vive con el indice).
    std::unordered_map<std::string, DdNode*> patternByKey;
    std::vector<DdNode*> distinctPatterns;  // para deref final

    // Diccionario + Piso 1: raiz MTZDD por termino.
    MtzddPool mtzdd;
    std::vector<const MtNode*> termRoots(termsToProcess, nullptr);

    uint64_t totalPairs = 0;

    for (uint32_t term = 0; term < termsToProcess; ++term) {
        const std::vector<uint64_t>& posting = postings[term];

        // Agrupar rels por master.
        std::unordered_map<uint32_t, std::vector<uint32_t>> relsByMaster;
        for (uint64_t packed : posting) {
            relsByMaster[static_cast<uint32_t>(ZDD_UNPACK_MASTER(packed))]
                .push_back(static_cast<uint32_t>(ZDD_UNPACK_REL(packed)));
        }

        // Construir/recuperar patron (Piso 2) por master y armar pares (master,ptr).
        std::vector<std::pair<uint32_t, DdNode*>> pairs;
        pairs.reserve(relsByMaster.size());
        for (auto& kv : relsByMaster) {
            uint32_t master = kv.first;
            std::vector<uint32_t>& rels = kv.second;
            std::sort(rels.begin(), rels.end());
            rels.erase(std::unique(rels.begin(), rels.end()), rels.end());

            std::string key = patternKey(rels);
            DdNode* pat = nullptr;
            auto it = patternByKey.find(key);
            if (it != patternByKey.end()) {
                pat = it->second;  // dedup exacta + sharing CUDD
            } else {
                pat = buildSingletonZdd(dd, rels);
                if (pat == nullptr) {
                    std::cerr << "Error construyendo patron (term " << term << ").\n";
                    Cudd_Quit(dd);
                    return 1;
                }
                // pat ya viene referenciado por buildSingletonZdd; conservarlo.
                patternByKey.emplace(std::move(key), pat);
                distinctPatterns.push_back(pat);
            }
            pairs.emplace_back(master, pat);
            ++totalPairs;
        }

        std::sort(pairs.begin(), pairs.end(),
                  [](const std::pair<uint32_t, DdNode*>& a, const std::pair<uint32_t, DdNode*>& b) {
                      return a.first < b.first;
                  });

        termRoots[term] = mtzdd.buildChain(pairs);

        if ((term + 1) % progressEvery == 0 || term + 1 == termsToProcess) {
            std::cout << "[PROGRESS] term " << (term + 1) << "/" << termsToProcess
                      << " | patrones=" << distinctPatterns.size()
                      << " | pares=" << totalPairs
                      << " | mtzdd_nodes=" << mtzdd.nodeCount() << "\n";
            std::cout.flush();
        }
    }

    auto wallBuild = std::chrono::steady_clock::now();
    double buildS = std::chrono::duration_cast<std::chrono::duration<double>>(wallBuild - wallStart).count();

    // ----------------------------- Metricas -----------------------------------
    long poolLiveNodes = Cudd_zddReadNodeCount(dd);  // nodos ZDD vivos (pool compartido)
    unsigned long memInUse = Cudd_ReadMemoryInUse(dd);

    size_t mtzddNodes = mtzdd.nodeCount();
    uint64_t mtzddNaive = totalPairs;  // 1 nodo por (t,d) sin sharing

    std::cout << "\n=== RESUMEN BUILD ===\n";
    std::cout << "terms processed             = " << termsToProcess << "\n";
    std::cout << "values packed               = " << totalPackedValues << "\n";
    std::cout << "pairs (t,d)                 = " << totalPairs << "\n";
    std::cout << "max rel                     = " << maxRelSeen << "\n";
    std::cout << "build time (s)              = " << std::fixed << std::setprecision(2) << buildS << "\n";
    std::cout << "\n--- PISO 1 (MTZDD) ---\n";
    std::cout << "nodos MTZDD (hash-consed)   = " << mtzddNodes << "\n";
    std::cout << "nodos naive (sin sharing)   = " << mtzddNaive << "\n";
    if (mtzddNodes > 0) {
        std::cout << "factor sharing Piso 1       = " << std::fixed << std::setprecision(2)
                  << static_cast<double>(mtzddNaive) / static_cast<double>(mtzddNodes) << "x\n";
    }
    double mtzddBytes = static_cast<double>(mtzddNodes) * sizeof(MtNode);
    std::cout << "memoria MTZDD aprox         = " << static_cast<uint64_t>(mtzddBytes) << " B ("
              << std::fixed << std::setprecision(2) << mtzddBytes / 1024.0 / 1024.0 << " MB), "
              << sizeof(MtNode) << " B/nodo\n";
    std::cout << "\n--- PISO 2 (ZDD pool CUDD) ---\n";
    std::cout << "patrones distintos          = " << distinctPatterns.size() << "\n";
    std::cout << "factor dedup (pares/patrones)= " << std::fixed << std::setprecision(2)
              << (distinctPatterns.empty() ? 0.0
                                           : static_cast<double>(totalPairs) /
                                                 static_cast<double>(distinctPatterns.size()))
              << "x\n";
    std::cout << "nodos vivos manager (pool)  = " << poolLiveNodes << "\n";
    std::cout << "memoria CUDD en uso         = " << memInUse << " B ("
              << std::fixed << std::setprecision(2) << memInUse / 1024.0 / 1024.0 << " MB)\n";

    // ----------------------------- Resolver palabras ---------------------------
    auto resolveTerm = [&](const std::string& w, int& outTid) -> bool {
        if (hasVocab) {
            auto it = voc.word2id.find(w);
            if (it != voc.word2id.end()) {
                outTid = static_cast<int>(it->second);
                return outTid >= 0 && static_cast<uint32_t>(outTid) < termsToProcess;
            }
        }
        // fallback: interpretar como term_id numerico.
        char* end = nullptr;
        long v = std::strtol(w.c_str(), &end, 10);
        if (end != w.c_str() && *end == '\0' && v >= 0 && static_cast<uint32_t>(v) < termsToProcess) {
            outTid = static_cast<int>(v);
            return true;
        }
        return false;
    };

    int tidA = -1, tidB = -1;
    bool okA = resolveTerm(wordA, tidA);
    bool okB = resolveTerm(wordB, tidB);

    // ----------------------------- CONSULTAS -----------------------------------
    std::cout << "\n=== CONSULTA 1: PUNTO VERSIONADO (word, master, rel) ===\n";
    if (!okA) {
        std::cout << "[INFO] wordA='" << wordA << "' no resuelta en rango procesado.\n";
    } else {
        std::cout << "wordA='" << wordA << "' -> term_id=" << tidA << "\n";
        const MtNode* node = mtNavigate(termRoots[tidA], static_cast<uint32_t>(queryMaster));
        if (node == nullptr) {
            std::cout << "  master " << queryMaster << " -> NO presente en M_t (rama baja agota).\n";
        } else {
            std::vector<uint32_t> rels = relsOfPattern(dd, node->pattern);
            bool found = std::binary_search(rels.begin(), rels.end(), static_cast<uint32_t>(queryRel));
            printSet("  R_{t,master}", rels, 16);
            std::cout << "  contains(" << wordA << ", master=" << queryMaster << ", rel=" << queryRel
                      << ") -> " << (found ? "FOUND" : "NOT FOUND") << "\n";
            // Auto-validacion: un rel presente debe dar FOUND, uno fuera del rango NOT FOUND.
            if (!rels.empty()) {
                uint32_t present = rels.front();
                uint32_t absent = rels.back() + 1;
                bool okPresent = std::binary_search(rels.begin(), rels.end(), present);
                bool okAbsent = std::binary_search(rels.begin(), rels.end(), absent);
                std::cout << "  [self-test] contains(rel=" << present << ")=" << (okPresent ? "FOUND" : "NOT FOUND")
                          << " (esperado FOUND); contains(rel=" << absent << ")="
                          << (okAbsent ? "FOUND" : "NOT FOUND") << " (esperado NOT FOUND) -> "
                          << ((okPresent && !okAbsent) ? "OK" : "FALLA") << "\n";
            }
        }
    }

    std::cout << "\n=== CONSULTA 2: M_t (documentos del termino) ===\n";
    if (okA) {
        std::vector<uint32_t> mA = mastersOf(termRoots[tidA]);
        printSet("  M_wordA", mA);
    }
    if (okB) {
        std::cout << "wordB='" << wordB << "' -> term_id=" << tidB << "\n";
        std::vector<uint32_t> mB = mastersOf(termRoots[tidB]);
        printSet("  M_wordB", mB);
    }

    std::cout << "\n=== CONSULTA 3: INTERSECCION DE DOCUMENTOS M_tA ∩ M_tB ===\n";
    std::vector<uint32_t> common;
    if (okA && okB) {
        std::vector<uint32_t> mA = mastersOf(termRoots[tidA]);
        std::vector<uint32_t> mB = mastersOf(termRoots[tidB]);
        common = intersectSorted(mA, mB);
        printSet("  M_wordA ∩ M_wordB", common);
    } else {
        std::cout << "[INFO] requiere ambas palabras resueltas.\n";
    }

    std::cout << "\n=== CONSULTA 4: INTERSECCION VERSIONADA R_{tA,d} ∩ R_{tB,d} ===\n";
    std::cout << "(versiones donde AMBOS terminos aparecen en el mismo master)\n";
    if (okA && okB) {
        if (common.empty()) {
            std::cout << "  sin masters comunes; nada que intersectar.\n";
        } else {
            size_t shown = 0;
            for (uint32_t d : common) {
                const MtNode* na = mtNavigate(termRoots[tidA], d);
                const MtNode* nb = mtNavigate(termRoots[tidB], d);
                if (na == nullptr || nb == nullptr) continue;
                std::vector<uint32_t> ra = relsOfPattern(dd, na->pattern);
                std::vector<uint32_t> rb = relsOfPattern(dd, nb->pattern);
                std::vector<uint32_t> inter = intersectSorted(ra, rb);
                std::cout << "  master " << d << ": |R_A|=" << ra.size()
                          << " |R_B|=" << rb.size() << " |∩|=" << inter.size() << "\n";
                printSet("    R_A ∩ R_B", inter, 16);
                if (++shown >= 8) {
                    std::cout << "    ... (" << common.size() << " masters comunes en total)\n";
                    break;
                }
            }
        }
    } else {
        std::cout << "[INFO] requiere ambas palabras resueltas.\n";
    }

    std::cout << "\n=== CONSULTA 5: UNION VERSIONADA R_{tA,d} ∪ R_{tB,d} (master query) ===\n";
    if (okA && okB) {
        const MtNode* na = mtNavigate(termRoots[tidA], static_cast<uint32_t>(queryMaster));
        const MtNode* nb = mtNavigate(termRoots[tidB], static_cast<uint32_t>(queryMaster));
        if (na && nb) {
            std::vector<uint32_t> ra = relsOfPattern(dd, na->pattern);
            std::vector<uint32_t> rb = relsOfPattern(dd, nb->pattern);
            std::vector<uint32_t> uni = unionSorted(ra, rb);
            std::vector<uint32_t> inter = intersectSorted(ra, rb);
            printSet("  R_A ∪ R_B", uni, 16);
            printSet("  R_A ∩ R_B", inter, 16);
        } else {
            std::cout << "  master " << queryMaster << " no comun a ambos terminos.\n";
        }
    }

    // ----------------------------- Verificacion ZDD-nativa ---------------------
    // El sharing del Piso 2 implica que dos punteros iguales -> mismo nodo fisico.
    std::cout << "\n=== VERIFICACION SHARING PISO 2 ===\n";
    if (okA && okB) {
        const MtNode* na = mtNavigate(termRoots[tidA], static_cast<uint32_t>(queryMaster));
        const MtNode* nb = mtNavigate(termRoots[tidB], static_cast<uint32_t>(queryMaster));
        if (na && nb) {
            std::cout << "  ptr(R_A,master=" << queryMaster << ") == ptr(R_B,master) ? "
                      << (na->pattern == nb->pattern ? "SI (misma raiz fisica)" : "no (historias distintas)")
                      << "\n";
            std::cout << "  DagSize(R_A)=" << Cudd_zddDagSize(na->pattern)
                      << " DagSize(R_B)=" << Cudd_zddDagSize(nb->pattern) << "\n";
        }
    }

    // ----------------------------- Metricas CSV --------------------------------
    std::ofstream metrics(metricsCsvPath, std::ios::app);
    if (metrics.good()) {
        if (metrics.tellp() == 0) {
            metrics << "packed_docs,terms,values_packed,pairs_td,distinct_patterns,"
                    << "pool_live_nodes,mtzdd_nodes,mtzdd_naive_nodes,sharing_piso1,"
                    << "cudd_mem_bytes,max_rel,build_s\n";
        }
        double sharing1 = mtzddNodes > 0 ? static_cast<double>(mtzddNaive) / mtzddNodes : 0.0;
        metrics << packedDocsPath << "," << termsToProcess << "," << totalPackedValues << ","
                << totalPairs << "," << distinctPatterns.size() << "," << poolLiveNodes << ","
                << mtzddNodes << "," << mtzddNaive << "," << std::fixed << std::setprecision(4)
                << sharing1 << "," << memInUse << "," << maxRelSeen << ","
                << std::setprecision(6) << buildS << "\n";
        std::cout << "\n[OK] metrics csv: " << metricsCsvPath << "\n";
    }

    // ----------------------------- Cleanup -------------------------------------
    for (DdNode* p : distinctPatterns) Cudd_RecursiveDerefZdd(dd, p);
    int leaked = Cudd_CheckZeroRef(dd);
    if (leaked != 0) std::cout << "[WARN] CUDD refs no liberadas: " << leaked << "\n";
    Cudd_Quit(dd);

    std::cout << "\nFinalizado (blueprint doble piso MTZDD + ZDD).\n";
    return 0;
}
