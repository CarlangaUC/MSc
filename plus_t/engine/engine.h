// =============================================================================
// engine.h — motor ZDD^t unificado: tag {u+t} (u+i) o φ(t) binario (log i)
// =============================================================================
// Universo logico 64-bit (version_packing.h: master 40b, rel 24b):
//   masters m  in [0, U)       U = 2^ZDD_MASTER_BITS
//   marca logica {u+t} in [U+1, U+V] t = termino 1-based, V = |Vocab|
//
// Mapeo CUDD segun codificacion (argv: u+t | log):
//   u+t: tag t (1-based) -> var t;  docOffset = V + 1
//   log: tag bit b -> var (1+b);    docOffset = 1 + tagWidth,  tagWidth = ceil_log2(V+1)
//   master m -> var (docOffset + m)  en ambos modos
//
// Pseudocodigo (For i in t: armar F_t; F_t + {codificacion}; lista_punteros[i] = &ZDD^t):
//   pointerList[i]          <-> lista_punteros[i]
//   TermFtInputs            <-> datos por termino para "armar F_t" (S_i^v)
//   collectVersionSnapshots <-> iterar versiones de la posting list -> S_i^v
//   buildVersionSnapshotZdd <-> construir un S_i^v como ZDD
//   ft                      <-> F_t = union de S_i^v
//   addEncodingToFt         <-> "F_t + {codificacion}"  (u+i o log i segun modo)
//   buildFtPointersForRange <-> loop "For i in t: ..."
// =============================================================================

#ifndef ZDD_PLUS_T_ENGINE_H
#define ZDD_PLUS_T_ENGINE_H

#include "nzdd_cudd_pack.h"
#include "nzdd_cudd_common.h"

#include "cudd.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "version_packing.h"

using namespace NzddCommon;

// u+i = tag canónico {u+t} (1 var CUDD por término). log = φ(t) binario (ceil_log2(V+1) vars).
enum class TagEncoding { UPlusT, LogBits };

static bool parseTagEncoding(const std::string& s, TagEncoding& out) {
    if (s == "u+t") {
        out = TagEncoding::UPlusT;
        return true;
    }
    if (s == "log") {
        out = TagEncoding::LogBits;
        return true;
    }
    std::cerr << "ERROR: codificacion desconocida '" << s << "' (use u+t o log)\n";
    return false;
}

static const char* tagEncodingName(TagEncoding enc) {
    return enc == TagEncoding::UPlusT ? "u+t" : "log";
}

static constexpr uint64_t ZDD_LOGICAL_U = (ZDD_MASTER_BITS >= 64u)
                                              ? ~0ULL
                                              : (1ULL << ZDD_MASTER_BITS);

static uint64_t logicalTagElement(uint64_t termOneBased) {
    return ZDD_LOGICAL_U + termOneBased;
}

static std::string masterSetKey(const std::vector<uint64_t>& masters) {
    std::string key;
    key.resize(masters.size() * sizeof(uint64_t));
    if (!masters.empty()) std::memcpy(&key[0], masters.data(), key.size());
    return key;
}

// u+i: var CUDD = t (1-based).
static int uPlusIVar(uint32_t termOneBased) {
    return static_cast<int>(termOneBased);
}

// log i: ancho en bits y var CUDD del bit k (LSB en k=0).
static uint32_t tagWidthForTerms(uint32_t nTerms) {
    if (nTerms == 0) return 0;
    uint32_t w = 0;
    while ((1ULL << w) <= static_cast<uint64_t>(nTerms)) ++w;
    return w;
}

static int tagVarForBit(int bitIndex) {
    return static_cast<int>(NZDD_ZDD_DOC_LEVEL_OFFSET) + bitIndex;
}

static int tagWidthFromDocOffset(int docOffset) {
    return docOffset - static_cast<int>(NZDD_ZDD_DOC_LEVEL_OFFSET);
}

static bool masterToZddVar(uint64_t master, int docOffset, int& outVar) {
    if (master >= ZDD_LOGICAL_U) return false;
    const uint64_t var64 = master + static_cast<uint64_t>(docOffset);
    if (var64 > static_cast<uint64_t>(std::numeric_limits<int>::max())) return false;
    outVar = static_cast<int>(var64);
    return true;
}

// Construye un S_i^v (una version de la posting list) como ZDD: {masters}.
static DdNode* buildVersionSnapshotZdd(DdManager* dd, const std::vector<uint64_t>& masters,
                                       int docOffset) {
    DdNode* p = Cudd_ReadOne(dd);
    Cudd_Ref(p);
    for (auto it = masters.rbegin(); it != masters.rend(); ++it) {
        int var = 0;
        if (!masterToZddVar(*it, docOffset, var)) {
            Cudd_RecursiveDerefZdd(dd, p);
            return nullptr;
        }
        DdNode* tmp = Cudd_zddChange(dd, p, var);
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

// Datos de un termino necesarios para "armar F_t": el conjunto de S_i^v
// (versiones distintas, cada una como set de masters) tras deduplicar.
struct TermFtInputs {
    uint32_t nVersions = 0;
    uint32_t versionSnapshotCount = 0;
    uint32_t nPostings = 0;
    uint32_t maxRel = 0;
    std::vector<std::vector<uint64_t>> versionSnapshots;
    bool parsed = false;
};

// Itera las versiones de la posting list de un termino y arma cada S_i^v:
// agrupa masters por rel (version), deduplica y descarta vacios/invalidos.
// Ejemplo: postings {(1,1),(1,2),(2,2)} (master,rel) -> S^1={1}, S^2={1,2}.
static TermFtInputs collectVersionSnapshots(const std::vector<uint64_t>& postings) {
    TermFtInputs out;
    out.nPostings = static_cast<uint32_t>(postings.size());
    std::unordered_map<uint64_t, std::vector<uint64_t>> snapsByRel;
    snapsByRel.reserve(postings.size() / 4 + 1);
    for (uint64_t x : postings) {
        snapsByRel[ZDD_UNPACK_REL(x)].push_back(ZDD_UNPACK_MASTER(x));
    }
    out.nVersions = static_cast<uint32_t>(snapsByRel.size());
    for (const auto& kv : snapsByRel) {
        if (kv.first > out.maxRel) out.maxRel = kv.first;
    }
    std::unordered_map<std::string, std::vector<uint64_t>> builtByMasters;
    builtByMasters.reserve(snapsByRel.size());
    for (auto& kv : snapsByRel) {
        std::vector<uint64_t>& masters64 = kv.second;
        std::sort(masters64.begin(), masters64.end());
        masters64.erase(std::unique(masters64.begin(), masters64.end()), masters64.end());
        if (masters64.empty()) continue;
        masters64.erase(std::remove_if(masters64.begin(), masters64.end(),
                                       [](uint64_t m) { return m >= ZDD_LOGICAL_U; }),
                        masters64.end());
        if (masters64.empty()) continue;
        std::string key = masterSetKey(masters64);
        if (builtByMasters.find(key) == builtByMasters.end())
            builtByMasters.emplace(std::move(key), std::move(masters64));
    }
    out.versionSnapshots.reserve(builtByMasters.size());
    for (auto& kv : builtByMasters) out.versionSnapshots.push_back(std::move(kv.second));
    out.versionSnapshotCount = static_cast<uint32_t>(out.versionSnapshots.size());
    out.parsed = true;
    return out;
}

struct TermMetrics {
    uint64_t dagSize = 0;
    uint64_t nSubsets = 0;
    uint64_t termTagLogical = 0;
    std::vector<uint64_t> masters;
};

struct BuildResult {
    DdManager* dd = nullptr;
    std::vector<DdNode*> pointerList;      // Lista_Punteros: pointerList[t] = &ZDD^t (= F_t + tag)
    std::vector<DdNode*> sharedSnapshots;  // S_i^v compartidos entre terminos (cache dedup)
    std::vector<TermMetrics> metrics;
    int numZddVars = 0;
    uint32_t nTerms = 0;
    int docOffset = 0;
    uint32_t tagWidth = 0;
    TagEncoding encoding = TagEncoding::UPlusT;
    uint32_t limit = 0;
    long poolNodes = 0;
    double parseS = 0;
    double buildS = 0;
    double mergeS = 0;
    int numShards = 1;
};

static bool computeZddLayout(uint64_t maxMaster, uint32_t nTerms, TagEncoding enc, int& docOffset,
                             int& numZddVars, uint32_t& tagWidth) {
    if (maxMaster >= ZDD_LOGICAL_U) {
        std::cerr << "ERROR: master id >= 2^" << ZDD_MASTER_BITS << " (" << maxMaster << ")\n";
        return false;
    }
    uint64_t docOff64 = 0;
    if (enc == TagEncoding::UPlusT) {
        tagWidth = 0;
        docOff64 = static_cast<uint64_t>(nTerms) + static_cast<uint64_t>(NZDD_ZDD_DOC_LEVEL_OFFSET);
    } else {
        tagWidth = tagWidthForTerms(nTerms);
        docOff64 = static_cast<uint64_t>(tagWidth) + static_cast<uint64_t>(NZDD_ZDD_DOC_LEVEL_OFFSET);
    }
    const uint64_t numVars64 = maxMaster + docOff64 + 1u;
    if (docOff64 > static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
        numVars64 > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        std::cerr << "ERROR: numZddVars overflow (maxMaster=" << maxMaster << " nTerms=" << nTerms
                  << " tagWidth=" << tagWidth << " enc=" << tagEncodingName(enc) << ")\n";
        return false;
    }
    docOffset = static_cast<int>(docOff64);
    numZddVars = static_cast<int>(numVars64);
    return true;
}

static bool parseDocsTerms(const std::string& docsPath, uint32_t maxTerms, TagEncoding enc,
                           std::vector<TermFtInputs>& termData, uint32_t& limit, int& docOffset,
                           int& numZddVars, uint32_t& tagWidth, double& parseS) {
    DocsIndex docsIdx = buildDocsIndex(docsPath);
    if (docsIdx.nlists == 0 || docsIdx.listOffsets.empty()) {
        std::cerr << "ERROR: indice invalido " << docsPath << std::endl;
        return false;
    }
    limit = (maxTerms > 0 && maxTerms < docsIdx.nlists) ? maxTerms : docsIdx.nlists;

    auto t0 = std::chrono::steady_clock::now();
    termData.assign(limit, TermFtInputs{});
    uint64_t globalMaxMaster = 0;

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic, 16) reduction(max : globalMaxMaster)
#endif
    for (int t = 0; t < static_cast<int>(limit); ++t) {
        std::vector<uint64_t> postings;
        if (!readPostingListAt(docsPath, docsIdx.listOffsets[static_cast<uint32_t>(t)], postings))
            continue;
        for (uint64_t x : postings) {
            uint64_t m = ZDD_UNPACK_MASTER(x);
            if (m > globalMaxMaster) globalMaxMaster = m;
        }
        termData[static_cast<uint32_t>(t)] = collectVersionSnapshots(postings);
    }

    auto tParse = std::chrono::steady_clock::now();
    if (!computeZddLayout(globalMaxMaster, limit, enc, docOffset, numZddVars, tagWidth)) return false;
    parseS = std::chrono::duration<double>(tParse - t0).count();
    return true;
}

// log i: φ(t) = subset de vars tag = bits de t en binario.
static DdNode* buildLogEncodingSubset(DdManager* dd, uint32_t termOneBased, int docOffset) {
    const int tw = tagWidthFromDocOffset(docOffset);
    DdNode* encodingSet = Cudd_ReadOne(dd);
    Cudd_Ref(encodingSet);
    for (int b = 0; b < tw; ++b) {
        if ((termOneBased & (1u << b)) == 0) continue;
        DdNode* tmp = Cudd_zddChange(dd, encodingSet, tagVarForBit(b));
        if (tmp == nullptr) {
            Cudd_RecursiveDerefZdd(dd, encodingSet);
            return nullptr;
        }
        Cudd_Ref(tmp);
        Cudd_RecursiveDerefZdd(dd, encodingSet);
        encodingSet = tmp;
    }
    return encodingSet;
}

// "F_t + {codificacion}": ZDD^t = F_t U {tag(t)}; ambas entradas son familias.
// Invariante: encodingSet queda con exactamente UNA referencia propia de esta
// funcion en ambas ramas, y se libera despues de referenciar el resultado.
static DdNode* addEncodingToFt(DdManager* dd, DdNode* ft, uint32_t termOneBased, int docOffset,
                               TagEncoding enc) {
    if (ft == nullptr) return ft;
    DdNode* encodingSet = nullptr;
    if (enc == TagEncoding::UPlusT) {
        // u+i: singleton {u+t} en 1 var CUDD (= t 1-based).
        DdNode* one = Cudd_ReadOne(dd);
        Cudd_Ref(one);
        encodingSet = Cudd_zddChange(dd, one, uPlusIVar(termOneBased));
        if (encodingSet != nullptr) Cudd_Ref(encodingSet);
        Cudd_RecursiveDerefZdd(dd, one);
    } else {
        // log i: φ(t) = bits de t encendidos en tagWidth vars. Ya viene referenciado.
        encodingSet = buildLogEncodingSubset(dd, termOneBased, docOffset);
    }
    if (encodingSet == nullptr) return nullptr;
    DdNode* ftWithEncoding = Cudd_zddUnion(dd, ft, encodingSet);
    if (ftWithEncoding != nullptr) Cudd_Ref(ftWithEncoding);
    Cudd_RecursiveDerefZdd(dd, encodingSet);
    return ftWithEncoding;
}

static bool extractTagOnlyPath(DdManager* dd, DdNode* z, int docOffset, std::vector<int>& path,
                               std::vector<int>& outTagPath) {
    if (z == nullptr || z == Cudd_ReadZero(dd)) return false;
    if (Cudd_IsConstant(z)) {
        if (z != Cudd_ReadOne(dd)) return false;
        if (path.empty()) return false;
        for (int v : path) {
            if (v >= docOffset) return false;
        }
        outTagPath = path;
        return true;
    }
    if (extractTagOnlyPath(dd, Cudd_E(z), docOffset, path, outTagPath)) return true;
    path.push_back(Cudd_NodeReadIndex(z));
    if (extractTagOnlyPath(dd, Cudd_T(z), docOffset, path, outTagPath)) return true;
    path.pop_back();
    return false;
}

static uint32_t decodeTermOneBasedFromTagPath(const std::vector<int>& tagPath) {
    uint32_t code = 0;
    for (int v : tagPath) {
        const int bit = v - static_cast<int>(NZDD_ZDD_DOC_LEVEL_OFFSET);
        if (bit >= 0) code |= (1u << static_cast<uint32_t>(bit));
    }
    return code;
}

static int readTermIdFromTagSearch(DdManager* dd, DdNode* z, int docOffset, TagEncoding enc) {
    if (z == nullptr || z == Cudd_ReadZero(dd) || Cudd_IsConstant(z)) return -1;
    if (enc == TagEncoding::UPlusT) {
        const int v = Cudd_NodeReadIndex(z);
        if (v >= NZDD_ZDD_DOC_LEVEL_OFFSET && v < docOffset && Cudd_T(z) == Cudd_ReadOne(dd)) {
            return v - NZDD_ZDD_DOC_LEVEL_OFFSET;
        }
        int k = readTermIdFromTagSearch(dd, Cudd_E(z), docOffset, enc);
        if (k >= 0) return k;
        return readTermIdFromTagSearch(dd, Cudd_T(z), docOffset, enc);
    }
    std::vector<int> path;
    std::vector<int> tagPath;
    if (!extractTagOnlyPath(dd, z, docOffset, path, tagPath)) return -1;
    const uint32_t termOneBased = decodeTermOneBasedFromTagPath(tagPath);
    if (termOneBased == 0) return -1;
    return static_cast<int>(termOneBased) - 1;
}

// "For i in t: armar F_t; ZDD^t = F_t + codificacion; pointerList[i] = &ZDD^t"
static bool buildFtPointersForRange(
    DdManager* dd, const std::vector<TermFtInputs>& termData, uint32_t first, uint32_t count,
    int docOffset, TagEncoding enc, std::unordered_map<std::string, DdNode*>& snapshotCache,
    std::vector<DdNode*>& sharedSnapshots, std::vector<DdNode*>& pointerListRange,
    std::ofstream* evoLog = nullptr, uint32_t logEvery = 500, uint64_t* totalIntsAcc = nullptr,
    const std::chrono::steady_clock::time_point* tBuild0 = nullptr) {
    pointerListRange.assign(count, nullptr);
    uint64_t localInts = totalIntsAcc ? *totalIntsAcc : 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t t = first + i;
        if (!termData[t].parsed) return false;
        localInts += termData[t].nPostings;
        if (termData[t].versionSnapshots.empty()) {
            const uint32_t paso = first + i + 1;
            if (evoLog && evoLog->is_open() && tBuild0 &&
                (paso % logEvery == 0 || paso == first + count)) {
                const double elapsed =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - *tBuild0).count();
                writeEvolutionRow(*evoLog, paso, localInts, dd, elapsed);
            }
            continue;
        }

        // "armar F_t": construir cada S_i^v (con cache entre terminos) y unirlos.
        std::vector<DdNode*> versionSnapshotNodes;
        versionSnapshotNodes.reserve(termData[t].versionSnapshots.size());
        for (const std::vector<uint64_t>& masters : termData[t].versionSnapshots) {
            std::string key = masterSetKey(masters);
            DdNode* snap = nullptr;
            auto it = snapshotCache.find(key);
            if (it != snapshotCache.end()) {
                snap = it->second;
            } else {
                snap = buildVersionSnapshotZdd(dd, masters, docOffset);
                if (snap == nullptr) return false;
                snapshotCache.emplace(std::move(key), snap);
                sharedSnapshots.push_back(snap);
            }
            versionSnapshotNodes.push_back(snap);
        }
        DdNode* ft = zddUnionBalancedCudd(dd, std::move(versionSnapshotNodes));
        if (ft == nullptr) return false;
        const uint32_t termOneBased = t + 1u;
        // "F_t + {codificacion}"  (u+i o log i segun enc)
        DdNode* ftWithEncoding = addEncodingToFt(dd, ft, termOneBased, docOffset, enc);
        if (ftWithEncoding == nullptr) {
            Cudd_RecursiveDerefZdd(dd, ft);
            return false;
        }
        Cudd_RecursiveDerefZdd(dd, ft);
        // "lista_punteros[i] = &ZDD^t"
        pointerListRange[i] = ftWithEncoding;

        const uint32_t paso = first + i + 1;
        if (evoLog && evoLog->is_open() && tBuild0 &&
            (paso % logEvery == 0 || paso == first + count)) {
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - *tBuild0).count();
            writeEvolutionRow(*evoLog, paso, localInts, dd, elapsed);
        }
    }
    if (totalIntsAcc) *totalIntsAcc = localInts;
    return true;
}

static void fillMetrics(DdManager* dd, DdNode* z, TermMetrics& m, int docOffset, TagEncoding enc) {
    if (z == nullptr || z == Cudd_ReadZero(dd)) {
        m.dagSize = 0;
        m.nSubsets = 0;
        m.termTagLogical = 0;
        m.masters.clear();
        return;
    }
    m.dagSize = static_cast<uint64_t>(Cudd_zddDagSize(z));
    m.nSubsets = ZddPack::countSubsets(dd, z);
    const int tagRead = readTermIdFromTagSearch(dd, z, docOffset, enc);
    m.termTagLogical = (tagRead >= 0) ? logicalTagElement(static_cast<uint64_t>(tagRead) + 1u) : 0u;
    m.masters = ZddPack::mastersOfZdd(dd, z, docOffset);
}

static bool buildForest(const std::string& docsPath, uint32_t maxTerms, TagEncoding enc,
                        BuildResult& out, const std::string& logCsvPath = "",
                        uint32_t logEvery = 500) {
    std::vector<TermFtInputs> termData;
    uint32_t limit = 0;
    int numZddVars = 0;
    int docOffset = 0;
    uint32_t tagWidth = 0;
    // postings(t) = [(1,1), (1,2), (2,2)] (master,rel)
    // Por cada t: lee su posting list y agrupa por version -> S_i^1={1}, S_i^2={1,2}, etc.
    if (!parseDocsTerms(docsPath, maxTerms, enc, termData, limit, docOffset, numZddVars, tagWidth,
                        out.parseS))
        return false;

    const uint32_t nTerms = limit;
    std::cout << "[INFO] enc=" << tagEncodingName(enc) << " nTerms(V)=" << nTerms << " U=2^"
              << ZDD_MASTER_BITS << " tagWidth=" << tagWidth << " docOffset=" << docOffset
              << " numZddVars=" << numZddVars << std::endl;

    std::ofstream evoLog;
    const bool logEnabled = !logCsvPath.empty();
    if (logEnabled) {
        evoLog.open(logCsvPath);
        if (!evoLog.is_open()) {
            std::cerr << "ERROR: no se pudo abrir log " << logCsvPath << std::endl;
            return false;
        }
        evoLog << "Paso,Total_Ints,Nodos_Pool,Bytes_CUDD,RSS_KB,Tiempo_s,bpi_build\n";
    }

    auto tBuild0 = std::chrono::steady_clock::now();
    DdManager* dd = Cudd_Init(0, numZddVars, NZDD_INIT_UNIQUE_SLOTS, CUDD_CACHE_SLOTS, 0);
    if (dd == nullptr) return false;
    configureCuddManager(dd);

    if (logEnabled) writeEvolutionRow(evoLog, 0, 0, dd, 0.0);

    std::unordered_map<std::string, DdNode*> snapshotCache;
    snapshotCache.reserve(limit * 2);
    std::vector<DdNode*> sharedSnapshots;
    std::vector<DdNode*> pointerList;  // = Lista_Punteros = []
    uint64_t totalInts = 0;
    // For i in t: armar F_t (union de S_i^v); ZDD^t = F_t + {codificacion}; pointerList[i] = &ZDD^t
    if (!buildFtPointersForRange(dd, termData, 0, limit, docOffset, enc, snapshotCache,
                                 sharedSnapshots, pointerList, logEnabled ? &evoLog : nullptr,
                                 logEvery, &totalInts, &tBuild0)) {
        Cudd_Quit(dd);
        return false;
    }
    // CSV de evolución cierra aquí: Bytes_CUDD es pre-trim (bpi_build diagnóstico, no EDD operativa).
    if (logEnabled) evoLog.close();

    std::vector<TermMetrics> metrics(limit);
    for (uint32_t t = 0; t < limit; ++t) {
        fillMetrics(dd, pointerList[t], metrics[t], docOffset, enc);
    }

    auto t1 = std::chrono::steady_clock::now();
    out.dd = dd;
    out.pointerList = std::move(pointerList);
    out.sharedSnapshots = std::move(sharedSnapshots);
    out.metrics = std::move(metrics);
    out.numZddVars = numZddVars;
    out.nTerms = nTerms;
    out.docOffset = docOffset;
    out.tagWidth = tagWidth;
    out.encoding = enc;
    out.limit = limit;
    out.poolNodes = Cudd_zddReadNodeCount(dd);
    out.buildS = std::chrono::duration<double>(t1 - tBuild0).count();
    out.mergeS = 0;
    out.numShards = 1;
    return true;
}

static void releaseForest(BuildResult& br) {
    if (br.dd == nullptr) return;
    ZddPack::freeTermZdd(br.dd, br.pointerList);
    for (DdNode* s : br.sharedSnapshots) Cudd_RecursiveDerefZdd(br.dd, s);
    br.sharedSnapshots.clear();
    Cudd_Quit(br.dd);
    br.dd = nullptr;
}

static void trimSnapshotRefs(BuildResult& br) {
    for (DdNode* s : br.sharedSnapshots) Cudd_RecursiveDerefZdd(br.dd, s);
    br.sharedSnapshots.clear();
    br.poolNodes = Cudd_zddReadNodeCount(br.dd);
}

#endif  // ZDD_PLUS_T_ENGINE_H
