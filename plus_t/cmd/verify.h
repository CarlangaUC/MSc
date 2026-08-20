// =============================================================================
// cmd/verify.h — modo CLI `verify` de zdd_cudd_plus_t
// =============================================================================

#ifndef ZDD_PLUS_T_CMD_VERIFY_H
#define ZDD_PLUS_T_CMD_VERIFY_H

#include "export/export.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <tdzdd/util/ResourceUsage.hpp>

static int cmdVerify(int argc, char** argv) {
    if (argc < 6) {
        usage(argv[0]);
        return 1;
    }
    TagEncoding enc = TagEncoding::UPlusT;
    if (!parseTagEncoding(argv[2], enc)) return 1;
    const std::string docsPath = argv[3];
    const std::string vocPath = argv[4];
    const std::string packPath = argv[5];
    const uint32_t maxTerms = (argc >= 7) ? static_cast<uint32_t>(std::stoul(argv[6])) : 0u;

    Vocabulary voc;
    if (!loadVocabulary(vocPath, voc)) return 1;

    BuildResult br;
    if (!buildForest(docsPath, maxTerms, enc, br)) return 1;
    trimSnapshotRefs(br);
    const long poolBefore = br.poolNodes;
    const unsigned long bytesBuild = Cudd_ReadMemoryInUse(br.dd);
    const uint32_t nTerms = br.nTerms;
    const int docOffset = br.docOffset;
    const uint32_t tagWidth = br.tagWidth;
    const int numZddVars = br.numZddVars;

    std::vector<TermFtInputs> termData;
    uint32_t limitCheck = 0;
    int numZddVarsCheck = 0;
    int docOffsetCheck = 0;
    uint32_t tagWidthCheck = 0;
    double parseDummy = 0;
    if (!parseDocsTerms(docsPath, maxTerms, enc, termData, limitCheck, docOffsetCheck,
                        numZddVarsCheck, tagWidthCheck, parseDummy)) {
        releaseForest(br);
        return 1;
    }

    uint64_t tagMismatches = 0;
    for (uint32_t t = 0; t < br.limit; ++t) {
        DdNode* z = br.pointerList[t];
        const bool empty = (z == nullptr || z == Cudd_ReadZero(br.dd));
        if (termData[t].versionSnapshots.empty()) {
            if (!empty) ++tagMismatches;
            continue;
        }
        if (empty) {
            ++tagMismatches;
            continue;
        }
        if (readTermIdFromTagSearch(br.dd, z, docOffset, enc) != static_cast<int>(t)) {
            ++tagMismatches;
            if (tagMismatches <= 5) {
                const int got = readTermIdFromTagSearch(br.dd, z, docOffset, enc);
                std::cerr << "[TAG_MISMATCH] term " << t << " exp tag={u+" << (t + 1u)
                          << "} logical=" << logicalTagElement(static_cast<uint64_t>(t) + 1u)
                          << " got term_id=" << got << std::endl;
            }
            continue;
        }
        const uint64_t subsets = ZddPack::countSubsets(br.dd, z);
        const uint64_t expectedSubsets = static_cast<uint64_t>(termData[t].versionSnapshotCount) + 1u;
        if (subsets != expectedSubsets) {
            ++tagMismatches;
            if (tagMismatches <= 5) {
                std::cerr << "[TAG_MISMATCH] term " << t << " exp |F|=" << expectedSubsets
                          << " got=" << subsets << std::endl;
            }
        }
    }

    auto tQueryBuild0 = std::chrono::steady_clock::now();
    const ForestQueryResult buildQ =
        runForestQueryChecks(br.dd, br.pointerList, docOffset, termData, voc, br.limit, "build");
    const double queryBuildS =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - tQueryBuild0).count();
    uint64_t queryMismatches = countQueryGroundTruthMismatches(buildQ, "build");

    if (!ZddPack::saveZddPack(br.dd, br.pointerList, br.numZddVars, br.docOffset, packPath)) {
        releaseForest(br);
        return 1;
    }

    std::vector<TermMetrics> expected = br.metrics;
    const uint32_t limit = br.limit;
    releaseForest(br);

    DdManager* dd = nullptr;
    std::vector<DdNode*> loaded;
    int loadedNumZddVars = 0, loadedDocOffset = 0;
    auto tLoad0 = std::chrono::steady_clock::now();
    if (!ZddPack::loadZddPack(dd, loaded, loadedNumZddVars, loadedDocOffset, packPath)) return 1;
    auto tLoad1 = std::chrono::steady_clock::now();
    const long poolAfter = Cudd_zddReadNodeCount(dd);
    const unsigned long bytesLoaded = Cudd_ReadMemoryInUse(dd);
    const double loadS = std::chrono::duration<double>(tLoad1 - tLoad0).count();
    tdzdd::ResourceUsage uLoad;

    uint64_t mismatches = 0;
    for (uint32_t t = 0; t < limit; ++t) {
        TermMetrics got;
        fillMetrics(dd, loaded[t], got, loadedDocOffset, enc);
        if (!metricsEqual(expected[t], got)) {
            ++mismatches;
            if (mismatches <= 5) {
                std::cerr << "[MISMATCH] term " << t << " exp dag=" << expected[t].dagSize
                          << " got=" << got.dagSize << " exp |F|=" << expected[t].nSubsets
                          << " got=" << got.nSubsets << " exp tag_logical=" << expected[t].termTagLogical
                          << " got=" << got.termTagLogical << " exp |M|=" << expected[t].masters.size()
                          << " got=" << got.masters.size() << std::endl;
            }
        }
    }

    auto tQueryLoaded0 = std::chrono::steady_clock::now();
    const ForestQueryResult loadedQ =
        runForestQueryChecks(dd, loaded, loadedDocOffset, termData, voc, limit, "loaded");
    const double queryLoadedS =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - tQueryLoaded0).count();
    queryMismatches += countQueryGroundTruthMismatches(loadedQ, "loaded");
    queryMismatches += countQueryParityMismatches(buildQ, loadedQ);

    const int spotId = resolveSpot(voc, "Abraham", limit);
    if (spotId >= 0) {
        std::cout << "\n=== Spot Abraham (loaded) ===\n";
        spotCheck(dd, loaded, voc, spotId, loadedDocOffset, enc);
    }

    std::cout << "\n=== verify ===\n";
    std::cout << "enc=" << tagEncodingName(enc) << " nTerms(V)=" << nTerms << " U=2^"
              << ZDD_MASTER_BITS << " tagWidth=" << tagWidth << " docOffset=" << docOffset
              << " numZddVars=" << numZddVars << "\n";
    std::cout << "pool_build_trimmed=" << poolBefore << " pool_loaded=" << poolAfter
              << " edd_nodes_loaded=" << (poolAfter - numZddVars)
              << " load_RSS=" << (uLoad.maxrss / 1024.0) << " MB\n";
    std::cout << "bytes_cudd_build=" << bytesBuild << " bytes_cudd_loaded=" << bytesLoaded;
    if (bytesBuild > 0) {
        std::cout << " ratio_loaded/build=" << (static_cast<double>(bytesLoaded) / bytesBuild);
    }
    std::cout << "\n";
    std::cout << "(pool incluye numZddVars nodos univ del manager; edd_nodes es el DAG. "
                 "bytes_cudd NO es el tamano de la EDD: usa scripts/measure_zpack_bpi)\n";
    // Tras el trim, el manager del build debe tener EXACTAMENTE los mismos nodos
    // vivos que un manager limpio con el .zpack cargado. Cualquier exceso son nodos
    // referenciados pero inalcanzables desde algun ZDD^t, es decir una fuga: infla
    // bpi_build y el RSS del build sin aportar nada a la EDD.
    const long leakedBuildNodes = poolBefore - poolAfter;
    std::cout << "leaked_build_nodes=" << leakedBuildNodes << " (esperado 0)\n";
    std::cout << "tag_mismatches=" << tagMismatches << "/" << limit << "\n";
    std::cout << "roundtrip_mismatches=" << mismatches << "/" << limit << " load=" << loadS
              << "s\n";
    std::cout << "query_mismatches=" << queryMismatches << " query_build=" << queryBuildS
              << "s query_loaded=" << queryLoadedS << "s\n";
    const bool pass = (mismatches == 0 && tagMismatches == 0 && queryMismatches == 0 &&
                       leakedBuildNodes == 0);
    std::cout << "overall: " << (pass ? "PASS" : "FAIL") << std::endl;

    ZddPack::freeTermZdd(dd, loaded);
    Cudd_Quit(dd);
    return pass ? 0 : 1;
}

#endif  // ZDD_PLUS_T_CMD_VERIFY_H
