// =============================================================================
// nzdd_cudd_serialize.cpp — build/save/load/verify bosque ZDD^t (CUDD + .zpack)
// =============================================================================
// Compilar (desde raiz MAGISTER):
//   g++ -O2 -std=c++17 -fopenmp -o nzdd_cudd_serialize \
//       zdd_cudd.cpp \
//       -I ./cudd/cudd -I ./cudd -L ./cudd/cudd/.libs \
//       -Wl,-rpath,'$ORIGIN/cudd/cudd/.libs' -lcudd \
//       -I ./TdZdd/include \
//       -I ./uiHRDC/uiHRDC/indexes/NOPOS/II_docs/src/utils
//
// Modos:
//   build         <docs> <voc> <out.zpack|none> [max_terms] [log_csv] [log_every]
//   pbuild        <docs> <voc> <out.zpack|none> [max_terms] [num_shards]
//   load          <in.zpack> [voc] [spot_word]
//   verify        <docs> <voc> <tmp.zpack> [max_terms]
//   verify-pbuild <docs> <voc> [max_terms] [num_shards]
//   compare-tdzdd <in.zpack> <tdzdd_metrics.csv.nodes> [voc] [spot_word]
// =============================================================================

#include "nzdd_cudd_pack.h"
#include "nzdd_cudd_common.h"

#include <cstddef>
#include <cstdio>

#include "cudd.h"

#include <tdzdd/util/ResourceUsage.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
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

static std::string masterSetKey(const std::vector<uint32_t>& masters) {
    std::string key;
    key.resize(masters.size() * sizeof(uint32_t));
    if (!masters.empty()) std::memcpy(&key[0], masters.data(), key.size());
    return key;
}

static int masterToZddVar(uint32_t master) {
    return static_cast<int>(master) + NZDD_ZDD_DOC_LEVEL_OFFSET;
}

static DdNode* buildSnapshotZdd(DdManager* dd, const std::vector<uint32_t>& masters) {
    DdNode* p = Cudd_ReadOne(dd);
    Cudd_Ref(p);
    for (auto it = masters.rbegin(); it != masters.rend(); ++it) {
        int var = masterToZddVar(*it);
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

struct TermSnapData {
    uint32_t nVersions = 0;
    uint32_t nSnapshots = 0;
    uint32_t nPostings = 0;
    std::vector<std::vector<uint32_t>> uniqueSnapshots;
    bool parsed = false;
};

static TermSnapData parseTermPostings(const std::vector<uint64_t>& postings) {
    TermSnapData out;
    out.nPostings = static_cast<uint32_t>(postings.size());
    std::unordered_map<uint64_t, std::vector<uint64_t>> snapsByRel;
    snapsByRel.reserve(postings.size() / 4 + 1);
    for (uint64_t x : postings) {
        snapsByRel[ZDD_UNPACK_REL(x)].push_back(ZDD_UNPACK_MASTER(x));
    }
    out.nVersions = static_cast<uint32_t>(snapsByRel.size());
    std::unordered_map<std::string, std::vector<uint32_t>> builtByMasters;
    builtByMasters.reserve(snapsByRel.size());
    for (auto& kv : snapsByRel) {
        std::vector<uint64_t>& masters64 = kv.second;
        std::sort(masters64.begin(), masters64.end());
        masters64.erase(std::unique(masters64.begin(), masters64.end()), masters64.end());
        if (masters64.empty()) continue;
        std::vector<uint32_t> masters;
        masters.reserve(masters64.size());
        for (uint64_t m : masters64) masters.push_back(static_cast<uint32_t>(m));
        std::string key = masterSetKey(masters);
        if (builtByMasters.find(key) == builtByMasters.end())
            builtByMasters.emplace(std::move(key), std::move(masters));
    }
    out.uniqueSnapshots.reserve(builtByMasters.size());
    for (auto& kv : builtByMasters) out.uniqueSnapshots.push_back(std::move(kv.second));
    out.nSnapshots = static_cast<uint32_t>(out.uniqueSnapshots.size());
    out.parsed = true;
    return out;
}

struct TermMetrics {
    uint64_t dagSize = 0;
    uint64_t nSubsets = 0;
    std::vector<uint64_t> masters;
};

struct BuildResult {
    DdManager* dd = nullptr;
    std::vector<DdNode*> termZdd;
    std::vector<DdNode*> distinctSnaps;
    std::vector<TermMetrics> metrics;
    int numZddVars = 0;
    uint32_t limit = 0;
    long poolNodes = 0;
    double parseS = 0;
    double buildS = 0;
    double mergeS = 0;
    int numShards = 1;
};

static bool parseDocsTerms(const std::string& docsPath, uint32_t maxTerms,
                           std::vector<TermSnapData>& termData, uint32_t& limit,
                           int& numZddVars, double& parseS) {
    DocsIndex docsIdx = buildDocsIndex(docsPath);
    if (docsIdx.nlists == 0 || docsIdx.listOffsets.empty()) {
        std::cerr << "ERROR: indice invalido " << docsPath << std::endl;
        return false;
    }
    limit = (maxTerms > 0 && maxTerms < docsIdx.nlists) ? maxTerms : docsIdx.nlists;

    auto t0 = std::chrono::steady_clock::now();
    termData.assign(limit, TermSnapData{});
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
        termData[static_cast<uint32_t>(t)] = parseTermPostings(postings);
    }

    auto tParse = std::chrono::steady_clock::now();
    numZddVars = static_cast<int>(globalMaxMaster) + NZDD_ZDD_DOC_LEVEL_OFFSET + 1;
    parseS = std::chrono::duration<double>(tParse - t0).count();
    return true;
}

static bool buildRangeIntoManager(
    DdManager* dd, const std::vector<TermSnapData>& termData, uint32_t first, uint32_t count,
    std::unordered_map<std::string, DdNode*>& snapByKey, std::vector<DdNode*>& distinctSnaps,
    std::vector<DdNode*>& rangeZdd, std::ofstream* evoLog = nullptr, uint32_t logEvery = 500,
    uint64_t* totalIntsAcc = nullptr,
    const std::chrono::steady_clock::time_point* tBuild0 = nullptr) {
    rangeZdd.assign(count, nullptr);
    uint64_t localInts = totalIntsAcc ? *totalIntsAcc : 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t t = first + i;
        if (!termData[t].parsed) return false;
        localInts += termData[t].nPostings;
        if (termData[t].uniqueSnapshots.empty()) {
            const uint32_t paso = first + i + 1;
            if (evoLog && evoLog->is_open() && tBuild0 &&
                (paso % logEvery == 0 || paso == first + count)) {
                const double elapsed =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - *tBuild0).count();
                writeEvolutionRow(*evoLog, paso, localInts, dd, elapsed);
            }
            continue;
        }

        std::vector<DdNode*> parts;
        parts.reserve(termData[t].uniqueSnapshots.size());
        for (const std::vector<uint32_t>& masters : termData[t].uniqueSnapshots) {
            std::string key = masterSetKey(masters);
            DdNode* snap = nullptr;
            auto it = snapByKey.find(key);
            if (it != snapByKey.end()) {
                snap = it->second;
            } else {
                snap = buildSnapshotZdd(dd, masters);
                if (snap == nullptr) return false;
                snapByKey.emplace(std::move(key), snap);
                distinctSnaps.push_back(snap);
            }
            parts.push_back(snap);
        }
        DdNode* family = zddUnionBalancedCudd(dd, std::move(parts));
        if (family == nullptr) return false;
        rangeZdd[i] = family;

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

static void fillMetrics(DdManager* dd, DdNode* z, TermMetrics& m, int docOffset) {
    if (z == nullptr || z == Cudd_ReadZero(dd)) {
        m.dagSize = 0;
        m.nSubsets = 0;
        m.masters.clear();
        return;
    }
    m.dagSize = static_cast<uint64_t>(Cudd_zddDagSize(z));
    m.nSubsets = ZddPack::countSubsets(dd, z);
    m.masters = ZddPack::mastersOfZdd(dd, z, docOffset);
}

static bool buildForest(const std::string& docsPath, uint32_t maxTerms, BuildResult& out,
                        const std::string& logCsvPath = "", uint32_t logEvery = 500) {
    std::vector<TermSnapData> termData;
    uint32_t limit = 0;
    int numZddVars = 0;
    if (!parseDocsTerms(docsPath, maxTerms, termData, limit, numZddVars, out.parseS)) return false;

    std::ofstream evoLog;
    const bool logEnabled = !logCsvPath.empty();
    if (logEnabled) {
        evoLog.open(logCsvPath);
        if (!evoLog.is_open()) {
            std::cerr << "ERROR: no se pudo abrir log " << logCsvPath << std::endl;
            return false;
        }
        evoLog << "Paso,Total_Ints,Nodos_Pool,Bytes_CUDD,RSS_KB,Tiempo_s\n";
    }

    auto tBuild0 = std::chrono::steady_clock::now();
    DdManager* dd = Cudd_Init(0, numZddVars, NZDD_INIT_UNIQUE_SLOTS, CUDD_CACHE_SLOTS, 0);
    if (dd == nullptr) return false;
    configureCuddManager(dd);

    if (logEnabled) writeEvolutionRow(evoLog, 0, 0, dd, 0.0);

    std::unordered_map<std::string, DdNode*> snapByKey;
    snapByKey.reserve(limit * 2);
    std::vector<DdNode*> distinctSnaps;
    std::vector<DdNode*> termZdd;
    uint64_t totalInts = 0;
    if (!buildRangeIntoManager(dd, termData, 0, limit, snapByKey, distinctSnaps, termZdd,
                               logEnabled ? &evoLog : nullptr, logEvery, &totalInts, &tBuild0)) {
        Cudd_Quit(dd);
        return false;
    }
    if (logEnabled) evoLog.close();

    std::vector<TermMetrics> metrics(limit);
    for (uint32_t t = 0; t < limit; ++t) {
        fillMetrics(dd, termZdd[t], metrics[t], NZDD_ZDD_DOC_LEVEL_OFFSET);
    }

    auto t1 = std::chrono::steady_clock::now();
    out.dd = dd;
    out.termZdd = std::move(termZdd);
    out.distinctSnaps = std::move(distinctSnaps);
    out.metrics = std::move(metrics);
    out.numZddVars = numZddVars;
    out.limit = limit;
    out.poolNodes = Cudd_zddReadNodeCount(dd);
    out.buildS = std::chrono::duration<double>(t1 - tBuild0).count();
    out.mergeS = 0;
    out.numShards = 1;
    return true;
}

static int defaultNumShards() {
#ifdef _OPENMP
    const int n = omp_get_max_threads();
    return (n > 0) ? n : 1;
#else
    return 1;
#endif
}

static void computeShardRanges(uint32_t limit, int numShards,
                               std::vector<std::pair<uint32_t, uint32_t>>& shardRanges) {
    shardRanges.assign(static_cast<size_t>(numShards), {0, 0});
    if (limit == 0 || numShards <= 0) return;
    const uint32_t perShard = (limit + static_cast<uint32_t>(numShards) - 1u) /
                              static_cast<uint32_t>(numShards);
    for (int s = 0; s < numShards; ++s) {
        const uint32_t first = static_cast<uint32_t>(s) * perShard;
        if (first >= limit) continue;
        const uint32_t count = std::min(perShard, limit - first);
        shardRanges[static_cast<size_t>(s)] = {first, count};
    }
}

static bool parallelBuildForest(const std::string& docsPath, uint32_t maxTerms, int numShards,
                                BuildResult& out) {
    if (numShards <= 0) numShards = 1;

    std::vector<TermSnapData> termData;
    uint32_t limit = 0;
    int numZddVars = 0;
    if (!parseDocsTerms(docsPath, maxTerms, termData, limit, numZddVars, out.parseS)) return false;

    std::vector<std::pair<uint32_t, uint32_t>> shardRanges;
    computeShardRanges(limit, numShards, shardRanges);

    std::vector<ZddPack::ZddPackData> shards(static_cast<size_t>(numShards));

    auto tBuild0 = std::chrono::steady_clock::now();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int s = 0; s < numShards; ++s) {
        const auto range = shardRanges[static_cast<size_t>(s)];
        if (range.second == 0) continue;

        DdManager* local = Cudd_Init(0, numZddVars, NZDD_INIT_UNIQUE_SLOTS, CUDD_CACHE_SLOTS, 0);
        if (local == nullptr) continue;
        configureCuddManager(local);

        std::unordered_map<std::string, DdNode*> snapByKey;
        snapByKey.reserve(range.second * 2 + 16);
        std::vector<DdNode*> distinctSnaps;
        std::vector<DdNode*> rangeZdd;
        if (!buildRangeIntoManager(local, termData, range.first, range.second, snapByKey,
                                   distinctSnaps, rangeZdd)) {
            ZddPack::freeTermZdd(local, rangeZdd);
            for (DdNode* snap : distinctSnaps) Cudd_RecursiveDerefZdd(local, snap);
            Cudd_Quit(local);
            continue;
        }

        shards[static_cast<size_t>(s)] =
            ZddPack::extractPackData(local, rangeZdd, numZddVars, NZDD_ZDD_DOC_LEVEL_OFFSET);

        ZddPack::freeTermZdd(local, rangeZdd);
        for (DdNode* snap : distinctSnaps) Cudd_RecursiveDerefZdd(local, snap);
        Cudd_Quit(local);
    }
    auto tBuild1 = std::chrono::steady_clock::now();

    auto tMerge0 = std::chrono::steady_clock::now();
    DdManager* dd = nullptr;
    std::vector<DdNode*> termZdd;
    if (!ZddPack::assembleForest(dd, termZdd, numZddVars, NZDD_ZDD_DOC_LEVEL_OFFSET, shards,
                                 shardRanges)) {
        return false;
    }
    auto tMerge1 = std::chrono::steady_clock::now();

    std::vector<TermMetrics> metrics(limit);
    for (uint32_t t = 0; t < limit; ++t) {
        fillMetrics(dd, termZdd[t], metrics[t], NZDD_ZDD_DOC_LEVEL_OFFSET);
    }

    out.dd = dd;
    out.termZdd = std::move(termZdd);
    out.distinctSnaps.clear();
    out.metrics = std::move(metrics);
    out.numZddVars = numZddVars;
    out.limit = limit;
    out.poolNodes = Cudd_zddReadNodeCount(dd);
    out.buildS = std::chrono::duration<double>(tBuild1 - tBuild0).count();
    out.mergeS = std::chrono::duration<double>(tMerge1 - tMerge0).count();
    out.numShards = numShards;
    return true;
}

static void releaseForest(BuildResult& br) {
    if (br.dd == nullptr) return;
    ZddPack::freeTermZdd(br.dd, br.termZdd);
    for (DdNode* s : br.distinctSnaps) Cudd_RecursiveDerefZdd(br.dd, s);
    br.distinctSnaps.clear();
    Cudd_Quit(br.dd);
    br.dd = nullptr;
}

static void trimSnapshotRefs(BuildResult& br) {
    for (DdNode* s : br.distinctSnaps) Cudd_RecursiveDerefZdd(br.dd, s);
    br.distinctSnaps.clear();
    br.poolNodes = Cudd_zddReadNodeCount(br.dd);
}

static void spotCheck(DdManager* dd, const std::vector<DdNode*>& termZdd, const Vocabulary& voc,
                      int spotId, int docOffset) {
    if (spotId < 0) {
        std::cout << "Spot: no resuelto\n";
        return;
    }
    std::cout << "term_id=" << spotId;
    if (voc.loaded && static_cast<uint32_t>(spotId) < voc.nwords)
        std::cout << " palabra='" << voc.words[static_cast<uint32_t>(spotId)] << "'";
    std::cout << std::endl;
    DdNode* z = termZdd[static_cast<uint32_t>(spotId)];
    if (z == nullptr || z == Cudd_ReadZero(dd)) {
        std::cout << "ZDD^t: vacio\n";
        return;
    }
    std::cout << "DagSize=" << Cudd_zddDagSize(z) << " |F_t|=" << ZddPack::countSubsets(dd, z)
              << std::endl;
    printSet("|M_t|", ZddPack::mastersOfZdd(dd, z, docOffset));
}

static bool metricsEqual(const TermMetrics& a, const TermMetrics& b) {
    return a.dagSize == b.dagSize && a.nSubsets == b.nSubsets && a.masters == b.masters;
}

static bool loadTdZddNodeCounts(const std::string& path, std::vector<uint64_t>& nodesOut) {
    std::ifstream in(path);
    if (!in.is_open()) {
        std::cerr << "ERROR: no se pudo abrir nodos TdZdd: " << path << std::endl;
        return false;
    }
    uint32_t maxTerm = 0;
    std::vector<uint64_t> tmp;
    uint32_t termId = 0;
    uint64_t nodos = 0;
    while (in >> termId >> nodos) {
        if (termId >= tmp.size()) tmp.resize(static_cast<size_t>(termId) + 1u, 0);
        tmp[termId] = nodos;
        if (termId > maxTerm) maxTerm = termId;
    }
    nodesOut.assign(static_cast<size_t>(maxTerm) + 1u, 0);
    for (uint32_t t = 0; t <= maxTerm; ++t) {
        if (t < tmp.size()) nodesOut[t] = tmp[t];
    }
    return !nodesOut.empty();
}

static void usage(const char* prog) {
    std::cerr
        << "Uso:\n"
        << "  " << prog
        << " build         <docs> <voc> <out.zpack|none> [max_terms] [log_csv] [log_every]\n"
        << "  " << prog
        << " pbuild        <docs> <voc> <out.zpack|none> [max_terms] [num_shards]\n"
        << "  " << prog << " load          <in.zpack> [voc] [spot_word]\n"
        << "  " << prog << " verify        <docs> <voc> <tmp.zpack> [max_terms]\n"
        << "  " << prog << " verify-pbuild <docs> <voc> [max_terms] [num_shards]\n"
        << "  " << prog << " compare-tdzdd <in.zpack> <tdzdd.csv.nodes> [voc] [spot_word]\n"
        << "\n"
        << "  out.zpack=none|-  construye sin serializar\n"
        << "  log_csv/log_every solo en build (evolucion bpi/memoria)\n";
}

// tag: prefijo de log ("BUILD"/"PBUILD") para distinguir el origen en stdout.
static int saveBuildResult(BuildResult& br, const std::string& packPath,
                           const std::string& tag = "BUILD") {
    if (!shouldSavePack(packPath)) {
        std::cout << "[" << tag << "] serializacion omitida (out=" << packPath << ")\n";
        return 0;
    }
    auto tSave0 = std::chrono::steady_clock::now();
    if (!ZddPack::saveZddPack(br.dd, br.termZdd, br.numZddVars, NZDD_ZDD_DOC_LEVEL_OFFSET,
                              packPath)) {
        return 1;
    }
    auto tSave1 = std::chrono::steady_clock::now();
    const double saveS = std::chrono::duration<double>(tSave1 - tSave0).count();
    std::ifstream chk(packPath, std::ios::binary | std::ios::ate);
    const auto packBytes = chk.tellg();
    std::cout << "pack: " << packPath << " (" << packBytes << " bytes) save=" << saveS << "s\n";
    return 0;
}

static int cmdBuild(int argc, char** argv) {
    if (argc < 5) {
        usage(argv[0]);
        return 1;
    }
    const std::string docsPath = argv[2];
    const std::string vocPath = argv[3];
    const std::string packPath = argv[4];
    const uint32_t maxTerms = (argc >= 6) ? static_cast<uint32_t>(std::stoul(argv[5])) : 0u;
    const std::string logCsv = (argc >= 7) ? argv[6] : "";
    const uint32_t logEvery = (argc >= 8) ? static_cast<uint32_t>(std::stoul(argv[7])) : 500u;

    Vocabulary voc;
    if (!loadVocabulary(vocPath, voc)) {
        std::cerr << "ERROR: vocabulario " << vocPath << std::endl;
        return 1;
    }
    std::cout << "[INFO] Vocabulario: " << voc.nwords << std::endl;
    if (!logCsv.empty()) std::cout << "[INFO] evolution log: " << logCsv << " every=" << logEvery << "\n";

    BuildResult br;
    auto tBuild0 = std::chrono::steady_clock::now();
    if (!buildForest(docsPath, maxTerms, br, logCsv, logEvery)) return 1;
    auto tBuild1 = std::chrono::steady_clock::now();

    std::cout << "[BUILD] pool_nodes=" << br.poolNodes << " parse=" << br.parseS << "s build="
              << br.buildS << "s\n";

    trimSnapshotRefs(br);
    std::cout << "[BUILD] pool_nodes tras trim snapshots=" << br.poolNodes << std::endl;

    if (saveBuildResult(br, packPath) != 0) {
        releaseForest(br);
        return 1;
    }
    const double totalBuildS = std::chrono::duration<double>(tBuild1 - tBuild0).count();

    std::cout << "\n=== build OK ===\n";
    std::cout << "pool_nodes=" << br.poolNodes << " total_build=" << totalBuildS << "s\n";
    if (!logCsv.empty()) std::cout << "evolution log: " << logCsv << "\n";

    releaseForest(br);
    return 0;
}

static int cmdLoad(int argc, char** argv) {
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }
    const std::string packPath = argv[2];
    const std::string vocPath = (argc >= 4) ? argv[3] : "";
    const std::string spotWord = (argc >= 5) ? argv[4] : "";

    Vocabulary voc;
    if (!vocPath.empty()) loadVocabulary(vocPath, voc);

    DdManager* dd = nullptr;
    std::vector<DdNode*> termZdd;
    int numZddVars = 0, docOffset = 0;

    auto tLoad0 = std::chrono::steady_clock::now();
    if (!ZddPack::loadZddPack(dd, termZdd, numZddVars, docOffset, packPath)) return 1;
    auto tLoad1 = std::chrono::steady_clock::now();
    const double loadS = std::chrono::duration<double>(tLoad1 - tLoad0).count();

    tdzdd::ResourceUsage u;
    std::cout << "\n=== load OK ===\n";
    std::cout << "pool_nodes=" << Cudd_zddReadNodeCount(dd) << " load=" << loadS << "s RSS="
              << u.maxrss / 1024.0 << " MB\n";

    const int spotId = resolveSpot(voc, spotWord, static_cast<uint32_t>(termZdd.size()));
    if (!spotWord.empty()) {
        std::cout << "\n=== Spot-check ===\n";
        spotCheck(dd, termZdd, voc, spotId, docOffset);
    }

    ZddPack::freeTermZdd(dd, termZdd);
    Cudd_Quit(dd);
    return 0;
}

static int cmdVerify(int argc, char** argv) {
    if (argc < 5) {
        usage(argv[0]);
        return 1;
    }
    const std::string docsPath = argv[2];
    const std::string vocPath = argv[3];
    const std::string packPath = argv[4];
    const uint32_t maxTerms = (argc >= 6) ? static_cast<uint32_t>(std::stoul(argv[5])) : 0u;

    Vocabulary voc;
    if (!loadVocabulary(vocPath, voc)) return 1;

    BuildResult br;
    if (!buildForest(docsPath, maxTerms, br)) return 1;
    trimSnapshotRefs(br);
    const long poolBefore = br.poolNodes;

    if (!ZddPack::saveZddPack(br.dd, br.termZdd, br.numZddVars, NZDD_ZDD_DOC_LEVEL_OFFSET,
                              packPath)) {
        releaseForest(br);
        return 1;
    }

    std::vector<TermMetrics> expected = br.metrics;
    const uint32_t limit = br.limit;
    releaseForest(br);

    DdManager* dd = nullptr;
    std::vector<DdNode*> loaded;
    int numZddVars = 0, docOffset = 0;
    auto tLoad0 = std::chrono::steady_clock::now();
    if (!ZddPack::loadZddPack(dd, loaded, numZddVars, docOffset, packPath)) return 1;
    auto tLoad1 = std::chrono::steady_clock::now();
    const long poolAfter = Cudd_zddReadNodeCount(dd);
    const double loadS = std::chrono::duration<double>(tLoad1 - tLoad0).count();

    uint64_t mismatches = 0;
    for (uint32_t t = 0; t < limit; ++t) {
        TermMetrics got;
        fillMetrics(dd, loaded[t], got, docOffset);
        if (!metricsEqual(expected[t], got)) {
            ++mismatches;
            if (mismatches <= 5) {
                std::cerr << "[MISMATCH] term " << t << " exp dag=" << expected[t].dagSize
                          << " got=" << got.dagSize << " exp |F|=" << expected[t].nSubsets
                          << " got=" << got.nSubsets << " exp |M|=" << expected[t].masters.size()
                          << " got=" << got.masters.size() << std::endl;
            }
        }
    }

    const int spotId = resolveSpot(voc, "Abraham", limit);
    if (spotId >= 0) {
        std::cout << "\n=== Spot Abraham (loaded) ===\n";
        spotCheck(dd, loaded, voc, spotId, docOffset);
    }

    std::cout << "\n=== verify ===\n";
    std::cout << "pool_build_trimmed=" << poolBefore << " pool_loaded=" << poolAfter << "\n";
    std::cout << "(pool_loaded < pool_build es normal: el .zpack guarda solo el DAG alcanzable desde termZdd)\n";
    std::cout << "mismatches=" << mismatches << "/" << limit << " load=" << loadS << "s\n";
    const bool pass = (mismatches == 0);
    std::cout << "overall: " << (pass ? "PASS" : "FAIL") << std::endl;

    ZddPack::freeTermZdd(dd, loaded);
    Cudd_Quit(dd);
    return pass ? 0 : 1;
}

static int cmdPbuild(int argc, char** argv) {
    if (argc < 5) {
        usage(argv[0]);
        return 1;
    }
    const std::string docsPath = argv[2];
    const std::string vocPath = argv[3];
    const std::string packPath = argv[4];
    const uint32_t maxTerms = (argc >= 6) ? static_cast<uint32_t>(std::stoul(argv[5])) : 0u;
    const int numShards =
        (argc >= 7) ? std::stoi(argv[6]) : defaultNumShards();

    Vocabulary voc;
    if (!loadVocabulary(vocPath, voc)) {
        std::cerr << "ERROR: vocabulario " << vocPath << std::endl;
        return 1;
    }
    std::cout << "[INFO] Vocabulario: " << voc.nwords << " num_shards=" << numShards << std::endl;

    BuildResult br;
    auto tBuild0 = std::chrono::steady_clock::now();
    if (!parallelBuildForest(docsPath, maxTerms, numShards, br)) return 1;
    auto tBuild1 = std::chrono::steady_clock::now();

    tdzdd::ResourceUsage uBuild;
    std::cout << "[PBUILD] shards=" << br.numShards << " pool_nodes=" << br.poolNodes
              << " parse=" << br.parseS << "s parallel_build=" << br.buildS
              << "s merge=" << br.mergeS << "s RSS=" << uBuild.maxrss / 1024.0 << " MB\n";

    auto tSave0 = std::chrono::steady_clock::now();
    if (saveBuildResult(br, packPath, "PBUILD") != 0) {
        releaseForest(br);
        return 1;
    }
    auto tSave1 = std::chrono::steady_clock::now();
    const double saveS = std::chrono::duration<double>(tSave1 - tSave0).count();
    const double totalBuildS = std::chrono::duration<double>(tBuild1 - tBuild0).count();

    std::cout << "\n=== pbuild" << (shouldSavePack(packPath) ? "+save OK" : " OK (sin save)") << " ===\n";
    std::cout << "pool_nodes=" << br.poolNodes << " save=" << saveS << "s total_pbuild="
              << totalBuildS << "s\n";

    releaseForest(br);
    return 0;
}

static int cmdVerifyPbuild(int argc, char** argv) {
    if (argc < 4) {
        usage(argv[0]);
        return 1;
    }
    const std::string docsPath = argv[2];
    const std::string vocPath = argv[3];
    const uint32_t maxTerms = (argc >= 5) ? static_cast<uint32_t>(std::stoul(argv[4])) : 0u;
    const int numShards =
        (argc >= 6) ? std::stoi(argv[5]) : defaultNumShards();

    Vocabulary voc;
    if (!loadVocabulary(vocPath, voc)) return 1;

    std::cout << "[verify-pbuild] build serial (referencia)...\n";
    BuildResult ref;
    if (!buildForest(docsPath, maxTerms, ref)) return 1;
    trimSnapshotRefs(ref);
    const long poolRef = ref.poolNodes;

    std::cout << "[verify-pbuild] pbuild K=" << numShards << "...\n";
    BuildResult par;
    if (!parallelBuildForest(docsPath, maxTerms, numShards, par)) {
        releaseForest(ref);
        return 1;
    }
    const long poolPar = par.poolNodes;

    uint64_t mismatches = 0;
    for (uint32_t t = 0; t < ref.limit; ++t) {
        if (!metricsEqual(ref.metrics[t], par.metrics[t])) {
            ++mismatches;
            if (mismatches <= 5) {
                std::cerr << "[MISMATCH] term " << t << " ref dag=" << ref.metrics[t].dagSize
                          << " par dag=" << par.metrics[t].dagSize << " ref |F|="
                          << ref.metrics[t].nSubsets << " par |F|=" << par.metrics[t].nSubsets
                          << " ref |M|=" << ref.metrics[t].masters.size()
                          << " par |M|=" << par.metrics[t].masters.size() << std::endl;
            }
        }
    }

    const int spotId = resolveSpot(voc, "Abraham", ref.limit);
    if (spotId >= 0) {
        std::cout << "\n=== Spot Abraham (ref) ===\n";
        spotCheck(ref.dd, ref.termZdd, voc, spotId, NZDD_ZDD_DOC_LEVEL_OFFSET);
        std::cout << "\n=== Spot Abraham (pbuild) ===\n";
        spotCheck(par.dd, par.termZdd, voc, spotId, NZDD_ZDD_DOC_LEVEL_OFFSET);
    }

    std::cout << "\n=== verify-pbuild ===\n";
    std::cout << "terms=" << ref.limit << " shards=" << numShards << "\n";
    std::cout << "pool_build=" << poolRef << " pool_pbuild=" << poolPar << "\n";
    std::cout << "build_s=" << ref.buildS << " pbuild_parallel_s=" << par.buildS
              << " pbuild_merge_s=" << par.mergeS << "\n";
    std::cout << "mismatches=" << mismatches << "/" << ref.limit << "\n";
    const bool pass = (mismatches == 0);
    std::cout << "overall: " << (pass ? "PASS" : "FAIL") << std::endl;

    releaseForest(ref);
    releaseForest(par);
    return pass ? 0 : 1;
}

static int cmdCompareTdZdd(int argc, char** argv) {
    if (argc < 4) {
        usage(argv[0]);
        return 1;
    }
    const std::string packPath = argv[2];
    const std::string csvPath = argv[3];
    const std::string vocPath = (argc >= 5) ? argv[4] : "";
    const std::string spotWord = (argc >= 6) ? argv[5] : "Abraham";

    Vocabulary voc;
    if (!vocPath.empty()) loadVocabulary(vocPath, voc);

    std::vector<uint64_t> tdzddNodes;
    if (!loadTdZddNodeCounts(csvPath, tdzddNodes)) return 1;

    DdManager* dd = nullptr;
    std::vector<DdNode*> termZdd;
    int numZddVars = 0, docOffset = 0;
    if (!ZddPack::loadZddPack(dd, termZdd, numZddVars, docOffset, packPath)) return 1;

    const uint32_t limit = static_cast<uint32_t>(termZdd.size());
    uint64_t sumTdZdd = 0, sumCudd = 0, sumSubsets = 0;
    uint64_t dagDiffTerms = 0, maxDagDiff = 0;
    for (uint32_t t = 0; t < limit; ++t) {
        TermMetrics m;
        fillMetrics(dd, termZdd[t], m, docOffset);
        const uint64_t tdNodes = (t < tdzddNodes.size()) ? tdzddNodes[t] : 0;
        sumTdZdd += tdNodes;
        sumCudd += m.dagSize;
        sumSubsets += m.nSubsets;
        if (tdNodes != m.dagSize) {
            ++dagDiffTerms;
            const uint64_t diff =
                (tdNodes > m.dagSize) ? (tdNodes - m.dagSize) : (m.dagSize - tdNodes);
            if (diff > maxDagDiff) maxDagDiff = diff;
        }
    }

    const long poolNodes = Cudd_zddReadNodeCount(dd);
    tdzdd::ResourceUsage u;

    std::cout << "\n=== compare-tdzdd (wiki_1gb) ===\n";
    std::cout << "terminos=" << limit << "\n";
    std::cout << "TdZdd sum nodos (sin sharing inter-termino): " << sumTdZdd << "\n";
    std::cout << "CUDD sum DagSize (sin sharing inter-termino): " << sumCudd << "\n";
    std::cout << "CUDD pool compartido (.zpack load): " << poolNodes << "\n";
    std::cout << "factor compresion pool vs suma TdZdd: "
              << (sumTdZdd > 0 ? static_cast<double>(sumTdZdd) / static_cast<double>(poolNodes) : 0)
              << "x\n";
    std::cout << "terminos con DagSize CUDD != nodos TdZdd: " << dagDiffTerms << "/" << limit
              << " (max diff=" << maxDagDiff << "; normal entre motores)\n";
    std::cout << "CUDD sum |F_t| (subconjuntos): " << sumSubsets << "\n";
    std::cout << "RSS load: " << u.maxrss / 1024.0 << " MB\n";

    const int spotId = resolveSpot(voc, spotWord, limit);
    if (spotId >= 0) {
        std::cout << "\n=== Spot " << spotWord << " ===\n";
        std::cout << "TdZdd nodos=" << ((static_cast<uint32_t>(spotId) < tdzddNodes.size())
                                         ? tdzddNodes[static_cast<uint32_t>(spotId)]
                                         : 0)
                  << std::endl;
        spotCheck(dd, termZdd, voc, spotId, docOffset);
    }

    ZddPack::freeTermZdd(dd, termZdd);
    Cudd_Quit(dd);
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    const std::string mode = argv[1];
    if (mode == "build") return cmdBuild(argc, argv);
    if (mode == "pbuild") return cmdPbuild(argc, argv);
    if (mode == "load") return cmdLoad(argc, argv);
    if (mode == "verify") return cmdVerify(argc, argv);
    if (mode == "verify-pbuild") return cmdVerifyPbuild(argc, argv);
    if (mode == "compare-tdzdd") return cmdCompareTdZdd(argc, argv);
    std::cerr << "Modo desconocido: " << mode << std::endl;
    usage(argv[0]);
    return 1;
}
