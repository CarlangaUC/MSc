// =============================================================================
// cmd/load.h — modo CLI `load` de zdd_cudd_plus_t
// =============================================================================

#ifndef ZDD_PLUS_T_CMD_LOAD_H
#define ZDD_PLUS_T_CMD_LOAD_H

#include "export/export.h"

#include <tdzdd/util/ResourceUsage.hpp>

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

static int cmdLoad(int argc, char** argv) {
    if (argc < 4) {
        usage(argv[0]);
        return 1;
    }
    TagEncoding enc = TagEncoding::UPlusT;
    if (!parseTagEncoding(argv[2], enc)) return 1;
    const std::string packPath = argv[3];
    const std::string vocPath = (argc >= 5) ? argv[4] : "";
    const std::string spotWord = (argc >= 6) ? argv[5] : "";

    Vocabulary voc;
    if (!vocPath.empty()) loadVocabulary(vocPath, voc);

    DdManager* dd = nullptr;
    std::vector<DdNode*> pointerList;
    int numZddVars = 0, docOffset = 0;

    auto tLoad0 = std::chrono::steady_clock::now();
    if (!ZddPack::loadZddPack(dd, pointerList, numZddVars, docOffset, packPath)) return 1;
    auto tLoad1 = std::chrono::steady_clock::now();
    const double loadS = std::chrono::duration<double>(tLoad1 - tLoad0).count();

    tdzdd::ResourceUsage u;
    std::cout << "\n=== load OK ===\n";
    std::cout << "enc=" << tagEncodingName(enc) << " tagWidth=" << tagWidthFromDocOffset(docOffset)
              << " docOffset=" << docOffset << " numZddVars=" << numZddVars
              << " pool_nodes=" << Cudd_zddReadNodeCount(dd) << " load=" << loadS << "s RSS="
              << u.maxrss / 1024.0 << " MB\n";

    const int spotId = resolveSpot(voc, spotWord, static_cast<uint32_t>(pointerList.size()));
    if (!spotWord.empty()) {
        std::cout << "\n=== Spot-check ===\n";
        spotCheck(dd, pointerList, voc, spotId, docOffset, enc);
    }

    ZddPack::freeTermZdd(dd, pointerList);
    Cudd_Quit(dd);
    return 0;
}

#endif  // ZDD_PLUS_T_CMD_LOAD_H
