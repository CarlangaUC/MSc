// =============================================================================
// measure_zpack_bpi — BPI de la EDD plus_t tras cargar un .zpack
// =============================================================================
// Compilar:
//   g++ -O2 -std=c++17 -o scripts/measure_zpack_bpi scripts/measure_zpack_bpi.cpp \
//       -I plus_t -I . -I ./TdZdd/include -I ./cudd -I ./cudd/cudd -I ./cudd/st \
//       -I ./cudd/util -I ./cudd/mtr -I ./cudd/epd \
//       -I ./uiHRDC/uiHRDC/indexes/NOPOS/II_docs/src/utils \
//       -L ./cudd/cudd/.libs -Wl,-rpath,'$ORIGIN/../cudd/cudd/.libs' -lcudd
// =============================================================================
#include "nzdd_cudd_pack.h"
#include "utils/bpi_scan.h"

#include "cuddInt.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

static_assert(sizeof(DdNode) == NzddBpi::kDdNodeBytes,
              "sizeof(DdNode) must match NzddBpi::kDdNodeBytes (32 bytes, not bits)");

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " <pack.zpack> <docs> [audit]\n";
        return 1;
    }
    const std::string packPath = argv[1];
    const std::string docsPath = argv[2];
    const bool doAudit = (argc >= 4 && std::string(argv[3]) == "audit") ||
                         (std::getenv("NZDD_BPI_AUDIT") != nullptr &&
                          std::string(std::getenv("NZDD_BPI_AUDIT")) == "1");

    DdManager* dd = nullptr;
    std::vector<DdNode*> roots;
    int numZddVars = 0, docOffset = 0;
    if (!ZddPack::loadZddPack(dd, roots, numZddVars, docOffset, packPath)) return 1;

    const uint32_t termsMeasured = static_cast<uint32_t>(roots.size());
    const NzddBpi::DenomMode mode =
        doAudit ? NzddBpi::DenomMode::FullAudit : NzddBpi::DenomMode::RawOnly;
    const NzddBpi::Denominators denom = NzddBpi::scanDocs(docsPath, termsMeasured, mode);
    if (denom.nRaw == 0u || termsMeasured == 0u) {
        std::cerr << "ERROR: total_ints=0 or terms_measured=0 for " << docsPath
                  << " (roots=" << roots.size() << ")\n";
        ZddPack::freeTermZdd(dd, roots);
        Cudd_Quit(dd);
        return 1;
    }

    const long poolNodes = Cudd_zddReadNodeCount(dd);
    const long eddNodes = NzddCommon::cuddEddNodeCount(dd);
    const long univNodes = poolNodes - eddNodes;

    std::ifstream in(packPath, std::ios::binary | std::ios::ate);
    const uint64_t fileBytes = static_cast<uint64_t>(in.tellg());
    const unsigned long bytesCudd = Cudd_ReadMemoryInUse(dd);

    NzddBpi::Numerators num{};
    num.eddNodes = static_cast<uint64_t>(eddNodes);
    num.poolNodes = static_cast<uint64_t>(poolNodes);
    num.fileBytes = fileBytes;
    num.memBytes = bytesCudd;
    num.numZddVars = static_cast<uint32_t>(numZddVars);

    const NzddBpi::Report rep = NzddBpi::compute(num, denom);

    const uint64_t cacheSlots = Cudd_ReadCacheSlots(dd);
    const uint64_t bytesCache = (cacheSlots + 1u) * sizeof(DdCache);
    const uint64_t bytesSubtables =
        static_cast<uint64_t>(dd->maxSize + dd->maxSizeZ) * (sizeof(DdSubtable) + 2 * sizeof(int));
    const uint64_t bytesHashSlots = static_cast<uint64_t>(Cudd_ReadSlots(dd)) * sizeof(DdNodePtr);
    const uint64_t deadNodes = static_cast<uint64_t>(dd->deadZ);
    const uint64_t bytesUniv = static_cast<uint64_t>(univNodes) * NzddBpi::kDdNodeBytes;
    const uint64_t bytesDead = deadNodes * NzddBpi::kDdNodeBytes;
    const uint64_t bytesOverhead = (bytesCudd > rep.bytesEdd) ? (bytesCudd - rep.bytesEdd) : 0u;

    std::cout << "pack=" << packPath << "\n";
    std::cout << "docOffset=" << docOffset << "\n";
    NzddBpi::printReport(std::cout, rep);
    std::cout << "bytes_overhead=" << bytesOverhead << "\n";
    std::cout << "overhead_pct=" << (100.0 * static_cast<double>(bytesOverhead) /
                                     static_cast<double>(bytesCudd))
              << "\n";
    std::cout << "cache_slots=" << cacheSlots << "\n";
    std::cout << "bytes_cache=" << bytesCache << "\n";
    std::cout << "bytes_subtables=" << bytesSubtables << "\n";
    std::cout << "bytes_hash_slots=" << bytesHashSlots << "\n";
    std::cout << "univ_nodes=" << univNodes << "\n";
    std::cout << "bytes_univ=" << bytesUniv << "\n";
    std::cout << "dead_nodes=" << deadNodes << "\n";
    std::cout << "bytes_dead=" << bytesDead << "\n";

    ZddPack::freeTermZdd(dd, roots);
    Cudd_Quit(dd);
    return 0;
}
