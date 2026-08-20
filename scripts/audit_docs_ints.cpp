// =============================================================================
// audit_docs_ints — qué mide realmente Total_Ints en un .docs packed64
// =============================================================================
// Compilar:
//   g++ -O2 -std=c++17 -fopenmp -o scripts/audit_docs_ints scripts/audit_docs_ints.cpp \
//       -I plus_t -I . -I ./TdZdd/include -I ./cudd -I ./cudd/cudd \
//       -I ./uiHRDC/uiHRDC/indexes/NOPOS/II_docs/src/utils \
//       -L ./cudd/cudd/.libs -Wl,-rpath,'$ORIGIN/../cudd/cudd/.libs' -lcudd
// =============================================================================
#include <cstddef>
#include <cstdio>

#include <cstdint>
#include <iostream>
#include <string>

#include "nzdd_cudd_common.h"
#include "utils/bpi_scan.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <docs> [max_terms]\n";
        return 1;
    }
    const std::string docsPath = argv[1];
    const uint32_t maxTerms = (argc >= 3) ? static_cast<uint32_t>(std::stoul(argv[2])) : 0u;

    const NzddBpi::Denominators denom =
        NzddBpi::scanDocs(docsPath, maxTerms, NzddBpi::DenomMode::FullAudit);
    if (denom.nlists == 0u) {
        std::cerr << "ERROR: indice invalido " << docsPath << std::endl;
        return 1;
    }

    const NzddBpi::Report rep = NzddBpi::compute(NzddBpi::Numerators{}, denom);
    NzddBpi::printAuditReport(std::cout, docsPath, rep);
    return 0;
}
