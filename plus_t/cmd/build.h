// =============================================================================
// cmd/build.h — modo CLI `build` de zdd_cudd_plus_t
// =============================================================================

#ifndef ZDD_PLUS_T_CMD_BUILD_H
#define ZDD_PLUS_T_CMD_BUILD_H

#include "export/export.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <sys/stat.h>

static std::string buildLogBasename(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    return name;
}

static std::string defaultBuildLogCsv(const std::string& docsPath, TagEncoding enc) {
    mkdir("resultados_test", 0755);
    return std::string("resultados_test/cudd_evolucion_") + buildLogBasename(docsPath) + '_' +
           tagEncodingName(enc) + ".csv";
}

static int cmdBuild(int argc, char** argv) {
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
    std::string logCsv = (argc >= 8) ? argv[7] : "";
    const uint32_t logEvery = (argc >= 9) ? static_cast<uint32_t>(std::stoul(argv[8])) : 500u;
    if (logCsv.empty()) logCsv = defaultBuildLogCsv(docsPath, enc);

    Vocabulary voc;
    if (!loadVocabulary(vocPath, voc)) {
        std::cerr << "ERROR: vocabulario " << vocPath << std::endl;
        return 1;
    }
    std::cout << "[INFO] Vocabulario: " << voc.nwords << std::endl;
    std::cout << "[INFO] evolution log: " << logCsv << " every=" << logEvery << "\n";

    BuildResult br;
    auto tBuild0 = std::chrono::steady_clock::now();
    if (!buildForest(docsPath, maxTerms, enc, br, logCsv, logEvery)) return 1;
    auto tBuild1 = std::chrono::steady_clock::now();

    std::cout << "[BUILD] enc=" << tagEncodingName(enc) << " tagWidth=" << br.tagWidth
              << " docOffset=" << br.docOffset << " numZddVars=" << br.numZddVars
              << " pool_nodes=" << br.poolNodes << " parse=" << br.parseS << "s build=" << br.buildS
              << "s\n";

    trimSnapshotRefs(br);
    std::cout << "[BUILD] pool_nodes tras trim snapshots=" << br.poolNodes << std::endl;

    if (saveBuildResult(br, packPath) != 0) {
        releaseForest(br);
        return 1;
    }
    const double totalBuildS = std::chrono::duration<double>(tBuild1 - tBuild0).count();

    std::cout << "\n=== build OK ===\n";
    std::cout << "pool_nodes=" << br.poolNodes << " total_build=" << totalBuildS << "s\n";
    std::cout << "evolution log: " << logCsv << "\n";

    releaseForest(br);
    return 0;
}

#endif  // ZDD_PLUS_T_CMD_BUILD_H
