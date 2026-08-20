// =============================================================================
// plus_t/utils/bpi.h — métricas BPI (bits por entero indexado)
// =============================================================================
// BPI = bits por entero del índice. NO confundir:
//   • 32  = BYTES por nodo CUDD en RAM (sizeof(DdNode)), no 32 bits
//   • 64  = bits del posting crudo packed64 (uint64_t con master|rel)
//   • × 8 = conversión bytes → bits en la fórmula
//
// Fórmula única (compute() es el ÚNICO lugar que divide por N):
//   bpi = (numerador_bytes × 8) / N
//
// Ejemplo bpi_edd:
//   bpi_edd = (edd_nodes × 32 B/nodo × 8 bit/B) / n_raw
//           = (edd_nodes × 256 bits) / n_raw
//
// Numeradores medidos:
//   bpi_edd     edd_nodes × kDdNodeBytes (32 B)     RAM del DAG en CUDD
//   bpi_file    |.zpack|                            disco (20 B/nodo en .zpack)
//   bpi_mem     Cudd_ReadMemoryInUse                diagnóstico (proceso completo)
//
// Cotas de encoding del mismo DAG, de más laxa a más estricta. Ninguna se mide:
// se derivan de (edd_nodes, numZddVars). Ver bitsZddStdPerNode y siguientes.
//   bpi_zdd_std        2N⌈log₂(N+1)⌉ + N⌈log₂(V+1)⌉
//   bpi_level_grouped  2N⌈log₂(N+1)⌉ + V⌈log₂(N+1)⌉
//   bpi_dag_counting   2·log₂((N+1)!)  + V⌈log₂(N+1)⌉
//
// ATENCIÓN — bpi_zdd_std NO es una cota inferior. Es la línea base "standard
// ZDD" de la literatura de ZDDs compactos (Matsuda–Denzumi–Sadakane, "Storing
// Set Families More Compactly with Top ZDDs", Algorithms 14(6):172, 2021, que
// la escribe como 2n⌊log n⌋ + n⌊log c⌋). DenseZDD y Top ZDD quedan POR DEBAJO.
// Se mantiene el alias de salida bpi_edd_min por compatibilidad de parsers.
//
// Denominador principal: n_raw = Total_Ints (suma de postings en .docs).
// Otros denominadores (n_snap_elems, etc.) viven en bpi_scan.h.
// =============================================================================

#ifndef PLUS_T_UTILS_BPI_H
#define PLUS_T_UTILS_BPI_H

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace NzddBpi {

inline constexpr double kRawBitsPerPosting = 64.0;  // packed64: 1 posting = 64 bits
inline constexpr uint64_t kDdNodeBytes = 32u;         // sizeof(DdNode) en RAM — BYTES, no bits
inline constexpr uint64_t kPackNodeBytes = 20u;       // varIndex u32 + else u64 + then u64

struct Denominators {
    uint64_t nRaw = 0;
    uint64_t nPairsUniq = 0;
    uint64_t nSnapElems = 0;
    uint64_t nMastersUniq = 0;
    uint64_t versionsTotal = 0;
    uint64_t snapshotsDistinct = 0;
    uint64_t dupPostings = 0;
    uint64_t emptyTerms = 0;
    uint64_t maxMaster = 0;
    uint64_t maxRel = 0;
    uint32_t termsScanned = 0;
    uint32_t nlists = 0;
    bool deepScanned = false;
};

struct Numerators {
    uint64_t eddNodes = 0;
    uint64_t poolNodes = 0;
    uint64_t fileBytes = 0;
    uint64_t memBytes = 0;
    uint32_t numZddVars = 0;
};

struct Report {
    Denominators denom;
    Numerators num;

    uint64_t bytesEdd = 0;
    uint64_t bytesFile = 0;
    uint64_t bytesMem = 0;

    // Cotas de encoding: bits totales para representar el DAG.
    uint64_t bitsZddStdTotal = 0;
    uint64_t bitsLevelGroupedTotal = 0;
    double bitsDagCountingTotal = 0.0;

    // Las mismas cotas expresadas por nodo (para inspección directa).
    double bitsPerNodeZddStd = 0.0;
    double bitsPerNodeLevelGrouped = 0.0;
    double bitsPerNodeCounting = 0.0;

    double bpiEdd = 0.0;
    double bpiFile = 0.0;
    double bpiMem = 0.0;
    double bpiZddStd = 0.0;
    double bpiLevelGrouped = 0.0;
    double bpiDagCounting = 0.0;
    double compressionVsRaw = 0.0;

    double bpiEddOverPairs = 0.0;
    double bpiEddOverStored = 0.0;
    double bpiEddOverMasters = 0.0;
    double bpiFileOverStored = 0.0;
    double bpiZddStdOverStored = 0.0;

    double ratioRawOverStored = 0.0;
    double dedupVersionsOverSnapshots = 0.0;
    double bitsPerStoredElem = 0.0;
};

inline uint64_t ceilLog2(uint64_t n) {
    if (n <= 1u) return 1u;
#if defined(__GNUC__) || defined(__clang__)
    return 64u - static_cast<uint64_t>(__builtin_clzll(n - 1u));
#else
    uint64_t r = 0;
    uint64_t v = n - 1u;
    while (v > 0u) {
        v >>= 1u;
        ++r;
    }
    return r > 0u ? r : 1u;
#endif
}

// Línea base "standard ZDD": índice de variable + dos punteros por nodo, cada
// puntero estrechado a ⌈log₂(N+1)⌉ bits (el +1 cubre el terminal). Equivale a
// 2n⌊log n⌋ + n⌊log c⌋ de la literatura. NO es una cota inferior.
inline uint64_t bitsZddStdPerNode(uint64_t eddNodes, uint32_t numZddVars) {
    if (eddNodes == 0u) return 0u;
    const uint64_t childBits = 2u * ceilLog2(eddNodes + 1u);
    const uint64_t varBits = ceilLog2(static_cast<uint64_t>(numZddVars) + 1u);
    return childBits + varBits;
}

inline uint64_t bitsZddStdTotal(uint64_t eddNodes, uint32_t numZddVars) {
    return eddNodes * bitsZddStdPerNode(eddNodes, numZddVars);
}

// En un DD ordenado el índice de variable no hace falta por nodo: basta disponer
// los nodos agrupados por nivel y guardar V fronteras de nivel. El término de
// etiquetas pasa de N⌈log₂(V+1)⌉ a V⌈log₂(N+1)⌉, amortizado a ~0 cuando N ≫ V.
inline uint64_t bitsLevelGroupedTotal(uint64_t eddNodes, uint32_t numZddVars) {
    if (eddNodes == 0u) return 0u;
    const uint64_t idBits = ceilLog2(eddNodes + 1u);
    return eddNodes * 2u * idBits + static_cast<uint64_t>(numZddVars) * idBits;
}

// Piso information-theoretic. Con los nodos en orden topológico el nodo i sólo
// puede apuntar a los i-1 anteriores más los terminales, así que sus punteros
// cuestan log₂(i+1) bits y no log₂(N+1): el total de los dos punteros es
// 2·log₂((N+1)!) ≈ N(2log₂N − 2log₂e), es decir ~2.89 bits/nodo por debajo de
// bitsZddStdPerNode. Se conserva el índice de nivel porque sigue siendo
// necesario para reconstruir el orden.
inline double bitsDagCountingTotal(uint64_t eddNodes, uint32_t numZddVars) {
    if (eddNodes == 0u) return 0.0;
    const double logFactorial =
        std::lgamma(static_cast<double>(eddNodes) + 2.0) / std::log(2.0);
    const double levelIndex = static_cast<double>(numZddVars) *
                              static_cast<double>(ceilLog2(eddNodes + 1u));
    return 2.0 * logFactorial + levelIndex;
}

inline double bpiFromBytes(uint64_t bytes, uint64_t n) {
    if (n == 0u) return 0.0;
    return (static_cast<double>(bytes) * 8.0) / static_cast<double>(n);
}

inline double bpiFromBits(uint64_t bits, uint64_t n) {
    if (n == 0u) return 0.0;
    return static_cast<double>(bits) / static_cast<double>(n);
}

inline double bpiFromBits(double bits, uint64_t n) {
    if (n == 0u) return 0.0;
    return bits / static_cast<double>(n);
}

inline double perNode(double bitsTotal, uint64_t eddNodes) {
    if (eddNodes == 0u) return 0.0;
    return bitsTotal / static_cast<double>(eddNodes);
}

inline Report compute(const Numerators& num, const Denominators& denom) {
    Report r;
    r.denom = denom;
    r.num = num;

    r.bytesEdd = num.eddNodes * kDdNodeBytes;
    r.bytesFile = num.fileBytes;
    r.bytesMem = num.memBytes;

    r.bitsZddStdTotal = bitsZddStdTotal(num.eddNodes, num.numZddVars);
    r.bitsLevelGroupedTotal = bitsLevelGroupedTotal(num.eddNodes, num.numZddVars);
    r.bitsDagCountingTotal = bitsDagCountingTotal(num.eddNodes, num.numZddVars);

    r.bitsPerNodeZddStd =
        perNode(static_cast<double>(r.bitsZddStdTotal), num.eddNodes);
    r.bitsPerNodeLevelGrouped =
        perNode(static_cast<double>(r.bitsLevelGroupedTotal), num.eddNodes);
    r.bitsPerNodeCounting = perNode(r.bitsDagCountingTotal, num.eddNodes);

    const uint64_t nRaw = denom.nRaw;
    r.bpiEdd = bpiFromBytes(r.bytesEdd, nRaw);
    r.bpiFile = bpiFromBytes(r.bytesFile, nRaw);
    r.bpiMem = bpiFromBytes(r.bytesMem, nRaw);
    r.bpiZddStd = bpiFromBits(r.bitsZddStdTotal, nRaw);
    r.bpiLevelGrouped = bpiFromBits(r.bitsLevelGroupedTotal, nRaw);
    r.bpiDagCounting = bpiFromBits(r.bitsDagCountingTotal, nRaw);
    if (r.bpiEdd > 0.0) r.compressionVsRaw = kRawBitsPerPosting / r.bpiEdd;

    if (denom.deepScanned) {
        r.bpiEddOverPairs = bpiFromBytes(r.bytesEdd, denom.nPairsUniq);
        r.bpiEddOverStored = bpiFromBytes(r.bytesEdd, denom.nSnapElems);
        r.bpiEddOverMasters = bpiFromBytes(r.bytesEdd, denom.nMastersUniq);
        r.bpiFileOverStored = bpiFromBytes(r.bytesFile, denom.nSnapElems);
        r.bpiZddStdOverStored = bpiFromBits(r.bitsZddStdTotal, denom.nSnapElems);
        if (denom.nSnapElems > 0u) {
            r.ratioRawOverStored =
                static_cast<double>(nRaw) / static_cast<double>(denom.nSnapElems);
            r.bitsPerStoredElem = bpiFromBytes(r.bytesEdd, denom.nSnapElems);
        }
        if (denom.snapshotsDistinct > 0u) {
            r.dedupVersionsOverSnapshots =
                static_cast<double>(denom.versionsTotal) /
                static_cast<double>(denom.snapshotsDistinct);
        }
    }
    return r;
}

inline void printReport(std::ostream& out, const Report& r) {
    out << "terms_scanned=" << r.denom.termsScanned << "\n";
    out << "total_ints=" << r.denom.nRaw << "\n";
    out << "n_raw=" << r.denom.nRaw << "\n";
    if (r.denom.deepScanned) {
        out << "n_pairs_uniq=" << r.denom.nPairsUniq << "\n";
        out << "dup_postings=" << r.denom.dupPostings << "\n";
        out << "n_snap_elems=" << r.denom.nSnapElems << "\n";
        out << "n_masters_uniq=" << r.denom.nMastersUniq << "\n";
        out << "versions_total=" << r.denom.versionsTotal << "\n";
        out << "snapshots_distinct=" << r.denom.snapshotsDistinct << "\n";
        out << "ratio_raw_over_stored=" << std::setprecision(8) << r.ratioRawOverStored << "\n";
        out << "dedup_versions_over_snapshots=" << r.dedupVersionsOverSnapshots << "\n";
        out << "bpi_edd_over_stored=" << r.bpiEddOverStored << "\n";
        out << "bpi_edd_over_masters=" << r.bpiEddOverMasters << "\n";
        out << "bpi_zdd_std_over_stored=" << r.bpiZddStdOverStored << "\n";
        out << "bpi_edd_min_over_stored=" << r.bpiZddStdOverStored << "\n";
        out << "bits_per_stored_elem=" << r.bitsPerStoredElem << "\n";
    }
    out << "numZddVars=" << r.num.numZddVars << "\n";
    out << "edd_nodes=" << r.num.eddNodes << "\n";
    out << "node_bytes=" << kDdNodeBytes << "\n";
    out << "bytes_edd=" << r.bytesEdd << "\n";
    out << "bpi_edd=" << std::setprecision(8) << r.bpiEdd << "\n";
    out << "file_bytes=" << r.bytesFile << "\n";
    out << "bpi_file=" << r.bpiFile << "\n";
    out << "pool_nodes=" << r.num.poolNodes << "\n";
    out << "bytes_cudd=" << r.bytesMem << "\n";
    out << "bpi_mem=" << r.bpiMem << "\n";

    // Cotas de encoding: no requieren FullAudit, sólo edd_nodes y numZddVars.
    out << "bits_per_node_zdd_std=" << std::setprecision(6) << r.bitsPerNodeZddStd << "\n";
    out << "bits_per_node_level_grouped=" << r.bitsPerNodeLevelGrouped << "\n";
    out << "bits_per_node_counting=" << r.bitsPerNodeCounting << "\n";
    out << "bpi_zdd_std=" << std::setprecision(8) << r.bpiZddStd << "\n";
    out << "bpi_level_grouped=" << r.bpiLevelGrouped << "\n";
    out << "bpi_dag_counting=" << r.bpiDagCounting << "\n";
    out << "bpi_edd_min=" << r.bpiZddStd << "\n";  // alias legacy de bpi_zdd_std

    if (r.bpiEdd > 0.0)
        out << "compression_vs_raw=" << std::setprecision(6) << r.compressionVsRaw << "\n";
}

inline void printAuditReport(std::ostream& out, const std::string& docsPath, const Report& r) {
    out << "docs=" << docsPath << "\n";
    out << "nlists=" << r.denom.nlists << "\n";
    out << "empty_terms=" << r.denom.emptyTerms << "\n";
    out << "max_master=" << r.denom.maxMaster << "\n";
    out << "max_rel=" << r.denom.maxRel << "\n";
    printReport(out, r);
}

inline std::string csvHeader() {
    return "total_ints,bpi_edd,bpi_file,bpi_mem,bpi_edd_min,edd_nodes,"
           "bpi_zdd_std,bpi_level_grouped,bpi_dag_counting,"
           "bits_per_node_zdd_std,bits_per_node_level_grouped,bits_per_node_counting,"
           "ratio_raw_over_stored,bpi_edd_over_stored,bpi_edd_min_over_stored";
}

inline std::string csvRow(const Report& r) {
    std::ostringstream oss;
    oss << r.denom.nRaw << ','
        << std::setprecision(8) << r.bpiEdd << ','
        << r.bpiFile << ','
        << r.bpiMem << ','
        << r.bpiZddStd << ','  // columna legacy bpi_edd_min
        << r.num.eddNodes << ','
        << r.bpiZddStd << ','
        << r.bpiLevelGrouped << ','
        << r.bpiDagCounting << ','
        << std::setprecision(6) << r.bitsPerNodeZddStd << ','
        << r.bitsPerNodeLevelGrouped << ','
        << r.bitsPerNodeCounting << ',';
    if (r.denom.deepScanned) {
        oss << std::setprecision(8) << r.ratioRawOverStored << ','
            << r.bpiEddOverStored << ',' << r.bpiZddStdOverStored;
    } else {
        oss << ",,";
    }
    return oss.str();
}

}  // namespace NzddBpi

#endif  // PLUS_T_UTILS_BPI_H
